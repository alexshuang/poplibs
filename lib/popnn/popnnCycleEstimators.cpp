// Copyright (c) 2017 Graphcore Ltd. All rights reserved.
#include "popnnCycleEstimators.hpp"

#include "PerformanceEstimation.hpp"
#include "PoolingDefUtil.hpp"
#include "popnn/NonLinearity.hpp"
#include "popnn/NonLinearityDefUtil.hpp"
#include "popnn/PoolingDef.hpp"

#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/algorithm/max_element.hpp>
#include <boost/range/irange.hpp>
#include <cassert>
#include <cmath>

using namespace poplar;

// Macro to create entries in cycle estimator table
#define INSTANTIATE_NL_CYCLE_ESTIMATOR(v)                                      \
  CYCLE_ESTIMATOR_ENTRY(popnn, v, FLOAT, popnn::NonLinearityType::SIGMOID),    \
      CYCLE_ESTIMATOR_ENTRY(popnn, v, HALF, popnn::NonLinearityType::SIGMOID), \
      CYCLE_ESTIMATOR_ENTRY(popnn, v, FLOAT, popnn::NonLinearityType::RELU),   \
      CYCLE_ESTIMATOR_ENTRY(popnn, v, HALF, popnn::NonLinearityType::RELU),    \
      CYCLE_ESTIMATOR_ENTRY(popnn, v, FLOAT, popnn::NonLinearityType::TANH),   \
      CYCLE_ESTIMATOR_ENTRY(popnn, v, HALF, popnn::NonLinearityType::TANH),    \
      CYCLE_ESTIMATOR_ENTRY(popnn, v, FLOAT, popnn::NonLinearityType::GELU),   \
      CYCLE_ESTIMATOR_ENTRY(popnn, v, HALF, popnn::NonLinearityType::GELU)
