// Copyright (c) 2020 Graphcore Ltd. All rights reserved.
#include "popsparseCycleEstimators.hpp"
#include "PerformanceEstimation.hpp"

using namespace poplar;

namespace popsparse {

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(SparseDenseMatMulElementWise)(
    const VertexIntrospector &vertex, const Target &target, const Type &fpType,
    const Type &accumType) {
  return 0;
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(SparseDenseMatMulGradAElementWise)(
    const VertexIntrospector &vertex, const Target &target, const Type &fpType,
    const Type &accumType) {
  return 0;
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(SparseDenseMatMulElementWiseTranspose)(
    const VertexIntrospector &vertex, const Target &target, const Type &fpType,
    const Type &accumType) {
  return 0;
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(SparseDenseMatMulGradWElementWise)(
    const VertexIntrospector &vertex, const Target &target, const Type &fpType,
    const Type &accumType) {
  return 0;
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(SparseGatherElementWise)(
    const VertexIntrospector &vertex, const Target &target,
    const Type &fpType) {
  const auto numWorkers = target.getNumWorkerContexts();
  CODELET_SCALAR_VAL(numIndices, unsigned);
  CODELET_SCALAR_VAL(workerOffsets, unsigned);
  const unsigned numBits = fpType == HALF ? 2 : 1;
  auto remainder = numIndices & ((1 << numBits) - 1);
  auto numVectors = (numIndices >> numBits) * numWorkers;

  // auto offsets = workerOffsets;
  for (unsigned i = 0, offsets = workerOffsets; i != numWorkers;
       ++i, offsets >>= 1) {
    numVectors += (offsets & 0x1);
  }

  return sparseGatherElementWiseCycles((numVectors << numBits) + remainder,
                                       numWorkers, fpType == FLOAT);
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(BufferIndexUpdate)(
    const VertexIntrospector &vertex, const Target &target, const Type &type) {
  return 6 * target.getNumWorkerContexts();
}

poplibs::CycleEstimatorTable makeCyclesFunctionTable() {
  return {
      CYCLE_ESTIMATOR_ENTRY(popsparse, SparseDenseMatMulElementWise, HALF,
                            FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popsparse, SparseDenseMatMulElementWise, FLOAT,
                            FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popsparse, SparseDenseMatMulElementWiseTranspose,
                            HALF, FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popsparse, SparseDenseMatMulElementWiseTranspose,
                            FLOAT, FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popsparse, SparseDenseMatMulGradWElementWise, HALF,
                            FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popsparse, SparseDenseMatMulGradWElementWise, FLOAT,
                            FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popsparse, SparseDenseMatMulGradAElementWise, HALF,
                            FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popsparse, SparseDenseMatMulGradAElementWise, FLOAT,
                            FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popsparse, SparseGatherElementWise, HALF),
      CYCLE_ESTIMATOR_ENTRY(popsparse, SparseGatherElementWise, FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popsparse, BufferIndexUpdate, UNSIGNED_INT),

  };
}

} // end namespace popsparse
