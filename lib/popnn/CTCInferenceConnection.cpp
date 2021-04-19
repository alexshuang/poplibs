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

void attachBeamScalars(Graph &graph, const BeamTensors &beams, unsigned batch,
                       unsigned partition, unsigned beamwidth,
                       const VertexRef &vertex) {

  Slice<4> begin = {batch, partition, 0, 0};
  Slice<4> end = {batch + 1, partition + 1, beamwidth, 1};
  graph.connect(vertex["beamProbBlank"], beams.pb.slice(begin, end).flatten());
  graph.connect(vertex["beamProbNonBlank"],
                beams.pnb.slice(begin, end).flatten());
  graph.connect(vertex["lastBeamOutputs"],
                beams.lastOutput.slice(begin, end).flatten());
}

void attachUpdateBeamScalars(Graph &graph, const BeamTensors &beams,
                             unsigned batch, unsigned partition,
                             unsigned beamwidth, const VertexRef &vertex) {
  attachBeamScalars(graph, beams, batch, partition, beamwidth, vertex);
  Slice<4> begin = {batch, partition, 0, 0};
  Slice<4> end = {batch + 1, partition + 1, beamwidth, 1};
  graph.connect(vertex["previousLastBeamOutputs"],
                beams.previousLastOutput.slice(begin, end).flatten());
}

void attachBeamHistory(Graph &graph, const BeamTensors &beams,
                       const Interval &time, unsigned batch, unsigned partition,
                       unsigned beamwidth, const VertexRef &vertex) {

  Slice<4> begin = {batch, partition, time.begin(), 0};
  Slice<4> end = {batch + 1, partition + 1, time.end(), beamwidth};
  graph.connect(vertex["beamAddend"], beams.addend.slice(begin, end).flatten());
  graph.connect(vertex["beamParent"], beams.parent.slice(begin, end).flatten());
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
  graph.connect(vertex["extendCandidateBeamProbBlank"],
                tempTensors.extendCandidatesPb.slice(begin, end).flatten());
}

void attachData(Graph &graph, const Tensor &data, unsigned batch,
                unsigned partition, unsigned numClasses, const Interval &time,
                const VertexRef &vertex) {
  Slice<4> begin = {batch, partition, time.begin(), 0};
  Slice<4> end = {batch + 1, partition + 1, time.end(), numClasses};
  graph.connect(vertex["logProbs"], data.slice(begin, end).flatten());
}

