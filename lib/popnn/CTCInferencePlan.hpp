// Copyright (c) 2021 Graphcore Ltd. All rights reserved.

#ifndef popnn_CTCInferencePlan_hpp
#define popnn_CTCInferencePlan_hpp

#include <poplibs_support/Algorithm.hpp>
#include <poplibs_support/Compiler.hpp>
#include <poplibs_support/Visitor.hpp>
#include <popnn/CTCInference.hpp>

#include <boost/variant.hpp>

namespace popnn {
namespace ctc {

enum class SortMethod { SIMPLE_SORT, RANK };

struct CtcInferencePlannerParams {
  poplar::Type inType;
  poplar::Type partialsType;
  poplar::Type outType;
  unsigned batchSize;
  unsigned maxTime;
  unsigned maxLabelLength;
  unsigned numClasses;
  unsigned beamWidth;
};

template <typename T> struct SimpleSortPartitions {
  std::vector<T> simpleSort;
};
template <typename T> struct RankPartitions {
  std::vector<T> rank;
  std::vector<T> reduce;
};

template <typename T> struct CtcInferencePartition {
  // Each stage is partitioned using different parameters.  Throughout we use:
  // Beam: 0,1,2...beamWidthMinus1
  // Classes: a,b,c, ... numClassesExcludingBlank
  // Copy candidate from beam [n]: C[0], C[1]
  // Copy candidates can be broadcast: C[0] gives C[0]' C[0]" C[0]"' C[0]"" ...
  // Extend candidate from beam with class E[0a], E[0b]..., E[1a]...
  // Extend candidates from a beam with all classes: E[0..], E[1..]

  // ***************** Overall parameters  *****************
  // Number of partitions to divide the batchSize into.
  T batch;
  // The number of partitions of the time dimension in the implementation.
  // At present this is always 1
  T time;

  // ***************** Stage 1 : Generate *****************
  // Copy and Extend candidate generation happen in parallel.  They are
  // partitioned independently but occupy the same tiles.
  // TODO - Optimise this choice, possibly using different tiles for Copy and
  // Extend candidate generation.
  //
  // Generate copy candidates spread over 'copy' partitions.  Each has a
  // single vertex generating a single copy candidate:
  // Partition 0: C[0]
  // Partition 1: C[1]
  // ... ('copy' partitions)
  T copy;
  // Generate extend candidates splitting work over `extend` partitions. Each
  // partition has `extendVerticesPerPartition`.  A vertex can generate extend
  // candidates extending with a single class, for a range of beams.
  // For example with beamwidth=4 and extendVerticesPerPartition=2 we get:
  // Partition 0: Vertex0: E[0a], E[1a]. Vertex1: E[2a], E[3a]
  // Partition 1: Vertex0: E[0b], E[1b]. Vertex1: E[2b], E[3b]
  // Partition 2: Vertex0: E[0c], E[1c]. Vertex1: E[2c], E[3c]
  // ... ('extend' partitions)
  T extend;
  // As Copy and Extend candidate generation happen in parallel the number of
  // workers for Extend is affected by the Copy vertices.
  // `extendVerticesPerPartition` is set to reflect this and avoid > 6 workers
  // being used.
  T extendVerticesPerPartition;

  // ***************** Stage 2 : Merge *****************
  // Each vertex attempts to merge a single copy candidate with a group of
  // extend candidates. The copy candidate is modified with the merged
  // probabilities, the extend candidates are unchanged.
  // This requires beamwidth^2 vertices, arranged over `merge` partitions:
  // P0: Vertex0:C[0]', E[0..]  Vertex1:C[1]', E[0..]  Vertex2:C[2]', E[0..]
  // P1: Vertex0:C[0]", E[1..]  Vertex1:C[1]", E[1..]  Vertex2:C[2]", E[1..]
  // P2: Vertex0:C[0]"',E[2..]  Vertex1:C[1]"',E[2..]  Vertex2:C[2]"',E[2..]
  // ... ('merge' partitions)
  //
  // TODO - We need not compare C[X] with E[X..], so there could be just
  // beamwidth * (beamwidth-1) vertices.  It makes this step kind of irregular
  // and so is awkward.  Unless this step runs out of workers with just 1 more
  // comparison to do it doesn't actually slow things down.
  T merge;

  // ***************** Stage 3 : Select copy, zero extend *****************
  // Copy -
  // Reduce the broadcast C[0]' C[0]" ... candidates to just C[0], being
  // the single merged candidate (There can only be 1 if any), or just any one
  // of them, given that with no merge they will all be the same.
  // This occupies `selectCopy` partitions
  // Partition 0: Vertex selects C[0] from C[0]',C[0]", C[0]"' ...
  // Partition 1: Vertex selects C[1] from C[1]',C[1]", C[1]"' ...
  // ... ('selectCopy' partitions)
  T selectCopy;
  // Extend -
  // Mark any extend candidates that were merged as zero probability and so
  // never selected in the next step
  // Partition 0: Use C[0]', C[1]', C[2]',  Change E[0..]
  // Partition 1: Use C[0]", C[1]", C[2]",  Change E[1..]
  // Partition 2: Use C[0]"',C[1]"',C[2]"', Change E[2..]
  // ... ('selectExtend' partitions)
  T selectExtend;

  // ***************** Stage 4 : Sort *****************
  // Sorting can be completed in multiple stages where there is a speed benefit
  // although 1 or 2 stages are usually all that is needed.  The `sort` plan
  // variables are vectors, each vector entry specifying the way to carry out
  // the work in a stage: How the candidates to sort will be split into groups
  // (which are sorted independently), how the groups are mapped onto tiles and
  // what partitions the sorting work is divided into. There will always be 1
  // or more groups, and if there is 1 group that stage will produce the final
  // sorted result.
  // Therefore the number of stages = sortStageGroups.size(), the `sort`,
  // `sortStageGroups` and `sortGroupsPerTile` vectors will be the same size.
  // Within a stage, the candidates to sort are divided into
  // `sortStageGroups[stage]` groups.  Sorting within the group is independent
  // of all the other groups and so after a stage completes we will have
  // sortStageGroups[stage] * beamwidth candidates remaining which is the input
  // to the next stage.  The last stage must have 1 group and will output
  // `beamwidth` candidates.
  // The `RankPartitions[stage]` and `SimpleSortPartitions[stages]` variables
  // specify how many tiles the work WITHIN A GROUP is divided between.  In the
  // case of `RankPartitions` the `rank` and `reduce` variables specify the
  // division of those 2 operation's work.  So the number of partitions used is
  // given by: sortStageGroups[stage] * max(rank[stage], reduce[stage])
  //
  // For best speed the operations will be spread over many tiles, but this can
  // become constricted when the number of tiles becomes limited.  Like other
  // parts of this process the number of partitions the work is divided into
  // will begin to reduce as the number of tiles (per batch entry) is limited.
  // When the number of tiles becomes equal to the number of groups the `rank`
  // and `reduce` parameters will both be 1. So when groups > tiles we specify
  // sortGroupsPerTile[stage] to indicate group overlap.  Until group overlap
  // is needed sortGroupsPerTile[stage] = 1.
  //
  // This explanation is for a single group:
  // Two sorting methods are available : SIMPLE_SORT and RANK.
  // SIMPLE_SORT:
  // There is a single simple sort vertex in the 1st partition assigned to any
  // batch entry.  It is attached to all candidates from the select stage:
  // Partition 0: C[0],C[1],C[2]...E[0..],E[1..],E[2..]...
  // The result is C[0],C[2] ... (beamwidth most probable candidates)
  //
  // RANK:
  // There are sortRanking partitions, each receives a copy all the candidates
  // from the select stage:
  // Partition N: C[0],C[1],C[2]...E[0..],E[1..],E[2..]...
  // Each partition is assigned a number of candidates to "rank", and returns
  // a vector of beamwidth sorted candidates which are populated where that
  // partition ranked any candidate in the beamwidth most likely candidates, and
  // zero otherwise.
  // Sorted candidates (size beamwidth) denoted S[0..], S'[0..], S"[0..]
  // Partition 0: Rank candidates [0,6)   Result: S[0..]
  // Partition 1: Rank candidates [6,12)  Result: S'[0..]
  // Partition 2: Rank candidates [12,18) Result: S"[0..]
  // .... ('sort.rank' partitions)
  // Then a second reduce stage will reduce these results into C[0],C[1]...
  // Partition 0: C[0] = S[0]+ S'[0] + S"[0] + ...
  // Partition 1: C[1] = S[1]+ S'[1] + S"[1] + ...
  // ....('sort.reduce' partitions)
  boost::variant<SimpleSortPartitions<T>, RankPartitions<T>> sort;
  std::vector<T> sortStageGroups;
  std::vector<T> sortGroupsPerTile;

  // ***************** Stage 5 : Update *****************
  // The above stages require a per partition copy of the beam information, with
  // a structure describing output sequences and probabilities.  This is
  // updated using the result of the `Select` stage.  It updates all copies of
  // the beam information - which is the maximum number of copies needed by the
  // other Stages.
  // No parameters required.

  // ***************** Post loop stage: Output *****************
  // Outputs are generated after the loop. This process is spread over `output`
  // partitions, where the `topPaths` most probable outputs are generated
  // Partition 0: Most probable path
  // Partition 1: 2nd most probable
  // ... ('output' partitions)
  T output;
};

class InferencePlan {
  poplar::Interval partition(unsigned fullSize, unsigned partitions,
                             unsigned index) const {
    const auto partitionSize = poplibs_support::ceildiv(fullSize, partitions);
    const auto begin = std::min(partitionSize * index, fullSize);
    const auto end = std::min(partitionSize * (index + 1), fullSize);
    return {begin, end};
  }

public:
  CtcInferencePlannerParams params;
  CtcInferencePartition<unsigned> parallel;

