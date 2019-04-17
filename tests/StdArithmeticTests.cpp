#define BOOST_TEST_MODULE StdArithmeticTests

#include <boost/test/unit_test.hpp>
#include <poputil/TileMapping.hpp>
#include <poplar/Engine.hpp>
#include <poplar/IPUModel.hpp>
#include <popops/codelets.hpp>
#include <iostream>
#include <popops/ScaledAdd.hpp>
#include "popops/ElementWise.hpp"
#include <popops/Cast.hpp>
#include "TestDevice.hpp"

using namespace poplar;
using namespace poplar::program;
using namespace poputil;
using namespace popops;
using namespace popops::expr;

namespace utf = boost::unit_test;
namespace fpc = boost::test_tools::fpc;

#define DIM_SIZE  10

static std::tuple<Tensor, Tensor> mapBinaryOpTensors(Graph &graph,
                                                     const Type &type) {
  auto in1 = graph.addVariable(type, {DIM_SIZE, DIM_SIZE}, "in1");
  mapTensorLinearly(graph, in1);

  auto in2 = graph.addVariable(type, {DIM_SIZE, DIM_SIZE}, "in2");
  mapTensorLinearly(graph, in2);

  return std::make_pair(in1.dimShuffle({1, 0}), in2.dimShuffle({1, 0}));
}


static void setBinaryOpInputs(float hIn1[DIM_SIZE][DIM_SIZE],
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

static void setBinaryOpInputs(int hIn1[DIM_SIZE][DIM_SIZE],
                              int hIn2[DIM_SIZE][DIM_SIZE]) {
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
static void setBroadcastOpInputs(float hIn1[DIM_SIZE][DIM_SIZE]) {
  float val1 = -100;
  for (auto r = 0; r != DIM_SIZE; ++r) {
    for (auto c = 0; c != DIM_SIZE; ++c) {
      int sign1 = (1 - 2 * ((c + 1) & 1));
      hIn1[r][c] = (val1 + (r * DIM_SIZE + c) * 1) * sign1;
    }
  }
}

BOOST_AUTO_TEST_CASE(StdBroadcastAdd_float,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))
                  ) {
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

    eng.writeTensor("in", hIn);
    eng.run();
    eng.readTensor("out", hOut);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn[i][j] + k;
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(StdBroadcastMultiply_float,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))
                  ) {
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

    eng.writeTensor("in", hIn);
    eng.run();
    eng.readTensor("out", hOut);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn[i][j] * k;
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(StdBroadcastSubtract_half,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))
                  ) {
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

    eng.writeTensor("in", rawIn.data());
    eng.run();
    eng.readTensor("out", rawOut.data());
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

BOOST_AUTO_TEST_CASE(StdAddTo_half_float_tensor,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(1.4))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(1.4))
                  ) {
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
    eng.writeTensor("in1", rawIn1.data());
    eng.writeTensor("in2", hIn2);
    eng.run();
    eng.readTensor("out", rawOut.data());
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

BOOST_AUTO_TEST_CASE(StdAddTo_float_constant,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))
                  ) {
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
    eng.writeTensor("in1", hIn1);
    eng.writeTensor("in2", hIn2);
    eng.run();
    eng.readTensor("out", hOut);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] + k * hIn2[i][j];
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}


BOOST_AUTO_TEST_CASE(StdAddTo_float_tensor,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))
                  ) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  float hIn1[DIM_SIZE][DIM_SIZE], hIn2[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn1, hIn2);

  float k = 2;
  auto factor = graph.addVariable(FLOAT, {});
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
    eng.writeTensor("in1", hIn1);
    eng.writeTensor("in2", hIn2);
    eng.run();
    eng.readTensor("out", hOut);
  });

  // Check result
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] + k * hIn2[i][j];
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}
void expressionTestHalfImpl(const unsigned expression) {
  auto device = createTestDevice(TEST_TARGET);
  auto target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  float hIn1[DIM_SIZE][DIM_SIZE], hIn2[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn1, hIn2);

  float k = 2;
  float k2 = 4.0;
  auto factor = graph.addVariable(HALF, {});
  graph.setInitialValue(factor, k);

  auto in1 = graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "in1");
  auto in2 = graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE}, "in2");
  mapTensorLinearly(graph, in1);
  mapTensorLinearly(graph, in2);
  mapTensorLinearly(graph, factor);

  auto rawBufSize = target.getTypeSize(HALF) * DIM_SIZE * DIM_SIZE;
  std::vector<char> rawIn1(rawBufSize);
  poplar::copyFloatToDeviceHalf(target, &hIn1[0][0], rawIn1.data(),
                                DIM_SIZE * DIM_SIZE);
  std::vector<char> rawIn2(rawBufSize);
  poplar::copyFloatToDeviceHalf(target, &hIn2[0][0], rawIn2.data(),
                                DIM_SIZE * DIM_SIZE);

  graph.createHostWrite("in1", in1);
  graph.createHostWrite("in2", in2);
  graph.createHostRead("out", in1);
  auto prog = Sequence();
  switch (expression) {
    case 1:
      mapInPlace(graph, Add(Sub(_1, Mul(_2, _3)), Mul(_2, _3)),
                {in1, in2, factor}, prog);
      break;
    case 2:
      mapInPlace(graph, Add(Mul(_1, _3),  Mul(_2, Const(k2))),
                {in1, in2, factor}, prog);
      break;
    case 3:
      mapInPlace(graph, Add(Sub(_1, Mul(Const(k2), _2)),  Mul(Square(_3),_2)),
                {in1, in2, factor}, prog);
      break;
  };
  Engine eng(graph, prog);

  std::vector<char> rawOut(rawBufSize);
  float hOut[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);
    eng.writeTensor("in1", rawIn1.data());
    eng.writeTensor("in2", rawIn2.data());
    eng.run();
    eng.readTensor("out", rawOut.data());
  });

  poplar::copyDeviceHalfToFloat(target, rawOut.data(), &hOut[0][0],
                                                          DIM_SIZE * DIM_SIZE);
  // Check result
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res;
      switch (expression) {
        case 1:
          res = (hIn1[i][j] - k * hIn2[i][j]) + hIn2[i][j] * k;
          break;
        case 2:
          res = hIn1[i][j] * k + hIn2[i][j] * k2;
          break;
        case 3:
          res = hIn1[i][j] - hIn2[i][j] * k2 + hIn2[i][j] * k * k;
          break;
      };
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}
BOOST_AUTO_TEST_CASE(StdExpression1_half_tensor,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(1.4))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(1.4))
                  ) {
    expressionTestHalfImpl(1);
}

