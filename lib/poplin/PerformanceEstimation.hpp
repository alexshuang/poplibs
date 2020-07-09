// Copyright (c) 2016 Graphcore Ltd. All rights reserved.
#ifndef _performance_estimation_h_
#define _performance_estimation_h_

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

inline static std::uint64_t convHorizontalMacOverhead(bool floatActivations) {
  return floatActivations ? 58 : 63;
}

inline static std::uint64_t convNx1Overhead() { return 101; }

// Number of worker cycle savings if state retention is used.
// The first entry is the total savings and the second is
// because of retention of state related to input channel processing.
inline static std::pair<std::uint64_t, std::uint64_t>
conv1x1WorkerRetentionSavings(bool floatActivations, bool floatPartials) {
  if (floatActivations == false && floatPartials == true) {
    return std::make_pair(10, 2);
  } else {
    return std::make_pair(0, 0);
  }
}

inline static std::uint64_t
convnx1WorkerRetentionSavings(bool /*floatActivations */,
                              bool /*floatPartials */) {
  return 4;
}

inline static std::uint64_t zeroPartialsRetentionSavings(bool floatPartials) {
  return floatPartials ? 9 : 10;
}

inline std::uint64_t getDenseDotProductCycles(bool floatActivations,
                                              bool floatPartials,
                                              unsigned size) {
  const auto innerCycles = 1 + // rpt
                           2 + // loop wind down
                           3 + // sum with previous partials (load, acc, store)
                           1;  // branch

  // float activations and float partials
  if (floatActivations) {
    if ((size % 2) == 0)
      return innerCycles + size;
    else
      return innerCycles + (2 * size);
  }

  // half activations and float partials
  if (floatPartials) {
    if ((size % 4) == 0)
      return innerCycles + size / 4;
    else
      return innerCycles + size;
  }

  // half activations and half partials
  if ((size % 4) == 0) {
    const auto innerCyclesv4 =
        2 * (1 + 2) + // rpt + loop wind down(macros)
        1 +           // f2h conversion (packing) (1)
        3 +           // sum with previous partials (load, acc, store)
        1;            // branch
    return innerCyclesv4 + size / 4;
  } else {
    const auto innerCyclesv2 =
        2 +           // weights load
        2 * (1 + 2) + // rpt + loop wind down
        3 + // results combine, sum with previous partials (load, acc, store)
        1;  // branch
    return innerCyclesv2 + size;
  }
}

template <class InputIterator>
bool allEqual(InputIterator begin, InputIterator end) {
  if (begin == end)
    return true;
  const auto &first = *begin;
  for (auto it = begin + 1; it != end; ++it) {
    if (*it != first)
      return false;
  }
  return true;
}

inline std::uint64_t getConvPartialHorizontalMacCycleEstimate(
    bool floatActivations, bool floatPartials, unsigned numInChans,
    unsigned numOutChans, const std::vector<unsigned> &convSizes) {
  uint64_t cycles = 16;
  for (auto convSize : convSizes) {
    if (convSize == 0) {
      cycles += 7;
    } else {
      if (!floatPartials) {
        numOutChans /= 2; // Processing two channels inside inner loop
      }
      cycles += 19;
      cycles += convSize * (7 + numOutChans * getDenseDotProductCycles(
                                                  floatActivations,
                                                  floatPartials, numInChans));
    }
  }
  return cycles;
}

inline std::uint64_t
getZeroSupervisorVertexCycleEstimate(const std::vector<unsigned> &worklist,
                                     unsigned numGroups, unsigned dataPathWidth,
                                     unsigned numWorkerContexts, bool isFloat) {
  const unsigned vectorWidth = dataPathWidth / (isFloat ? 32 : 16);

  std::uint64_t maxWorkerCyclesZero = 0;
  for (unsigned context = 0; context != worklist.size(); ++context) {
    uint64_t numVectors = (worklist[context] + vectorWidth - 1) / vectorWidth;
    maxWorkerCyclesZero = std::max(maxWorkerCyclesZero,
                                   numVectors + (isFloat ? 14 : 15) -
                                       zeroPartialsRetentionSavings(isFloat));
  }
  uint64_t zeroCycles = maxWorkerCyclesZero * numWorkerContexts * numGroups;
  return zeroCycles;
}

