// Copyright (c) 2020 Graphcore Ltd. All rights reserved.
#ifndef poplin_ConvPlanTypes_hpp
#define poplin_ConvPlanTypes_hpp

#include "CanonicalConvParams.hpp"
#include "ConvOptions.hpp"
#include "ConvPlan.hpp"

#include <poplar/Target.hpp>

#include <popsolver/Model.hpp>

#include <vector>

namespace poplin {

// constraint variables that represent how each item is split for a particular
// level in the hierarchy.
struct PartitionVariables {
  // indexed by field dimension.
  std::vector<popsolver::Variable> fieldSplit;
  popsolver::Variable batchSplit;
  Split<popsolver::Variable> outChanSplit;
  // indexed by kernel dimension.
  std::vector<popsolver::Variable> kernelSplit;
  Split<popsolver::Variable> inChanSplit;
  popsolver::Variable convGroupSplit;
  std::vector<unsigned> fieldGrainSize;

  unsigned convGroupGrainSize;
  unsigned inChanGrainSize;
  unsigned outChanGrainSize;
};

// constraint variables that specify the grain sizes of each dimension.
struct ConvSizeVariables {
  // indexed by field dimension.
  std::vector<popsolver::Variable> numFieldGrains;
  popsolver::Variable batchSize;
  // indexed by kernel dimension.
  std::vector<popsolver::Variable> kernelSize;

  popsolver::Variable numConvGroupGrains;
  popsolver::Variable numInChanGrains;
  popsolver::Variable numOutChanGrains;
};

// a description of a (sub-)convolution at a particular level in the hierarchy.
template <typename T> struct ConvSize {
  T convGroupSize;
  T batchSize;
  std::vector<T> fieldSize;
  std::vector<T> kernelSize;
  T inChanSize;
  T outChanSize;
};

template <typename T> struct ExchangeEstimates {
  T inputExchangeCycles;
  T weightExchangeCycles;
  T reduceFirstStageExchangeCycles;
  T reduceRemainingStagesExchangeCycles;
};

template <typename T>
inline bool operator<(const ExchangeEstimates<T> &a,
                      const ExchangeEstimates<T> &b) {
  constexpr static auto helper = poplibs_support::makeStructHelper(
      &ExchangeEstimates<T>::inputExchangeCycles,
      &ExchangeEstimates<T>::weightExchangeCycles,
      &ExchangeEstimates<T>::reduceFirstStageExchangeCycles,
      &ExchangeEstimates<T>::reduceRemainingStagesExchangeCycles);

  return helper.lt(a, b);
}

template <typename T> struct SinglePassEstimates {
  SinglePassEstimates() = default;

  // the four values we support minimizing on.
  T totalTiles;
  T totalCycles;
  T totalTempBytes;
  T totalPerStepCycleDiff;

  // break-down of the above totals
  T rearrangeBeforeSliceCycles;
  T memsetZeroBeforeAddInPlace;
  T dynamicSliceCycles;
  T transformCopyCycles;
  T transformExchangeCycles;

  T inputRearrangeBytesPerTile;
  T weightsRearrangeBytesPerTile;

  T totalExchangeCycles;
  ExchangeEstimates<T> itemisedExchangeCycles;

  T tileLevelTransformCycles;
  T partialCalcCycles;
  T reduceCycles;
  T dynamicUpdateCycles;
  T addInPlaceCycles;
  T castCycles;

  T rearrangeBeforeSliceTempBytes;
  T rearrangeBeforeSliceTempDuringRearrangeBytes;
  T transformTempBytes;
  T tileLevelTransformTempBytes;
  T convTempBytes;
  T reduceTempBytes;
  T addInPlaceTempBytes;
};

using SinglePassCost = SinglePassEstimates<popsolver::DataType>;

template <typename T> struct Estimates {
  Estimates() = default;
  Estimates(const T totalTiles, const T totalCycles, const T totalTempBytes,
            const T totalPerStepCycleDiff)
      : totalTiles(totalTiles), totalCycles(totalCycles),
        totalTempBytes(totalTempBytes),
        totalPerStepCycleDiff(totalPerStepCycleDiff) {}

  // the four values we support minimizing on.
  T totalTiles;
  T totalCycles;
  T totalTempBytes;
  T totalPerStepCycleDiff;

  SinglePassEstimates<T> passEstimates;
  boost::optional<SinglePassEstimates<T>> jointPlanBwdEstimates;
  boost::optional<SinglePassEstimates<T>> jointPlanWuEstimates;
};

using Cost = Estimates<popsolver::DataType>;

static Cost highestCost(popsolver::DataType::max(), popsolver::DataType::max(),
                        popsolver::DataType::max(), popsolver::DataType::max());

inline bool operator==(const Cost &a, const Cost &b) {
  return a.totalTiles == b.totalTiles && a.totalCycles == b.totalCycles &&
         a.totalTempBytes == b.totalTempBytes &&
         a.totalPerStepCycleDiff == b.totalPerStepCycleDiff;
}

inline bool operator!=(const Cost &a, const Cost &b) { return !(a == b); }

inline bool operator<(const Cost &a, const Cost &b) {
  constexpr static auto helper = poplibs_support::makeStructHelper(
      &Cost::totalTiles, &Cost::totalCycles, &Cost::totalTempBytes,
      &Cost::totalPerStepCycleDiff);

  return helper.lt(a, b);
}

// performs a max on the itemised cycle counts only.
inline SinglePassCost maxPerStepCycles(SinglePassCost a,
                                       const SinglePassCost &b) {
  a.rearrangeBeforeSliceCycles =
      std::max(a.rearrangeBeforeSliceCycles, b.rearrangeBeforeSliceCycles);
  a.memsetZeroBeforeAddInPlace =
      std::max(a.memsetZeroBeforeAddInPlace, b.memsetZeroBeforeAddInPlace);
  a.dynamicSliceCycles = std::max(a.dynamicSliceCycles, b.dynamicSliceCycles);
  a.transformCopyCycles =
      std::max(a.transformCopyCycles, b.transformCopyCycles);
  a.transformExchangeCycles =
      std::max(a.transformExchangeCycles, b.transformExchangeCycles);

  // the MINIMIZE_COST_DIFF method currently using the totalExchangeCycles, if
  // that changes we would need to update this too.
  a.totalExchangeCycles =
      std::max(a.totalExchangeCycles, b.totalExchangeCycles);

  a.tileLevelTransformCycles =
      std::max(a.tileLevelTransformCycles, b.tileLevelTransformCycles);
  a.partialCalcCycles = std::max(a.partialCalcCycles, b.partialCalcCycles);
  a.reduceCycles = std::max(a.reduceCycles, b.reduceCycles);
  a.dynamicUpdateCycles =
      std::max(a.dynamicUpdateCycles, b.dynamicUpdateCycles);
  a.addInPlaceCycles = std::max(a.addInPlaceCycles, b.addInPlaceCycles);
  a.castCycles = std::max(a.castCycles, b.castCycles);

  return a;
}

// performs a max on the itemised cycle counts only.
inline Cost maxPerStepCycles(Cost a, const Cost &b) {
  a.passEstimates = maxPerStepCycles(a.passEstimates, b.passEstimates);
  assert(a.jointPlanBwdEstimates.has_value() ==
         b.jointPlanBwdEstimates.has_value());
  assert(a.jointPlanWuEstimates.has_value() ==
         b.jointPlanWuEstimates.has_value());
  if (a.jointPlanBwdEstimates) {
    a.jointPlanBwdEstimates =
        maxPerStepCycles(*a.jointPlanBwdEstimates, *b.jointPlanBwdEstimates);
  }
  if (a.jointPlanWuEstimates) {
    a.jointPlanWuEstimates =
        maxPerStepCycles(*a.jointPlanWuEstimates, *b.jointPlanWuEstimates);
  }
  return a;
}

inline std::ostream &operator<<(std::ostream &os, const Cost &c) {
  os << "Cost{cycles=" << c.totalCycles << ", memory=" << c.totalTempBytes;
  if (c.totalPerStepCycleDiff != popsolver::DataType::max()) {
    os << ", diff=" << c.totalPerStepCycleDiff;
  }
  os << ", tiles=" << c.totalTiles << "}";
  return os;
}

struct ConvDescription {
  CanonicalConvParams params;
  ConvOptions options;
  poplar::Target target;
  boost::optional<Plan> referencePlan;
  boost::optional<Cost> referenceCost;
  bool minimizeForTiles;
  boost::optional<popsolver::DataType> cycleLimit;
  unsigned startTileIdxForVirtualHierarchy;

