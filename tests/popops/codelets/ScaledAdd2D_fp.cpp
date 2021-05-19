// Copyright (c) 2018 Graphcore Ltd. All rights reserved.
#define BOOST_TEST_MODULE ScaledAdd2D_fp
#include "poplibs_test/Util.hpp"
#include "popops/codelets.hpp"
#include <cassert>
#include <poplar/Engine.hpp>
#include <poplibs_support/TestDevice.hpp>

using namespace poplar;
using namespace poplar::program;
using namespace poplibs_test::util;
using namespace poplibs_support;

#define PADDING_VALUE -50.0

// test data of lengths 1 to 16
const std::vector<std::vector<float>> data = {
    {36.8533},
    {23.5636, 23.7882},
    {24.417, 38.0108, 17.0022},
    {12.1692, 6.9111, 18.6011, 32.8726},
    {45.094, 24.5472, 37.5412, 3.4541, 6.9619},
    {10.4058, 20.8296, 33.4116, 16.1244, 25.7758, 48.9353},
    {6.7283, 46.5906, 42.5837, 1.6201, 47.1409, 35.1936, 40.1955},
    {47.5474, 36.7667, 30.1007, 15.7696, 10.2956, 36.4658, 28.5871, 29.7736},
    {29.6436, 35.528, 1.8762, 18.5469, 34.1792, 11.3447, 7.5079, 17.6522,
     1.4099},
    {8.0924, 6.3349, 38.9663, 23.1158, 32.5319, 24.9817, 20.7309, 0.2784,
     10.5053, 9.5292},
    {45.321, 21.2337, 2.8621, 1.3115, 5.2678, 35.5605, 44.7426, 29.9398,
     18.3015, 7.652, 17.8568},
    {9.7856, 46.8125, 47.1037, 39.7729, 9.9586, 11.7717, 41.9851, 2.2573,
     33.2076, 3.7827, 1.2203, 12.4487},
    {34.2556, 39.2798, 24.5538, 30.5591, 12.5051, 15.4922, 25.2939, 27.9103,
     48.8992, 37.6403, 49.1898, 30.2812, 44.8177},
    {37.9318, 42.0591, 22.0478, 32.4315, 13.4697, 18.2585, 18.1887, 42.0544,
     13.2323, 39.8405, 0.9929, 16.7709, 6.0279, 27.7244},
    {37.5095, 29.3018, 42.4159, 41.1092, 15.3115, 8.1059, 49.794, 33.2661,
     12.0308, 32.1723, 20.4024, 33.2543, 45.788, 31.629, 10.0015},
    {15.4047, 20.302, 30.0201, 22.2119, 18.5737, 9.0296, 19.6283, 15.2062,
     29.6811, 26.4103, 3.3177, 37.487, 11.1615, 29.5318, 20.1781, 8.4898},
};

const std::vector<std::vector<float>> deltas = {
    {24.4383},
    {37.1046, 35.5357},
    {13.4149, 2.0323, 8.3695},
    {16.8299, 8.2711, 47.8028, 16.8223},
    {28.5599, 32.9726, 18.01, 49.5828, 26.3351},
    {43.593, 8.4078, 29.9879, 22.1314, 32.8828, 45.6865},
    {49.1341, 44.1327, 46.3776, 7.5378, 31.5884, 12.8373, 5.3876},
    {41.0354, 29.5542, 5.6224, 1.62, 23.3489, 42.2291, 18.367, 33.6943},
    {4.0742, 4.1536, 41.8209, 13.1041, 27.9982, 9.8072, 22.2375, 36.9369,
     35.2985},
    {34.6223, 13.4062, 23.1151, 28.9503, 25.0751, 5.6493, 26.9687, 36.45,
     28.7066, 22.3477},
    {11.8482, 38.347, 45.53, 40.5914, 22.6172, 5.8899, 49.6522, 10.6218, 5.4388,
     49.3297, 15.2486},
    {29.7998, 13.2817, 42.2754, 13.4615, 46.4793, 10.8529, 43.8179, 15.9517,
     14.1261, 46.1555, 24.9081, 13.8895},
    {4.6595, 46.7121, 16.9035, 41.9907, 24.343, 21.4885, 16.4146, 1.8442,
     36.553, 34.3669, 14.7804, 14.9641, 34.8731},
    {6.426, 29.7145, 25.934, 34.9078, 34.9429, 10.8451, 49.6866, 24.9291, 6.338,
     9.7048, 33.9664, 0.5189, 16.1818, 30.5154},
    {27.3815, 39.4755, 18.1972, 36.0831, 3.7732, 45.9714, 25.2575, 3.7553,
     47.3133, 5.6741, 5.8831, 20.8678, 1.2767, 20.6127, 37.955},
    {29.0532, 40.3651, 44.8964, 1.4079, 0.9379, 19.0102, 8.4806, 10.0201,
     31.092, 34.0013, 11.8073, 20.0071, 49.0702, 25.1766, 5.3527, 9.115},
};