inline std::uint64_t
getConvPartialHorizontalMacSupervisorInnerLoopCycleEstimate(
    const std::vector<std::vector<std::vector<unsigned>>> &workerPartitions,
    unsigned kernelSize, unsigned numInChansPerGroup,
    unsigned numOutChansPerGroup, unsigned numWorkerContexts,
    bool floatActivations, bool floatPartials) {
  unsigned usedContexts = workerPartitions.size();
  uint64_t cycles = 0;
  uint64_t maxWorkerCycles = 0;
  uint64_t minWorkerCycles = usedContexts < numWorkerContexts
                                 ? 0
                                 : std::numeric_limits<uint64_t>::max();
  for (auto context = 0U; context != usedContexts; ++context) {
    uint64_t thisWorkerCycles = 0;
    for (auto k = 0U; k != kernelSize; ++k) {
      thisWorkerCycles += getConvPartialHorizontalMacCycleEstimate(
          floatActivations, floatPartials, numInChansPerGroup,
          numOutChansPerGroup, workerPartitions[context][k]);
    }
    const unsigned workerNonLoopOverhead = 16;
    thisWorkerCycles += workerNonLoopOverhead;
    maxWorkerCycles =
        std::max(maxWorkerCycles, numWorkerContexts * thisWorkerCycles);
    minWorkerCycles =
        std::min(minWorkerCycles, numWorkerContexts * thisWorkerCycles);
  }
  cycles += std::max(maxWorkerCycles, minWorkerCycles);
  return cycles;
}

inline std::uint64_t
getConvPartialHorizontalMacSupervisorOuterLoopCycleEstimate(
    std::uint64_t innerLoopCycles, unsigned numConvGroups, unsigned numInGroups,
    unsigned numOutGroups, unsigned numWorkers, bool floatActivations) {
  uint64_t cycles = innerLoopCycles;
  return convHorizontalMacOverhead(floatActivations) +
         numWorkers * zeroPartialsRetentionSavings(/* floatPartials */ true) +
         numConvGroups *
             (23 + numInGroups * (15 + numOutGroups * (10 + cycles)));
}

inline std::uint64_t getConvPartialHorizontalMacSupervisorCycleEstimate(
    const std::vector<std::vector<std::vector<unsigned>>> &workerPartitions,
    unsigned numConvGroups, unsigned numInGroups, unsigned numOutGroups,
    unsigned kernelSize, unsigned numInChansPerGroup,
    unsigned numOutChansPerGroup, unsigned numWorkerContexts,
    bool floatActivations, bool floatPartials) {
  auto cycles = getConvPartialHorizontalMacSupervisorInnerLoopCycleEstimate(
      workerPartitions, kernelSize, numInChansPerGroup, numOutChansPerGroup,
      numWorkerContexts, floatActivations, floatPartials);
  return getConvPartialHorizontalMacSupervisorOuterLoopCycleEstimate(
      cycles, numConvGroups, numInGroups, numOutGroups, numWorkerContexts,
      floatActivations);
}

inline std::uint64_t getConvPartial1x1SupervisorInnerLoopCycleEstimate(
    const std::vector<std::vector<unsigned>> &workerPartitions,
    unsigned numWorkerContexts, unsigned numConvUnits, bool outputZeroing,
    bool floatActivations, bool floatPartials) {
  // Core loop cycles for 16x8 AMP vertex
  auto coreCycles = floatActivations ? 8 : 4;
  // Core loop cycles for 8x4 AMP vertex
  if (numConvUnits == 4) {
    coreCycles /= 2;
  }

  auto retentionSavings =
      conv1x1WorkerRetentionSavings(floatActivations, floatPartials);
  unsigned usedContexts = workerPartitions.size();
  uint64_t maxWorkerCycles = 0;
  uint64_t minWorkerCycles = usedContexts < numWorkerContexts
                                 ? 0
                                 : std::numeric_limits<uint64_t>::max();
  unsigned zeroCyclesPerGroup = floatPartials ? 4 : 2;
  for (const auto &worker : workerPartitions) {
    // 1x1 vertex doesn't support more than one worklist item per worker.
    assert(worker.size() <= 1);

    uint64_t thisWorkerCycles = 0;
    if (!worker.empty()) {
      const auto numElems = worker.front();
      switch (numElems) {
      case 0:
        if (floatActivations) {
          thisWorkerCycles += 24;
        } else {
          if (floatPartials) {
            thisWorkerCycles += (outputZeroing ? 22 : 25);
          } else {
            thisWorkerCycles += 24;
          }
        }
        break;
      case 1:
        if (floatActivations)
          thisWorkerCycles += 47 + (2 + zeroCyclesPerGroup) * outputZeroing;
        else {
          if (floatPartials) {
            thisWorkerCycles += (outputZeroing ? 35 : 39);
          } else {
            thisWorkerCycles += 39 + (2 + zeroCyclesPerGroup) * outputZeroing;
          }
        }
        break;
      case 2:
        if (floatActivations)
          thisWorkerCycles += 46 + (2 + zeroCyclesPerGroup * 2) * outputZeroing;
        else {
          if (floatPartials) {
            thisWorkerCycles += (outputZeroing ? 37 : 41);
          } else {
            thisWorkerCycles +=
                40 + (2 + zeroCyclesPerGroup * 2) * outputZeroing;
          }
        }
        break;
      default:
        if (floatActivations)
          thisWorkerCycles +=
              46 + (2 + zeroCyclesPerGroup * numElems) * outputZeroing +
              (numElems - 3) * coreCycles;
        else {
          if (floatPartials) {
            thisWorkerCycles +=
                (outputZeroing ? 37 : 40) + (numElems - 3) * coreCycles;
          } else {
            thisWorkerCycles +=
                41 + (2 + zeroCyclesPerGroup * numElems) * outputZeroing +
                (numElems - 3) * coreCycles;
          }
        }
      }
      thisWorkerCycles -= retentionSavings.first;
    }

    maxWorkerCycles =
        std::max(maxWorkerCycles, numWorkerContexts * thisWorkerCycles);
    minWorkerCycles =
        std::min(minWorkerCycles, numWorkerContexts * thisWorkerCycles);
  }

  // tag cost to worker with min cycles
  maxWorkerCycles = std::max(maxWorkerCycles, minWorkerCycles + 14);

  return maxWorkerCycles;
}

