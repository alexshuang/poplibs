// Copyright (c) 2020 Graphcore Ltd. All rights reserved.
#include "FullyConnectedPlan.hpp"
#include "PerformanceEstimation.hpp"
#include "SparseMetaInfo.hpp"

#include "popsparse/FullyConnectedParams.hpp"

#include "poplibs_support/Algorithm.hpp"
#include "poplibs_support/Compiler.hpp"
#include "poplibs_support/TileHierarchy.hpp"
#include "poplibs_support/VectorUtils.hpp"
#include "poplibs_support/gcd.hpp"
#include "poplibs_support/logging.hpp"
#include "poputil/exceptions.hpp"

#include "popsolver/Model.hpp"

#include "FullyConnectedOptions.hpp"
#include "FullyConnectedUtils.hpp"
#include "popsparse/FullyConnected.hpp"

#include <map>
#include <utility>
#include <vector>

using namespace poplar;
using namespace poplibs_support;

// TODO: share this across files
using MetaInfoType = unsigned short;

namespace popsparse {

namespace dynamic {

class PlanningCacheImpl {
public:
  struct Key {
    FullyConnectedParams params;
    fullyconnected::Options options;
    Key(FullyConnectedParams params, fullyconnected::Options options)
        : params(std::move(params)), options(std::move(options)) {}
    Key() = default;
    bool operator<(const Key &other) const {
      return std::tie(params, options) < std::tie(other.params, other.options);
    }
  };

  std::map<Key, std::tuple<fullyconnected::Plan, fullyconnected::Cost>> plans;
};

PlanningCache::PlanningCache() {
  impl = std::unique_ptr<PlanningCacheImpl>(new PlanningCacheImpl());
}

PlanningCache::~PlanningCache() = default;

} // end namespace dynamic

using namespace dynamic;

namespace fullyconnected {

namespace {

const static auto metaInfoType = UNSIGNED_SHORT;

using CostBreakdown = std::vector<std::pair<std::string, Cost>>;

using CostVariables = Estimates<popsolver::Variable>;
using CostBreakdownVariables =
    std::vector<std::pair<std::string, CostVariables>>;

static Cost highestCost(std::numeric_limits<unsigned>::max(),
                        std::numeric_limits<unsigned>::max());

// TODO: This can easily be shared along with other stuff with the
// (dense) convolution library.
class PlanningObjective {
public:
  enum Type { MINIMIZE_CYCLES, MINIMIZE_TILE_TEMP_MEMORY };

private:
  Type type;
  unsigned cyclesBound = std::numeric_limits<unsigned>::max();
  unsigned tileTempMemoryBound = std::numeric_limits<unsigned>::max();
  PlanningObjective(Type type) : type(type) {}

public:
  PlanningObjective() {}
  static PlanningObjective minimizeCycles() {
    return PlanningObjective(MINIMIZE_CYCLES);
  }
  static PlanningObjective minimizeTileTempMemory() {
    return PlanningObjective(MINIMIZE_TILE_TEMP_MEMORY);
  }
  PlanningObjective &setCyclesBound(unsigned bound) {
    assert(type != MINIMIZE_CYCLES);
    assert(bound > 0);
    cyclesBound = bound;
    return *this;
  }
  PlanningObjective &setTileTempMemoryBound(unsigned bound) {
    assert(type != MINIMIZE_TILE_TEMP_MEMORY);
    assert(bound > 0);
    tileTempMemoryBound = bound;
    return *this;
  }
  unsigned getCyclesBound() const { return cyclesBound; }
  unsigned getTileTempMemoryBound() const { return tileTempMemoryBound; }
  Type getType() const { return type; }
  bool lowerCost(Cost a, Cost b) const {
    bool aCyclesOutOfBounds = a.cycles >= cyclesBound;
    bool bCyclesOutOfBounds = b.cycles >= cyclesBound;
    bool aMemoryOutOfBounds = a.tempBytes >= tileTempMemoryBound;
    bool bMemoryOutOfBounds = b.tempBytes >= tileTempMemoryBound;
    switch (type) {
    case MINIMIZE_CYCLES:
      return std::tie(aCyclesOutOfBounds, aMemoryOutOfBounds, a.cycles,
                      a.tempBytes) < std::tie(bCyclesOutOfBounds,
                                              bMemoryOutOfBounds, b.cycles,
                                              b.tempBytes);
    case MINIMIZE_TILE_TEMP_MEMORY:
      return std::tie(aMemoryOutOfBounds, aCyclesOutOfBounds, a.tempBytes,
                      a.cycles) < std::tie(bMemoryOutOfBounds,
                                           bCyclesOutOfBounds, b.tempBytes,
                                           b.cycles);
    }
    POPLIB_UNREACHABLE();
  }
};

class ExchangeEstimator {
  // Exchange bytes per cycle is given as a floating point value but the
  // constaint solver only supports unsigned integer variables. To reduce
  // quantization error in the calculation of the number of cycles we multiply
  // both the divisor (exchange bytes per cycle) and the dividend (the number of
  // bytes) by this scaling factor. Larger values of the scaling factor reduce
  // the quantization error but reduce the maximum number of bytes that can
  // be exchanged before running into the limits of the data type used to store
  // it.
  constexpr static unsigned exchangeBytesScalingFactor = 16u;

public:
  ExchangeEstimator(popsolver::Model &m, const poplar::Target &target,
                    const std::vector<unsigned> &hierarchy,
                    const std::vector<double> &perLevelExchangeBytesPerCycle)
      : m(m), target(target), levelsOfHierarchy(hierarchy.size()) {
    for (unsigned level = 0; level != hierarchy.size(); ++level) {
      const auto scaledBytesPerCycle = getScaledExchangeBytesPerCycle(
          m, perLevelExchangeBytesPerCycle[level], exchangeBytesScalingFactor);

      perLevelScaledExchangeBytesPerCycle.push_back(scaledBytesPerCycle);
      perLevelScaledExchangeBytesPerCycleVar.push_back(
          m.addConstant(scaledBytesPerCycle));
    }
  }

  // TODO: How to represent the idea of super-tile send/receive in the
  // estimator? Just provide a method to getCycles which specifies some
  // properties of the exchange pattern?
  popsolver::Variable operator()(const popsolver::Variable mNumBytes,
                                 const unsigned level,
                                 const std::string &debugName = "") const {
    return getCycles(mNumBytes, level, debugName);
  }

  popsolver::Variable
  operator()(const popsolver::Variable mNumBytes,
             const popsolver::Variable mConsecutiveTilesReceivingSameData,
             const popsolver::Variable mTotalReceivingTiles,
             const unsigned level, const std::string &debugName = "") const {
    return getCycles(mNumBytes, mConsecutiveTilesReceivingSameData,
                     mTotalReceivingTiles, level, debugName);
  }

  unsigned operator()(unsigned numBytes, unsigned level) const {
    assert(level < perLevelScaledExchangeBytesPerCycle.size());
    const unsigned scalingFactor = exchangeBytesScalingFactor;
    const auto scaledElementBytes = numBytes * scalingFactor;
    return ceildiv(scaledElementBytes,
                   perLevelScaledExchangeBytesPerCycle[level]);
  }

private:
  popsolver::Variable
  getCycles(const popsolver::Variable mNumBytes,
            const popsolver::Variable mConsecutiveTilesReceivingSameData,
            const popsolver::Variable mTotalReceivingTiles,
            const unsigned level, const std::string &debugName = "") const {
    assert(level < perLevelScaledExchangeBytesPerCycleVar.size());

    auto mScaledBytesPerCycle = perLevelScaledExchangeBytesPerCycleVar[level];
    assert(target.getTilesPerSharedExchangeBus() == 2);
    if (level == levelsOfHierarchy - 1 && target.supportsExchangeBusSharing() &&
        target.getTilesPerSharedExchangeBus() == 2) {

      // In general the factor by which we can speed up the exchange by sharing
      // the exchange bus is the greatest common divisor of the number of
      // consecutive tiles receiving the same data and the number of tiles
      // sharing an exchange bus. A separate special case where we can always
      // share the exchange bus is when the number of consecutive tiles
      // receiving the same data is equal the number of tiles receiving data
      // (even if that number shared no common factor with the number of tiles
      // sharing the exchange bus > 1).
      //
      // Because gcd is hard to do in popsolver and because we only ever have
      // a maximum of 2 tiles sharing an exchange bus for current architecture
      // we assume 2 tiles share an exchange bus at most and the logic below
      // reflects this and would not work for more.
      const auto tilesSharingBus = target.getTilesPerSharedExchangeBus();
      const auto mTilesSharingBus = m.addConstant(tilesSharingBus);
      const auto mZeroWhenFullBroadcast =
          m.sub(mTotalReceivingTiles, mConsecutiveTilesReceivingSameData);
      const auto mZeroWhenCanShareBusAnyway =
          m.mod(mConsecutiveTilesReceivingSameData, mTilesSharingBus);
      const auto mZeroWhenCanShareBus =
          m.product({mZeroWhenFullBroadcast, mZeroWhenCanShareBusAnyway});
      const auto mCanShareBus =
          m.sub(m.one(), m.min({m.one(), mZeroWhenCanShareBus}));
      const auto mShareFactor = m.sum({m.one(), mCanShareBus});
      mScaledBytesPerCycle = m.product({mScaledBytesPerCycle, mShareFactor});
    }

    const auto mScalingFactor = m.addConstant(exchangeBytesScalingFactor);
    const auto mScaledBytes = m.product({mNumBytes, mScalingFactor});
    return m.ceildiv(mScaledBytes, mScaledBytesPerCycle, debugName);
  }

