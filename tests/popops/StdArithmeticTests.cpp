// Copyright (c) 2017 Graphcore Ltd. All rights reserved.
#define BOOST_TEST_MODULE StdArithmeticTests

#include "popops/ElementWise.hpp"
#include <boost/multi_array.hpp>
#include <boost/test/data/test_case.hpp>
#include <boost/test/unit_test.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
#include <poplar/CSRFunctions.hpp>
#include <poplar/Engine.hpp>
#include <poplar/IPUModel.hpp>
#include <poplar/OptionFlags.hpp>
#include <poplibs_support/TestDevice.hpp>
#include <popops/Cast.hpp>
#include <popops/ScaledAdd.hpp>
#include <popops/codelets.hpp>
#include <poputil/TileMapping.hpp>
#include <pva/pva.hpp>
#include <vector>

using namespace poplar;
using namespace poplar::program;
using namespace poputil;
using namespace popops;
using namespace poplibs_support;

namespace utf = boost::unit_test;
namespace fpc = boost::test_tools::fpc;

#define DIM_SIZE 10

__attribute((unused)) static std::tuple<Tensor, Tensor>
mapBinaryOpTensors(Graph &graph, const Type &type) {
  auto in1 = graph.addVariable(type, {DIM_SIZE, DIM_SIZE}, "in1");
  mapTensorLinearly(graph, in1);

  auto in2 = graph.addVariable(type, {DIM_SIZE, DIM_SIZE}, "in2");
  mapTensorLinearly(graph, in2);

  return std::make_pair(in1.dimShuffle({1, 0}), in2.dimShuffle({1, 0}));
}

__attribute((unused)) static void
setBinaryOpInputs(float hIn1[DIM_SIZE][DIM_SIZE],
                  float hIn2[DIM_SIZE][DIM_SIZE]) {
  float val1 = -100;
  float val2 = 50;
  for (auto r = 0U; r != DIM_SIZE; ++r) {
    for (auto c = 0U; c != DIM_SIZE; ++c) {
      float sign1 = (1.0 - 2.0 * ((c + 1) & 1));
      float sign2 = (1.0 - 2.0 * ((r + c) & 1));
      hIn1[r][c] = (val1 + (r * DIM_SIZE + c) * .1) * sign1;
      hIn2[r][c] = (val2 + (r * DIM_SIZE + c) * .1) * sign2;
    }
  }
}

__attribute((unused)) static void
setBinaryOpInputs(int hIn1[DIM_SIZE][DIM_SIZE], int hIn2[DIM_SIZE][DIM_SIZE]) {
  int val1 = -100;
  int val2 = 50;
  for (auto r = 0; r != DIM_SIZE; ++r) {
    for (auto c = 0; c != DIM_SIZE; ++c) {
      int sign1 = (1 - 2 * ((c + 1) & 1));
      int sign2 = (1 - 2 * ((r + c) & 1));
      hIn1[r][c] = (val1 + (r * DIM_SIZE + c) * 1) * sign1;
      hIn2[r][c] = (val2 + (r * DIM_SIZE + c) * 1) * sign2;
    }
  }
}
__attribute((unused)) static void
setBroadcastOpInputs(float hIn1[DIM_SIZE][DIM_SIZE]) {
  float val1 = -100;
  for (auto r = 0; r != DIM_SIZE; ++r) {
    for (auto c = 0; c != DIM_SIZE; ++c) {
      int sign1 = (1 - 2 * ((c + 1) & 1));
      hIn1[r][c] = (val1 + (r * DIM_SIZE + c) * 1) * sign1;
    }
  }
}