inline unsigned getConvPartialAmpSupervisorCycleWeightLoadEstimate(
    unsigned convUnitInputLoadElemsPerCycle, unsigned numConvUnits,
    unsigned convUnitCoeffLoadBytesPerCycle, bool floatActivations,
    unsigned filterHeight) {

  // Number of load instruction per AMP loop (See Loop_start_Amp label)
  unsigned numInputLoadsInnerLoop = 4;

  // When using AMP 4 engines number of loads need to be halved
  if (numConvUnits == 4) {
    numInputLoadsInnerLoop /= 2;
  }

  // Nx1 specific - due to data shuffling can't use ld128 for filter height
  // equal to 4 so it's always uses ld64. ld128 allows to us to
  // load 16 bytes per cycle hence convUnitCoeffLoadBytesPerCycle needs to be
  // halved
  if (filterHeight == 4 && convUnitCoeffLoadBytesPerCycle > 8) {
    convUnitCoeffLoadBytesPerCycle /= 2;
  }

  unsigned weightsLoadCycles =
      (convUnitInputLoadElemsPerCycle * // 2 for floats and 4 for halves
       numInputLoadsInnerLoop * numConvUnits *
       (floatActivations ? 4 : 2)) / // convert elements to bytes
      convUnitCoeffLoadBytesPerCycle;

  return weightsLoadCycles;
}

inline std::uint64_t getConvPartial1x1SupervisorOuterLoopCycleEstimate(
    std::uint64_t innerLoopCyclesWithZeroing,
    std::uint64_t innerLoopCyclesWithoutZeroing, unsigned numConvGroups,
    unsigned numInGroups, unsigned numOutGroups, unsigned outChansPerGroup,
    unsigned convUnitInputLoadElemsPerCycle, unsigned numConvUnits,
    unsigned convUnitCoeffLoadBytesPerCycle, bool floatActivations,
    bool floatPartials, unsigned numWorkerContexts) {
  const auto outputPassesPerGroup =
      (outChansPerGroup + numConvUnits - 1) / numConvUnits;

  const auto retentionSavings =
      conv1x1WorkerRetentionSavings(floatActivations, floatPartials);

  // Filter height is not applicable to 1x1 vertex so set it to 1
  const auto numLoads = getConvPartialAmpSupervisorCycleWeightLoadEstimate(
      convUnitInputLoadElemsPerCycle, numConvUnits,
      convUnitCoeffLoadBytesPerCycle, floatActivations, 1);

  const uint64_t supervisorNonloopOverhead = 50;
  const unsigned outPassesOverhead = 7;
  const unsigned excessInChanOverhead = 1;
  return supervisorNonloopOverhead +
         numWorkerContexts *
             (retentionSavings.first +
              retentionSavings.second * (numInGroups * numConvGroups - 1)) +
         numConvGroups *
             (12 +
              (numInGroups - 1) *
                  (15 + excessInChanOverhead +
                   numOutGroups * (19 + outputPassesPerGroup *
                                            (6 + numLoads +
                                             innerLoopCyclesWithoutZeroing))) +
              (10 + excessInChanOverhead +
               numOutGroups *
                   (19 + outputPassesPerGroup * (outPassesOverhead + numLoads +
                                                 innerLoopCyclesWithZeroing))));
}

