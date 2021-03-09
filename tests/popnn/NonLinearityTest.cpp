// Copyright (c) 2016 Graphcore Ltd. All rights reserved.
// Simple test case for IPU nonLinearity
//
#define BOOST_TEST_MODULE NonLinearityTest
#include "../popnn/NonLinearityInternal.hpp"
#include <boost/test/unit_test.hpp>
#include <iostream>
#include <limits>
#include <poplar/Engine.hpp>
#include <poplibs_support/TestDevice.hpp>
#include <poplibs_test/NonLinearity.hpp>
#include <poplibs_test/Util.hpp>
#include <poplin/codelets.hpp>
#include <popnn/NonLinearity.hpp>
#include <popnn/NonLinearityDefUtil.hpp>
#include <popnn/codelets.hpp>
#include <popops/EncodingConstants.hpp>
#include <popops/codelets.hpp>
#include <poputil/TileMapping.hpp>

using namespace poplar;
using namespace poplar::program;
using namespace poputil;
using namespace popnn;
using namespace poplibs_test;
using namespace poplibs_test::util;
using namespace poplibs_support;

namespace utf = boost::unit_test;
namespace fpc = boost::test_tools::fpc;

#define TOL 0.1 // tolerance of 0.1%
#define FLOAT_ATOL 1e-20
#define HALF_ATOL 1e-7

