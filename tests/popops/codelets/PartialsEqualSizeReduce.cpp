// Copyright (c) 2018 Graphcore Ltd. All rights reserved.
#define BOOST_TEST_MODULE PartialsEqualSizeReduce
#include <poplar/Engine.hpp>
#include <poplibs_support/TestDevice.hpp>
// codelets
#include "../../lib/popops/reduction/ReductionVertex.hpp"
#include "poplar/Target.hpp"
#include "poplibs_test/Check.hpp"
#include "poplibs_test/Util.hpp"
#include "popops/codelets.hpp"
#include "poputil/VertexTemplates.hpp"
#include <boost/program_options.hpp>
#include <poplibs_test/Reduce.hpp>
#include <popops/Reduce.hpp>
#include <stdexcept>
#include <string.h>

using namespace poplar;
using namespace poplar::program;
using namespace popops;
using namespace poputil;
using namespace poplibs_test::util;
using namespace poplar_test;
using namespace poplibs_test::reduce;
using namespace poplibs_support;

const OptionFlags options;

static bool doTest(const DeviceType &deviceType, const Type &partialsType,
                   const Type &outType, const unsigned outerDim,
                   const unsigned _innerDim, const unsigned outputDim,
                   const popops::Operation op, const float scale,
                   bool isUpdate) {
  auto device = createTestDevice(deviceType);
  auto &target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  const float initialValue = 1.0;
  const unsigned pad = 1;
  const unsigned innerDim = _innerDim + pad;
  const auto grainSize = partialsType == poplar::HALF ? 8 : 4;

  // Check constraints:
  if (_innerDim % outputDim) {
    std::cerr << "Inner dimension must be a multiple of the output dimension\n";
    return false;
  }
  if (_innerDim % grainSize) {
    std::cerr
        << "Inner dimension must be a multiple of 8 (half) or 4 (float)\n";
    return false;
  }
  if (outputDim % grainSize) {
    std::cerr
        << "Output dimension must be a multiple of 8 (half) or 4 (float)\n";
    return false;
  }

  // Claim enough space for floats
  std::vector<char> data(innerDim * outerDim * 4);
  std::vector<float> nums(innerDim * outerDim);
  std::vector<int> int_data(innerDim * outerDim);
  for (unsigned i = 0; i < outerDim; ++i) {
    for (unsigned j = 0; j < innerDim; ++j) {
      nums[(i * innerDim) + j] = i + j;
      int_data[(i * innerDim + j)] = i + j;
    }
  }

  copy(target, nums.data(), innerDim * outerDim, partialsType, data.data());
  std::vector<float> answers(innerDim, initialValue);
  std::vector<char> ans_data((innerDim)*4);
  copy(target, answers.data(), innerDim, outType, ans_data.data());

  Sequence prog;

  auto cs = graph.addComputeSet("cs");

  Tensor partials;
  partials = graph.addVariable(partialsType, {outerDim, innerDim});
  Tensor out;
  out = graph.addVariable(outType, {outputDim});

  const auto vertexClass =
      templateVertex(scale == 1.0f ? "popops::ReducePartialsEqualSize"
                                   : "popops::ScaledReducePartialsEqualSize",
                     "popops::" + getReductionVertexOpName(op), partialsType,
                     outType, isUpdate);

  auto v1 = graph.addVertex(cs, vertexClass);
  std::vector<Interval> inputSlices(outerDim);
  for (unsigned i = 0; i < outerDim; i++) {
    inputSlices[i] = {i * (innerDim), i * (innerDim) + innerDim - pad};
  }
  graph.connect(v1["partials"], partials.flatten().slices(inputSlices));
  graph.connect(v1["out"], out);

  graph.setInitialValue(v1["outCount"], outputDim / grainSize);
  graph.setInitialValue(v1["partialsSizeM1"], (innerDim / outputDim) - 1);

  auto scaleTensor = graph.addVariable(FLOAT, {});
  graph.setTileMapping(scaleTensor, 0);
  graph.setInitialValue(scaleTensor, scale);
  if (scale != 1.0f) {
    graph.connect(v1["k"], scaleTensor.reshape({1}));
  }
  graph.setTileMapping(v1, 0);
  graph.setTileMapping(partials, 0);
  graph.setTileMapping(out, 0);

  graph.createHostWrite("partials", partials);
  graph.createHostWrite("outw", out);
  graph.createHostRead("out", out);

  prog.add(Execute(cs));

  Engine e(graph, prog, options);
  auto outSize = out.numElements() * target.getTypeSize(outType);

  device.bind([&](const Device &d) {
    e.load(d);
    if (outType == FLOAT || outType == HALF) {
      e.writeTensor("partials", data.data(),
                    data.data() + partials.numElements() *
                                      target.getTypeSize(partialsType));
      e.writeTensor("outw", ans_data.data(), ans_data.data() + outSize);
    } else if (outType == INT) {
      e.writeTensor("partials", int_data.data(),
                    int_data.data() + partials.numElements() *
                                          target.getTypeSize(partialsType));
      e.writeTensor("outw", ans_data.data(), ans_data.data() + outSize);
    }
    e.readTensor("out", ans_data.data(), ans_data.data() + outSize);

    e.run();

    e.readTensor("out", ans_data.data(), ans_data.data() + outSize);
  });

  copy(target, partialsType, data.data(), nums.data(), innerDim * outerDim);

  copy(target, outType, ans_data.data(), answers.data(), outputDim);
  copy(target, outType, ans_data.data(), int_data.data(), outputDim);

  MultiArray<float> input{(outerDim * innerDim - pad) / outputDim, outputDim};
  for (unsigned i = 0; i < input.numElements(); i++) {
    const unsigned row = i / (innerDim - pad);
    const unsigned column = i % (innerDim - pad);
    input.data()[i] = nums[row * innerDim + column];
  }

  auto result = reduce(input, {0}, op);

  std::vector<float> correct_answer(outputDim, initialValue);
  for (unsigned i = 0; i < outputDim; i++) {
    if (isUpdate) {
      correct_answer[i] += result[i] * scale;
    } else {
      correct_answer[i] = result[i] * scale;
    }
  }
  bool success = true;

  if (outType == FLOAT || outType == HALF) {
    CHECK_ELEMWISE_EQ(success, correct_answer, answers, outputDim);
    for (unsigned i = 0; i < outputDim; ++i) {
      answers[i] = 0; // zero for next iteration
    }
  } else if (outType == INT) {
    CHECK_ELEMWISE_EQ(success, correct_answer, int_data, outputDim);
  } else {
    success = false;
  }
  if (!success) {
    std::cerr << "nums = " << nums << '\n';
    std::cerr << "scale = " << scale << '\n';
    if (isUpdate) {
      std::cerr << "result = " << result << '\n';
      std::cerr << "initialValue = " << initialValue << '\n';
    }
  }
  return success;
}