BOOST_AUTO_TEST_CASE(
    StdBroadcastAdd_float,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  float hIn[DIM_SIZE][DIM_SIZE];
  setBroadcastOpInputs(hIn);

  float k = 2;
  auto B = graph.addVariable(FLOAT, {});
  graph.setInitialValue(B, k);
  auto in = graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "in1");
  mapTensorLinearly(graph, in);
  mapTensorLinearly(graph, B);

  graph.createHostWrite("in", in);
  graph.createHostRead("out", in);
  auto prog = Sequence();

  addInPlace(graph, in, B, prog);
  Engine eng(graph, prog);
  float hOut[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);

    eng.writeTensor("in", hIn, &hIn[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", hOut, &hOut[DIM_SIZE]);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn[i][j] + k;
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(
    StdBroadcastMultiply_float,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  float hIn[DIM_SIZE][DIM_SIZE];
  setBroadcastOpInputs(hIn);

  float k = 2;
  auto B = graph.addVariable(FLOAT, {});
  graph.setInitialValue(B, k);
  auto in = graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "in1");
  mapTensorLinearly(graph, in);
  mapTensorLinearly(graph, B);

  graph.createHostWrite("in", in);
  graph.createHostRead("out", in);
  auto prog = Sequence();

  mulInPlace(graph, in, B, prog);
  Engine eng(graph, prog);
  float hOut[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);

    eng.writeTensor("in", hIn, &hIn[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", hOut, &hOut[DIM_SIZE]);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn[i][j] * k;
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(
    StdBroadcastSubtract_half,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))) {
  auto device = createTestDevice(TEST_TARGET);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  float hIn[DIM_SIZE][DIM_SIZE];
  setBroadcastOpInputs(hIn);

  auto rawBufSize = target.getTypeSize(HALF) * DIM_SIZE * DIM_SIZE;
  std::vector<char> rawIn(rawBufSize);
  poplar::copyFloatToDeviceHalf(target, &hIn[0][0], rawIn.data(),
                                DIM_SIZE * DIM_SIZE);

  float k = 2;
  auto B = graph.addVariable(HALF, {});
  graph.setInitialValue(B, k);
  auto in = graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "in1");
  mapTensorLinearly(graph, in);
  mapTensorLinearly(graph, B);

  std::vector<char> rawOut(rawBufSize);
  graph.createHostWrite("in", in);
  graph.createHostRead("out", in);
  auto prog = Sequence();

  subInPlace(graph, in, B, prog);
  Engine eng(graph, prog);

  device.bind([&](const Device &d) {
    eng.load(d);

    eng.writeTensor("in", rawIn.data(), rawIn.data() + rawIn.size());
    eng.run();
    eng.readTensor("out", rawOut.data(), rawOut.data() + rawOut.size());
  });

  float hOut[DIM_SIZE][DIM_SIZE];
  poplar::copyDeviceHalfToFloat(target, rawOut.data(), &hOut[0][0],
                                DIM_SIZE * DIM_SIZE);

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn[i][j] - k;
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(
    StdAddTo_half_float_tensor,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(1.4)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(1.4))) {
  auto device = createTestDevice(TEST_TARGET);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  float hIn1[DIM_SIZE][DIM_SIZE], hIn2[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn1, hIn2);

  float k = 2;
  auto factor = graph.addVariable(HALF, {});
  graph.setInitialValue(factor, k);

  auto in1 = graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "in1");
  auto in2 = graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "in2");
  mapTensorLinearly(graph, in1);
  mapTensorLinearly(graph, in2);
  mapTensorLinearly(graph, factor);

  auto rawBufSize = target.getTypeSize(HALF) * DIM_SIZE * DIM_SIZE;
  std::vector<char> rawIn1(rawBufSize);
  poplar::copyFloatToDeviceHalf(target, &hIn1[0][0], rawIn1.data(),
                                DIM_SIZE * DIM_SIZE);

  graph.createHostWrite("in1", in1);
  graph.createHostWrite("in2", in2);
  graph.createHostRead("out", in1);
  auto prog = Sequence();
  scaledAddTo(graph, in1, in2, factor, prog);
  Engine eng(graph, prog);

  std::vector<char> rawOut(rawBufSize);
  float hOut[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);
    eng.writeTensor("in1", rawIn1.data(), rawIn1.data() + rawIn1.size());
    eng.writeTensor("in2", hIn2, &hIn2[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", rawOut.data(), rawOut.data() + rawOut.size());
  });

  poplar::copyDeviceHalfToFloat(target, rawOut.data(), &hOut[0][0],
                                DIM_SIZE * DIM_SIZE);

  // Check result
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] + k * hIn2[i][j];
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(
    StdAddTo_float_half,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(1.4)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(1.4))) {
  auto device = createTestDevice(TEST_TARGET, 1, 2);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  float hIn1[DIM_SIZE][DIM_SIZE], hIn2[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn1, hIn2);

  float k = 2.1;
  auto factorHalf = graph.addVariable(HALF, {});
  auto factorFloat = graph.addVariable(FLOAT, {});
  graph.setInitialValue(factorHalf, k);
  graph.setInitialValue(factorFloat, k);

  auto in1TensorFloat = graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "in1TF");
  auto in1TensorHalf = graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "in1TH");
  auto in1ConstFloat = graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "in1CF");
  auto in1ConstHalf = graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "in1CH");
  auto in2 = graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "in2");
  graph.setTileMapping(in1TensorFloat, 0);
  graph.setTileMapping(in1TensorHalf, 0);
  graph.setTileMapping(in1ConstFloat, 0);
  graph.setTileMapping(in1ConstHalf, 0);

  mapTensorLinearly(graph, in2);
  mapTensorLinearly(graph, factorHalf);
  mapTensorLinearly(graph, factorFloat);

  // Map differently, causing 2D decisions and vertex connection to happen
  graph.setTileMapping(in1TensorFloat.flatten().slice({4, 8}), 1);
  graph.setTileMapping(in1ConstFloat.flatten().slice({4, 8}), 1);
  graph.setTileMapping(in1TensorHalf.flatten().slice({4, 8}), 1);
  graph.setTileMapping(in1ConstHalf.flatten().slice({4, 8}), 1);

  auto rawBufSize = target.getTypeSize(HALF) * DIM_SIZE * DIM_SIZE;
  std::vector<char> rawIn2(rawBufSize);
  poplar::copyFloatToDeviceHalf(target, &hIn2[0][0], rawIn2.data(),
                                DIM_SIZE * DIM_SIZE);

  graph.createHostWrite("in1TF", in1TensorFloat);
  graph.createHostWrite("in1TH", in1TensorHalf);
  graph.createHostWrite("in1CF", in1ConstFloat);
  graph.createHostWrite("in1CH", in1ConstHalf);
  graph.createHostWrite("in2", in2);
  graph.createHostRead("outTF", in1TensorFloat);
  graph.createHostRead("outTH", in1TensorHalf);
  graph.createHostRead("outCF", in1ConstFloat);
  graph.createHostRead("outCH", in1ConstHalf);
  auto prog = Sequence();
  scaledAddTo(graph, in1TensorFloat, in2, factorFloat, prog);
  scaledAddTo(graph, in1TensorHalf, in2, factorHalf, prog);
  scaledAddTo(graph, in1ConstFloat, in2, k, prog, "ForcedFloatScale",
              {{"scaleFloatToHalfTolerance", "2.0"}});
  scaledAddTo(graph, in1ConstHalf, in2, k, prog);
  Engine eng(graph, prog);

  float hOutTensorFloat[DIM_SIZE][DIM_SIZE];
  float hOutTensorHalf[DIM_SIZE][DIM_SIZE];
  float hOutConstFloat[DIM_SIZE][DIM_SIZE];
  float hOutConstHalf[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);
    eng.writeTensor("in1TF", hIn1, &hIn1[DIM_SIZE]);
    eng.writeTensor("in1TH", hIn1, &hIn1[DIM_SIZE]);
    eng.writeTensor("in1CF", hIn1, &hIn1[DIM_SIZE]);
    eng.writeTensor("in1CH", hIn1, &hIn1[DIM_SIZE]);
    eng.writeTensor("in2", rawIn2.data(), rawIn2.data() + rawIn2.size());
    eng.run();
    eng.readTensor("outTF", hOutTensorFloat, &hOutTensorFloat[DIM_SIZE]);
    eng.readTensor("outTH", hOutTensorHalf, &hOutTensorHalf[DIM_SIZE]);
    eng.readTensor("outCF", hOutConstFloat, &hOutConstFloat[DIM_SIZE]);
    eng.readTensor("outCH", hOutConstHalf, &hOutConstHalf[DIM_SIZE]);
  });

  // Check result
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] + k * hIn2[i][j];
      BOOST_TEST(hOutTensorFloat[i][j] == res);
      BOOST_TEST(hOutTensorHalf[i][j] == res);
      BOOST_TEST(hOutConstFloat[i][j] == res);
      BOOST_TEST(hOutConstHalf[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(
    StdAddTo_half_scale_float_tensor_const,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.1))) {
  auto device = createTestDevice(TEST_TARGET, 1, 2);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  float hIn1[DIM_SIZE];
  float hIn2[DIM_SIZE];

  // Large values for the 2nd operand which, when multiplied by a very small
  // scale should have a sensible result
  for (unsigned i = 0; i < DIM_SIZE; i++) {
    hIn1[i] = (1.0f / 1024.0f);
    hIn2[i] = 10000 * (i % 4);
  }
  // Very small k to make the selection of codelets work to solve the issue
  // that it can't be represented as a half
  float k = (3.0e-9);
  auto factor = graph.addVariable(FLOAT, {});
  graph.setInitialValue(factor, k);

  auto in1 = graph.addVariable(HALF, {DIM_SIZE}, "in1");
  auto in1ConstTest = graph.addVariable(HALF, {DIM_SIZE}, "in1");
  auto in1Fails = graph.addVariable(HALF, {DIM_SIZE}, "in1");
  auto in1ConstTestFails = graph.addVariable(HALF, {DIM_SIZE}, "in1");
  auto in2 = graph.addVariable(HALF, {DIM_SIZE}, "in2");
  mapTensorLinearly(graph, in1);
  mapTensorLinearly(graph, in1ConstTest);
  mapTensorLinearly(graph, in1Fails);
  mapTensorLinearly(graph, in1ConstTestFails);
  mapTensorLinearly(graph, in2);
  mapTensorLinearly(graph, factor);
  // Map differently, causing 2D decisions and vertex connection to happen
  graph.setTileMapping(in1[4], 1);

  auto rawBufSize = target.getTypeSize(HALF) * DIM_SIZE;
  std::vector<char> rawIn1(rawBufSize), rawIn2(rawBufSize);
  poplar::copyFloatToDeviceHalf(target, &hIn1[0], rawIn1.data(), DIM_SIZE);
  poplar::copyFloatToDeviceHalf(target, &hIn2[0], rawIn2.data(), DIM_SIZE);

  graph.createHostWrite("in1", in1);
  graph.createHostWrite("in1ConstTest", in1ConstTest);
  graph.createHostWrite("in1Fails", in1Fails);
  graph.createHostWrite("in1ConstTestFails", in1ConstTestFails);
  graph.createHostWrite("in2", in2);
  graph.createHostRead("out", in1);
  graph.createHostRead("outConstTest", in1ConstTest);
  graph.createHostRead("outFails", in1Fails);
  graph.createHostRead("outConstTestFails", in1ConstTestFails);

  auto prog = Sequence();
  // These tests should produce a reasonable answer as the scale and multiply
  // is implemented in full precision
  scaledAddTo(graph, in1, in2, factor, prog, "Tensor test",
              {{"scaleFloatToHalfTolerance", "1e-6"}});
  scaledAddTo(graph, in1ConstTest, in2, k, prog, "Const test",
              {{"scaleFloatToHalfTolerance", "1e-6"}});

  // These tests should "fail", producing 0.0 exactly as the scale becomes
  // 0.0 in half precision.  We use the tolerance option to switch
  // automatic selection of full precision arithmetic off.
  scaledAddTo(graph, in1Fails, in2, factor, prog, "Tensor test fail",
              {{"scaleFloatToHalfTolerance", "2.0"}});
  scaledAddTo(graph, in1ConstTestFails, in2, k, prog, "Const test fail",
              {{"scaleFloatToHalfTolerance", "2.0"}});
  Engine eng(graph, prog);

  std::vector<char> rawOut(rawBufSize), rawOutConstTest(rawBufSize);
  std::vector<char> rawOutFails(rawBufSize), rawOutConstTestFails(rawBufSize);

  device.bind([&](const Device &d) {
    eng.load(d);

    eng.writeTensor("in1", rawIn1.data(), rawIn1.data() + rawIn1.size());
    eng.writeTensor("in1ConstTest", rawIn1.data(),
                    rawIn1.data() + rawIn1.size());
    eng.writeTensor("in1Fails", rawIn1.data(), rawIn1.data() + rawIn1.size());
    eng.writeTensor("in1ConstTestFails", rawIn1.data(),
                    rawIn1.data() + rawIn1.size());
    eng.writeTensor("in2", rawIn2.data(), rawIn2.data() + rawIn2.size());

    eng.run();

    eng.readTensor("out", rawOut.data(), rawOut.data() + rawOut.size());
    eng.readTensor("outConstTest", rawOutConstTest.data(),
                   rawOutConstTest.data() + rawOutConstTest.size());
    eng.readTensor("outFails", rawOutFails.data(),
                   rawOutFails.data() + rawOutFails.size());
    eng.readTensor("outConstTestFails", rawOutConstTestFails.data(),
                   rawOutConstTestFails.data() + rawOutConstTestFails.size());
  });

  float hOut[DIM_SIZE], hOutConstTest[DIM_SIZE];
  float hOutFails[DIM_SIZE], hOutConstTestFails[DIM_SIZE];
  poplar::copyDeviceHalfToFloat(target, rawOut.data(), &hOut[0], DIM_SIZE);
  poplar::copyDeviceHalfToFloat(target, rawOutConstTest.data(),
                                &hOutConstTest[0], DIM_SIZE);
  poplar::copyDeviceHalfToFloat(target, rawOutFails.data(), &hOutFails[0],
                                DIM_SIZE);
  poplar::copyDeviceHalfToFloat(target, rawOutConstTestFails.data(),
                                &hOutConstTestFails[0], DIM_SIZE);

  // Check result
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    double res = hIn1[i] + k * hIn2[i];
    BOOST_TEST(hOut[i] == res);
    BOOST_TEST(hOutConstTest[i] == res);
    // These tests should "fail", producing hIn1 exactly.
    std::cout << "hOut:" << hOut[i] << " hOutConstTest:" << hOutConstTest[i]
              << " hOutFails:" << hOutFails[i]
              << " hOutConstTestFails:" << hOutConstTestFails[i] << "\n";
    // Avoid testing this properly in IPUModel, as half isn't accurate
    if (!isIpuModel(TEST_TARGET)) {
      BOOST_TEST(static_cast<bool>(hOutFails[i] == hIn1[i]));
      BOOST_TEST(static_cast<bool>(hOutConstTestFails[i] == hIn1[i]));
    }
  }
}