float k = 2.5653;

double atol(const Type &type) { return type == HALF ? 1e-7 : 1e-20; }

void testScaledAdd2D(const char *vertex, const Type &dataType,
                     const Type &deltaType, const Type &scaleType,
                     const bool &constantFactor, const float &factorA,
                     const float &factorB, const float &factorData = 1.0,
                     const float &factorDelta = 1.0, const float testSign = 1.0,
                     const bool doXminusaXPlusbY = false,
                     const float &scaleFloatTolerance = 0.0,
                     const double testTolerance = 0.1) {
  auto device = createTestDevice(TEST_TARGET);
  Graph graph(device.getTarget());
  popops::addCodelets(graph);

  // Scale the input vectors
  std::vector<std::vector<float>> scaledData(data.size());
  std::vector<std::vector<float>> scaledDeltas(deltas.size());
  unsigned tensorLength = data.back().size();
  for (unsigned i = 0; i < deltas.size(); i++) {
    scaledData[i].resize(tensorLength);
    scaledDeltas[i].resize(tensorLength);
    for (unsigned j = 0; j < deltas[i].size(); j++) {
      scaledData[i][j] = factorData * data[i][j];
      scaledDeltas[i][j] = factorDelta * deltas[i][j];
    }

    // Append padding
    for (unsigned j = deltas[i].size(); j < tensorLength; j++) {
      scaledData[i][j] = PADDING_VALUE;
      scaledDeltas[i][j] = PADDING_VALUE;
    }
  }
  const bool vertexHasTolerance =
      ((std::string(vertex).rfind("popops::ScaledAdd2D", 0) == 0 ||
        std::string(vertex).rfind("popops::aXPlusbY2D", 0) == 0) &&
       dataType == HALF && deltaType == HALF && scaleType == FLOAT);

  // Generate the expected result
  std::vector<std::vector<float>> expected(scaledData.size());

  auto dataScaling = doXminusaXPlusbY ? 1 - factorA : factorA;
  for (unsigned i = 0; i < scaledData.size(); i++) {
    expected[i].resize(tensorLength);
    std::copy(scaledData[i].begin(), scaledData[i].end(), expected[i].begin());
    for (unsigned j = 0; j < data[i].size(); j++) {
      expected[i][j] = dataScaling * scaledData[i][j] +
                       testSign * factorB * scaledDeltas[i][j];
    }
  }

  const auto &target = device.getTarget();

  auto cs = graph.addComputeSet("cs");
  auto v = graph.addVertex(cs, vertex);
  graph.setTileMapping(v, 0);
  graph.setFieldSize(v["A"], scaledData.size());
  graph.setFieldSize(v["B"], scaledDeltas.size());

  if (constantFactor) {
    if (factorA == 1.0) {
      graph.setInitialValue(v["scaleB"], std::fabs(factorB));
    } else {
      graph.setInitialValue(v["scaleA"], factorA);
      graph.setInitialValue(v["scaleB"], factorB);
    }
  } else {
    auto factorBTensor = graph.addVariable(scaleType, {});
    graph.setTileMapping(factorBTensor, 0);
    if (factorA == 1.0) {
      graph.connect(v["scaleB"], factorBTensor);
      graph.setInitialValue(factorBTensor, std::fabs(factorB));
    } else {
      graph.connect(v["scaleB"], factorBTensor);
      graph.setInitialValue(factorBTensor, factorB);

      auto factorATensor = graph.addVariable(scaleType, {});
      graph.setTileMapping(factorATensor, 0);
      graph.connect(v["scaleA"], factorATensor);
      graph.setInitialValue(factorATensor, factorA);
    }
    if (vertexHasTolerance) {
      graph.setInitialValue(v["tolerance"], scaleFloatTolerance);
    }
  }

  // create tensors for each of the input rows.
  assert(scaledData.size() == scaledDeltas.size());

  for (unsigned i = 0; i < scaledData.size(); ++i) {
    Interval interval = {0, data[i].size()};
    auto datumTensor = graph.addVariable(dataType, {tensorLength});
    graph.setTileMapping(datumTensor, 0);
    graph.connect(v["A"][i], datumTensor.slice(interval));
    graph.createHostRead("datum" + std::to_string(i), datumTensor);
    graph.createHostWrite("datum" + std::to_string(i), datumTensor);

    auto deltaTensor = graph.addVariable(deltaType, {tensorLength});
    graph.setTileMapping(deltaTensor, 0);
    graph.connect(v["B"][i], deltaTensor.slice(interval));
    graph.createHostWrite("delta" + std::to_string(i), deltaTensor);
  }

  Execute prog(cs);
  Engine e(graph, prog);
  device.bind([&](const Device &d) {
    e.load(d);

    // write tensors to the device.
    std::vector<char> dataBuffer(scaledData.size() *
                                 target.getTypeSize(dataType));
    std::vector<char> deltaBuffer(scaledData.size() *
                                  target.getTypeSize(deltaType));
    for (unsigned i = 0; i < scaledData.size(); ++i) {
      const auto &datum = scaledData[i];
      const auto &delta = scaledDeltas[i];

      const auto size = datum.size();
      copy(target, datum.data(), size, dataType, &dataBuffer[0]);
      e.writeTensor("datum" + std::to_string(i), &dataBuffer[0],
                    &dataBuffer[size * target.getTypeSize(dataType)]);

      copy(target, delta.data(), size, deltaType, &deltaBuffer[0]);
      e.writeTensor("delta" + std::to_string(i), &deltaBuffer[0],
                    &deltaBuffer[size * target.getTypeSize(deltaType)]);
    }

    e.run();

    // check results against the expected output.
    for (unsigned i = 0; i < scaledData.size(); ++i) {
      const auto &datum = scaledData[i];
      const auto size = datum.size();
      const auto streamSizeInBytes = size * target.getTypeSize(dataType);
      std::unique_ptr<char[]> src(new char[streamSizeInBytes]);
      e.readTensor("datum" + std::to_string(i), src.get(),
                   src.get() + streamSizeInBytes);

      std::vector<float> actual(size);
      copy(target, dataType, src.get(), actual.data(), size);

      BOOST_CHECK(checkIsClose("i=" + std::to_string(i), actual.data(), {size},
                               expected[i].data(), size, testTolerance,
                               atol(dataType)));
    }
  });
}