  // Given a batch size and partition index, return range of batch elements
  // represented in this partition
  poplar::Interval partitionBatch(unsigned batchSize, unsigned index) const {
    return partition(batchSize, parallel.batch, index);
  }

  // Given a time size and partition index, return range of time elements
  // represented in this partition
  poplar::Interval partitionTime(unsigned timeSize, unsigned index) const {
    return partition(timeSize, parallel.time, index);
  }

  // The larger of the `classes` and `beam` partitions is the total number
  // of broadcast inputs, and replicas of the beam history that we will build.
  // In this simple model of splitting up the work, copy candidates and extend
  // candidates are generated with vertices allocated on overlapping tiles, so
  // the maximum of the 2 parameters is used here. In a more complete solution
  // we could choose between overlapping (total=max) or sequential (total=sum)
  // allocation of vertices
  unsigned batchEntryPartitions(void) const {
    const auto maxCommon =
        std::max(parallel.merge, std::max(parallel.extend, parallel.copy));
    return boost::apply_visitor(
        poplibs_support::make_visitor<unsigned>(
            [&](const RankPartitions<unsigned> &sort) {
              unsigned largestStage = 0;
              auto maxSortPartitionsPerGroup =
                  std::max(sort.reduce[largestStage], sort.rank[largestStage]);
              auto maxSort = maxSortPartitionsPerGroup *
                             parallel.sortStageGroups[largestStage] /
                             parallel.sortGroupsPerTile[largestStage];
              return std::max(maxSort, maxCommon);
            },
            [&](const SimpleSortPartitions<unsigned> &sort) {
              return maxCommon;
            }),
        parallel.sort);
  }