BOOST_AUTO_TEST_CASE(
    StdAddTo_float_constant,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  float hIn1[DIM_SIZE][DIM_SIZE], hIn2[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn1, hIn2);

  float k = 2;
  Tensor in1, in2;
  std::tie(in1, in2) = mapBinaryOpTensors(graph, FLOAT);

  graph.createHostWrite("in1", in1);
  graph.createHostWrite("in2", in2);
  graph.createHostRead("out", in1);
  auto prog = Sequence();
  scaledAddTo(graph, in1, in2, k, prog);
  Engine eng(graph, prog);
  float hOut[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);
    eng.writeTensor("in1", hIn1, &hIn1[DIM_SIZE]);
    eng.writeTensor("in2", hIn2, &hIn2[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", hOut, &hOut[DIM_SIZE]);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] + k * hIn2[i][j];
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

static std::size_t getMaxMemoryElementBytes(const Target &target) {
  std::size_t max = 0;
  const auto &elementOffsets = target.getMemoryElementOffsets();
  std::size_t lastOffset = target.getBytesPerTile();
  for (auto it = elementOffsets.rbegin(); it != elementOffsets.rend(); ++it) {
    max = std::max(max, lastOffset - *it);
    lastOffset = *it;
  }
  return max;
}

BOOST_AUTO_TEST_CASE(
    StdAddTo_float_runtime_fast_path,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  float hIn1[DIM_SIZE][DIM_SIZE], hIn2[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn1, hIn2);

  float k = 2;
  // Creating a larger tensor to force a gap between allocations of the two
  // operands will result in the fast path being chosen at runtime.
  const unsigned padSize = 16 + getMaxMemoryElementBytes(graph.getTarget()) / 4;
  const unsigned regionSize = 2 * DIM_SIZE * DIM_SIZE + padSize;
  auto in = graph.addVariable(FLOAT, {regionSize}, "Whole input");
  graph.setTileMapping(in, 0);

  auto in1 = in.slice(0, DIM_SIZE * DIM_SIZE);
  auto in2 = in.slice(padSize, padSize + DIM_SIZE * DIM_SIZE);

  graph.createHostWrite("in1", in1);
  graph.createHostWrite("in2", in2);
  graph.createHostRead("out", in1);
  auto prog = Sequence();
  scaledAddTo(graph, in1, in2, k, prog);
  Engine eng(graph, prog);
  float hOut[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);
    eng.writeTensor("in1", hIn1, &hIn1[DIM_SIZE]);
    eng.writeTensor("in2", hIn2, &hIn2[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", hOut, &hOut[DIM_SIZE]);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] + k * hIn2[i][j];
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(
    StdAddTo_float_tensor,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  float hIn1[DIM_SIZE][DIM_SIZE], hIn2[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn1, hIn2);

  float k = 2;
  auto factor = graph.addVariable(FLOAT, {});
  graph.setTileMapping(factor, 0);
  graph.setInitialValue(factor, k);
  Tensor in1, in2;
  std::tie(in1, in2) = mapBinaryOpTensors(graph, FLOAT);

  graph.createHostWrite("in1", in1);
  graph.createHostWrite("in2", in2);
  graph.createHostRead("out", in1);
  auto prog = Sequence();
  scaledAddTo(graph, in1, in2, factor, prog);
  Engine eng(graph, prog);
  float hOut[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);
    eng.writeTensor("in1", hIn1, &hIn1[DIM_SIZE]);
    eng.writeTensor("in2", hIn2, &hIn2[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", hOut, &hOut[DIM_SIZE]);
  });

  // Check result
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] + k * hIn2[i][j];
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(
    StdSubtractFrom_float_tensor,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  float hIn1[DIM_SIZE][DIM_SIZE], hIn2[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn1, hIn2);

  float k = 2;
  auto factor = graph.addVariable(FLOAT, {});
  graph.setTileMapping(factor, 0);
  graph.setInitialValue(factor, k);
  Tensor in1, in2;
  std::tie(in1, in2) = mapBinaryOpTensors(graph, FLOAT);

  graph.createHostWrite("in1", in1);
  graph.createHostWrite("in2", in2);
  graph.createHostRead("out", in1);
  auto prog = Sequence();
  scaledSubtractFrom(graph, in1, in2, factor, prog);
  Engine eng(graph, prog);
  float hOut[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);
    eng.writeTensor("in1", hIn1, &hIn1[DIM_SIZE]);
    eng.writeTensor("in2", hIn2, &hIn2[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", hOut, &hOut[DIM_SIZE]);
  });

  // Check result
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] - k * hIn2[i][j];
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(
    StdSubFrom_int,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  int hIn1[DIM_SIZE][DIM_SIZE], hIn2[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn1, hIn2);

  int k = 2;
  Tensor in1, in2;
  std::tie(in1, in2) = mapBinaryOpTensors(graph, INT);

  graph.createHostWrite("in1", in1);
  graph.createHostWrite("in2", in2);
  graph.createHostRead("out", in1);
  auto prog = Sequence();
  scaledSubtractFrom(graph, in1, in2, k, prog);
  Engine eng(graph, prog);
  int hOut[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);
    eng.writeTensor("in1", hIn1, &hIn1[DIM_SIZE]);
    eng.writeTensor("in2", hIn2, &hIn2[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", hOut, &hOut[DIM_SIZE]);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] - k * hIn2[i][j];
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

// Test for "aX + bY", via 'scaledAddTo()'. A few different sub-tests are run.
//    X   can be    HALF or FLOAT
//    Y   is always HALF
//   a,b  can be    HALF or FLOAT tensors, or constants
BOOST_AUTO_TEST_CASE(
    StdaXPlusbY_halfin_tensor_and_const,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(1))) {
  auto device = createTestDevice(TEST_TARGET);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  // We do a bunch of sub test cases (test variants) that do mostly the same
  // stuff; we put in this class the common parts.
  struct Variant {
    std::string name; // name of the test
    Type dataXType;   // Device type of the X data
    float a, b;       // Scale values
    poplar::OptionFlags opts;

    Variant(std::string name, Type dataXType, float a, float b,
            OptionFlags opts)
        : name(name), dataXType(dataXType), a(a), b(b), opts(opts) {}
    virtual ~Variant() {}

    // data tensors and associated buffers
    Tensor X, Y;
    std::vector<char> rawX;
    boost::multi_array<float, 2> hX{boost::extents[DIM_SIZE][DIM_SIZE]};
    std::vector<char> rawY;
    boost::multi_array<float, 2> hY{boost::extents[DIM_SIZE][DIM_SIZE]};

    // Setup everything before bind/load/run
    void setup(Graph &graph, Target &target, Sequence &prog) {
      X = graph.addVariable(dataXType, {DIM_SIZE, DIM_SIZE}, name + "X");
      Y = graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, name + "Y");
      mapTensorLinearly(graph, X);
      mapTensorLinearly(graph, Y);
      graph.createHostWrite(name + "X", X);
      graph.createHostRead(name + "Xout", X);
      graph.createHostWrite(name + "Y", Y);
      callScaledAdd(graph, prog);
    };

    // This will call the 'scaledAddTo()'. Different for Tensors and Const
    virtual void callScaledAdd(Graph &graph, Sequence &prog) = 0;

    // Setup the host buffers and write the data Tensors to the device
    void write(Target &target, Engine &eng, float Xvalues[DIM_SIZE][DIM_SIZE],
               float Yvalues[DIM_SIZE][DIM_SIZE]) {
      auto floatBufSize = sizeof(float) * DIM_SIZE * DIM_SIZE;
      auto rawBufSize = target.getTypeSize(HALF) * DIM_SIZE * DIM_SIZE;

      std::memcpy(&hX[0][0], Xvalues, floatBufSize);
      if (dataXType == HALF) {
        rawX.resize(rawBufSize);
        poplar::copyFloatToDeviceHalf(target, &hX[0][0], rawX.data(),
                                      DIM_SIZE * DIM_SIZE);
        eng.writeTensor(name + "X", rawX.data(), rawX.data() + rawX.size());
      } else {
        float *data = &hX[0][0];
        eng.writeTensor(name + "X", data, data + floatBufSize);
      }

      std::memcpy(&hY[0][0], Yvalues, floatBufSize);
      rawY.resize(rawBufSize);
      poplar::copyFloatToDeviceHalf(target, &hY[0][0], rawY.data(),
                                    DIM_SIZE * DIM_SIZE);
      eng.writeTensor(name + "Y", rawY.data(), rawY.data() + rawY.size());
    };

    // Read the result Tensor from the device and check results
    void readAndVerify(Target &target, Engine &eng) {
      boost::multi_array<float, 2> hXout{boost::extents[DIM_SIZE][DIM_SIZE]};
      if (dataXType == HALF) {
        eng.readTensor(name + "Xout", rawX.data(), rawX.data() + rawX.size());
        poplar::copyDeviceHalfToFloat(target, rawX.data(), &hXout[0][0],
                                      DIM_SIZE * DIM_SIZE);
      } else {
        float *data = &hXout[0][0];
        auto floatBufSize = sizeof(float) * DIM_SIZE * DIM_SIZE;
        eng.readTensor(name + "Xout", data, data + floatBufSize);
      }

      // Is the result from the device within expected accuracy?
      for (auto i = 0U; i < DIM_SIZE; ++i) {
        for (auto j = 0U; j < DIM_SIZE; ++j) {
          auto expected = a * hX[i][j] + b * hY[i][j];
          auto computed = hXout[i][j];
          BOOST_TEST(expected == computed, name << ": [" << i << "][" << j
                                                << "] - expected:" << expected
                                                << ", computed:" << computed);
        }
      }
    }
  };

  // Some test variants have the scale values as constants. We just call
  // scaledAddTo with the parameters.
  struct VariantConst : Variant {
    VariantConst(std::string name, Type dataXType, float a, float b,
                 OptionFlags opts)
        : Variant(name + "Const", dataXType, a, b, opts) {}
    void callScaledAdd(Graph &graph, Sequence &prog) {
      scaledAddTo(graph, X, a, Y, b, prog, name, opts);
    }
  };

  // Some test variants have the scale values 'a', 'b' as tensors, so we create
  // 'A' and 'B' tensors for them.
  struct VariantTens : Variant {
    Type scaleType;
    VariantTens(std::string name, Type dataXType, Type scaleType, float a,
                float b, OptionFlags opts)
        : Variant(name + "Tensor", dataXType, a, b, opts),
          scaleType(scaleType) {}
    void callScaledAdd(Graph &graph, Sequence &prog) {
      auto A = graph.addVariable(scaleType, {});
      mapTensorLinearly(graph, A);
      graph.setInitialValue(A, a);
      auto B = graph.addVariable(scaleType, {});
      mapTensorLinearly(graph, B);
      graph.setInitialValue(B, b);
      scaledAddTo(graph, X, A, Y, B, prog, name, opts);
    }
  };

  // Input data values (as single) is the same for all variants
  float Xdata[DIM_SIZE][DIM_SIZE]; // a.k.a "A" data tensor
  float Ydata[DIM_SIZE][DIM_SIZE]; // a.k.a "B" data tensor
  setBinaryOpInputs(Ydata, Xdata);

  // ============ All the variants ===============

  // Value for 'bSmall' is chosen so that it doesn't have enough accuracy to fit
  // in a HALF float (with default 'ScaledAddOptions::floatToHalfTolerance').
  // When the 'half, float, Tensor' vertex is created, this will make sure the
  // real 'mixed' path is chosen at runtime.
  float a = 2, b = 3, bSmall = 0.0007;

  std::vector<Variant *> variants = {
      // clang-format off
    new VariantTens{"half half", HALF, HALF, a, bSmall,
                    {{"optimizeForSpeed", "true"}}},
    new VariantConst{"half half", HALF, -a, -b,
                     {{"optimizeForSpeed", "true"}}},
    new VariantTens{"float half", FLOAT, HALF, a, b,
                    {{"optimizeForSpeed", "true"}}},
    new VariantConst{"float half", FLOAT, -a, -b,
                     {{"optimizeForSpeed", "true"}}},

    // Test the "mixed" tensor vertex (data = HALF, scales = FLOAT) with a
    // "normal" and a "small" 'b', so that we verify that both paths are taken
    // at runtime.
    new VariantTens{"half float", HALF, FLOAT, a, b,
                    {{"optimizeForSpeed", "true"}}},
    new VariantTens{"half float bSmall", HALF, FLOAT, a, bSmall,
                    {{"optimizeForSpeed", "true"}}},
    // With a "small" 'b', the "mixed" tensor vertex (data = HALF,
    // scales = FLOAT) will be chosen directly by 'scaledAddTo'
    new VariantConst{"half float", HALF, a, bSmall,
                    {{"optimizeForSpeed", "true"}}},
      // clang-format on
  };

  // ------ Run all the variants contained in Variants[] ------
  auto prog = Sequence();
  for (auto test : variants)
    test->setup(graph, target, prog);
  Engine eng(graph, prog);
  device.bind([&](const Device &d) {
    eng.load(d);
    for (auto test : variants)
      test->write(target, eng, Xdata, Ydata);
    eng.run();
    for (auto test : variants)
      test->readAndVerify(target, eng);
  });
}

BOOST_AUTO_TEST_CASE(
    StdXMinusaXPlusbY_halfin_tensor_and_const,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(1))) {
  auto device = createTestDevice(TEST_TARGET);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  float hInOut[DIM_SIZE][DIM_SIZE];
  float hIn[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn, hInOut);

  auto rawBufSize = target.getTypeSize(HALF) * DIM_SIZE * DIM_SIZE;
  std::vector<char> rawIn(rawBufSize);
  poplar::copyFloatToDeviceHalf(target, &hIn[0][0], rawIn.data(),
                                DIM_SIZE * DIM_SIZE);
  std::vector<char> rawInOut(rawBufSize);
  poplar::copyFloatToDeviceHalf(target, &hInOut[0][0], rawInOut.data(),
                                DIM_SIZE * DIM_SIZE);

  float k = 2, k2 = 3;
  auto A = graph.addVariable(HALF, {});
  graph.setInitialValue(A, k);
  auto B = graph.addVariable(HALF, {});
  graph.setInitialValue(B, k2);
  auto inOut = graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "inOut");
  auto inOutConstTest =
      graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "inOutConstTest");
  auto in = graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "in");
  mapTensorLinearly(graph, A);
  mapTensorLinearly(graph, B);
  mapTensorLinearly(graph, inOut);
  mapTensorLinearly(graph, inOutConstTest);
  mapTensorLinearly(graph, in);

  std::vector<char> rawOut(rawBufSize);
  std::vector<char> rawOutConstTest(rawBufSize);
  graph.createHostWrite("in", in);
  graph.createHostWrite("inOut", inOut);
  graph.createHostRead("out", inOut);
  graph.createHostRead("outConstTest", inOutConstTest);

  auto prog = Sequence();

  prog.add(Copy(inOut, inOutConstTest));
  scaledAddTo(graph, inOut, A, in, B, prog,
              popops::ScaledAddSpecialisation::X_MINUS_AX_PLUS_BY,
              "Debug - optimized", {{"optimizeForSpeed", "true"}});
  scaledAddTo(graph, inOutConstTest, -k, in, -k2, prog,
              popops::ScaledAddSpecialisation::X_MINUS_AX_PLUS_BY,
              "Debug - optimized", {{"optimizeForSpeed", "true"}});
  Engine eng(graph, prog);
  device.bind([&](const Device &d) {
    eng.load(d);

    eng.writeTensor("in", rawIn.data(), rawIn.data() + rawIn.size());
    eng.writeTensor("inOut", rawInOut.data(),
                    rawInOut.data() + rawInOut.size());
    eng.run();
    eng.readTensor("out", rawOut.data(), rawOut.data() + rawOut.size());
    eng.readTensor("outConstTest", rawOutConstTest.data(),
                   rawOutConstTest.data() + rawOutConstTest.size());
  });

  float hOut[DIM_SIZE][DIM_SIZE];
  poplar::copyDeviceHalfToFloat(target, rawOut.data(), &hOut[0][0],
                                DIM_SIZE * DIM_SIZE);
  float hOutConstTest[DIM_SIZE][DIM_SIZE];
  poplar::copyDeviceHalfToFloat(target, rawOutConstTest.data(),
                                &hOutConstTest[0][0], DIM_SIZE * DIM_SIZE);

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = (1 - k) * hInOut[i][j] + k2 * hIn[i][j];
      BOOST_TEST(hOut[i][j] == res, "Tensor scale test");
      double resConst = (1 + k) * hInOut[i][j] - k2 * hIn[i][j];
      BOOST_TEST(hOutConstTest[i][j] == resConst, "Constant scale test");
    }
  }
}

BOOST_AUTO_TEST_CASE(
    StdaXPlusbY_float_tensor_and_const,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(1))) {
  auto device = createTestDevice(TEST_TARGET);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  float hInOut[DIM_SIZE][DIM_SIZE];
  float hIn[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn, hInOut);

  float k = 2, k2 = 3;
  auto A = graph.addVariable(FLOAT, {});
  graph.setInitialValue(A, k);
  auto B = graph.addVariable(FLOAT, {});
  graph.setInitialValue(B, k2);

  auto inOut = graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "inOut");
  auto inOutConstTest =
      graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "inOutConstTest");
  auto in = graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "in");

  mapTensorLinearly(graph, A);
  mapTensorLinearly(graph, B);
  mapTensorLinearly(graph, inOut);
  mapTensorLinearly(graph, inOutConstTest);
  mapTensorLinearly(graph, in);

  graph.createHostWrite("in", in);
  graph.createHostWrite("inOut", inOut);
  graph.createHostRead("out", inOut);
  graph.createHostRead("outConstTest", inOutConstTest);

  auto prog = Sequence();
  prog.add(Copy(inOut, inOutConstTest));

  scaledAddTo(graph, inOut, A, in, B, prog, "Debug - optimized",
              {{"optimizeForSpeed", "true"}});
  scaledAddTo(graph, inOutConstTest, -k, in, -k2, prog, "Debug - optimized",
              {{"optimizeForSpeed", "true"}});

  Engine eng(graph, prog);

  float hResult[DIM_SIZE][DIM_SIZE];
  float hResultConst[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);

    eng.writeTensor("in", hIn, &hIn[DIM_SIZE]);
    eng.writeTensor("inOut", hInOut, &hInOut[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", hResult, &hResult[DIM_SIZE]);
    eng.readTensor("outConstTest", hResultConst, &hResultConst[DIM_SIZE]);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = k * hInOut[i][j] + k2 * hIn[i][j];
      BOOST_TEST(hResult[i][j] == res, "Tensor scale test");
      BOOST_TEST(hResultConst[i][j] == -res, "Constant scale test");
    }
  }
}

