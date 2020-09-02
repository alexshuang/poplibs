// Copyright (c) 2018 Graphcore Ltd. All rights reserved.

#include "PoolVertices.hpp"
#include "CreatePoolingVertex.hpp"
#include "PerformanceEstimation.hpp"
#include "PoolingDefUtil.hpp"
#include "poplibs_support/Compiler.hpp"
#include "poplibs_support/VectorUtils.hpp"
#include "poplibs_support/logging.hpp"
#include "poplin/ConvUtil.hpp"
#include "poputil/TileMapping.hpp"
#include "poputil/VertexTemplates.hpp"
#include "poputil/exceptions.hpp"
#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <cassert>

using namespace poplar;
using namespace poplar::program;
using namespace poplin;
using namespace popnn::pooling;
using namespace poputil;
using namespace poplibs_support;
using BoostInterval = boost::icl::interval<std::size_t>;

namespace {
struct PartialRow {
  unsigned b;
  std::vector<unsigned> outerFieldIndices;
  unsigned xBegin;
  unsigned xEnd;
  PartialRow(unsigned b, std::vector<unsigned> outerFieldIndices,
             unsigned xBegin, unsigned xEnd)
      : b(b), outerFieldIndices(std::move(outerFieldIndices)), xBegin(xBegin),
        xEnd(xEnd) {}
};

// partition work such that the innermost dimension of the output field is
// split into chunks. Only one element of the other dimensions can contribute
// to work for each partial row.
static std::vector<std::vector<PartialRow>>
partitionPartialByContext(std::size_t batchElements,
                          const std::vector<std::size_t> &tileConvOutSize,
                          std::size_t numContexts) {
  const auto numFieldDims = tileConvOutSize.size();
  std::vector<std::vector<PartialRow>> partitionByContext;
  partitionByContext.reserve(numContexts);
  const auto elementsPerRow = tileConvOutSize.back();
  unsigned activeRows = 1;
  std::vector<unsigned> activeRowShape;
  activeRowShape.reserve(numFieldDims);
  for (unsigned dim = 0; dim + 1 < numFieldDims; ++dim) {
    auto dimActiveRows = tileConvOutSize[dim];
    activeRowShape.push_back(dimActiveRows);
    activeRows *= dimActiveRows;
  }
  const auto numElements = batchElements * activeRows * elementsPerRow;
  for (unsigned i = 0; i != numContexts; ++i) {
    partitionByContext.emplace_back();
    const auto beginElement = (i * numElements) / numContexts;
    const auto endElement = ((i + 1) * numElements) / numContexts;
    if (beginElement == endElement)
      continue;
    const auto lastElement = endElement - 1;
    auto beginIndices = poputil::unflattenIndex<std::size_t>(
        {batchElements, activeRows, elementsPerRow}, beginElement);
    auto lastIndices = poputil::unflattenIndex<std::size_t>(
        {batchElements, activeRows, elementsPerRow}, lastElement);
    for (unsigned b = beginIndices[0]; b != lastIndices[0] + 1; ++b) {
      unsigned activeRowBegin = b == beginIndices[0] ? beginIndices[1] : 0;
      unsigned activeRowLast =
          b == lastIndices[0] ? lastIndices[1] : activeRows - 1;
      for (unsigned activeRow = activeRowBegin; activeRow != activeRowLast + 1;
           ++activeRow) {
        unsigned activeXBegin =
            b == beginIndices[0] && activeRow == beginIndices[1]
                ? beginIndices[2]
                : 0;
        unsigned activeXLast =
            b == lastIndices[0] && activeRow == lastIndices[1]
                ? lastIndices[2]
                : elementsPerRow - 1;
        auto outerFieldIndices =
            poputil::unflattenIndex(activeRowShape, activeRow);
        for (unsigned dim = 0; dim != outerFieldIndices.size(); ++dim) {
          assert(outerFieldIndices[dim] < tileConvOutSize[dim]);
        }
        const auto xBegin = activeXBegin;
        const auto xEnd = activeXLast + 1;
        assert(b < batchElements);
        assert(xBegin < tileConvOutSize.back());
        assert(xEnd <= tileConvOutSize.back());
        partitionByContext.back().emplace_back(b, outerFieldIndices, xBegin,
                                               xEnd);
      }
    }
  }
  return partitionByContext;
}

static std::string getVertexName(const PoolConfig &poolCfg, const Type &dType) {
  switch (poolCfg.type) {
  case popnn::PoolingType::MAX:
    if (poolCfg.pass == PoolPass::POOL_FWD)
      return poolCfg.scaledGradient
                 ? templateVertex("popnn::MaxPoolingGradientScale", dType)
                 : templateVertex("popnn::MaxPooling", dType);
    else
      return templateVertex("popnn::MaxPoolingGrad", dType);
  case popnn::PoolingType::AVG:
  case popnn::PoolingType::SUM:
    return templateVertex("popnn::SumPooling", dType);
  }
  POPLIB_UNREACHABLE();
}

static std::pair<unsigned, unsigned> getTileOutRange(const ConvParams &params,
                                                     const Partition &partition,
                                                     unsigned tileIndex,
                                                     unsigned dim) {
  const auto outSize = params.getOutputSize(dim);
  const auto split = partition.field[dim];
  const auto outBegin = (tileIndex * outSize) / split;
  const auto outEnd = ((tileIndex + 1) * outSize) / split;
  return {outBegin, outEnd};
}

// Work paritions derived from splitting batch and field on this tile
struct PartitionPerKernelPos {
  std::vector<std::size_t> inBeginIndices;
  std::vector<std::size_t> outBeginIndices;
  std::size_t inWidthX;
  std::size_t outWidthX;
  std::size_t b;
};

std::vector<std::vector<std::vector<PartitionPerKernelPos>>>
createPartitions(const std::vector<std::vector<PartialRow>> &contextPartition,
                 const std::vector<std::size_t> &kernelShape,
                 unsigned numContexts, const ConvParams &params,
                 const PoolSlice &slice) {

  std::vector<std::vector<std::vector<PartitionPerKernelPos>>> partitions(
      numContexts);

  // compute the number of kernel positions used by this slice
  const auto numKernelPositions = product(kernelShape);
  const auto numFieldDims = slice.kernelBegin.size();

  // Using a flattened vector in each of these cases provides a speed
  // improvement over a vector of vectors [numFieldDims][numKernelPositions],
  // due to simpler memory allocation and access
  std::vector<std::size_t> tileOutBegin(numFieldDims * numKernelPositions);
  std::vector<std::size_t> tileOutSize(numFieldDims * numKernelPositions);
  std::vector<std::size_t> kernelBeginIndices(numFieldDims *
                                              numKernelPositions);

  std::vector<std::size_t> indices(numKernelPositions);
  for (unsigned k = 0; k < numKernelPositions; k++) {
    indices = unflattenIndex(kernelShape, k);
    // update kernel begin indices to those of the full field because these
    // are positions with the kernel positions assigned to this tile
    for (std::size_t dim = 0; dim != numFieldDims; ++dim) {
      kernelBeginIndices[k * numFieldDims + dim] =
          indices[dim] + slice.kernelBegin[dim];
    }
  }
  for (std::size_t c = 0; c != contextPartition.size(); ++c) {
    for (const auto &row : contextPartition[c]) {
      // for each partial row find the output range for each kernel position
      for (std::size_t k = 0; k != numKernelPositions; ++k) {
        // get the output range in the full field
        for (unsigned dim = 0; dim != numFieldDims - 1; ++dim) {
          const auto kernelBeginIndex =
              kernelBeginIndices[k * numFieldDims + dim];
          const auto outBegin =
              slice.fieldBegin[dim] + row.outerFieldIndices[dim];
          const auto outEnd =
              slice.fieldBegin[dim] + row.outerFieldIndices[dim] + 1;
          auto outRange = getOutputRangeForKernelIndex(
              dim, {outBegin, outEnd}, kernelBeginIndex, params);
          tileOutBegin[k * numFieldDims + dim] = outRange.first;
          tileOutSize[k * numFieldDims + dim] =
              outRange.second - outRange.first;
        }
        const auto outBegin = slice.fieldBegin[numFieldDims - 1] + row.xBegin;
        const auto outEnd = slice.fieldBegin[numFieldDims - 1] + row.xEnd;
        auto outRange = getOutputRangeForKernelIndex(
            numFieldDims - 1, {outBegin, outEnd},
            kernelBeginIndices[k * numFieldDims + numFieldDims - 1], params);
        tileOutBegin[k * numFieldDims + numFieldDims - 1] = outRange.first;
        tileOutSize[k * numFieldDims + numFieldDims - 1] =
            outRange.second - outRange.first;
      }
      // This contains the work done per partial row from input for each
      // contributing kernel position
      std::vector<PartitionPerKernelPos> rowPartition;
      rowPartition.reserve(numKernelPositions);
      for (std::size_t k = 0; k != numKernelPositions; ++k) {
        // There may be no contribution to the output for the kernel position.
        // If it the case, there is no work to be done.
        // Move on to next kernel position.
        if (std::accumulate(tileOutSize.begin() + k * numFieldDims,
                            tileOutSize.begin() + (k + 1) * numFieldDims, 1,
                            std::multiplies<std::size_t>()) == 0) {
          continue;
        }
        // Find the input range which contributes to the output. We need the
        // output indices to be relative to the slice we take from the full
        // field. But because we need to take the slice from the full field
        // of the input and output tensors we first use offsets from the full
        // field, except the batch because all batches assigned to this tile
        // will anyway be sliced out. We later find offsets of the field
        // dimensions once we know what slice we extract for this tile.
        std::vector<std::size_t> outBeginIndices = {row.b};
        std::vector<std::size_t> inBeginIndices = {row.b};
        outBeginIndices.reserve(numFieldDims);
        inBeginIndices.reserve(numFieldDims);
        for (unsigned dim = 0; dim + 1 < numFieldDims; ++dim) {
          assert(tileOutSize[k * numFieldDims + dim] == 1);
          auto inIndex =
              getInputIndex(dim, tileOutBegin[k * numFieldDims + dim],
                            kernelBeginIndices[k * numFieldDims + dim], params);
          assert(inIndex != ~0U);
          inBeginIndices.emplace_back(inIndex);
          outBeginIndices.emplace_back(tileOutBegin[k * numFieldDims + dim]);
        }
        // innermost dimension is treated differently
        const auto dim = numFieldDims - 1;
        auto workerInXRange = getInputRange(
            dim,
            {tileOutBegin[k * numFieldDims + dim],
             tileOutBegin[k * numFieldDims + dim] +
                 tileOutSize[k * numFieldDims + dim]},
            kernelBeginIndices[k * numFieldDims + numFieldDims - 1], params);
        assert(workerInXRange.first != ~0U);
        inBeginIndices.emplace_back(workerInXRange.first);
        outBeginIndices.emplace_back(tileOutBegin[k * numFieldDims + dim]);
        rowPartition.push_back({std::move(inBeginIndices),
                                std::move(outBeginIndices),
                                workerInXRange.second - workerInXRange.first,
                                tileOutSize[k * numFieldDims + dim], row.b});
      }
      partitions[c].emplace_back(std::move(rowPartition));
    }
  }
  return partitions;
}
void createSlices(
    const std::vector<PartitionPerKernelPos *> &flattenedPartitions,
    const unsigned numFieldDims, std::vector<std::size_t> &inSliceBegin,
    std::vector<std::size_t> &inSliceEnd,
    std::vector<std::size_t> &outSliceBegin,
    std::vector<std::size_t> &outSliceEnd) {
  // From the partitions find the range of field dimensions used by the
  // partition. This is required because we extract only the portion of the
  // input required by the tile (Need to check if this would result in a
  // copy. It shouldn't unless there is an explicit truncation which is not
  // supported)
  const auto maxValue = std::numeric_limits<std::size_t>::max();
  std::vector<std::pair<std::size_t, std::size_t>> inputRange(
      numFieldDims, std::make_pair(maxValue, 0));
  std::vector<std::pair<std::size_t, std::size_t>> outputRange(
      numFieldDims, std::make_pair(maxValue, 0));

  for (const auto &fp : flattenedPartitions) {
    for (std::size_t dim = 0; dim != numFieldDims; ++dim) {
      const auto inWidth = (dim + 1 == numFieldDims) ? fp->inWidthX : 1;
      const auto outWidth = (dim + 1 == numFieldDims) ? fp->outWidthX : 1;
      inputRange[dim].first =
          std::min(inputRange[dim].first, fp->inBeginIndices[dim + 1]);
      inputRange[dim].second = std::max(inputRange[dim].second,
                                        fp->inBeginIndices[dim + 1] + inWidth);
      outputRange[dim].first =
          std::min(outputRange[dim].first, fp->outBeginIndices[dim + 1]);
      outputRange[dim].second = std::max(
          outputRange[dim].second, fp->outBeginIndices[dim + 1] + outWidth);
    }
  }

  for (std::size_t dim = 0; dim != numFieldDims; ++dim) {
    inSliceBegin.push_back(inputRange[dim].first);
    inSliceEnd.push_back(inputRange[dim].second);
    outSliceBegin.push_back(outputRange[dim].first);
    outSliceEnd.push_back(outputRange[dim].second);
  }
  // once the input and output tensor slices are taken, adjust the indices to
  // reflect that
  for (auto &fp : flattenedPartitions) {
    for (auto dim = 0UL; dim != numFieldDims; ++dim) {
      fp->outBeginIndices[dim + 1] -= outputRange[dim].first;
      fp->inBeginIndices[dim + 1] -= inputRange[dim].first;
    }
  }
}
// transform indices into offsets
struct WorkListEntry {
  unsigned inBeginOffset;
  unsigned outBeginOffset;
  unsigned numElements;
};

void createWorklists(
    const unsigned numContexts, const unsigned tile, const unsigned strideX,
    const std::vector<std::vector<std::vector<PartitionPerKernelPos>>>
        &partitions,
    const std::vector<std::size_t> &inputBatchAndFieldShape,
    const std::vector<std::size_t> &outputBatchAndFieldShape,
    const bool isAveragePooling, std::vector<unsigned> &contextStartPos,
    std::vector<unsigned> &offsetBase,
    std::vector<std::vector<unsigned>> &worklist,
    boost::icl::interval_map<std::size_t, std::size_t> &scaleFactorMap) {

  // These are ordered the same way as inputs
  std::vector<std::vector<std::vector<WorkListEntry>>> worklistEntries(
      numContexts);

  // Build scale factors for average pooling
  for (std::size_t c = 0; c != numContexts; ++c) {
    for (const auto &rowPartition : partitions[c]) {
      std::vector<WorkListEntry> row;
      for (const auto &r : rowPartition) {
        const unsigned outBeginOffset =
            flattenIndex(outputBatchAndFieldShape, r.outBeginIndices);
        const unsigned inBeginOffset =
            flattenIndex(inputBatchAndFieldShape, r.inBeginIndices);
        const unsigned numFieldElems = r.outWidthX;
        row.push_back({inBeginOffset, outBeginOffset, numFieldElems});
        if (isAveragePooling) {
          const auto region = boost::icl::interval<std::size_t>::right_open(
              outBeginOffset, outBeginOffset + numFieldElems);
          scaleFactorMap.add(std::make_pair(region, 1));
        }
      }
      // sort work list entries in each row
      std::sort(row.begin(), row.end(), [](WorkListEntry &a, WorkListEntry &b) {
        return std::tie(a.outBeginOffset, a.inBeginOffset, a.numElements) <
               std::tie(b.outBeginOffset, b.inBeginOffset, b.numElements);
      });
      if (!row.empty())
        worklistEntries[c].push_back(std::move(row));
    }
  }

  unsigned contextStart = 0;
  logging::popnn::trace("Tile: {}", tile);
  contextStartPos.reserve(numContexts);
  for (std::size_t c = 0; c != numContexts; ++c) {
    std::string loggingStr = "Worklist " + std::to_string(c) + ": ";

    for (auto &rowWorkList : worklistEntries[c]) {
      const auto inBase = rowWorkList.at(0).inBeginOffset;
      const auto outBase = rowWorkList.at(0).outBeginOffset;
      std::vector<unsigned> row;
      row.reserve(3 * rowWorkList.size());
      for (auto &r : rowWorkList) {
        row.push_back(r.outBeginOffset - outBase);
        row.push_back(r.inBeginOffset - inBase);
        const auto numElements = (r.numElements + strideX - 1) / strideX;
        assert(numElements != 0);
        row.push_back(numElements - 1);
        if (logging::popnn::shouldLog(logging::Level::Trace)) {
          loggingStr += "[" + std::to_string(r.outBeginOffset - outBase) + "," +
                        std::to_string(r.inBeginOffset - inBase) + "," +
                        std::to_string(numElements - 1) + "] ";
        }
      }
      assert(!row.empty());
      offsetBase.push_back(outBase);
      offsetBase.push_back(inBase);
      worklist.push_back(std::move(row));
      ++contextStart;
    }
    contextStartPos.push_back(contextStart);
    logging::popnn::trace(loggingStr);
  }
}

// Generate vertices on a tile
// in               Input tensor of shape [CG][B][...][CPG]
// out              Input tensor of shape [CG][B][...][CPG]
// params           Parameters for the pooling operation
// cs               Compute sets to attach vertices to
// tile             Tile on which vertices are generated
// indices          indices of planning parameter splits assigned to this tile
// slice            parameters for slicing channels, batch, field and kernel
static void
generateVertices(Graph &graph, const PoolConfig &poolCfg, const Tensor &in,
                 const Tensor &out, const Tensor *fwdInputActs,
                 const Tensor *fwdOutputActs, const ConvParams &params,
                 std::vector<ComputeSet> &cs, unsigned tile,
                 const PoolSlice &slice, const std::string &debugPrefix) {
  const auto &target = graph.getTarget();
  const auto numContexts = target.getNumWorkerContexts();
  const auto numFieldDims = slice.kernelBegin.size();
  const auto chansPerGroup = out.dim(out.rank() - 1);

  if (cs.empty()) {
    cs.push_back(graph.addComputeSet(debugPrefix + "/Pool"));
  }

  // build input and kernel shapes used on this tile. These are relative offsets
  // from the slice begin offsets
  std::vector<std::size_t> kernelShape;
  std::vector<std::size_t> outputShape;
  kernelShape.reserve(numFieldDims);
  outputShape.reserve(numFieldDims);

  for (std::size_t dim = 0; dim != numFieldDims; ++dim) {
    kernelShape.push_back(slice.getKernelSize(dim));
    outputShape.push_back(slice.getFieldSize(dim));
  }

  if (slice.getBatchSize() == 0 || slice.getNumChans() == 0 ||
      product(kernelShape) == 0 || product(outputShape) == 0)
    return;

  // Note that some calculations here are on the original field. i.e. the full
  // field given by "params".

  // Ensure that each output is always processed by a single context. This will
  // guarantee that no parallel writes can occur between contexts writing to
  // the same output sample as long as there are no sub-word writes. That can
  // be controlled by the channel grain size.
  // The partitioner splits the batch axis and all the field dimension such that
  // other than the innermost dimension every partition has size 1.
  // The indices and offsets returned by the partitioner are relative to the
  // slice used on this tile and given by outputShape
  auto contextPartition =
      partitionPartialByContext(slice.getBatchSize(), outputShape, numContexts);

  // For each context and each partial row, keep a vector of partitions
  auto partitions = createPartitions(contextPartition, kernelShape, numContexts,
                                     params, slice);

  // flattened view of the partitions
  std::vector<PartitionPerKernelPos *> flattenedPartitions;
  for (auto &partition : partitions) {
    for (auto &rowPartition : partition) {
      for (auto &p : rowPartition) {
        flattenedPartitions.push_back(&p);
      }
    }
  }

  // There may be no work to do on this tile
  if (flattenedPartitions.empty()) {
    return;
  }
  // now all the ranges are available and we can take the required slice from
  // the input and output tensors
  std::vector<std::size_t> inSliceBegin = {slice.chanBegin / chansPerGroup,
                                           slice.batchBegin};
  std::vector<std::size_t> inSliceEnd = {slice.chanEnd / chansPerGroup,
                                         slice.batchEnd};
  auto outSliceBegin = inSliceBegin;
  auto outSliceEnd = inSliceEnd;

  createSlices(flattenedPartitions, numFieldDims, inSliceBegin, inSliceEnd,
               outSliceBegin, outSliceEnd);

  auto inWindow = in.slice(inSliceBegin, inSliceEnd);
  auto outWindow = out.slice(outSliceBegin, outSliceEnd);
  Tensor fwdInputActsWindow;
  Tensor fwdOutputActsWindow;
  if (fwdInputActs) {
    fwdInputActsWindow = fwdInputActs->slice(outSliceBegin, outSliceEnd);
  }
  if (fwdOutputActs) {
    fwdOutputActsWindow = poolCfg.scaledGradient
                              ? fwdOutputActs->slice(outSliceBegin, outSliceEnd)
                              : fwdOutputActs->slice(inSliceBegin, inSliceEnd);
  }

  // Get shapes to translate input and output indices
  auto inputBatchAndFieldShape = inWindow[0].shape();
  auto outputBatchAndFieldShape = outWindow[0].shape();
  inputBatchAndFieldShape.pop_back();
  outputBatchAndFieldShape.pop_back();

  // we could keep a 1D tensor by flattening the channel dimension as
  // well but it may be that the channels groups are exchanged from other tiles
  const size_t n = slice.getNumChans() / chansPerGroup;
  std::vector<Tensor> inWindows;
  std::vector<Tensor> outWindows;
  std::vector<Tensor> fwdInputActsWindows;
  std::vector<Tensor> fwdOutputActsWindows;

  inWindows.reserve(n);
  outWindows.reserve(n);
  if (fwdInputActs) {
    fwdInputActsWindows.reserve(n);
  }
  if (fwdOutputActs) {
    fwdOutputActsWindows.reserve(n);
  }

  for (std::size_t oc = 0; oc != slice.getNumChans() / chansPerGroup; ++oc) {
    inWindows.push_back(inWindow[oc].flatten());
    auto outWindowFlat = outWindow[oc].flatten();
    outWindows.push_back(outWindowFlat);
    if (fwdInputActs) {
      fwdInputActsWindows.push_back(fwdInputActsWindow[oc].flatten());
    }
    if (fwdOutputActs) {
      fwdOutputActsWindows.push_back(fwdOutputActsWindow[oc].flatten());
    }
    // map output tensor to tile
    graph.setTileMapping(outWindowFlat, tile);
  }
  // Build scale factors for average pooling
  boost::icl::interval_map<std::size_t, std::size_t> scaleFactorMap;
  std::vector<unsigned> contextStartPos;
  std::vector<unsigned> offsetBase;
  std::vector<std::vector<unsigned>> worklist;
  unsigned strideX = params.inputTransform.dilation.back();

  createWorklists(numContexts, tile, strideX, partitions,
                  inputBatchAndFieldShape, outputBatchAndFieldShape,
                  poolCfg.type == popnn::PoolingType::AVG, contextStartPos,
                  offsetBase, worklist, scaleFactorMap);

  auto codeletName = getVertexName(poolCfg, in.elementType());
  auto v = graph.addVertex(cs[0], codeletName);
  graph.connect(v["in"], inWindows);
  graph.connect(v["out"], outWindows);
  graph.setInitialValue(v["initInfo"],
                        outWindows[0].numElements() / chansPerGroup);
  const auto vectorWidth = (in.elementType() == HALF ? 4 : 2);
  assert(chansPerGroup % vectorWidth == 0);
  const auto chansPerGroupD = chansPerGroup / vectorWidth;
  graph.setInitialValue(v["chansPerGroupD"], chansPerGroupD);
  const auto numChanGroups = slice.getNumChans() / chansPerGroup;
  assert(numChanGroups != 0);
  graph.setInitialValue(v["numChanGroupsM1"], numChanGroups - 1);

  const auto worklistEntryType = UNSIGNED_SHORT;
  auto tContextStartPos = graph.addConstant(
      worklistEntryType, {contextStartPos.size()}, contextStartPos.data(),
      debugPrefix + "/ContextStartPos");
  graph.setTileMapping(tContextStartPos, 0);
  graph.connect(v["startPos"], tContextStartPos);
  auto tOffsetBase =
      graph.addConstant(worklistEntryType, {offsetBase.size()},
                        offsetBase.data(), debugPrefix + "/OffsetBase");
  graph.setTileMapping(tOffsetBase, 0);
  graph.connect(v["offsetBase"], tOffsetBase);
  for (unsigned i = 0; i < worklist.size(); ++i) {
    auto t = graph.addConstant(worklistEntryType, {worklist[i].size()},
                               worklist[i].data(), debugPrefix + "/worklist");
    graph.setTileMapping(t, 0);
    graph.connect(v["workList"][i], t);
  }
  graph.setFieldSize(v["workList"], worklist.size());
  const auto inStride = params.outputTransform.stride.back() * chansPerGroup;
  const auto outStride = strideX * chansPerGroup;
  assert(inStride % vectorWidth == 0);
  assert(outStride % vectorWidth == 0);
  graph.setInitialValue(v["inStrideD"], inStride / vectorWidth);
  graph.setInitialValue(v["outStrideD"], outStride / vectorWidth);

  logging::popnn::trace(
      "chansPerGroup: {} Groups: {} inStride: {} outStride: {}", chansPerGroup,
      numChanGroups, inStride, outStride);

  if (poolCfg.pass == PoolPass::POOL_BWD &&
      poolCfg.type == popnn::PoolingType::MAX) {
    graph.connect(v["fwdActsIn"], fwdInputActsWindows);
    graph.connect(v["fwdActsOut"], fwdOutputActsWindows);
  }

  if (poolCfg.pass == PoolPass::POOL_FWD &&
      poolCfg.type == popnn::PoolingType::MAX && poolCfg.scaledGradient) {
    graph.connect(v["fwdActsOut"], fwdOutputActsWindows);
  }

  // extract a common scale factor for the whole field if possible
  float commonScaleFactor = 0.0;
  if (poolCfg.pass == PoolPass::POOL_FWD &&
      poolCfg.type == popnn::PoolingType::AVG) {
    assert(cs.size() >= 1);
    if (cs.size() == 1) {
      cs.push_back(graph.addComputeSet(debugPrefix + "/Scale"));
    }
    // split regions between workers to scale output
    // first convert interval regions to poplar regions
    std::vector<Interval> regions;
    regions.reserve(scaleFactorMap.size());
    for (auto &r : scaleFactorMap) {
      commonScaleFactor = static_cast<float>(r.second);
      regions.emplace_back(r.first.lower(), r.first.upper());
    }
    auto scalePartitions = splitRegions(regions, 1, numContexts);

    // build scale work list
    std::vector<std::vector<unsigned short>> scaleWorklist(numContexts);
    for (std::size_t c = 0; c != scalePartitions.size(); ++c) {
      scaleWorklist[c].reserve(3 * scalePartitions[c].size());
      for (const auto &s : scalePartitions[c]) {
        scaleWorklist[c].push_back(s.begin());
        scaleWorklist[c].push_back(s.size());
        auto scaleRegion =
            boost::icl::interval<std::size_t>::right_open(s.begin(), s.end());

        const auto it = scaleFactorMap.find(scaleRegion);
        if (it->second != commonScaleFactor)
          commonScaleFactor = 0.0f;

        scaleWorklist[c].push_back(it->second);
      }
    }

    if (commonScaleFactor == 0.0f) {
      auto vScale = graph.addVertex(
          cs[1], templateVertex("popnn::SelectiveScaling", in.elementType()));
      graph.connect(vScale["inOut"], outWindows);
      graph.setInitialValue(vScale["chansPerGroup"], chansPerGroup);
      graph.setInitialValue(vScale["numChanGroups"],
                            slice.getNumChans() / chansPerGroup);

      for (unsigned i = 0; i < scaleWorklist.size(); ++i) {
        auto t = graph.addConstant(worklistEntryType, {scaleWorklist[i].size()},
                                   scaleWorklist[i].data(),
                                   debugPrefix + "/worklist");
        graph.setTileMapping(t, 0);
        graph.connect(vScale["scaleWorklist"][i], t);
      }
      graph.setFieldSize(vScale["scaleWorklist"], scaleWorklist.size());
      graph.setTileMapping(vScale, tile);
      graph.setInitialValue(v["scale"], 1.0);

    } else {
      graph.setInitialValue(v["scale"], 1.0f / commonScaleFactor);
    }
  } else {
    if (poolCfg.type != popnn::PoolingType::MAX && !poolCfg.scaledGradient)
      graph.setInitialValue(v["scale"], 1.0);
  }
  graph.setTileMapping(v, tile);
}

// Linearly map to tiles bsed on the parition split and the indices for that
// split
static unsigned linearTileMap(const PoolIndices &indices,
                              const Partition &split) {
  const auto numFieldDims = indices.out.size();
  std::size_t tile = indices.chan;
  for (std::size_t dim = 0; dim != numFieldDims; ++dim) {
    tile = tile * split.kernel[dim] + indices.kernel[dim];
  }
  tile = tile * split.batch + indices.batch;
  for (std::size_t dim = 0; dim != numFieldDims; ++dim) {
    tile = tile * split.field[dim] + indices.out[dim];
  }
  return static_cast<unsigned>(tile);
}

// Build an interval set of regions used by a slice
// This needs to be sped up
static boost::icl::interval_set<std::size_t>
tileRegionsSet(const PoolSlice &slice, const std::vector<std::size_t> &shape) {
  // create tensor on the original tensor
  boost::icl::interval_set<std::size_t> regions;
  const auto numFieldDims = shape.size() - 3;
  const auto chansPerGroup = shape.back();
  auto reducedShape = shape;
  reducedShape.pop_back();
  std::vector<std::size_t> fieldSliceSize;
  fieldSliceSize.reserve(numFieldDims);

  for (std::size_t dim = 0; dim != numFieldDims; ++dim) {
    fieldSliceSize.push_back(slice.getFieldSize(dim));
  }
  const auto fieldSize = product(fieldSliceSize);

  for (std::size_t b = slice.batchBegin; b != slice.batchEnd; ++b) {
    for (std::size_t c = slice.chanBegin / chansPerGroup;
         c != slice.chanEnd / chansPerGroup; ++c) {
      for (std::size_t f = 0; f != fieldSize; ++f) {
        std::vector<std::size_t> indices = {c, b};
        auto fieldIndices = unflattenIndex(fieldSliceSize, f);
        std::transform(fieldIndices.begin(), fieldIndices.end(),
                       slice.fieldBegin.begin(), std::begin(fieldIndices),
                       std::plus<std::size_t>());
        indices.insert(indices.end(), fieldIndices.begin(), fieldIndices.end());
        auto groupBegin = flattenIndex(reducedShape, indices);
        regions += boost::icl::interval<std::size_t>::right_open(
            groupBegin * chansPerGroup, (groupBegin + 1) * chansPerGroup);
      }
    }
  }
  return regions;
}

// Get mapping of output tensor given the input tensor and the pooling operation
// parameters. The mapping is represented as an interval set.
static std::vector<boost::icl::interval_set<std::size_t>>
getTileMappingSets(Graph &graph, const Tensor &in_) {
  Tensor in = in_;
  const auto inMapping = graph.getTileMapping(in);
  const auto numTiles = inMapping.size();
  // convert to interval set
  std::vector<boost::icl::interval_set<std::size_t>> tileMappingSets(numTiles);
  for (std::size_t tile = 0; tile != numTiles; ++tile) {
    for (const auto &r : inMapping[tile])
      tileMappingSets[tile].insert(
          boost::icl::interval<std::size_t>::right_open(r.begin(), r.end()));
  }
  return tileMappingSets;
}

// get tile to map based on the largest intersection with regions already
// mapped on tile
static unsigned getTileToMap(
    const std::vector<boost::icl::interval_set<std::size_t>> &tileMappingSet,
    const boost::icl::interval_set<std::size_t> &setToMatch,
    std::vector<unsigned> &tileMapOrder) {
  assert(tileMapOrder.size() == tileMappingSet.size());
  unsigned bestSize = 0;
  unsigned bestIndex = ~0U;
  for (unsigned t = 0; t != tileMapOrder.size(); ++t) {
    auto index = tileMapOrder[t];
    if (index == ~0U)
      continue;
    // find which has the best match
    auto setUnion = tileMappingSet[index] + setToMatch;
    auto setIntersection =
        tileMappingSet[index].size() + setToMatch.size() - setUnion.size();
    if (setIntersection > bestSize || bestIndex == ~0U) {
      bestIndex = t;
      bestSize = setIntersection;
    }
  }
  assert(bestIndex != ~0U);
  auto tile = tileMapOrder[bestIndex];
  tileMapOrder[bestIndex] = ~0U;
  return tile;
}

} // namespace

