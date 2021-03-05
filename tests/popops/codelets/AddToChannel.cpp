// Copyright (c) 2018 Graphcore Ltd. All rights reserved.

// Legacy code to test some BroadcastVectorInner vertices.
// This code is no longer used.

#define BOOST_TEST_MODULE AddToChannel

#include "../lib/popops/ExprOpUtil.hpp"
#include <boost/multi_array.hpp>
#include <functional>
#include <iostream>
#include <limits>
#include <poplar/Engine.hpp>
#include <poplibs_support/TestDevice.hpp>
#include <poplibs_test/Util.hpp>
#include <popops/codelets.hpp>
#include <poputil/TileMapping.hpp>
#include <poputil/VertexTemplates.hpp>

// Tolerances used in tests
#define FLOAT_REL_TOL 0.01
#define HALF_REL_TOL 0.1
#define FLOAT_ABS_TOL 1e-6
#define HALF_ABS_TOL 1e-3

using namespace poplar;
using namespace poplar::program;
using namespace poputil;
using namespace poplibs_test::util;
using namespace poplibs_support;

const OptionFlags options;

struct TestCase {
  poplar::Type type;
  std::size_t addendLen;
  std::size_t actsLen;
  bool subtract;
};

struct TestCaseData {
  std::vector<double> addend;
  std::vector<double> acts;

  std::unique_ptr<char[]> rawAddend;
  std::unique_ptr<char[]> rawActs;
};

static bool addToChannelTests(const std::vector<TestCase> &cases) {

  const std::size_t overwriteLen = 32;

  auto device = createTestDevice(TEST_TARGET, 1, 1);
  const auto &target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  // One compute set, with a vertex for each test case.
  auto cs = graph.addComputeSet("cs");

  Sequence uploadProg;
  Sequence downloadProg;
  std::vector<std::pair<std::string, char *>> tmap;
  std::vector<TestCaseData> tcData(cases.size());

  for (std::size_t i = 0; i < cases.size(); ++i) {
    const auto &tc = cases[i];
    if (tc.actsLen % tc.addendLen != 0) {
      return false;
    }

    std::cout << "Test case [" << i << "]: "
              << " addendLen: " << tc.addendLen << " actsLen: " << tc.actsLen
              << " subtract: " << tc.subtract << " type: " << tc.type.toString()
              << "\n";

    std::string suffix = "_" + std::to_string(i);

    auto addend = graph.addVariable(tc.type, {tc.addendLen}, "addend" + suffix);
    graph.setTileMapping(addend, 0);
    auto acts = graph.addVariable(tc.type, {tc.actsLen + overwriteLen},
                                  "acts" + suffix);
    graph.setTileMapping(acts, 0);

    popops::expr::BinaryOpType op = tc.subtract
                                        ? popops::expr::BinaryOpType::SUBTRACT
                                        : popops::expr::BinaryOpType::ADD;
    std::string templateVertexName = templateVertex(
        "popops::BroadcastVectorInnerInPlaceSupervisor", op, tc.type);

    auto v = graph.addVertex(cs, templateVertexName,
                             {{"data", acts}, {"B", addend}});

    auto actsBlockCount = tc.actsLen / tc.addendLen;
    auto actsBlockCountPacked =
        ((actsBlockCount / 6) << 3) | (actsBlockCount % 6);
    uint16_t actsBlockCountPacked16 = actsBlockCountPacked;

    if (actsBlockCountPacked16 != actsBlockCountPacked)
      return false;

    graph.setInitialValue(v["dataBlockCountPacked"], actsBlockCountPacked16);

    graph.setTileMapping(v, 0);

    tcData[i].rawAddend = allocateHostMemoryForTensor(
        addend, "addend" + suffix, graph, uploadProg, downloadProg, tmap);
    tcData[i].rawActs = allocateHostMemoryForTensor(
        acts, "acts" + suffix, graph, uploadProg, downloadProg, tmap);

    tcData[i].addend.resize(tc.addendLen);
    tcData[i].acts.resize(tc.actsLen + overwriteLen);

    std::mt19937 randomEngine;
    writeRandomValues(target, addend.elementType(), tcData[i].addend.data(),
                      tcData[i].addend.data() + tcData[i].addend.size(), -2.,
                      2., randomEngine);
    writeRandomValues(target, acts.elementType(), tcData[i].acts.data(),
                      tcData[i].acts.data() + tcData[i].acts.size(), -2., 2.,
                      randomEngine);

    copy(target, tcData[i].addend.data(), tcData[i].addend.size(),
         addend.elementType(), tcData[i].rawAddend.get());
    copy(target, tcData[i].acts.data(), tcData[i].acts.size(),
         acts.elementType(), tcData[i].rawActs.get());
  }

  std::cout << "Executing engine\n";

  auto prog = Execute(cs);
  Engine engine(graph, Sequence(uploadProg, prog, downloadProg), options);

  attachStreams(engine, tmap);

  device.bind([&](const Device &d) {
    engine.load(d);
    engine.run(0);
  });

  std::cout << "Checking results\n";

  // Check the results for each test case.

  for (std::size_t i = 0; i < tcData.size(); ++i) {
    const auto &tc = cases[i];

    std::cout << "Checking case [" << i << "]\n";

    // Convert back to double.
    std::vector<double> actsOut(tcData[i].acts.size(), 0.0);

    copy(target, tc.type, tcData[i].rawActs.get(), actsOut.data(),
         actsOut.size());

    // Check the answer.

    auto actsRef = tcData[i].acts;
    for (std::size_t k = 0; k < tc.actsLen; ++k)
      actsRef[k] += tcData[i].addend[k % tc.addendLen] * (tc.subtract ? -1 : 1);

    const double absoluteTolerance =
        tc.type == FLOAT ? FLOAT_ABS_TOL : HALF_ABS_TOL;
    const double relativeTolerance =
        tc.type == FLOAT ? FLOAT_REL_TOL : HALF_REL_TOL;

    auto matchesModel =
        checkIsClose("out", actsOut.data(), {actsOut.size()}, actsRef.data(),
                     actsOut.size(), relativeTolerance, absoluteTolerance);
    if (!matchesModel)
      return false;
  }

  return true;
}