BOOST_AUTO_TEST_SUITE(ScaledAdd2DHalfConst)

BOOST_AUTO_TEST_CASE(ScaledAdd2DHalfConst) {
  testScaledAdd2D("popops::ScaledAdd2D<half,half,half,true,true>", HALF, HALF,
                  HALF, true, 1.0, k);
  testScaledAdd2D("popops::ScaledAdd2D<half,half,half,true,false>", HALF, HALF,
                  HALF, true, 1.0, k);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DHalfTensor)

BOOST_AUTO_TEST_CASE(ScaledAdd2DHalfTensor) {
  testScaledAdd2D("popops::ScaledAdd2D<half,half,half,false,true>", HALF, HALF,
                  HALF, false, 1.0, k);
  testScaledAdd2D("popops::ScaledAdd2D<half,half,half,false,false>", HALF, HALF,
                  HALF, false, 1.0, k);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DHalfFloatConst)

BOOST_AUTO_TEST_CASE(ScaledAdd2DHalfFloatConst) {
  testScaledAdd2D("popops::ScaledAdd2D<half,float,half,true,false>", HALF,
                  FLOAT, HALF, true, 1.0, k);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DHalfFloatTensor)

BOOST_AUTO_TEST_CASE(ScaledAdd2DHalfTFloatensor) {
  testScaledAdd2D("popops::ScaledAdd2D<half,float,half,false,false>", HALF,
                  FLOAT, HALF, false, 1.0, k);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DHalfFloatFloatConst)