BOOST_AUTO_TEST_CASE(
    NonLinearity,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(TOL)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(TOL))) {
  auto device = createTestDevice(TEST_TARGET);
  auto &target = device.getTarget();
  Graph graph(target);
  popnn::addCodelets(graph);
  popops::addCodelets(graph);
  // layer parameters

  const unsigned zNGroups = 1;
  const std::size_t zChunk = 1;
  const std::size_t ySize = 100;
  const std::size_t xSize = 30;
  auto actF =
      graph.addVariable(FLOAT, {1, zNGroups, ySize, xSize, zChunk}, "actF");
  auto actH =
      graph.addVariable(HALF, {1, zNGroups, ySize, xSize, zChunk}, "actH");
  auto deltaF =
      graph.addVariable(FLOAT, {1, zNGroups, ySize, xSize, zChunk}, "actF");
  auto deltaH =
      graph.addVariable(HALF, {1, zNGroups, ySize, xSize, zChunk}, "actH");

  // arbitraray mappings
  mapTensorLinearly(graph, actF);
  mapTensorLinearly(graph, actH);
  mapTensorLinearly(graph, deltaF);
  mapTensorLinearly(graph, deltaH);

  graph.createHostWrite("inF", actF);
  graph.createHostWrite("inH", actH);
  graph.createHostRead("outF", actF);
  graph.createHostRead("outH", actH);
  graph.createHostWrite("inDeltaF", deltaF);
  graph.createHostWrite("inDeltaH", deltaH);
  graph.createHostRead("outDeltaF", deltaF);
  graph.createHostRead("outDeltaH", deltaH);

  const auto batchSize = 1;

  // test inputs calculated in harness
  boost::multi_array<double, 4> hActIn(
      boost::extents[batchSize][ySize][xSize][zChunk]);
  boost::multi_array<double, 4> hDeltaIn(
      boost::extents[batchSize][ySize][xSize][zChunk]);

  // outputs calculated by target code
  std::size_t actOutFSize = 0;
  std::size_t actOutHSize = 0;
  std::size_t actInFSize = 0;
  std::size_t actInHSize = 0;
  auto rawHActOutF = allocateHostMemoryForTensor(target, actF, 1, actOutFSize);
  auto rawHActOutH = allocateHostMemoryForTensor(target, actH, 1, actOutHSize);
  auto rawHActInF = allocateHostMemoryForTensor(target, actF, 1, actInFSize);
  auto rawHActInH = allocateHostMemoryForTensor(target, actH, 1, actInHSize);

  std::size_t dOutFSize = 0;
  std::size_t dOutHSize = 0;
  std::size_t dInFSize = 0;
  std::size_t dInHSize = 0;
  auto rawHDeltaOutF =
      allocateHostMemoryForTensor(target, deltaF, 1, dOutFSize);
  auto rawHDeltaOutH =
      allocateHostMemoryForTensor(target, deltaH, 1, dOutHSize);
  auto rawHDeltaInF = allocateHostMemoryForTensor(target, deltaF, 1, dInFSize);
  auto rawHDeltaInH = allocateHostMemoryForTensor(target, deltaH, 1, dInHSize);
  boost::multi_array<double, 4> hActOutF(
      boost::extents[batchSize][ySize][xSize][zChunk]);
  boost::multi_array<double, 4> hActOutH(
      boost::extents[batchSize][ySize][xSize][zChunk]);
  boost::multi_array<double, 4> hDeltaOutF(
      boost::extents[batchSize][ySize][xSize][zChunk]);
  boost::multi_array<double, 4> hDeltaOutH(
      boost::extents[batchSize][ySize][xSize][zChunk]);

  // reference results calculated in harness
  boost::multi_array<double, 4> hRefActOut(
      boost::extents[batchSize][ySize][xSize][zChunk]);
  boost::multi_array<double, 4> hRefDeltaOut(
      boost::extents[batchSize][ySize][xSize][zChunk]);

  // initialse hInF[][] to arbitrary values
  float val = -100.0;
  for (unsigned b = 0; b < batchSize; ++b) {
    for (unsigned y = 0; y < ySize; ++y) {
      for (unsigned x = 0; x < xSize; ++x) {
        for (unsigned chan = 0; chan < zChunk; chan++) {
          hRefDeltaOut[b][y][x][chan] = hDeltaIn[b][y][x][chan] = val / 200;
          hActIn[b][y][x][chan] = val + 1000 * chan;
        }
        val += 7.01;
        if (val > 200)
          val -= 400;
      }
    }
  }

  for (auto n : {
           NonLinearityType::RELU,
           NonLinearityType::SIGMOID,
           NonLinearityType::HARD_SIGMOID,
           NonLinearityType::TANH,
           NonLinearityType::GELU,
           NonLinearityType::SWISH,
       }) {
    // Check backward gradient calculations
    std::cerr << "Check nl type " << n << "\n";
    // Check forward activation calculation
    hRefActOut = hActIn;
    poplibs_test::nonLinearity(n, hRefActOut);
    // build and run the target code
    auto fwdProg = Sequence();
    nonLinearityInPlace(graph, n, actF, fwdProg);
    nonLinearityInPlace(graph, n, actH, fwdProg);
    ;
    Engine fwdEng(graph, fwdProg);
    device.bind([&](const Device &d) {
      fwdEng.load(d);
      copy(target, hActIn, FLOAT, rawHActInF.get());
      fwdEng.writeTensor("inF", rawHActInF.get(),
                         rawHActInF.get() + actInFSize);
      copy(target, hActIn, HALF, rawHActInH.get());
      fwdEng.writeTensor("inH", rawHActInH.get(),
                         rawHActInH.get() + actInHSize);
      fwdEng.run();
      fwdEng.readTensor("outF", rawHActOutF.get(),
                        rawHActOutF.get() + actOutFSize);
      fwdEng.readTensor("outH", rawHActOutH.get(),
                        rawHActOutH.get() + actOutHSize);
    });
    copy(target, HALF, rawHActOutH.get(), hActOutH);
    copy(target, FLOAT, rawHActOutF.get(), hActOutF);

    BOOST_TEST(checkIsClose("outF", hActOutF, hRefActOut, TOL, FLOAT_ATOL));
    BOOST_TEST(checkIsClose("outH", hActOutH, hRefActOut, TOL, HALF_ATOL));

    hRefDeltaOut = hDeltaIn;
    poplibs_test::bwdNonLinearity(n, hActIn, hRefDeltaOut);
    // build and run the target code
    auto bwdProg = Sequence();
    auto deltaFF = nonLinearityInputGradient(graph, n, actF, deltaF, bwdProg);
    bwdProg.add(Copy(deltaFF, deltaF));
    auto deltaHH = nonLinearityInputGradient(graph, n, actH, deltaH, bwdProg);
    bwdProg.add(Copy(deltaHH, deltaH));
    Engine bwdEng(graph, bwdProg);
    device.bind([&](const Device &d) {
      bwdEng.load(d);
      copy(target, hActIn, FLOAT, rawHActInF.get());
      bwdEng.writeTensor("inF", rawHActInF.get(),
                         rawHActInF.get() + actInFSize);
      copy(target, hActIn, HALF, rawHActInH.get());
      bwdEng.writeTensor("inH", rawHActInH.get(),
                         rawHActInH.get() + actInHSize);
      copy(target, hDeltaIn, FLOAT, rawHDeltaInF.get());
      bwdEng.writeTensor("inDeltaF", rawHDeltaInF.get(),
                         rawHDeltaInF.get() + dInFSize);
      copy(target, hDeltaIn, HALF, rawHDeltaInH.get());
      bwdEng.writeTensor("inDeltaH", rawHDeltaInH.get(),
                         rawHDeltaInH.get() + dInHSize);
      bwdEng.run();
      bwdEng.readTensor("outDeltaF", rawHDeltaOutF.get(),
                        rawHDeltaOutF.get() + dOutFSize);
      bwdEng.readTensor("outDeltaH", rawHDeltaOutH.get(),
                        rawHDeltaOutH.get() + dOutHSize);
    });
    copy(target, HALF, rawHDeltaOutH.get(), hDeltaOutH);
    copy(target, FLOAT, rawHDeltaOutF.get(), hDeltaOutF);

    BOOST_TEST(
        checkIsClose("deltaOutF", hDeltaOutF, hRefDeltaOut, TOL, FLOAT_ATOL));
    BOOST_TEST(
        checkIsClose("deltaOutH", hDeltaOutH, hRefDeltaOut, TOL, HALF_ATOL));
  }
}