inline std::uint64_t getConvPartial1x1SupervisorCycleEstimate(
    const std::vector<std::vector<unsigned>> &workerPartitions,
    unsigned numConvGroups, unsigned numInGroups, unsigned numOutGroups,
    unsigned outChansPerGroup, unsigned convUnitInputLoadElemsPerCycle,
    unsigned numConvUnits, unsigned convUnitCoeffLoadBytesPerCycle,
    unsigned numWorkerContexts, bool floatActivations, bool floatPartials) {
  auto innerLoopCyclesWithZeroing =
      getConvPartial1x1SupervisorInnerLoopCycleEstimate(
          workerPartitions, numWorkerContexts, numConvUnits, true,
          floatActivations, floatPartials);
  auto innerLoopCyclesWithoutZeroing =
      getConvPartial1x1SupervisorInnerLoopCycleEstimate(
          workerPartitions, numWorkerContexts, numConvUnits, false,
          floatActivations, floatPartials);

  return getConvPartial1x1SupervisorOuterLoopCycleEstimate(
      innerLoopCyclesWithZeroing, innerLoopCyclesWithoutZeroing, numConvGroups,
      numInGroups, numOutGroups, outChansPerGroup,
      convUnitInputLoadElemsPerCycle, numConvUnits,
      convUnitCoeffLoadBytesPerCycle, floatActivations, floatPartials,
      numWorkerContexts);
}

inline std::uint64_t getConvPartialnx1SupervisorCycleOuterLoopEstimate(
    std::uint64_t innerLoopCycles, unsigned numConvGroups,
    unsigned numOutGroups, unsigned numInGroups, unsigned outChansPerGroup,
    unsigned numConvUnits, unsigned numWorkerContexts, bool floatActivations,
    bool floatPartials) {
  uint64_t cycles = innerLoopCycles;
  return // Other constant supervisor code cycles
      convNx1Overhead() +
      // First iteration does not save cycles to calculate state which
      // will then be retained
      numWorkerContexts *
          convnx1WorkerRetentionSavings(floatActivations, floatPartials) +
      numWorkerContexts * zeroPartialsRetentionSavings(floatPartials) +
      // Supervisor code loop to zero partials. brnzdec loops mean
      // 6-cycle stall for all but last iteration.
      numConvGroups * (numOutGroups * 17 + (numOutGroups - 1) * 6 + 1) +
      (numConvGroups - 1) * 6 + 1 +
      // Supervisor code loop over conv/in/out groups
      numConvGroups * (16 + numInGroups * (14 + numOutGroups * (14 + cycles)));
}

inline std::uint64_t getConvPartialnx1SupervisorCycleInnerLoopEstimate(
    const std::vector<std::vector<std::vector<unsigned>>> &workerPartitions,
    unsigned kernelInnerElems, unsigned kernelOuterElems, unsigned filterHeight,
    unsigned outChansPerGroup, unsigned convUnitInputLoadElemsPerCycle,
    unsigned numConvUnits, unsigned convUnitCoeffLoadBytesPerCycle,
    unsigned numWorkerContexts, bool floatActivations, bool floatPartials) {
  // Core loop cycles for vertex will all engines in use
  auto coreCycles = floatActivations ? 8 : 4;
  // when using half of AMP engines need to reduce core cycles as well
  if (numConvUnits == 4) {
    coreCycles /= 2;
  }

  const auto retentionSavings =
      convnx1WorkerRetentionSavings(floatActivations, floatPartials);
  unsigned usedContexts = workerPartitions.size();
  unsigned numOutChanPasses = outChansPerGroup / numConvUnits;

  // innermostLoopCycles is the cycles in the innermost supervisor loop
  uint64_t innermostLoopCycles =
      getConvPartialAmpSupervisorCycleWeightLoadEstimate(
          convUnitInputLoadElemsPerCycle, numConvUnits,
          convUnitCoeffLoadBytesPerCycle, floatActivations, filterHeight);

  // additional load cycles dependent on filterHeight
  switch (filterHeight) {
  case 4:
    innermostLoopCycles += 60;
    break;
  case 2:
    innermostLoopCycles += 46;
    break;
  case 1:
    innermostLoopCycles += 15;
    break;
  default:
    // non-limited version will pick this up and we don't estimate unlimited
    // version correctly
    innermostLoopCycles += 20 * filterHeight;
  }

  innermostLoopCycles += 3;

  uint64_t innerLoopCycles = 0;
  for (auto ky = 0U; ky != kernelOuterElems; ++ky) {
    innerLoopCycles += 14;
    for (auto kx = 0U; kx != kernelInnerElems; ++kx) {
      // remove cycles for branch in outChanPasses loop for last iteration
      innerLoopCycles += 17 - 5;
      const unsigned extraCycles = floatPartials ? 0 : 1;
      for (auto ocp = 0U; ocp != numOutChanPasses; ++ocp) {
        uint64_t maxWorkerCycles = 0;
        uint64_t minWorkerCycles = usedContexts < numWorkerContexts
                                       ? 0
                                       : std::numeric_limits<uint64_t>::max();
        for (auto context = 0U; context != usedContexts; ++context) {
          uint64_t thisWorkerCycles = 17 + extraCycles;
          const auto k = ky * kernelInnerElems + kx;
          for (auto &numElems : workerPartitions[context][k]) {
            switch (numElems) {
            case 0:
              thisWorkerCycles += 17;
              break;
            case 1:
              thisWorkerCycles += (floatActivations ? 33 : 29);
              break;
            case 2:
              thisWorkerCycles += (floatActivations ? 44 : 33);
              break;
            default:
              if (floatActivations)
                thisWorkerCycles += 45 + (numElems - 3) * coreCycles;
              else
                thisWorkerCycles += 34 + (numElems - 3) * coreCycles;
            }
            thisWorkerCycles -= retentionSavings;
          }
          maxWorkerCycles =
              std::max(maxWorkerCycles, numWorkerContexts * thisWorkerCycles);
          minWorkerCycles =
              std::min(minWorkerCycles, numWorkerContexts * thisWorkerCycles);
        }
        innerLoopCycles += innermostLoopCycles +
                           std::max(maxWorkerCycles, minWorkerCycles + 9);
      }
    }
  }
  return innerLoopCycles;
}

