// Copyright (c) 2020 Graphcore Ltd. All rights reserved.
#include <poplibs_support/LogArithmetic.hpp>
#include <poplibs_support/TestDevice.hpp>
#include <poplibs_test/CTCLoss.hpp>
#include <poplibs_test/CTCUtil.hpp>
#include <poplibs_test/Embedding.hpp>
#include <poplibs_test/MatrixTransforms.hpp>
#include <poplibs_test/Util.hpp>
#include <popnn/CTCLoss.hpp>
#include <popnn/codelets.hpp>
#include <popops/codelets.hpp>
#include <poputil/VertexTemplates.hpp>
#include <poputil/exceptions.hpp>

#include <poplar/Engine.hpp>
#include <poplar/Graph.hpp>
#include <poplar/IPUModel.hpp>

#include <boost/multi_array.hpp>
#include <boost/optional/optional_io.hpp>
#include <boost/program_options.hpp>
#include <boost/test/tools/floating_point_comparison.hpp>

#include <fstream>
#include <iomanip>
#include <random>

namespace po = boost::program_options;

using namespace poplar;
using namespace poplar::program;
using namespace poplibs_test::ctc;
using namespace poplibs_test;
using namespace poplibs_test::matrix;
using namespace poplibs_test::util;
using namespace poplibs_support;
using namespace poputil;

// Default tolerances used in tests
#define FLOAT_REL_TOL 0.04
#define HALF_REL_TOL 0.1
#define FLOAT_ABS_TOL 1e-3
#define HALF_ABS_TOL 1e-2

// The result returned by the codelet / reduction stages can be checked more
// precisely for all tests - especially those with larger lengths
#define CODELET_TEST_FLOAT_REL_TOL 0.01
#define CODELET_TEST_HALF_REL_TOL 0.1
#define CODELET_TEST_FLOAT_ABS_TOL 1e-6
#define CODELET_TEST_HALF_ABS_TOL 1e-5

// Mask results that aren't valid due to the time of the batch entry
boost::multi_array<double, 2>
maskResults(const boost::multi_array<double, 2> &in, size_t validTimesteps) {
  auto out = in;
  const auto timeSteps = out.shape()[0];
  const auto symbols = out.shape()[1];
  for (unsigned sym = 0; sym < symbols; sym++) {
    for (unsigned t = validTimesteps; t < timeSteps; t++) {
      out[t][sym] = 0;
    }
  }
  return out;
}
// Print a sequence, inserting `-` for the blank symbol
void print(const std::string &prefix, const std::vector<unsigned> &symbols,
           unsigned blank, bool verbose = true) {
  if (!verbose) {
    return;
  }
  std::cout << prefix << " ";
  for (auto &i : symbols) {
    if (i == blank) {
      std::cout << "- ";
    } else {
      std::cout << i << " ";
    }
  }
  std::cout << "\n";
}

// Print the matrix `in`
template <typename FPType>
void print(const std::string &prefix, const boost::multi_array<FPType, 2> &in,
           unsigned blank, bool verbose = true) {
  if (!verbose) {
    return;
  }
  std::cout << prefix << "\n        ";
  for (unsigned i = 0; i < in.size(); i++) {
    std::cout << std::setw(11) << (std::string{"t"} + std::to_string(i));
  }

  for (unsigned i = 0; i < in[0].size(); i++) {
    if (i == blank) {
      std::cout << "\nIndex:-  ";
    } else {
      std::cout << "\nIndex:" << i << "  ";
    }
    for (unsigned j = 0; j < in.size(); j++) {
      std::cout << std::setw(10) << std::setprecision(4) << in[j][i] << ",";
    }
  }
  std::cout << "\n\n";
}

// Struct and function to return the test inputs
template <typename FPType> struct InputSequence {
  // Input, always of max size (time) but with only inputLength valid values,
  // the rest padded as blank
  boost::multi_array<FPType, 2> input;
  unsigned inputLength;
  // Labels, of the randomly chosen size for this batch
  std::vector<unsigned> labels;
  bool isLogits;
};