BOOST_AUTO_TEST_CASE(StdCast) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  float hIn[DIM_SIZE];
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    hIn[i] = (float)i;
  }

  auto in = graph.addVariable(FLOAT, {DIM_SIZE}, "in");
  mapTensorLinearly(graph, in);
  graph.createHostWrite("in", in);

  auto prog = Sequence();

  poplar::Tensor out = cast(graph, in, INT, prog, "cast");
  graph.createHostRead("out", out);

  int hOut[DIM_SIZE];

  Engine eng(graph, prog);
  device.bind([&](const Device &d) {
    eng.load(d);
    eng.writeTensor("in", hIn, &hIn[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", hOut, &hOut[DIM_SIZE]);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    BOOST_TEST(hOut[i] == i);
  }
}

BOOST_AUTO_TEST_CASE(
    StdaXMinusbY_float_tensor_and_const,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(1))) {
  auto device = createTestDevice(TEST_TARGET);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  float hInOut[DIM_SIZE][DIM_SIZE];
  float hIn[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn, hInOut);

  float k = 2, k2 = 3;
  auto A = graph.addVariable(FLOAT, {});
  graph.setInitialValue(A, k);
  auto B = graph.addVariable(FLOAT, {});
  graph.setInitialValue(B, k2);

  auto inOut = graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "inOut");
  auto inOutConstTest =
      graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "inOutConstTest");
  auto in = graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "in");

  mapTensorLinearly(graph, A);
  mapTensorLinearly(graph, B);
  mapTensorLinearly(graph, inOut);
  mapTensorLinearly(graph, inOutConstTest);
  mapTensorLinearly(graph, in);

  graph.createHostWrite("in", in);
  graph.createHostWrite("inOut", inOut);
  graph.createHostRead("out", inOut);
  graph.createHostRead("outConstTest", inOutConstTest);

  auto prog = Sequence();
  prog.add(Copy(inOut, inOutConstTest));

  scaledSubtractFrom(graph, inOut, A, in, B, prog, "Debug - optimized",
                     {{"optimizeForSpeed", "true"}});
  scaledSubtractFrom(graph, inOutConstTest, k, in, k2, prog,
                     "Debug - optimized", {{"optimizeForSpeed", "true"}});

  Engine eng(graph, prog);

  float hResult[DIM_SIZE][DIM_SIZE];
  float hResultConst[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);

    eng.writeTensor("in", hIn, &hIn[DIM_SIZE]);
    eng.writeTensor("inOut", hInOut, &hInOut[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", hResult, &hResult[DIM_SIZE]);
    eng.readTensor("outConstTest", hResultConst, &hResultConst[DIM_SIZE]);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = k * hInOut[i][j] - k2 * hIn[i][j];
      BOOST_TEST(hResult[i][j] == res, "Tensor scale test");
      BOOST_TEST(hResultConst[i][j] == res, "Constant scale test");
    }
  }
}