BOOST_AUTO_TEST_CASE(ScaledAdd2DHalfFloatFloatConst) {
  testScaledAdd2D("popops::ScaledAdd2D<half,float,float,true,false>", HALF,
                  FLOAT, FLOAT, true, 1.0, k);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DHalfFloatFloatTensor)

BOOST_AUTO_TEST_CASE(ScaledAdd2DHalfFloatFloatTensor) {
  testScaledAdd2D("popops::ScaledAdd2D<half,float,float,false,false>", HALF,
                  FLOAT, FLOAT, false, 1.0, k);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledSubtract2DHalfTensor)

BOOST_AUTO_TEST_CASE(ScaledSubtract2DHalfTensor) {
  testScaledAdd2D("popops::ScaledSubtract2D<half,half,true>", HALF, HALF, HALF,
                  false, 1.0, -k);
  testScaledAdd2D("popops::ScaledSubtract2D<half,half,false>", HALF, HALF, HALF,
                  false, 1.0, -k);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DFloatConst)

BOOST_AUTO_TEST_CASE(ScaledAdd2DFloatConst) {
  testScaledAdd2D("popops::ScaledAdd2D<float,float,float,true,true>", FLOAT,
                  FLOAT, FLOAT, true, 1.0, k);
  testScaledAdd2D("popops::ScaledAdd2D<float,float,float,true,false>", FLOAT,
                  FLOAT, FLOAT, true, 1.0, k);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DFloatTensor)

BOOST_AUTO_TEST_CASE(ScaledAdd2DFloatTensor) {
  testScaledAdd2D("popops::ScaledAdd2D<float,float,float,false,true>", FLOAT,
                  FLOAT, FLOAT, false, 1.0, k);
  testScaledAdd2D("popops::ScaledAdd2D<float,float,float,false,false>", FLOAT,
                  FLOAT, FLOAT, false, 1.0, k);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DFloatHalfHalf)

BOOST_AUTO_TEST_CASE(ScaledAdd2DFloatHalfHalf) {
  testScaledAdd2D("popops::ScaledAdd2D<float,half,half,false,false>", FLOAT,
                  HALF, HALF, false, 1.0, k);
  testScaledAdd2D("popops::ScaledAdd2D<float,half,half,true,false>", FLOAT,
                  HALF, HALF, true, 1.0, k);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DFloatHalfFloat)

BOOST_AUTO_TEST_CASE(ScaledAdd2DFloatHalfFloat) {
  testScaledAdd2D("popops::ScaledAdd2D<float,half,float,false,false>", FLOAT,
                  HALF, FLOAT, false, 1.0, k);
  testScaledAdd2D("popops::ScaledAdd2D<float,half,float,true,false>", FLOAT,
                  HALF, FLOAT, true, 1.0, k);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DHalfHalfFloatConst)