inline std::uint64_t getConvPartialnx1SupervisorCycleEstimate(
    const std::vector<std::vector<std::vector<unsigned>>> &workerPartitions,
    unsigned numConvGroups, unsigned numOutGroups, unsigned numInGroups,
    unsigned kernelInnerElems, unsigned kernelOuterElems, unsigned filterHeight,
    unsigned inChansPerGroup, unsigned outChansPerGroup,
    unsigned convUnitInputLoadElemsPerCycle, unsigned numConvUnits,
    unsigned convUnitCoeffLoadBytesPerCycle, unsigned numWorkerContexts,
    bool floatActivations, bool floatPartials) {
  auto innerLoopCycles = getConvPartialnx1SupervisorCycleInnerLoopEstimate(
      workerPartitions, kernelInnerElems, kernelOuterElems, filterHeight,
      outChansPerGroup, convUnitInputLoadElemsPerCycle, numConvUnits,
      convUnitCoeffLoadBytesPerCycle, numWorkerContexts, floatActivations,
      floatPartials);
  return getConvPartialnx1SupervisorCycleOuterLoopEstimate(
      innerLoopCycles, numConvGroups, numOutGroups, numInGroups,
      outChansPerGroup, numConvUnits, numWorkerContexts, floatActivations,
      floatPartials);
}

inline std::uint64_t getConvPartialSlicSupervisorCycleWeightLoadEstimate(
    unsigned convGroupsPerGroup, unsigned chansPerGroup,
    unsigned numWorkerContexts, unsigned slicWindowWidth) {
  assert(slicWindowWidth == 4u);
  assert(chansPerGroup == 4u / convGroupsPerGroup);
  std::uint64_t cycles = 0;
  if (convGroupsPerGroup == 1) {
    cycles += (6 + // brnzdec
               6 + // put CCCSLOAD
               6); // bri
  } else {
    assert(convGroupsPerGroup == 4 || convGroupsPerGroup == 2);
    const std::uint64_t workerLoadWeightsCycles =
        (convGroupsPerGroup == 4) ? 10 : 12;
    cycles += (9 + // brnzdec, put CCCSLOAD pointer (stall), store weights
                   // pointer for rearrangement.
               6 + // runall
               // Rearrange weights in workers
               (workerLoadWeightsCycles * numWorkerContexts) + 6); // sync
  }
  cycles += 16; // 16 * ld64putcs
  return cycles;
}