BOOST_AUTO_TEST_CASE(
    NonLinearitySoftMax,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.1))) {
  auto device = createTestDevice(TEST_TARGET);
  auto &target = device.getTarget();
  Graph graph(target);
  popnn::addCodelets(graph);
  popops::addCodelets(graph);
  poplin::addCodelets(graph);

  // support only 2D
  const unsigned batchSize = 2;
  const unsigned numChannels = 128;

  auto actF = graph.addVariable(FLOAT, {batchSize, numChannels}, "actF");
  auto actH = graph.addVariable(HALF, {batchSize, numChannels}, "actH");
  auto deltaF = graph.addVariable(FLOAT, {batchSize, numChannels}, "deltaF");
  auto deltaH = graph.addVariable(HALF, {batchSize, numChannels}, "deltaH");

  // arbitrary mappings
  mapTensorLinearly(graph, actF);
  mapTensorLinearly(graph, actH);
  mapTensorLinearly(graph, deltaF);
  mapTensorLinearly(graph, deltaH);

  std::vector<std::pair<std::string, char *>> tmap;
  Sequence uploadProg, downloadProg;

  auto rawHActF = allocateHostMemoryForTensor(actF, "actF", graph, uploadProg,
                                              downloadProg, tmap);
  auto rawHActH = allocateHostMemoryForTensor(actH, "actH", graph, uploadProg,
                                              downloadProg, tmap);
  auto rawHDeltaF = allocateHostMemoryForTensor(deltaF, "deltaF", graph,
                                                uploadProg, downloadProg, tmap);
  auto rawHDeltaH = allocateHostMemoryForTensor(deltaH, "deltaH", graph,
                                                uploadProg, downloadProg, tmap);

  boost::multi_array<double, 2> hActIn(boost::extents[batchSize][numChannels]),
      hDeltaIn(boost::extents[batchSize][numChannels]),
      hActOutF(boost::extents[batchSize][numChannels]),
      hActOutH(boost::extents[batchSize][numChannels]),
      hDeltaOutF(boost::extents[batchSize][numChannels]),
      hDeltaOutH(boost::extents[batchSize][numChannels]);

  for (unsigned b = 0; b < batchSize; ++b) {
    for (unsigned c = 0; c < numChannels; ++c) {
      double sample = (1.0 - 2 * (c & 1)) * (1 + b) * 0.01 * c;
      hActIn[b][c] = sample;
      hDeltaIn[b][c] = double(b * numChannels) - double(c * batchSize);
    }
  }

  for (const auto nl :
       {NonLinearityType::SOFTMAX, NonLinearityType::SOFTMAX_STABLE,
        NonLinearityType::SOFTMAX_SCALED}) {
    auto hActOut = hActIn;
    poplibs_test::nonLinearity(nl, hActOut);
    if (nl == NonLinearityType::SOFTMAX_SCALED) {
      for (unsigned i = 0; i < batchSize; i++) {
        for (unsigned j = 0; j < numChannels; j++) {
          hActOut[i][j] *= SOFTMAX_SCALING;
        }
      }
    }
    // build and run the target code
    auto fwdProg = Sequence();
    float nonLinearityScalingF, nonLinearityScalingH;
    nonLinearityInPlace(graph, nl, actF, nonLinearityScalingF, fwdProg);
    nonLinearityInPlace(graph, nl, actH, nonLinearityScalingH, fwdProg);
    const float expectedScaling =
        nl == NonLinearityType::SOFTMAX_SCALED ? SOFTMAX_SCALING : 1.0f;

    BOOST_TEST(nonLinearityScalingF == expectedScaling);
    BOOST_TEST(nonLinearityScalingH == expectedScaling);

    copy(target, hActIn, FLOAT, rawHActF.get());
    copy(target, hActIn, HALF, rawHActH.get());
    Engine fwdEng(graph, Sequence(uploadProg, fwdProg, downloadProg));
    attachStreams(fwdEng, tmap);
    device.bind([&](const Device &d) { fwdEng.loadAndRun(d); });
    copy(target, FLOAT, rawHActF.get(), hActOutF);
    copy(target, HALF, rawHActH.get(), hActOutH);

    BOOST_TEST(checkIsClose("actOutF", hActOutF, hActOut, TOL, FLOAT_ATOL));
    BOOST_TEST(checkIsClose("actOutH", hActOutH, hActOut, TOL, HALF_ATOL));

    auto hRefDeltaOut = hDeltaIn;
    poplibs_test::bwdNonLinearity(nl, hActIn, hRefDeltaOut);

    auto bwdProg = Sequence();
    auto deltaFF = nonLinearityInputGradient(graph, nl, actF, deltaF, bwdProg);
    auto deltaHH = nonLinearityInputGradient(graph, nl, actH, deltaH, bwdProg);
    bwdProg.add(Copy(deltaFF, deltaF));
    bwdProg.add(Copy(deltaHH, deltaH));

    copy(target, hActIn, FLOAT, rawHActF.get());
    copy(target, hActIn, HALF, rawHActH.get());
    copy(target, hDeltaIn, FLOAT, rawHDeltaF.get());
    copy(target, hDeltaIn, HALF, rawHDeltaH.get());
    Engine bwdEng(graph, Sequence(uploadProg, bwdProg, downloadProg));
    attachStreams(bwdEng, tmap);
    device.bind([&](const Device &d) { bwdEng.loadAndRun(d); });
    copy(target, FLOAT, rawHDeltaF.get(), hDeltaOutF);
    copy(target, HALF, rawHDeltaH.get(), hDeltaOutH);

    BOOST_TEST(
        checkIsClose("deltaOutF", hDeltaOutF, hRefDeltaOut, TOL, FLOAT_ATOL));
    BOOST_TEST(
        checkIsClose("deltaOutH", hDeltaOutH, hRefDeltaOut, TOL, HALF_ATOL));
  }
}