  popsolver::Variable getCycles(const popsolver::Variable mNumBytes,
                                const unsigned level,
                                const std::string &debugName = "") const {
    assert(level < perLevelScaledExchangeBytesPerCycleVar.size());
    const auto mScaledBytesPerCycle =
        perLevelScaledExchangeBytesPerCycleVar[level];
    const auto mScalingFactor = m.addConstant(exchangeBytesScalingFactor);
    const auto mScaledBytes = m.product({mNumBytes, mScalingFactor});
    return m.ceildiv(mScaledBytes, mScaledBytesPerCycle, debugName);
  }

  static unsigned getScaledExchangeBytesPerCycle(popsolver::Model &m,
                                                 double exchangeBytesPerCycle,
                                                 unsigned scaleFactor) {
    auto scaledExchangeBytesPerCycle =
        std::round(exchangeBytesPerCycle * scaleFactor);
    // Ensure scaled bytes per cycle is at least one to avoid divide by zero
    // errors.
    scaledExchangeBytesPerCycle = std::max(1.0, scaledExchangeBytesPerCycle);
    // Saturate to the half the maximum unsigned integer value (we avoid the
    // maximum value to avoid range problems with the intermediate variables
    // used to implement ceildiv).
    scaledExchangeBytesPerCycle =
        std::min(scaledExchangeBytesPerCycle,
                 static_cast<double>(std::numeric_limits<unsigned>::max() / 2));
    return static_cast<unsigned>(scaledExchangeBytesPerCycle);
  }

  popsolver::Model &m;
  const poplar::Target &target;
  const unsigned levelsOfHierarchy;
  std::vector<unsigned> perLevelScaledExchangeBytesPerCycle;
  std::vector<popsolver::Variable> perLevelScaledExchangeBytesPerCycleVar;
};

// Contains variables describing partitions. Only one form canonically describes
// the partitions, but it is useful to be able to store this information in
// redundant forms to avoid recomputing different forms/combinations of
// partitions all over the place.
struct PartitionVariables {
  // Partitions in each dimension at each level.
  std::vector<Vector<popsolver::Variable>> partition;
  // Product of the partitions of each dimension in each level.
  std::vector<popsolver::Variable> product;
  // Number of tile-level partitions at and below each level.
  // i.e. productByLevel[level] * productByLevel[level + 1]
  // .. * productByLevel[maxLevels]
  std::vector<popsolver::Variable> tile;
  // Cumulative product of partitions at each level and all levels
  // higher than it.
  std::vector<Vector<popsolver::Variable>> cumulative;