BOOST_AUTO_TEST_CASE(ScaledAdd2DHalfHalfFloatConst) {
  testScaledAdd2D("popops::ScaledAdd2D<half,half,float,true,true>", HALF, HALF,
                  FLOAT, true, 1.0, 1e-6, 6e-8, 1310.0, 1.0, false, 0.0, 0.01);
  testScaledAdd2D("popops::ScaledAdd2D<half,half,float,true,false>", HALF, HALF,
                  FLOAT, true, 1.0, 1e-6, 6e-8, 1310.0, 1.0, false, 0.0, 0.01);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DHalfHalfFloatTensorHighTol)

BOOST_AUTO_TEST_CASE(ScaledAdd2DHalfHalfFloatTensorHighTol) {
  testScaledAdd2D("popops::ScaledAdd2D<half,half,float,false,true>", HALF, HALF,
                  FLOAT, false, 1.0, k, 1.0, 1.0, 1.0, false, 1e-3);
  testScaledAdd2D("popops::ScaledAdd2D<half,half,float,false,false>", HALF,
                  HALF, FLOAT, false, 1.0, k, 1.0, 1.0, 1.0, false, 1e-3);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledAdd2DHalfHalfFloatTensorLowTol)

BOOST_AUTO_TEST_CASE(ScaledAdd2DHalfHalfFloatTensorLowTol) {
  testScaledAdd2D("popops::ScaledAdd2D<half,half,float,false,true>", HALF, HALF,
                  FLOAT, false, 1.0, 1e-6, 6e-8, 1310.0, 1.0, false, 0.0, 0.01);
  testScaledAdd2D("popops::ScaledAdd2D<half,half,float,false,false>", HALF,
                  HALF, FLOAT, false, 1.0, 1e-6, 6e-8, 1310.0, 1.0, false, 0.0,
                  0.01);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledSubtract2DFloatTensor)

BOOST_AUTO_TEST_CASE(ScaledSubtract2DFloatTensor) {
  testScaledAdd2D("popops::ScaledSubtract2D<float,float,true>", FLOAT, FLOAT,
                  FLOAT, false, 1.0, -k);
  testScaledAdd2D("popops::ScaledSubtract2D<float,float,false>", FLOAT, FLOAT,
                  FLOAT, false, 1.0, -k);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledSubtract2DHalfHalfFloatTensorHighTol)

BOOST_AUTO_TEST_CASE(ScaledSubtract2DHalfHalfFloatTensorHighTol) {
  testScaledAdd2D("popops::ScaledSubtract2D<half,float,true>", HALF, HALF,
                  FLOAT, false, 1.0, k, 1.0, 1.0, -1.0, false, 1e-3);
  testScaledAdd2D("popops::ScaledSubtract2D<half,float,false>", HALF, HALF,
                  FLOAT, false, 1.0, k, 1.0, 1.0, -1.0, false, 1e-3);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ScaledSubract2DHalfHalfFloatTensorLowTol)

