#include "popops/DynamicSlice.hpp"

#include "poplibs_support/gcd.hpp"
#include "poplibs_support/logging.hpp"
#include "poplibs_support/Algorithm.hpp"
#include "poplibs_support/PlanConstraints.hpp"
#include "poplibs_support/Algorithms.hpp"
#include "poplibs_support/ContiguousRegionsByTile.hpp"
#include "poputil/VertexTemplates.hpp"
#include "poputil/Util.hpp"
#include "poplar/Tensor.hpp"
#include "poplar/Interval.hpp"
#include "poplar/Program.hpp"
#include "poputil/VarStructure.hpp"
#include "poputil/TileMapping.hpp"
#include "poputil/exceptions.hpp"
#include "popops/ElementWise.hpp"
#include "popsolver/Model.hpp"

#include <boost/range/adaptor/reversed.hpp>
#include <cassert>
#include <numeric>
#include <algorithm>

using namespace poplar;
using namespace poplar::program;
using namespace poputil;
using namespace poplibs_support;
using namespace poplibs;

namespace popops {

namespace {

struct SliceOptions {
  SliceOptions() = default;

  PlanConstraints planConstraints;
  // TODO: You can currently only specify whether or not a particular
  // plan will be used for an update or not. This should also
  // be possible for the lookup.
  bool usedForUpdate = true;
};

struct ValidateSlicePlanConstraintsOption {
  void operator()(const boost::property_tree::ptree &t) const {
    if (t.empty() && !t.data().empty()) {
      throw poplar::invalid_option("Plan constraints must be an object");
    }

    for (const auto &child : t) {
      if (child.first != "lookupSplit" &&
          child.first != "slicedDimSplit" &&
          child.first != "unslicedDimSplit" &&
          child.first != "unslicedGrainSize") {
        throw poplibs_error("Unrecognised constraint " + child.first);
      }

      validatePlanConstraintsUnsigned(child.first, child.second);
    }
  }
};

// How to partition work across tiles.
struct Partition {
  // How much to split processing of lookup indices between tiles.
  std::size_t lookupSplit;
  // How much to split the sliced/updated dimension of the
  // tensor to be sliced/updated between tiles.
  std::size_t slicedDimSplit;
  // How much to split the product of dimensions that are not
  // sliced/updated between tiles.
  std::size_t unslicedDimSplit;
  // Grain size for no. of elements in the product of dimensions that
  // are not sliced/updated on each tile.
  std::size_t unslicedGrainSize;
};

} // unnamed namespace

class SlicePlanInternal {
public:
  SlicePlanInternal() : isNull(true) {}
public:
  bool isNull;
  Partition partition;

  // For validation, to identify the restrictions on what this
  // plan can be used to implement,
  std::size_t rank;
  std::vector<std::size_t> slicedDims;
  std::vector<std::size_t> slicedDimSizes;