template <typename FPType>
std::pair<FPType, boost::multi_array<FPType, 2>>
gradReference(const InputSequence<FPType> &test_, unsigned blankClass,
              unsigned numClasses, bool testReducedCodeletGradient,
              bool verbose) {
  auto test = test_;
  if (test.isLogits) { // Convert to log probs
    test.input = log::log(transpose(log::softMax(transpose(test.input))));
  }
  auto paddedSequence = extendedLabels(test.labels, blankClass);
  auto in = transpose(test.input);
  boost::multi_array<FPType, 2> logSequence(
      boost::extents[paddedSequence.size()][in.shape()[1]]);
  poplibs_test::embedding::multiSlice(in, paddedSequence, logSequence);

  auto alphaLog =
      alpha(logSequence, paddedSequence, blankClass, test.inputLength);
  auto betaLog =
      beta(logSequence, paddedSequence, blankClass, test.inputLength);
  auto expandedGradient =
      expandedGrad(logSequence, alphaLog, betaLog, paddedSequence, blankClass,
                   test.inputLength);
  auto negLogLoss =
      loss(logSequence, paddedSequence, blankClass, test.inputLength);
  auto gradient =
      grad(logSequence, in, alphaLog, betaLog, paddedSequence, numClasses,
           blankClass, test.inputLength, testReducedCodeletGradient);

  return {negLogLoss, transpose(gradient)};
}