BOOST_AUTO_TEST_CASE(
    NonLinearitySoftMax1D,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.1))) {
  auto device = createTestDevice(TEST_TARGET);
  auto &target = device.getTarget();
  Graph graph(target);
  popnn::addCodelets(graph);
  popops::addCodelets(graph);
  poplin::addCodelets(graph);
  const unsigned numChannels = 128;

  auto actF = graph.addVariable(FLOAT, {numChannels}, "actF");
  auto actH = graph.addVariable(HALF, {numChannels}, "actH");
  auto deltaF = graph.addVariable(FLOAT, {numChannels}, "deltaF");
  auto deltaH = graph.addVariable(HALF, {numChannels}, "deltaH");

  // arbitrary mappings
  mapTensorLinearly(graph, actF);
  mapTensorLinearly(graph, actH);
  mapTensorLinearly(graph, deltaF);
  mapTensorLinearly(graph, deltaH);

  std::vector<std::pair<std::string, char *>> tmap;
  Sequence uploadProg, downloadProg;

  auto rawHActF = allocateHostMemoryForTensor(actF, "actF", graph, uploadProg,
                                              downloadProg, tmap);
  auto rawHActH = allocateHostMemoryForTensor(actH, "actH", graph, uploadProg,
                                              downloadProg, tmap);
  auto rawHDeltaF = allocateHostMemoryForTensor(deltaF, "deltaF", graph,
                                                uploadProg, downloadProg, tmap);
  auto rawHDeltaH = allocateHostMemoryForTensor(deltaH, "deltaH", graph,
                                                uploadProg, downloadProg, tmap);

  boost::multi_array<double, 2> hActIn(boost::extents[1][numChannels]),
      hDeltaIn(boost::extents[1][numChannels]),
      hActOutF(boost::extents[1][numChannels]),
      hActOutH(boost::extents[1][numChannels]),
      hDeltaOutF(boost::extents[1][numChannels]),
      hDeltaOutH(boost::extents[1][numChannels]);

  for (unsigned c = 0; c < numChannels; ++c) {
    double sample = (1.0 - 2 * (c & 1)) * 0.01 * c;
    hActIn[0][c] = sample;
    hDeltaIn[0][c] = double(numChannels - c);
  }

  for (const auto nl :
       {NonLinearityType::SOFTMAX, NonLinearityType::SOFTMAX_STABLE,
        NonLinearityType::SOFTMAX_SCALED}) {
    auto hActOut = hActIn;
    poplibs_test::nonLinearity(nl, hActOut);
    if (nl == NonLinearityType::SOFTMAX_SCALED) {
      for (unsigned i = 0; i < numChannels; i++) {
        hActOut[0][i] *= SOFTMAX_SCALING;
      }
    }
    // build and run the target code
    auto fwdProg = Sequence();
    float nonLinearityScalingF, nonLinearityScalingH;
    nonLinearityInPlace(graph, nl, actF, nonLinearityScalingF, fwdProg);
    nonLinearityInPlace(graph, nl, actH, nonLinearityScalingH, fwdProg);
    const float expectedScaling =
        nl == NonLinearityType::SOFTMAX_SCALED ? SOFTMAX_SCALING : 1.0f;

    BOOST_TEST(nonLinearityScalingF == expectedScaling);
    BOOST_TEST(nonLinearityScalingH == expectedScaling);

    copy(target, hActIn, FLOAT, rawHActF.get());
    copy(target, hActIn, HALF, rawHActH.get());
    Engine fwdEng(graph, Sequence(uploadProg, fwdProg, downloadProg));
    attachStreams(fwdEng, tmap);
    device.bind([&](const Device &d) { fwdEng.loadAndRun(d); });
    copy(target, FLOAT, rawHActF.get(), hActOutF);
    copy(target, HALF, rawHActH.get(), hActOutH);

    BOOST_TEST(checkIsClose("actOutF", hActOutF, hActOut, TOL, FLOAT_ATOL));
    BOOST_TEST(checkIsClose("actOutH", hActOutH, hActOut, TOL, HALF_ATOL));

    auto hRefDeltaOut = hDeltaIn;
    poplibs_test::bwdNonLinearity(nl, hActIn, hRefDeltaOut);

    auto bwdProg = Sequence();
    auto deltaFF = nonLinearityInputGradient(graph, nl, actF, deltaF, bwdProg);
    auto deltaHH = nonLinearityInputGradient(graph, nl, actH, deltaH, bwdProg);
    bwdProg.add(Copy(deltaFF, deltaF));
    bwdProg.add(Copy(deltaHH, deltaH));

    copy(target, hActIn, FLOAT, rawHActF.get());
    copy(target, hActIn, HALF, rawHActH.get());
    copy(target, hDeltaIn, FLOAT, rawHDeltaF.get());
    copy(target, hDeltaIn, HALF, rawHDeltaH.get());
    Engine bwdEng(graph, Sequence(uploadProg, bwdProg, downloadProg));
    attachStreams(bwdEng, tmap);
    device.bind([&](const Device &d) { bwdEng.loadAndRun(d); });
    copy(target, FLOAT, rawHDeltaF.get(), hDeltaOutF);
    copy(target, HALF, rawHDeltaH.get(), hDeltaOutH);

    BOOST_TEST(
        checkIsClose("deltaOutF", hDeltaOutF, hRefDeltaOut, TOL, FLOAT_ATOL));
    BOOST_TEST(
        checkIsClose("deltaOutH", hDeltaOutH, hRefDeltaOut, TOL, HALF_ATOL));
  }
}

