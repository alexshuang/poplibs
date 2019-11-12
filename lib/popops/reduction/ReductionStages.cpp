#include "ReductionStages.hpp"
#include <iostream>

#include <algorithm>
#include <cassert>

#include <fstream>
#include <numeric>

#include <boost/icl/split_interval_map.hpp>
#include <boost/optional.hpp>
#include <boost/variant.hpp>

#include <poputil/TileMapping.hpp>
#include <poputil/VertexTemplates.hpp>
#include <poputil/exceptions.hpp>

#include "poplibs_support/logging.hpp"
#include <poplibs_support/Algorithms.hpp>
#include <poplibs_support/ContiguousRegionsByTile.hpp>
#include <poplibs_support/IclUtil.hpp>
#include <poplibs_support/print.hpp>
#include <poplibs_support/vv_iterator.hpp>

#include <popops/Cast.hpp>

#include "IntermediatePartialsUtil.hpp"
#include "ReductionConnection.hpp"
#include "RegionWrapping.hpp"

using namespace poplar;
using namespace poputil;
using namespace poplibs;
using namespace poplibs_support;

namespace popops {

// createElementRefsFromRegions: List a reference containing (region, offset)
// for each tensor element found in the intervals for each region.  These are
// arranged into a vector per column - ie output element to reduce into.
struct ElementRef {
  unsigned region;
  unsigned offset;
};
std::vector<std::vector<ElementRef>> createElementRefsFromRegions(
    const std::vector<std::vector<Interval>> &regions,
    std::vector<PartialsDescription> &partialsDescription,
    const unsigned columns, bool detectColumns) {
  std::vector<std::vector<ElementRef>> elementRefs(columns);

  for (unsigned r = 0; r < regions.size(); r++) {
    unsigned regionStartOffset = 0;
    for (unsigned i = 0; i < regions[r].size(); i++) {
      for (unsigned e = 0; e < regions[r][i].size(); e++) {
        // Examine the column number of every element on tile.  Append it to the
        // vector of elements for that column
        const unsigned column = (regions[r][i].begin() + e) % columns;
        elementRefs[column].push_back({r, regionStartOffset});
        regionStartOffset++;
      }
    }
  }
  // Largely to make test cases simple to understand, we may only be interested
  // in certain columns.  Omit those that are not of interest.
  if (!detectColumns) {
    for (unsigned i = 0; i < columns; i++) {
      bool useColumn = false;
      for (unsigned j = 0; j < partialsDescription.size(); j++) {
        if (partialsDescription[j].columns[0] == i) {
          useColumn = true;
        }
      }
      if (!useColumn) {
        elementRefs[i].resize(0);
      }
    }
  }
  return elementRefs;
}

// updatePartialsDescription: Given a "signal" indicating that the colomn
// of interest is/is not detected in the region, update the partialsDescription
// structure.

struct PatternBuildState {
  bool patternColumnEnd;
  unsigned patternColumnRef;
  bool buildingPattern = false;
};

void updatePartialsDescription(PatternBuildState &pbs, PartialsDescription &rt,
                               bool thisColumnFound, unsigned region,
                               unsigned elementOffset, bool isRegionEnd) {

  if (thisColumnFound && !pbs.buildingPattern) {
    // The first pattern in this region
    pbs.patternColumnRef = elementOffset;
    rt.patterns.push_back({0, pbs.patternColumnRef, 0, 0, region});
    pbs.patternColumnEnd = false;
    pbs.buildingPattern = true;
  }
  if (pbs.buildingPattern) {
    auto length = elementOffset - pbs.patternColumnRef;
    if (!pbs.patternColumnEnd && !thisColumnFound) {
      // Like a falling edge of the signal
      // "column == this reduction column"
      // Means the length can be created or checked
      if (rt.patterns.back().length) {
        if (rt.patterns.back().length != length) {
          // A new pattern as the "column == this reduction column"
          // signal was too long compared to the current pattern
          // Begin a fresh pattern as if the signal pulse was all part
          // of it
          // OR A new pattern as the signal was too short
          rt.patterns.push_back({length, pbs.patternColumnRef, 0, 0, region});
        }
      } else {
        // Initialise the length of a new pattern
        rt.patterns.back().length = length;
      }
      pbs.patternColumnEnd = true;
      rt.patterns.back().repetitions++;
    }
    if (thisColumnFound && pbs.patternColumnEnd) {
      // Like a rising edge of the signal
      // "column == this reduction column"
      // Means the stride can be created or checked
      pbs.patternColumnEnd = false;
      if (rt.patterns.back().stride) {
        if (rt.patterns.back().stride != length) {
          // The stride is inconsistent with the current pattern so
          // start a new pattern
          rt.patterns.push_back({0, elementOffset, 0, 0, region});
          pbs.buildingPattern = true;
        }
      } else {
        rt.patterns.back().stride = length;
      }
      pbs.patternColumnRef = elementOffset;
      // Update length to assist with end of region condition
      length = 0;
    }
    if (isRegionEnd) {
      if (pbs.buildingPattern && !pbs.patternColumnEnd) {
        if (rt.patterns.back().length) {
          if (rt.patterns.back().length == length + 1) {
            // Region ends nicely truncating the pattern at the
            // point of a "column == this reduction column" signal
            // "falling edge"
            rt.patterns.back().repetitions++;
          } else {
            // Truncated early - add a fresh pattern to describe it
            rt.patterns.push_back(
                {length + 1, pbs.patternColumnRef, 0, 1, region});
          }
        }
        if (rt.patterns.back().length == 0) {
          // Pattern length not yet been found:
          // "column == this reduction column" signal was = 1 throughout
          // the region or for a last separate pattern
          rt.patterns.back().length = length + 1;
          rt.patterns.back().repetitions = 1;
        }
      }
      // Fresh region will begin if there is one
      pbs.buildingPattern = false;
    }
  }
}

unsigned
initialisePatternStructs(PatternBuildState &patternBuildState,
                         std::vector<PartialsDescription> &partialsDescription,
                         const std::vector<ElementRef> &elementRefs,
                         bool detectColumns, const unsigned column) {
  // Is this the end of the region, if so complete the pattern accordingly
  bool regionEnd = false;
  if (elementRefs.size() == 1) {
    regionEnd = true;
  } else if (elementRefs[0].region != elementRefs[1].region) {
    regionEnd = true;
  }
  const unsigned lastOne = regionEnd ? 1u : 0u;
  // Create a pattern and complete a struct to look after updating it
  unsigned preDetIndex;
  if (detectColumns) {
    partialsDescription.push_back({{column},
                                   {{lastOne, elementRefs[0].offset, 0, lastOne,
                                     elementRefs[0].region}}});
  } else {
    for (preDetIndex = 0; preDetIndex < partialsDescription.size();
         preDetIndex++) {
      if (partialsDescription[preDetIndex].columns[0] == column) {
        break;
      }
    }
    partialsDescription[preDetIndex].patterns.push_back(
        {lastOne, elementRefs[0].offset, 0, lastOne, elementRefs[0].region});
  }

  patternBuildState = {false, elementRefs[0].offset, !regionEnd};
  return detectColumns ? partialsDescription.size() - 1 : preDetIndex;
}

// Reduction patterns describe the part of a contiguous region of data that is
// required by a given reduction.  See the definition of PartialsPattern and
// PartialsDescription for an explanation.
//
// In the description below we talk about a "signal" where "column == this
// reduction column".  In other words 1 = signal true, 0 = signal false in
// the examples.
//
//  Examples:
//  00111001110011100 1 pattern : len=3, sta=2, str=5, rep=3, reg=0
//
//  011100111010100 2 patterns : len=3, sta=1, str=5, rep=2, reg=0
//                               len=1, sta=10, str=2, rep=2, reg=0
//
//
// gatherReductionPatterns will scan the regions on tile and determine what data
// is required to reduce each column. It will create a PartialsDescription
// containing as many patterns as are required to describe that columns's data.
//
// If the partialsDescription vector is empty on entry it will automatically
// determine what columns have data on tile, otherwise it will look to the
// 'columns' entry within the partialsDescription and create patterns for those
// columns only. Either way each PartialsDescription will describe all
// of the elements for a particular column in the given regions.
//
// Note: the purpose of only finding selected column's data is for test, as the
//       results are clearer.

void gatherReductionPatterns(
    std::vector<PartialsDescription> &partialsDescription,
    const std::vector<std::vector<Interval>> &regions, unsigned columns) {

  // First list all references to each column in a vector of vectors: One outer
  // vector per column (ie output element from the reduction)
  const bool detectColumns = (partialsDescription.size() == 0);
  auto elementRefs = createElementRefsFromRegions(regions, partialsDescription,
                                                  columns, detectColumns);

  // Looking at each vector in turn, build a pattern.
  for (unsigned i = 0; i < elementRefs.size(); i++) {
    // elements belonging to this column were detected on tile
    if (elementRefs[i].size()) {

      PatternBuildState patternBuildState;
      // Create a pattern structure to deal with this, and return a reference
      // to it.  Initialise the pattern build state.
      auto currentPatternIdx =
          initialisePatternStructs(patternBuildState, partialsDescription,
                                   elementRefs[i], detectColumns, i);
      PartialsDescription &currentPattern =
          partialsDescription[currentPatternIdx];

      // Add the rest of the elements belonging to this column to the pattern
      for (unsigned j = 1; j < elementRefs[i].size(); j++) {

        const bool isNewRegion =
            elementRefs[i][j].region != elementRefs[i][j - 1].region;

        const bool nonColumnElementsExist =
            isNewRegion ||
            elementRefs[i][j].offset != elementRefs[i][j - 1].offset + 1;
        // Update the pattern for the presence of memory that isn't in its
        // column
        if (nonColumnElementsExist) {
          // Mark the end of the "column detected" signal with a single
          // element where column detect = false.  This could be because a
          // new region was found - in which case it updates due to the gap
          // between regions
          updatePartialsDescription(patternBuildState, currentPattern, false,
                                    elementRefs[i][j].region,
                                    elementRefs[i][j - 1].offset + 1,
                                    isNewRegion);
          if (!isNewRegion) {
            // If that didn't happen due to a region Change, then update the
            // pattern with the information that there were potentially many
            // elements with a "column detected" signal = 0
            updatePartialsDescription(patternBuildState, currentPattern, false,
                                      elementRefs[i][j].region,
                                      elementRefs[i][j].offset - 1, false);
          }
        }
        // Update the pattern for its own column, taking note of special case of
        // the end of the data on tile for this column
        const bool isLastElement = j == elementRefs[i].size() - 1;
        updatePartialsDescription(patternBuildState, currentPattern, true,
                                  elementRefs[i][j].region,
                                  elementRefs[i][j].offset, isLastElement);
      }
    }
  }
}

// Cleaner function for use below, which returns a PartialsDescription vector
// and therefore will always automatically determine all columns referenced in
// the  "regions".  The function above is mostly useful for test.
std::vector<PartialsDescription>
gatherReductionPatterns(const std::vector<std::vector<Interval>> &regions,
                        unsigned columns) {
  std::vector<PartialsDescription> partialsDescription;
  gatherReductionPatterns(partialsDescription, regions, columns);
  return partialsDescription;
}

void addPartialDebug(const PartialsDescription &partialsDescription,
                     RegionReduction &reduction, unsigned tile, unsigned start,
                     unsigned end, unsigned columns) {
  ReductionDebug::Partial di;
  di.sourceCols = {partialsDescription.columns[0],
                   partialsDescription.columns[0] +
                       partialsDescription.columns.size()};
  di.sourceRows = {start / columns, end / columns};
  di.sourceTile = tile;
  reduction.partialsDebugInfo.push_back(di);
}

// A function which accepts a vector of patterns which each describe
// a reduction of one or more columns. Each pattern references a region /
// regions and describes a number of tensor elements (partials) found within
// that region.
// The function adds references to the partials for each reduction into the
// "reductions" structure.
std::vector<RegionReduction> listPartialsUsingPatterns(
    const std::vector<PartialsDescription> &partialsDescription,
    const poplar::Tensor &input,
    const std::vector<std::vector<poplar::Interval>> &inputRegions,
    unsigned tile, unsigned columns) {
  // For speed, prepare a vector of tensors for each on tile region, each of
  // which will be referenced many times in the loop below.
  std::vector<Tensor> regionTensors(inputRegions.size());
  for (unsigned i = 0; i < inputRegions.size(); i++) {
    regionTensors[i] = concat(input.flatten().slices(inputRegions[i]));
  }

  std::vector<RegionReduction> reductions(partialsDescription.size());
  for (unsigned i = 0; i < reductions.size(); i++) {
    for (auto &pat : partialsDescription[i].patterns) {
      auto &partials = reductions[i].partials;

      auto &in = regionTensors[pat.regionIdx];
      if (pat.repetitions > 1) {
        if (pat.stride == partialsDescription[i].columns.size() &&
            pat.length == 1) {
          // If the sequence of columns repeats end to end with no gap in
          // memory we can create partials with a single slice.
          // (Note that this expression could be simplified as stride == no of
          // columns.  However the expression below is clearer)
          const auto end = pat.regionOffset +
                           pat.stride * (pat.repetitions - 1) +
                           partialsDescription[i].columns.size();
          partials.push_back(in.slice(pat.regionOffset, end));
          addPartialDebug(partialsDescription[i], reductions[i], tile,
                          pat.regionOffset, end, columns);
        } else {
          // If the patterns repeats and has "gaps"
          // (i.e. stride != no of columns) we need multiple slices to create
          // the partials.
          for (unsigned k = 0; k < pat.repetitions; k++) {
            const auto start = pat.regionOffset + k * pat.stride;
            const auto end = pat.regionOffset + k * pat.stride +
                             pat.length * partialsDescription[i].columns.size();
            partials.push_back(in.slice(start, end));
            addPartialDebug(partialsDescription[i], reductions[i], tile, start,
                            end, columns);
          }
        }
      } else {
        // If there are no pattern repetitions we can create partials with a
        // single silce.
        const auto end = pat.regionOffset +
                         pat.length * partialsDescription[i].columns.size();
        partials.push_back(in.slice(pat.regionOffset, end));
        addPartialDebug(partialsDescription[i], reductions[i], tile,
                        pat.regionOffset, end, columns);
      }
    }
  }
  return reductions;
}
// Function defining the criteria for two patterns to be adjacent - that is,
// they can be grouped together.  The two patterns need to be next to each other
// memory consistently each time the pattern repeats, and in every region the
// pattern appears in.  The actual column number is not important, so we can
// end up with a grouping of patterns from columns 3, 4, 6, 7 which lie
// sequentially in memory but are not numbered sequentially.
// We are always keeping complete columns together, never grouping parts of
// columns, even over separate regions.
bool isAdjacent(const PartialsDescription &a, const PartialsDescription &b,
                unsigned columns) {
  if (a.patterns.size() != b.patterns.size()) {
    return false;
  }
  for (unsigned i = 0; i < a.patterns.size(); i++) {
    if (a.patterns[i].regionOffset + a.patterns[i].length !=
            b.patterns[i].regionOffset ||
        a.patterns[i].length != b.patterns[i].length ||
        a.patterns[i].stride != b.patterns[i].stride ||
        a.patterns[i].repetitions != b.patterns[i].repetitions ||
        a.patterns[i].regionIdx != b.patterns[i].regionIdx) {
      return false;
    }
  }
  return true;
}
// Group partials operates on PartialsDescriptions, each of which contains
// information about the layout of a single column's data on tile.  It attempts
// to group any structures that describe columns which are "adjacent" - ie
// next to each other in memory and of consistent shape.  The "isAdjacent"
// function defines this.
std::vector<PartialsDescription>
groupPartials(std::vector<PartialsDescription> &partialsDescription,
              unsigned columns) {
  std::vector<PartialsDescription> groupedPartials;
  // Keep track of which patterns have been added to grouped patterns
  std::vector<bool> partialsDescriptionIsGrouped(partialsDescription.size(),
                                                 false);
  unsigned partialsDescriptionsToGroup = partialsDescription.size();
  for (unsigned i = 0;
       i < partialsDescription.size() && partialsDescriptionsToGroup; i++) {
    // If the next one hasn't been grouped already, put it into the
    // groupedPartials structure.
    if (partialsDescriptionIsGrouped[i] == false) {
      groupedPartials.push_back(partialsDescription[i]);
      groupedPartials.back().columns.resize(1);
      partialsDescriptionIsGrouped[i] = true;
      partialsDescriptionsToGroup--;

      // Scan the remaining ones for adjacent, matching patterns, append their
      // column to the column list and mark them as grouped
      for (unsigned j = i + 1; j < partialsDescription.size(); j++) {
        if (partialsDescriptionIsGrouped[j] == false) {
          if (isAdjacent(partialsDescription[i], partialsDescription[j],
                         columns)) {
            groupedPartials.back().columns.push_back(
                partialsDescription[j].columns[0]);
            partialsDescriptionIsGrouped[j] = true;
            partialsDescriptionsToGroup--;
            // Update offsets into the patterns so that we can continue to group
            // Overwrites the structure, but it's not needed anymore
            for (unsigned k = 0; k < partialsDescription[i].patterns.size();
                 k++) {
              partialsDescription[i].patterns[k].regionOffset +=
                  partialsDescription[i].patterns[k].length;
            }
          }
        }
      }
    }
  }
  return groupedPartials;
}

// dividePartials: Accepts a number of groupedPartials structures, each of which
// can contain pattern layout information about a number of columns to be
// reduced.  These are divided up into smaller groups of columns so that:
// a) There are no multi column groups where the "length"!=1.  This is
//    because we want each pattern to be implemented by one RegionReduction
//    structure.  Each of these takes partials Tensors that are repeated and
//    wrapped over the output region.
//    Eg: Output = [1 2].  (Where 1 means "reduction of column 1") Partials are
//    treated as [1 2 1 2 1 2] [1 2 1 2] ...  There is no mechanism to convey
//    the information [1 1 2 2 1 1 2 2] [1 1 2 2] .... Which is what these
//    patterns describe.  Of course [1 1 1 1 1] [1 1 1 1 1] is just a simpler
//    case, where the output happens to be [1]
// b) To divide work between available workers
//
// a) is a restriction that is presently necessary given the code for
// the steps that connect up the outputs from reduction and definition of
// RegionReductions.  It should be possible to avoid splitting for reason
// a) in the future.
std::vector<PartialsDescription>
dividePartials(std::vector<PartialsDescription> &groupedPartials, Graph &graph,
               Type inType, ReduceParams params) {
  std::vector<PartialsDescription> splitGroupedPartials;
  // Split up patterns that have both length > 1 and columns > 1 as these
  // represent multiple reductions
  for (unsigned i = 0; i < groupedPartials.size(); i++) {
    // Check the characteristics of each pattern within the group of partials
    bool patternsAreSimple = true;
    if (groupedPartials[i].columns.size() != 1) {
      for (unsigned j = 0; j < groupedPartials[i].patterns.size(); j++) {
        if (groupedPartials[i].patterns[j].length != 1) {
          patternsAreSimple = false;
          break;
        }
      }
    }
    // Copy or split up patterns accordingly
    if (patternsAreSimple) {
      splitGroupedPartials.push_back(groupedPartials[i]);
    } else {
      // Split all the patterns so that we have a pattern per column,
      // maintaining the length
      splitGroupedPartials.reserve(groupedPartials[i].columns.size());
      for (unsigned j = 0; j < groupedPartials[i].columns.size(); j++) {
        // The split partials have the same patterns but only one column
        splitGroupedPartials.emplace_back();
        splitGroupedPartials.back().patterns = groupedPartials[i].patterns;
        splitGroupedPartials.back().columns.push_back(
            groupedPartials[i].columns[j]);

        // Adjust the start of the new patterns to match the new starting
        // column
        for (unsigned k = 0; k < groupedPartials[i].patterns.size(); k++) {
          splitGroupedPartials.back().patterns[k].regionOffset =
              groupedPartials[i].patterns[k].regionOffset +
              j * groupedPartials[i].patterns[k].length;
        }
      }
    }
  }

  // Split up patterns to divide work between workers by column.  Later on
  // reductions can be split by row as well/instead. Both have a potential
  // downside: Splitting by row requires a second reduction stage. Splitting
  // by column could introduce copies.
  //
  // The method here is based on splitting output regions, which we
  // temporarily create just for splitting of work purposes.
  std::vector<Interval> outRegions;
  for (auto &partials : splitGroupedPartials) {
    outRegions.push_back(
        {partials.columns[0], partials.columns[0] + partials.columns.size()});
  }
  outRegions = splitOutputRegionsForWorkers(
      graph.getTarget(), graph.getTarget().getNumWorkerContexts(), params.op,
      inType, outRegions);

  // Having divided the temporary output regions, update the
  // splitGroupedPartials so that each set of columns represents an outRegion

  if (outRegions.size() != splitGroupedPartials.size()) {
    for (auto &region : outRegions) {
      for (unsigned i = 0; i < splitGroupedPartials.size(); i++) {
        if (region.begin() == splitGroupedPartials[i].columns[0]) {
          if (region.size() != splitGroupedPartials[i].columns.size()) {
            // This group was split so update its column list and create an
            // entry containing the remaining columns.  They too could be
            // split - but this will be dealt with on a later loop pass.
            // This will only be picked up if the columns in each reduction
            // are contiguous, but that was ensured by the code above
            splitGroupedPartials.emplace_back();
            splitGroupedPartials.back().patterns =
                splitGroupedPartials[i].patterns;

            const auto excessLength =
                splitGroupedPartials[i].columns.size() - region.size();
            splitGroupedPartials.back().columns.resize(excessLength);
            std::copy(splitGroupedPartials[i].columns.begin() + region.size(),
                      splitGroupedPartials[i].columns.end(),
                      splitGroupedPartials.back().columns.begin());
            // Adjust the start of the new patterns to match their starting
            // column
            for (auto &pat : splitGroupedPartials.back().patterns) {
              pat.regionOffset += region.size();
            }
            // Resize the original partial's column list as we've chopped some
            // off the end
            splitGroupedPartials[i].columns.resize(region.size());
          }
          // We found what we were looking for and split if necessary
          break;
        }
      }
    }
  }
  return splitGroupedPartials;
}
// Create reductions for the cases: Input to Output and Input to Intermediate
void createInputReductions(Graph &graph, const Tensor &in,
                           boost::variant<Tensor &, IntermediatePartials &> out,
                           bool createOutput,
                           const Graph::TileToTensorMapping mapping,
                           ReduceParams params, Type outType, Type inVertexType,
                           ComputeSetList &css,
                           std::vector<Tensor> &reductionResultTensors,
                           const std::string &debugPrefix,
                           ReductionDebug::ReductionStage *stageDebug) {

  logging::debug("DebugStr: {}", debugPrefix);
  const bool isInputToOutput = out.type() == typeid(Tensor);

  // Store the output tensors for each reduction vertex, one per column
  std::vector<Tensor> outputs(isInputToOutput ? in.dim(1) : 0);
  std::size_t csPos = css.pos();
  // Get the set of contiguous regions on each tile (splitting them if
  // necessary at tile mapping boundaries). The region indices here are in
  // the flattened input tensor.
  auto contiguousRegionsByTile =
      getSortedContiguousRegionsByTile(graph, in, mapping);
  // Number of columns in the reduction.
  const auto columns = in.dim(1);
  auto inType = in.elementType();
  // Loop through the tiles. We can process each tile independently.
  for (unsigned tile = 0; tile < contiguousRegionsByTile.size(); ++tile) {
    const auto &contiguousRegionsThisTile = contiguousRegionsByTile[tile];

    // Ignore empty tiles.
    if (contiguousRegionsThisTile.empty()) {
      continue;
    }
    // Make a pattern for each column that is detected in the regions on tile
    auto partialsDescription =
        gatherReductionPatterns(contiguousRegionsThisTile, columns);

    // Grouping works by identifying a compatible patterns that follow a base
    // pattern in memory.  This requires them to be in memory order.
    std::sort(partialsDescription.begin(), partialsDescription.end(),
              [](const PartialsDescription &a, const PartialsDescription &b) {
                return (a.patterns[0].regionOffset <
                        b.patterns[0].regionOffset);
              });

    // Group the patterns according to columns with identical patterns and
    // adjacent in memory
    auto groupedPartials = groupPartials(partialsDescription, columns);

    // Divide the patterns to split work between workers and cope with
    // other limitations
    auto splitGroupedPartials =
        dividePartials(groupedPartials, graph, in.elementType(), params);

    // logging begin
    if (logging::shouldLog(logging::Level::Trace)) {
      // Use to select which to view at compile time...
      auto &debugPartials = splitGroupedPartials;
      logging::trace(" Tile:{} Reduction Patterns:{}", tile,
                     debugPartials.size());
      for (auto &pats : debugPartials) {
        std::stringstream colStr;
        for (auto col : pats.columns) {
          colStr << " " << col;
        }
        logging::trace("  Patterns:{} Column list[{}]:{}", pats.patterns.size(),
                       pats.columns.size(), colStr.str());
        for (auto &pat : pats.patterns) {
          logging::trace(
              "    Pattern Length:{} Start:{} Stride:{} Reps:{} Region:{}",
              pat.length, pat.regionOffset, pat.stride, pat.repetitions,
              pat.regionIdx);
        }
      }
    }
    // logging end

    // Create the regionReductions with partials populated from patterns
    auto reductions = listPartialsUsingPatterns(
        splitGroupedPartials, in, contiguousRegionsThisTile, tile, columns);
    // Record the tensor in the IR, and the merged regions.
    std::vector<Interval> outputRegionsSplit;
    for (unsigned i = 0; i < splitGroupedPartials.size(); i++) {
      for (unsigned j = 0; j < splitGroupedPartials[i].columns.size(); j++) {
        outputRegionsSplit.push_back({splitGroupedPartials[i].columns[j],
                                      splitGroupedPartials[i].columns[j] + 1});
      }
    }
    // Create a 2D array of Intervals, each referencing a single column of the
    // whole reduction - So all columns should be referenced once, when
    // we aggregate over all tiles.  This is maintained as intervals rather
    // than individual columns as it is used below (required to be intervals).
    // Dimensions: [reduction][output columns in reduction]
    // For example, 2 reductions with regions/columns
    // {[0,3)} and {[4,5), [7,8), [6,7)]}
    // Gives [0] = [0,1), [1,2), [2,3)
    //       [1] = [4,5), [7,8), [6,7)
    std::vector<std::vector<Interval>> outputRegionsSplit2D(
        splitGroupedPartials.size());
    for (unsigned i = 0; i < splitGroupedPartials.size(); i++) {
      for (unsigned j = 0; j < splitGroupedPartials[i].columns.size(); j++) {
        outputRegionsSplit2D[i].push_back(
            {splitGroupedPartials[i].columns[j],
             splitGroupedPartials[i].columns[j] + 1});
      }
    }
    if (!isInputToOutput) {
      // Add a tensor for this tile.
      auto data = graph.addVariable(outType, {partialsDescription.size()},
                                    debugPrefix + "/tile_data1");
      reductionResultTensors.push_back(data);
      // Map it to this tile.
      graph.setTileMapping(data, tile);
      auto outputRegionsSplitIcl = poplarToSplitIntervalSet(outputRegionsSplit);

      boost::get<IntermediatePartials &>(out).setTensor(
          tile, data,
          boost::icl::interval_set<std::size_t>(outputRegionsSplitIcl));
      // Converting this back provides a sorted list of output columns
      // which tells us the order in which to connect the 2D column intervals
      auto outputRegionsSplit = splitIntervalSetToPoplar(outputRegionsSplitIcl);
      // Create a revised mapping so that the references are wrt to the partial
      // outputs. Ie - each is in the numerical order of their original column
      // number but have an index range equal to the number of individual
      // columns found on tile.
      //
      // {[1,3)} and {[4,5), [7,8), [6,7)]}
      // Gives [0] = [1,2), [2,3)                (5 elements with gaps, start=1)
      //       [1] = [4,5), [7,8), [6,7)
      // So, columns 1, 2, 4, 7, 6 appear in that order.
      // We want to maintain order but represent 5 columns, zero based:
      //             0, 1, 2, 4, 3
      // Now   [0] = [0,1), [1,2),               (5 elements, start=0, no gaps)
      //       [1] = [2,3), [4,5), [3,4)

      for (unsigned i = 0; i < reductions.size(); i++) {
        for (unsigned j = 0; j < outputRegionsSplit2D[i].size(); j++) {
          const auto match = std::lower_bound(outputRegionsSplit.begin(),
                                              outputRegionsSplit.end(),
                                              outputRegionsSplit2D[i][j]);
          const unsigned offset = match - outputRegionsSplit.begin();
          outputRegionsSplit2D[i][j] = {offset, offset + 1};
        }
      }
    }
    for (unsigned i = 0; i < reductions.size(); i++) {
      if (isInputToOutput) {
        if (!createOutput) {
          // Get the output slice, mapping each to the required slices
          // of the output tensor to ensure correct ordering: column 0...N
          reductions[i].output =
              concat(boost::get<Tensor &>(out).slices(outputRegionsSplit2D[i]));
        } else {
          // Get the output slice.
          reductions[i].output = graph.addVariable(
              inVertexType, {splitGroupedPartials[i].columns.size()});
          graph.setTileMapping(reductions[i].output, tile);
          // Record the outputs from the reduction ready to make the output
          // tensor, created in this function, to avoid re ordering
          for (unsigned j = 0; j < splitGroupedPartials[i].columns.size();
               j++) {
            outputs[splitGroupedPartials[i].columns[j]] =
                reductions[i].output[j].reshape({1});
          }
        }
      } else {
        auto &ir = boost::get<IntermediatePartials &>(out);
        // TODO: InputToIntermediate only: This:
        // size_t dataIdx = outputRegionsSplit2D[i][0].begin();
        // reductions[i].output = ir.data(tile).slice(dataIdx,
        //                        dataIdx + outputRegionsSplit2D[i].size());
        // With the re-arranged outputRegionsSplit2D will result in a correct
        // output but a rearrangedTensor being created at the end of the first
        // stage.  Although better than re-arranging the input it could be left
        // until the last reduction stage.  However the IR information contains
        // sorted columns, meaning that the information required is lost.
        reductions[i].output =
            concat(ir.data(tile).slices(outputRegionsSplit2D[i]));
      }
      // Debugging info about the output..
      reductions[i].outputDebugInfo.outputRegion = outputRegionsSplit[i];
      reductions[i].outputDebugInfo.dataRegion = outputRegionsSplit[i];
    }

    ReductionDebug::TileReduction *tileDebug = nullptr;
    if (stageDebug != nullptr) {
      stageDebug->tiles.emplace_back();
      tileDebug = &stageDebug->tiles.back();
    }

    // Start from our current position in the compute set list.
    ComputeSetList cssFork = css;
    connectReductions(graph, cssFork, params, inType, inVertexType, tile,
                      reductions, true, debugPrefix, tileDebug);
    // Record the maximum number of compute sets we've used.
    if (cssFork.pos() > csPos) {
      csPos = cssFork.pos();
    }
  }
  css.setPos(csPos);

  if (createOutput) {
    boost::get<Tensor>(out) = concat(outputs);
  }
  if (!params.update && isInputToOutput) {
    reductionResultTensors.push_back(boost::get<Tensor>(out));
  }
}

void inputToOutputNoExchange(Graph &graph, const Tensor &in,
                             const Graph::TileToTensorMapping &mapping,
                             boost::optional<Tensor> &finalOutput,
                             const std::vector<std::size_t> outputShape,
                             Type outputType, Type inVertexType,
                             ReduceParams params, ComputeSetList &css,
                             std::vector<Tensor> &reductionResultTensors,
                             const std::string &debugPrefix,
                             ReductionDebug *debug) {
  // If we're doing an update, things get really complicated if we have to do
  // casts too, so for now just use the same type for accumulation as the
  // output type.
  if (params.update) {
    inVertexType = outputType;
  }

  // The inVertex type is also the type that the vertex outputs (for simplicity
  // and to avoid having a million template specialisations). If it is
  // different from the output type we just add an explicit cast.
  const bool castRequired = inVertexType != outputType;

  // If we have an output, create the output Tensor for the
  // createInputReductions function.  This is either finalOutput or an
  // intermediate result which will be cast into finalOutput later. If we don't
  // have an output, createInputReductions will create its own output
  Tensor out;
  if (finalOutput) {
    if (castRequired) {
      // Create an output for the reduction, which will be cast later
      out = graph.clone(inVertexType, finalOutput.get().flatten());
    } else {
      // If no casting required and we have an output then use that as the
      // output from the reduction
      out = finalOutput.get().flatten();
    }
    if (!params.update) {
      reductionResultTensors.push_back(out);
    }
    // If the output isn't mapped yet, map it exactly the same as the first
    // row of the input which ensures no exchange will happen.
    bool mappingComplete;
    graph.getTileMapping(out, &mappingComplete);
    if (!mappingComplete) {
      auto mapping = graph.getTileMapping(in.slice(0, 1, 0));
      graph.setTileMapping(out, mapping);
    }
  }
  assert(in.rank() == 2);

  // Debug information.
  ReductionDebug::ReductionStage *stageDebug = nullptr;
  if (debug != nullptr) {
    debug->stages.emplace_back();
    stageDebug = &debug->stages.back();
    stageDebug->label = "Input to Output (No Exchange)";
  }
  createInputReductions(graph, in, out, !static_cast<bool>(finalOutput),
                        mapping, params, inVertexType, inVertexType, css,
                        reductionResultTensors,
                        debugPrefix + "/InToOutNoExchange", stageDebug);
  if (castRequired) {
    auto cs = css.add(graph, debugPrefix + "/Cast");
    if (finalOutput) {
      cast(graph, out, finalOutput.get().flatten(), cs);
    } else {
      finalOutput = graph.clone(outputType, out);
      cast(graph, out, finalOutput.get(), cs);
      graph.setTileMapping(finalOutput.get(),
                           graph.getTileMapping(in.slice(0, 1, 0)));
    }
  } else {
    if (!finalOutput) {
      finalOutput = out;
    }
  }
  finalOutput = finalOutput.get().reshape(outputShape);
}

IntermediatePartials inputToIntermediateNoExchange(
    Graph &graph, const Tensor &in, const Graph::TileToTensorMapping &mapping,
    Operation op, const Type &inVertexType, const Type &outType,
    ComputeSetList &css, std::vector<Tensor> &reductionResultTensors,
    const std::string &debugPrefix, ReductionDebug *debug) {

  // Number of output values of the reduction.
  auto outputSize = in.dim(1);

  auto inType = in.elementType();

  // Add a new tensor for each tile to output its partials to. These tensors
  // and the meta-info needed are stored in an IntermediatePartials.
  IntermediatePartials ir;
  ir.setDataType(outType);
  ir.setOutputSize(outputSize);

  // Debug information.
  ReductionDebug::ReductionStage *stageDebug = nullptr;
  if (debug != nullptr) {
    debug->stages.emplace_back();
    stageDebug = &debug->stages.back();
    stageDebug->label = "Input to Intermediate (No Exchange)";
  }
  createInputReductions(graph, in, ir, false, mapping, op, outType,
                        inVertexType, css, reductionResultTensors,
                        debugPrefix + "/InToIntermediateNoExchange",
                        stageDebug);
  return ir;
}

IntermediatePartials intermediateToIntermediate(
    Graph &graph, const IntermediatePartials &ipIn, Operation op,
    const Type &outType, ComputeSetList &css,
    std::vector<Tensor> &reductionResultTensors, const std::string &debugPrefix,
    ReductionDebug *debug) {

  logging::debug("DebugStr: {}", debugPrefix);
  // Debug information.
  ReductionDebug::ReductionStage *stageDebug = nullptr;
  if (debug != nullptr) {
    debug->stages.emplace_back();
    stageDebug = &debug->stages.back();
    stageDebug->label = "Intermediate to Intermediate";
  }

  IntermediatePartials ir;

  ir.setOutputSize(ipIn.outputSize());
  ir.setDataType(outType);

  auto inType = ipIn.dataType();

  const auto &target = graph.getTarget();

  unsigned grainSize = target.getVectorWidth(inType);

  if (grainSize == 0)
    throw poputil::poplibs_error("Zero vector width for type " +
                                 inType.toString());

  // The grain size is doubled for ADD (and ABS_ADD and SQUARE_ADD) because
  // these operations have dedicated instructions on Colossus that can operate
  // on twice as much data as all the other operations (MUL etc).
  if (op == popops::Operation::ADD ||
      op == popops::Operation::SQUARE_ADD) // Or ABS_ADD.
    grainSize *= 2;

  // If each piece is really small the overhead of having extra reduction
  // stages, and exchange and everything outweighs the savings.
  //
  // Optimisation: This was found empirically and not tested a lot.
  std::size_t minPieceSize = 64;

  auto splitMapIcl = calculateSplit(ipIn, grainSize, grainSize, 2, minPieceSize,
                                    target.getNumTiles());

  std::vector<boost::icl::interval<std::size_t>::type> allOutputRegionsSplit;
  allOutputRegionsSplit.reserve(splitMapIcl.iterative_size());
  for (const auto &it : splitMapIcl)
    allOutputRegionsSplit.push_back(it.first);

  // 1. Find all the partials for each output region.
  // 2. Split them up into N pieces.
  // 3. Assign them to tiles in a round-robin way.

  const auto &tilesForOutput = ipIn.getTilesForOutput();

  // Just do a round-robin assignment for now.

  // If we assign two blocks of the same interval to one tile then they will
  // be merged.

  struct ReductionBlock {
    std::vector<unsigned> sourceTiles;
  };

  // The reductions for each tile.
  struct TileReductions {
    // Map from the interval number (index into allOutputRegionsSplit)
    // to a list of source tiles to reduce on this tile.
    std::map<unsigned, std::vector<unsigned>> sourceTilesForInterval;
  };

  std::vector<TileReductions> tileReductions(target.getNumTiles());

  // Divide a by b, rounding up.
  auto udiv = [](unsigned a, unsigned b) { return (a + b - 1) / b; };

  unsigned t = 0;
  unsigned ival = 0;
  for (const auto &it : splitMapIcl) {
    const auto &sourceTiles = tilesForOutput(it.first.lower());

    auto numPartials = sourceTiles.size();
    auto splitCount = it.second;

    assert(splitCount > 0);

    // N is the number of rows to take for each reduction. This should be at
    // least 2 so we actually do some reducing.
    std::size_t N = udiv(numPartials, splitCount);
    if (N < 2)
      N = 2;

    for (unsigned i = 0; i < numPartials; i += N) {
      auto &st = tileReductions[t].sourceTilesForInterval[ival];

      unsigned Nclip = std::min(N, numPartials - i);

      st.reserve(st.size() + Nclip);

      st.insert(st.end(), sourceTiles.nth(i), sourceTiles.nth(i + Nclip));

      t = (t + 1) % target.getNumTiles();
    }

    ++ival;
  }

  std::size_t csPos = css.pos();

  // For each output tile...
  for (unsigned tile = 0; tile < tileReductions.size(); ++tile) {
    auto &tr = tileReductions[tile];

    if (tileReductions[tile].sourceTilesForInterval.empty())
      continue;

    // Work out the set of all output regions for this tile.
    boost::icl::interval_set<std::size_t> outputRegionsMergedIcl;
    for (auto it : tr.sourceTilesForInterval) {
      outputRegionsMergedIcl.insert(allOutputRegionsSplit[it.first]);
    }

    // Add a variable to receive the results.
    Tensor data = graph.addVariable(outType, {outputRegionsMergedIcl.size()},
                                    debugPrefix + "/tile_data2");
    reductionResultTensors.push_back(data);

    graph.setTileMapping(data, tile);

    // Add it to the output.
    ir.setTensor(tile, data, outputRegionsMergedIcl);

    // Store the tensors that we will connect up.
    std::vector<RegionReduction> reductions;
    reductions.reserve(tr.sourceTilesForInterval.size());

    // For each of the regions.
    for (const auto &it : tr.sourceTilesForInterval) {
      auto re = allOutputRegionsSplit[it.first];

      // The corresponding region in the data
      RegionReduction rt;

      size_t outputDataIdx = ir.dataElement(tile, re.lower());
      size_t len = boost::icl::size(re);

      // Check it is contiguous.
      assert(ir.dataElement(tile, re.lower() + len - 1) ==
             outputDataIdx + len - 1);

      // Loop through the source tiles for this region...
      for (auto partialTile : it.second) {
        size_t sourceDataIdx = ipIn.dataElement(partialTile, re.lower());

        assert(ipIn.dataElement(partialTile, re.upper() - 1) ==
               sourceDataIdx + boost::icl::size(re) - 1);

        rt.partials.push_back(
            ipIn.data(partialTile).slice(sourceDataIdx, sourceDataIdx + len));

        // Debugging info about the partial.
        ReductionDebug::Partial di;
        di.sourceCols = {sourceDataIdx, sourceDataIdx + len};
        di.sourceTile = partialTile;
        rt.partialsDebugInfo.push_back(di);
      }

      // Connect the output region.

      rt.output = ir.data(tile).slice(outputDataIdx, outputDataIdx + len);

      // Debugging infor about the output...
      rt.outputDebugInfo.outputRegion = {re.lower(), re.upper()};
      rt.outputDebugInfo.dataRegion = {outputDataIdx, outputDataIdx + len};

      reductions.push_back(rt);
    }

    ReductionDebug::TileReduction *tileDebug = nullptr;
    if (stageDebug != nullptr) {
      stageDebug->tiles.emplace_back();
      tileDebug = &stageDebug->tiles.back();
    }

    // Start from our current position in the compute set list.
    ComputeSetList cssFork = css;
    connectReductions(graph, cssFork, op, inType, outType, tile, reductions,
                      false, debugPrefix + "/IntermediateToIntermediate",
                      tileDebug);
    // Record the maximum number of compute sets we've used.
    if (cssFork.pos() > csPos)
      csPos = cssFork.pos();
  }

  css.setPos(csPos);

  return ir;
}

void intermediateToOutput(Graph &graph, const IntermediatePartials &ipIn,
                          boost::optional<Tensor> &finalOutput,
                          const std::vector<std::size_t> outputShape,
                          Type outputType, ReduceParams params,
                          Type inVertexType, ComputeSetList &css,
                          std::vector<Tensor> &reductionResultTensors,
                          const Tensor &in, const std::string &debugPrefix,
                          ReductionDebug *debug) {
  logging::debug("DebugStr: {}", debugPrefix);
  const auto numOutElements = in.dim(1);
  // If we're doing an update, things get really complicated if we have to do
  // casts too, so for now just use the same type for accumulation as the
  // output type.
  if (params.update) {
    inVertexType = outputType;
  }

  // The inVertex type is also the type that the vertex outputs (for simplicity
  // and to avoid having a million template specialisations). If it is
  // different from the output type we just add an explicit cast.

  Tensor out;
  bool castRequired = inVertexType != outputType;
  if (castRequired) {
    // Always need an output tensor creating for the reduction output if we
    // then intend to cast
    out = graph.addVariable(inVertexType, {numOutElements}, debugPrefix);
    graph.setTileMapping(out, graph.getTileMapping(in.slice(0, 1, 0), false));
    reductionResultTensors.push_back(out);
  } else {
    if (finalOutput) {
      // If no casting required and we have an output then use that as the
      // output from the reduction
      out = finalOutput.get().flatten();
    } else {
      // Otherwise create the output here
      out = graph.addVariable(inVertexType, {numOutElements}, debugPrefix);
      graph.setTileMapping(out, graph.getTileMapping(in.slice(0, 1, 0), false));
    }
    if (!params.update) {
      reductionResultTensors.push_back(out);
    }
  }
  // This is assumed below.
  assert(out.rank() == 1);

  auto inType = ipIn.dataType();

  // Debug information.
  ReductionDebug::ReductionStage *stageDebug = nullptr;
  if (debug != nullptr) {
    debug->stages.emplace_back();
    stageDebug = &debug->stages.back();
    stageDebug->label = "Intermediate To Output";
  }

  // If the output isn't already mapped, map it linearly and do the reduction
  // there, otherwise decide whether it is better to do the reduction at the
  // destination or not.
  bool mappingComplete;
  Graph::TileToTensorMapping mapping =
      graph.getTileMapping(out, &mappingComplete);
  if (mappingComplete) {
    if (!shouldReduceAtDestination(graph.getTarget(), ipIn, mapping,
                                   inVertexType, out.numElements())) {
      mapping = poputil::calcLinearTileMapping(graph, out);
    }
  } else {
    mapping = poputil::calcLinearTileMapping(graph, out);
    graph.setTileMapping(out, mapping);
  }

  // An interval_map from output element to the set of tiles that have
  // partials for it.
  const auto &tilesForOutput = ipIn.getTilesForOutput();

  // An interval_map from output element to the tile it is mapped to.
  auto mappingIcl = tileMappingToIntervalMap(mapping);

  assert(tilesForOutput.size() == ipIn.outputSize());
  assert(mappingIcl.size() == ipIn.outputSize());

  // We've got something like:
  //
  //   [0, 12) has partials on tiles {1, 4, 6}
  //   [12, 40) has partials on tiles {5, 6, 7}
  //   [40, 100) has partials on tiles {1, 2}
  //
  //         and
  //
  //   [0, 2) is mapped to tile 1
  //   [2, 5) is mapped to tile 4
  //   [5, 35) is mapped to tiles 3
  //   [35, 100) is mapped to tile 1
  //
  // And I want an interval_map<size_t, set<unsigned>> for each tile:
  //
  //   [
  //       {} // Tile 0
  //       {  // Tile 1
  //           [0, 2) has partials on {1, 4, 6}
  //           [35, 40) has partials on {5, 6, 7}
  //           [40, 100) has partials on tiles {1, 2}
  //       }
  //       {} // Tile 2
  //       {  // Tile 3
  //           [5, 12) has partials on {1, 4, 6}
  //           [12, 35) has partials on {5, 6, 7}
  //       }
  //       {  // Tile 4
  //           [2, 5) has partials on {1, 4, 6}
  //       }
  //   ]

  std::vector<boost::icl::interval_map<std::size_t,
                                       boost::container::flat_set<unsigned>>>
      tilesForOutputPerTile(mapping.size());

  // Iterate through both maps together.
  for_each_zipped_region(
      mappingIcl.begin(), mappingIcl.end(), tilesForOutput.begin(),
      tilesForOutput.end(),
      [&](std::size_t begin, std::size_t end, unsigned mappedToTile,
          const boost::container::flat_set<unsigned> &partialTiles) {
        tilesForOutputPerTile[mappedToTile].set(std::make_pair(
            boost::icl::interval<std::size_t>::right_open(begin, end),
            partialTiles));
      });

  std::size_t csPos = css.pos();

  // Partition tilesForOutput based on mappingIcl.

  for (unsigned tile = 0; tile < mapping.size(); ++tile) {
    if (mapping[tile].empty())
      continue;

    // Get the regions that are mapped to this tile.
    auto outputRegionsSplitIcl = poplarToSplitIntervalSet(mapping[tile]);

    // Take the subset of the map from output element to partial tiles
    // for the output regions that are mapped to this tile.
    const auto &thisTilesForOutput = tilesForOutputPerTile[tile];

    // Convert the output element indices to poplar interval format.

    std::vector<Interval> outputRegionsSplit;
    outputRegionsSplit.reserve(thisTilesForOutput.size());

    for (const auto &ival : thisTilesForOutput)
      outputRegionsSplit.emplace_back(ival.first.lower(), ival.first.upper());

    // Split them if it would make it faster by processing them separately
    // with different vertices.
    outputRegionsSplit = splitOutputRegionsForWorkers(
        graph.getTarget(), graph.getTarget().getNumWorkerContexts(), params.op,
        inVertexType, outputRegionsSplit);

    // Store the tensors that we will connect up. Have to do this
    // here so we can resize the Vectors in the vertex.
    std::vector<RegionReduction> reductions;

    reductions.reserve(outputRegionsSplit.size());

    // Finally we repeat the above but this time record the actual connections.
    for (const auto &re : outputRegionsSplit) {
      RegionReduction rt;

      // Connect the output. This is fine because output is 1D.
      rt.output = out.slice(re);

      rt.partials.reserve(32); // This speeds things up a bit.

      // Get the list of partials to use.
      auto partialTiles = thisTilesForOutput(re.begin());

      for (auto partialTile : partialTiles) {
        size_t sourceDataIdx = ipIn.dataElement(partialTile, re.begin());
        size_t len = re.size();

        assert(ipIn.dataElement(partialTile, re.begin() + len - 1) ==
               sourceDataIdx + len - 1);

        rt.partials.emplace_back(
            ipIn.data(partialTile).slice(sourceDataIdx, sourceDataIdx + len));

        // Debugging info about the partial.
        ReductionDebug::Partial di;
        di.sourceCols = {sourceDataIdx, sourceDataIdx + len};
        di.sourceTile = partialTile;
        rt.partialsDebugInfo.push_back(di);
      }

      // Debugging infor about the output...
      rt.outputDebugInfo.outputRegion = re;
      rt.outputDebugInfo.dataRegion = re;

      reductions.push_back(rt);
    }

    ReductionDebug::TileReduction *tileDebug = nullptr;
    if (stageDebug != nullptr) {
      stageDebug->tiles.emplace_back();
      tileDebug = &stageDebug->tiles.back();
    }

    // Start from our current position in the compute set list.
    ComputeSetList cssFork = css;
    connectReductions(graph, cssFork, params, inType, inVertexType, tile,
                      reductions, false, debugPrefix + "/IntermediateToOutput",
                      tileDebug);
    // Record the maximum number of compute sets we've used.
    if (cssFork.pos() > csPos)
      csPos = cssFork.pos();
  }

  css.setPos(csPos);

  if (castRequired) {
    // If the mapping of finalOutput was incomplete we need to
    // set it.
    auto cs = css.add(graph, debugPrefix + "/Cast");
    if (finalOutput) {
      // Note - Check if we should really be setting the mapping of the output
      // here in the case where we already had an output, which may be mapped
      // already.
      graph.setTileMapping(finalOutput.get(), graph.getTileMapping(out));
      cast(graph, out.flatten(), finalOutput.get().flatten(), cs);
    } else {
      finalOutput = graph.clone(outputType, out, debugPrefix + "/CastFinal");
      cast(graph, out, finalOutput.get(), cs);
    }
  } else if (!finalOutput) {
    finalOutput = out;
  }
  finalOutput = finalOutput.get().reshape(outputShape);
}

} // namespace popops