BOOST_AUTO_TEST_CASE(
    checkAccuracyFloatHalf,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(0.1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(0.1))) {
  auto device = createTestDevice(TEST_TARGET);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);
  // Values chosen because:
  // 1.0 - simple, exact, expect 1
  // 65500 - in range, expect 1
  // 80000 - Not in range of half, expect 0
  // 3e-7 - Uses denorms so expect 0, but on IPUModel we get 1 as it is not
  //        doing half precision correctly
  // (1.0f/32768.0f) - Precise as, although denorm it is a power of 2
  float hIn[DIM_SIZE] = {1.0,  65500,  80000,  3e-7,  (1.0f / 32768.0f),
                         -1.0, -65500, -80000, -3e-7, (-1.0f / 32768.0f)};
  double tolerance[DIM_SIZE] = {1e-6, 1e-4, 1e-6, 1e-6, 1e-6,
                                1e-6, 1e-4, 1e-6, 1e-6, 1e-6};
  auto input = graph.addVariable(FLOAT, {DIM_SIZE}, "in");
  graph.setTileMapping(input, 0);

  auto prog = Sequence();
  // Some casts can cause exceptions, if the float is unrepresentable as a half.
  // The codelet should disable exceptions.  Setting them on here means that
  // we are checking that it does so.
  FloatingPointBehaviour behaviour;
  setFloatingPointBehaviour(graph, prog, behaviour, "Set Exceptions");

  auto castResult =
      checkAccuracyWhenCast(graph, input[0], HALF, tolerance[0], prog);
  auto isAccurate = castResult.reshape({1});
  for (unsigned i = 1; i < DIM_SIZE; i++) {
    auto castResult =
        checkAccuracyWhenCast(graph, input[i], HALF, tolerance[i], prog);
    isAccurate = concat(isAccurate, castResult.reshape({1}));
  }
  graph.createHostWrite("input", input);
  graph.createHostRead("isAccurate", isAccurate);

  bool hIsAccurate[DIM_SIZE];
  Engine eng(graph, prog);
  device.bind([&](const Device &d) {
    eng.load(d);
    eng.writeTensor("input", hIn, &hIn[DIM_SIZE]);
    eng.run();
    eng.readTensor("isAccurate", hIsAccurate, &hIsAccurate[DIM_SIZE]);
  });

  bool expected[DIM_SIZE] = {1, 1, 0, 0, 1, 1, 1, 0, 0, 1};
  bool expectedIpuModel[DIM_SIZE] = {1, 1, 0, 1, 1, 1, 1, 0, 1, 1};
  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    BOOST_TEST(hIsAccurate[i] ==
               (isIpuModel(TEST_TARGET) ? expectedIpuModel[i] : expected[i]));
  }
}