  ConvDescription(CanonicalConvParams params, ConvOptions options,
                  poplar::Target target, boost::optional<Plan> referencePlan,
                  boost::optional<Cost> referenceCost, bool minimizeForTiles,
                  boost::optional<popsolver::DataType> cycleLimit,
                  unsigned startTileIdxForVirtualHierarchy)
      : params{std::move(params)},
        options({std::move(options)}), target{std::move(target)},
        referencePlan{std::move(referencePlan)}, referenceCost{std::move(
                                                     referenceCost)},
        minimizeForTiles{minimizeForTiles}, cycleLimit{std::move(cycleLimit)},
        startTileIdxForVirtualHierarchy{startTileIdxForVirtualHierarchy} {}

  ConvDescription(const ConvDescription &) = default;
  ConvDescription(ConvDescription &&) = default;

  bool operator<(const ConvDescription &other) const {
    constexpr static auto helper = poplibs_support::makeStructHelper(
        &ConvDescription::target, &ConvDescription::params,
        &ConvDescription::options, &ConvDescription::referenceCost,
        &ConvDescription::referencePlan, &ConvDescription::minimizeForTiles,
        &ConvDescription::cycleLimit,
        &ConvDescription::startTileIdxForVirtualHierarchy);

    return helper.lt(*this, other);
  }
};

} // namespace poplin

#endif // poplin_ConvPlanTypes_hpp