  std::unique_ptr<SlicePlanInternal> clone() const {
    return std::make_unique<SlicePlanInternal>(*this);
  };
};

std::ostream &operator<<(std::ostream &o, const SlicePlanInternal &p) {
  o << "SlicePlan:\n";
  o << "  Partition:\n";
  o << "    lookupSplit=" << p.partition.lookupSplit << "\n";
  o << "    slicedDimSplit=" << p.partition.slicedDimSplit << "\n";
  o << "    unslicedDimSplit=" << p.partition.unslicedDimSplit << "\n";
  o << "    unslicedGrainSize=" << p.partition.unslicedGrainSize << "\n";
  return o;
}

SlicePlan::SlicePlan() : internal(std::make_unique<SlicePlanInternal>()) {}
SlicePlan::~SlicePlan() = default;
SlicePlan::SlicePlan(const SlicePlan &other) {
  internal = other.internal->clone();
}
SlicePlan::SlicePlan(SlicePlan &&other) = default;
SlicePlan &SlicePlan::operator=(const SlicePlan &other) {
  internal = other.internal->clone();
  return *this;
}
SlicePlan &SlicePlan::operator=(SlicePlan &&other) = default;

SlicePlan::SlicePlan(std::unique_ptr<SlicePlanInternal> internal) :
  internal(std::move(internal)) {}

std::ostream &operator<<(std::ostream &o, const SlicePlan &p) {
  if (!p.internal->isNull) {
    o << *p.internal;
  } else {
    o << "SlicePlan: Introspect\n";
  }
  return o;
}

static SliceOptions parseSliceOptions(const OptionFlags &optionFlags) {
  SliceOptions options;

  using poplibs::OptionHandler;
  using poplibs::OptionSpec;
  using poplibs_support::makePlanConstraintsOptionHandler;

  const auto makeSlicePlanConstraintsOptionHandler =
      &makePlanConstraintsOptionHandler<ValidateSlicePlanConstraintsOption>;

  /*
   * Any changes to spec must be reflected in the documentation comment in
   * the header.
   */
  const OptionSpec spec{
    {"planConstraints",
     makeSlicePlanConstraintsOptionHandler(options.planConstraints)},
    {"usedForUpdate",
     OptionHandler::createWithBool(options.usedForUpdate)}
  };

  for (const auto &entry : optionFlags) {
    spec.parse(entry.first, entry.second);
  }

  return options;
}

/** Create vertices with matching elements in t2d and s2d
 * \param vName     The base name of vertices to create
 * \param graph     The graph to update
 * \param cs        The compute set to update
 * \param offset    The offset within t2d corresponding to the first element in
 *                  s2d. A single element for all tiles, or one element per tile
 * \param t2d       A 2d base tensor
 * \param s2d       A 2d sub tensor
 **/
static void generateVertices(std::string vertexName,
                             Graph &graph,
                             Sequence &prog,
                             const Tensor &offset,
                             Tensor t2d,   // 2d base Tensor [sliceD][]
                             Tensor s2d,   // 2d sub Tensor [sizeD][]
                             const std::string &debugName)
{
  auto cs = graph.addComputeSet(debugName);

  constexpr unsigned slicedDim = 0;
  constexpr unsigned unslicedDim = 1;
  assert(t2d.rank() == 2);
  assert(s2d.rank() == 2);
  assert(t2d.dim(unslicedDim) == s2d.dim(unslicedDim));
  const auto &target = graph.getTarget();
  const auto grainSize = target.getVectorWidth(t2d.elementType());
  const auto numTiles = target.getNumTiles();
  const unsigned numBaseElements = t2d.dim(slicedDim);
  const unsigned numSubElements = s2d.dim(slicedDim);
  assert(numSubElements <= numBaseElements);

  // Offset must be a scalar. It will be replicated over tiles
  // by the small graph  replication optimisation during lowering.
  assert(offset.rank() == 0 && offset.numElements() == 1);
  // Build vertices assuming all sliced dimensions have the same mapping as
  // the first one.
  auto mapping = graph.getTileMapping(t2d[0]);
  auto numVarRegions = t2d[0].getVarRegions().size();
  unsigned numUsedTiles = 0;
  for (const auto &e : mapping) {
    if (e.size() != 0)
      ++numUsedTiles;
  }
  // If there are multiple regions on a tile try reordering to simplify vertex
  // state. Reordering can be expensive when there are many elements so don't
  // reorder if it is unnecessary
  if (numVarRegions > numUsedTiles)
  {
    // Reorder to minimize the number of contiguous regions.
    std::vector<Tensor *> toRearrange;
    std::vector<Tensor> s2dElems(numSubElements), t2dElems(numBaseElements);

    for (unsigned i = 0; i != numSubElements; ++i) {
      s2dElems[i] = s2d[i];
      if (i != 0)
        toRearrange.push_back(&s2dElems[i]);
    }
    for (unsigned i = 0; i != numBaseElements; ++i) {
      t2dElems[i] = t2d[i];
      toRearrange.push_back(&t2dElems[i]);
    }
    graph.reorderToSimplify(&s2dElems[0], toRearrange);

    // Reordering may cause the element size to change if there were repeated
    // elements in s2d.
    unsigned elemSize = s2dElems[0].numElements();
    s2d = concat(s2dElems).reshape({numSubElements, elemSize});
    t2d = concat(t2dElems).reshape({numBaseElements, elemSize});
    mapping = graph.getTileMapping(t2d[0]);
  }

  // instantiate vertices following the mapping of t's first slice
  for (unsigned tile = 0; tile != numTiles; ++tile) {
    const auto tileContiguousRegions =
      graph.getSortedContiguousRegions(t2d[0], mapping[tile]);
    if (tileContiguousRegions.size() == 0)
      // do nothing on this tile
      continue;

    assert(offset.numElements() == 1);
    if (tileContiguousRegions.size() == 1) {
      unsigned regionSize = 0;
      std::vector<Tensor> baseSlices, subSlices; // [slice]
      auto &regions = tileContiguousRegions[0];
      for (const auto &region : regions) {
        regionSize += region.size();
        baseSlices.emplace_back(t2d.transpose().slice(region));
        subSlices.emplace_back(s2d.transpose().slice(region));
      }

      Tensor tileBase = concat(baseSlices).transpose().flatten();
      Tensor tileSub = concat(subSlices).transpose().flatten();

      if (tileBase.isContiguous()) {
        auto v = graph.addVertex(cs,
                                 templateVertex(vertexName + "Supervisor",
                                                t2d.elementType()),
                                 {{"offset", offset},
                                  {"baseT", tileBase},
                                  {"subT", tileSub}
                                 });

        // the assembly relies on underflow of baseIdx with numBaseElements,
        // therefore the maximum value each can be is 2^31 - 1. we can't check
        // baseIdx at compile time but we can the size of numBaseElements at
        // the very least. both are checked at runtime in the C++ codelet.
        assert(numBaseElements < (1 << 31));
        graph.setInitialValue(v["numBaseElements"], numBaseElements);
        graph.setInitialValue(v["numSubElements"], numSubElements);
        graph.setInitialValue(v["regionSize"], regionSize);
        graph.setTileMapping(v, tile);
        continue;
      }
    }

    auto vertexSeqs =
      splitRegionsBetweenWorkers(target, tileContiguousRegions,
                                 grainSize, 2 * grainSize);
    for (const auto &sequences : vertexSeqs) {
      // vector of sequences per vertex
      std::vector<Tensor> base, sub;
      for (const auto &regions : sequences) {
        for (const auto &region : regions) {
          for (unsigned slice = 0; slice != numBaseElements; ++slice) {
            base.emplace_back(t2d[slice].slice(region));
          }
          for (unsigned slice = 0; slice != numSubElements; ++slice) {
            Tensor subRegion = s2d[slice].slice(region);
            sub.emplace_back(std::move(subRegion));
          }
        }
      }
      auto v = graph.addVertex(cs,
                               templateVertex(vertexName + "2d",
                                t2d.elementType()),
                               {{"offset", offset},
                                {"baseT", base},
                                {"subT", sub}
                               });
      graph.setInitialValue(v["numBaseElements"], numBaseElements);
      graph.setInitialValue(v["numSubElements"], numSubElements);
      graph.setInitialValue(v["numRegions"], base.size()/numBaseElements);
      graph.setTileMapping(v, tile);
    }
  } // end loop over tiles

  prog.add(Execute(cs));
}

static void generateMultiSliceVertices(
    const std::string &vertexNameUntemplated,
    bool isUpdate,
    bool isUpdateAdd,
    Graph &graph,
    Sequence &prog,
    const Tensor &offsets,
    Tensor base,
    Tensor slices,
    const Tensor *scale,
    unsigned baseSlicedDim,
    std::string &debugName) {
  auto cs = graph.addComputeSet(debugName);

  // un-/slicedDim are in base, must add one in slices
  constexpr unsigned slicedDim = 0;
  constexpr unsigned unslicedDim = 1; //
  assert(offsets.rank() == 2);
  assert(base.rank() == 2);
  assert(slices.rank() == base.rank() + 1);
  assert(offsets.dim(0) == slices.dim(0));
  // only single-dim slicing supported by these vertices
  assert(offsets.dim(1) == 1);
  if (baseSlicedDim != slicedDim) {
    // This function is coded to slice the innermost dimension. If the outermost
    // is being sliced swap the tensor dimesions.
    base = base.transpose();
    slices = slices.dimRoll(2, 1);
  }
  assert(base.dim(unslicedDim) == slices.dim(1 + unslicedDim));
  assert(isUpdate || scale == nullptr); // no scale on slice

  auto offsets1d = offsets.squeeze({1});
  const auto &target = graph.getTarget();
  const auto numTiles = target.getNumTiles();
  const auto type = base.elementType();
  const unsigned vectorWidth = target.getDataPathWidth() /
                               ((type == HALF) ? 16 : 32);
  const unsigned numBaseElements = base.dim(slicedDim);
#ifndef NDEBUG
  const unsigned numSubElements = slices.dim(1 + slicedDim);
  assert(numSubElements == 1);
#endif

  // Build vertices assuming all sliced dimensions have the same mapping as
  // the first one and the non-sliced dimension is contiguous. If this is
  // not honoured gathering internal exchange/copies will be generated
  auto baseSlice0 = base.slice(0, 1, slicedDim);
  auto mapping = graph.getTileMapping(baseSlice0);
  auto atomsPerWord = graph.getTarget().getAtomicStoreGranularity() /
                      graph.getTarget().getTypeSize(type);

  // instantiate vertices following the mapping of t's first slice
  std::vector<unsigned> multiUpdateSubwordTiles;
  for (unsigned tile = 0; tile != numTiles; ++tile) {
    const auto tileContiguousRegions =
      graph.getSortedContiguousRegions(baseSlice0, mapping[tile]);
    if (tileContiguousRegions.size() == 0) {
      // do nothing on this tile
      continue;
    }

    // separate vertices for each
    unsigned regionSize = 0;
    std::vector<Tensor> baseSlices, subSlices;
    for (const auto &tcr : tileContiguousRegions) {
      for (const auto &region : tcr) {
        regionSize += region.size();
        baseSlices.emplace_back(base.transpose().slice(region));
        subSlices.emplace_back(slices.dimRoll(2, 1).slice(region, 1));
      }
    }
    // When tcr.size() == 1 and the tensors are correctly layed out no gather
    // will be required for these edges
    // If multiple elements of the slice are on the same tile numBaseElements
    // and regionSize will differ

    Tensor tileBase = concat(baseSlices, slicedDim).transpose();
    Tensor tileSub = concat(subSlices, 1 + slicedDim).dimRoll(2, 1);

    std::string vertexName;
    if (isUpdateAdd) {
      bool padTo32Bits = false;//TODO: control this via a plan field
      if (!padTo32Bits)  {
        // We have different specialisations for half data depending on the need
        // for subword writes
        bool needSubwordWrites = target.getTypeSize(type) == 2
                                 && regionSize % 2 != 0;

        if (needSubwordWrites)
          multiUpdateSubwordTiles.emplace_back(tile);
        vertexName = templateVertex(vertexNameUntemplated,
                                    base.elementType(),
                                    needSubwordWrites);
       } else {
         // for halves we process 32-bit at a time and therefore pad the tensors
         // in the case where region size is odd.
         if (target.getTypeSize(type) == 2 && regionSize % 2 != 0) {
           const auto padWithSelf = [&](const StringRef name, const Tensor &t) {
             logging::debug("Padding {} in {} to avoid sub-word writes.", name,
                            debugName);

             // as we want to pad the last dimension, we might as well do that
             // with ourselves. so slice that dimension out, clone it (to avoid
             // aliasing) and then interleave it back with the original.
             const auto lastDim = t.rank() - 1;
             const auto first = t.slice(0, 1, lastDim);
             const auto firstCloned =
               graph.clone(first, debugName + "/padding");

             // TODO: a WriteUndef may be needed here (see T11457). as this code
             // is just to handle odd grain sizes and should never come up in
             // practice this is left out for now.
             prog.add(Copy(first, firstCloned));
             return concat({t, firstCloned}, lastDim);
           };

         tileBase = padWithSelf("baseT", tileBase);
         tileSub = padWithSelf("subT", tileSub);
         ++regionSize;
        }
      }
    } else {
      vertexName = templateVertex(vertexNameUntemplated, base.elementType());
    }

    auto numParallelWorkers = isUpdate ? 1 : target.getNumWorkerContexts();

    auto copiesPerOffset = (regionSize + vectorWidth - 1) / vectorWidth;
    // min 4 copies per thread to avoid excessive vertex state
    auto offsetsPerThread =
        std::max((offsets1d.numElements() + numParallelWorkers - 1
                 ) / numParallelWorkers,
        4ul / copiesPerOffset);
    // ensure that words are not split between workers
    if (atomsPerWord) {
      if (auto numSubwordElements = offsetsPerThread % atomsPerWord) {
        offsetsPerThread += atomsPerWord - numSubwordElements;
      }
    }

    offsetsPerThread = std::min(offsetsPerThread,
                                graph.getMaxFieldDim(vertexName, "offsets", 0));
    for (unsigned o = 0; o != offsets1d.numElements();) {
      auto firstOffset = o;
      o = std::min(o + offsetsPerThread, offsets1d.numElements());
      Tensor workerOffsets = offsets1d.slice({firstOffset, o});
      Tensor workerSlices = tileSub.slice({firstOffset, o});
      auto v = graph.addVertex(cs,
                               vertexName,
                               {{"offsets", workerOffsets},
                                {"baseT", tileBase.flatten()},
                                {"subT", workerSlices.flatten()}
                               });
      if (scale != nullptr)
        graph.connect(v["scale"], *scale);

      // TODO: as a part of T10844 and T10845 we will add support for slicing /
      // updating only part of an offset. This field will eventually be set to
      // correspond to which part that is.
      graph.setInitialValue(v["baseOffset"], 0u);
      graph.setInitialValue(v["numBaseElements"], numBaseElements);
      graph.setInitialValue(v["regionSize"], regionSize);
      graph.setTileMapping(v, tile);
    }
  }
  if (!multiUpdateSubwordTiles.empty())
    logging::debug("UpdateAdd in {} with odd regionSize on tile(s) {}",
                   debugName, multiUpdateSubwordTiles);


  prog.add(Execute(cs));
}

/** Return the sub-tensor acquired by indexing 't' at position 'offset' in
 * dimension 'dim'. The other output dimensions will match the size of the
 * corresponding input dimensions.
 *
 * \param graph           The poplar graph
 * \param t               The source tensor
 * \param offset          The offset in \a's \a dim dimension. This tensor must
 *                        have a single element, or an element per tile
 * \param dim             The dimension to slice
 * \param numOutIndices   The size of the output Tensor in the sliced dimension
 * \param prog            Pointer to program to be updated. If the program
 *                        pointer is nullptr, vertices are not generated
 * \param debugPrefix     The prefix prepended to debugging info
 * \returns               The specified subtensor
 */
static Tensor slice(Graph &graph,
                    const Tensor &t,
                    const Tensor &offset,
                    unsigned dim,
                    unsigned numOutIndices,
                    poplar::program::Sequence *prog,
                    const std::string &debugPrefix)
{
  const unsigned numInIndices = t.dim(dim);
  assert(dim < t.rank());
  assert(numOutIndices <= t.dim(dim));
  // Get a 2d view of the source tensor, with the dim we're slicing at dim0
  // and the other dimensions collapsed into dim1
  Tensor t2d = t.dimRoll(dim).reshape({numInIndices,
                                       t.numElements() / numInIndices});
  Tensor s = graph.clone(t.slice(0, numOutIndices, dim),
                         debugPrefix + "/sliced_" + std::to_string(dim));

  rebalanceTensor(graph, s);
  if (prog != nullptr) {
    Tensor s2d = s.dimRoll(dim).reshape({numOutIndices,
                                         s.numElements() / numOutIndices});

    generateVertices("popops::DynamicSlice", graph, *prog, offset, t2d, s2d,
                     debugPrefix + "/slice");
  }
  return s;
}

/** Update the sub-tensor at 'offset; within \a t's dimension 'dim' with the
 *  contents of 's'
 *
 *  \param graph        The poplar graph
 *  \param t            The base tensor
 *  \param s            The subtensor to insert. Its dimensions must match t's,
 *                      except in dimension \a dim
 *  \param offset       The offset in \a t's \a dim dimension. This tensor must
 *                      have either a single element, or an element per tile
 *  \param dim          The dimension in which to insert
 *  \param prog         The program to be updated
 *  \param debugPrefix  The prefix prepended to debugging info
 **/
static void update(Graph &graph,
                   const Tensor &t,
                   const Tensor &s,
                   const Tensor &offset,
                   unsigned dim,
                   poplar::program::Sequence &prog,
                   const std::string &debugPrefix)
{
  const unsigned numTElements = t.dim(dim);
  const unsigned numSElements = s.dim(dim);
  assert(t.rank() == s.rank());
  for (unsigned d = 0; d != t.rank(); ++d) {
    if (d != dim)
      assert (s.dim(d) == t.dim(d));
    else
      assert (s.dim(d) <= t.dim(d));
  }
  assert(dim < t.rank());
  assert(numSElements <= numTElements);
  // Get a 2d view of the source tensor, with the dim we're updating at dim0
  // and the other dimensions collapsed into dim1
  Tensor t2d = t.dimRoll(dim).reshape({numTElements,
                                       t.numElements() / numTElements});
  Tensor s2d = s.dimRoll(dim).reshape({numSElements,
                                       s.numElements() / numSElements});

  generateVertices("popops::DynamicUpdateSlice",
                   graph, prog, offset, t2d, s2d, debugPrefix + "/update");
}

// If we are slicing up a tensor with the given `shape` in the dimensions
// `dims`, and the slice size in each dimension is `sizes`, then what is
// the best order to do the slices? The returned vector contains
// indexes into `dims` (and `sizes`).
static std::vector<size_t> bestSliceOrder(const std::vector<std::size_t> &shape,
                                          const std::vector<std::size_t> &dims,
                                          const std::vector<std::size_t> &sizes)
{

  assert(dims.size() == sizes.size());
  assert(dims.size() <= shape.size());

  // Process the dimensions in an order that slices out the most elements
  // first. That dimension is the one that reduces the size of the tensor
  // to the lowest percentage of its former size. Since each slice only
  // reduces the tensor's size in one dimension, that percentage is equal to
  //
  //    sizes[a] / shape[dims[a]]
  //
  // so if we sort on  (sizes[a] / shape[dims[a]] < sizes[b] / shape[dims[b]])
  // then we should end up slicing in an optimal order.

  // Initialise with default order (0, 1, 2...)
  std::vector<size_t> idxOrder(dims.size());
  std::iota(idxOrder.begin(), idxOrder.end(), 0);

  // Sort the most slicey dimension first. Assumes no integer overflows.
  std::sort(idxOrder.begin(), idxOrder.end(),
            [&](size_t a, size_t b) {
              return sizes[b] * shape[dims[a]] > sizes[a] * shape[dims[b]];
            });

  return idxOrder;
}

static void ValidateParams(std::string name,
                           const std::vector<std::size_t> &shape,
                           const Tensor &offset,
                           const std::vector<std::size_t> &dims,
                           const std::vector<std::size_t> &sizesOrSlices,
                           bool checkOffset = true,
                           bool checkSizes = true,
                           bool sizesAreSlices = false
                           ) {
  auto tRank = shape.size();
  std::string exceptionStr;
  std::string sizesStr = sizesAreSlices ? "numSlices" : "sizes";
  if (checkOffset) {
    auto offsetElems = offset.rank() == 0 ? 0 : offset.dim(0);
   if  (offset.rank() > 2 || offsetElems != dims.size())
     exceptionStr = name + " offset (" + std::to_string(offsetElems) + ") ";
  }
  if (checkSizes && dims.size() != sizesOrSlices.size()) {
    exceptionStr +=  "dims (" + std::to_string(dims.size()) +
                      ") and " + sizesStr + " (" +
                      std::to_string(sizesOrSlices.size()) + ") ";
  }
  if (!exceptionStr.empty()) {
    exceptionStr +=  ": must be the same size";
    throw graph_connection_error(exceptionStr);
  }
  std::vector<bool> dimUsed(tRank);
  for (unsigned i = 0; i != dims.size(); ++i) {
    if (dims[i] >= tRank)
      throw graph_connection_error(
        name + ": invalid dimension " + std::to_string(dims[i]));
    if (checkSizes && !sizesAreSlices && sizesOrSlices[i] > shape[dims[i]])
      throw graph_connection_error(
        name + ": requested slice dimension bigger than buffer");
    if (dimUsed[dims[i]])
      throw graph_connection_error(
        name + ": dimension " + std::to_string(dims[i])
        + " specified multiple times");
    dimUsed[dims[i]] = true;
  }
}

// Create and map a tensor so that dynamic slicing of it will not require
// exchange.
// The underlying variables will be [U/N][S0]..[Sn][N] where
// N is the number of contiguous unsliced elements per tile
// U is the product of the unsliced dimensions
// This distibutes the input/output slice across U/N tiles.
// S0-Sn are the sliced dimensions, sorted to optimise the number of copies
// Typically two variables are used; the second variable for the final
// tile, which may have a different N.
// If U/N << numTiles an outer stage can be added to convert part of an
// S dimension to an extra U dimensions
static Tensor
createSliceableTensorGivenOrder(poplar::Graph &graph,
                                const poplar::Type &type,
                                const std::vector<std::size_t> &shape,
                                const std::vector<std::size_t> &dims,
                                const std::vector<std::size_t> &idxOrder,
                                std::size_t minGrainSize,
                                const std::string &debugPrefix)
{
  ValidateParams("createSliceableTensor", shape, {}, dims, {}, false, false);
  bool noOutputElements = std::any_of(shape.begin(), shape.end(),
                                      [](std::size_t n) { return n == 0; });
  if (dims.size() == 0 || noOutputElements) {
    // no slicing specified
    auto t = graph.addVariable(type, shape);
    mapTensorLinearly(graph, t);
    return t;
  }

  std::vector<bool> dimIsSliced(shape.size(), false);
  std::vector<unsigned> inversePermutation(shape.size());
  std::vector<std::size_t> createShape;
  createShape.reserve(dims.size() + 1);
  for (const auto i : idxOrder) {
    const auto d = dims[i];
    if (d >= shape.size()) {
      throw poputil::poplibs_error(
          "createSliceableTensor called to slice dimension " +
          std::to_string(d) + " but the target has rank " +
          std::to_string(shape.size()));
    }
    if (dimIsSliced[d]) {
      throw poputil::poplibs_error(
          "createSliceableTensor called with repeated dims entry");
    }
    dimIsSliced[d] = true;
    inversePermutation[d] = createShape.size();
    createShape.push_back(shape[dims[i]]);
  }
  std::size_t numUnslicedElems = 1;
  std::vector<std::size_t> unslicedShape;
  unslicedShape.reserve(shape.size() - dims.size());
  for (std::size_t d = 0; d < shape.size(); ++d) {
    if (!dimIsSliced[d]) {
      inversePermutation[d] = createShape.size() + unslicedShape.size();
      unslicedShape.push_back(shape[d]);
      numUnslicedElems *= shape[d];
    }
  }
  createShape.push_back(numUnslicedElems);

  // Calculate how we should divide the unsliced dimension.
  //
  // T10013 - We don't necessarily have to map this to minimize the
  // number of tiles used - i.e. we could have multiple tiles with
  // fewer than unslicedElemsPerSplit elements mapped to them.
  const auto numTiles = graph.getTarget().getNumTiles();
  const auto unslicedElemsPerSplit =
    std::max(ceildiv(numUnslicedElems, numTiles), minGrainSize);
  const auto unslicedSplit = ceildiv(numUnslicedElems, unslicedElemsPerSplit);
  std::vector<std::size_t> dimSplits(createShape.size(), 1);
  dimSplits.back() = unslicedSplit;

  auto t = createPartitionableTensor(graph,
                                     type,
                                     createShape,
                                     dimSplits,
                                     debugPrefix + "/sliceable");

  // Distribute over tiles starting from 0.
  unsigned tile = 0;
  iterateTensorPartitions(t, dimSplits,
    [&](const std::vector<std::size_t> &,
        const Tensor &s) {
      graph.setTileMapping(s, tile++);
    });

  t = t.reshapePartial(t.rank() - 1, t.rank(), unslicedShape)
       .dimShuffle(inversePermutation);

  logging::debug(
      "createSliceableTensor {}, minGrainSize {}, dims {}, "
      "used tiles {}, "
      "{} tiles with {} elems, "
      "{} tiles with {} elems",
      t.shape(), minGrainSize, dims,
      unslicedSplit,
      // Tiles with ceildiv(numElems, numSplits) elements
      numUnslicedElems / unslicedElemsPerSplit,
      unslicedElemsPerSplit,
      // Any remainder
      numUnslicedElems % unslicedElemsPerSplit ? 1 : 0,
      numUnslicedElems % unslicedElemsPerSplit);
  return t;
}

// Create and map a tensor so that dynamic slicing of it will not require
// exchange
// The underlying layout will be [U/N][S0]..[Sn][N] where
// N is the number of contiguous unsliced elements per tile
// U is the product of the unsliced dimensions
// S0-Sn are the sliced dimensions, sorted to optimise the number of copies
// This distibutes the input/output slice across U/N tiles.
// If U/N << numTiles an outer stage can be added to convert part of an
// S dimension to an extra U dimensions
Tensor
createSliceableTensor(Graph &graph,
                      const Type &type,
                      const std::vector<std::size_t> &shape,
                      const std::vector<std::size_t> &dims,
                      const std::vector<std::size_t> &sizes,
                      std::size_t minGrainSize,
                      const std::string &debugPrefix)
{
  auto idxOrder = bestSliceOrder(shape, dims, sizes);
  return createSliceableTensorGivenOrder(graph, type, shape, dims, idxOrder,
                                         minGrainSize, debugPrefix);
}

Tensor
createSliceableTensor(Graph &graph,
                      const Type &type,
                      const std::vector<std::size_t> &shape,
                      const std::vector<std::size_t> &dims,
                      const std::vector<std::size_t> &sizes,
                      const SlicePlan &plan,
                      const OptionFlags &options,
                      const std::string &debugPrefix) {
  return createSliceableTensor(graph, type, shape, dims, sizes, 0,
                               debugPrefix);
}

static Tensor
createSliceTensor(Graph &graph,
                  const poplar::Type &type,
                  const std::vector<std::size_t> &inputShape,
                  const std::vector<std::size_t> &dims,
                  const std::vector<std::size_t> &sizes,
                  const std::size_t numUpdates,
                  const std::string &debugPrefix) {
  ValidateParams("createSliceTensor", inputShape, {}, dims, sizes, false);

  auto uShape = inputShape;
  // update/slicing order is based on the tensor shape before any update is
  // performed. full-sized dimensions do not affect the order.
  auto idxOrder = bestSliceOrder(uShape, dims, sizes);

  // shrink the dimensions to the size of the update
  for (unsigned i = 0; i != dims.size(); ++i) {
    uShape[dims[i]] = sizes[i];
  }
  // The update tensor has an an outer dimension of the number of slices to be
  // updated, with the remaining dimensions taken from t reduced to the sliced
  // size
  uShape.insert(uShape.begin(), numUpdates);
  // uDims holds dims shifted due to the new outer numUpdates dimension
  std::vector<std::size_t> uDims(dims.size() + 1);
  std::vector<std::size_t> uIdxOrder(idxOrder.size() + 1);
  uDims[0] = 0;
  for (unsigned i = 0; i != dims.size(); ++i)
    uDims[1 + i] = 1 + dims[i];
  // adjust uIdxOrder for the new outer numUpdates dimension
  for (unsigned i = 0; i != idxOrder.size(); ++i)
    uIdxOrder[i] = 1 + idxOrder[i];
  uIdxOrder[idxOrder.size()] = 0;

  // For an update tensor only the outermost dimension is "sliceable"
  return createSliceableTensorGivenOrder(graph, type, uShape, uDims,
                                         uIdxOrder, 0, debugPrefix);
}

Tensor
createSliceTensor(Graph &graph,
                  const Type &type,
                  const std::vector<std::size_t> &shape,
                  const std::vector<std::size_t> &dims,
                  const std::vector<std::size_t> &sizes,
                  const std::size_t numIndices,
                  const SlicePlan &plan,
                  const OptionFlags &options,
                  const std::string &debugPrefix) {
  return createSliceTensor(graph, type, shape, dims, sizes, numIndices,
                           debugPrefix);
}

Tensor
createSliceTensor(Graph &graph,
                   const Tensor &t,
                   const std::vector<std::size_t> &dims,
                   const std::vector<std::size_t> &sizes,
                   const std::size_t numIndices,
                   const std::string &debugPrefix) {
  // Special case for 1 index, we just clone the input tensor's first slice.
  if (numIndices == 1) {
    std::string name = debugPrefix + "/slice";
    Tensor s = t;
    // When updating a single slice map the update tensor with the mapping
    // of the first slice of the base tensor
    for (unsigned i = 0; i != dims.size(); ++i) {
      s = s.slice(0, sizes[i], dims[i]);
      name = name + "_d" + std::to_string(dims[i]);
    }
    auto mapping = graph.getTileMapping(s);
    s = graph.clone(s, name);
    graph.setTileMapping(s, mapping);
    return s.expand({0});
  }
  return createSliceTensor(graph, t.elementType(), t.shape(), dims, sizes,
                           numIndices, debugPrefix);
}

poplar::Tensor
createIndicesTensor(Graph &graph,
                    const std::vector<std::size_t> &dims,
                    const std::size_t numIndices,
                    const SlicePlan & /* plan */,
                    const OptionFlags & /* options */,
                    const std::string &debugPrefix) {
  // If plan is 'null' plan/nullptr, i.e. specifies nothing, we fall back
  // to original implementation.
  const auto indices =
    graph.addVariable(UNSIGNED_INT, {numIndices, dims.size()}, debugPrefix);
  mapTensorLinearly(graph, indices);
  return indices;
}

template <typename T>
std::vector<std::vector<T>> flattenInnermostRegions(
  const std::vector<std::vector<std::vector<T>>> &regions) {

  std::vector<std::vector<T>> result(regions.size());
  for (std::size_t i = 0; i < regions.size(); ++i) {
    result[i] = regions[i][0];
    for (std::size_t j = 1; j < regions[i].size(); ++j) {
      std::copy(regions[i][j].begin(), regions[i][j].end(),
                std::back_inserter(result[i]));
    }
  }
  return result;
}

Tensor
createSliceableTensorFromSlice(Graph &graph,
                               const Tensor &s,
                               const std::vector<std::size_t> &dims,
                               const std::vector<std::size_t> &numSlices,
                               const std::string &debugPrefix) {

  ValidateParams("createSliceableTensorFromSlice",
                 s.shape(), {}, dims, numSlices, false, true, true);
  std::vector<std::size_t> sizes(dims.size());
  for (std::size_t i = 0; i < dims.size(); ++i) {
    sizes[i] = s.dim(dims[i]);
  }

  // The final shape of the returned sliceable tensor.
  auto sliceableShape = s.shape();
  for (unsigned i = 0; i < dims.size(); ++i) {
    sliceableShape[dims[i]] *= numSlices[i];
  }

  const auto idxOrder = bestSliceOrder(sliceableShape, dims, sizes);

  // Create a variable with sliced dimensions factored out
  // as the outermost dimensions.
  auto createShape = s.shape();
  for (const auto idx : boost::adaptors::reverse(idxOrder)) {
    createShape.insert(createShape.begin(), numSlices[idx]);
  }

  auto t =
    graph.addVariable(s.elementType(), createShape, debugPrefix).flatten();

  const auto totalNumSlices =
    std::accumulate(numSlices.begin(), numSlices.end(), std::size_t(1),
                    std::multiplies<std::size_t>());
  // We build up the memory regions of the sliceable tensor
  // based on the given slice such that each slice/update operation
  // operates on contiguous memory and produces contiguous memory.
  const auto sBroadcast = s.expand({0}).broadcast(totalNumSlices, 0);
  const auto mapping = graph.getTileMapping(sBroadcast);
  const auto contiguousRegionsByTile =
    getSortedContiguousRegionsByTile(graph, sBroadcast, mapping);

  std::size_t offset = 0;
  for (unsigned tile = 0; tile < contiguousRegionsByTile.size(); ++tile) {
    const auto numElems =
      intervalSequenceNumElements(contiguousRegionsByTile[tile]);
    graph.setTileMapping(t.slice(offset, offset + numElems), tile);
    offset += numElems;
  }

  const auto mappingOrderedContiguously =
    flattenInnermostRegions(contiguousRegionsByTile);
  const auto inverseMapping = getInverseMapping(mappingOrderedContiguously);

  std::vector<Tensor> toConcat;
  toConcat.reserve(inverseMapping.size());
  for (const auto &i : inverseMapping) {
    toConcat.push_back(t.slice(i.begin(), i.end()));
  }

  t = concat(toConcat).reshape(createShape);
  auto referenceMapping = graph.getTileMapping(s);

  for (std::size_t i = 0; i < dims.size(); ++i) {
    const auto dim = dims.size() - i + dims[idxOrder[i]];
    t = t.dimRoll(0, dim - 1).flatten(dim - 1, dim + 1);
  }
  assert(t.shape() == sliceableShape);

  return t;
}

static
Tensor dynamicSlice(Graph &graph,
                    const Tensor &t,
                    const Tensor &offset,
                    const std::vector<std::size_t> &dims,
                    const std::vector<std::size_t> &sizes,
                    poplar::program::Sequence *prog,
                    const std::string &debugPrefix)
{
  bool checkOffset = prog != nullptr;
  ValidateParams("dynamicSlice", t.shape(), offset, dims, sizes, checkOffset);
  for (unsigned i = 0; i != dims.size(); ++i) {
    if (sizes[i] == 0) {
      // Since one of the slice sizes is zero, the resulting tensor has no
      // elements. We can return a static slice of the original tensor
      // of the correct size. The offset for each slice can be 0 because
      // it won't have any elements anyway. Tensorflow tests for 0-sized slices.
      Tensor emptyT = t;
      for (unsigned d = 0; d < dims.size(); ++d)
        emptyT = emptyT.slice(0, sizes[d], dims[d]);
      return emptyT;
    }
  }
  Tensor out = t;

  auto idxOrder = bestSliceOrder(t.shape(), dims, sizes);

  for (auto i : idxOrder) {
    // don't care about offset if vertices are not mapped as we are only
    // interested in the mapping
    out = slice(graph, out,
                checkOffset ? offset[i] : offset,
                dims[i],
                sizes[i],
                prog,
                debugPrefix + "/dynamicSlice_d" + std::to_string(dims[i]));
  }

  return out;
}

Tensor dynamicSlice(Graph &graph,
                    const Tensor &t,
                    const Tensor &offset,
                    const std::vector<std::size_t> &dims,
                    const std::vector<std::size_t> &sizes,
                    poplar::program::Sequence &prog,
                    const std::string &debugPrefix) {
  return
    dynamicSlice(graph, t, offset, dims, sizes, &prog, debugPrefix);
}

Graph::TileToTensorMapping
getSliceMapping(poplar::Graph &graph,
                const poplar::Tensor &t,
                const std::vector<std::size_t> &dims,
                const std::vector<std::size_t> &sizes) {
  // give a dummy offset tensor as it is not used
  Tensor offset;
  auto sliceT =
    dynamicSlice(graph, t, offset, dims, sizes, nullptr, "");
  return graph.getTileMapping(sliceT);
}

void dynamicUpdate(Graph &graph,
                   const Tensor &t,
                   const Tensor &s,
                   const Tensor &offset,
                   const std::vector<std::size_t> &dims,
                   const std::vector<std::size_t> &sizes,
                   poplar::program::Sequence &prog,
                   const std::string &debugPrefix)
{
  ValidateParams("dynamicUpdate", t.shape(), offset, dims, sizes);
  // empty sizes or dimensions are full update (TF does this)
  if (dims.size() == 0) {
    prog.add(Copy(s, t));
    return;
  }
  // If any of sizes is 0 then this is a nop. Tensorflow tests do this.
  for (auto& sz : sizes)
    if (sz == 0)
      return;

  // We insert into a single dimension at a time. When more than one dimension
  // is to be inserted this entails slicing off the outer dimensions until there
  // is a single dynamic dimension. That Tensor is updated with s. Then
  // the dimension traversal is reversed, updating one into one extra dimension
  // each time.

  auto idxOrder = bestSliceOrder(t.shape(), dims, sizes);

  std::vector<Tensor> reducedT;
  reducedT.emplace_back(t); // reducedT[0] = t
  // slice off the larger dimensions one at a time
  for (unsigned i = 0; i != idxOrder.size() - 1; ++i) {
    auto dim = idxOrder[i];
    reducedT.emplace_back(slice(graph, reducedT[i],
                                offset[dim],
                                dims[dim],
                                sizes[dim],
                                &prog,
                                debugPrefix + "/dynamicUpdateS_d" +
                                std::to_string(dims[i])));
  }
  // copy s into the reduced t, iterating back to full dimensions
  reducedT.emplace_back(s);
  for (unsigned ii = idxOrder.size(); ii != 0; --ii) {
    auto i = ii - 1;
    auto dsIdx = idxOrder[i]; // index into dims[] and sizes[]
    update(graph, reducedT[i], reducedT[i + 1], offset[dsIdx],
           dims[dsIdx], prog,
           debugPrefix + "/dynamicUpdateU_d" + std::to_string(dims[dsIdx]));
  }
}

// create a sequence that runs \a loopProgram the number of times stored in
// \a i. \a i is incremented after each call
static poplar::program::Sequence
countedLoop(poplar::Graph &graph,
            unsigned count,
            poplar::Tensor &i,
            poplar::program::Program &loopProgram,
            const std::string &debugPrefix) {
  poplar::program::Sequence result;
  auto one =
      graph.addConstant(poplar::UNSIGNED_INT, {}, 1, debugPrefix + "/const_1");
  graph.setTileMapping(one, 0);

  poplar::program::Sequence loopProgramInc;
  loopProgramInc.add(loopProgram);
  addInPlace(graph, i.reshape({}), one, loopProgramInc,
             debugPrefix + "/i/increment");

  result.add(poplar::program::Repeat(count, loopProgramInc));

  return result;
}

Tensor multiSlice(Graph &graph,
                  const Tensor &t,
                  const Tensor &offset,
                  const std::vector<std::size_t> &dims,
                  const std::vector<std::size_t> &sizes,
                  Sequence &prog,
                  const SlicePlan & /* plan */,
                  const OptionFlags & /* options */,
                  const std::string &debugPrefix) {
  // small number of slices are instantiated individually
  // large number of slices are sliced by a specialisation or in a loop
  std::string dName = debugPrefix + "/multiSlice";
  // Check the offsets have been specified with a multi-slice dimension
  if (offset.rank() != 2)
    throw poputil::poplibs_error(
        "multiSlice expects offset.rank() == 2 but it is" +
        std::to_string(offset.rank()));
  if (offset.dim(1) != dims.size())
    throw poputil::poplibs_error(
        "multiSlice expects offset.dim(1) == dims.size(); offset.dim(1)==" +
        std::to_string(offset.dim(1)) + ", dims.size()== " +
        std::to_string(dims.size()));
  ValidateParams("multiSlice", t.shape(), offset[0], dims, sizes);
  // We always map the output in the same way to avoid surprising changes when
  // the number of slices changes
  auto sMulti = createSliceTensor(graph, t, dims, sizes, offset.dim(0),
                                  dName);
  poplibs_support::logging::info(
      "multiSlice {} -> {}, name={}",
      t.shape(), sMulti.shape(), debugPrefix);
  // When there are only a few slices the looping code can be larger than
  // instantiating multiple vertices
  constexpr unsigned inliningThreshold = 3;
  if (offset.dim(0) <= inliningThreshold) {
    for (unsigned slice = 0; slice != offset.dim(0); ++slice) {
      auto s = dynamicSlice(graph, t, offset[slice], dims, sizes, prog,
                            dName + "/" + std::to_string(slice));
      prog.add(Copy(s, sMulti[slice]));
    }
    return sMulti;
  }
  // When there are many offsets of single slices there is a fast vertex.
  // For now only 1d slices of 2d base tensors are supported.
  if (t.rank() == 2 && dims.size() == 1 &&
      sMulti.rank() == 3 &&
      offset.rank() == 2 && offset.dim(1) == 1 && offset.dim(0) > 6) {
    generateMultiSliceVertices("popops::MultiSlice", false, false, graph, prog,
                               offset, t, sMulti, nullptr, dims[0], dName);
    return sMulti;
  }

  // looping case
  Sequence body;
  auto sIdx = graph.addVariable(UNSIGNED_INT, {1}, dName + "/sIdx");
  auto zero = graph.addConstant(UNSIGNED_INT, {1}, 0, dName + "/zero");
  graph.setTileMapping(sIdx, 0);
  graph.setTileMapping(zero, 0);
  prog.add(Copy(zero, sIdx));
  auto tIdx = dynamicSlice(graph, offset, sIdx, {0}, {1},
                           body, dName + "/sliceIndex").squeeze({0});

  auto sI = dynamicSlice(graph, t, tIdx, dims, sizes, body,
                         dName + "/slice").expand({0});
  dynamicUpdate(graph, sMulti, sI, sIdx, {0}, {1}, body, dName + "/update");
  prog.add(countedLoop(graph, offset.dim(0), sIdx, body, dName + "/loop"));
  return sMulti;
}

// This is derived from multiSlice with \a s input rather than generated,
// the tensors swapped, etc
void multiUpdate(Graph &graph,
                  const Tensor &t,
                  const Tensor &sMulti,
                  const Tensor &offset,
                  const std::vector<std::size_t> &dims,
                  const std::vector<std::size_t> &sizes,
                  Sequence &prog,
                  const SlicePlan & /* plan */,
                  const OptionFlags & /* options */,
                  const std::string &debugPrefix) {
  poplibs_support::logging::info(
      "multiUpdate {} into {}, name={}",
      sMulti.shape(), t.shape(), debugPrefix);
  // small number of slices are updated individually
  // large number of slices are updated by a specialisation or in a loop
  std::string dName = debugPrefix + "/multiUpdate";
  // Check the offsets have been specified with a multi-slice dimension
  if (offset.rank() != 2)
    throw poputil::poplibs_error(
        "multiUpdate expects offset.rank() == 2 but it is" +
        std::to_string(offset.rank()));
  if (offset.dim(1) != dims.size())
    throw poputil::poplibs_error(
        "multiUpdate expects offset.dim(1) == dims.size(); offset.dim(1)==" +
        std::to_string(offset.dim(1)) + ", dims.size()== " +
        std::to_string(dims.size()));
  ValidateParams("multiUpdate", t.shape(), offset[0], dims, sizes);
  // When there are only a few slices the looping code can be larger than
  // instantiating multiple vertices
  constexpr unsigned inliningThreshold = 3;
  if (offset.dim(0) <= inliningThreshold) {
    for (unsigned slice = 0; slice != offset.dim(0); ++slice) {
      dynamicUpdate(graph, t, sMulti[slice], offset[slice], dims, sizes, prog,
                    dName + "/" + std::to_string(slice));
    }
    return;
  }
  // When there are many offsets of single slices there is a fast vertex.
  // For now only 1d slices of 2d base tensors are supported.
  if (t.rank() == 2 && dims.size() == 1 &&
      sMulti.rank() == 3 &&
      offset.rank() == 2 && offset.dim(1) == 1 && offset.dim(0) > 6) {
    generateMultiSliceVertices("popops::MultiUpdate", true, false, graph, prog,
                               offset, t, sMulti, nullptr, dims[0], dName);
    return;
  }
  // looping case
  Sequence body;
  auto sIdx = graph.addVariable(UNSIGNED_INT, {1}, dName + "/sIdx");
  auto zero = graph.addConstant(UNSIGNED_INT, {1}, 0, dName + "/zero");
  graph.setTileMapping(sIdx, 0);
  graph.setTileMapping(zero, 0);
  prog.add(Copy(zero, sIdx));
  auto tIdx = dynamicSlice(graph, offset, sIdx, {0}, {1},
                           body, dName + "/sliceIndex").squeeze({0});

  auto sI = dynamicSlice(graph, sMulti, sIdx, dims, sizes, body,
                         dName + "/slice").squeeze({0});
  dynamicUpdate(graph, t, sI, tIdx, {0}, {1}, body, dName + "/update");
  prog.add(countedLoop(graph, offset.dim(0), sIdx, body, dName + "/loop"));
}

// This is derived from multiUpdate, but s is added to t rather than replacing
// it
// Currently only a single dimension may be sliced
void multiUpdateAdd(Graph &graph,
                    const Tensor &t,
                    const Tensor &sMulti,
                    const Tensor &offset,
                    const Tensor &scale,
                    const std::vector<std::size_t> &dims,
                    const std::vector<std::size_t> &sizes,
                    Sequence &prog,
                    const SlicePlan & /* plan */,
                    const OptionFlags & /* options */,
                    const std::string &debugPrefix) {
  poplibs_support::logging::info(
      "multiUpdateAdd {} into {}, name={}",
      sMulti.shape(), t.shape(), debugPrefix);
  std::string dName = debugPrefix + "/multiUpdateAdd";
  // Check the offsets have been specified with a multi-slice dimension
  if (offset.rank() != 2)
    throw poputil::poplibs_error(
        "multiUpdateAdd expects offset.rank() == 2 but it is" +
        std::to_string(offset.rank()));
  if (offset.dim(1) != dims.size())
    throw poputil::poplibs_error(
        "multiUpdateAdd expects offset.dim(1) == dims.size(); offset.dim(1)==" +
        std::to_string(offset.dim(1)) + ", dims.size()== " +
        std::to_string(dims.size()));
  ValidateParams("multiUpdateAdd", t.shape(), offset[0], dims, sizes);
  if (t.rank() != 2 || dims.size() != 1 || offset.rank() != 2 ||
      offset.dim(1) != 1)
    throw poputil::poplibs_error(
        "multiUpdateAdd requires t to have 2 dimensions and dims to specify "
        "1 dimension");
  if (t.elementType() != sMulti.elementType() ||
      t.elementType() != scale.elementType())
    throw poputil::poplibs_error(
        "multiUpdateAdd expects t, sMulti and scale to have the same type");
  if (scale.rank() != 0)
    throw poputil::poplibs_error(
        "multiUpdateAdd scale must be a scaler");
  generateMultiSliceVertices("popops::MultiUpdateAdd", true, true, graph, prog,
                             offset, t, sMulti, &scale, dims[0], dName);
}

namespace embedding {

static void applyPlanConstraints(popsolver::Model &m,
                                 const PlanConstraints &planConstraints,
                                 const popsolver::Variable mSlicedDimSplit,
                                 const popsolver::Variable mUnslicedDimSplit,
                                 const popsolver::Variable mLookupSplit) {
  const auto constrainVar = [&](const char *name, popsolver::Variable var) {
    if (auto constraint = planConstraints.get_optional<unsigned>(name)) {
      m.equal(var, *constraint);
    }
  };

  // unslicedGrainSize is constrained at the beginning of model construction
  // as that number is used for calculating other values in the model.
  constrainVar("slicedDimSplit", mSlicedDimSplit);
  constrainVar("unslicedDimSplit", mUnslicedDimSplit);
  constrainVar("lookupSplit", mLookupSplit);
}

// Plan an embedding layer for slicing/updating.
// This planner aims to minimise the persistent tile memory while keeping
// temporary memory below a bound.
SlicePlan
plan(const Graph &graph,
     const Type &dataType,
     const std::size_t numEntries,
     const std::size_t outputSize,//embedding size
     const std::vector<std::size_t> &numLookups,
     const OptionFlags &optionFlags) {
  const auto options = parseSliceOptions(optionFlags);

  logging::debug(
      "DynamicSlicePlan for type {}, numEntries {}, outputSize {},"
      " numLookups {}", dataType, numEntries, outputSize, numLookups);
  const auto &target = graph.getTarget();
  const auto dataElementSize = target.getTypeSize(dataType);

  // Plan based on the max supplied number of indices
  unsigned plannedNumIndices =
      numLookups.empty() ? 1 : *std::max_element(numLookups.cbegin(),
                                                 numLookups.cend());
  SlicePlanInternal p;

  // Choose the grainsize in unsliced dimension to avoid subword writes
  const std::size_t minGrainSizeBytes = target.getAtomicStoreGranularity();

  // The embedding dimension can be split (embeddingSplit),
  // the entries can be split (dictSplit),
  // the indices can be split (lookupSplit)
  popsolver::Model m;
  // Indices are int32 so 4bytes each
  const auto mBytesPerIndex = m.addConstant(target.getTypeSize(UNSIGNED_INT));
  const auto mBytesPerFloat = m.addConstant(target.getTypeSize(FLOAT));

  // The grainsize can be constrained externally so bytesPerGrain must be
  // derived from it
  const auto unslicedGrainSize =
    options.planConstraints.get_optional<unsigned>("unslicedGrainSize")
                           .value_or(minGrainSizeBytes /
                                     gcd(minGrainSizeBytes, dataElementSize));
  const auto mUnslicedGrainSize =
    m.addConstant(unslicedGrainSize, "unslicedGrainSize");
  const auto bytesPerGrain = unslicedGrainSize * dataElementSize;
  const auto mBytesPerGrain = m.addConstant(bytesPerGrain);

  const auto mOutputSize = m.addConstant(outputSize, "outputSize");
  const auto mNumUnslicedGrains = // per row
    m.ceildiv(mOutputSize, mUnslicedGrainSize, "numUnslicedGrains");

  // split the embedding between \a mEmbeddingSplit tiles
  const auto mEmbeddingSplit =
    m.addVariable(1, std::numeric_limits<unsigned>::max(), "embeddingSplit");
  m.lessOrEqual(mEmbeddingSplit, mNumUnslicedGrains);
  m.ceildivConstrainDivisor(mNumUnslicedGrains, mEmbeddingSplit);

  // The entries are split across \a entriesSplit groups of tiles,
  // each of which will select a candidate in the first stage of a lookup.
  // A second stage is then required to select between theses candidates. This
  // means that temporary memory is required after the first pass.
  // Spilts leaving less than 2 entries per tile will have more unmeasured
  // overhead than is saved in base memory so are prohbited
  const auto mDictSplit =
      m.addVariable(1, ceildiv(numEntries, 2u), "entriesSplit");
  // mDictIsSplit=0 when mDictSplit==1, else 1
  const auto mDictIsSplit = m.sub(m.addConstant(1),
                                  m.floordiv(m.addConstant(1), mDictSplit));

  // When there are many lookups we can split the indices between multiple
  // groups of tiles each performing the same lookup on a subset of indices.
  // This requires the embedding to be broadcast for lookups, and the updates
  // to be serialised or reduced on update
  // When there is an indices split a temporary embedding buffer is required in
  // both passes
  const auto mLookupSplit = m.addVariable(1, plannedNumIndices,
                                          "lookupSplit");
  // mLookupsAreSplit=0 when mLookupSplit==1 split, else 1
  const auto mLookupsAreSplit = m.sub(m.addConstant(1),
                                m.floordiv(m.addConstant(1), mLookupSplit));
  const auto mNumTiles = m.addConstant(target.getNumTiles(), "numTiles");
  const auto mNumEntries = m.addConstant(numEntries);
  const auto mNumIndices = m.addConstant(plannedNumIndices);

  // When `mLookupSplit` != 1 the dictionary is distributed across the different
  // lookup instantiations and broadcast before use
  const auto mDictEntriesPerTile =
      m.ceildivConstrainDivisor(mNumEntries,
                                m.product({mDictSplit, mLookupSplit}));

  const auto mBaseGrainsPerRow =
      m.ceildiv(mNumUnslicedGrains, mEmbeddingSplit);
  const auto mIndicesPerLGroup = m.ceildiv(mNumIndices, mLookupSplit);
  const auto mUsedTiles =
    m.product({mEmbeddingSplit, mDictSplit, mLookupSplit}, "totalSplit");
  m.lessOrEqual(mUsedTiles, mNumTiles);

  // The memory required by the base (embedding) tensor.
  // Note we budget assuming each group will have 1/mDictSplit
  // of the embedding plus a full copy in temporary memory.
  const auto mBaseGrains =
    m.product({mBaseGrainsPerRow, mDictEntriesPerTile});
  const auto mSlicesGrains =
    m.product({mBaseGrainsPerRow, mIndicesPerLGroup});
  const auto mOutputGrains =
    m.product({mBaseGrainsPerRow, m.ceildiv(mIndicesPerLGroup, mDictSplit)});
  const auto mBaseBytes = m.product({mBaseGrains, mBytesPerGrain});
  const auto mIndicesBytes = m.product({mIndicesPerLGroup, mBytesPerIndex});
  const auto mOutputBytes = m.product({mOutputGrains, mBytesPerGrain});

  // The base tensor must be broadcast across the `mLookupSplit` groups as it
  // is distributed to balance memory.
  // The indices must be broadcast across the `mDictSplit` groups since all need
  // them in phase0. Similarly the rearrangement before phase1 also requires
  // all-all exchange across the `mDictSplit` groups.
  // Including a term for exchange code gives a small bias increasing
  // `embeddingSplit` and decreasing `lookupSplit` and `slicedDimSplit`.
   auto mExchangeCodeBytes =
     m.sum({m.product({mLookupsAreSplit, mLookupSplit, m.addConstant(4)}),
            m.product({mDictIsSplit, mDictSplit, m.addConstant(2*4)})});

  auto mUpdateTmpBytes = m.addConstant(0);
  if (options.usedForUpdate) {
    // When no index split there are no temporaries beyond those used in a
    // lookup, the vertice work directly on the base, slices and indices
    // tensors.
    // When `mLookupsAreSplit` the indices and updates are rearranged onto the
    // tile, the updates are cast to FLOAT and then accumulated
    // with a FLOAT copy of the base tensor.
    const auto mPreCastUpdateBytes = // copy of the slices for a tile
      m.product({mSlicesGrains, mBytesPerGrain});
    const auto mCastUpdateBytes =
      m.product({mSlicesGrains, mUnslicedGrainSize, mBytesPerFloat});
    const auto mPartialBytes = m.product({mBaseGrains, mLookupSplit,
                                          mUnslicedGrainSize, mBytesPerFloat});
    const auto mRearrangedIndices =
      m.product({mIndicesPerLGroup, mBytesPerIndex});

    const auto mMaxTmp = m.max({
       // pre-cast and float-cast updates
       m.sum({mPreCastUpdateBytes, mCastUpdateBytes}),
       // float-updates, indices and partial
       m.sum({mRearrangedIndices, mCastUpdateBytes, mPartialBytes}),
       // reduction (also the actual update will have the base upcast to the
       // same size as the partials, so the same footprint)
       m.sum({mPartialBytes, mPartialBytes})
    });
    mUpdateTmpBytes =
      m.product({mLookupsAreSplit, mMaxTmp});

    // indices must be broadcast from any `dictSplit` as for the forward pass,
    // plus the rearrangement will be an all-all exchange
    mExchangeCodeBytes =
      m.sum({mExchangeCodeBytes,
             m.sum({m.product({mDictIsSplit, mDictSplit, m.addConstant(4)}),
                    m.product({mLookupsAreSplit, mLookupSplit, m.addConstant(4)
                              })})});
  }

  // When `mLookupsAreSplit` the base tensor must be reconstituted
  const auto mTmpTileDictBytes =
    m.product({mLookupsAreSplit, mLookupSplit, mBaseBytes});

  // When splitting the dictionary a rearrangement is required between the two
  // stages
  const auto mTmpRearrangeGrains =
    m.product({mDictIsSplit, mBaseGrainsPerRow, mIndicesPerLGroup});
  const auto mTmpRearrangeBytes =
    m.product({mTmpRearrangeGrains, mBytesPerGrain});

  const auto mPeakTmpBytes =
      m.max({m.sum({mTmpTileDictBytes, mTmpRearrangeBytes}),
             m.sum({mTmpRearrangeBytes, mTmpRearrangeBytes}),
             options.usedForUpdate ?
               m.sum({mTmpRearrangeBytes, mUpdateTmpBytes}) :
               m.addConstant(0)});

  // Minimise total memory footprint, prioritising persistent memory
  // indices are persistent if they are required for the update pass
  //
  // TODO: Consider hard limit on temporary bytes specified via options
  // to the plan.
  auto goal = m.sum({mBaseBytes, mOutputBytes, mExchangeCodeBytes});
  if (options.usedForUpdate) {
    goal = m.sum({goal, mIndicesBytes});
  }
  goal = m.product({goal, m.addConstant(10)});
  goal = m.sum({goal, mPeakTmpBytes});

  applyPlanConstraints(m, options.planConstraints, mDictSplit, mEmbeddingSplit,
                       mLookupSplit);
  popsolver::Solution s = m.minimize({goal});

  // We must have a valid solution.
  if (!s.validSolution()) {
    logging::critical("Slice planner could not find a valid solution");
    return std::make_unique<SlicePlanInternal>();
  }

  p.partition.lookupSplit = s[mLookupSplit];
  p.partition.slicedDimSplit = s[mDictSplit];
  p.partition.unslicedDimSplit = s[mEmbeddingSplit];
  p.partition.unslicedGrainSize = s[mUnslicedGrainSize];
  p.rank = 2;
  p.slicedDims = {0};
  p.slicedDimSizes = {1};
  p.isNull = false;

  logging::info("Embedding {}", p);
  logging::debug("UsedTiles {}", s[mUsedTiles]);
  logging::debug("mNumUnslicedGrains {}, mBaseGrainsPerRow {}",
                 s[mNumUnslicedGrains], s[mBaseGrainsPerRow]);
  logging::debug("Memory estimates(bytes): base {}, output {}, indices {}, exch"
                 " {} DictTemp {}, ReTemp {}, UpdateReduction {}, goal {}",
                 s[mBaseBytes], s[mOutputBytes], s[mIndicesBytes],
                 s[mExchangeCodeBytes], s[mTmpTileDictBytes],
                 s[mTmpRearrangeBytes], s[mUpdateTmpBytes], s[goal]);
  logging::debug("mDictSplit {}, mEmbeddingSplit {}, lookupSplit {}",
                 s[mDictSplit], s[mEmbeddingSplit], s[mLookupSplit]);

  return std::make_unique<SlicePlanInternal>(std::move(p));
}

} // end namespace embedding

} // end namespace popops