void checkVarianceConvertImpl(float hInVariance[DIM_SIZE],
                              float hInInvStdDev[DIM_SIZE], bool doCast,
                              bool force2D, bool useConstEpsilon) {
  auto device = createTestDevice(TEST_TARGET, 1, 4);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  const float epsilon = 0.001f;

  auto epsilonF = graph.addVariable(FLOAT, {});
  graph.setInitialValue(epsilonF, epsilon);

  auto epsilonH = graph.addVariable(HALF, {});
  graph.setInitialValue(epsilonH, epsilon);

  auto varianceIn = graph.addVariable(FLOAT, {DIM_SIZE}, "varIn");
  auto invStdDevIn = graph.addVariable(HALF, {DIM_SIZE}, "isdIn");

  mapTensorLinearly(graph, varianceIn);
  mapTensorLinearly(graph, invStdDevIn);
  if (force2D) {
    graph.setTileMapping(varianceIn.slice({1, 4}), 1);
    graph.setTileMapping(invStdDevIn.slice({7, 8}), 1);
  }
  mapTensorLinearly(graph, epsilonF);
  mapTensorLinearly(graph, epsilonH);

  graph.createHostWrite("varianceIn", varianceIn);
  graph.createHostWrite("invStdDevIn", invStdDevIn);

  auto prog = Sequence();
  auto invStdDevOut = useConstEpsilon
                          ? varianceToInvStdDev(graph, varianceIn, epsilon,
                                                prog, doCast ? HALF : FLOAT)
                          : varianceToInvStdDev(graph, varianceIn, epsilonF,
                                                prog, doCast ? HALF : FLOAT);
  auto varianceOut = useConstEpsilon
                         ? invStdDevToVariance(graph, invStdDevIn, epsilon,
                                               prog, doCast ? FLOAT : HALF)
                         : invStdDevToVariance(graph, invStdDevIn, epsilonH,
                                               prog, doCast ? FLOAT : HALF);

  graph.createHostRead("invStdDevOut", invStdDevOut);
  graph.createHostRead("varianceOut", varianceOut);

  auto rawBufSize = target.getTypeSize(HALF) * DIM_SIZE;
  std::vector<char> rawIn(rawBufSize);
  std::vector<char> rawOut(rawBufSize);
  poplar::copyFloatToDeviceHalf(target, &hInInvStdDev[0], rawIn.data(),
                                DIM_SIZE);

  float hInvStdDevOut[DIM_SIZE];
  float hVarianceOut[DIM_SIZE];

  Engine eng(graph, prog);
  device.bind([&](const Device &d) {
    eng.load(d);

    eng.writeTensor("varianceIn", hInVariance, &hInVariance[DIM_SIZE]);
    eng.writeTensor("invStdDevIn", rawIn.data(), rawIn.data() + rawIn.size());
    eng.run();
    if (doCast) {
      eng.readTensor("invStdDevOut", rawOut.data(),
                     rawOut.data() + rawOut.size());
      eng.readTensor("varianceOut", hVarianceOut, &hVarianceOut[DIM_SIZE]);
    } else {
      eng.readTensor("varianceOut", rawOut.data(),
                     rawOut.data() + rawOut.size());
      eng.readTensor("invStdDevOut", hInvStdDevOut, &hInvStdDevOut[DIM_SIZE]);
    }
  });
  if (doCast) {
    poplar::copyDeviceHalfToFloat(target, rawOut.data(), &hInvStdDevOut[0],
                                  DIM_SIZE);
  } else {
    poplar::copyDeviceHalfToFloat(target, rawOut.data(), &hVarianceOut[0],
                                  DIM_SIZE);
  }

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    double resInvStdDev = 1 / std::sqrt(hInVariance[i] + epsilon);
    double resVariance = (1 / (hInInvStdDev[i] * hInInvStdDev[i])) - epsilon;
    BOOST_TEST(hInvStdDevOut[i] == resInvStdDev, "varianceToInvStdDev test");
    BOOST_TEST(hVarianceOut[i] == resVariance, "invStdDevToVariance test");
  }
}

