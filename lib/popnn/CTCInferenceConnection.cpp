// Copyright (c) 2021 Graphcore Ltd. All rights reserved.

#include "CTCInferenceConnection.hpp"

#include "CTCInferencePlan.hpp"
#include "CTCPlanInternal.hpp"

#include <poplar/Graph.hpp>
#include <poplar/Tensor.hpp>

#include <poplibs_support/LogArithmetic.hpp>
#include <poplibs_support/Tracepoint.hpp>
#include <poplibs_support/logging.hpp>
#include <poputil/TileMapping.hpp>
#include <poputil/exceptions.hpp>

#include <boost/optional.hpp>

using namespace poplar;
using namespace poplar::program;
using namespace poplibs_support;
using namespace poputil;

template <unsigned size> using Slice = std::array<std::size_t, size>;

namespace {

using TempTensors = popnn::ctc_infer::TempTensors;
using BeamTensors = popnn::ctc_infer::BeamTensors;

enum class BeamScalars { BLANK, NON_BLANK, BLANK_AND_NON_BLANK };

void attachBeamScalars(Graph &graph, const BeamTensors &beams, unsigned batch,
                       unsigned partition, unsigned beamwidth,
                       BeamScalars selectBlank, const VertexRef &vertex) {

  Slice<4> begin = {batch, partition, 0, 0};
  Slice<4> end = {batch + 1, partition + 1, beamwidth, 1};
  if (selectBlank == BeamScalars::BLANK ||
      selectBlank == BeamScalars::BLANK_AND_NON_BLANK) {
    graph.connect(vertex["beamProbBlank"],
                  beams.pb.slice(begin, end).flatten());
  }
  if (selectBlank == BeamScalars::NON_BLANK ||
      selectBlank == BeamScalars::BLANK_AND_NON_BLANK) {
    graph.connect(vertex["beamProbNonBlank"],
                  beams.pnb.slice(begin, end).flatten());
  }
  graph.connect(vertex["beamProbTotal"],
                beams.pTotal.slice(begin, end).flatten());
  graph.connect(vertex["lastBeamOutputs"],
                beams.lastOutput.slice(begin, end).flatten());
}

void attachBeamHistory(Graph &graph, const BeamTensors &beams,
                       const Interval &time, unsigned batch, unsigned partition,
                       unsigned beamwidth, const VertexRef &vertex) {

  Slice<4> begin = {batch, partition, time.begin(), 0};
  Slice<4> end = {batch + 1, partition + 1, time.end(), beamwidth};
  graph.connect(vertex["beamAddend"], beams.addend.slice(begin, end).flatten());
  graph.connect(vertex["beamParent"], beams.parent.slice(begin, end).flatten());
  Slice<4> beginLength = {batch, partition, 0, 0};
  Slice<4> endLength = {batch + 1, partition + 1, 2 * beamwidth, 1};
  graph.connect(vertex["beamLength"],
                beams.length.slice(beginLength, endLength).flatten());
}

void attachSingleCopyCandidate(Graph &graph, const TempTensors &tempTensors,
                               unsigned batch, unsigned partition,
                               const VertexRef &vertex) {
  auto transform = [=](const Tensor &in) {
    Slice<3> copyBegin = {batch, partition, 0};
    Slice<3> copyEnd = {batch + 1, partition + 1, 1};
    return in.slice(copyBegin, copyEnd).reshape({});
  };
  graph.connect(vertex["candidateParent"],
                transform(tempTensors.copyCandidatesParent));
  graph.connect(vertex["candidateAddend"],
                transform(tempTensors.copyCandidatesAddend));
  graph.connect(vertex["candidateBeamProbNonBlank"],
                transform(tempTensors.copyCandidatesPnb));
  graph.connect(vertex["candidateBeamProbBlank"],
                transform(tempTensors.copyCandidatesPb));
  graph.connect(vertex["candidateBeamProbTotal"],
                transform(tempTensors.copyCandidatesPTotal));
}

void attachGenerateExtendCandidates(Graph &graph,
                                    const TempTensors &tempTensors,
                                    unsigned batch, unsigned partition,
                                    const Interval &beamPartition,
                                    const VertexRef &vertex) {

  Slice<3> begin = {batch, partition, beamPartition.begin()};
  Slice<3> end = {batch + 1, partition + 1, beamPartition.end()};
  graph.connect(vertex["extendCandidateParent"],
                tempTensors.extendCandidatesParent.slice(begin, end).flatten());
  graph.connect(vertex["extendCandidateAddend"],
                tempTensors.extendCandidatesAddend.slice(begin, end).flatten());
  graph.connect(vertex["extendCandidateBeamProbNonBlank"],
                tempTensors.extendCandidatesPnb.slice(begin, end).flatten());
  graph.connect(vertex["extendCandidateBeamProbBlank"],
                tempTensors.extendCandidatesPb.slice(begin, end).flatten());
  graph.connect(vertex["extendCandidateBeamProbTotal"],
                tempTensors.extendCandidatesPTotal.slice(begin, end).flatten());
}

void attachMergeExtendCandidates(Graph &graph, const TempTensors &tempTensors,
                                 unsigned batch, unsigned partition,
                                 const VertexRef &vertex) {
  const auto numAddends = tempTensors.extendCandidatesParent.dim(1);

  Slice<3> begin = {batch, 0, partition};
  Slice<3> end = {batch + 1, numAddends, partition + 1};
  graph.connect(vertex["extendCandidateParent"],
                tempTensors.extendCandidatesParent.slice(begin, end).flatten());
  graph.connect(vertex["extendCandidateAddend"],
                tempTensors.extendCandidatesAddend.slice(begin, end).flatten());
  graph.connect(vertex["extendCandidateBeamProbNonBlank"],
                tempTensors.extendCandidatesPnb.slice(begin, end).flatten());
}

void attachData(Graph &graph, const Tensor &data, unsigned batch,
                unsigned partition, unsigned numClasses, const Interval &time,
                const VertexRef &vertex) {
  Slice<4> begin = {batch, partition, time.begin(), 0};
  Slice<4> end = {batch + 1, partition + 1, time.end(), numClasses};
  graph.connect(vertex["logProbs"], data.slice(begin, end).flatten());
}

void attachTimeAndCompleteFlag(Graph &graph, const TempTensors &tempTensors,
                               unsigned batch, unsigned partition,
                               const VertexRef &vertex) {
  graph.connect(vertex["currentTimestep"],
                tempTensors.currentTimestep[batch][partition][0]);
  graph.connect(vertex["complete"], tempTensors.complete[batch][partition][0]);
}
void attachTimeAndLength(Graph &graph, const TempTensors &tempTensors,
                         unsigned batch, unsigned partition,
                         const VertexRef &vertex) {
  attachTimeAndCompleteFlag(graph, tempTensors, batch, partition, vertex);
  graph.connect(vertex["dataLength"],
                tempTensors.dataLengths[batch][partition][0]);
}

void attachCompleteFlag(Graph &graph, const TempTensors &tempTensors,
                        unsigned batch, unsigned partition,
                        const VertexRef &vertex) {
  graph.connect(vertex["complete"], tempTensors.complete[batch][partition][0]);
}

} // namespace
namespace popnn {
namespace ctc_infer {

void generateExtendCandidateVertex(
    Graph &graph, const Tensor &data, const BeamTensors &beams,
    const TempTensors &tempTensors, ComputeSet &cs, unsigned batch,
    const Interval &time, unsigned addendPartition, unsigned dataPartition,
    unsigned blankClass, unsigned beamwidth, const Interval &beamPartition,
    unsigned addendClass, unsigned tile) {

  const auto partialsType = beams.pb.elementType();
  const auto vertexName =
      templateVertex("popnn::CTCGenerateExtendCandidates", data.elementType(),
                     partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex for symbol {} on tile {}", vertexName,
                        addendClass, tile);
  graph.setTileMapping(vertex, tile);

  // Data connection
  const auto numClasses = data.dim(3);
  attachData(graph, data, batch, dataPartition, numClasses, time, vertex);
  // Beam connection
  attachBeamScalars(graph, beams, batch, dataPartition, beamwidth,
                    BeamScalars::BLANK, vertex);
  // Timestep, complete flag connection
  attachTimeAndCompleteFlag(graph, tempTensors, batch, dataPartition, vertex);
  // Extend candidate connection
  attachGenerateExtendCandidates(graph, tempTensors, batch, addendPartition,
                                 beamPartition, vertex);
  // Constants
  graph.setInitialValue(vertex["numClassesIncBlank"], numClasses);
  graph.setInitialValue(vertex["startBeam"], beamPartition.begin());
  graph.setInitialValue(vertex["endBeam"], beamPartition.end());
  graph.setInitialValue(vertex["addendSymbol"], addendClass);
}

void generateCopyCandidateVertex(Graph &graph, const Tensor &data,
                                 const BeamTensors &beams,
                                 const TempTensors &tempTensors, ComputeSet &cs,
                                 unsigned batch, const Interval &time,
                                 unsigned beamPartition, unsigned dataPartition,
                                 unsigned blankClass, unsigned beamwidth,
                                 unsigned tile) {

  const auto partialsType = beams.pb.elementType();
  const auto vertexName =
      templateVertex("popnn::CTCGenerateCopyCandidates", data.elementType(),
                     partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex for beam {} on tile {}", vertexName,
                        beamPartition, tile);
  graph.setTileMapping(vertex, tile);

  // Data connection
  const auto numClasses = data.dim(3);
  attachData(graph, data, batch, dataPartition, numClasses, time, vertex);
  // Beam connection
  attachBeamScalars(graph, beams, batch, dataPartition, beamwidth,
                    BeamScalars::NON_BLANK, vertex);
  // Timestep, complete flag connection
  attachTimeAndCompleteFlag(graph, tempTensors, batch, dataPartition, vertex);
  // Copy candidate connection
  attachSingleCopyCandidate(graph, tempTensors, batch, beamPartition, vertex);
  // Constants
  graph.setInitialValue(vertex["numClassesIncBlank"], numClasses);
  graph.setInitialValue(vertex["blankClass"], blankClass);
  graph.setInitialValue(vertex["beamIdx"], beamPartition);
}

void mergeCandidateVertex(Graph &graph, const BeamTensors &beams,
                          const TempTensors &tempTensors, ComputeSet &cs,
                          unsigned batch, const Interval &time,
                          unsigned extendPartition, unsigned copyPartition,
                          unsigned beamPartition, unsigned blankClass,
                          unsigned beamwidth, unsigned numClasses,
                          unsigned tile) {

  const auto partialsType = beams.pb.elementType();
  const auto extendCandidates = tempTensors.extendCandidatesParent.dim(1);

  const auto vertexName =
      templateVertex("popnn::CTCMergeCandidates", partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex for copy {}, extend {},"
                        " candidates {}, on tile {}",
                        vertexName, copyPartition, extendPartition,
                        extendCandidates, tile);
  graph.setTileMapping(vertex, tile);

  // Extend candidate connection
  attachMergeExtendCandidates(graph, tempTensors, batch, extendPartition,
                              vertex);

  // Merge candidate connection (a single broadcast copy candidate)
  auto transform = [=](const std::vector<Tensor> &in) {
    Slice<3> mergeBegin = {batch, extendPartition, 0};
    Slice<3> mergeEnd = {batch + 1, extendPartition + 1, 1};
    return in[copyPartition].slice(mergeBegin, mergeEnd).reshape({});
  };
  graph.connect(vertex["copyCandidateParent"],
                transform(tempTensors.mergeCandidatesParent));
  graph.connect(vertex["copyCandidateAddend"],
                transform(tempTensors.mergeCandidatesAddend));
  graph.connect(vertex["copyCandidateBeamProbNonBlank"],
                transform(tempTensors.mergeCandidatesPnb));
  graph.connect(vertex["copyCandidateBeamProbBlank"],
                transform(tempTensors.mergeCandidatesPb));
  graph.connect(vertex["copyCandidateBeamProbTotal"],
                transform(tempTensors.mergeCandidatesPTotal));

  // Beam history connection
  attachBeamHistory(graph, beams, time, batch, beamPartition, beamwidth,
                    vertex);
  // The last output of the beam that the copy candidate came from
  graph.connect(vertex["lastBeamOutput"],
                beams.lastOutput[batch][beamPartition][copyPartition][0]);

  // Time and complete flag connection
  attachTimeAndCompleteFlag(graph, tempTensors, batch, beamPartition, vertex);

  // Constants
  graph.setInitialValue(vertex["beamwidth"], beamwidth);
  graph.setInitialValue(vertex["blankClass"], blankClass);
}

void selectCopyCandidateVertex(Graph &graph, const TempTensors &tempTensors,
                               ComputeSet &cs, unsigned batch,
                               unsigned copyPartition, unsigned beamPartition,
                               unsigned copyCandidates, unsigned tile) {

  const auto partialsType = tempTensors.mergeCandidatesPb[0].elementType();
  const auto vertexName = templateVertex("popnn::CTCSelectCopyCandidates",
                                         partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex for copy {} on tile {}", vertexName,
                        copyPartition, tile);
  graph.setTileMapping(vertex, tile);

  // Merge candidate connection (broadcast copy candidates, broadcast from a
  // single original beam)
  auto sliceIn = [=](const std::vector<Tensor> &in) {
    Slice<3> mergeBegin = {batch, 0, 0};
    Slice<3> mergeEnd = {batch + 1, copyCandidates, 1};
    return in[copyPartition].slice(mergeBegin, mergeEnd).flatten();
  };
  graph.connect(vertex["copyCandidateParent"],
                sliceIn(tempTensors.mergeCandidatesParent));
  graph.connect(vertex["copyCandidateAddend"],
                sliceIn(tempTensors.mergeCandidatesAddend));
  graph.connect(vertex["copyCandidateBeamProbNonBlank"],
                sliceIn(tempTensors.mergeCandidatesPnb));
  graph.connect(vertex["copyCandidateBeamProbBlank"],
                sliceIn(tempTensors.mergeCandidatesPb));
  graph.connect(vertex["copyCandidateBeamProbTotal"],
                sliceIn(tempTensors.mergeCandidatesPTotal));

  // Single result copy candidate connection using the original copy candidates
  // tensor
  attachSingleCopyCandidate(graph, tempTensors, batch, copyPartition, vertex);

  // Complete flag connection
  attachCompleteFlag(graph, tempTensors, batch, beamPartition, vertex);

  // Constants
  graph.setInitialValue(vertex["numCandidates"], copyCandidates);
}

void selectExtendCandidateVertex(Graph &graph, const TempTensors &tempTensors,
                                 ComputeSet &cs, unsigned batch,
                                 unsigned extendPartition,
                                 unsigned beamPartition,
                                 unsigned copyCandidates, unsigned blankClass,
                                 unsigned tile) {

  const auto partialsType = tempTensors.mergeCandidatesPb[0].elementType();
  const auto vertexName = templateVertex("popnn::CTCSelectExtendCandidates",
                                         partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex for extend {} on tile {}", vertexName,
                        extendPartition, tile);
  graph.setTileMapping(vertex, tile);

  // Merge candidate connection (broadcast copy candidates)
  auto transform = [=](const std::vector<Tensor> &in) {
    Slice<3> mergeBegin = {batch, extendPartition, 0};
    Slice<3> mergeEnd = {batch + 1, extendPartition + 1, 1};
    std::vector<Tensor> slices(copyCandidates);
    for (unsigned i = 0; i < copyCandidates; i++) {
      slices[i] = in[i].slice(mergeBegin, mergeEnd);
    }
    return concat(slices).flatten();
  };
  graph.connect(vertex["copyCandidateAddend"],
                transform(tempTensors.mergeCandidatesAddend));

  // Extend candidate connection
  const auto numAddends = tempTensors.extendCandidatesParent.dim(1);
  Slice<3> begin = {batch, 0, extendPartition};
  Slice<3> end = {batch + 1, numAddends, extendPartition + 1};
  graph.connect(
      vertex["extendCandidateBeamProbTotal"],
      tempTensors.selectExtendCandidatesPTotal.slice(begin, end).flatten());
  graph.connect(
      vertex["extendCandidateAddend"],
      tempTensors.selectExtendCandidatesAddend.slice(begin, end).flatten());

  // Complete flag connection
  attachCompleteFlag(graph, tempTensors, batch, beamPartition, vertex);

  // Constants
  graph.setInitialValue(vertex["numCopyCandidates"], copyCandidates);
  graph.setInitialValue(vertex["blankClass"], blankClass);
}

void simpleSortCandidatesVertex(Graph &graph, const TempTensors &tempTensors,
                                ComputeSet &cs, unsigned batch,
                                unsigned partition,
                                unsigned candidatesToCompare,
                                unsigned beamwidth, unsigned tile) {

  const auto partialsType = tempTensors.mergeCandidatesPb[0].elementType();
  const auto vertexName = templateVertex("popnn::CTCSimpleSortCandidates",
                                         partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex on tile {}", vertexName, tile);
  graph.setTileMapping(vertex, tile);

  // Connect candidates, the vertex needs correctly ordered slices of the
  // original copy candidates followed by all extend candidates.
  // Sorted result is in the original copy candidates
  auto gatherCandidates = [=](const Tensor &copyIn, const Tensor &extendIn) {
    Slice<3> copyBegin = {batch, 0, 0};
    Slice<3> copyEnd = {batch + 1, beamwidth, 1};
    return concat(copyIn.slice(copyBegin, copyEnd).flatten(),
                  extendIn[batch].flatten());
  };
  const auto parents = gatherCandidates(tempTensors.copyCandidatesParent,
                                        tempTensors.extendCandidatesParent);
  const auto addends = gatherCandidates(tempTensors.copyCandidatesAddend,
                                        tempTensors.extendCandidatesAddend);
  const auto pnb = gatherCandidates(tempTensors.copyCandidatesPnb,
                                    tempTensors.extendCandidatesPnb);
  const auto pb = gatherCandidates(tempTensors.copyCandidatesPb,
                                   tempTensors.extendCandidatesPb);
  const auto pTotal =
      gatherCandidates(tempTensors.copyCandidatesPTotal,
                       tempTensors.selectExtendCandidatesPTotal);

  graph.connect(vertex["candidateParent"], parents);
  graph.connect(vertex["candidateAddend"], addends);
  graph.connect(vertex["candidateBeamProbNonBlank"], pnb);
  graph.connect(vertex["candidateBeamProbBlank"], pb);
  graph.connect(vertex["candidateBeamProbTotal"], pTotal);

  // Complete flag connection
  attachCompleteFlag(graph, tempTensors, batch, partition, vertex);

  // Constants
  graph.setInitialValue(vertex["beamwidth"], beamwidth);
  graph.setInitialValue(vertex["totalCandidates"], candidatesToCompare);
}

void rankCandidatesVertex(Graph &graph, const TempTensors &tempTensors,
                          const SortTensors &sortTensors, ComputeSet &cs,
                          unsigned batch, unsigned partition,
                          unsigned beamPartition,
                          const Interval &candidatesToCompare,
                          const Interval &rangeToRank, unsigned beamwidth,
                          unsigned tile) {

  const auto partialsType = tempTensors.mergeCandidatesPb[0].elementType();
  const auto vertexName =
      templateVertex("popnn::CTCRankCandidates", partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex for candidates in range {}"
                        " ranked against {} on tile {}",
                        vertexName, rangeToRank, candidatesToCompare, tile);
  graph.setTileMapping(vertex, tile);

  auto inCandidates = [&](const Tensor &in) {
    return in[batch]
        .slice(candidatesToCompare.begin(), candidatesToCompare.end(), 0)
        .flatten();
  };

  graph.connect(vertex["candidateParent"],
                inCandidates(sortTensors.inCandidatesParent));
  graph.connect(vertex["candidateAddend"],
                inCandidates(sortTensors.inCandidatesAddend));
  graph.connect(vertex["candidateBeamProbNonBlank"],
                inCandidates(sortTensors.inCandidatesPnb));
  graph.connect(vertex["candidateBeamProbBlank"],
                inCandidates(sortTensors.inCandidatesPb));
  graph.connect(vertex["candidateBeamProbTotal"],
                inCandidates(sortTensors.inCandidatesPTotal));

  // Result candidates
  graph.connect(vertex["rankedCandidateParent"],
                sortTensors.rankedCandidatesParent[batch][partition].flatten());
  graph.connect(vertex["rankedCandidateAddend"],
                sortTensors.rankedCandidatesAddend[batch][partition].flatten());
  graph.connect(vertex["rankedCandidateBeamProbNonBlank"],
                sortTensors.rankedCandidatesPnb[batch][partition].flatten());
  graph.connect(vertex["rankedCandidateBeamProbBlank"],
                sortTensors.rankedCandidatesPb[batch][partition].flatten());
  graph.connect(vertex["rankedCandidateBeamProbTotal"],
                sortTensors.rankedCandidatesPTotal[batch][partition].flatten());

  // Complete flag connection
  attachCompleteFlag(graph, tempTensors, batch, beamPartition, vertex);

  // Constants
  graph.setInitialValue(vertex["beamwidth"], beamwidth);
  graph.setInitialValue(vertex["totalCandidates"], candidatesToCompare.size());
  graph.setInitialValue(vertex["firstCandidateToRank"],
                        rangeToRank.begin() - candidatesToCompare.begin());
  graph.setInitialValue(vertex["lastCandidateToRank"],
                        rangeToRank.end() - candidatesToCompare.begin());
}

void reduceCandidatesVertex(Graph &graph, const TempTensors &tempTensors,
                            const SortTensors &sortTensors, ComputeSet &cs,
                            unsigned batch, unsigned group, unsigned partition,
                            unsigned beamPartition,
                            const Interval &candidatesToReduce, unsigned tile) {

  const auto partialsType = tempTensors.mergeCandidatesPb[0].elementType();
  const auto vertexName =
      templateVertex("popnn::CTCReduceCandidates", partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex for beam {} reducing {} on tile {}",
                        vertexName, partition, candidatesToReduce, tile);
  graph.setTileMapping(vertex, tile);

  auto gatherCandidates = [=](const Tensor &in) {
    Slice<3> begin = {batch, candidatesToReduce.begin(), partition};
    Slice<3> end = {batch + 1, candidatesToReduce.end(), partition + 1};
    return in.slice(begin, end).flatten();
  };

  graph.connect(vertex["candidateParent"],
                gatherCandidates(sortTensors.rankedCandidatesParent));
  graph.connect(vertex["candidateAddend"],
                gatherCandidates(sortTensors.rankedCandidatesAddend));
  graph.connect(vertex["candidateBeamProbNonBlank"],
                gatherCandidates(sortTensors.rankedCandidatesPnb));
  graph.connect(vertex["candidateBeamProbBlank"],
                gatherCandidates(sortTensors.rankedCandidatesPb));
  graph.connect(vertex["candidateBeamProbTotal"],
                gatherCandidates(sortTensors.rankedCandidatesPTotal));

  // Result candidates
  auto resultCandidates = [=](const Tensor &in) {
    Slice<2> begin = {batch, group};
    Slice<2> end = {batch + 1, group + 1};
    return in.slice(begin, end).reshape({});
  };

  graph.connect(vertex["reducedCandidateParent"],
                resultCandidates(sortTensors.outCandidatesParent));
  graph.connect(vertex["reducedCandidateAddend"],
                resultCandidates(sortTensors.outCandidatesAddend));
  graph.connect(vertex["reducedCandidateBeamProbNonBlank"],
                resultCandidates(sortTensors.outCandidatesPnb));
  graph.connect(vertex["reducedCandidateBeamProbBlank"],
                resultCandidates(sortTensors.outCandidatesPb));
  graph.connect(vertex["reducedCandidateBeamProbTotal"],
                resultCandidates(sortTensors.outCandidatesPTotal));
  // Constants
  graph.setInitialValue(vertex["totalCandidates"], candidatesToReduce.size());
}

void updateVertex(Graph &graph, const BeamTensors &beams,
                  const TempTensors &tempTensors, ComputeSet &cs,
                  unsigned batch, const Interval &time, unsigned beamPartition,
                  unsigned beamwidth, unsigned tile) {

  const auto partialsType = tempTensors.mergeCandidatesPb[0].elementType();
  const auto vertexName =
      templateVertex("popnn::CTCUpdate", partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex on tile {}", vertexName, tile);
  graph.setTileMapping(vertex, tile);

  // Beam connection
  attachBeamScalars(graph, beams, batch, beamPartition, beamwidth,
                    BeamScalars::BLANK_AND_NON_BLANK, vertex);
  attachBeamHistory(graph, beams, time, batch, beamPartition, beamwidth,
                    vertex);
  // Timestep, data length connections
  attachTimeAndLength(graph, tempTensors, batch, beamPartition, vertex);

  // Candidate connection
  // Connect candidates, the vertex needs correctly ordered slices of the
  // sorted result, held in the original copy candidates.
  auto transform = [=](const Tensor &in) {
    Slice<3> copyBegin = {batch, 0, 0};
    Slice<3> copyEnd = {batch + 1, beamwidth, 1};
    return in.slice(copyBegin, copyEnd).flatten();
  };
  graph.connect(vertex["candidateParent"],
                transform(tempTensors.copyCandidatesParent));
  graph.connect(vertex["candidateAddend"],
                transform(tempTensors.copyCandidatesAddend));
  graph.connect(vertex["candidateBeamProbNonBlank"],
                transform(tempTensors.copyCandidatesPnb));
  graph.connect(vertex["candidateBeamProbBlank"],
                transform(tempTensors.copyCandidatesPb));
  graph.connect(vertex["candidateBeamProbTotal"],
                transform(tempTensors.copyCandidatesPTotal));

  // Constants
  graph.setInitialValue(vertex["beamwidth"], beamwidth);
}

void generateOutputVertex(Graph &graph, const BeamTensors &beams,
                          const TempTensors &tempTensors, const Tensor &labels,
                          const Tensor &labelLengths, ComputeSet &cs,
                          unsigned batch, unsigned path, unsigned partition,
                          unsigned beamwidth, unsigned numClassesIncBlank,
                          unsigned tile) {

  const auto maxT = labels.dim(2);

  const auto vertexName =
      templateVertex("popnn::CTCGenerateOutput", UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex for beam output {} on tile {}",
                        vertexName, path, tile);
  graph.setTileMapping(vertex, tile);

  graph.connect(
      vertex["beamOutput"],
      labels.slice({batch, path, 0}, {batch + 1, path + 1, maxT}).flatten());
  graph.connect(
      vertex["outputLength"],
      labelLengths.slice({batch, path}, {batch + 1, path + 1}).reshape({}));

  attachBeamHistory(graph, beams, {0, maxT + 1}, batch, partition, beamwidth,
                    vertex);

  // Input data length connection
  graph.connect(vertex["dataLength"],
                tempTensors.dataLengths[batch][partition][0]);

  // Constants
  graph.setInitialValue(vertex["beam"], path);
  graph.setInitialValue(vertex["beamwidth"], beamwidth);
}

} // namespace ctc_infer
} // end namespace popnn