int main(int argc, char **argv) {
  namespace po = boost::program_options;

  popops::Operation op = popops::Operation::ADD;
  DeviceType deviceType;
  Type partialsType;
  Type outType;
  float scale = 2.0f;
  bool isUpdate = true;
  unsigned outerDim, innerDim, outputDim;
  po::options_description desc("Options");
  // clang-format off
  desc.add_options()
    ("help", "Print help")
    ("device-type",
     po::value<DeviceType>(&deviceType)->required(),
     "Device Type")
    ("partials-type",
     po::value<Type>(&partialsType)->required(),
     "Partials Type")
    ("out-type",
     po::value<Type>(&outType)->required(),
     "Output type")
    ("operation",
     po::value(&op),
     "operation:ADD SQUARE_ADD MAX MIN MUL LOGICAL_OR or LOGICAL_AND")
    ("update",
     po::value<bool>(&isUpdate)->default_value(isUpdate),
     "reduce with update")
    ("scale",
     po::value<float>(&scale)->default_value(scale),
     "scale")
    ("outer-dim",
     po::value<unsigned>(&outerDim)->required(),
     "Outer dimension")
    ("inner-dim",
     po::value<unsigned>(&innerDim)->required(),
     "Inner dimension")
    ("output-dim",
     po::value<unsigned>(&outputDim)->required(),
     "Output dimension");
  // clang-format on
  po::variables_map vm;
  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    if (vm.count("help")) {
      std::cout << desc << "\n\n";
      return 1;
    }
    po::notify(vm);
  } catch (std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }

  if (!doTest(deviceType, partialsType, outType, outerDim, innerDim, outputDim,
              op, scale, isUpdate))
    return 1;
  return 0;
}
