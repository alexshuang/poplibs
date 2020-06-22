// Copyright (c) 2020 Graphcore Ltd. All rights reserved.

#include "HyperGraphBlockZoltan.hpp"
#include "poplibs_support/logging.hpp"
#include <algorithm>
#include <vector>

#define DEBUG_INFO 0

namespace logging = poplibs_support::logging;

namespace popsparse {
namespace experimental {

HyperGraphBlockZoltan::HyperGraphBlockZoltan(
    const BlockMatrix &A, const BlockMatrix &B, poplar::Type inDataTypeIn,
    poplar::Type outDataTypeIn, poplar::Type partialDataTypeIn, int nTileIn,
    float memoryCycleRatioIn, int nMulNodesSplitFactorIn)
    : HyperGraphBlock(A, B, inDataTypeIn, outDataTypeIn, partialDataTypeIn,
                      nTileIn, memoryCycleRatioIn, nMulNodesSplitFactorIn) {

  partitioner = std::make_unique<ZoltanPartitioner>(
      ZoltanPartitioner::PartitionType::HYPERGRAPH);

  logging::info("HyperGraphBlockZoltan is created");
}

HyperGraphData HyperGraphBlockZoltan::getDataForPartitioner() {
  logging::info("Number of nodes in A: {}", nodeA.size());
  logging::info("Number of nodes in B: {}", nodeB.size());
  logging::info("Number of nodes in V: {}", nodeV.size());

  HyperGraphData graph;

  graph.nodes = nodeA.size() + nodeB.size() + nodeC.size() + nodeV.size();

  // The pins vector stores the indices of the vertices in each of the edges.
  std::vector<unsigned int> pins;

  // The hyperedge vector stores the offsets into the pins vector.
  std::vector<unsigned int> hyperEdges;
  std::vector<float> weights(graph.nodes, 0.0F);

  for (const auto &n : nodeA) {
    weights[n.id] = n.w;
  }

  for (const auto &n : nodeB) {
    weights[n.id] = n.w;
  }

  for (const auto &n : nodeV) {
    weights[n.id] = n.w;
  }

  hyperEdges.reserve(edgeA.size() + edgeB.size());

  for (const auto &e : edgeA) {
    std::vector<unsigned int> v(e.in);

    if (!e.out.empty()) {
      v.insert(v.end(), e.out.begin(), e.out.end());
    }

    std::sort(v.begin(), v.end());
    hyperEdges.push_back(pins.size());
    pins.insert(pins.end(), v.begin(), v.end());
  }

  for (const auto &e : edgeB) {
    std::vector<unsigned int> v(e.in);

    if (!e.out.empty()) {
      v.insert(v.end(), e.out.begin(), e.out.end());
    }

    std::sort(v.begin(), v.end());
    hyperEdges.push_back(pins.size());
    pins.insert(pins.end(), v.begin(), v.end());
  }

  logging::info("Number of pins is {}", pins.size());
  logging::info("Number of edges is {}", hyperEdges.size());

  graph.pins = std::move(pins);
  graph.hyperEdges = std::move(hyperEdges);
  graph.weights = std::move(weights);

  return graph;
}

void HyperGraphBlockZoltan::partitionGraph() {
  HyperGraphData data = getDataForPartitioner();
  partitioner->partitionGraph(data, nTile, tileAssignment);
}

} // namespace experimental
} // namespace popsparse