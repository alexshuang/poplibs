#include <poplar/Engine.hpp>
#include "TestDevice.hpp"
// codelets
#include "popops/codelets.hpp"
#include "poputil/VertexTemplates.hpp"
#include "poplibs_test/Util.hpp"
#include "poplar/Target.hpp"
#include <popops/Reduce.hpp>
#include <boost/program_options.hpp>
#include <poplibs_test/Reduce.hpp>
#include <string.h>
#include <stdexcept>

#include <boost/program_options.hpp>

using namespace poplar;
using namespace poplar::program;
using namespace popops;
using namespace poputil;
using namespace poplibs_test::util;
using namespace poplibs_test::reduce;

std::string inline getReductionVertexOpName(popops::Operation op) {
  switch (op) {
    case popops::Operation::ADD:
      return "ReduceAdd";
    case popops::Operation::SQUARE_ADD:
      return "ReduceSquareAdd";
    case popops::Operation::MUL:
      return "ReduceMul";
    case popops::Operation::MIN:
      return "ReduceMin";
    case popops::Operation::MAX:
      return "ReduceMax";
    case popops::Operation::LOGICAL_AND:
      return "ReduceAnd";
    case popops::Operation::LOGICAL_OR:
      return "ReduceOr";
  }
}

#define CHECK_IF(result, cond) \
  do { \
    if (!(cond)) { \
      std::cerr << "Condition failed: " << #cond << '\n'; \
      result = false; \
    } \
  } while(false)

static bool doTest(const DeviceType &deviceType,
                   const Type &partialsType,
                   const Type &outType,
                   const unsigned outerDim,
                   const unsigned innerDim,
                   const popops::Operation op,
                   const float scale,
                   bool isUpdate) {
  auto device = createTestDevice(deviceType);
  auto &target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  const float initialValue = 3.0;

  // Claim enough space for floats
  std::vector<char> data(innerDim * outerDim * 4);
  std::vector<float> nums(innerDim * outerDim);
  std::vector<int> int_data(innerDim * outerDim);
  for (unsigned i = 0; i < outerDim; ++i) {
    for (unsigned j = 0; j < innerDim; ++j) {
      nums[(i * innerDim) + j] = i + j;
      int_data[(i * innerDim) + j] = i + j;
    }
  }

  copy(target, nums.data(), innerDim * outerDim, partialsType, data.data());
  std::vector<float> answers(outerDim, initialValue);
  std::vector<char> ans_data((outerDim) * 4);
  copy(target, answers.data(), outerDim, outType, ans_data.data());

  Sequence prog;

  auto cs = graph.addComputeSet("cs");

  Tensor partials;
  partials = graph.addVariable(partialsType, {outerDim, innerDim});
  Tensor out;
  out = graph.addVariable(outType, {outerDim});

  const auto vertexClass = templateVertex(
                           scale == 1.0f ? "popops::ContinuousReduce" :
                                           "popops::ScaledContinuousReduce",
                           "popops::" + getReductionVertexOpName(op),
                           partialsType, outType, isUpdate);

  auto v1 = graph.addVertex(cs, vertexClass);

  graph.connect(v1["partials"], partials.flatten());
  graph.connect(v1["out"], out);

  graph.setInitialValue(v1["numOutputs"], outerDim);
  graph.setInitialValue(v1["numPartials"], innerDim);

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

  Engine e(graph, prog);
  auto outSize = out.numElements() * target.getTypeSize(outType);

  device.bind([&](const Device &d) {
    e.load(d);
    if (outType == FLOAT || outType == HALF) {
      e.writeTensor("partials", data.data(), data.data() +
                  partials.numElements() * target.getTypeSize(partialsType));
      e.writeTensor("outw", ans_data.data(), ans_data.data() + outSize);
    } else if (outType == INT) {
      e.writeTensor("partials", int_data.data(), int_data.data() +
                    partials.numElements() * target.getTypeSize(partialsType));
      e.writeTensor("outw", ans_data.data(), ans_data.data() + outSize);
    }
    e.readTensor("out", ans_data.data(), ans_data.data() + outSize);

    e.run();

    e.readTensor("out", ans_data.data(), ans_data.data() + outSize);
  });

  copy(target, partialsType, data.data(), nums.data(), innerDim * outerDim);

  copy(target, outType, ans_data.data(), answers.data(), outerDim);
  copy(target, outType, ans_data.data(), int_data.data(), outerDim);

  bool success = true;

  ReferenceTensor<float> input;
  input.shape = {outerDim, innerDim};
  input.values = nums;
  auto result = reduce(input, {1}, op);

  std::vector<float> correct_answer (outerDim, initialValue);
  for (unsigned i = 0; i <outerDim; i++) {
    if (isUpdate) {
      correct_answer[i] += result.values[i] * scale;
    } else {
      correct_answer[i] = result.values[i] * scale;
    }
  }
  if (outType == FLOAT || outType == HALF) {
    for(unsigned i = 0; i < outerDim; ++i){
      CHECK_IF(success, correct_answer[i] == answers[i]);
      answers[i] = 0; // zero for next iteration
    }
  } else if (outType == INT) {
    for(unsigned i = 0; i < outerDim; ++i) {
      CHECK_IF(success, correct_answer[i] == int_data[i]);
    }
  } else {
    success = false;
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
  unsigned outerDim, innerDim;
  po::options_description desc("Options");
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
     "Output Type")
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
     "Inner dimension");
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

  if (!doTest(deviceType, partialsType, outType, outerDim, innerDim,
              op, scale, isUpdate))
    return 1;
  return 0;
}