BOOST_AUTO_TEST_CASE(
    checkVarianceConversionWithCast,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(1))) {

  float hInVariance[DIM_SIZE], hInInvStdDev[DIM_SIZE];
  for (unsigned i = 0; i < DIM_SIZE; i++) {
    hInVariance[i] = 500 * i;
    hInInvStdDev[i] = 0.001 * (i + 1);
  }
  checkVarianceConvertImpl(hInVariance, hInInvStdDev, true, false, false);
}

BOOST_AUTO_TEST_CASE(
    checkVarianceConversionWithCast2D,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(1))) {

  float hInVariance[DIM_SIZE], hInInvStdDev[DIM_SIZE];
  for (unsigned i = 0; i < DIM_SIZE; i++) {
    hInVariance[i] = 500 * i;
    hInInvStdDev[i] = 0.001 * (i + 1);
  }
  checkVarianceConvertImpl(hInVariance, hInInvStdDev, true, true, true);
}

BOOST_AUTO_TEST_CASE(
    checkVarianceConversionWithoutCast,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(1))) {
  auto device = createTestDevice(TEST_TARGET);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  float hInVariance[DIM_SIZE], hInInvStdDev[DIM_SIZE];
  for (unsigned i = 0; i < DIM_SIZE; i++) {
    hInVariance[i] = 10 * i;
    hInInvStdDev[i] = 10 * (i + 1);
  }
  checkVarianceConvertImpl(hInVariance, hInInvStdDev, false, false, true);
}

BOOST_AUTO_TEST_CASE(
    StdaXMinusbY_halfin_tensor_and_const,
    *utf::tolerance<float>(fpc::percent_tolerance<float>(1)) *
        utf::tolerance<double>(fpc::percent_tolerance<double>(1))) {
  auto device = createTestDevice(TEST_TARGET);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  float hInOut[DIM_SIZE][DIM_SIZE];
  float hIn[DIM_SIZE][DIM_SIZE];
  float hInOutFloat[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn, hInOut);
  std::memcpy(hInOutFloat, hInOut, sizeof(hInOutFloat));

  auto rawBufSize = target.getTypeSize(HALF) * DIM_SIZE * DIM_SIZE;
  std::vector<char> rawIn(rawBufSize);
  poplar::copyFloatToDeviceHalf(target, &hIn[0][0], rawIn.data(),
                                DIM_SIZE * DIM_SIZE);
  std::vector<char> rawInOut(rawBufSize);
  poplar::copyFloatToDeviceHalf(target, &hInOut[0][0], rawInOut.data(),
                                DIM_SIZE * DIM_SIZE);

  float k = 2, k2 = 3;
  auto A = graph.addVariable(HALF, {});
  graph.setInitialValue(A, k);
  auto B = graph.addVariable(HALF, {});
  graph.setInitialValue(B, k2);
  auto inOut = graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "inOut");
  auto inOutFloat =
      graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "inOutFloat");
  auto inOutConstTest =
      graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "inOutConstTest");
  auto inOutFloatConstTest =
      graph.addVariable(FLOAT, {DIM_SIZE, DIM_SIZE}, "inOutFloatConstTest");
  auto in = graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "in");
  mapTensorLinearly(graph, A);
  mapTensorLinearly(graph, B);
  mapTensorLinearly(graph, inOut);
  mapTensorLinearly(graph, inOutFloat);
  mapTensorLinearly(graph, inOutConstTest);
  mapTensorLinearly(graph, inOutFloatConstTest);
  mapTensorLinearly(graph, in);

  std::vector<char> rawOut(rawBufSize);
  std::vector<char> rawOutConstTest(rawBufSize);
  graph.createHostWrite("in", in);
  graph.createHostWrite("inOut", inOut);
  graph.createHostWrite("inOutFloat", inOutFloat);
  graph.createHostRead("out", inOut);
  graph.createHostRead("outFloat", inOutFloat);
  graph.createHostRead("outConstTest", inOutConstTest);
  graph.createHostRead("outFloatConstTest", inOutFloatConstTest);

  auto prog = Sequence();

  prog.add(Copy(inOut, inOutConstTest));
  prog.add(Copy(inOutFloat, inOutFloatConstTest));
  scaledSubtractFrom(graph, inOut, A, in, B, prog, "Debug - optimized",
                     {{"optimizeForSpeed", "true"}});
  scaledSubtractFrom(graph, inOutConstTest, -k, in, -k2, prog,
                     "Debug - optimized", {{"optimizeForSpeed", "true"}});
  scaledSubtractFrom(graph, inOutFloat, A, in, B, prog,
                     "Float out Debug - optimized",
                     {{"optimizeForSpeed", "true"}});
  scaledSubtractFrom(graph, inOutFloatConstTest, -k, in, -k2, prog,
                     "Float out Debug - optimized",
                     {{"optimizeForSpeed", "true"}});

  Engine eng(graph, prog);

  float hOutFloat[DIM_SIZE][DIM_SIZE];
  float hOutFloatConst[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);

    eng.writeTensor("in", rawIn.data(), rawIn.data() + rawIn.size());
    eng.writeTensor("inOut", rawInOut.data(),
                    rawInOut.data() + rawInOut.size());
    eng.writeTensor("inOutFloat", hInOutFloat, &hInOutFloat[DIM_SIZE]);
    eng.run();
    eng.readTensor("out", rawOut.data(), rawOut.data() + rawOut.size());
    eng.readTensor("outConstTest", rawOutConstTest.data(),
                   rawOutConstTest.data() + rawOutConstTest.size());
    eng.readTensor("outFloat", hOutFloat, &hOutFloat[DIM_SIZE]);
    eng.readTensor("outFloatConstTest", hOutFloatConst,
                   &hOutFloatConst[DIM_SIZE]);
  });

  float hOut[DIM_SIZE][DIM_SIZE];
  poplar::copyDeviceHalfToFloat(target, rawOut.data(), &hOut[0][0],
                                DIM_SIZE * DIM_SIZE);
  float hOutConstTest[DIM_SIZE][DIM_SIZE];
  poplar::copyDeviceHalfToFloat(target, rawOutConstTest.data(),
                                &hOutConstTest[0][0], DIM_SIZE * DIM_SIZE);

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = k * hInOut[i][j] - k2 * hIn[i][j];
      double resFloat = k * hInOutFloat[i][j] - k2 * hIn[i][j];
      BOOST_TEST(hOut[i][j] == res, "Tensor scale test");
      BOOST_TEST(hOutConstTest[i][j] == -res, "Constant scale test");
      BOOST_TEST(hOutFloat[i][j] == resFloat, "Tensor scale float out test");
      BOOST_TEST(hOutFloatConst[i][j] == -res, "Constant float out scale test");
    }
  }
}