  PartitionVariables(
      popsolver::Model &m,
      const std::vector<Vector<popsolver::Variable>> &mPartitions)
      : partition(mPartitions), product(mPartitions.size()),
        tile(mPartitions.size() + 1), cumulative(mPartitions.size() + 1) {

    // Calculate products of partitions
    for (unsigned level = 0; level < partition.size(); ++level) {
      product[level] = m.product(partition[level].asStdVector());
    }
    // Calculate no. of tile-level partitions at each level
    tile[mPartitions.size() + 1] = m.one();
    for (int level = partition.size() - 1; level >= 0; --level) {
      tile[level] = m.product({product[level], tile[level + 1]});
    }
    // Calculate cumulative partitions
    cumulative[0] =
        Vector<popsolver::Variable>::generate([&] { return m.one(); });
    for (unsigned level = 1; level < partition.size() + 1; ++level) {
      cumulative[level] = partition[level - 1].binaryOp(
          cumulative[level - 1],
          [&](const auto &partition, const auto &cumulativePrev) {
            return m.product({partition, cumulativePrev});
          });
    }
  }
};

} // end anonymous namespace

static std::tuple<CostVariables, popsolver::Variable, popsolver::Variable>
addDistributionExchangeCostSparseDense(
    popsolver::Model &m, const Target &target, const Type &inputType,
    const Type &metaInfoType, const Options &options,
    const std::vector<unsigned> &hierarchy,
    const ExchangeEstimator &exchangeEstimator,
    const std::vector<Vector<popsolver::Variable>> &mGroups,
    const Vector<popsolver::Variable> &mGrouping,
    const popsolver::Variable &mRElemsPerBucket,
    const popsolver::Variable &mRMetaInfoElemsPerBucket,
    const PartitionVariables &p) {

  const auto mBytesPerInput = m.addConstant(target.getTypeSize(inputType));
  const auto mBytesPerMetaInfoElem =
      m.addConstant(target.getTypeSize(metaInfoType));

  const auto mRBytesPerBucket = m.product({mRElemsPerBucket, mBytesPerInput});
  const auto mRMetaInfoBytesPerBucket =
      m.product({mRMetaInfoElemsPerBucket, mBytesPerMetaInfoElem});

  std::vector<popsolver::Variable> mRBytesPerTile(hierarchy.size() + 1),
      mSBytesPerTile(hierarchy.size() + 1);
  for (unsigned level = 0; level < hierarchy.size() + 1; ++level) {
    // Bytes per-tile for the dense input at each level are given by the
    // product of number of grains of each dimension of the input, spread
    // over the tiles that will eventually compute on those bytes.
    mSBytesPerTile[level] =
        m.product({m.ceildiv(m.product({mGroups[level].groups, mGroups[level].y,
                                        mGroups[level].z}),
                             p.tile[level]),
                   mGrouping.groups, mGrouping.y, mGrouping.z, mBytesPerInput});

    // In the initial distribution we broadcast the buckets of the sparse
    // operand across partitions processing the same X and Y partitions.
    // Buckets are constrained to be of equal size on all tiles so this
    // product will not introduce any error in the calculation moving up
    // levels of the hierarchy.
    mRBytesPerTile[level] =
        m.product({p.cumulative[level].z,
                   m.sum({mRBytesPerBucket, mRMetaInfoBytesPerBucket})});
  }

  // Estimate exchange for the initial distribution. We exchange input
  // operands s and r to the tiles that will process them during the first
  // compute step.
  //
  // Exchange cycles are calculated by finding the critical path for send
  // /receive of data. In this case the exchange will multi-cast data from
  // each tile within a particular set of partitions to all tiles in that
  // particular partition. The critical path then is the sending of each
  // chunk of data on each tile in series due to not being able to receive
  // on all tiles in parallel.
  //
  // Exchange temporary memory is more complex as this is dependent on the
  // need to gather the input operands into contiguous memory as part of the
  // exchange or not.
  //
  // There are 2 special cases:
  //
  // The first occurs when there is no broadcast
  // of data and we assume that inputs are allocated such that they are
  // already resident on each tile. There is no exchange and no temporary
  // memory requirement for these inputs in this case.
  //
  // TODO: The second case occurs when the data is only being
  // multi-cast to one other tile and/ we don't need to gather the data
  // into one contiguous region. In this case we can simultaneously
  // send/receive from both tiles in each set. This doesn't affect
  // single-IPU planning.
  std::vector<popsolver::Variable> mCyclesPerLevel(hierarchy.size()),
      mTempBytesPerLevel(hierarchy.size());
  popsolver::Variable mSTempBytesAfterExchange = m.zero(),
                      mRTempBytesAfterExchange = m.zero();
  for (unsigned level = 0; level < hierarchy.size(); ++level) {
    // If this is the last level then we need to gather the operand
    // S as this needs to be contiguous on-tile. TODO: We don't
    // need to gather at other levels so current estimation of temp
    // memory is exaggerated.
    const auto mSBytesAreExchanged = m.min(
        {m.one(), m.sub(mSBytesPerTile[level + 1], mSBytesPerTile[level])});
    const auto mSBytesToSendReceivePerTile =
        m.product({mSBytesAreExchanged, mSBytesPerTile[level + 1]});
    const auto mSTempBytes = mSBytesToSendReceivePerTile;
    const auto mSBytesToSendReceive =
        m.product({mSBytesToSendReceivePerTile, p.tile[level + 1]});

    const auto mRBytesAreExchanged = m.min(
        {m.one(), m.sub(mRBytesPerTile[level + 1], mRBytesPerTile[level])});
    const auto mRBytesToSendReceive = m.product(
        {mRBytesAreExchanged, p.tile[level + 1], mRBytesPerTile[level + 1]});
    // Because we never need to gather R temporary memory at any stage is
    // just the difference between the bytes for original locations of
    // buckets at level 0 and the current level.
    const auto mRTempBytes =
        m.sub(mRBytesPerTile[level + 1], mRBytesPerTile[0]);

    // Using our knowledge of how the source and destination of the exchange
    // will be laid out to allow the exchange estimator to account for the
    // possibility of exchange bus sharing between tiles during the broadcast
    // of information.
    //
    // We choose a tile to process this partition based on the flattened
    // index into a 3D array with shape {y,z,x}. This means that 2
    // partitions of x will be on neighbouring tiles and input S could be
    // broadcast. Alternatively if there is only 1 partition of x then
    // 2 partitions of z will be on neighbouring tiles.
    const auto mSConsecutiveTilesReceivingSameData = p.partition[level].x;
    // Only if X is not partitioned can we broadcast R.
    const auto mXPartitionsM1 = m.sub(p.partition[level].x, m.one());
    const auto mRCanBeBroadcast =
        m.sub(m.one(), m.min({m.one(), mXPartitionsM1}));
    const auto mZPartitionsM1 = m.sub(p.partition[level].z, m.one());
    const auto mRConsecutiveTilesReceivingSameData =
        m.sum({m.one(), m.product({mRCanBeBroadcast, mZPartitionsM1})});

    const auto mSExchangeCycles = exchangeEstimator(
        mSBytesToSendReceive, mSConsecutiveTilesReceivingSameData,
        p.product[level], level);
    const auto mRExchangeCycles = exchangeEstimator(
        mRBytesToSendReceive, mRConsecutiveTilesReceivingSameData,
        p.product[level], level);
    mCyclesPerLevel[level] = m.sum({mSExchangeCycles, mRExchangeCycles});

    mTempBytesPerLevel[level] =
        m.sum({mSTempBytesAfterExchange, mSTempBytes, mRTempBytes});
    mSTempBytesAfterExchange = mSTempBytes;
    mRTempBytesAfterExchange = mRTempBytes;
  }
  CostVariables mCost(m.sum(mCyclesPerLevel), m.max(mTempBytesPerLevel));
  return std::make_tuple(mCost, mSTempBytesAfterExchange,
                         mRTempBytesAfterExchange);
}

static std::tuple<CostVariables, popsolver::Variable, popsolver::Variable>
addDistributionExchangeCostDenseDense(
    popsolver::Model &m, const Target &target, const Type &inputType,
    const Options &options, const std::vector<unsigned> &hierarchy,
    const ExchangeEstimator &exchangeEstimator,
    const std::vector<Vector<popsolver::Variable>> &mGroups,
    const Vector<popsolver::Variable> &mGrouping, const PartitionVariables &p) {

  const auto mBytesPerInput = m.addConstant(target.getTypeSize(inputType));

  std::vector<popsolver::Variable> mQGradBytesPerTile(hierarchy.size() + 1),
      mSBytesPerTile(hierarchy.size() + 1);
  for (unsigned level = 0; level < hierarchy.size() + 1; ++level) {
    mQGradBytesPerTile[level] =
        m.product({m.ceildiv(m.product({mGroups[level].groups, mGroups[level].x,
                                        mGroups[level].z}),
                             p.tile[level]),
                   mGrouping.groups, mGrouping.x, mGrouping.z, mBytesPerInput});
    mSBytesPerTile[level] =
        m.product({m.ceildiv(m.product({mGroups[level].groups, mGroups[level].y,
                                        mGroups[level].z}),
                             p.tile[level]),
                   mGrouping.groups, mGrouping.y, mGrouping.z, mBytesPerInput});
  }

  std::vector<popsolver::Variable> mCyclesPerLevel(hierarchy.size()),
      mTempBytesPerLevel(hierarchy.size());
  popsolver::Variable mQGradTempBytesAfterExchange = m.zero(),
                      mSTempBytesAfterExchange = m.zero();
  for (unsigned level = 0; level < hierarchy.size(); ++level) {
    const auto mQGradBytesAreExchanged =
        m.min({m.one(), m.sub(mQGradBytesPerTile[level + 1],
                              mQGradBytesPerTile[level])});
    const auto mQGradBytesToSendReceivePerTile =
        m.product({mQGradBytesAreExchanged, mQGradBytesPerTile[level + 1]});
    const auto mQGradTempBytes = mQGradBytesToSendReceivePerTile;
    const auto mQGradBytesToSendReceive =
        m.product({mQGradBytesToSendReceivePerTile, p.tile[level + 1]});

    const auto mSBytesAreExchanged = m.min(
        {m.one(), m.sub(mSBytesPerTile[level + 1], mSBytesPerTile[level])});
    const auto mSBytesToSendReceivePerTile =
        m.product({mSBytesAreExchanged, mSBytesPerTile[level + 1]});
    const auto mSTempBytes = mSBytesToSendReceivePerTile;
    const auto mSBytesToSendReceive =
        m.product({mSBytesToSendReceivePerTile, p.tile[level + 1]});

    // Unlikely to be able to broadcast QGrad to consecutive tiles.
    // QGrad is broadcast over forward pass Y partitions which will
    // only be consecutive if both x and z have 1 partition a-piece.
    const auto mQGradConsecutiveTilesReceivingSameData = m.one();
    const auto mSConsecutiveTilesReceivingSameData = p.partition[level].x;

    const auto mQGradExchangeCycles = exchangeEstimator(
        mQGradBytesToSendReceive, mQGradConsecutiveTilesReceivingSameData,
        p.product[level], level);
    const auto mSExchangeCycles = exchangeEstimator(
        mSBytesToSendReceive, mSConsecutiveTilesReceivingSameData,
        p.product[level], level);
    mCyclesPerLevel[level] = m.sum({mQGradExchangeCycles, mSExchangeCycles});

    mTempBytesPerLevel[level] =
        m.sum({mQGradTempBytesAfterExchange, mSTempBytesAfterExchange,
               mQGradTempBytes, mSTempBytes});
    mQGradTempBytesAfterExchange = mQGradTempBytes;
    mSTempBytesAfterExchange = mSTempBytes;
  }

  CostVariables mCost(m.sum(mCyclesPerLevel), m.max(mTempBytesPerLevel));
  return std::make_tuple(mCost, mQGradTempBytesAfterExchange,
                         mSTempBytesAfterExchange);
}

static std::tuple<unsigned, unsigned> getNumGroupsGivenUniformSparsityPattern(
    const double nzRatio, const unsigned xGroups, const unsigned yGroups) {
  const double pGroupIsZero = 1.0 - nzRatio;
  const double pXGroupHasAllZeroGroups = std::pow(pGroupIsZero, yGroups);
  const double pXGroupHasNonZeroGroup = 1.0 - pXGroupHasAllZeroGroups;
  const unsigned totalNonZeroGroups = std::ceil(xGroups * yGroups * nzRatio);
  const unsigned xNonZeroGroups = std::ceil(xGroups * pXGroupHasNonZeroGroup);
  const unsigned yNonZeroGroups = ceildiv(totalNonZeroGroups, xNonZeroGroups);
  return std::make_tuple(xNonZeroGroups, yNonZeroGroups);
}

static std::tuple<CostVariables, popsolver::Variable>
addInitialComputeCostSparseDense(
    popsolver::Model &m, const Target &target, const Type &inputType,
    const double &nzRatio, const Options &options, const OnTileMethod &method,
    const Vector<popsolver::Variable> &mGroups,
    const Vector<popsolver::Variable> &mGrouping,
    const Vector<popsolver::Variable> &mCumulativePartitions,
    const popsolver::Variable &mSTempBytes,
    const popsolver::Variable &mRTempBytes) {

  // TODO: Padding estimates etc...

  const auto mPartialsPerTile =
      m.product({mGroups.groups, mGroups.x, mGroups.z, mGrouping.groups,
                 mGrouping.x, mGrouping.z});

  const auto numWorkers = target.getNumWorkerContexts();
  const auto partialsType = options.partialsType;
  const auto mBytesPerPartial = m.addConstant(target.getTypeSize(partialsType));
  const auto mNumBucketsPerTile = mCumulativePartitions.z;

  const auto mCycles = m.call(
      {mPartialsPerTile, mNumBucketsPerTile, mGroups.x, mGroups.y, mGroups.z,
       mGrouping.x, mGrouping.y, mGrouping.z},
      [=](const std::vector<unsigned> &values) {
        const auto partialsPerTile = values[0];
        const auto numBuckets = values[1];
        const auto xGroups = values[2];
        const auto yGroups = values[3];
        const auto zGroups = values[4];
        const auto xGrouping = values[5];
        const auto yGrouping = values[6];
        const auto zGrouping = values[7];
        const auto partialsPerWorker = ceildiv(partialsPerTile, numWorkers);
        std::uint64_t cycles = zeroPartialsCycles(
            partialsPerWorker, numWorkers, options.partialsType == FLOAT);

        unsigned xNonZeroGroups, yNonZeroGroups;
        std::tie(xNonZeroGroups, yNonZeroGroups) =
            getNumGroupsGivenUniformSparsityPattern(nzRatio, xGroups, yGroups);

        const auto workerTiles =
            splitTileBetweenWorkers(xNonZeroGroups, zGroups, numWorkers);

        std::uint64_t maxMulCycles = 0;
        for (const auto &workerTile : workerTiles) {
          const auto numXPerWorker = workerTile.getRows().size() * xGrouping;
          const auto numZPerWorker = workerTile.getColumns().size() * zGrouping;
          const auto numY = yNonZeroGroups * yGrouping;

          // Because we are assuming best case with perfectly uniform
          // distribution of sparsity over the dense sparse of R, there should
          // be a perfect distribution of sub-groups over buckets such that each
          // bucket only contains elements of 1 sub-group.
          constexpr auto numSubGroupsPerBucket = 1u;

          std::uint64_t mulCycles = 0;
          switch (method) {
          case OnTileMethod::Forward:
            mulCycles = sparseDenseElementwiseMultiply(
                numBuckets, numBuckets, numSubGroupsPerBucket, numXPerWorker,
                numZPerWorker, std::vector<unsigned>({numY}),
                inputType == FLOAT, partialsType == FLOAT, numWorkers);
            break;
          case OnTileMethod::GradA:
            mulCycles = sparseDenseGradAElementwiseMultiply(
                numBuckets, numBuckets, numSubGroupsPerBucket, numXPerWorker,
                numZPerWorker, std::vector<unsigned>({numY}),
                inputType == FLOAT, partialsType == FLOAT, numWorkers);
            break;
          case OnTileMethod::Transpose:
            // The transpose method divides the work along the X-dimension.
            mulCycles = sparseDenseTransposeElementwiseMultiply(
                numBuckets, numBuckets, numSubGroupsPerBucket, numY, zGroups,
                std::vector<unsigned>({xNonZeroGroups}), inputType == FLOAT,
                partialsType == FLOAT, numWorkers);
            break;
          default:
            throw poputil::poplibs_error("Unhandled method when planning");
          }
          maxMulCycles = std::max(maxMulCycles, mulCycles);
        }
        cycles += maxMulCycles;

        return cycles;
      });

  // The temporary memory during this operation is the temporary memory for
  // both the inputs, and the memory for partial outputs. Memory for partial
  // outputs is only temporary if there is a cast or reduction to be done
  // later on.
  const auto mNeedsCast = m.addConstant(inputType != partialsType ? 1u : 0u);
  const auto mNeedsReduction = m.sub(mCumulativePartitions.z, m.one());
  const auto mNeedsCastOrReduction =
      m.min({m.one(), m.sum({mNeedsCast, mNeedsReduction})});

  const auto mPartialsTempBytes =
      m.product({mNeedsCastOrReduction, mPartialsPerTile, mBytesPerPartial});
  const auto mTempBytes = m.sum({mSTempBytes, mRTempBytes, mPartialsTempBytes});
  return std::make_tuple(CostVariables(mCycles, mTempBytes),
                         mPartialsTempBytes);
}

static std::tuple<CostVariables, popsolver::Variable>
addInitialComputeCostDenseDense(
    popsolver::Model &m, const Target &target, const Type &inputType,
    const double &nzRatio, const Options &options, const OnTileMethod &method,
    const Vector<popsolver::Variable> &mGroups,
    const Vector<popsolver::Variable> &mGrouping,
    const Vector<popsolver::Variable> &mCumulativePartitions,
    const popsolver::Variable &mSparseElems,
    const popsolver::Variable &mQGradTempBytes,
    const popsolver::Variable &mSTempBytes) {
  const auto mPartialsPerTile = mSparseElems;

  const auto numWorkers = target.getNumWorkerContexts();
  const auto &partialsType = options.partialsType;
  const auto mBytesPerPartial = m.addConstant(target.getTypeSize(partialsType));

  const auto mCycles = m.call(
      {mPartialsPerTile, mGroups.x, mGroups.y, mGroups.z, mGrouping.x,
       mGrouping.y, mGrouping.z},
      [=](const std::vector<unsigned> &values) {
        const auto partialsPerTile = values[0];
        const auto xGroups = values[1];
        const auto yGroups = values[2];
        const auto zGroups = values[3];
        const auto xGrouping = values[4];
        const auto yGrouping = values[5];
        const auto zGrouping = values[6];
        const auto partialsPerWorker = ceildiv(partialsPerTile, numWorkers);

        std::uint64_t cycles = zeroPartialsCycles(partialsPerWorker, numWorkers,
                                                  partialsType == FLOAT);

        unsigned xNonZeroGroups, yNonZeroGroups;
        std::tie(xNonZeroGroups, yNonZeroGroups) =
            getNumGroupsGivenUniformSparsityPattern(nzRatio, xGroups, yGroups);
        unsigned nonZeroGroups = xNonZeroGroups * yNonZeroGroups;
        const auto groupsPerWorker = ceildiv(nonZeroGroups, numWorkers);
        const auto numUsedWorkers = ceildiv(nonZeroGroups, groupsPerWorker);

        const auto numZ = zGroups * zGrouping;

        std::uint64_t maxMulCycles = 0;
        for (unsigned worker = 0; worker < numUsedWorkers; ++worker) {
          auto startGroup = worker * groupsPerWorker;
          auto endGroup =
              std::min(nonZeroGroups, (worker + 1) * groupsPerWorker);

          const auto numXGroupsThisWorker =
              ceildiv(endGroup, yNonZeroGroups) -
              floordiv(startGroup, yNonZeroGroups);
          std::vector<unsigned> numYThisWorker;
          numYThisWorker.reserve(numXGroupsThisWorker);
          while (startGroup != endGroup) {
            const auto numYGroupsForXGroup =
                std::min(endGroup, startGroup + yNonZeroGroups) - startGroup;
            numYThisWorker.emplace_back(numYGroupsForXGroup * yGrouping);
            startGroup += numYGroupsForXGroup;
          }

          constexpr auto numBuckets = 1u;
          constexpr auto numSubGroupsPerBucket = 1u;

          const auto numXThisWorker = numXGroupsThisWorker * xGrouping;
          std::uint64_t mulCycles = 0;
          switch (method) {
          case OnTileMethod::GradW:
            mulCycles = sparseDenseGradWElementwiseMultiply(
                numBuckets, numBuckets, numSubGroupsPerBucket, numXThisWorker,
                numZ, numYThisWorker, inputType == FLOAT, partialsType == FLOAT,
                numWorkers);
            // Average over different values of Y. TODO: The Y provided aren't
            // statistically significant, they just assume a rectangle and
            // divide between workers so there is some accounting for overheads.
            mulCycles = ceildiv(mulCycles, numYThisWorker.size());
            break;
          default:
            throw poputil::poplibs_error("Unhandled method when planning");
          }
          maxMulCycles = std::max(maxMulCycles, mulCycles);
        }
        cycles += maxMulCycles;

        return cycles;
      });
  const auto mNeedsCast = m.addConstant(inputType != partialsType ? 1u : 0u);
  const auto mNeedsReduction = m.sub(mCumulativePartitions.z, m.one());
  const auto mNeedsCastOrReduction =
      m.min({m.one(), m.sum({mNeedsCast, mNeedsReduction})});

  const auto mPartialsTempBytes =
      m.product({mNeedsCastOrReduction, mPartialsPerTile, mBytesPerPartial});
  const auto mTempBytes =
      m.sum({mQGradTempBytes, mSTempBytes, mPartialsTempBytes});
  return std::make_tuple(CostVariables(mCycles, mTempBytes),
                         mPartialsTempBytes);
}

static std::tuple<CostVariables, popsolver::Variable>
addPropagatingExchangeCost(popsolver::Model &m, const Target &target,
                           const Type &inputType,
                           const std::vector<unsigned> &hierarchy,
                           const Options &options,
                           const ExchangeEstimator &exchangeEstimator,
                           const popsolver::Variable &mBytesPerBucket,
                           const PartitionVariables &p) {
  // Estimate cost of a single iteration of the dynamically executed exchange
  // based on this plan.
  std::vector<popsolver::Variable> mTempBytesPerLevel(hierarchy.size());
  auto mTempBytesAfterExchange = m.zero();
  for (unsigned level = 0; level < hierarchy.size(); ++level) {
    // During the propagating exchange, we will need space for 2 buckets which
    // we will flip flop between to allow simulatenous forwarding and receiving
    // of buckets to/from other tiles. For the timebeing we won't treat this
    // as using the home location as one of the 2 buffers hence temporary
    // memory is 2x size of a single bucket.
    const auto mTempBytes = m.product({mBytesPerBucket, m.addConstant(2u)});
    mTempBytesPerLevel[level] = mTempBytes;
    mTempBytesAfterExchange = mTempBytes;
  }
  CostVariables mCost(m.zero(), m.max(mTempBytesPerLevel));
  return std::make_tuple(mCost, mTempBytesAfterExchange);
}

static std::tuple<CostVariables, CostVariables, popsolver::Variable>
addReductionCost(
    popsolver::Model &m, const Target &target, const Type &inputType,
    const std::vector<unsigned> &hierarchy, const Options &options,
    const ExchangeEstimator &exchangeEstimator,
    const popsolver::Variable &mPartialsPerTileToReduce,
    const std::vector<popsolver::Variable> &mReductionDepth,
    const std::vector<popsolver::Variable> &mReductionDepthCumulative,
    const std::vector<popsolver::Variable> &mTileLevelPartitions,
    popsolver::Variable mQTempBytesAfterCompute) {
  // This is not dependent upon the distribution of the sparsity
  // pattern as we are reducing the dense output. This occurs after all other
  // steps of exchange and compute are complete.
  //
  // The cost of reduction is determined by the factor by which we reduce.
  //
  // There is no on-tile reduction naturally as partials for the same result
  // are partitioned between tiles.
  const auto mBytesPerPartial =
      m.addConstant(target.getTypeSize(options.partialsType));
  std::vector<popsolver::Variable> mPartialsPerTile(hierarchy.size() + 1);
  std::vector<popsolver::Variable> mExchangeCyclesPerLevel(hierarchy.size()),
      mExchangeTempBytesPerLevel(hierarchy.size()),
      mComputeCyclesPerLevel(hierarchy.size()),
      mComputeTempBytesPerLevel(hierarchy.size());
  const auto numWorkers = target.getNumWorkerContexts();
  const auto dataPathWidth = target.getDataPathWidth();
  for (int level = hierarchy.size(); level >= 0; --level) {
    if (static_cast<unsigned>(level) == hierarchy.size()) {
      mPartialsPerTile[level] = mPartialsPerTileToReduce;
    } else {
      // Now estimate compute portion of reduction exchange cost.
      const auto reducePartialsType = options.partialsType;
      const auto reduceOutputType =
          (level == 0) ? inputType : options.partialsType;
      bool floatPartials = reducePartialsType == FLOAT;
      bool floatOutput = reduceOutputType == FLOAT;
      const auto partialsVectorWidth =
          target.getVectorWidth(reducePartialsType);
      const auto outputVectorWidth = target.getVectorWidth(reduceOutputType);
      const auto mBytesPerOutput =
          m.addConstant(target.getTypeSize(reduceOutputType));

      mPartialsPerTile[level] =
          m.ceildiv(mPartialsPerTile[level + 1], mReductionDepth[level]);

      const auto mNeedsReduction = m.min(
          {m.one(), m.sub(mReductionDepthCumulative[level + 1], m.one())});

      // The reduction's exchange cost will be given by each tile needing to
      // receive (reductionDepth - 1)/reductionDepth of the partials, and
      // send 1/reductionDepth of the partials. > 2 reductionFactor means we
      // cannot overlap send/receive of partials so cost is based on full
      // partials size. This is an all-to-all exchange.
      const auto mPartialsToExchangePerTile = mPartialsPerTile[level + 1];
      const auto mBytesToExchangePerTile = m.product(
          {mPartialsToExchangePerTile, mBytesPerPartial, mNeedsReduction});
      const auto mBytesToExchange =
          m.product({mBytesToExchangePerTile, mTileLevelPartitions[level + 1]});
      mExchangeCyclesPerLevel[level] =
          exchangeEstimator(mBytesToExchange, level);
      mExchangeTempBytesPerLevel[level] =
          m.sum({mQTempBytesAfterCompute, mBytesToExchangePerTile});

      mComputeCyclesPerLevel[level] = m.call(
          {mPartialsPerTile[level], mReductionDepth[level]},
          [=](const std::vector<unsigned> &values) -> unsigned {
            const auto partialsPerTile = values[0];
            const auto reductionDepth = values[1];

            if (reductionDepth == 0) {
              return 0;
            }

            if (reductionDepth == 1) {
              if (floatOutput == floatPartials) {
                return 0;
              } else {
                return getCastCycleEstimate(partialsPerTile,
                                            partialsVectorWidth,
                                            outputVectorWidth, numWorkers);
              }
            }

            return getReduceCycleEstimate(partialsPerTile, reductionDepth,
                                          dataPathWidth, floatOutput,
                                          floatPartials, numWorkers);
          });
      const auto mNeedsCast =
          m.addConstant(reducePartialsType != inputType ? 1u : 0u);
      const auto mNeedsCastOrReduction =
          m.min({m.one(), m.sum({mNeedsCast, mNeedsReduction})});

      mQTempBytesAfterCompute = m.product(
          {mNeedsCastOrReduction, mPartialsPerTile[level], mBytesPerOutput});
      mComputeTempBytesPerLevel[level] =
          m.sum({mExchangeTempBytesPerLevel[level], mQTempBytesAfterCompute});
    }
  }
  CostVariables mExchangeCost(m.sum(mExchangeCyclesPerLevel),
                              m.max(mExchangeTempBytesPerLevel));
  CostVariables mComputeCost(m.sum(mComputeCyclesPerLevel),
                             m.max(mComputeTempBytesPerLevel));
  return std::make_tuple(mExchangeCost, mComputeCost, mQTempBytesAfterCompute);
}

static std::tuple<CostVariables, CostBreakdownVariables>
addEstimates(const Target &target, const Type &inputType,
             const Vector<std::size_t> &shape,
             const SparsityParams &sparsityParams, const double &nzRatio,
             const OnTileMethod &method, const std::vector<unsigned> &hierarchy,
             const ExchangeEstimator &exchangeEstimator, popsolver::Model &m,
             const PartitionVariables &p,
             const std::vector<Vector<popsolver::Variable>> &mGroups,
             const Vector<popsolver::Variable> &mGrouping,
             const popsolver::Variable &mRElemsPerBucket,
             const popsolver::Variable &mRMetaInfoElemsPerBucket,
             const Options &options) {

  CostBreakdownVariables costBreakdown;

  CostVariables mDistributionExchangeCost;
  popsolver::Variable mSTempBytesAfterExchange, mRTempBytesAfterExchange;
  std::tie(mDistributionExchangeCost, mSTempBytesAfterExchange,
           mRTempBytesAfterExchange) =
      addDistributionExchangeCostSparseDense(
          m, target, inputType, metaInfoType, options, hierarchy,
          exchangeEstimator, mGroups, mGrouping, mRElemsPerBucket,
          mRMetaInfoElemsPerBucket, p);
  costBreakdown.emplace_back("Initial distribution exchange",
                             mDistributionExchangeCost);

  CostVariables mInitialComputeCost;
  popsolver::Variable mQTempBytesAfterCompute;
  std::tie(mInitialComputeCost, mQTempBytesAfterCompute) =
      addInitialComputeCostSparseDense(
          m, target, inputType, nzRatio, options, method, mGroups.back(),
          mGrouping, p.cumulative.back(), mSTempBytesAfterExchange,
          mRTempBytesAfterExchange);
  costBreakdown.emplace_back("Initial compute", mInitialComputeCost);

  CostVariables mPropagatingExchangeCost;
  const auto mBytesPerMetaInfoElem =
      m.addConstant(target.getTypeSize(metaInfoType));
  const auto mRBytesPerBucket =
      m.product({mRElemsPerBucket, mBytesPerMetaInfoElem});
  std::tie(mPropagatingExchangeCost, mRTempBytesAfterExchange) =
      addPropagatingExchangeCost(m, target, inputType, hierarchy, options,
                                 exchangeEstimator, mRBytesPerBucket, p);
  mPropagatingExchangeCost.tempBytes =
      m.sum({mPropagatingExchangeCost.tempBytes, mSTempBytesAfterExchange,
             mQTempBytesAfterCompute});
  costBreakdown.emplace_back("Propagating exchange (per-iteration)",
                             mPropagatingExchangeCost);

  CostVariables mReductionExchangeCost, mReductionComputeCost;
  const popsolver::Variable mPartialsPerTileToReduce =
      m.product({mGroups.back().groups, mGroups.back().x, mGroups.back().z,
                 mGrouping.groups, mGrouping.x, mGrouping.z});
  std::vector<popsolver::Variable> mReductionDepth(hierarchy.size()),
      mReductionDepthCumulative(hierarchy.size() + 1);
  for (unsigned level = 0; level < hierarchy.size() + 1; ++level) {
    if (level < hierarchy.size()) {
      mReductionDepth[level] = p.partition[level].y;
    }
    mReductionDepthCumulative[level] = p.cumulative[level].y;
  }
  std::tie(mReductionExchangeCost, mReductionComputeCost,
           mQTempBytesAfterCompute) =
      addReductionCost(m, target, inputType, hierarchy, options,
                       exchangeEstimator, mPartialsPerTileToReduce,
                       mReductionDepth, mReductionDepthCumulative, p.tile,
                       mQTempBytesAfterCompute);
  costBreakdown.emplace_back("Exchange to reduce", mReductionExchangeCost);
  costBreakdown.emplace_back("Reduction or cast", mReductionComputeCost);

  CostVariables cost(
      m.sum({mDistributionExchangeCost.cycles, mInitialComputeCost.cycles,
             mPropagatingExchangeCost.tempBytes, mReductionExchangeCost.cycles,
             mReductionComputeCost.cycles}),
      m.max({mDistributionExchangeCost.tempBytes, mInitialComputeCost.tempBytes,
             mPropagatingExchangeCost.tempBytes,
             mReductionExchangeCost.tempBytes,
             mReductionComputeCost.tempBytes}));
  costBreakdown.emplace_back("Total", cost);
  return std::make_tuple(cost, costBreakdown);
}

static std::tuple<CostVariables, CostBreakdownVariables> addEstimatesGradW(
    const Target &target, const Type &inputType,
    const Vector<std::size_t> &shape, const SparsityParams &sparsityParams,
    const double nzRatio, const OnTileMethod &method,
    const std::vector<unsigned> &hierarchy,
    const ExchangeEstimator &exchangeEstimator, popsolver::Model &m,
    const PartitionVariables &p,
    const std::vector<Vector<popsolver::Variable>> &mGroups,
    const Vector<popsolver::Variable> &mGrouping,
    const popsolver::Variable &mRElemsPerBucket, const Options &options) {
  CostBreakdownVariables costBreakdown;

  CostVariables mInitialExchangeCost;
  popsolver::Variable mQGradTempBytesAfterExchange, mSTempBytesAfterExchange;
  std::tie(mInitialExchangeCost, mQGradTempBytesAfterExchange,
           mSTempBytesAfterExchange) =
      addDistributionExchangeCostDenseDense(m, target, inputType, options,
                                            hierarchy, exchangeEstimator,
                                            mGroups, mGrouping, p);
  costBreakdown.emplace_back("Initial exchange", mInitialExchangeCost);

  // Our GradW vertex does not handle multiple inputs currently, therefore
  // the initial distribution theoretically introduces no exchange unless
  // the input came from another layer (quite likely but for now it's okay).
  CostVariables mInitialComputeCost;
  popsolver::Variable mRGradTempBytesAfterCompute;
  std::tie(mInitialComputeCost, mRGradTempBytesAfterCompute) =
      addInitialComputeCostDenseDense(
          m, target, inputType, nzRatio, options, method, mGroups.back(),
          mGrouping, p.cumulative.back(), mRElemsPerBucket,
          mQGradTempBytesAfterExchange, mSTempBytesAfterExchange);
  costBreakdown.emplace_back("Initial compute", mInitialComputeCost);

  CostVariables mPropagatingExchangeCost;
  popsolver::Variable mQGradAndSTempBytesAfterExchange; // NOTE: Unused
  // The temporary memory cost is that of both the buffers for QGrad and for S
  // so just do these together. The cycle cost is way more complicated but
  // not accounted for here.
  std::tie(mPropagatingExchangeCost, mQGradAndSTempBytesAfterExchange) =
      addPropagatingExchangeCost(
          m, target, inputType, hierarchy, options, exchangeEstimator,
          m.sum({mQGradTempBytesAfterExchange, mSTempBytesAfterExchange}), p);
  mPropagatingExchangeCost.tempBytes =
      m.sum({mPropagatingExchangeCost.tempBytes, mRGradTempBytesAfterCompute});
  costBreakdown.emplace_back("Propagating exchange (per-iteration)",
                             mPropagatingExchangeCost);

  const popsolver::Variable mPartialsPerTileToReduce = mRElemsPerBucket;
  std::vector<popsolver::Variable> mReductionDepth(hierarchy.size(), m.one()),
      mReductionDepthCumulative(hierarchy.size() + 1, m.one());
  CostVariables mReductionExchangeCost, mReductionComputeCost;
  std::tie(mReductionExchangeCost, mReductionComputeCost,
           mRGradTempBytesAfterCompute) =
      addReductionCost(m, target, inputType, hierarchy, options,
                       exchangeEstimator, mPartialsPerTileToReduce,
                       mReductionDepth, mReductionDepthCumulative, p.tile,
                       mRGradTempBytesAfterCompute);
  costBreakdown.emplace_back("Exchange to reduce", mReductionExchangeCost);
  costBreakdown.emplace_back("Reduction or cast", mReductionComputeCost);

  CostVariables cost(
      m.sum({mInitialExchangeCost.cycles, mInitialComputeCost.cycles,
             mPropagatingExchangeCost.cycles, mReductionExchangeCost.cycles,
             mReductionComputeCost.cycles}),
      m.max({mInitialExchangeCost.tempBytes, mInitialComputeCost.tempBytes,
             mPropagatingExchangeCost.tempBytes,
             mReductionExchangeCost.tempBytes,
             mReductionComputeCost.tempBytes}));
  costBreakdown.emplace_back("Total", cost);
  return std::make_tuple(cost, costBreakdown);
}

static std::tuple<Plan, Cost, CostBreakdown>
createPlan(const PlanningObjective &objective, const Target &target,
           const Type &inputType, const FullyConnectedParams &params,
           const Options &options) {

  const auto numIPUs = target.getNumIPUs();
  const auto hierarchy =
      poplibs::getTileHierarchy(numIPUs, target.getTilesPerIPU());
  const auto perLevelExchangeBytesPerCycle =
      poplibs::getPerLevelExchangeBytesPerCycle(target, numIPUs);

  // For now we just handle single-IPU for simplicity. Handling further
  // levels should not be significantly harder functionally however.
  assert(hierarchy.size() == 1);

  // TODO: This should be based on the vertex we are going to use but also
  // the operand which is sparse. We only have an element-wise MAC vertex
  // currently and no sparse weight update vertex so it's unclear what the
  // requirements are yet.
  Vector<unsigned> grouping = {
      1,                                // groups
      1,                                // x
      1,                                // y
      target.getVectorWidth(inputType), // z
  };
  Vector<unsigned> size = {
      static_cast<unsigned>(params.getNumGroups()),              // groups
      static_cast<unsigned>(params.getOutputChannelsPerGroup()), // x
      static_cast<unsigned>(params.getInputChannelsPerGroup()),  // y
      static_cast<unsigned>(params.getBatchSize()),              // z
  };
  Vector<unsigned> groups =
      size.binaryOp(grouping, [&](const auto size, const auto grouping) {
        return ceildiv(size, grouping);
      });

  popsolver::Model m;
  // Create partitions variables
  const PartitionVariables fwdPartition = [&] {
    std::vector<Vector<popsolver::Variable>> mPartitions(hierarchy.size());
    for (unsigned level = 0; level < hierarchy.size(); ++level) {
      mPartitions[level] = Vector<popsolver::Variable>::generate(
          [&] { return m.addVariable(1, hierarchy[level]); });
    }
    return PartitionVariables(m, mPartitions);
  }();

  // Calculate grains, add constraints on partitions
  std::vector<Vector<popsolver::Variable>> mFwdGroups(hierarchy.size() + 1);
  mFwdGroups[0] = groups.transform<popsolver::Variable>(
      [&](const auto groups) { return m.addConstant(groups); });
  for (unsigned level = 0; level < hierarchy.size(); ++level) {
    m.lessOrEqual(fwdPartition.product[level], hierarchy[level]);
    mFwdGroups[level + 1] = mFwdGroups[level].binaryOp(
        fwdPartition.partition[level],
        [&](const auto &groups, const auto &partition) {
          return m.ceildivConstrainDivisor(groups, partition);
        });

    // Partitions of Z must be of equal size on every tile.
    m.factorOf(mFwdGroups[level].z, fwdPartition.partition[level].z);

    // Our vertex doesn't handle groups at all.
    if (level == hierarchy.size() - 1) {
      m.equal(mFwdGroups[level + 1].groups, 1u);
    }
  }

  // Number of subgroups per bucket for memory planning
  constexpr unsigned numSubgroupsPerBucket = 2U;

  // Calculate size of buckets.
  const unsigned bytesPerMetaInfoElem = target.getTypeSize(metaInfoType);
  const unsigned bytesPerInputElem = target.getTypeSize(inputType);
  const unsigned exchangeAtomSize = target.getExchangeBytesPerCycle();
  const auto metaInfoElemsPerExchangeAtom =
      lcm(bytesPerMetaInfoElem, exchangeAtomSize) / bytesPerMetaInfoElem;
  const auto inputElemsPerExchangeAtom =
      lcm(bytesPerInputElem, exchangeAtomSize) / bytesPerInputElem;
  const auto rElems = params.getNumGroups() *
                      unsigned(std::ceil(params.getInputChannelsPerGroup() *
                                         params.getOutputChannelsPerGroup() *
                                         params.getNzRatio()));
  const auto mRElems = m.addConstant(rElems);
  const auto mRElemsPerBucket = [&] {
    auto mElems = m.ceildiv(mRElems, fwdPartition.tile.at(0));
    return m.call({mElems}, [=](const std::vector<unsigned> &values) {
      const unsigned elems = std::round(
          values[0] * (1.0 + options.metaInfoBucketOversizeProportion));
      return roundUp(elems, inputElemsPerExchangeAtom);
    });
  }();

  auto calcFwdBucketSize = [=](const std::vector<unsigned> &values) {
    const auto xGroups = values[0];
    const auto yGroups = values[1];
    unsigned xNonZeroGroups, yNonZeroGroups;
    std::tie(xNonZeroGroups, yNonZeroGroups) =
        getNumGroupsGivenUniformSparsityPattern(params.getNzRatio(), xGroups,
                                                yGroups);

    // Knowing that we use a CSR based format we can calculate the
    // number of elements of meta-info that would be required to
    // store this in a perfect world.
    const auto outputEntryElems =
        sizeof(MetaInfo<MetaInfoType>::OutputEntry) / sizeof(MetaInfoType);
    const auto subGroupElems =
        sizeof(MetaInfo<MetaInfoType>::SubGroupEntry) / sizeof(MetaInfoType);
    const auto workerEntryElems =
        sizeof(MetaInfo<MetaInfoType>::WorkerEntry) / sizeof(MetaInfoType);
    const auto numElemsPerfectlyUniform =
        xNonZeroGroups * (outputEntryElems + yNonZeroGroups);
    const auto gradWWorkerEntryElems =
        options.doGradWPass
            ? (1 + sizeof(MetaInfo<MetaInfoType>::GradWWorkerEntry) /
                       sizeof(MetaInfoType))
            : 0;

    const unsigned elems =
        (subGroupElems + target.getNumWorkerContexts() *
                             (workerEntryElems + gradWWorkerEntryElems)) *
            numSubgroupsPerBucket +
        std::ceil(numElemsPerfectlyUniform *
                  (1.0 + options.metaInfoBucketOversizeProportion));
    return roundUp(elems, metaInfoElemsPerExchangeAtom);
  };

  const auto mRFwdMetaInfoElemsPerBucket =
      m.call({mFwdGroups.back().x, mFwdGroups.back().y}, calcFwdBucketSize);

  const auto mFwdGrouping = grouping.transform<popsolver::Variable>(
      [&](const auto grouping) { return m.addConstant(grouping); });

  CostVariables fwdCost;
  CostBreakdownVariables fwdCostBreakdown;
  const Vector<std::size_t> fwdShape = {
      params.getNumGroups(),
      params.getOutputChannelsPerGroup(),
      params.getInputChannelsPerGroup(),
      params.getBatchSize(),
  };
  const ExchangeEstimator exchangeEstimator(m, target, hierarchy,
                                            perLevelExchangeBytesPerCycle);
  const auto fwdMethod = OnTileMethod::Forward;
  std::tie(fwdCost, fwdCostBreakdown) =
      addEstimates(target, inputType, fwdShape, params.getSparsityParams(),
                   params.getNzRatio(), fwdMethod, hierarchy, exchangeEstimator,
                   m, fwdPartition, mFwdGroups, mFwdGrouping, mRElemsPerBucket,
                   mRFwdMetaInfoElemsPerBucket, options);

  // TODO: This could eventually be based on a memory/cycle tradeoff
  const auto gradAMethod =
      options.sharedBuckets ? OnTileMethod::Transpose : OnTileMethod::GradA;
  CostVariables gradACost(m.zero(), m.zero());
  CostBreakdownVariables gradACostBreakdown;
  popsolver::Variable mRGradAMetaInfoElemsPerBucket = m.zero();
  if (options.doGradAPass) {
    // TODO: Encapsulate this translation to GradA pass better.
    // This is just a swizzle applied to all vectors in 'planning
    // space'.
    const auto toGradA = [](const auto fwdV) {
      return decltype(fwdV){
          fwdV.groups,
          fwdV.y,
          fwdV.x,
          fwdV.z,
      };
    };

    const auto gradAShape = toGradA(fwdShape);
    const auto gradAPartition = [&] {
      auto gradA = fwdPartition;
      for (auto &p : gradA.partition) {
        p = toGradA(p);
      }
      for (auto &p : gradA.cumulative) {
        p = toGradA(p);
      }
      return gradA;
    }();
    const auto mGradAGroups = [&] {
      auto v = mFwdGroups;
      for (auto &g : v) {
        g = toGradA(g);
      }
      return v;
    }();
    const auto mGradAGrouping = toGradA(mFwdGrouping);

    auto calcGradABucketSize = [=](const std::vector<unsigned> &values) {
      const auto xGroups = values[0];
      const auto yGroups = values[1];
      unsigned xNonZeroGroups, yNonZeroGroups;
      std::tie(xNonZeroGroups, yNonZeroGroups) =
          getNumGroupsGivenUniformSparsityPattern(params.getNzRatio(), xGroups,
                                                  yGroups);

      // Knowing that we use a CSR based format we can calculate the
      // number of elements of meta-info that would be required to
      // store this in a perfect world.
      const auto outputEntryElems =
          sizeof(MetaInfo<MetaInfoType>::OutputEntry) / sizeof(MetaInfoType);
      const auto subGroupElems =
          sizeof(MetaInfo<MetaInfoType>::SubGroupEntry) / sizeof(MetaInfoType);
      const auto workerEntryElems =
          sizeof(MetaInfo<MetaInfoType>::WorkerEntry) / sizeof(MetaInfoType);
      // yNonZeroGroups * 2 because we encode information to transpose
      // weights along with offsets for inputs if GradA method is selected
      // other wise the same bucket as forward is used
      const auto elementsPerY = gradAMethod == OnTileMethod::GradA ? 2 : 1;
      const auto numElemsPerfectlyUniform =
          xNonZeroGroups * (outputEntryElems + yNonZeroGroups * elementsPerY);
      const unsigned elems =
          (subGroupElems + target.getNumWorkerContexts() * workerEntryElems) *
              numSubgroupsPerBucket +
          std::ceil(numElemsPerfectlyUniform *
                    (1.0 + options.metaInfoBucketOversizeProportion));
      return roundUp(elems, metaInfoElemsPerExchangeAtom);
    };

    if (gradAMethod == OnTileMethod::Transpose) {
      // We actually use the same buckets as forward and for a joint plan
      // the split should just be the tranpose of the forward.
      mRGradAMetaInfoElemsPerBucket = m.call(
          {mGradAGroups.back().y, mGradAGroups.back().x}, calcFwdBucketSize);
    } else {
      mRGradAMetaInfoElemsPerBucket = m.call(
          {mGradAGroups.back().x, mGradAGroups.back().y}, calcGradABucketSize);
    }

    std::tie(gradACost, gradACostBreakdown) = addEstimates(
        target, inputType, gradAShape, params.getSparsityParams(),
        params.getNzRatio(), gradAMethod, hierarchy, exchangeEstimator, m,
        gradAPartition, mGradAGroups, mGradAGrouping, mRElemsPerBucket,
        mRGradAMetaInfoElemsPerBucket, options);
  }

  const auto gradWMethod = OnTileMethod::GradW;
  CostVariables gradWCost(m.zero(), m.zero());
  CostBreakdownVariables gradWCostBreakdown;
  if (options.doGradWPass) {
    std::tie(gradWCost, gradWCostBreakdown) = addEstimatesGradW(
        target, inputType, fwdShape, params.getSparsityParams(),
        params.getNzRatio(), gradWMethod, hierarchy, exchangeEstimator, m,
        fwdPartition, mFwdGroups, mFwdGrouping, mRElemsPerBucket, options);
  }

  const CostVariables cost(
      m.sum({fwdCost.cycles, gradACost.cycles, gradWCost.cycles}),
      m.max({fwdCost.tempBytes, gradACost.tempBytes, gradWCost.tempBytes}));
  CostBreakdownVariables costBreakdown;
  for (auto &entry : fwdCostBreakdown) {
    costBreakdown.emplace_back("Fwd: " + std::move(entry.first),
                               std::move(entry.second));
  }
  for (auto &entry : gradACostBreakdown) {
    costBreakdown.emplace_back("GradA: " + std::move(entry.first),
                               std::move(entry.second));
  }
  for (auto &entry : gradWCostBreakdown) {
    costBreakdown.emplace_back("GradW: " + std::move(entry.first),
                               std::move(entry.second));
  }

  popsolver::Solution solution;
  switch (objective.getType()) {
  case PlanningObjective::MINIMIZE_CYCLES:
    m.lessOrEqual(cost.tempBytes, objective.getTileTempMemoryBound());
    solution = m.minimize({cost.cycles, cost.tempBytes});
    break;
  case PlanningObjective::MINIMIZE_TILE_TEMP_MEMORY:
    m.lessOrEqual(cost.cycles, objective.getCyclesBound());
    solution = m.minimize({cost.tempBytes, cost.cycles});
    break;
  }

  if (!solution.validSolution()) {
    return {Plan(), highestCost, {}};
  }

  Plan plan;
  plan.grouping = grouping;
  plan.partition = fwdPartition.partition[0].transform<unsigned>(
      [&](auto partitionVar) { return solution[partitionVar]; });
  // For now these are hard-coded but we could plan for it further
  // down the road if temporary memory did not allow this
  plan.initialDistributionBucketPartition = plan.partition;
  plan.initialDistributionBucketPartition.z = 1;
  plan.nzElemsPerBucket = solution[mRElemsPerBucket];
  plan.fwdMetaInfoElemsPerBucket = solution[mRFwdMetaInfoElemsPerBucket];
  plan.gradAMetaInfoElemsPerBucket = solution[mRGradAMetaInfoElemsPerBucket];
  plan.mappingOrder = PartitionToPNMappingOrder::FwdLinearGYZX;
  plan.fwdMethod = fwdMethod;
  plan.gradAMethod = gradAMethod;
  plan.gradWMethod = gradWMethod;

  Cost bestCost;
  bestCost.cycles = solution[cost.cycles];
  bestCost.tempBytes = solution[cost.tempBytes];

  CostBreakdown bestCostBreakdown;
  bestCostBreakdown.reserve(costBreakdown.size());
  for (const auto &entry : costBreakdown) {
    bestCostBreakdown.emplace_back(
        std::move(entry.first),
        Cost(solution[entry.second.cycles], solution[entry.second.tempBytes]));
  }

  return std::make_tuple(std::move(plan), std::move(bestCost),
                         std::move(bestCostBreakdown));
}

static std::tuple<Plan, Cost> runPlanner(const Target &target,
                                         const Type &inputType,
                                         const FullyConnectedParams &params,
                                         const Options &options) {
  Plan plan;
  auto cost = highestCost;
  CostBreakdown costBreakdown;

  const unsigned availableTileMem =
      target.getBytesPerTile() * options.availableMemoryProportion;

  if (availableTileMem != 0) {
    logging::debug(
        "Planning sparse-dense matrix multiply with a per-tile memory "
        "limit of {} bytes.",
        availableTileMem);
    auto objective = PlanningObjective::minimizeCycles();
    objective.setTileTempMemoryBound(availableTileMem);

    std::tie(plan, cost, costBreakdown) =
        createPlan(objective, target, inputType, params, options);
  } else {
    logging::debug(
        "Planning sparse-dense matrix multiply with unlimited memory usage.");
  }

  if (cost == highestCost) {
    if (availableTileMem != 0) {
      logging::warn("Warning: sparse-dense matmul planner unable to meet "
                    "memory target; retrying while targeting minimum memory.");
    } else {
      logging::debug("Planning sparse-dense matmul that uses the least amount "
                     "of temporary memory");
    }

    auto objective = PlanningObjective::minimizeTileTempMemory();
    std::tie(plan, cost, costBreakdown) =
        createPlan(objective, target, inputType, params, options);

    if (cost == highestCost) {
      throw poputil::poplibs_error("No plan found for sparse-dense matmul");
    }
  }

  logging::debug("Found best plan: {}.", cost);
  if (logging::shouldLog(logging::Level::Debug)) {
    logging::debug("  Cost breakdown:");
    for (const auto &entry : costBreakdown) {
      logging::debug("    {}: cycles={}, tempBytes={}", entry.first,
                     entry.second.cycles, entry.second.tempBytes);
    }
  }
  logging::debug("  for params:\n{}", params);
  logging::debug("  and input type: {}", inputType);
  logging::debug("  with options:\n{}", options);
  logging::debug("{}", plan);

  return std::make_tuple(std::move(plan), std::move(cost));
}

std::ostream &operator<<(std::ostream &os, const Cost &c) {
  os << "Cost{cycles=" << c.cycles << ", memory=" << c.tempBytes << "}";
  return os;
}

std::ostream &operator<<(std::ostream &os, const OnTileMethod &m) {
  switch (m) {
  case OnTileMethod::Forward:
    os << "Forward";
    break;
  case OnTileMethod::GradA:
    os << "GradA";
    break;
  case OnTileMethod::GradW:
    os << "GradW";
    break;
  case OnTileMethod::Transpose:
    os << "Transpose";
    break;
  default:
    throw poputil::poplibs_error("Unrecognised on-tile method");
  }
  return os;
}

std::ostream &operator<<(std::ostream &os, const Plan &p) {
  os << "Plan:\n  grouping: " << p.grouping << "\n  partition: " << p.partition
     << "\n  initial distribution bucket partition: "
     << p.initialDistributionBucketPartition
     << "\n  used tiles: " << product(p.partition.asStdVector())
     << "\n  mapping order: " << p.mappingOrder
     << "\n  no. of non-zero elements per bucket: " << p.nzElemsPerBucket
     << "\n  no. of meta-info elements per bucket (forward): "
     << p.fwdMetaInfoElemsPerBucket
     << "\n  no. of meta-info elements per bucket (grad-a): "
     << p.gradAMetaInfoElemsPerBucket
     << "\n  forward pass on-tile method: " << p.fwdMethod
     << "\n  grad-a pass on-tile method: " << p.gradAMethod
     << "\n  grad-w pass on-tile method: " << p.gradWMethod << "\n";
  return os;
}

std::tuple<Plan, Cost> getPlan(const Target &target, const Type &inputType,
                               const FullyConnectedParams &params,
                               const OptionFlags &optionFlags,
                               PlanningCache *cache) {
  // TODO: Verify some basic things about the input.
  const auto &options = parseOptionFlags(optionFlags);

  auto cacheImpl = cache ? cache->impl.get() : nullptr;
  PlanningCacheImpl::Key key(params, options);
  if (cacheImpl) {
    auto &plans = cacheImpl->plans;
    auto match = plans.find(key);
    if (match != plans.end()) {
      return match->second;
    }
  }

  auto planAndCost = runPlanner(target, inputType, params, options);
  if (cacheImpl) {
    cacheImpl->plans.emplace(key, planAndCost);
  }
  return planAndCost;
}

} // end namespace fullyconnected
} // end namespace popsparse