BOOST_AUTO_TEST_CASE(StdExpression2_half_tensor,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(1.4))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(1.4))
                  ) {
    expressionTestHalfImpl(2);
}

BOOST_AUTO_TEST_CASE(StdExpression3_half_tensor,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(1.4))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(1.4))
                  ) {
    expressionTestHalfImpl(3);
}

BOOST_AUTO_TEST_CASE(StdExpressionSubtractFrom_float,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))
                  ) {
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
  mapInPlace(graph, Sub(_1, Mul(_2, Const(k))), {in1, in2}, prog);
  Engine eng(graph, prog);
  float hOut[DIM_SIZE][DIM_SIZE];

  device.bind([&](const Device &d) {
    eng.load(d);
    eng.writeTensor("in1", hIn1);
    eng.writeTensor("in2", hIn2);
    eng.run();
    eng.readTensor("out", hOut);
  });

  // Check result
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] - k * hIn2[i][j];
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}


BOOST_AUTO_TEST_CASE(StdSubtractFrom_float_tensor,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))
                  ) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  float hIn1[DIM_SIZE][DIM_SIZE], hIn2[DIM_SIZE][DIM_SIZE];
  setBinaryOpInputs(hIn1, hIn2);

  float k = 2;
  auto factor = graph.addVariable(FLOAT, {});
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
    eng.writeTensor("in1", hIn1);
    eng.writeTensor("in2", hIn2);
    eng.run();
    eng.readTensor("out", hOut);
  });

  // Check result
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] - k * hIn2[i][j];
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(StdSubFrom_int,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(0.01))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(0.01))
                  ) {
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
    eng.writeTensor("in1", hIn1);
    eng.writeTensor("in2", hIn2);
    eng.run();
    eng.readTensor("out", hOut);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    for (auto j = 0U; j < DIM_SIZE; ++j) {
      double res = hIn1[i][j] - k * hIn2[i][j];
      BOOST_TEST(hOut[i][j] == res);
    }
  }
}

BOOST_AUTO_TEST_CASE(StdaXPlusbY_half_tensor_and_const,
                  *utf::tolerance<float>(fpc::percent_tolerance<float>(1))
                  *utf::tolerance<double>(fpc::percent_tolerance<double>(1))
                  ) {
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
  auto inOutConstTest = graph.addVariable(HALF, {DIM_SIZE, DIM_SIZE},
                                                            "inOutConstTest");
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
  scaledAddTo(graph, inOut, A, in, B, prog);
  scaledAddTo(graph, inOutConstTest, -k, in, -k2, prog);

  Engine eng(graph, prog);

  device.bind([&](const Device &d) {
    eng.load(d);

    eng.writeTensor("in", rawIn.data());
    eng.writeTensor("inOut", rawInOut.data());
    eng.run();
    eng.readTensor("out", rawOut.data());
    eng.readTensor("outConstTest", rawOutConstTest.data());
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
      double res = k * hInOut[i][j] + k2 * hIn[i][j];
      BOOST_TEST(hOut[i][j] == res, "Tensor scale test");
      BOOST_TEST(hOutConstTest[i][j] == -res, "Constant scale test");
    }
  }
}


BOOST_AUTO_TEST_CASE(StdCast) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  float hIn[DIM_SIZE];
  for (auto i = 0U; i<DIM_SIZE; ++i) {
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
    eng.writeTensor("in", hIn);
    eng.run();
    eng.readTensor("out", hOut);
  });

  /* Check result */
  for (auto i = 0U; i < DIM_SIZE; ++i) {
    BOOST_TEST(hOut[i] == i);
  }
}