namespace popnn {

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(NonLinearitySupervisor)(
    const VertexIntrospector &vertex, const Target &target, const Type &type,
    const NonLinearityType &nlType) {
  bool isFloat = type == FLOAT;
  CODELET_FIELD(data);
  const auto numWorkers = target.getNumWorkerContexts();
  const auto vectorWidth = target.getVectorWidth(type);
  const auto n = data.size();

  const auto numVectors = n / vectorWidth;
  const auto remainder = n % vectorWidth;

  // If any worker handles an extra vector due to the remainder
  // we take the longest worker hence rounded up.
  const auto vectorsPerWorker = (numVectors + numWorkers - 1) / numWorkers;

  // We do 2 ops per vector.
  const auto opCycles = getNonLinearityOpCycles(nlType, isFloat);
  const auto vectorLoopCycles = opCycles * 2;

  // These cycle estimates follow the aligned path. Slightly optimistic.
  // The cost of misalignment is ~9 cycles for half, less for float.
  std::uint64_t cycles = 9; // Supervisor vertex overhead
  std::uint64_t workerCycles =
      2 + // Load input pointer and size
      5 + // Divide & Remainder to split work between workers
      2 + // Get worker ID
      2 + // Check 64-bit aligned and branch
      5 + // Setup remainders and size for worker
      2 + // Offset worker's pointer and branch if done
      (vectorsPerWorker ? 1 : 0) *
          (2 + opCycles + // Warm up pipeline, rpt
           (vectorsPerWorker - 1) * vectorLoopCycles + 1 +
           opCycles); // Handle remaining element from pipeline

  // possibly unpack pointers
  workerCycles += poplibs::getUnpackCost(data.getProfilerVectorLayout(0));

  // Add remainder handling cycles. This handling could be slightly overlapped
  // with other workers if the worker doing the remainder had less vector
  // work than the others. Some of these transcendental ops may take
  // less time anyway so we'll just stick with the simpler estimation.
  if (isFloat) {
    workerCycles +=
        3 + // Test worker ID to handle remainder, test remainder, branch
        ((remainder & 1) ? 1 : 0) * (2 + opCycles); // Handle 32-bit remainder
  } else {
    workerCycles +=
        2 + // Test worker ID to handle remainder with
        1 + // branch for 32-bit remainder
        ((remainder & 2) ? 1 : 0) * (2 + opCycles) + // Handle 32-bit remainder
        1 + // branch for 16-bit remainder
        ((remainder & 1) ? 1 : 0) * (3 + opCycles); // Handle 16-bit remainder
  }

  return cycles + (workerCycles * numWorkers);
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(NonLinearityGradSupervisor)(
    const VertexIntrospector &vertex, const Target &target, const Type &type,
    const NonLinearityType &nlType) {
  bool isFloat = type == FLOAT;
  const auto vectorWidth = target.getVectorWidth(type);
  const auto numWorkers = target.getNumWorkerContexts();
  CODELET_FIELD(inGrad);
  CODELET_FIELD(outGrad);
  CODELET_FIELD(out);
  const auto n = inGrad.size();
  assert(outGrad.size() == n);
  assert(out.size() == n);

  const auto inGradLayout = inGrad.getProfilerVectorLayout(0);
  assert(inGradLayout == outGrad.getProfilerVectorLayout(0) &&
         inGradLayout == out.getProfilerVectorLayout(0));

  const auto numVectors = n / vectorWidth;
  const auto remainder = n % vectorWidth;
  const auto vectorsPerWorker = (numVectors + numWorkers - 1) / numWorkers;

  std::uint64_t cycles = 9; // Supervisor vertex overhead
  std::uint64_t workerCycles =
      3 + // Load vertex state
      5 + // Split work between workers
      2 + // Get worker ID
      3 + // Add remaining vectors to relevant workers
      3 + // Offset pointers to data
      3 + // Pre-load inputs, and generate ones if needed
      1 + // Branch if no vectors
      (vectorsPerWorker ? 1 : 0) *
          (4 +                              // Warm up the pipeline
           (vectorsPerWorker - 1) * 3 + 1); // Store remaining element

  // get real pointers from scaled pointers
  if (inGradLayout == layout::Vector::ScaledPtr64) {
    workerCycles += poplibs::getUnpackCost(inGradLayout) + 2;
  }

  if (isFloat) {
    workerCycles += 2 + // Pick a worker to handle the remainder, branch
                    2 + // Check for remainder
                    (remainder ? 1 : 0) * 4;
  } else {
    workerCycles += 2 + // Pick a worker to handle remainders, branch
                    2 + // Check for 32-bit remainder
                    ((remainder & 2) ? 1 : 0) * 5 +
                    2 + // Check for 16-bit remainder
                    ((remainder & 1) ? 1 : 0) * 7;
  }

  return cycles + (workerCycles * numWorkers);
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(NonLinearity2D)(
    const VertexIntrospector &vertex, const Target &target, const Type &type,
    const NonLinearityType &nlType) {
  bool isFloat = type == FLOAT;
  const auto vectorWidth = target.getVectorWidth(type);
  CODELET_FIELD(data);
  const auto n0 = data.size();
  assert(n0 > 0);

  std::uint64_t cycles = 5; // Vertex overhead

  // We do 2 ops per vector.
  const auto opCycles = getNonLinearityOpCycles(nlType, isFloat);
  const auto vectorLoopCycles = opCycles * 2;

  cycles += 2 + // Load base pointer, DeltaN pointer
            5 + // Unpack base pointer, n0, DeltaN pointer
            2;  // Set mask for inner loop, sub for brnzdec

  // Following 64-bit aligned path
  for (std::size_t i = 0; i < n0; ++i) {
    const auto n1 = data[i].size();
    const auto numVectors = n1 / vectorWidth;
    const auto remainder = n1 % vectorWidth;

    cycles += 4 +                 // Load DeltaN, calculate inner pointer and n1
              (isFloat ? 0 : 2) + // Test 32-bit aligned
              2 +                 // Test 64-bit aligned
              2 +                 // Shift to get num vectors, branch if 0
              (numVectors ? 1 : 0) * (2 + opCycles + // Warm up pipeline
                                      (numVectors - 1) * vectorLoopCycles + 1 +
                                      opCycles); // Handle last element

    if (isFloat) {
      cycles += 2 + // Check for remainder, branch
                (remainder ? 1 : 0) * (2 + opCycles);
    } else {
      cycles += 2 + // Check for 32-bit remainder, branch
                ((remainder & 2) ? 1 : 0) * (2 + opCycles) +
                2 + // Check for 16-bit remainder, branch
                ((remainder & 1) ? 1 : 0) * (3 + opCycles);
    }

    cycles += 1; // brnzdec
  }

  return cycles;
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(NonLinearityGrad2D)(
    const VertexIntrospector &vertex, const Target &target, const Type &type,
    const NonLinearityType &nlType) {
  bool isFloat = type == FLOAT;
  const auto vectorWidth = target.getVectorWidth(type);
  CODELET_FIELD(inGrad);
  CODELET_FIELD(outGrad);
  CODELET_FIELD(out);
  const auto n0 = inGrad.size();
  assert(outGrad.size() == n0);
  assert(out.size() == n0);
  assert(n0 > 0);

  std::uint64_t cycles = 5; // Vertex overhead

  cycles += 4 + // Load vertex state
            3 + // Load DeltaN base/n0, generate ones if needed
            3 + // Calculate DeltaN pointer
            2;  // Set mask for inner loop, sub for brnzdec

  for (std::size_t i = 0; i < n0; ++i) {
    const auto n1 = inGrad[i].size();
    assert(outGrad[i].size() == n1);
    assert(out[i].size() == n1);
    const auto numVectors = n1 / vectorWidth;
    const auto remainder = n1 % vectorWidth;

    cycles +=
        6 + // Load DeltaN, calculate inner pointer/n1, shift for n1 vecs
        3 + // Pre-load inputs for pipeline, branch if 0
        (numVectors ? 1 : 0) * (4 +                        // Warm up pipeline
                                (numVectors - 1) * 3 + 1); // Store last element

    if (isFloat) {
      cycles += 2 + // Check for remainder
                (remainder ? 1 : 0) * 4;
    } else {
      cycles += 2 + // Check for 32-bit remainder
                ((remainder & 2) ? 1 : 0) * 5 +
                2 + // Check for 16-bit remainder
                ((remainder & 1) ? 1 : 0) * 7;
    }

    cycles += 1; // brnzdec
  }

  return cycles;
}

std::uint64_t poolingCycleEstimator(const VertexIntrospector &vertex,
                                    const Target &target,
                                    const PoolingType &pType,
                                    const bool isBwdPass) {
  CODELET_SCALAR_VAL(initInfo, unsigned short);
  CODELET_SCALAR_VAL(chansPerGroupD, unsigned short);
  CODELET_SCALAR_VAL(numChanGroupsM1, unsigned short);
  CODELET_VECTOR_VALS(startPos, unsigned short);
  CODELET_VECTOR_2D_VALS(workList, unsigned short);
  CODELET_FIELD(out);
  CODELET_FIELD(in);

  const auto numWorkers = target.getNumWorkerContexts();

  const auto outLayout = out.getProfilerVectorLayout(0);
  const auto inLayout = in.getProfilerVectorLayout(0);

  const auto fwdOutLayout = outLayout;
  const auto fwdInLayout = inLayout;

  const auto outInnerLayout = out.getProfilerVectorLayout(1);
  const auto inInnerLayout = in.getProfilerVectorLayout(1);

  const auto startPosLayout =
      vertex.getFieldInfo("startPos").getProfilerVectorLayout(0);
  const auto workListLayout =
      vertex.getFieldInfo("workList").getProfilerVectorListLayout();

  // per-worker cycles
  const auto workerCycles = [&](unsigned wId) {
    std::uint64_t cycles = 4   // load vertex state
                           + 1 // scale initInfo
                           + 2 // get $WSR and load identity
                           + 7 // divide init work
        ;
    // maybe unpack outPtrPtr
    cycles += poplibs::getUnpackCost(outLayout);

    // calculate how much initialisation each worker does.
    const auto initElems = [&] {
      const unsigned numElems = initInfo * chansPerGroupD;
      const unsigned extra = wId < (initInfo - numElems * numWorkers);

      return (numElems + extra) * 8;
    }();
    // init loop overhead, number of rpt loop cycles, number of brnzdec cycles.
    cycles += (2 + initElems) * numChanGroupsM1;

    cycles += 5   // load startPosPtr, numRows and startPos
              + 1 // bnz numRows
        ;

    // maybe unpack outPtr and startPosPtr
    cycles += poplibs::getUnpackCost(outInnerLayout);
    cycles += poplibs::getUnpackCost(startPosLayout);

    // if numRows is zero this worker is done.
    const unsigned numRows =
        wId == 0 ? startPos[0] : startPos[wId] - startPos[wId - 1];
    if (numRows == 0) {
      return cycles + 1; // exitz
    }

    cycles +=
        2 // save startPos, load inPtrPtr and workListBase
        +
        (pType == PoolingType::MAX ? 1 : 2) // unpack inPtrPtr, maybe load scale
        + poplibs::getUnpackCost(inInnerLayout);

    // load and (possibly) unpack acts pointer pointers
    if (isBwdPass) {
      cycles += 6 + poplibs::getUnpackCost(outLayout) +
                poplibs::getUnpackCost(inLayout);
    }

    cycles += 2   // unpack workListBase
              + 1 // decrement numRows
        ;

    for (unsigned row = 0; row < numRows; ++row) {
      cycles +=
          13 + poplibs::getUnpackCost(workListLayout); // row_loop overhead

      const unsigned sPos = wId == 0 ? 0 : startPos[wId - 1];
      const unsigned numWorkItems = workList[sPos + row].size();
      for (unsigned w = 0; w < numWorkItems; w += 3) {
        cycles += 20; // work_loop overhead
        for (unsigned cg = 0; cg < numChanGroupsM1 + 1u; ++cg) {
          cycles += 2 // reload outPos and inPos
                    + poplibs::getUnpackCost(outLayout) +
                    poplibs::getUnpackCost(inLayout) +
                    2   // reload chansPerGroupD, decrement it
                    + 4 // move pointers on by outPos and inPos
              ;

          if (isBwdPass) {
            cycles += poplibs::getUnpackCost(outInnerLayout) +
                      poplibs::getUnpackCost(inInnerLayout) +
                      poplibs::getUnpackCost(fwdInLayout) +
                      poplibs::getUnpackCost(fwdOutLayout) +
                      4 // move pointers on by outPos and inPos
                ;
          }

          for (unsigned c = 0; c < chansPerGroupD; ++c) {
            // rpt loop cycles.
            const auto rptCycles = [&] {
              // numElementsM1, aka the rpt count
              const unsigned n = workList[sPos + row][w + 2];

              if (isBwdPass) {
                return 7 + 5 * n;
              } else if (pType == PoolingType::MAX) {
                return 4 + 3 * n;
              } else {
                return 5 + 3 * n;
              }
            }();

            cycles += 2           // chans_per_group_loop overhead
                      + rptCycles // innermost loop
                      + 1         // brnzdec chansPerGroupD
                ;
          }
          ++cycles; // brnzdec numChanGroupsM1
        }
        cycles += 3; // reload, decrement and brnz numWorkItems
      }
      cycles += 2; // reload numRows and brnzdec
    }
    return cycles + 1; // exitz
  };

  // calculate how long each worker take
  std::vector<std::uint64_t> allWorkerCycles;
  for (unsigned wId = 0; wId < numWorkers; ++wId) {
    allWorkerCycles.push_back(workerCycles(wId));
  }

  return 7                                          // supervisor overhead
         + *boost::max_element(allWorkerCycles) * 6 // longest worker
         + 6                                        // br $lr
      ;
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(MaxPooling)(const VertexIntrospector &vertex,
                                      const Target &target, const Type &type) {
  (void)type;
  return poolingCycleEstimator(vertex, target, PoolingType::MAX, false);
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(MaxPoolingGradientScale)(
    const VertexIntrospector &vertex, const Target &target, const Type &type) {
  (void)type;
  return poolingCycleEstimator(vertex, target, PoolingType::MAX, false);
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(SumPooling)(const VertexIntrospector &vertex,
                                      const Target &target, const Type &type) {
  (void)type;
  return poolingCycleEstimator(vertex, target, PoolingType::SUM, false);
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(SelectiveScaling)(
    const VertexIntrospector &vertex, const Target &target, const Type &type) {
  // TODO: T5436 Improve this estimate.
  return 10;
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(MaxPoolingGrad)(
    const VertexIntrospector &vertex, const Target &target, const Type &type) {
  (void)type;
  return poolingCycleEstimator(vertex, target, PoolingType::MAX, true);
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(LossSumSquaredTransform)(
    const VertexIntrospector &vertex, const Target &target,
    const Type &fpType) {
  const bool isFloat = fpType == FLOAT;
  const auto size = vertex.getFieldInfo("probs").size();
  const auto isSoftmax = false;
  return getLossTransformCycles(isFloat, isSoftmax, size);
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(LossCrossEntropyTransform)(
    const VertexIntrospector &vertex, const Target &target,
    const Type &fpType) {
  const bool isFloat = fpType == FLOAT;
  const auto size = vertex.getFieldInfo("probs").size();
  const auto isSoftmax = true;
  return getLossTransformCycles(isFloat, isSoftmax, size);
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(ReduceMaxClassGather)(
    const VertexIntrospector &vertex, const Target &target, const Type &inType,
    const Type &labelType) {
  std::uint64_t supervisorCycles = 5 + // Vertex overhead
                                   4;  // Supervisor call + sync
  CODELET_FIELD(activations);
  CODELET_SCALAR_VAL(size, unsigned);
  CODELET_SCALAR_VAL(workerSize, unsigned);
  const auto numWorkers = target.getNumWorkerContexts();
  // Check the divisor chosen is large enough to process all inputs
  // with the target number of workers and the grain size.
  assert(workerSize * numWorkers >= size);
  std::uint64_t cycles;
  if (inType == FLOAT || inType == HALF) {
    // Assembly, supervisor implementation
    // Size is the size of the whole tensor, divisor indicates the region an
    // individual worker operates on.  So each worker does divisor inner loop
    // passes unless size is small, in which case one worker does size inner
    // loop passes, and all the others do nothing.
    cycles = 3 + // Load acts pointer, size, divisor
             2 + // Get worker ID
             4 + // Calculate the worker's region
             3 + // Calculate N, sub 1 for first element, branch if no work.
             1 + // Offset pointer for worker
             3 + // Load first element as max, setup pointers
             1 + // rpt
             std::min(workerSize - 1, size - 1) * 3 +
             3 + // Handle remaining element from loop
             6 + // Calculate max index from max act pointer
             4;  // Load maxValue/maxIndex pointers, store (+f16->f32 for half)
  } else {
    // Compiled, 1 worker (pseudo supervisor) version for other types
    const auto nOutputs = (size + workerSize - 1) / workerSize;
    cycles = 22 +                                // Net overhead
             nOutputs * ((workerSize * 6) + 25); // Inner, outer loop overhead
  }
  return cycles * numWorkers + supervisorCycles;
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(ReduceMaxNClassGather)(
    const VertexIntrospector &vertex, const Target &target, const Type &fpType,
    const bool sorted) {
  CODELET_FIELD(activations);
  CODELET_SCALAR_VAL(divisorLog2, unsigned short);
  CODELET_SCALAR_VAL(numK, unsigned short);

#ifndef NDEBUG
  const auto numWorkers = target.getNumWorkerContexts();
#endif
  const auto divisor = (1u << divisorLog2);
  // Check the divisor chosen is large enough to process all inputs
  // with the target number of workers and the grain size.
  assert(divisor * numWorkers >= activations.size());

  const auto nOutputs = (activations.size() + divisor - 1) / divisor;

  std::uint64_t cycles = 10 + // Initial set up.
                         2;   // Enter nOutputsloop.

  // Gather is assumed to have (roughly) the same cycles as sparse but in a
  // loop. It also doesn't benefit from compile time optimizations.
  for (unsigned i = 0; i < nOutputs; ++i) {
    cycles += 23; // Rough estimate for the first add.

    // For the first K we have a guaranteed push op.
    for (unsigned i = 1; i < numK; ++i) {
      cycles += 13 +              // Setup
                std::log(i) * 20; // log(i) loop.
    }

    // Assumes the worst case. This would be the case of activations being
    // sorted in assending order.
    cycles += (activations.size() - numK) * (13 + std::log(numK) * 20);

    // As we are working on the indices we do a bit at the end to store the
    // actual values as well and transform the indices.
    cycles += 8 * numK;

    if (sorted) {
      for (int i = numK; i >= 1; --i) {
        cycles += 10;               // Setup.
        cycles += 20 * std::log(i); // log(k-1) pop operation.
      }
    }
  }

  return cycles;
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(ReduceMaxNClassSparse)(
    const VertexIntrospector &vertex, const Target &target, const Type &type,
    const bool sorted) {
  CODELET_SCALAR_VAL(numK, unsigned short);
  CODELET_FIELD(activations);

  std::uint64_t cycles = 10 + // Initial set up.
                         2;   // Enter N loop.

  cycles += 23; // Rough estimate for the first add.

  // For the first K we have a guaranteed push op.
  for (int i = 1; i < numK; ++i) {
    cycles += 13 +              // Setup
              std::log(i) * 20; // log(i) loop.
  }

  // Assumes the worst case. This would be the case of activations being
  // sorted in assending order.
  cycles += (activations.size() - numK) * (13 + std::log(numK) * 20);

  // As we are working on the indices we do a bit at the end to store the actual
  // values as well and transform the indices.
  cycles += 8 * numK;

  // Sorting is very expensive but even if requested by the user it will ony be
  // performed on the very last reduction.
  if (sorted) {
    for (int i = numK; i >= 1; --i) {
      cycles += 10;               // Setup.
      cycles += 20 * std::log(i); // log(k-1) pop operation.
    }
  }
  return cycles;
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(ReduceMaxClassSparse)(
    const VertexIntrospector &vertex, const Target &target,
    const Type &inOutType, const Type &labelType) {
  std::uint64_t cycles = 5; // Vertex overhead
  CODELET_FIELD(activations);
  CODELET_FIELD(labels);
  const auto numActs = activations.size();
  assert(numActs == labels.size());
  if (inOutType == HALF || inOutType == FLOAT) {
    // Assembly implementation
    cycles += 2 + // Load acts start/end pointer
              3 + // Calculate N, sub 1 for first element
              3 + // Load first element as max, setup pointers
              1 + // rpt
              (numActs - 1) * 3 + 3 + // Handle remaining element from loop
              6 + // Calculate max index from max act pointer
              4;  // Load maxValue/maxIndex pointers, store
  } else {
    // Compiled versions for other types
    cycles += 18 +         // Net overhead
              numActs * 6; // Loop cycles
  }
  return cycles;
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(ReduceMinClassGather)(
    const VertexIntrospector &vertex, const Target &target, const Type &inType,
    const Type &labelType) {
  std::uint64_t supervisorCycles = 5 + // Vertex overhead
                                   4;  // Supervisor call + sync
  CODELET_FIELD(activations);
  CODELET_SCALAR_VAL(size, unsigned);
  CODELET_SCALAR_VAL(workerSize, unsigned);
  const auto numWorkers = target.getNumWorkerContexts();
  // Check the divisor chosen is large enough to process all inputs
  // with the target number of workers and the grain size.
  assert(workerSize * numWorkers >= size);
  std::uint64_t cycles;
  if (inType == FLOAT || inType == HALF) {
    // Assembly, supervisor implementation
    // Size is the size of the whole tensor, divisor indicates the region an
    // individual worker operates on.  So each worker does divisor inner loop
    // passes unless size is small, in which case one worker does size inner
    // loop passes, and all the others do nothing.
    cycles = 3 + // Load acts pointer, size, divisor
             2 + // Get worker ID
             4 + // Calculate the worker's region
             3 + // Calculate N, sub 1 for first element, branch if no work.
             1 + // Offset pointer for worker
             3 + // Load first element as max, setup pointers
             1 + // rpt
             std::min(workerSize - 1, size - 1) * 3 +
             3 + // Handle remaining element from loop
             6 + // Calculate min index from min act pointer
             4;  // Load minValue/minIndex pointers, store (+f16->f32 for half)
  } else {
    // Compiled, 1 worker (pseudo supervisor) version for other types
    const auto nOutputs = (size + workerSize - 1) / workerSize;
    cycles = 22 +                                // Net overhead
             nOutputs * ((workerSize * 6) + 25); // Inner, outer loop overhead
  }
  return cycles * numWorkers + supervisorCycles;
}

std::uint64_t MAKE_CYCLE_ESTIMATOR_NAME(ReduceMinClassSparse)(
    const VertexIntrospector &vertex, const Target &target,
    const Type &inOutType, const Type &labelType) {
  std::uint64_t cycles = 5; // Vertex overhead
  CODELET_FIELD(activations);
  CODELET_FIELD(labels);
  const auto numActs = activations.size();
  assert(numActs == labels.size());
  if (inOutType == HALF || inOutType == FLOAT) {
    // Assembly implementation
    cycles += 2 + // Load acts start/end pointer
              3 + // Calculate N, sub 1 for first element
              3 + // Load first element as max, setup pointers
              1 + // rpt
              (numActs - 1) * 3 + 3 + // Handle remaining element from loop
              6 + // Calculate min index from min act pointer
              4;  // Load minValue/minIndex pointers, store
  } else {
    // Compiled version for other types
    cycles += 18 +         // Total overhead
              numActs * 6; // Loop cycles
  }
  return cycles;
}

std::uint64_t
MAKE_CYCLE_ESTIMATOR_NAME(CalcAccuracy)(const VertexIntrospector &vertex,
                                        const Target &target,
                                        const Type &labelType) {
  std::uint64_t cycles = 5; // Vertex overhead
  CODELET_FIELD(maxPerBatch);
  CODELET_FIELD(expected);
  const auto batchSize = maxPerBatch.size();
  assert(batchSize == expected.size());

  cycles += 4 + // Load maxPerBatch start/end, sub, shift for num elements
            2 + // Load expected and numCorrect pointer
            1 + // Load initial numCorrect value
            1;  // rpt

  cycles += batchSize * (2 + // Load maxPerBatch/expected
                         1 + // cmpeq
                         1); // add

  cycles += 1; // Store final numCorrect

  return cycles;
}

poplibs::CycleEstimatorTable makeCyclesFunctionTable() {
  return {
      CYCLE_ESTIMATOR_ENTRY(popnn, LossSumSquaredTransform, FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popnn, LossSumSquaredTransform, HALF),

      CYCLE_ESTIMATOR_ENTRY(popnn, LossCrossEntropyTransform, FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popnn, LossCrossEntropyTransform, HALF),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, FLOAT, UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, HALF, UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, INT, UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, UNSIGNED_INT,
                            UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, FLOAT, INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, HALF, INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, INT, INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassGather, UNSIGNED_INT, INT),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassSparse, FLOAT, UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassSparse, UNSIGNED_INT,
                            UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassSparse, UNSIGNED_INT, INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassSparse, FLOAT, INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassSparse, INT, UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxClassSparse, INT, INT),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassGather, FLOAT, false),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassGather, FLOAT, true),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassGather, HALF, false),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassGather, HALF, true),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassGather, INT, false),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassGather, INT, true),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassGather, UNSIGNED_INT, false),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassGather, UNSIGNED_INT, true),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassSparse, FLOAT, false),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassSparse, FLOAT, true),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassSparse, HALF, false),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassSparse, HALF, true),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassSparse, INT, false),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassSparse, INT, true),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassSparse, UNSIGNED_INT, false),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMaxNClassSparse, UNSIGNED_INT, true),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassGather, FLOAT, UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassGather, HALF, UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassGather, INT, UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassGather, UNSIGNED_INT,
                            UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassGather, FLOAT, INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassGather, HALF, INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassGather, INT, INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassGather, UNSIGNED_INT, INT),

      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassSparse, FLOAT, UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassSparse, INT, UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassSparse, UNSIGNED_INT,
                            UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassSparse, FLOAT, INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassSparse, UNSIGNED_INT, INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, ReduceMinClassSparse, INT, INT),

      CYCLE_ESTIMATOR_ENTRY(popnn, CalcAccuracy, UNSIGNED_INT),
      CYCLE_ESTIMATOR_ENTRY(popnn, CalcAccuracy, INT),

      CYCLE_ESTIMATOR_ENTRY(popnn, MaxPoolingGrad, FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popnn, MaxPoolingGrad, HALF),

      CYCLE_ESTIMATOR_ENTRY(popnn, SumPooling, FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popnn, SumPooling, HALF),

      CYCLE_ESTIMATOR_ENTRY(popnn, MaxPooling, FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popnn, MaxPooling, HALF),

      CYCLE_ESTIMATOR_ENTRY(popnn, MaxPoolingGradientScale, FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popnn, MaxPoolingGradientScale, HALF),

      CYCLE_ESTIMATOR_ENTRY(popnn, SelectiveScaling, FLOAT),
      CYCLE_ESTIMATOR_ENTRY(popnn, SelectiveScaling, HALF),

      INSTANTIATE_NL_CYCLE_ESTIMATOR(NonLinearityGradSupervisor),
      INSTANTIATE_NL_CYCLE_ESTIMATOR(NonLinearitySupervisor),
      INSTANTIATE_NL_CYCLE_ESTIMATOR(NonLinearityGrad2D),
      INSTANTIATE_NL_CYCLE_ESTIMATOR(NonLinearity2D)};
}

} // end namespace popnn