void attachTimeAndLength(Graph &graph, const TempTensors &tempTensors,
                         unsigned batch, unsigned partition,
                         const VertexRef &vertex) {
  graph.connect(vertex["currentTimestep"],
                tempTensors.currentTimestep[batch][partition][0]);
  graph.connect(vertex["dataLength"],
                tempTensors.dataLengths[batch][partition][0]);
}

} // namespace
namespace popnn {
namespace ctc_infer {

void generateExtendCandidateVertex(
    Graph &graph, const Tensor &data, const BeamTensors &beams,
    const TempTensors &tempTensors, ComputeSet &cs, unsigned batch,
    const Interval &time, unsigned addendPartition, unsigned blankClass,
    unsigned beamwidth, const Interval &beamPartition, unsigned addendClass,
    unsigned tile) {

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
  attachData(graph, data, batch, addendPartition, numClasses, time, vertex);
  // Beam connection
  attachBeamScalars(graph, beams, batch, addendPartition, beamwidth, vertex);
  // Timestep, data length connection
  attachTimeAndLength(graph, tempTensors, batch, addendPartition, vertex);
  // Extend candidate connection
  attachGenerateExtendCandidates(graph, tempTensors, batch, addendPartition,
                                 beamPartition, vertex);
  // Constants
  graph.setInitialValue(vertex["numClassesIncBlank"], numClasses);
  graph.setInitialValue(vertex["blankClass"], blankClass);
  graph.setInitialValue(vertex["startBeam"], beamPartition.begin());
  graph.setInitialValue(vertex["endBeam"], beamPartition.end());
  graph.setInitialValue(vertex["addendSymbol"], addendClass);
}

void generateCopyCandidateVertex(Graph &graph, const Tensor &data,
                                 const BeamTensors &beams,
                                 const TempTensors &tempTensors, ComputeSet &cs,
                                 unsigned batch, const Interval &time,
                                 unsigned beamPartition, unsigned blankClass,
                                 unsigned beamwidth, unsigned tile) {

  const auto partialsType = beams.pb.elementType();
  const auto vertexName =
      templateVertex("popnn::CTCGenerateCopyCandidates", data.elementType(),
                     partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex on tile {}", vertexName, tile);
  graph.setTileMapping(vertex, tile);

  // Data connection
  const auto numClasses = data.dim(3);
  attachData(graph, data, batch, beamPartition, numClasses, time, vertex);
  // Beam connection
  attachBeamScalars(graph, beams, batch, beamPartition, beamwidth, vertex);
  // Timestep, data length connection
  attachTimeAndLength(graph, tempTensors, batch, beamPartition, vertex);
  // Copy candidate connection
  attachSingleCopyCandidate(graph, tempTensors, batch, beamPartition, vertex);
  // Constants
  graph.setInitialValue(vertex["numClassesIncBlank"], numClasses);
  graph.setInitialValue(vertex["blankClass"], blankClass);
  graph.setInitialValue(vertex["beamwidth"], beamwidth);
  graph.setInitialValue(vertex["beamIdx"], beamPartition);
}

void mergeCandidateVertex(Graph &graph, const BeamTensors &beams,
                          const TempTensors &tempTensors, ComputeSet &cs,
                          unsigned batch, const Interval &time,
                          unsigned extendPartition, unsigned beamPartition,
                          unsigned blankClass, unsigned beamwidth,
                          unsigned tile) {

  const auto partialsType = beams.pb.elementType();
  const auto extendCandidates = tempTensors.extendCandidatesParent.dim(1);

  const auto vertexName =
      templateVertex("popnn::CTCMergeCandidates", partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex for copy {}, extend {},"
                        " candidates {}, on tile {}",
                        vertexName, beamPartition, extendPartition,
                        extendCandidates, tile);
  graph.setTileMapping(vertex, tile);

  // Extend candidate connection
  attachMergeExtendCandidates(graph, tempTensors, batch, extendPartition,
                              vertex);

  // Merge candidate connection (a single broadcast copy candidate)
  auto transform = [=](const std::vector<Tensor> &in) {
    Slice<3> mergeBegin = {batch, extendPartition, 0};
    Slice<3> mergeEnd = {batch + 1, extendPartition + 1, 1};
    return in[beamPartition].slice(mergeBegin, mergeEnd).reshape({});
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
  attachBeamHistory(graph, beams, time, batch, extendPartition, beamwidth,
                    vertex);
  // The last output of the beam that the copy candidate came from
  graph.connect(vertex["lastBeamOutput"],
                beams.lastOutput[batch][extendPartition][beamPartition][0]);

  // Timestep, data length connection
  attachTimeAndLength(graph, tempTensors, batch, extendPartition, vertex);

  // Constants
  graph.setInitialValue(vertex["extendCandidates"], extendCandidates);
  graph.setInitialValue(vertex["beamwidth"], beamwidth);
  graph.setInitialValue(vertex["blankClass"], blankClass);
}

void selectCopyCandidateVertex(Graph &graph, const TempTensors &tempTensors,
                               ComputeSet &cs, unsigned batch,
                               unsigned copyPartition, unsigned copyCandidates,
                               unsigned tile) {

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

  // Timestep, data length connection
  attachTimeAndLength(graph, tempTensors, batch, copyPartition, vertex);

  // Constants
  graph.setInitialValue(vertex["numCandidates"], copyCandidates);
}

void selectExtendCandidateVertex(Graph &graph, const TempTensors &tempTensors,
                                 ComputeSet &cs, unsigned batch,
                                 unsigned extendPartition,
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
  graph.connect(vertex["extendCandidateBeamProbTotal"],
                tempTensors.extendCandidatesPTotal.slice(begin, end).flatten());

  // Timestep, data length connection
  attachTimeAndLength(graph, tempTensors, batch, extendPartition, vertex);

  // Constants
  graph.setInitialValue(vertex["numCopyCandidates"], copyCandidates);
}

void selectCandidatesVertex(Graph &graph, const TempTensors &tempTensors,
                            ComputeSet &cs, unsigned batch, unsigned partition,
                            unsigned candidatesToCompare, unsigned beamwidth,
                            unsigned tile) {

  const auto partialsType = tempTensors.mergeCandidatesPb[0].elementType();
  const auto vertexName =
      templateVertex("popnn::CTCSelectCandidates", partialsType, UNSIGNED_INT);
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
  const auto pTotal = gatherCandidates(tempTensors.copyCandidatesPTotal,
                                       tempTensors.extendCandidatesPTotal);

  graph.connect(vertex["candidateParent"], parents);
  graph.connect(vertex["candidateAddend"], addends);
  graph.connect(vertex["candidateBeamProbNonBlank"], pnb);
  graph.connect(vertex["candidateBeamProbBlank"], pb);
  graph.connect(vertex["candidateBeamProbTotal"], pTotal);

  // Timestep, data length connection (Only for early end)
  attachTimeAndLength(graph, tempTensors, batch, partition, vertex);

  // Constants
  graph.setInitialValue(vertex["beamwidth"], beamwidth);
  graph.setInitialValue(vertex["totalCandidates"], candidatesToCompare);
}

void updateVertex(Graph &graph, const BeamTensors &beams,
                  const TempTensors &tempTensors, ComputeSet &cs,
                  unsigned batch, const Interval &time, unsigned beamPartition,
                  unsigned sortedResultOffset, unsigned beamwidth,
                  unsigned tile) {

  const auto partialsType = tempTensors.mergeCandidatesPb[0].elementType();
  const auto vertexName =
      templateVertex("popnn::CTCUpdate", partialsType, UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex on tile {}", vertexName, tile);
  graph.setTileMapping(vertex, tile);

  // Beam connection
  attachUpdateBeamScalars(graph, beams, batch, beamPartition, beamwidth,
                          vertex);
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

  // Constants
  graph.setInitialValue(vertex["beamwidth"], beamwidth);
}

void generateOutputVertex(Graph &graph, const BeamTensors &beams,
                          const TempTensors &tempTensors, const Tensor &labels,
                          const Tensor &labelLengths, ComputeSet &cs,
                          unsigned batch, unsigned beamwidth,
                          unsigned partition, unsigned path, unsigned tile) {

  const auto maxT = labels.dim(2);

  const auto vertexName =
      templateVertex("popnn::CTCGenerateOutput", UNSIGNED_INT);
  const auto vertex = graph.addVertex(cs, vertexName);
  logging::popnn::trace("Making {} vertex on tile {}", vertexName, tile);
  graph.setTileMapping(vertex, tile);

  graph.connect(
      vertex["beamOutput"],
      labels.slice({batch, path, 0}, {batch + 1, path + 1, maxT}).flatten());
  graph.connect(
      vertex["outputLength"],
      labelLengths.slice({batch, path}, {batch + 1, path + 1}).reshape({}));

  attachBeamHistory(graph, beams, {0, maxT + 1}, batch, partition, beamwidth,
                    vertex);

  // Timestep connection
  graph.connect(vertex["currentTimestep"],
                tempTensors.currentTimestep[batch][partition][0]);

  // Constants
  graph.setInitialValue(vertex["beam"], path);
  graph.setInitialValue(vertex["maxT"], maxT);
  graph.setInitialValue(vertex["beamwidth"], beamwidth);
}

} // namespace ctc_infer
} // end namespace popnn