namespace popnn {
namespace pooling {

void tilePartitions(Graph &graph, const PoolConfig &poolCfg, const Tensor &in,
                    const Tensor &out, const Tensor *fwdInputActs,
                    const Tensor *fwdOutputActs, const ConvParams &params,
                    Sequence &prog, const Plan &plan,
                    const std::string &debugPrefix,
                    const PoolOptions &poolOptions) {
  const auto &partition = plan.partition;
  const auto numFieldDims = params.getNumFieldDims();
  const auto numChans = in.dim(0) * in.dim(in.rank() - 1);
  const auto batchSplit = partition.batch;
  const auto chanSplit = partition.chanGroups;
  const auto batchSize = in.dim(1);
  const auto chanGrainSize = in.dim(in.rank() - 1);
  const auto chanNumGrains = (numChans + chanGrainSize - 1) / chanGrainSize;

  // Used only with tile introspective mapping
  // By default use tile introspection on input tensor. But in the case when
  // output mapping is available, allow an option to use it.
  std::vector<boost::icl::interval_set<std::size_t>> tileMappingSets;
  std::vector<unsigned> mapOrder;
  bool useIntrospectionOnInput = true;

  if (!useIntrospectionOnInput) {
    // only allow introspection on output if output mapping is available
    useIntrospectionOnInput = fwdInputActs == nullptr;
  }

  auto tensorForTileIntrospection =
      useIntrospectionOnInput ? in : *fwdInputActs;

  if (poolOptions.poolUseIntrospectiveMapping) {
    tileMappingSets = getTileMappingSets(graph, tensorForTileIntrospection);
    mapOrder.resize(tileMappingSets.size());
    std::iota(mapOrder.begin(), mapOrder.end(), 0);
    std::stable_sort(
        mapOrder.begin(), mapOrder.end(), [&](unsigned a, unsigned b) {
          return tileMappingSets[a].size() < tileMappingSets[b].size();
        });
  }

  std::vector<ComputeSet> cs;
  const auto totalFieldSplit = product(partition.field);
  const auto totalKernelSplit = product(partition.kernel);
  for (std::size_t b = 0; b != batchSplit; ++b) {
    const auto batchBegin = (b * batchSize) / batchSplit;
    const auto batchEnd = ((b + 1) * batchSize) / batchSplit;
    for (std::size_t c = 0; c != chanSplit; ++c) {
      const auto chanGrainBegin = (c * chanNumGrains) / chanSplit;
      const auto chanGrainEnd = ((c + 1) * chanNumGrains) / chanSplit;
      const auto chanBegin = chanGrainBegin * chanGrainSize;
      const auto chanEnd = std::min(chanGrainEnd * chanGrainSize, numChans);
      for (std::size_t k = 0; k != totalKernelSplit; ++k) {
        auto kernelIndices = unflattenIndex(partition.kernel, k);
        std::vector<std::size_t> kernelBegin(numFieldDims),
            kernelEnd(numFieldDims);
        for (unsigned dim = 0; dim != numFieldDims; ++dim) {
          const auto kernelSize = params.kernelShape[dim];
          kernelBegin[dim] =
              (kernelIndices[dim] * kernelSize) / partition.kernel[dim];
          kernelEnd[dim] =
              ((kernelIndices[dim] + 1) * kernelSize) / partition.kernel[dim];
        }
        for (std::size_t of = 0; of != totalFieldSplit; ++of) {
          auto outIndices = unflattenIndex(partition.field, of);
          std::vector<std::size_t> outFieldBegin(numFieldDims),
              outFieldEnd(numFieldDims);
          std::vector<std::size_t> inputFieldBegin(numFieldDims),
              inputFieldEnd(numFieldDims);
          for (unsigned dim = 0; dim != numFieldDims; ++dim) {
            std::tie(outFieldBegin[dim], outFieldEnd[dim]) =
                getTileOutRange(params, partition, outIndices[dim], dim);
            std::tie(inputFieldBegin[dim], inputFieldEnd[dim]) =
                getInputRange(dim, {outFieldBegin[dim], outFieldEnd[dim]},
                              {kernelBegin[dim], kernelEnd[dim]}, params);
          }
          const PoolIndices poolIndices = {b, outIndices, c, kernelIndices};
          unsigned tile;
          PoolSlice outputSlice = {batchBegin,  batchEnd,  outFieldBegin,
                                   outFieldEnd, chanBegin, chanEnd,
                                   kernelBegin, kernelEnd};

          if (poolOptions.poolUseIntrospectiveMapping) {
            PoolSlice inputSlice = {batchBegin,    batchEnd,  inputFieldBegin,
                                    inputFieldEnd, chanBegin, chanEnd,
                                    kernelBegin,   kernelEnd};

            const auto tileRegions = tileRegionsSet(
                useIntrospectionOnInput ? inputSlice : outputSlice,
                tensorForTileIntrospection.shape());
            tile = getTileToMap(tileMappingSets, tileRegions, mapOrder);
          } else {
            tile = linearTileMap(poolIndices, partition);
          }
          generateVertices(graph, poolCfg, in, out, fwdInputActs, fwdOutputActs,
                           params, cs, tile, outputSlice, debugPrefix);
        }
      }
    }
  }

  for (auto c : cs) {
    prog.add(Execute(c));
  }
}
// Test function for vertex test
void createPoolingVertex(Graph &graph, const PoolParams &poolParams,
                         const poplar::Tensor &prevAct,
                         const poplar::Tensor &nextAct,
                         poplar::program::Sequence &prog) {

  auto convParams = makeConvParams(poolParams);
  std::vector<ComputeSet> cs;
  std::vector<std::size_t> start(poolParams.kernelShape.size(), 0);
  auto outShapeSpatial = nextAct.shape();
  outShapeSpatial.pop_back();
  outShapeSpatial.erase(outShapeSpatial.begin(), outShapeSpatial.begin() + 2);
  PoolSlice slice = {0, poolParams.batchSize,   start, outShapeSpatial,
                     0, poolParams.numChannels, start, poolParams.kernelShape};
  generateVertices(graph, {poolParams.poolingType, PoolPass::POOL_FWD, false},
                   prevAct, nextAct, nullptr, nullptr, convParams, cs, 0, slice,
                   "TestPoolingVertex");
  prog.add(Execute(cs[0]));
}

// Cycle estimator call for use by the planner
std::size_t poolVertexCycleEstimate(const Partition &tilePartition,
                                    const PoolConfig &poolCfg,
                                    const ConvParams &params,
                                    const unsigned numContexts) {
  std::vector<std::size_t> start(tilePartition.field.size(), 0);
  PoolSlice slice = {0,     tilePartition.batch,         // Batch start, end
                     start, tilePartition.field,         // Field start, end
                     0,     tilePartition.chansPerGroup, // Channel begin, end
                     start, tilePartition.kernel};       // Kernel begin, end

  auto contextPartition = partitionPartialByContext(
      slice.getBatchSize(), slice.fieldEnd, numContexts);

  // Build worklist, startPos array, populating the worklist with (size-1) only,
  // as we don't need in and out offsets for cycle estimation.
  // This can be done more correctly by calling `createPartitions` but that is
  // time consuming.  The benefit in doing so would be that padding would be
  // correctly accounted for, which results in fewer worklist entries where an
  // output is a function of inputs that are actually padding  so are not
  // computed at all).  The performance difference in not calling
  // `createPartitions` is small (probably noise as some tests are faster,
  // some slower), whereas the compilation speed is improved.

  // The vertex accepts
  // `workList` [N, workListEntries]
  // `startPos` [contexts] = Start position in workList for worker, indexed with
  //            id - 1.  Worker 0 starts at position 0
  unsigned numPartitions = 0;
  for (unsigned i = 0; i < contextPartition.size(); i++) {
    numPartitions += contextPartition[i].size();
  }
  std::vector<unsigned short> startPos;
  std::vector<std::vector<unsigned short>> workList(numPartitions);
  unsigned index = 0;
  unsigned strideX = params.inputTransform.dilation.back();

  const unsigned numKernelPositions = product(params.kernelShape);
  for (unsigned i = 0; i < numContexts; i++) {
    for (unsigned j = 0; j < contextPartition[i].size(); j++) {
      for (unsigned k = 0; k < numKernelPositions; k++) {
        const auto size = (contextPartition[i][j].xEnd -
                           contextPartition[i][j].xBegin + strideX - 1) /
                          strideX;
        workList[index].push_back(size - 1);
      }
      index++;
    }
    startPos.push_back(index);
  }

  unsigned outSize = product(tilePartition.field) * tilePartition.batch;
  const auto chansPerGroupD =
      tilePartition.chansPerGroup / (params.inputType == HALF ? 4 : 2);
  // Amount of data the tile has to initialise
  auto initInfo = outSize / chansPerGroupD;

  const auto usingBwdPassVertex = (poolCfg.type == PoolingType::MAX) &&
                                  (poolCfg.pass == PoolPass::POOL_BWD);

  return getPoolingCycles(initInfo, chansPerGroupD,
                          tilePartition.chanGroups - 1, startPos, workList,
                          boost::none, poolCfg.type == PoolingType::MAX,
                          usingBwdPassVertex, numContexts, true);
}

} // namespace pooling
} // namespace popnn