BOOST_AUTO_TEST_CASE(ScaledSubtract2DHalfHalfFloatTensorLowTol) {
  testScaledAdd2D("popops::ScaledSubtract2D<half,float,true>", HALF, HALF,
                  FLOAT, false, 1.0, 1e-6, 6e-8, 1310.0, -1.0, false, 0.0,
                  0.01);
  testScaledAdd2D("popops::ScaledSubtract2D<half,float,false>", HALF, HALF,
                  FLOAT, false, 1.0, 1e-6, 6e-8, 1310.0, -1.0, false, 0.0,
                  0.01);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(aXPlusbYHalfConst)

BOOST_AUTO_TEST_CASE(aXPlusbYHalfConst) {
  testScaledAdd2D("popops::aXPlusbY2D<half,half,true,true>", HALF, HALF, HALF,
                  true, 1.0 * k, -k);
  testScaledAdd2D("popops::aXPlusbY2D<half,half,true,false>", HALF, HALF, HALF,
                  true, 1.0 * k, -k);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(aXPlusbYHalfTensor)

BOOST_AUTO_TEST_CASE(aXPlusbYHalfTensor) {
  testScaledAdd2D("popops::aXPlusbY2D<half,half,false,true>", HALF, HALF, HALF,
                  false, -1.0 * k, k);
  testScaledAdd2D("popops::aXPlusbY2D<half,half,false,false>", HALF, HALF, HALF,
                  false, -1.0 * k, k);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(aXPlusbYMixedConst)

BOOST_AUTO_TEST_CASE(aXPlusbYMixedConst) {
  testScaledAdd2D("popops::aXPlusbY2D<half,float,true,false>", HALF, HALF,
                  FLOAT, true, 1.0 * k, -k);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(aXPlusbYMixedTensor)

BOOST_AUTO_TEST_CASE(aXPlusbYMixedTensorSlow) {
  // Run with a small tolerance (0.0001%) so that at runtime we chose the
  // slower mixed (data=HALF, scale values=FLOAT) path
  testScaledAdd2D("popops::aXPlusbY2D<half,float,false,false>", HALF, HALF,
                  FLOAT, false, -1.0 * k, k, 1.0, 1.0, 1.0, false, 1e-6);
}
BOOST_AUTO_TEST_CASE(aXPlusbYMixedTensorFast) {
  // Run with a big tolerance (1%) so that at runtime we chose the fast
  // path with data=HALF, scale values=HALF
  testScaledAdd2D("popops::aXPlusbY2D<half,float,false,false>", HALF, HALF,
                  FLOAT, false, -1.0 * k, k, 1.0, 1.0, 1.0, false, 0.01);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(XMinusaXPlusbYHalfConst)

BOOST_AUTO_TEST_CASE(XMinusaXPlusbYHalfConst) {
  testScaledAdd2D("popops::XMinusaXPlusbY2D<half,true,true>", HALF, HALF, HALF,
                  true, -1.0 * k, k, 1, 1, 1, true);
  testScaledAdd2D("popops::XMinusaXPlusbY2D<half,true,false>", HALF, HALF, HALF,
                  true, -1.0 * k, k, 1, 1, 1, true);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(XMinusaXPlusbYHalfTensor)

BOOST_AUTO_TEST_CASE(XMinusaXPlusbYHalfTensor) {
  testScaledAdd2D("popops::XMinusaXPlusbY2D<half,false,true>", HALF, HALF, HALF,
                  false, -1.0 * k, k, 1, 1, 1, true);
  testScaledAdd2D("popops::XMinusaXPlusbY2D<half,false,false>", HALF, HALF,
                  HALF, false, -1.0 * k, k, 1, 1, 1, true);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(aXMinusbYHalfTensor)

BOOST_AUTO_TEST_CASE(aXMinusbYHalfTensor) {
  // testSign = -1.0 to test aXMinusb
  testScaledAdd2D("popops::aXMinusbY2D<half,half,false,true>", HALF, HALF, HALF,
                  false, -1.0 * k, k, 1.0, 1.0, -1.0);
  testScaledAdd2D("popops::aXMinusbY2D<half,half,false,false>", HALF, HALF,
                  HALF, false, -1.0 * k, k, 1.0, 1.0, -1.0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(aXMinusbYMixedTensor)

BOOST_AUTO_TEST_CASE(aXMinusbYMixedTensorSlow) {
  // Run with a small tolerance (0.0001%) so that at runtime we chose the
  // slower mixed (data=HALF, scale values=FLOAT) path
  testScaledAdd2D("popops::aXMinusbY2D<half,float,false,false>", HALF, HALF,
                  FLOAT, false, -1.0 * k, k, 1.0, 1.0, -1.0, false, 1e-6);
}
BOOST_AUTO_TEST_CASE(aXMinusbYMixedTensorFast) {
  // Run with a big tolerance (1%) so that at runtime we chose the fast
  // path with data=HALF, scale values=HALF
  testScaledAdd2D("popops::aXMinusbY2D<half,float,false,false>", HALF, HALF,
                  FLOAT, false, -1.0 * k, k, 1.0, 1.0, -1.0, false, 0.01);
}

BOOST_AUTO_TEST_SUITE_END()
