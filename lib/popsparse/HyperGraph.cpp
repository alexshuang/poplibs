// Copyright (c) 2020 Graphcore Ltd. All rights reserved.

#include "HyperGraph.hpp"
#include "BSOps.hpp"
#include "BSUtils.hpp"
#include <popops/ElementWise.hpp>
#include <poputil/VertexTemplates.hpp>
#include <poputil/exceptions.hpp>

namespace popsparse {
namespace experimental {

void HyperGraph::addConv1x1Vertex(poplar::Graph &graph,
                                  const std::vector<poplar::Tensor> &lhs,
                                  const std::vector<poplar::Tensor> &rhs,
                                  const poplar::Tensor &output,
                                  unsigned int tileId,
                                  poplar::ComputeSet &mulCS,
                                  const std::string &debugPrefix) {
  const unsigned convInChannels = (inDataType == poplar::FLOAT ? 8 : 16);
  assert(convInChannels == static_cast<unsigned int>(matB.getBlockRow()));

  const auto convOutChannels = matB.getBlockCol();
  const auto outElementTypeSize = outDataType == poplar::HALF ? 2 : 4;
  const auto inElementTypeSize = inDataType == poplar::HALF ? 2 : 4;
  assert((convOutChannels * outElementTypeSize) % 8 == 0);

  if (!worklistTensor.valid()) {
    int nWorker = graph.getTarget().getNumWorkerContexts();
    if (nWorker != 6) {
      throw poputil::poplibs_error("Error: the number of IPU work contexts is "
                                   "NOT 6");
      return;
    }

    // Here we use term "batchSize" more general just for the lack of better
    // term Number of rows in matrix A == batchSize if we use matmut to multiply
    // input by weights for forward pass
    int batchSize = matA.getBlockRow();

    int workerSize = batchSize / nWorker;
    int leftover = batchSize % nWorker;
    std::vector<int> worklist(nWorker * 3);
    int offset = 0;

    // Distrubute batchSize between workers as even as possible
    for (int i = 0; i < 6; ++i) {
      worklist[i * 3] = (offset * convOutChannels * outElementTypeSize) / 8;
      int curWorkerSize = (i < leftover ? workerSize + 1 : workerSize);
      // The ConvPartial1x1Out vertex expects worklists[3i+1] to be the number
      // of elements minus 3.
      worklist[i * 3 + 1] = curWorkerSize - 3;
      worklist[i * 3 + 2] = (offset * convInChannels * inElementTypeSize) / 8;
      offset += curWorkerSize;
    }

    worklistTensor = graph.addConstant(
        poplar::UNSIGNED_SHORT, {static_cast<unsigned long>(nWorker * 3)},
        worklist.data(), debugPrefix + "/worklists");
    graph.setTileMapping(worklistTensor, getRandomTile());
  }

  assert(rhs.size() == lhs.size());

  const unsigned numInGroups = matA.getBlockCol() / convInChannels;
  std::vector<poplar::Tensor> inputA, inputB;
  for (unsigned i = 0; i < rhs.size(); i++) {
    for (unsigned g = 0; g < numInGroups; g++) {
      inputA.push_back(lhs[i][g]);
      inputB.push_back(rhs[i][g]);
    }
  }

  std::vector<poplar::Tensor> out;
  out.push_back(output);

  poplar::VertexRef v = graph.addVertex(
      mulCS, poputil::templateVertex("poplin::ConvPartial1x1Out", inDataType,
                                     partialDataType, "true", "false", 8));

  graph.connect(v["in"], inputA);
  graph.connect(v["out"], out);
  graph.connect(v["weights"], inputB);
  graph.connect(v["worklists"], worklistTensor);

  graph.setInitialValue(v["outChansPerGroup"], convOutChannels);
  graph.setInitialValue(v["inChansPerGroup"], convInChannels);
  graph.setInitialValue(v["numOutGroupsM1"], 0);
  graph.setInitialValue(v["numInGroups"], inputA.size());
  graph.setInitialValue(v["transformedInStride"], 1);
  graph.setInitialValue(v["numConvGroupsM1"], 0);
  if (partialDataType == poplar::FLOAT)
    graph.setInitialValue(v["transformedOutStride"], convOutChannels - 6);
  else
    graph.setInitialValue(v["transformedOutStride"], convOutChannels - 4);
  graph.setTileMapping(v, tileId);
}

void HyperGraph::addReduceVertex(
    poplar::Graph &graph, const std::vector<poplar::Tensor> &partialBlocks,
    poplar::Tensor &output, unsigned int tileId, poplar::ComputeSet &reduceCS) {
  int nWorker = graph.getTarget().getNumWorkerContexts();
  if (nWorker != 6) {
    throw poputil::poplibs_error("Error: The number of IPU work context is "
                                 "NOT 6");
    return;
  }

  // how many partials we can fit in 128-bits: 4 is for FLOAT, 8 for HALF
  const unsigned numValsIn128 = outDataType == poplar::FLOAT ? 4 : 8;
  int blockSize = matC->getBlockRow() * matC->getBlockCol();
  if (blockSize % numValsIn128 != 0) {
    throw poputil::poplibs_error(
        "Error: the size of block in output matrix should be divisible by " +
        std::to_string(numValsIn128));
  }
  int grains = blockSize / numValsIn128;
  int grainsPerWorker = grains / nWorker;
  int grainsLeftover = grains % nWorker;

  for (int w = 0, offset = 0; w < nWorker; w++) {
    int curGrains =
        (w < grainsLeftover) ? (grainsPerWorker + 1) : grainsPerWorker;
    int curElements = curGrains * numValsIn128;
    std::vector<poplar::Tensor> inputOneWorker;
    for (const auto &b : partialBlocks) {
      inputOneWorker.push_back(b.slice(offset, offset + curElements));
    }
    auto v = graph.addVertex(
        reduceCS,
        poputil::templateVertex(
            "popops::Reduce", "popops::ReduceAdd", partialDataType, outDataType,
            "false", "popops::ReductionSpecialisation::STRIDED_REDUCE"));
    graph.connect(v["out"], output.slice(offset, offset + curElements));
    graph.setInitialValue(v["numOutputs"], curElements);
    graph.connect(v["partials"], concat(inputOneWorker));
    graph.setInitialValue(v["numPartialsM1"],
                          concat(inputOneWorker).numElements() / curElements -
                              1);
    graph.setInitialValue(v["partialsWidth"], curElements);
    graph.setTileMapping(v, tileId);

    offset += curElements;
  }
}

void HyperGraph::applySubBlockMask(poplar::Graph &graph,
                                   SubBlockMask subBlockMask,
                                   poplar::program::Sequence &prog,
                                   const std::string &debugPrefix) {
  const unsigned blockRowC = static_cast<unsigned>(matC->getBlockRow());
  const unsigned blockColC = static_cast<unsigned>(matC->getBlockCol());
  const unsigned rowC = static_cast<unsigned>(matC->getRowCount());
  const unsigned colC = static_cast<unsigned>(matC->getColCount());
  const unsigned nRowC = rowC / blockRowC;
  const unsigned nColC = colC / blockColC;

  std::vector<poplar::Tensor> matcBlocks = matC->getBlockTensor();
  if (matcBlocks.empty()) {
    return;
  }
  for (auto &matcBlock : matcBlocks) {
    matcBlock = matcBlock.expand({0});
  }
  poplar::Tensor matcBsFormat = poplar::concat(matcBlocks);
  assert(matcBsFormat.rank() == 2);

  std::vector<unsigned char> sparsity(nRowC * nColC, 0);
  auto blockIdMatrixC = matC->getBlockIdMatrix();
  for (unsigned r = 0; r < nRowC; r++) {
    for (unsigned c = 0; c < nColC; c++) {
      if (blockIdMatrixC[r][c] != -1) {
        sparsity[r * nColC + c] = 1;
      }
    }
  }
  popsparse::experimental::applySubBlockMask(
      graph, matcBsFormat, subBlockMask, blockRowC, blockColC, nRowC, nColC,
      sparsity.data(), 1, prog, debugPrefix);
}

poplar::Tensor HyperGraph::getResultTensor() const {
  if (matC->isDense()) {
    return static_cast<BlockDenseMatrix *>(matC.get())->denseMatrix;
  } else {
    std::vector<poplar::Tensor> blocks;
    for (auto &b : matC->getBlockTensor()) {
      blocks.push_back(b.expand({0}));
    }
    return concat(blocks);
  }
}

void HyperGraph::getResultBlockSize(int &blockRow, int &blockCol) const {
  blockRow = matC->getBlockRow();
  blockCol = matC->getBlockCol();
}

void HyperGraph::getResultBlockCount(int &blockRowCount,
                                     int &blockColCount) const {
  blockRowCount = matC->getBlockRowCount();
  blockColCount = matC->getBlockColCount();
}

void HyperGraph::preprocessBlocks(poplar::Graph &graph, const BlockMatrix &lhs,
                                  const BlockMatrix &rhs,
                                  std::vector<poplar::Tensor> &lhsBlocks,
                                  std::vector<poplar::Tensor> &rhsBlocks,
                                  const std::vector<int> &rhsTileAssignment,
                                  poplar::ComputeSet *transposeCS,
                                  const std::string &debugPrefix) {

  const unsigned inChansPerGroup = (inDataType == poplar::FLOAT ? 8 : 16);
  if (lhs.getBlockCol() % inChansPerGroup != 0) {
    throw poputil::poplibs_error(
        "Error: The column block size of left hand" + std::string(" matrix(") +
        std::to_string(matA.getBlockCol()) + ") is NOT divisible by " +
        std::to_string(inChansPerGroup));
  }
  const unsigned numInGroups = lhs.getBlockCol() / inChansPerGroup;

  const std::vector<poplar::Tensor> lhsInBlocks = lhs.getBlockTensor();
  unsigned long lhsBlockRow = static_cast<unsigned long>(lhs.getBlockRow());
  unsigned long lhsBlockCol = static_cast<unsigned long>(lhs.getBlockCol());
  for (unsigned i = 0; i < lhsInBlocks.size(); i++) {
    // split into small groups
    poplar::Tensor reshaped =
        lhsInBlocks[i].reshape({lhsBlockRow, lhsBlockCol});
    std::vector<poplar::Tensor> smallBlocks;
    std::size_t start = 0, end = 0;
    for (unsigned g = 0; g < numInGroups; g++) {
      end += inChansPerGroup;
      smallBlocks.push_back(
          reshaped.slice({0, start}, {lhsBlockRow, end}).flatten().expand({0}));
      start = end;
    }

    lhsBlocks.push_back(concat(smallBlocks));
  }

  const std::vector<poplar::Tensor> rhsInBlocks = rhs.getBlockTensor();
  const std::vector<std::vector<int>> rhsBlockIdMatrix = rhs.getBlockIdMatrix();
  unsigned long rhsBlockRow = static_cast<unsigned long>(rhs.getBlockRow());
  unsigned long rhsBlockCol = static_cast<unsigned long>(rhs.getBlockCol());
  int rhsRow = rhs.getBlockRowCount();
  int rhsCol = rhs.getBlockColCount();

  rhsBlocks.resize(rhs.getNonZeroBlockCount());
  for (int c = 0; c < rhsCol; c++) {
    for (int r = 0; r < rhsRow; r++) {
      int blockId = rhsBlockIdMatrix[r][c];
      if (blockId == -1) {
        continue;
      }
      unsigned int tileId = rhsTileAssignment[blockId];

      // split into small groups
      std::vector<poplar::Tensor> smallBlocks;
      std::size_t start = 0, end = 0;
      for (unsigned g = 0; g < numInGroups; g++) {
        end += inChansPerGroup;
        if (rhs.getNeedTranspose()) {
          // rhsBlockCol and rhsBlockRow is the dimension after transposed.
          poplar::Tensor reshaped =
              rhsInBlocks[blockId].reshape({rhsBlockCol, rhsBlockRow});
          poplar::Tensor oneSlice =
              reshaped.slice({0, start}, {rhsBlockCol, end}).flatten();
          smallBlocks.push_back(oneSlice.expand({0}));
        } else {
          assert(transposeCS);
          // TODO:
          // [richard]
          // This creates one transpose vertex per block which is inefficient
          // both in terms of memory and cycles
          // - it would be better to group the blocks that need to be
          // transposed by tile and split the work between just enough
          // vertices to keep all workers busy. This logic already exists in
          // addTransposeVertices() in ConvUtil.cpp
          // - would it be possible to use this function instead of adding the
          // vertices here?
          poplar::Tensor reshaped =
              rhsInBlocks[blockId].reshape({rhsBlockRow, rhsBlockCol});
          poplar::Tensor oneSlice =
              reshaped.slice({start, 0}, {end, rhsBlockCol}).flatten();
          poplar::Tensor transposedSlice = graph.addVariable(
              inDataType,
              {static_cast<unsigned long>(rhsBlockRow * rhsBlockCol /
                                          numInGroups)},
              debugPrefix + "/transposed_block_" + std::to_string(blockId));
          std::vector<poplar::Tensor> src, dst;
          src.push_back(oneSlice);
          dst.push_back(transposedSlice);
          auto v = graph.addVertex(
              *transposeCS,
              poputil::templateVertex("popops::Transpose2d", inDataType));

          graph.connect(v["src"], src);
          graph.connect(v["dst"], dst);
          graph.setInitialValue(v["numSrcRows"], rhsBlockRow / numInGroups);
          graph.setInitialValue(v["numSrcColumns"], rhsBlockCol);
          graph.setTileMapping(v, tileId);
          graph.setTileMapping(transposedSlice, tileId);

          smallBlocks.push_back(transposedSlice.expand({0}));
        }

        start = end;
      }

      rhsBlocks[blockId] = concat(smallBlocks);
    }
  }
}

} // namespace experimental
} // namespace popsparse