inline std::uint64_t getConvPartialSlicSupervisorCycleOuterLoopEstimate(
    std::uint64_t implicitZeroingInnerLoopCycles, std::uint64_t innerLoopCycles,
    std::uint64_t weightLoadCycles, unsigned numConvGroupGroups,
    unsigned numSubKernels, unsigned numConvUnits, unsigned slicWindowWidth,
    bool floatActivations, bool floatPartials) {

  // TODO: we currently only target a kernel width of 4.
  assert(!floatActivations);
  assert(slicWindowWidth == 4);
  assert(numConvGroupGroups >= 1);
  assert(numSubKernels >= 1);

  // Similar, but different function for the 8 convUnits, half partials case
  const bool half8Conv = (numConvUnits == 8 && floatPartials == false);

  const std::uint64_t supervisorPreambleCycles = half8Conv ? 25 : 28;
  const std::uint64_t supervisorConvGroupGroupsBodyCycles = half8Conv ? 12 : 15;
  const std::uint64_t supervisorConvGroupGroupsLoopCycles =
      supervisorConvGroupGroupsBodyCycles * numConvGroupGroups +
      6 * (numConvGroupGroups - 1) +
      1; // 6 cycles brnzdec stall for all but last conv group group
  const std::uint64_t supervisorSubKernelBodyCycles =
      weightLoadCycles +
      (half8Conv ? 0 : 3) + // deal with whether to swap output pointers or not
      2 +                   // store new worklist pointer and increment
      (half8Conv ? 0 : 1) + // or, store implicit zero/stride
      6 +                   // runall
      6 +                   // sync
      1;                    // load new weights pointer

  const std::uint64_t supervisorSubKernelLoopCycles =
      supervisorSubKernelBodyCycles * numSubKernels + 6 * (numSubKernels - 1) +
      1; // brnzdec is 6 cycles in all but the last iteration.

  const std::uint64_t cycles =
      supervisorPreambleCycles + supervisorConvGroupGroupsLoopCycles +
      supervisorSubKernelLoopCycles +
      // Workers make one pass for the first sub-kernel implicitly zeroing
      // partials, and the remainder of the sub-kernels not implicitly zeroing.
      (numConvGroupGroups * implicitZeroingInnerLoopCycles +
       numConvGroupGroups * (numSubKernels - 1) * innerLoopCycles);

  return cycles;
}

// This gives us the number of cycles in terms of supervisor cycles
// for all workers to process a single conv group/sub-kernel. There is
// a strong assumption that the amount of work is always the same between
// sub-kernels.
inline std::uint64_t getConvPartialSlicSupervisorCycleInnerLoopEstimate(
    const std::vector<std::vector<unsigned>> &workerPartitions,
    unsigned numWorkerContexts, unsigned numConvUnits, unsigned slicWindowWidth,
    bool floatActivations, bool floatPartials, unsigned outputStride,
    bool implicitZeroing) {
  // TODO: we currently only target kernel width of 4.
  assert(!floatActivations);
  assert(slicWindowWidth == 4);

  const unsigned inputDataPasses = numConvUnits == 16 ? 1 : 2;
  // Similar, but different function for the 8 convUnits, half partials case
  const bool half8Conv = (numConvUnits == 8 && floatPartials == false);
  const unsigned loopDecisionThreshold = (half8Conv ? 6 : 5);

  std::uint64_t maxWorkerCycles = 0;

  const std::uint64_t workerProcessGroupPreambleCycles =
      2 +                   // Get worker ID
      (half8Conv ? 2 : 3) + // Load and maybe switch output pointers
      1 +                   // Load input pointer
      2 +                   // Load worklist DeltaN for worker
      4 +                   // Unpack DeltaN
      2 + // Load base pointer for DeltaN and add to form final worklist pointer
      2 + // Divide number of work items in the list by 3
      1 + // Load implicit zero flag + strides from stack
      (half8Conv ? 1 : 0); // Implicit zero loop decision
  // worker partitions is indexed by [worker][partitions].
  std::uint64_t cumulativeFieldElems = 0;
  for (const auto &worker : workerPartitions) {
    std::uint64_t workerCycles = workerProcessGroupPreambleCycles;

    for (const auto &numFieldElems : worker) {
      workerCycles += (half8Conv ? 9 : 10); // Pre-amble, brnzdec
      if (implicitZeroing) {
        workerCycles += 1; // Extra branch to exit
      }
      std::uint64_t rowCycles = 0;

      if (outputStride == 1) {
        if (numFieldElems < loopDecisionThreshold) {
          if (implicitZeroing) {
            rowCycles += 10 + (numFieldElems > 1 ? numFieldElems : 0) + 3;
          } else {
            rowCycles += 7;
            if (numFieldElems == 1) {
              rowCycles += 6;
            } else {
              rowCycles += 1 + (numFieldElems - 1) + 2 + (4 - numFieldElems) +
                           2 + (3 - (4 - numFieldElems)) + 3;
            }
          }
        } else {
          if (implicitZeroing) {
            rowCycles += 15 + (numFieldElems - 5);
          } else {
            // Account for decisions on numFieldElements in half8Conv loop
            rowCycles += 15 + (numFieldElems - 5) + (half8Conv ? 3 : 0);
          }
        }
      } else {
        // outputStride == 2
        if (numFieldElems < 3) {
          // Cycles for > 3 field elements matches for implicit
          // zeroing vs. normal
          rowCycles += 7 + (numFieldElems == 1 ? 3 : 5) + 3;
        } else {
          // Cycles for < 3 field elements matches for implicit
          // zeroing vs. normal
          rowCycles += 15 + 2 * (numFieldElems - 3);
        }
      }

      // For float partials, dummy dual load is used to incrememnt pointers
      if (floatPartials) {
        rowCycles -= 1;
      }

      // Account for the passes over input data
      workerCycles += (floatPartials ? 3 : 0) + rowCycles * inputDataPasses;
      // Count field elems total so we can account for the merging copy
      cumulativeFieldElems += numFieldElems;
    }
    // Account for the copy to merge the 2 outputs. Decision only
    workerCycles += (half8Conv ? 2 : 0);
    maxWorkerCycles = std::max(maxWorkerCycles, workerCycles);
  }
  // So far we have the total max cycles for any worker for all the work which
  // can be spread over many sub kernels.  Only on one pass (of 8 conv, half
  // vertex) will workers merge the 2 outputs together (When the last sub kernel
  // is used). Here we add the cycles to account for this on one pass - the
  // pass where implicit zeroing is used
  const std::uint64_t copyCycles =
      (half8Conv && implicitZeroing) ? (2 + 2 * cumulativeFieldElems) : 0;
  return maxWorkerCycles * numWorkerContexts + copyCycles;
}