// Test fixture with common graph setup for testing the hard sigmoid activation
// function.
struct NonLinearityHardSigmoidTest {
  NonLinearityHardSigmoidTest()
      : device_(createTestDevice(TEST_TARGET)), graph_(device_.getTarget()) {
    popnn::addCodelets(graph_);
    popops::addCodelets(graph_);
  }

  std::vector<float> runProgram(Program &program, Tensor &inputTensor,
                                Tensor &outputTensor,
                                const std::vector<float> &inputData) {
    graph_.createHostWrite("in", inputTensor);
    graph_.createHostRead("out", outputTensor);

    std::vector<float> output(inputData.size());

    Engine engine(graph_, program);
    device_.bind([&](const Device &d) {
      engine.load(d);
      engine.writeTensor("in", inputData.data(),
                         inputData.data() + inputData.size());
      engine.run();
      engine.readTensor("out", output.data(), output.data() + output.size());
    });

    return output;
  }

  poplibs_support::TestDevice device_;
  Graph graph_;

  Sequence program_;
  VariableMappingMethod variableMappingMethod_ = VariableMappingMethod::LINEAR;
};

BOOST_FIXTURE_TEST_CASE(HardSigmoid_ValuesBelowRangeAreClampedTo0,
                        NonLinearityHardSigmoidTest) {
  const std::vector<float> inputData = {-10, -5, -3, -2.7, -2.6, -2.55};

  auto inputTensor = graph_.addVariable(FLOAT, {inputData.size()},
                                        variableMappingMethod_, "activation");

  auto outputTensor = nonLinearity(graph_, NonLinearityType::HARD_SIGMOID,
                                   inputTensor, program_);

  const auto output =
      runProgram(program_, inputTensor, outputTensor, inputData);
  for (auto value : output) {
    BOOST_TEST(value == 0);
  }
}

