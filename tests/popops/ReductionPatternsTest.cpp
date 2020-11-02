// Copyright (c) 2019 Graphcore Ltd. All rights reserved.
#define BOOST_TEST_MODULE ReductionPatternsTest
#include "popops/reduction/ReductionIntrospection.hpp"
#include <boost/test/unit_test.hpp>
#include <poplar/Engine.hpp>
#include <poplibs_support/TestDevice.hpp>

#include <iostream>

using namespace poplar;
using namespace popops;
using namespace poplibs_support;

void printResult(const std::vector<PartialsDescription> &partialsDescription) {
  for (auto &par : partialsDescription) {
    std::cout << "Reduction patterns for column(s):";
    for (auto &column : par.columns) {
      std::cout << " " << column;
    }
    std::cout << "\n";
    for (auto &pattern : par.patterns) {
      std::cout << "Pattern innerFactor: " << pattern.innerFactor
                << " Start:" << pattern.regionOffset
                << " Stride:" << pattern.stride
                << " outerFactor:" << pattern.outerFactor
                << " Region:" << pattern.regionIdx << "\n";
    }
  }
}

bool operator!=(const PartialsPattern &lhs, const PartialsPattern &rhs) {
  return (lhs.innerFactor != rhs.innerFactor) ||
         (lhs.regionOffset != rhs.regionOffset) || (lhs.stride != rhs.stride) ||
         (lhs.outerFactor != rhs.outerFactor) ||
         (lhs.regionIdx != rhs.regionIdx);
}

bool checkResult(const std::vector<PartialsDescription> &generatedPatterns,
                 const std::vector<std::vector<PartialsPattern>> &patterns,
                 const std::vector<std::vector<unsigned>> &columns) {
  if (generatedPatterns[0].patterns.size() == 0 && patterns.size() == 0) {
    return true;
  }
  if (generatedPatterns.size() != patterns.size()) {
    return false;
  }
  for (unsigned i = 0; i < generatedPatterns.size(); i++) {
    if (generatedPatterns[i].patterns.size() != patterns[i].size()) {
      return false;
    }
    for (unsigned j = 0; j < generatedPatterns[i].patterns.size(); j++) {
      if (generatedPatterns[i].patterns[j] != patterns[i][j]) {
        return false;
      }
    }
  }

  for (unsigned i = 0; i < columns.size(); i++) {
    if (columns[i] != generatedPatterns[i].columns) {
      return false;
    }
  }
  return true;
}