// Test fixture with common graph setup for testing aX - bY where
// X and Y are half precision tensors.
struct HalfTensorAXBYTestFixture {
  static std::vector<poplar::OptionFlags> OptionFlags() {
    return {{{"optimizeForSpeed", "false"}}, {{"optimizeForSpeed", "true"}}};
  }

  HalfTensorAXBYTestFixture()
      : device_(createTestDevice(TEST_TARGET)), target_(device_.getTarget()),
        graph_(target_),
        rawBufSize_(target_.getTypeSize(HALF) * DIM_SIZE * DIM_SIZE),
        rawIn_(rawBufSize_), rawInOut_(rawBufSize_), rawOut_(rawBufSize_) {
    popops::addCodelets(graph_);

    setBinaryOpInputs(hIn_, hInOut_);

    poplar::copyFloatToDeviceHalf(target_, &hIn_[0][0], rawIn_.data(),
                                  DIM_SIZE * DIM_SIZE);
    poplar::copyFloatToDeviceHalf(target_, &hInOut_[0][0], rawInOut_.data(),
                                  DIM_SIZE * DIM_SIZE);

    inOut_ = graph_.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "inOut");
    in_ = graph_.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "in");
    mapTensorLinearly(graph_, inOut_);
    mapTensorLinearly(graph_, in_);

    graph_.createHostWrite("in", in_);
    graph_.createHostWrite("inOut", inOut_);
    graph_.createHostRead("out", inOut_);
  }

  pva::Report runProgram(const poplar::program::Program &program) {
    Engine engine(graph_, program);
    engine.enableExecutionProfiling();

    device_.bind([&](const Device &d) {
      engine.load(d);

      engine.writeTensor("in", rawIn_.data(), rawIn_.data() + rawIn_.size());
      engine.writeTensor("inOut", rawInOut_.data(),
                         rawInOut_.data() + rawInOut_.size());

      engine.run();

      engine.readTensor("out", rawOut_.data(), rawOut_.data() + rawOut_.size());
    });

    poplar::copyDeviceHalfToFloat(target_, rawOut_.data(), &hOut_[0][0],
                                  DIM_SIZE * DIM_SIZE);

    return engine.getReport();
  }

  void CHECK_OUTPUT_IS_AXMINUSBY(float a, float b) {
    for (auto i = 0U; i < DIM_SIZE; ++i) {
      for (auto j = 0U; j < DIM_SIZE; ++j) {
        double res = a * hInOut_[i][j] - b * hIn_[i][j];
        BOOST_TEST(hOut_[i][j] == res, boost::test_tools::tolerance(1.0));
      }
    }
  }

  void CHECK_OUTPUT_IS_XMINUSBY(float b) { CHECK_OUTPUT_IS_AXMINUSBY(1, b); }

  void CHECK_WAS_MIXED_PRECISION(const pva::Report &report) {
    for (const auto &cs : report.compilation().computeSets()) {
      for (const auto &vertexInstance : cs.vertices()) {
        const auto &vertex = vertexInstance.type().name();
        const auto isCast = vertex.rfind("popops::Cast", 0) == 0;
        BOOST_TEST(!isCast, "Profile contained cast: " << vertex);
      }
    }
  }

  float hInOut_[DIM_SIZE][DIM_SIZE];
  float hIn_[DIM_SIZE][DIM_SIZE];
  float hOut_[DIM_SIZE][DIM_SIZE];

  poplibs_support::TestDevice device_;
  poplar::Target target_;
  poplar::Graph graph_;

  unsigned long rawBufSize_;
  std::vector<char> rawIn_;
  std::vector<char> rawInOut_;
  std::vector<char> rawOut_;

  poplar::Tensor inOut_;
  poplar::Tensor in_;
};

BOOST_DATA_TEST_CASE_F(HalfTensorAXBYTestFixture,
                       StdAXMinusBy_ScaleByFloatTensor,
                       HalfTensorAXBYTestFixture::OptionFlags(), optionFlag) {
  const float a = 2, b = 3;

  auto A = graph_.addVariable(FLOAT, {});
  auto B = graph_.addVariable(FLOAT, {});
  graph_.setInitialValue(A, a);
  graph_.setInitialValue(B, b);
  mapTensorLinearly(graph_, A);
  mapTensorLinearly(graph_, B);

  auto prog = Sequence();
  popops::scaledSubtractFrom(graph_, inOut_, A, in_, B, prog, "debug string",
                             optionFlag);
  const auto profile = runProgram(prog);

  CHECK_OUTPUT_IS_AXMINUSBY(a, b);
  CHECK_WAS_MIXED_PRECISION(profile);
}

BOOST_DATA_TEST_CASE_F(HalfTensorAXBYTestFixture,
                       StdAXMinusBy_ScaleByFloatConstant,
                       HalfTensorAXBYTestFixture::OptionFlags(), optionFlag) {
  const float a = 2, b = 3;

  auto prog = Sequence();
  popops::scaledSubtractFrom(graph_, inOut_, a, in_, b, prog, "debug string",
                             optionFlag);
  runProgram(prog);

  // We dont check for casts when scaling by a constant because type handling
  // happens in C++ in scaledSubtractFrom
  CHECK_OUTPUT_IS_AXMINUSBY(a, b);
}

BOOST_DATA_TEST_CASE_F(HalfTensorAXBYTestFixture,
                       StdXMinusBy_ScaleByFloatTensor,
                       HalfTensorAXBYTestFixture::OptionFlags(), optionFlag) {
  const float b = 3;

  auto B = graph_.addVariable(FLOAT, {});
  graph_.setInitialValue(B, b);
  mapTensorLinearly(graph_, B);

  auto prog = Sequence();
  popops::scaledSubtractFrom(graph_, inOut_, in_, B, prog, "debug string",
                             optionFlag);
  const auto profile = runProgram(prog);

  CHECK_OUTPUT_IS_XMINUSBY(b);
  CHECK_WAS_MIXED_PRECISION(profile);
}

BOOST_DATA_TEST_CASE_F(HalfTensorAXBYTestFixture,
                       StdXMinusBy_ScaleByFloatConstant,
                       HalfTensorAXBYTestFixture::OptionFlags(), optionFlag) {
  const float b = 3;

  auto prog = Sequence();
  popops::scaledSubtractFrom(graph_, inOut_, in_, b, prog, "debug string",
                             optionFlag);
  runProgram(prog);

  // We dont check for casts when scaling by a constant because type handling
  // happens in C++ in scaledSubtractFrom
  CHECK_OUTPUT_IS_XMINUSBY(b);
}