BOOST_FIXTURE_TEST_CASE(HardSigmoid_ValuesAboveRangeAreClampedTo1,
                        NonLinearityHardSigmoidTest) {
  const std::vector<float> inputData = {10, 5, 3, 2.7, 2.6, 2.55};
  auto inputTensor = graph_.addVariable(FLOAT, {inputData.size()},
                                        variableMappingMethod_, "activation");

  auto outputTensor = nonLinearity(graph_, NonLinearityType::HARD_SIGMOID,
                                   inputTensor, program_);

  const auto output =
      runProgram(program_, inputTensor, outputTensor, inputData);
  for (auto value : output) {
    BOOST_TEST(value == 1);
  }
}

BOOST_FIXTURE_TEST_CASE(HardSigmoid_OutputIsBetween01ForValuesWithinRange,
                        NonLinearityHardSigmoidTest) {
  // Valid range is [-2.5, +2.5]
  const std::vector<float> inputData = {-2.5, -2.4, -2, -1,  -0.5,
                                        0.5,  1,    2,  2.4, 2.5};
  auto inputTensor = graph_.addVariable(FLOAT, {inputData.size()},
                                        variableMappingMethod_, "activation");

  auto outputTensor = nonLinearity(graph_, NonLinearityType::HARD_SIGMOID,
                                   inputTensor, program_);

  const auto output =
      runProgram(program_, inputTensor, outputTensor, inputData);
  for (auto value : output) {
    BOOST_TEST(value <= 1);
    BOOST_TEST(value >= 0);
  }
}

