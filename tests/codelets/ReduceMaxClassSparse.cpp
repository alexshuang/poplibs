#include <poplar/Engine.hpp>
#include "TestDevice.hpp"

#include "popnn/codelets.hpp"
#include "poputil/VertexTemplates.hpp"
#include "poplibs_test/Util.hpp"
#include "poplibs_support/Algorithm.hpp"

#include <cstdint>
#include <boost/program_options.hpp>
#include <boost/random.hpp>

using namespace poplar;
using namespace poplar::program;
using namespace poplibs_test::util;
using namespace poputil;

namespace {

static void modelVertex(
    const std::vector<double> &activations,
    const std::vector<std::uint64_t> &labels,
    double &maxAct,
    std::uint64_t &maxIndex) {
  std::uint64_t maxI = 0;
  double maxV = activations[maxI];
  for (std::size_t i = 1; i < activations.size(); ++i) {
    if (activations[i] > maxV) {
      maxV = activations[i];
      maxI = i;
    }
  }
  maxAct = maxV;
  maxIndex = labels[maxI];
}

} // end anonymous namespace


static bool doTest(const DeviceType &deviceType,
                   const Type &labelType,
                   unsigned size) {
  auto device = createTestDevice(deviceType);
  auto &target = device.getTarget();
  Graph graph(device);
  popnn::addCodelets(graph);

  auto activations =
    graph.addVariable(FLOAT, {size}, "activations");
  auto labels =
    graph.addVariable(labelType, {size}, "labels");
  auto maxAct =
    graph.addVariable(FLOAT, {}, "maxValuePartials");
  auto maxIndex =
    graph.addVariable(labelType, {}, "maxIndexPartials");
  graph.setTileMapping(activations, 0);
  graph.setTileMapping(labels, 0);
  graph.setTileMapping(maxAct, 0);
  graph.setTileMapping(maxIndex, 0);

  Sequence uploadProg, downloadProg;
  std::vector<std::pair<std::string, char*>> tmap;
  auto rawHostActivations =
    allocateHostMemoryForTensor(activations, "activations", graph, uploadProg,
                                downloadProg, tmap);
  auto rawHostLabels =
    allocateHostMemoryForTensor(labels, "labels", graph, uploadProg,
                                downloadProg, tmap);
  auto rawHostMaxAct =
    allocateHostMemoryForTensor(maxAct, "maxValuePartial", graph, uploadProg,
                                downloadProg, tmap);
  auto rawHostMaxIndex =
    allocateHostMemoryForTensor(maxIndex, "maxIndexPartial", graph, uploadProg,
                                downloadProg, tmap);

  // TODO: Embed this data rather than generating on the fly
  std::mt19937 randomEngine;
  std::vector<double> hostActivations(size);
  { boost::random::uniform_real_distribution<float> randDist;
    for (auto &a : hostActivations) {
      a = randDist(randomEngine);
    }
  }
  copy(target, hostActivations.data(), size, FLOAT, rawHostActivations.get());

  std::vector<std::uint64_t> hostLabels(size);
  { boost::random::uniform_int_distribution<std::uint64_t> randDist(0, 500);
    for (auto &l : hostLabels) {
      l = randDist(randomEngine);
    }
  }
  copy(target, hostLabels.data(), size, labelType, rawHostLabels.get());

  auto cs = graph.addComputeSet();
  auto v = graph.addVertex(cs, templateVertex("popnn::ReduceMaxClassSparse",
                                              labelType));
  graph.setTileMapping(v, 0);

  graph.connect(v["activations"], activations);
  graph.connect(v["labels"], labels);
  graph.connect(v["maxValue"], maxAct);
  graph.connect(v["maxIndex"], maxIndex);

  Engine e(std::move(graph), Sequence(uploadProg, Execute(cs), downloadProg));
  attachStreams(e, tmap);
  e.loadAndRun(device);

  double modelAct;
  std::uint64_t modelIndex;
  modelVertex(hostActivations, hostLabels, modelAct, modelIndex);

  double hostMaxAct;
  std::uint64_t hostMaxIndex;
  copy(target, FLOAT, rawHostMaxAct.get(),
       &hostMaxAct, 1);
  copy(target, labelType, rawHostMaxIndex.get(),
       &hostMaxIndex, 1);

  bool success = true;
  success &=
    checkIsClose("maxValue", &hostMaxAct, {1},
                 &modelAct, 1, 0.1, 1e-20);
  success &=
    checkEqual("maxIndex", &hostMaxIndex, {1},
               &modelIndex, 1);
  return success;
}


int main(int argc, char **argv) {
  namespace po = boost::program_options;

  DeviceType deviceType;
  Type labelType = UNSIGNED_INT;
  unsigned size;
  po::options_description desc("Options");
  desc.add_options()
    ("help", "Print help")
    ("device-type",
     po::value<DeviceType>(&deviceType)->required(),
     "Device Type")
    ("size",
     po::value<unsigned>(&size)->required(),
     "Total size to process with vertex");

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

  if (!doTest(deviceType, labelType, size))
    return 1;
  return 0;
}