  poplar::Interval partitionBatchEntry(unsigned size, unsigned index) const {
    return partition(size, batchEntryPartitions(), index);
  }

  poplar::Interval partitionMerge(unsigned mergeSize, unsigned index) const {
    return partition(mergeSize, parallel.merge, index);
  }

  poplar::Interval partitionOutput(unsigned outSize, unsigned index) const {
    return partition(outSize, parallel.output, index);
  }

  poplar::Interval partitionCopy(unsigned copySize, unsigned index) const {
    return partition(copySize, parallel.copy, index);
  }

  poplar::Interval partitionSelectCopy(unsigned copySize,
                                       unsigned index) const {
    return partition(copySize, parallel.selectCopy, index);
  }
  poplar::Interval partitionSelectExtend(unsigned extendSize,
                                         unsigned index) const {
    return partition(extendSize, parallel.selectExtend, index);
  }

  poplar::Interval partitionExtend(unsigned extendSize, unsigned index) const {
    return partition(extendSize, parallel.extend, index);
  }

  poplar::Interval partitionSort(unsigned sortSize, unsigned index,
                                 unsigned stage) const {
    return boost::apply_visitor(
        poplibs_support::make_visitor<poplar::Interval>(
            [&](const RankPartitions<unsigned> &sort) {
              return partition(sortSize, sort.rank[stage], index);
            },
            [&](const SimpleSortPartitions<unsigned> &sort) {
              return partition(sortSize, sort.simpleSort[stage], index);
            }),
        parallel.sort);
  }