void runAddToChannelTests(std::vector<TestCase> cases) {
  // Test ScaledAddToChannel
  BOOST_TEST(addToChannelTests(cases));

  // Test AddToChannel
  for (auto &tc : cases)
    tc.subtract = false;
  BOOST_TEST(addToChannelTests(cases));
}

const bool isNotSim = !isSimulator(TEST_TARGET);

BOOST_AUTO_TEST_CASE(AddToChannelTiny) {
  std::vector<TestCase> cases = {
      {HALF, 1, 8, false},   {HALF, 4, 16, false},  {HALF, 8, 32, false},
      {HALF, 5, 15, false},  {FLOAT, 1, 8, false},  {FLOAT, 4, 16, false},
      {FLOAT, 8, 32, false}, {FLOAT, 5, 15, false},
  };
  runAddToChannelTests(cases);
}

BOOST_AUTO_TEST_CASE(AddToChannelSmall,
                     *boost::unit_test::precondition(enableIfNotSim())) {
  std::vector<TestCase> cases = {
      {HALF, 1, 480, false},   {HALF, 4, 480, false},   {HALF, 8, 480, false},
      {HALF, 12, 480, false},  {HALF, 16, 480, false},  {HALF, 1, 15, false},
      {HALF, 4, 12, false},    {HALF, 8, 40, false},    {HALF, 5, 15, false},
      {HALF, 8, 168, false},

      {FLOAT, 1, 480, false},  {FLOAT, 4, 480, false},  {FLOAT, 8, 480, false},
      {FLOAT, 12, 480, false}, {FLOAT, 16, 480, false}, {FLOAT, 1, 15, false},
      {FLOAT, 4, 12, false},   {FLOAT, 8, 40, false},   {FLOAT, 5, 15, false},
      {FLOAT, 8, 168, false},
  };
  runAddToChannelTests(cases);
}

std::size_t maxBlockCount() {
  // This is the maximum acts_block_count the vertex supports.
  // We can only test it for addend length of 1 otherwise it doesn't
  // fit in memory.
  return 4094 * 6;
}

BOOST_AUTO_TEST_CASE(AddToChannelLarge1_half,
                     *boost::unit_test::precondition(enableIfNotSim())) {
  std::vector<TestCase> cases = {
      {HALF, 1, maxBlockCount(), false},
  };
  runAddToChannelTests(cases);
}

BOOST_AUTO_TEST_CASE(AddToChannelLarge8_half,
                     *boost::unit_test::precondition(enableIfNotSim())) {
  std::vector<TestCase> cases = {
      {HALF, 8, 8000, false},
  };
  runAddToChannelTests(cases);
}

BOOST_AUTO_TEST_CASE(AddToChannelLarge1_float,
                     *boost::unit_test::precondition(enableIfNotSim())) {
  std::vector<TestCase> cases = {
      {FLOAT, 1, maxBlockCount(), false},
  };
  runAddToChannelTests(cases);
}

BOOST_AUTO_TEST_CASE(AddToChannelLarge8_float,
                     *boost::unit_test::precondition(enableIfNotSim())) {
  std::vector<TestCase> cases = {
      {FLOAT, 8, 8000, false},
  };
  runAddToChannelTests(cases);
}

// Above an addend length over 2048, we switch to scalar code. Check that works.
BOOST_AUTO_TEST_CASE(AddToChannel_MaxChannels_MultipleOfFour_half,
                     *boost::unit_test::precondition(enableIfNotSim())) {
  for (std::size_t addendLen = 2044; addendLen <= 2056; addendLen += 4) {
    std::vector<TestCase> cases = {
        {HALF, addendLen, addendLen * 4, false},
    };
    runAddToChannelTests(cases);
  }
}
