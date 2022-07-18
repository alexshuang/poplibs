// Copyright (c) 2020 Graphcore Ltd. All rights reserved.

#define BOOST_TEST_MODULE ExprName
#include <../lib/popops/ExpressionGenerator.hpp>
#include <boost/test/unit_test.hpp>
#include <limits>
#include <poplar/TypeTraits.hpp>
#include <poplibs_support/TestDevice.hpp>
#include <popops/Expr.hpp>

using namespace poplar;
using namespace popops::expr;
using namespace popops;
using namespace poplibs_support;

static void checkNames(const Expr &A, const Expr &B, const bool match = false,
                       const bool inPlace = false) {

  auto device = createTestDevice(TEST_TARGET, 1, 4);
  auto target = device.getTarget();
  poplar::Graph graph(target);

  std::vector<Tensor> inputs;
  for (auto type : {HALF, FLOAT}) {
    for (unsigned i = 0; i < 3; ++i) {
      inputs.push_back(graph.addVariable(type, {i}, "some_name"));
    }
  }
  const auto aName = GenerateCodeletFromMapExpr::createVertexName(
      A, inputs, inPlace, false, false);

  const auto bName = GenerateCodeletFromMapExpr::createVertexName(
      B, inputs, inPlace, false, false);

  BOOST_CHECK((aName == bName) == match);
  std::cerr << "A " << aName << " ------ B " << bName
            << " (should match: " << (match ? "yes" : "no") << ")" << std::endl;
}

const int One = 1;
const short OtherOne = 1;
TypeTraits tt;

BOOST_AUTO_TEST_CASE(CheckNames) {
  checkNames(Mul(Add(_1, _2), _3), Mul(Add(_1, _1), _1));
  checkNames(Mul(Add(_1, _2), _3), Mul(Add(_4, _5), _6));
  checkNames(Mul(Add(_1, _2), _3), Mul(Add(_3, _2), _1));
  checkNames(Mul(Add(_1, _2), _3), Mul(Add(_1, _2), _3), true);
  checkNames(Select(_1, _2, _3), Clamp(_1, _2, _3));
  checkNames(Asin(Add(_1, _2)), Add(_1, _2));
  checkNames(Add(_1, _2), Add(_2, _1));
  checkNames(Sub(Add(_1, _1), _1), Sub(_1, Add(_1, _1)));
  checkNames(ConstHalf(4.0f), ConstHalf(4.0f), true);
  checkNames(Const(4.0f), ConstHalf(4.0f));
  checkNames(Const(One), Const(OtherOne), true);
  checkNames(Add(_1, Cast(_4, HALF)), Add(_2, Cast(_5, HALF)));
  checkNames(
      Mul(Add(_1, popops::expr::Const(std::numeric_limits<float>::max())), _3),
      Mul(Add(_1, _1), _1));
}

BOOST_AUTO_TEST_CASE(ConstFloatRoundTrip) {

  BOOST_CHECK(std::stof(Const(1.0f).printValue()) == 1.0f);
  BOOST_CHECK(std::stof(Const(1e-8f).printValue()) == 1e-8f);
  BOOST_CHECK(
      std::stof(Const(std::numeric_limits<float>::lowest()).printValue()) ==
      std::numeric_limits<float>::lowest());
  BOOST_CHECK(
      std::stof(Const(std::numeric_limits<float>::min()).printValue()) ==
      std::numeric_limits<float>::min());
  BOOST_CHECK(
      std::stof(Const(std::numeric_limits<float>::max()).printValue()) ==
      std::numeric_limits<float>::max());
}