inline std::uint64_t getMatMul2CycleEstimate(unsigned size) {
  // Inner loop is dominated by loads (load pointer, load 64bits, load 16
  // bits). This could be improved if we uses strided loads instead of
  // pointers.
  return 5 + size * 3;
}

inline uint64_t getWgdDataTransformCycles(unsigned numChannels, bool isFloat) {
  unsigned chansPerOp = isFloat ? 2 : 4;
  return 13 + 56 * ((numChannels + chansPerOp - 1) / chansPerOp);
}

inline uint64_t getWgdKernelTransformCycles(unsigned numChannels,
                                            bool isFloat) {
  unsigned chansPerOp = isFloat ? 2 : 4;
  return 2 + 35 * ((numChannels + chansPerOp - 1) / chansPerOp);
}

inline uint64_t getWgdInvTransformCycles(unsigned numChannels, bool isFloat) {
  unsigned chansPerOp = isFloat ? 2 : 4;
  return 15 + 30 * ((numChannels + chansPerOp - 1) / chansPerOp);
}

/**
 * The accumulator operates on pencils which are of depth "pencilDepth".
 * An inner product of a coefficient vector and data vector is computed.
 * "comPencils" gives the number of pencils which share a common coefficient
 * vector. "numPencils" gives a set of pencils which share common coefficients
 */
inline uint64_t getWgdAccumCycles(unsigned numPencils, unsigned comPencils,
                                  unsigned pencilDepth, unsigned outDepth,
                                  unsigned numWorkers, unsigned numConvUnits,
                                  unsigned weightsPerConvUnit,
                                  unsigned convUnitCoeffLoadBytesPerCycle,
                                  bool isFloat) {

  unsigned numCoeffSets = (outDepth + numConvUnits - 1) / numConvUnits;
  numCoeffSets *= (pencilDepth + weightsPerConvUnit - 1) / weightsPerConvUnit;
  numCoeffSets *= numPencils;
  const auto coeffLoadCycles = numConvUnits * weightsPerConvUnit *
                               (isFloat ? 2 : 4) /
                               convUnitCoeffLoadBytesPerCycle;
  const auto overhead = 4;

  const auto numPencilsPerWorker = (comPencils + numWorkers - 1) / numWorkers;
  return (overhead + coeffLoadCycles + numPencilsPerWorker * numWorkers * 4) *
         numCoeffSets;
}

inline uint64_t getWgdReduceCycles(unsigned numPencils, unsigned depth,
                                   bool isFloat) {
  unsigned chansPerOp = isFloat ? 2 : 4;
  return 5 + ((numPencils * depth + chansPerOp - 1) / chansPerOp);
}

inline uint64_t getWgdCompleteCycles(unsigned numChannels, bool isFloat) {
  unsigned divFactor = isFloat ? 2 : 4;

  return 5 + numChannels / divFactor;
}