BOOST_AUTO_TEST_CASE(ReducePatternsSimple) {
  // Define one reduction which, being empty causes the function to
  // identify columns
  std::vector<PartialsDescription> reductions;
  // The reductions operate on a matrix with 2 columns
  unsigned columns = 2;

  // Define a single region with 10 elements in it, starting at the
  // beginning of the Tensor
  std::vector<std::vector<Interval>> regions = {{{0, 10}}};

  // Given 2 columns, 10 elements in the region the elements expected in
  // column 0 are given by 1's:
  // 1 0 1 0 1 0 1 0 1 0
  // And for column 1:
  // 0 1 0 1 0 1 0 1 0 1
  //
  //   start (0 for column 0, 1 for column 1)
  //   Pattern size 1 element
  //   Pattern stride (repeat length) = 2
  //   Pattern repetitions (of the pattern 1 0 = 5). Lack of the  last
  //   tailing 0 doesn't matter.
  std::vector<std::vector<PartialsPattern>> expected = {{{1, 0, 2, 5, 0}},
                                                        {{1, 1, 2, 5, 0}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  BOOST_TEST(checkResult(reductions, expected, {{0}, {1}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsTwoReductions) {
  // Define two reductions which describe the reduction of columns 0, 2 each
  // spanning 1 column
  std::vector<PartialsDescription> reductions(2);
  reductions[0].columns.push_back(0);
  reductions[1].columns.push_back(2);
  // The reductions operate on a matrix with 4 columns
  unsigned columns = 4;

  // Define a single region with 20 elements in it, starting at the
  // beginning of the Tensor
  std::vector<std::vector<Interval>> regions = {{{0, 20}}};

  // Given 4 columns, 20 elements in the region the elements expected in
  // column 0 are given by 1's:
  // 1 0 0 0 1 0 0 0 1 0 0 0 1 0 0 0 1 0 0 0
  // And column 2:
  // 0 0 1 0 0 0 1 0 0 0 1 0 0 0 1 0 0 0 1 0
  std::vector<std::vector<PartialsPattern>> expected = {{{1, 0, 4, 5, 0}},
                                                        {{1, 2, 4, 5, 0}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  BOOST_TEST(checkResult(reductions, expected, {{0}, {2}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsMultiPattern) {
  std::vector<PartialsDescription> reductions(1);
  reductions[0].columns.push_back(1);
  // The reductions operate on a matrix with 10 columns
  unsigned columns = 10;

  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {{{1, 2},
                                                 {11, 13},
                                                 {21, 22},
                                                 {31, 33},
                                                 {41, 42},
                                                 {51, 54},
                                                 {61, 62},
                                                 {71, 74},
                                                 {81, 82},
                                                 {91, 95}}};

  // Given 10 columns, and concatenating the region described, those in
  // column 0 are given by 1's:
  // 1  11 12 21 31 32 41 51 52 53 61 71 72 73 81 91 92 93
  // 1  1  0  1  1  0  1  1  0  0  1  1  0  0  1  1  0  0
  // So, 2 patterns: 1 1 0 and 1 1 0 0
  std::vector<std::vector<PartialsPattern>> expected = {
      {{2, 0, 3, 3, 0}, {2, 10, 4, 2, 0}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  BOOST_TEST(checkResult(reductions, expected, {{1}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsTruncatedPattern) {
  std::vector<PartialsDescription> reductions(1);
  reductions[0].columns.push_back(0);
  // The reductions operate on a matrix with 4 columns
  unsigned columns = 4;

  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {{{1, 2},
                                                 {4, 5},
                                                 {8, 9},
                                                 {12, 14},
                                                 {16, 17},
                                                 {20, 21},
                                                 {24, 26},
                                                 {28, 29},
                                                 {32, 33}}};

  // Given 4 columns, and concatenating the region described, those in
  // column 0 are given by 1's:
  // 0 1 1 1 0 1 1 1 0 1 1
  // So, 2 patterns: 1 1 1 0 and 1 1 are expected:
  std::vector<std::vector<PartialsPattern>> expected = {
      {{3, 1, 4, 2, 0}, {2, 9, 1, 1, 0}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  BOOST_TEST(checkResult(reductions, expected, {{0}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsStop) {
  std::vector<PartialsDescription> reductions(1);
  reductions[0].columns.push_back(0);
  // The reductions operate on a matrix with 4 columns
  unsigned columns = 4;

  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {
      {{1, 2}, {4, 5}, {8, 10}, {12, 13}, {16, 18}, {20, 21}, {24, 28}}};

  // Given 4 columns, and concatenating the region described, those in
  // column 0 are given by 1's:
  // 0 1 1 0 1 1 0 1 1 0 0 0
  // So, the pattern: 1 1 0 is expected:
  std::vector<std::vector<PartialsPattern>> expected = {{{2, 1, 3, 3, 0}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  BOOST_TEST(checkResult(reductions, expected, {{0}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsAllOnePattern) {
  std::vector<PartialsDescription> reductions(1);
  reductions[0].columns.push_back(0);
  // The reductions operate on a matrix with 4 columns
  unsigned columns = 4;

  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {{{4, 5}, {8, 9}, {12, 13}}};

  // Given 4 columns, and concatenating the region described, those in
  // column 0 are given by 1's:
  // 1 1 1
  std::vector<std::vector<PartialsPattern>> expected = {{{3, 0, 1, 1, 0}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  BOOST_TEST(checkResult(reductions, expected, {{0}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsNoPattern) {
  std::vector<PartialsDescription> reductions(1);
  reductions[0].columns.push_back(1);
  // The reductions operate on a matrix with 4 columns
  unsigned columns = 4;

  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {{{4, 5}, {8, 9}, {12, 13}}};

  // Given 4 columns, and concatenating the region described, there is nothing
  // in column 1.
  std::vector<std::vector<PartialsPattern>> expected;

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  BOOST_TEST(checkResult(reductions, expected, {{1}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsMultiRegion) {
  std::vector<PartialsDescription> reductions(1);
  reductions[0].columns.push_back(0);
  // The reductions operate on a matrix with 4 columns
  unsigned columns = 4;
  // Define a series of regions - illustrated below
  std::vector<std::vector<Interval>> regions = {
      {{4, 5}}, {{0, 3}, {8, 9}}, {{12, 13}}};

  // Given 4 columns, and concatenating the regions described, we have
  // Region 0: 1
  // Region 1: 1 0 0 1
  // Region 2: 1
  std::vector<std::vector<PartialsPattern>> expected = {
      {{1, 0, 1, 1, 0}, {1, 0, 3, 2, 1}, {1, 0, 1, 1, 2}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  BOOST_TEST(checkResult(reductions, expected, {{0}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsLongerOne) {
  std::vector<PartialsDescription> reductions(1);
  reductions[0].columns.push_back(0);
  // The reductions operate on a matrix with 4 columns
  unsigned columns = 4;

  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {{{1, 3},
                                                 {4, 5},
                                                 {8, 10},
                                                 {12, 13},
                                                 {16, 18},
                                                 {20, 21},
                                                 {24, 25},
                                                 {28, 29},
                                                 {32, 35}}};

  // Given 4 columns, and concatenating the region described, those in
  // column 0 are given by 1's:
  // 0 0 1 1 0 1 1 0 1 1 1 1 0 0
  // So, the patterns: 1 1 0, 1 1 are expected:
  std::vector<std::vector<PartialsPattern>> expected = {
      {{2, 2, 3, 2, 0}, {4, 8, 1, 1, 0}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  BOOST_TEST(checkResult(reductions, expected, {{0}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsShorterOne) {
  std::vector<PartialsDescription> reductions(1);
  reductions[0].columns.push_back(0);
  // The reductions operate on a matrix with 4 columns
  unsigned columns = 4;

  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {{{1, 3},
                                                 {4, 5},
                                                 {8, 9},
                                                 {12, 14},
                                                 {16, 17},
                                                 {20, 21},
                                                 {24, 26},
                                                 {28, 33}}};

  // Given 4 columns, and concatenating the region described, those in
  // column 0 are given by 1's:
  // 0 0 1 1 1 0 1 1 1 0 1 0 0 0 1
  // So, the patterns: 1 1 1 0, 1 0 0 0 are expected:
  std::vector<std::vector<PartialsPattern>> expected = {
      {{3, 2, 4, 2, 0}, {1, 10, 4, 2, 0}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  BOOST_TEST(checkResult(reductions, expected, {{0}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsEndAtEnd) {
  std::vector<PartialsDescription> reductions(1);
  reductions[0].columns.push_back(0);
  // The reductions operate on a matrix with 4 columns
  unsigned columns = 4;

  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {
      {{1, 3}, {4, 5}, {8, 10}, {12, 13}, {16, 18}, {20, 21}, {24, 25}}};

  // Given 4 columns, and concatenating the region described, those in
  // column 0 are given by 1's:
  // 0 0 1 1 0 1 1 0 1 1
  // So, the patterns: 1 1 0 is expected:
  std::vector<std::vector<PartialsPattern>> expected = {{{2, 2, 3, 3, 0}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  BOOST_TEST(checkResult(reductions, expected, {{0}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsGroupedSimple) {
  std::vector<PartialsDescription> reductions;
  unsigned columns = 4;
  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {{{0, 24}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  auto groupedReductions = groupPartials(reductions, columns);
  std::cout << "Grouped:\n";
  printResult(groupedReductions);
  // As reductions is empty at the start, this will gather information on
  // all 4 columns. The intervals span 6 rows so we should see a sequence of
  // columns in our groupedReductions = 0, 1, 2, 3
  // Elements repeat once, start at the beginning of the region, have
  // a stride of 4 and repeat 6 times:
  std::vector<std::vector<PartialsPattern>> expected = {{{1, 0, 4, 6, 0}}};
  BOOST_TEST(checkResult(groupedReductions, expected, {{0, 1, 2, 3}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsGrouped2Groups) {
  std::vector<PartialsDescription> reductions;
  unsigned columns = 4;
  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {
      {{0, 2}, {4, 6}, {8, 10}, {12, 14}, {16, 18}, {20, 22}, {24, 27}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  auto groupedReductions = groupPartials(reductions, columns);
  std::cout << "Grouped:\n";
  printResult(groupedReductions);
  // Here we have a groupable pattern with columns 0, 1 in it and then an
  // individual patten with column 2 in it.
  std::vector<std::vector<PartialsPattern>> expected = {{{1, 0, 2, 7, 0}},
                                                        {{1, 14, 1, 1, 0}}};
  BOOST_TEST(checkResult(groupedReductions, expected, {{0, 1}, {2}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsGroupedTruncatedRegion) {
  std::vector<PartialsDescription> reductions;
  unsigned columns = 6;
  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {{{0, 23}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  auto groupedReductions = groupPartials(reductions, columns);
  std::cout << "Grouped:\n";
  printResult(groupedReductions);
  // Here the region almost conatains a whole 4 x 6 matrix but the last
  // element is missing.  We should get 2 grouped patterns:
  std::vector<std::vector<PartialsPattern>> expected = {{{1, 0, 6, 4, 0}},
                                                        {{1, 5, 6, 3, 0}}};
  BOOST_TEST(checkResult(groupedReductions, expected, {{0, 1, 2, 3, 4}, {5}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsGroupedMultiRegion) {
  std::vector<PartialsDescription> reductions;
  unsigned columns = 2;
  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {{{0, 24}}, {{24, 48}}};

  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  auto groupedReductions = groupPartials(reductions, columns);
  std::cout << "Grouped:\n";
  printResult(groupedReductions);
  // Here there are 2 identical sets of patterns for column 0, 1 but split over
  // 2 regions.  They can be grouped - the one group contains 2 patterns.
  std::vector<std::vector<PartialsPattern>> expected = {
      {{1, 0, 2, 12, 0}, {1, 0, 2, 12, 1}}};
  BOOST_TEST(checkResult(groupedReductions, expected, {{0, 1}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsMultiRegion3Patterns) {
  std::vector<PartialsDescription> reductions(1);
  reductions[0].columns.push_back(0);
  unsigned columns = 10;
  // Define a series of intervals in one region - illustrated below
  std::vector<std::vector<Interval>> regions = {
      {{0, 1}, {10, 11}, {11, 13}, {40, 41}, {50, 51}, {60, 61}}, {{0, 1}}};
  // Data in memory: column 0 or don't care : x
  //           01234567890123
  // Region 0: 00xx000
  // Region 1: 0
  gatherReductionPatterns(reductions, regions, columns);
  printResult(reductions);
  std::vector<std::vector<PartialsPattern>> expected = {
      {{2, 0, 4, 1, 0}, {3, 4, 1, 1, 0}, {1, 0, 1, 1, 1}}};
  BOOST_TEST(checkResult(reductions, expected, {{0}}));
}

BOOST_AUTO_TEST_CASE(ReducePatternsDivideDifferentLengths) {
  std::vector<PartialsDescription> reductions;
  std::vector<unsigned> columns = {1, 2};
  reductions.push_back({columns, {}});
  // 2 patterns where we have >1 column, and patterns with a large and different
  // innerFactor parameter.  The other parameters are arbitrary. These should be
  // split up.
  reductions[0].patterns.push_back({8, 0, (8 * 2), 3, 0});
  reductions[0].patterns.push_back({12, (8 * 2 * 3), (12 * 2), 6, 0});
  printResult(reductions);

  auto device = createTestDevice(DeviceType::IpuModel2);
  Graph graph(device.getTarget());
  auto dividedReductions =
      dividePartials(reductions, graph, HALF, popops::Operation::ADD);
  std::cout << "Divided:\n";
  printResult(dividedReductions);

  std::vector<std::vector<PartialsPattern>> expected = {
      {{8, 0, 16, 3, 0}, {12, 48, 24, 6, 0}},
      {{8, 8, 16, 3, 0}, {12, 60, 24, 6, 0}}};
  std::vector<std::vector<unsigned>> expectedColumns(columns.size());
  for (unsigned i = 0; i < columns.size(); i++) {
    expectedColumns[i].push_back(columns[i]);
  }
  BOOST_TEST(checkResult(dividedReductions, expected, expectedColumns));
}

// Testing functions to analyse column ordering
//

// We are only interested in column order so initialise patterns with the
// columns provided by the test.
// input dimensions [tile][region][columns]
// [tile]:   Each tile can contain several regions, a column can feature on
//           only 1 tile or many tiles
// [region]  There may be several contiguous blocks on a tile, they will
//           never contain the same column number as another block on that tile
// [columns] A list of columns found in a contiguous block.
TilePartialsDescription
initialiseRegions(const std::vector<std::vector<std::vector<unsigned>>> &in) {
  TilePartialsDescription result(in.size());
  for (unsigned i = 0; i < in.size(); i++) {
    for (unsigned j = 0; j < in[i].size(); j++) {
      PartialsDescription pattern;
      pattern.columns = in[i][j];
      result[i].push_back(pattern);
    }
  }
  return result;
}

void printResult(const boost::optional<std::vector<unsigned>> &result) {
  if (result) {
    std::cout << "Column order found:";
    for (unsigned i = 0; i < result.get().size(); i++) {
      std::cout << result.get()[i] << ",";
    }
  } else {
    std::cout << "No result: Columns are consecutive";
  }
  std::cout << "\n";
}

bool checkResult(const boost::optional<std::vector<unsigned>> &result,
                 const boost::optional<std::vector<unsigned>> &expected) {
  if (expected) {
    if (!result) {
      return false;
    }
    return result.get() == expected.get();
  }
  if (result) {
    return false;
  }
  return true;
}

BOOST_AUTO_TEST_CASE(ReduceFindCommonColumnOrder) {
  std::vector<std::vector<std::vector<unsigned>>> tileColumns = {
      {{0, 1, 2, 3}, {4, 5, 6, 7}}, // Tile 0
      {{0, 1, 2, 3}},
      {{4, 5, 6, 7, 8, 9}}, // Tile 1
      {{4, 5, 6, 7, 8, 9}}, // Tile 2
      {{4, 5, 6, 7}},       // Tile 3
      {{10}}                // Tile 4
  };
  const auto regions = initialiseRegions(tileColumns);
  auto result = findCommonColumnOrder(regions, 11);
  printResult(result);
  // All consistent and in numeric order so expect none
  BOOST_TEST(checkResult(result, boost::none));
}

BOOST_AUTO_TEST_CASE(ReduceFindCommonColumnOrderShuffled) {
  std::vector<std::vector<std::vector<unsigned>>> tileColumns = {
      {{0, 2, 3, 1}},             // Tile 0
      {{7, 0, 2, 3, 1, 4, 5, 6}}, // Tile 1
  };
  const auto regions = initialiseRegions(tileColumns);
  auto result = findCommonColumnOrder(regions, 8);
  printResult(result);
  // Consistent ordering, all columns are associated with all others
  std::vector<unsigned> expected = {7, 0, 2, 3, 1, 4, 5, 6};
  BOOST_TEST(checkResult(result, expected));
}
BOOST_AUTO_TEST_CASE(ReduceFindCommonColumnOrderBackwards) {
  std::vector<std::vector<std::vector<unsigned>>> tileColumns = {
      {{3, 2, 1, 0}}, // Tile 0
      {{3, 2, 1, 0}}, // Tile 1
  };
  const auto regions = initialiseRegions(tileColumns);
  auto result = findCommonColumnOrder(regions, 4);
  printResult(result);
  std::vector<unsigned> expected = {3, 2, 1, 0};
  BOOST_TEST(checkResult(result, expected));
}
BOOST_AUTO_TEST_CASE(ReduceFindCommonColumnOrderBackwardsForwards) {
  std::vector<std::vector<std::vector<unsigned>>> tileColumns = {
      {{3, 2, 1, 0}},             // Tile 0
      {{3, 2, 1, 0, 4, 6, 5, 7}}, // Tile 1
  };
  const auto regions = initialiseRegions(tileColumns);
  auto result = findCommonColumnOrder(regions, 8);
  printResult(result);
  std::vector<unsigned> expected = {3, 2, 1, 0, 4, 6, 5, 7};
  BOOST_TEST(checkResult(result, expected));
}
BOOST_AUTO_TEST_CASE(ReduceFindCommonColumnOrderCircular) {
  std::vector<std::vector<std::vector<unsigned>>> tileColumns = {
      {{0, 2, 3}}, // Tile 0
      {{3, 1, 4}}, // Tile 1
      {{1, 4, 0}}  // Tile 2
  };
  const auto regions = initialiseRegions(tileColumns);
  auto result = findCommonColumnOrder(regions, 5);
  printResult(result);
  // Consistent ordering as below, but circular (internal implementation detail)
  // as column 4 is followed by column  0
  // The internal algorithm will deliver circular groups with the lowest
  // numbered column first.
  std::vector<unsigned> expected = {0, 2, 3, 1, 4};
  BOOST_TEST(checkResult(result, expected));
}
BOOST_AUTO_TEST_CASE(ReduceFindCommonColumnOrderIndependantGroups) {
  std::vector<std::vector<std::vector<unsigned>>> tileColumns = {
      {{0, 2, 3}, {4, 7, 6, 1}}, // Tile 0
      {{3, 0, 2}},               // Tile 1
      {{4, 7, 6}, {2, 3, 0}},    // Tile 2
      {{5}, {9}, {7, 6}},        // Tile 3
      {{8}, {9, 10}},
      {{9, 10}}, // Tile 4
  };
  const auto regions = initialiseRegions(tileColumns);
  auto result = findCommonColumnOrder(regions, 11);
  printResult(result);
  // Consistent ordering as below with:
  // 0,2,3 as an independent circular group
  // 4,7,6,1 grouped together
  // 5 On its own
  // 8 On its own
  // 9,10 grouped together
  // The internal algorithm will deliver circular groups with the lowest
  // numbered column first, and will concatenate groups based on the lowest
  // column number in the group (even if it is not first)
  std::vector<unsigned> expected = {0, 2, 3, 4, 7, 6, 1, 5, 8, 9, 10};
  BOOST_TEST(checkResult(result, expected));
}

// These tests provide an inconsistent ordering which doesn't happen that
// often in practice.  This means that a column is found to have >1 columns
// that follow it.  Picking an answer (with all columns represented once) is
// correct, but the exact ordering is based on the implementation of the
// function under test.

BOOST_AUTO_TEST_CASE(ReduceFindCommonColumnOrderInconsistent1) {
  std::vector<std::vector<std::vector<unsigned>>> tileColumns = {
      {{0, 2, 4, 5}},             // Tile 0
      {{0, 2, 3, 1, 4, 5, 6, 7}}, // Tile 1
  };
  const auto regions = initialiseRegions(tileColumns);
  auto result = findCommonColumnOrder(regions, 8);
  printResult(result);
  // Inconsistent ordering but we pick out an order based on the 1st column
  // noted to follow a column
  std::vector<unsigned> expected = {0, 2, 4, 5, 6, 7, 3, 1};
  BOOST_TEST(checkResult(result, expected));
}
BOOST_AUTO_TEST_CASE(ReduceFindCommonColumnOrderInconsistent2) {
  std::vector<std::vector<std::vector<unsigned>>> tileColumns = {
      {{0, 2, 3}}, // Tile 0
      {{3, 1, 4}}, // Tile 1
      {{1, 4, 0}}, // Tile 2
      {{4, 6, 5}}, // Tile 3
      {{6, 7, 8}}, // Tile 4
      {{8, 4, 6}}, // Tile 5
  };
  const auto regions = initialiseRegions(tileColumns);
  auto result = findCommonColumnOrder(regions, 9);
  printResult(result);
  // Inconsistent ordering - column 4 is followed by 0 and by 6.  This creates
  // 2 linked rings for added complication
  std::vector<unsigned> expected = {0, 2, 3, 1, 4, 6, 5, 7, 8};
  BOOST_TEST(checkResult(result, expected));
}
BOOST_AUTO_TEST_CASE(ReduceFindCommonColumnOrderInconsistent3) {
  std::vector<std::vector<std::vector<unsigned>>> tileColumns = {
      {{0, 2, 4, 5}},             // Tile 0
      {{1, 2, 4, 5}},             // Tile 1
      {{0, 2, 3, 1, 4, 5, 6, 7}}, // Tile 2
  };
  const auto regions = initialiseRegions(tileColumns);
  auto result = findCommonColumnOrder(regions, 8);
  printResult(result);
  // Inconsistent ordering but we pick out an order based on the 1st column
  // noted to follow a column
  std::vector<unsigned> expected = {0, 2, 4, 5, 6, 7, 3, 1};
  BOOST_TEST(checkResult(result, expected));
}
BOOST_AUTO_TEST_CASE(ReduceFindCommonColumnOrderInconsistent4) {
  std::vector<std::vector<std::vector<unsigned>>> tileColumns = {
      {{0, 1, 2, 3}}, // Tile 0
      {{2, 4}},       // Tile 1
      {{2, 5, 6, 7}}, // Tile 2
      {{8, 2, 9, 10}} // Tile 3
  };
  const auto regions = initialiseRegions(tileColumns);
  auto result = findCommonColumnOrder(regions, 11);
  printResult(result);
  // Inconsistent ordering but we pick out an order based on the 1st column
  // noted to follow a column.  This turns out to be consecutive!
  BOOST_TEST(checkResult(result, boost::none));
}
BOOST_AUTO_TEST_CASE(ReduceFindCommonColumnOrderInconsistent5) {
  std::vector<std::vector<std::vector<unsigned>>> tileColumns = {
      {{0, 2, 1, 3}}, // Tile 0
      {{2, 4}},       // Tile 1
      {{2, 5, 6, 7}}, // Tile 2
      {{8, 2, 9, 10}} // Tile 3
  };
  const auto regions = initialiseRegions(tileColumns);
  auto result = findCommonColumnOrder(regions, 11);
  printResult(result);
  // Inconsistent ordering but we pick out an order based on the 1st column
  // noted to follow a column.
  std::vector<unsigned> expected = {0, 2, 1, 3, 4, 5, 6, 7, 8, 9, 10};
  BOOST_TEST(checkResult(result, expected));
}
