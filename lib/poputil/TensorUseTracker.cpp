// Copyright (c) 2019 Graphcore Ltd. All rights reserved.
#include "poputil/TileMapping.hpp"

#include "poputil/Util.hpp"
#include "poputil/exceptions.hpp"

#include <boost/icl/interval_map.hpp>
#include <iterator>
#include <unordered_map>

namespace poputil {

class TensorUseTrackerState {
public:
  using TileUsage = std::vector<boost::icl::interval_set<unsigned>>;
  std::unordered_map<poplar::VariableRef, TileUsage> usage;
  unsigned numTiles;
  TensorUseTrackerState(unsigned numTiles) : numTiles(numTiles) {}
  TensorUseTrackerState(const TensorUseTrackerState &other) = default;
  TileUsage &getUsage(poplar::VariableRef v) {
    auto m = usage.find(v);
    if (m != usage.end())
      return m->second;
    return usage.emplace(v, TileUsage(numTiles)).first->second;
  }
};

static boost::icl::interval<unsigned>::type
toIclInterval(const poplar::Interval &interval) {
  return boost::icl::interval<unsigned>::right_open(interval.begin(),
                                                    interval.end());
}

TensorUseTracker::TensorUseTracker(unsigned numTiles) {
  st = std::unique_ptr<TensorUseTrackerState>(
      new TensorUseTrackerState(numTiles));
}

TensorUseTracker::TensorUseTracker(const TensorUseTracker &other)
    : st(new TensorUseTrackerState(*other.st)) {}

TensorUseTracker::TensorUseTracker(TensorUseTracker &&other) = default;

TensorUseTracker &TensorUseTracker::operator=(const TensorUseTracker &other) {
  st.reset(new TensorUseTrackerState(*other.st));
  return *this;
}

TensorUseTracker &
TensorUseTracker::operator=(TensorUseTracker &&other) = default;

TensorUseTracker::~TensorUseTracker() {}

void TensorUseTracker::add(const poplar::Graph &graph, unsigned tile,
                           const poplar::Tensor &t_) {
  auto t = t_.flatten();
  graph.reorderToSimplify(&t, {}, false);
  const auto varRegions = t.getVarRegions();
  for (const auto &region : varRegions) {
    if (graph.isConstant(region.var))
      continue;
    auto &usage = st->getUsage(region.var);
    usage[tile].add(toIclInterval(region.interval));
  }
}

void TensorUseTracker::add(TensorUseTracker other) {
  if (other.st->numTiles != st->numTiles) {
    throw poputil::poplibs_error("Trying to add tensor use tracker state "
                                 "with differing no. of tiles");
  }
  for (auto &entry : other.st->usage) {
    const auto &varRef = entry.first;
    auto &otherVarUse = entry.second;
    assert(!otherVarUse.empty());
    auto &varUse = st->usage[varRef];
    if (varUse.empty()) {
      varUse = std::move(otherVarUse);
    } else {
      for (unsigned tile = 0; tile < otherVarUse.size(); ++tile) {
        if (varUse[tile].empty()) {
          varUse[tile] = std::move(otherVarUse[tile]);
        } else {
          varUse[tile] += std::move(otherVarUse[tile]);
        }
      }
    }
  }
}

/// Extend a partial map to a total map in the range [lower, upper). The value
/// of keys not in the partial map are based on the value of the neighbouring
/// keys that are in the map. The partial map must contain at least one entry.
template <class K, class V>
static void extendPartialMap(boost::icl::interval_map<K, V> &map, K lower,
                             K upper) {
  assert(iterative_size(map) > 0);
  boost::icl::interval_map<K, V> extendedMap;
  for (auto begin = map.begin(), it = begin, end = map.end(); it != end; ++it) {
    const auto &interval = it->first;
    auto next = std::next(it);
    auto extendedIntervalLower = it == begin ? lower : interval.lower();
    auto extendedIntervalUpper = next == end ? upper : next->first.lower();
    auto extendedInterval = boost::icl::interval<unsigned>::right_open(
        extendedIntervalLower, extendedIntervalUpper);
    extendedMap.insert({extendedInterval, std::move(it->second)});
  }
  std::swap(map, extendedMap);
}

static bool isHaloRegion(const std::set<unsigned> &prevTiles,
                         const std::set<unsigned> &tiles,
                         const std::set<unsigned> &nextTiles) {
  if (prevTiles.size() + nextTiles.size() != tiles.size())
    return false;
  return std::includes(tiles.begin(), tiles.end(), prevTiles.begin(),
                       prevTiles.end()) &&
         std::includes(tiles.begin(), tiles.end(), nextTiles.begin(),
                       nextTiles.end());
}

static void mergeIntersectingTileGroups(
    boost::icl::interval_map<unsigned, std::set<unsigned>> &map) {
  if (map.empty()) {
    return;
  }
  boost::icl::interval_map<unsigned, std::set<unsigned>> optimizedMap;

  auto it = map.begin();
  std::set<unsigned> mergedSet = it->second;
  unsigned mergedRegionBegin = it->first.lower();
  unsigned mergedRegionEnd = it->first.upper();

  while (++it != map.end()) {
    // union of already merged tile groups and next
    std::set<unsigned> setUnion;
    // check if adjacent tile sets intersect
    std::set_union(mergedSet.begin(), mergedSet.end(), it->second.begin(),
                   it->second.end(), std::inserter(setUnion, setUnion.begin()));
    if (mergedSet.size() + it->second.size() == setUnion.size()) {
      // If there is no intersection we just we just insert the entry into the
      // optimised map
      optimizedMap.insert({boost::icl::interval<unsigned>::right_open(
                               mergedRegionBegin, mergedRegionEnd),
                           mergedSet});
      mergedSet = it->second;
      mergedRegionBegin = it->first.lower();
      mergedRegionEnd = it->first.upper();
    } else {
      // else we continue to check if further tile groups need to be added
      mergedSet = setUnion;
      mergedRegionEnd = it->first.upper();
    }
  }

  optimizedMap.insert({boost::icl::interval<unsigned>::right_open(
                           mergedRegionBegin, mergedRegionEnd),
                       mergedSet});

  std::swap(map, optimizedMap);
}

static void optimizeHaloMapping(
    boost::icl::interval_map<unsigned, std::set<unsigned>> &map) {
  // Modify the map so that "halo" regions where the uses are the union of the
  // uses of the neighbouring regions are mapped as if they were only used by
  // one of the sets of tiles. This heuristic reduces exchange code for
  // convolutional layers since the halos tend to be small and mapping them
  // independently splits up the tensor tile mapping, increasing the amount of
  // exchange code required.
  boost::icl::interval_map<unsigned, std::set<unsigned>> optimizedMap;
  for (auto begin = map.begin(), it = begin, end = map.end(); it != end; ++it) {
    if (it != begin && std::next(it) != end &&
        isHaloRegion(std::prev(it)->second, it->second,
                     std::next(it)->second)) {
      optimizedMap.insert({it->first, it == begin ? std::next(it)->second
                                                  : std::prev(it)->second});
    } else {
      optimizedMap.insert(*it);
    }
  }
  std::swap(map, optimizedMap);
}

void TensorUseTracker::resolve(const poplar::Graph &graph, unsigned grainSize,
                               unsigned minElementsPerTile,
                               bool extendPartialUsage,
                               TensorUseTracker::MappingMethod mappingMethod) {
  using TileUseInterval = boost::icl::interval<unsigned>;
  const auto numTiles = graph.getTarget().getNumTiles();

  unsigned sharedGrainSize;
  if (mappingMethod ==
      TensorUseTracker::MappingMethod::ConstrainMappingToUsedTiles) {
    sharedGrainSize = 1;
  } else {
    sharedGrainSize = grainSize;
    grainSize = 1;
  }

  for (auto &usageEntry : st->usage) {
    const auto t = graph.getVariable(usageEntry.first);
    auto &usage = usageEntry.second;
    boost::icl::interval_map<unsigned, std::set<unsigned>> uses;
    for (unsigned tile = 0; tile < usage.size(); ++tile) {
      std::set<unsigned> tileSet{tile};
      for (const auto &region : usage[tile]) {
        uses.add(std::make_pair(region, tileSet));
      }
    }
    assert(iterative_size(uses) != 0);

    usage.clear();
    usage.resize(numTiles);

    boost::icl::interval_map<unsigned, std::set<unsigned>> grainToTiles;
    for (const auto &entry : uses) {
      const auto &interval = entry.first;
      unsigned grainLower = interval.lower() / sharedGrainSize;
      unsigned grainUpper = (interval.upper() - 1) / sharedGrainSize + 1;
      auto grainInterval = TileUseInterval::right_open(grainLower, grainUpper);
      grainToTiles.insert({grainInterval, entry.second});
    }

    const auto numElements = t.numElements();
    if (extendPartialUsage) {
      // Extend the grainUses map to cover the entire tensor.
      const unsigned numGrains =
          (numElements + sharedGrainSize - 1) / sharedGrainSize;
      extendPartialMap(grainToTiles, 0U, numGrains);
    }

    switch (mappingMethod) {
    case TensorUseTracker::MappingMethod::OptimizeHaloRegions:
      optimizeHaloMapping(grainToTiles);
      break;
    case TensorUseTracker::MappingMethod::ConstrainMappingToUsedTiles:
      mergeIntersectingTileGroups(grainToTiles);
      break;
    case TensorUseTracker::MappingMethod::None:
      break;
    }

    // Build a map from sets of tiles to grains they use.
    std::map<std::set<unsigned>, std::vector<poplar::Interval>> tilesToGrains;
    for (const auto &entry : grainToTiles) {
      tilesToGrains[entry.second].emplace_back(entry.first.lower(),
                                               entry.first.upper());
    }
    const auto minGrainsPerTile =
        (minElementsPerTile + sharedGrainSize - 1) / sharedGrainSize;
    for (const auto &entry : tilesToGrains) {
      const auto &tiles = entry.first;
      const auto &sharedGrains = entry.second;
      const auto perTileGrains =
          splitRegions(sharedGrains, grainSize, tiles.size(), minGrainsPerTile);
      unsigned i = 0;
      for (auto tile : tiles) {
        if (i == perTileGrains.size())
          break;
        for (const auto &interval : perTileGrains[i]) {
          const auto lower = interval.begin() * sharedGrainSize;
          const auto upper =
              std::min(interval.end() * sharedGrainSize, numElements);
          usage[tile] += TileUseInterval::right_open(lower, upper);
        }
        ++i;
      }
    }
  }
}

void TensorUseTracker::mapTensorsByUse(
    poplar::Graph &graph, unsigned grainSize, unsigned minElementsPerTile,
    bool extendPartialUsage, TensorUseTracker::MappingMethod mappingMethod) {
  resolve(graph, grainSize, minElementsPerTile, extendPartialUsage,
          mappingMethod);
  const auto numTiles = graph.getTarget().getNumTiles();

  for (const auto &usageEntry : st->usage) {
    const auto t = graph.getVariable(usageEntry.first);
    const auto &usage = usageEntry.second;

    std::vector<std::vector<poplar::Interval>> mapping(numTiles);
    for (unsigned tile = 0; tile < usage.size(); ++tile) {
      mapping[tile].reserve(usage[tile].iterative_size());
      for (const auto &interval : usage[tile]) {

        mapping[tile].emplace_back(interval.lower(), interval.upper());
      }
    }
    graph.setTileMapping(t, mapping);
  }
}

bool TensorUseTracker::empty() const { return st->usage.empty(); }

} // end namespace poputil