  poplar::Interval partitionSortReduce(unsigned sortSize, unsigned index,
                                       unsigned stage) const {
    return boost::apply_visitor(
        poplibs_support::make_visitor<poplar::Interval>(
            [&](const RankPartitions<unsigned> &sort) {
              return partition(sortSize, sort.reduce[stage], index);
            },
            [&](const SimpleSortPartitions<unsigned> &sort) {
              // No member to partition
              POPLIB_UNREACHABLE();
              return partition(0, 0, 0);
            }),
        parallel.sort);
  }

  poplar::Interval partitionExtendVertices(unsigned extendSize,
                                           unsigned index) const {
    return partition(extendSize, parallel.extendVerticesPerPartition, index);
  }

  unsigned getTile(unsigned batch, unsigned time, unsigned stage,
                   unsigned group, unsigned partition) const {
    const auto groupSize = boost::apply_visitor(
        poplibs_support::make_visitor<unsigned>(
            [&](const RankPartitions<unsigned> &sort) {
              return std::max(sort.rank[stage], sort.reduce[stage]);
            },
            [&](const SimpleSortPartitions<unsigned> &sort) {
              return sort.simpleSort[stage];
            }),
        parallel.sort);

    const auto perBatchEntry = batchEntryPartitions();
    return batch * (parallel.time * perBatchEntry)                 // Batch
           + time * perBatchEntry                                  // Time
           + groupSize * group / parallel.sortGroupsPerTile[stage] // Group
           + partition; // Partitions in a group
  }
  unsigned getTile(unsigned batch, unsigned time, unsigned batchEntry) const {
    return batch * (parallel.time * batchEntryPartitions()) // Batch
           + time * batchEntryPartitions()                  // Time
           + batchEntry;                                    // Batch entry
  }
  // Tile allocation when splitting across batch and time dimensions only
  unsigned getTile(unsigned batch, unsigned time) const {
    return batch * (parallel.time * batchEntryPartitions()) // Batch
           + time;                                          // Time
  }

  unsigned numTiles() const { return parallel.batch * batchEntryPartitions(); }

  std::unique_ptr<InferencePlan> clone() const {
    return std::make_unique<InferencePlan>(*this);
  };
};

bool operator<(const InferencePlan &a, const InferencePlan &b) noexcept;
bool operator==(const InferencePlan &a, const InferencePlan &b) noexcept;

std::ostream &operator<<(std::ostream &o, const InferencePlan &p);

} // namespace ctc
} // namespace popnn

#endif // #ifndef popnn_CTCInferencePlan_hpp