struct NonLinearityInputGradientHardSigmoidTest : NonLinearityHardSigmoidTest {
  std::vector<float> runProgram(Program &program, Tensor &inputTensor,
                                Tensor &gradientTensor, Tensor &outputTensor,
                                const std::vector<float> &inputData,
                                const std::vector<float> &gradientData) {
    graph_.createHostWrite("in", inputTensor);
    graph_.createHostWrite("grad", gradientTensor);
    graph_.createHostRead("out", outputTensor);

    std::vector<float> output(inputData.size());

    Engine engine(graph_, program_);
    device_.bind([&](const Device &d) {
      engine.load(d);
      engine.writeTensor("in", inputData.data(),
                         inputData.data() + inputData.size());
      engine.writeTensor("grad", gradientData.data(),
                         gradientData.data() + gradientData.size());
      engine.run();
      engine.readTensor("out", output.data(), output.data() + output.size());
    });

    return output;
  }
};

BOOST_FIXTURE_TEST_CASE(HardSigmoid_GradientIs0ForValuesOutsideValidRange,
                        NonLinearityInputGradientHardSigmoidTest) {
  const std::vector<float> inputData = {-2.6, -2.55, -3, -5, 2.55, 2.6, 10, 3};
  const std::vector<float> gradientData(inputData.size(), 1);

  auto inputTensor = graph_.addVariable(FLOAT, {inputData.size()},
                                        variableMappingMethod_, "activation");
  auto gradientTensor = graph_.addVariable(FLOAT, {gradientData.size()},
                                           variableMappingMethod_, "gradient");

  auto outputTensor =
      nonLinearityInputGradient(graph_, NonLinearityType::HARD_SIGMOID,
                                inputTensor, gradientTensor, program_);

  const auto output = runProgram(program_, inputTensor, gradientTensor,
                                 outputTensor, inputData, gradientData);
  for (auto value : output) {
    BOOST_CHECK(value == 0);
  }
}

BOOST_FIXTURE_TEST_CASE(HardSigmoid_GradientIs0_2ForValuesInsideValidRange,
                        NonLinearityInputGradientHardSigmoidTest) {
  const std::vector<float> inputData = {-2.5, -2.4, -2, -1,  -0.5,
                                        0.5,  1,    2,  2.4, 2.5};
  const std::vector<float> gradientData(inputData.size(), 1);

  auto inputTensor = graph_.addVariable(FLOAT, {inputData.size()},
                                        variableMappingMethod_, "activation");
  auto gradientTensor = graph_.addVariable(FLOAT, {gradientData.size()},
                                           variableMappingMethod_, "gradient");

  auto outputTensor =
      nonLinearityInputGradient(graph_, NonLinearityType::HARD_SIGMOID,
                                inputTensor, gradientTensor, program_);

  const auto output = runProgram(program_, inputTensor, gradientTensor,
                                 outputTensor, inputData, gradientData);
  for (auto value : output) {
    BOOST_CHECK(value == 0.2f);
  }
}