inline std::uint64_t getOuterProductCycleEstimate(bool isFloat, unsigned width,
                                                  unsigned numChannels,
                                                  unsigned chansPerGroup,
                                                  unsigned dataPathWidth) {
  assert(numChannels % chansPerGroup == 0);
  const auto numChanGroups = numChannels / chansPerGroup;

// TODO T14719: Derive this from IPUArchInfo
#define CSR_W_REPEAT_COUNT__VALUE__MASK 0x0FFF
  auto const hardwareRptCountConstraint = CSR_W_REPEAT_COUNT__VALUE__MASK + 1;

  int cycles;
  // Conditions for executing a fast or slow path, replicated from the assembly
  // implementation
  if (isFloat) {
    if ((chansPerGroup >= 6) &&       // Min size of unrolled loop
        ((chansPerGroup & 1) == 0) && // Loop processes 2 at once
        ((chansPerGroup / 2 - 3) < hardwareRptCountConstraint) &&
        ((chansPerGroup / 2 + 1) < 512)) { // Stride size constraint

      // Float, Fast path cycle estimates
      cycles =
          25 + numChanGroups * (11 + width * (6 + (chansPerGroup - 6) / 2));
    } else {
      // Float, Slow path cycle estimates
      cycles = 25 + numChanGroups * (11 + width * (10 + chansPerGroup * 2));
    }
  } else {
    if ((chansPerGroup >= 12) &&      // Min size of unrolled loop
        ((chansPerGroup & 3) == 0) && // Loop processes 2 at once
        ((chansPerGroup / 4 - 3) < hardwareRptCountConstraint) &&
        ((chansPerGroup / 4 + 1) < 512)) { // Stride size constraint

      // Half, Fast path cycle estimates
      cycles =
          25 + numChanGroups * (10 + width * (6 + (chansPerGroup - 12) / 4));
    } else {
      // Half, Slow path cycle estimates
      cycles =
          25 + numChanGroups * (10 + width * (10 + (chansPerGroup * 5) / 2));
    }
  }
  return cycles;
}

inline uint64_t getReduceCycleEstimate(unsigned outSize, unsigned partialsSize,
                                       unsigned dataPathWidth,
                                       bool isOutTypeFloat,
                                       bool isPartialsFloat, bool singleInput,
                                       bool constrainPartials,
                                       unsigned numWorkers) {
  unsigned cycles = 0;
  if (singleInput) {
    unsigned supervisorCycles = 33;
    // Simpler optimised vertex, 1 or 2 cycle inner loop
    const auto cyclesPerInnerLoop = constrainPartials ? 1 : 2;
    const auto loops = isPartialsFloat ? (outSize / 4) : (outSize / 8);
    auto loopsDividedBetweenWorkers = loops / numWorkers;
    if (loops % numWorkers) {
      loopsDividedBetweenWorkers++;
    }
    cycles = 20;
    unsigned outerLoopOverHead;
    if (isPartialsFloat) {
      outerLoopOverHead = isOutTypeFloat ? 8 : 7;
    } else {
      outerLoopOverHead = isOutTypeFloat ? 10 : 9;
    }
    cycles += (cyclesPerInnerLoop * partialsSize + outerLoopOverHead) *
              loopsDividedBetweenWorkers;
    return cycles * numWorkers + supervisorCycles;
  }

  // Supervisor vertex, and new implementation
  if (isPartialsFloat) {
    cycles = 32;
    // Float - workers process 4 at once, and account for remainder loops
    auto loops = outSize / 4;
    if (outSize & 1)
      loops++;
    if (outSize & 2)
      loops++;
    // Account for time at full load - all workers busy
    auto loopsDividedBetweenWorkers = loops / numWorkers;
    // and a remainder where only some are busy which can be a shorter loop
    if (loops % numWorkers) {
      if (outSize & 3)
        cycles += (2 * partialsSize + 13);
      else
        loopsDividedBetweenWorkers++;
    }

    if (isOutTypeFloat)
      cycles += (3 * partialsSize + 7) * loopsDividedBetweenWorkers;
    else
      cycles += (3 * partialsSize + 6) * loopsDividedBetweenWorkers;
  } else {
    cycles = 32;
    // Half - workers process 8 at once, and account for remainder loops
    auto loops = outSize / 8;
    if (outSize & 1)
      loops++;
    if (outSize & 2)
      loops++;
    if (outSize & 4)
      loops++;
    // Account for time at full load - all workers busy
    auto loopsDividedBetweenWorkers = loops / numWorkers;
    // and a remainder where only some are busy which can be a shorter loop
    if (loops % numWorkers) {
      if (outSize & 7)
        cycles += (2 * partialsSize + 11);
      else
        loopsDividedBetweenWorkers++;
    }

    if (isOutTypeFloat)
      cycles += (3 * partialsSize + 9) * loopsDividedBetweenWorkers;
    else
      cycles += (3 * partialsSize + 8) * loopsDividedBetweenWorkers;
  }
  cycles = cycles * numWorkers;

  return cycles;
}

#endif // _performance_estimation_h_