std::vector<std::pair<double, boost::multi_array<double, 2>>>
gradIPU(const std::vector<InputSequence<double>> &inputs, unsigned maxLabels,
        unsigned blankSymbol, std::size_t numClasses, Type inType, Type outType,
        OptionFlags planOpts, OptionFlags debugOpts,
        const DeviceType &deviceType, boost::optional<unsigned> tiles,
        bool ignoreData, bool profile,
        boost::optional<std::string> &profileDir) {

  auto device = createTestDevice(deviceType, 1, tiles);
  const auto &target = device.getTarget();
  Graph graph(target);
  popnn::addCodelets(graph);
  popops::addCodelets(graph);

  const auto maxT = inputs[0].input.shape()[0];
  const auto batchSize = inputs.size();

  // Create the inputs to the gradient function
  const auto plan = popnn::ctc::plan(graph, inType, outType, batchSize, maxT,
                                     maxLabels, numClasses, planOpts);

  auto data = popnn::ctc::createDataInput(graph, inType, batchSize, maxT,
                                          numClasses, plan, "DataInput");
  auto labels = popnn::ctc::createLabelsInput(graph, UNSIGNED_INT, batchSize,
                                              maxLabels, plan, "LabelsInput");

  auto dataLengths = graph.addVariable(UNSIGNED_INT, {batchSize});
  auto labelLengths = graph.addVariable(UNSIGNED_INT, {batchSize});
  graph.setTileMapping(dataLengths, 0);
  graph.setTileMapping(labelLengths, 0);

  // Write the inputs
  Sequence uploadProg, downloadProg;
  std::vector<std::pair<std::string, char *>> tmap;
  std::unique_ptr<char[]> rawDataLengths, rawLabelLengths;
  std::vector<std::unique_ptr<char[]>> rawData(batchSize), rawLabels(batchSize);
  rawDataLengths = allocateHostMemoryForTensor(
      dataLengths, "dataLengths", graph, uploadProg, downloadProg, tmap);
  rawLabelLengths = allocateHostMemoryForTensor(
      labelLengths, "labelLengths", graph, uploadProg, downloadProg, tmap);

  for (unsigned i = 0; i < batchSize; i++) {
    rawData[i] = allocateHostMemoryForTensor(data.slice(i, i + 1, 1),
                                             "data_" + std::to_string(i), graph,
                                             uploadProg, downloadProg, tmap);
    rawLabels[i] = allocateHostMemoryForTensor(
        labels.slice(i, i + 1, 0), "labels_" + std::to_string(i), graph,
        uploadProg, downloadProg, tmap);
    if (!ignoreData) {
      copy(target, inputs[i].input, inType, rawData[i].get());
      copy(target, inputs[i].labels, labels.elementType(), rawLabels[i].get());
    }
  }
  std::vector<unsigned> initLabelLengths(batchSize), initDataLengths(batchSize);
  for (unsigned i = 0; i < batchSize; i++) {
    initLabelLengths[i] = inputs[i].labels.size();
    initDataLengths[i] = inputs[i].inputLength;
  }
  copy(target, initLabelLengths, labelLengths.elementType(),
       rawLabelLengths.get());
  copy(target, initDataLengths, dataLengths.elementType(),
       rawDataLengths.get());

  // Create gradient
  Sequence prog;
  const auto result = [&]() {
    const auto layer = "ctc_loss";
    if (inputs[0].isLogits) {
      return popnn::ctc::calcLossAndGradientLogits(
          graph, outType, data, labels, dataLengths, labelLengths, prog,
          blankSymbol, plan, layer, debugOpts);
    } else {
      return popnn::ctc::calcLossAndGradientLogProbabilities(
          graph, outType, data, labels, dataLengths, labelLengths, prog,
          blankSymbol, plan, layer, debugOpts);
    }
  }();

  auto lossResult = result.first;
  auto gradResult = result.second;
  // Create handles for reading the result
  std::vector<std::unique_ptr<char[]>> rawLossResult(batchSize);
  std::vector<std::unique_ptr<char[]>> rawGradResult(batchSize);
  if (!ignoreData) {
    for (unsigned i = 0; i < batchSize; i++) {
      rawLossResult[i] = allocateHostMemoryForTensor(
          lossResult.slice(i, i + 1, 0), "result_loss_" + std::to_string(i),
          graph, uploadProg, downloadProg, tmap);
      rawGradResult[i] = allocateHostMemoryForTensor(
          gradResult.slice(i, i + 1, 1), "result_grad_" + std::to_string(i),
          graph, uploadProg, downloadProg, tmap);
    }
  }

  // Run input, gradient, output
  OptionFlags engineOptions;
  if (profile || profileDir) {
    engineOptions.set("debug.instrumentCompute", "true");
    if (profileDir) {
      engineOptions.set("autoReport.all", "true");
      engineOptions.set("autoReport.directory", *profileDir);
    }
  }

  Sequence s = [&]() {
    if (ignoreData) {
      // Because the input data has constraints of what is valid, we can't
      // ignore the uploadProg without reasonable likelihood of encountering an
      // exception or unexpected behaviour.
      return Sequence{uploadProg, prog};
    } else {
      return Sequence{uploadProg, prog, downloadProg};
    }
  }();
  Engine engine(graph, s, engineOptions);
  attachStreams(engine, tmap);
  device.bind([&](const Device &d) {
    engine.load(d);
    engine.run();
  });

  // Fetch the result
  std::vector<std::pair<double, boost::multi_array<double, 2>>> output(
      batchSize);
  if (!ignoreData) {
    for (unsigned i = 0; i < batchSize; i++) {
      copy(target, outType, rawLossResult[i].get(), &output[i].first, 1);
      output[i].second.resize(boost::extents[maxT][numClasses]);
      copy(target, outType, rawGradResult[i].get(), output[i].second);
    }
  }

  if (profile && deviceType != DeviceType::Cpu) {
    engine.printProfileSummary(std::cout,
                               OptionFlags{{"showExecutionSteps", "true"}});
  }
  return output;
}

int main(int argc, char **argv) {
  // Default input parameters.
  DeviceType deviceType = DeviceType::IpuModel2;
  boost::optional<std::string> profileDir;
  boost::optional<std::string> planConstraints;
  boost::optional<unsigned> minRandomTime = boost::none;
  boost::optional<unsigned> fixedTime = boost::none;
  unsigned maxTime = 15;
  boost::optional<unsigned> minRandomLabelLength = boost::none;
  boost::optional<unsigned> fixedLabelLength = boost::none;
  unsigned maxLabelLength = 5;
  unsigned blankClass = 0;
  unsigned numClasses = 4;
  unsigned batchSize = 1;
  boost::optional<unsigned> tiles = boost::none;
  Type inType = FLOAT;
  Type partialsType = FLOAT;
  Type outType = FLOAT;
  bool isLogits = true;
  bool testReducedCodeletGradient = false;

  po::options_description desc("Options");
  // clang-format off
  desc.add_options()
    ("help", "Produce help message")
    ("device-type",
     po::value<DeviceType>(&deviceType)->default_value(deviceType),
     deviceTypeHelp)
    ("tiles-per-ipu",po::value<boost::optional<unsigned>>(&tiles),
      "Number of tiles per IPU")
    ("profile", "Show profile report")
    ("profile-dir",
     po::value<decltype(profileDir)>(&profileDir)
      ->default_value(boost::none),
     "Write profile files to the specified directory.")
    ("plan-constraints", po::value(&planConstraints),
     "JSON constraints for planner, e.g. {\"parallel\": {\"batch\": 1}}")
    ("in-type", po::value(&inType)->default_value(inType),
     "Input data type")
    ("partials-type", po::value(&partialsType)->default_value(partialsType),
     "Input data type")
    ("out-type", po::value(&outType)->default_value(outType),
     "Output data type")
    ("batch", po::value(&batchSize)->default_value(batchSize),
     "Batch size")
    ("label-length", po::value(&fixedLabelLength),
     "If set, forces every label to be of length `label-length`")
    ("min-label-length",
      po::value(&minRandomLabelLength),
     "If set, minimum randomly generated label length")
    ("max-label-length",
      po::value(&maxLabelLength)->default_value(maxLabelLength),
     "Max test length (labels)")
    ("time", po::value(&fixedTime),
     "If set, forces every sequence to be of length `time`")
    ("min-time", po::value(&minRandomTime),
     "If set, minimum randomly generated time length")
    ("max-time", po::value(&maxTime)->default_value(maxTime),
     "Max test length (time)")
    ("blank-class", po::value(&blankClass)->default_value(blankClass),
     "Index of the blank symbol. Range 0 to (num-classes-1)")
    ("num-classes", po::value(&numClasses)->default_value(numClasses),
     "Classes in the alphabet including blank")
    ("ignore-data", "Ignore data, to check execution time")
    ("logit-inputs", po::value(&isLogits)->default_value(isLogits),
     "pass logit inputs to ctc loss api, otherwise convert to logProbs prior")
    ("test-reduced-codelet-result",
        po::value(&testReducedCodeletGradient)->
            default_value(testReducedCodeletGradient),
     "Test the reduced result: alpha * beta / probability, omitting any further"
     " processing")
    ("disable-always-satisfiable-error", "Disable the check when validating time"
    " and labelLength before generating random labels. This check ensures that the"
    " label is always representable for given t and labelLength."
    "\nThe length of t required to represent a given label depends on the number"
    " of duplicate classes in the label, this check assumes the worst case where"
    " every class is a duplicate."
    "\nSpecifically:"
    "\n  2 * t - 1 >= labelLength")
    ("plan-only", "Only plan the requested passes, don't build or run a graph")
    ("verbose", "Provide debug printout");
  // clang-format on

  po::variables_map vm;
  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    if (vm.count("help")) {
      std::cout << desc << "\n\n";
      return 1;
    }

  } catch (std::exception &e) {
    std::cerr << "error parsing command line: " << e.what() << "\n";
    return 1;
  }

  const bool profile = vm.count("profile");
  const bool verbose = vm.count("verbose");
  const bool ignoreData = vm.count("ignore-data");
  const bool planOnly = vm.count("plan-only");
  const bool disableAlwaysSatisfiableError =
      vm.count("disable-always-satisfiable-error");

  // Needed to set default arguments.
  po::notify(vm);
  // Pick up on some parameters that are easy to get wrong
  if (blankClass >= numClasses) {
    throw poputil::poplibs_error("The blank class must be in the range 0 to "
                                 "(number of classes - 1)");
  }
  if (!minRandomTime && !fixedTime) {
    fixedTime = maxTime;
  }
  if (!minRandomLabelLength && !fixedLabelLength) {
    fixedLabelLength = maxLabelLength;
  }
  validateTimeAndLabelBounds(minRandomTime, fixedTime, maxTime,
                             minRandomLabelLength, fixedLabelLength,
                             maxLabelLength);

  poplar::OptionFlags planOpts;
  if (planConstraints) {
    planOpts.set("planConstraints", *planConstraints);
  }
  planOpts.set("partialsType", partialsType.toString());

  poplar::OptionFlags debugOpts;
  if (testReducedCodeletGradient) {
    debugOpts.set("returnReducedCodeletGradient", "true");
  }

  if (planOnly) {
    auto device = createTestDevice(deviceType, 1, tiles);
    const auto &target = device.getTarget();
    Graph graph(target);

    const auto plan =
        popnn::ctc::plan(graph, inType, outType, batchSize, maxTime,
                         maxLabelLength, numClasses, planOpts);

    std::cout << plan << std::endl;
    std::cout << "No test run - plan only" << std::endl;
    return 0;
  }

  RandomUtil rand{42};

  // For test call the reference function for each batch input
  std::vector<InputSequence<double>> tests;
  std::vector<std::pair<double, boost::multi_array<double, 2>>> references;
  for (unsigned i = 0; i < batchSize; i++) {
    const auto [t, labelLength] = getRandomSize(
        minRandomTime, fixedTime, maxTime, minRandomLabelLength,
        fixedLabelLength, maxLabelLength, disableAlwaysSatisfiableError, rand);
    auto [input, label] = getRandomTestInput<double>(
        t, maxTime, labelLength, numClasses, blankClass, isLogits, rand);
    tests.push_back(InputSequence<double>{input, t, label, isLogits});

    if (verbose) {
      std::cout << "\nBatch:" << i << " Time:" << tests[i].inputLength
                << " Label length:" << tests[i].labels.size();
    }
    print(" Test sequence[" + std::to_string(tests[i].labels.size()) + "] ",
          tests[i].labels, blankClass, verbose);
    if (tests[i].isLogits) {
      print("Logits in", tests[i].input, blankClass, verbose);
      print("Log Softmax in",
            log::log(transpose(log::softMax(transpose(tests[i].input)))),
            blankClass, verbose);
    } else {
      print("Log Softmax in", tests[i].input, blankClass, verbose);
    }
    if (!ignoreData) {
      references.push_back(
          gradReference<double>(tests[i], blankClass, numClasses,
                                testReducedCodeletGradient, verbose));
      references.back().second =
          maskResults(references.back().second, tests[i].inputLength);
      if (verbose) {
        std::cout << "Reference loss = " << references.back().first << "\n";
      }
    }
  }
  auto outputs = gradIPU(tests, maxLabelLength, blankClass, numClasses, inType,
                         outType, planOpts, debugOpts, deviceType, tiles,
                         ignoreData, profile, profileDir);

  for (unsigned i = 0; i < batchSize; i++) {
    outputs[i].second = maskResults(outputs[i].second, tests[i].inputLength);
    if (verbose) {
      std::cout << "Result loss = " << outputs[i].first << "\n";
    }
    print("Result gradient, batch:" + std::to_string(i), outputs[i].second,
          blankClass, verbose);
  }
  double relativeTolerance, absoluteTolerance;
  if (testReducedCodeletGradient) {
    relativeTolerance = outType == FLOAT ? CODELET_TEST_FLOAT_REL_TOL
                                         : CODELET_TEST_HALF_REL_TOL;
    absoluteTolerance = outType == FLOAT ? CODELET_TEST_FLOAT_ABS_TOL
                                         : CODELET_TEST_HALF_ABS_TOL;
  } else {
    relativeTolerance = outType == FLOAT ? FLOAT_REL_TOL : HALF_REL_TOL;
    absoluteTolerance = outType == FLOAT ? FLOAT_ABS_TOL : HALF_ABS_TOL;
  }
  bool success = true;
  if (!ignoreData) {
    for (unsigned i = 0; i < batchSize; i++) {
      bool batchSuccess = checkIsClose(outputs[i].first, references[i].first,
                                       relativeTolerance) &&
                          checkIsClose("Batch:" + std::to_string(i) + " result",
                                       outputs[i].second, references[i].second,
                                       relativeTolerance, absoluteTolerance);
      if (!batchSuccess) {
        success = false;
      }
    }
  }
  if (!success) {
    std::cerr << "Data mismatch\n";
  }
  return !success;
}
