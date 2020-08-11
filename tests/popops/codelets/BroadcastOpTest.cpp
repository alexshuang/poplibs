// Copyright (c) 2018 Graphcore Ltd. All rights reserved.
#define BOOST_TEST_MODULE BroadcastOpTest
// Test for the broadcastOp vertex operations.
// Used to verify aspects of implementation that
// aren't simply to correctness of arithmetic on a single item. Also
// for benchmarking.
// Eg - different length vectors for Supervisor vertices or other
// vectorised implementations, where data quantity is important.
//
#include <poplar/Engine.hpp>
#include <poplibs_support/TestDevice.hpp>
#include <popops/Zero.hpp>

#include "poputil/VertexTemplates.hpp"

#include "../lib/popops/ExprOpUtil.hpp"
#include "popops/ElementWise.hpp"
#include <poplibs_test/Util.hpp>
#include <popops/codelets.hpp>
#include <poputil/TileMapping.hpp>

#include <boost/program_options.hpp>
#include <iostream>

using namespace poplar;
using namespace poplar::program;
using namespace poputil;
using namespace poplibs_test::util;
using namespace popops;
using namespace poplibs_support;

const poplar::OptionFlags options{{"debug.instrumentCompute", "true"}};

//*************************************************
bool doBroadcastOpTest(const DeviceType &deviceType, const Type &dataType,
                       unsigned rows, unsigned columns,
                       expr::BinaryOpType operation, bool testSupervisor,
                       unsigned bElems, bool inPlace, bool divideByRow,
                       const std::function<double(double, double)> &hostFn,
                       bool doCheck, bool doReport, int in1Offset,
                       int outOffset) {

  // Whole data array size, with some padding to check for overwrite.
  // Avoid using extra columns as that will affect the alignment of other
  // rows which matters, especially in the supervisor cases, so simply add
  // a pad row.
  auto total_elems = (rows + 1) * columns;

  // Program generated test data
  std::vector<double> outTest(total_elems);
  std::vector<double> inTest(total_elems);
  std::vector<double> BTest(bElems);

  // Initialise input patterns
  for (unsigned i = 0; i < total_elems; i++) {
    inTest[i] = static_cast<double>(i) + 1;
    outTest[i] = static_cast<double>(i) + 1;
  }

  double k = 4;
  for (unsigned i = 0; i < BTest.size(); i++) {
    BTest[i] = static_cast<double>(i) + k;
  }

  // Create Graph object, target and device
  auto device = createTestDevice(deviceType);
  Target target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  Tensor inOut;
  if (in1Offset != 0 || outOffset != 0) {
    const auto regionSize = std::max(in1Offset, outOffset) + total_elems;
    inOut = graph.addVariable(dataType, {regionSize}, "Whole input region");
  } else {
    inOut =
        graph.addVariable(dataType, {2 * total_elems}, "Whole input region");
  }
  graph.setTileMapping(inOut, 0);

  if (in1Offset == 0 && outOffset == 0) {
    outOffset = total_elems;
  }
  if (unsigned(std::abs(in1Offset - outOffset)) < total_elems) {
    std::cerr << " Error: specified offsets produce overlapping data"
                 " (includes 1 pad row)\n";
    return false;
  }

  Tensor in;
  if (testSupervisor) {
    in = inOut.slice(in1Offset, in1Offset + total_elems);
  } else {
    in = inOut.slice(in1Offset, in1Offset + total_elems)
             .reshape({rows + 1, columns});
  }

  graph.setTileMapping(in, 0);

  // Create B as scalar or vector, as required
  Tensor B;
  if (bElems == 1) {
    B = graph.addVariable(dataType, {}, "Constant");
  } else {
    B = graph.addVariable(dataType, {bElems}, "Constant");
  }
  graph.setTileMapping(B, 0);

  // Output Tensor, used only if not in-place
  Tensor out;
  if (!inPlace) {
    if (testSupervisor) {
      out = inOut.slice(outOffset, outOffset + total_elems);
    } else {
      out = inOut.slice(outOffset, outOffset + total_elems)
                .reshape({rows + 1, columns});
    }
    graph.setTileMapping(out, 0);
  }

  // Make a sequence to run the operation
  Sequence sequence;
  ComputeSet testComputeSet = graph.addComputeSet("computeOp");
  std::string vertexName, vertexClass;

  // There are 8 (counting the "InPlace" options) vertex variants to test,
  // named as follows::
  //
  // If 'B' has 1 element (i.e. a scalar or a 1-elem tensor):
  //   "popops::BroadcastScalar1D[InPlace]Supervisor"    :'data' is 1D
  //   "popops::BroadcastScalar2DData[InPlace]"          :'data' is 2D
  //
  // If 'B' is a vector:
  //   "popops::BroadcastVectorOuter[InPlace]Supervisor" :'data' is 2D flattened
  //   "popops::Broadcast2D[InPlace]"                    :'data' is 2D
  //
  // Having selected the VectorOuter case, there are 4 possible variants
  if (testSupervisor) {
    if (bElems == 1) {
      vertexName = inPlace ? "popops::BroadcastScalar1DInPlaceSupervisor"
                           : "popops::BroadcastScalar1DSupervisor";
    } else {
      if (divideByRow) {
        vertexName = inPlace
                         ? "popops::BroadcastVectorOuterByRowInPlaceSupervisor"
                         : "popops::BroadcastVectorOuterByRowSupervisor";
      } else {
        vertexName =
            inPlace ? "popops::BroadcastVectorOuterByColumnInPlaceSupervisor"
                    : "popops::BroadcastVectorOuterByColumnSupervisor";
      }
    }
  } else {
    if (bElems == 1) {
      vertexName = inPlace ? "popops::BroadcastScalar2DDataInPlace"
                           : "popops::BroadcastScalar2DData";
    } else {
      vertexName = inPlace ? "popops::BroadcastScalar2DInPlace"
                           : "popops::BroadcastScalar2D";
    }
  }

  if (vertexName.find("VectorOuter") != std::string::npos) {
    vertexClass = templateVertex(
        vertexName, operation, dataType,
        columns % target.getVectorWidth(dataType) ? true : false);
  } else {
    vertexClass = templateVertex(vertexName, operation, dataType);
  }
  auto vertex = graph.addVertex(testComputeSet, vertexClass);
  graph.setTileMapping(vertex, 0);

  if (testSupervisor) {
    graph.connect(vertex["data"], in.slice(0, rows * columns, 0));
  } else {
    graph.connect(vertex["data"], in.slice(0, rows, 0));
  }
  if (!inPlace) {
    if (testSupervisor) {
      graph.connect(vertex["out"], out.slice(0, rows * columns, 0));
    } else {
      graph.connect(vertex["out"], out.slice(0, rows, 0));
    }
  }

  graph.connect(vertex["B"], B);

  if (vertexName.find("VectorOuter") != std::string::npos) {
    graph.setInitialValue(vertex["columns"], columns);
    graph.setInitialValue(vertex["rows"], rows);
  }

  // allocateHostMemoryForTensor
  Sequence uploadProg, downloadProg;
  std::vector<std::pair<std::string, char *>> tmap;
  auto input = allocateHostMemoryForTensor(in, "in", graph, uploadProg,
                                           downloadProg, tmap);
  auto inputB = allocateHostMemoryForTensor(B, "inB", graph, uploadProg,
                                            downloadProg, tmap);
  auto output = inPlace
                    ? NULL
                    : allocateHostMemoryForTensor(out, "out", graph, uploadProg,
                                                  downloadProg, tmap);
  sequence.add(Execute(testComputeSet));

  // If in-place, 'in' will contain the result
  graph.createHostRead("outStream", inPlace ? in : out);

  // Run sequence and compare host and IPU result
  Engine engine(graph, Sequence(uploadProg, sequence, downloadProg), options);
  attachStreams(engine, tmap);

  // Put test inputs into an array of the correct type ready to use
  copy(target, inTest.data(), inTest.size(), dataType, input.get());
  copy(target, BTest.data(), BTest.size(), dataType, inputB.get());
  if (!inPlace) {
    copy(target, outTest.data(), outTest.size(), dataType, output.get());
  }
  std::vector<double> outHost(total_elems);
  std::vector<char> outHostRaw(total_elems * 4);

  device.bind([&](const Device &d) {
    engine.load(d);
    engine.run(0);

    if (doReport) {
      OptionFlags opt;
      opt.set("showExecutionSteps", "true");

      engine.printProfileSummary(std::cerr, opt);
    }

    // Fetch the result and convert to a double for comparison
    engine.readTensor("outStream", (void *)&outHostRaw[0]);
  });

  copy(target, dataType, outHostRaw.data(), outHost.data(), outHost.size());

  // Host generated result, start with zeros, leaving the unmodified input data
  // in the padding section
  for (unsigned i = 0; i < total_elems - columns; i++) {
    outTest[i] = 0;
  }
  // Then do the operation for comparison
  unsigned bIndex = 0;
  for (unsigned i = 0; i < rows; i++) {
    for (unsigned j = 0; j < columns; j++) {
      if (bElems == 1) {
        outTest[j + i * columns] = hostFn(inTest[j + i * columns], BTest[0]);
      } else {
        outTest[j + i * columns] =
            hostFn(inTest[j + i * columns], BTest[bIndex]);
      }
    }
    bIndex++;
    if (bIndex == bElems) {
      bIndex = 0;
    }
  }
  // Check the result, in the outTest array
  if (doCheck) {
    bool check = checkIsClose("BroadcastTest", outHost.data(), {outHost.size()},
                              outTest.data(), outTest.size(), 0.01, 0.01);
    return check;
  } else {
    return true;
  }
}

bool doBroadcastOpTestCastOutput(
    const DeviceType &deviceType, const Type &dataType, unsigned rows,
    unsigned columns, expr::BinaryOpType operation, bool testSupervisor,
    unsigned bElems, const std::function<double(double, double)> &hostFn,
    bool doCheck, bool doReport) {
  Type outputType;
  if (operation == expr::BinaryOpType::INV_STD_DEV_TO_VARIANCE) {
    outputType = FLOAT;
  }
  if (operation == expr::BinaryOpType::VARIANCE_TO_INV_STD_DEV) {
    outputType = HALF;
  }
  auto total_elems = rows * columns;

  // Program generated test data
  std::vector<double> outTest(total_elems);
  std::vector<double> inTest(total_elems);
  std::vector<double> BTest(bElems);

  // Initialise input patterns
  for (unsigned i = 0; i < total_elems; i++) {
    inTest[i] = static_cast<double>(i) + 1;
    outTest[i] = static_cast<double>(i) + 1;
  }

  double k = 4;
  for (unsigned i = 0; i < BTest.size(); i++) {
    BTest[i] = static_cast<double>(i) + k;
  }

  // Create Graph object, target and device
  auto device = createTestDevice(deviceType);
  Target target = device.getTarget();
  Graph graph(target);
  popops::addCodelets(graph);

  auto in = graph.addVariable(dataType, {total_elems}, "Input");
  auto out = graph.addVariable(outputType, {total_elems}, "Output");
  graph.setTileMapping(in, 0);
  graph.setTileMapping(out, 0);

  // Create B as scalar or vector, as required
  Tensor B;
  if (bElems == 1) {
    B = graph.addVariable(dataType, {}, "Constant");
  } else {
    B = graph.addVariable(dataType, {bElems}, "Constant");
  }
  graph.setTileMapping(B, 0);

  // Make a sequence to run the operation
  Sequence sequence;
  ComputeSet testComputeSet = graph.addComputeSet("computeOp");
  std::string vertexName, vertexClass;

  if (testSupervisor) {
    vertexName = "popops::BroadcastScalar2Types1DSupervisor";
  } else {
    vertexName = bElems == 1 ? "popops::BroadcastScalar2Types2DData"
                             : "popops::BroadcastScalar2Types2D";
  }
  vertexClass = templateVertex(vertexName, operation, dataType, outputType);

  auto vertex = graph.addVertex(testComputeSet, vertexClass);
  graph.setTileMapping(vertex, 0);

  if (testSupervisor) {
    graph.connect(vertex["data"], in);
  } else {
    graph.connect(vertex["data"], in.reshape({rows, columns}));
  }
  if (testSupervisor) {
    graph.connect(vertex["out"], out);
  } else {
    graph.connect(vertex["out"], out.reshape({rows, columns}));
  }

  graph.connect(vertex["B"], B);
  // allocateHostMemoryForTensor
  Sequence uploadProg, downloadProg;
  std::vector<std::pair<std::string, char *>> tmap;
  auto input = allocateHostMemoryForTensor(in, "in", graph, uploadProg,
                                           downloadProg, tmap);
  auto inputB = allocateHostMemoryForTensor(B, "inB", graph, uploadProg,
                                            downloadProg, tmap);
  auto output = allocateHostMemoryForTensor(out, "out", graph, uploadProg,
                                            downloadProg, tmap);
  sequence.add(Execute(testComputeSet));

  graph.createHostRead("outStream", out);

  // Run sequence and compare host and IPU result
  Engine engine(graph, Sequence(uploadProg, sequence, downloadProg), options);
  attachStreams(engine, tmap);

  // Put test inputs into an array of the correct type ready to use
  copy(target, inTest.data(), inTest.size(), dataType, input.get());
  copy(target, BTest.data(), BTest.size(), dataType, inputB.get());
  copy(target, outTest.data(), outTest.size(), outputType, output.get());

  std::vector<double> outHost(total_elems);
  std::vector<char> outHostRaw(total_elems * 4);
  device.bind([&](const Device &d) {
    engine.load(d);
    engine.run(0);

    if (doReport) {
      OptionFlags opt;
      opt.set("showExecutionSteps", "true");

      engine.printProfileSummary(std::cerr, opt);
    }

    // Fetch the result and convert to a double for comparison
    engine.readTensor("outStream", (void *)&outHostRaw[0]);
  });

  copy(target, outputType, outHostRaw.data(), outHost.data(), outHost.size());

  // Host generated result, start with zeros, leaving the unmodified input data
  // in the padding section
  for (unsigned i = 0; i < total_elems; i++) {
    outTest[i] = 0;
  }
  // Then do the operation for comparison
  unsigned bIndex = 0;
  for (unsigned i = 0; i < rows; i++) {
    for (unsigned j = 0; j < columns; j++) {
      if (bElems == 1) {
        outTest[j + i * columns] = hostFn(inTest[j + i * columns], BTest[0]);
      } else {
        outTest[j + i * columns] =
            hostFn(inTest[j + i * columns], BTest[bIndex]);
      }
    }
    bIndex++;
    if (bIndex == bElems) {
      bIndex = 0;
    }
  }
  // Check the result, in the outTest array
  if (doCheck) {
    bool check = checkIsClose("BroadcastTest", outHost.data(), {outHost.size()},
                              outTest.data(), outTest.size(), 0.01, 0.01);
    return check;
  } else {
    return true;
  }
}

//******************************************************************************
int main(int argc, char **argv) {
  namespace po = boost::program_options;

  DeviceType deviceType;
  Type dataType;

  std::string operation;
  unsigned rows, columns;
  bool doCheck = true;
  bool doReport = false;
  unsigned bLength = 1;
  bool testSupervisor = false;
  bool inPlace = true;
  bool divideByRow = false;
  int in1Offset = 0;
  int outOffset = 0;
  bool castOutput = false;

  po::options_description desc("Options");

  // clang-format off
  desc.add_options()
    ("help", "Print help")
     ("check",
     po::value<bool>(&doCheck)->default_value(doCheck),
     "Activate check for correct result")
     ("report",
     po::value<bool>(&doReport)->default_value(doReport),
     "Provide a poplar report")
     ("b-length",
     po::value<unsigned>(&bLength)->default_value(bLength),
     "Length of second tensor")
     ("supervisor",
     po::value<bool>(&testSupervisor)->default_value(testSupervisor),
     "Test supervisor vertices")
    ("device-type",
     po::value<DeviceType>(&deviceType)->required(),
     "Device Type")
    ("data-type",
     po::value<Type>(&dataType)->required(),
     "Data Type")
    ("cast-out",
     po::value<bool>(&castOutput)->default_value(castOutput),
     "Cast output (VARIANCE_TO_INV_STD_DEV->half, "
                  "INV_STD_DEV_TO_VARIANCE->float")
    ("rows",
     po::value<unsigned>(&rows)->required(),
     "In/Out data rows")
    ("columns",
     po::value<unsigned>(&columns)->required(),
     "In/Out data columns")
    ("in-place",
     po::value<bool>(&inPlace)->required(),
     "Test the in-place variant")
    ("in1-offset",
     po::value<int>(&in1Offset)->default_value(in1Offset),
     "Number of elements to pad between region start and in1")
    ("out-offset",
     po::value<int>(&outOffset)->default_value(outOffset),
     "Number of elements to pad between region start and out")
     ("divide-by-row",
     po::value<bool>(&divideByRow)->default_value(divideByRow),
     "Divide work by row for the vector outer variant")
    ("operation",
     po::value<std::string>(&operation)->required(),
     "Allowed operations: ADD MULTIPLY SUBTRACT VARIANCE_TO_INV_STD_DEV"
     " INV_STD_DEV_TO_VARIANCE\n");
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
  expr::BinaryOpType broadcastOperation;
  std::function<double(double, double)> broadcastHostFn;

  // Operations
  if (operation == "ADD") {
    broadcastOperation = expr::BinaryOpType::ADD;
    broadcastHostFn = [](double x, double y) -> double { return x + y; };
  } else if (operation == "MULTIPLY") {
    broadcastOperation = expr::BinaryOpType::MULTIPLY;
    broadcastHostFn = [](double x, double y) -> double { return x * y; };
  } else if (operation == "SUBTRACT") {
    broadcastOperation = expr::BinaryOpType::SUBTRACT;
    broadcastHostFn = [](double x, double y) -> double { return x - y; };
  } else if (operation == "INV_STD_DEV_TO_VARIANCE") {
    broadcastOperation = expr::BinaryOpType::INV_STD_DEV_TO_VARIANCE;
    broadcastHostFn = [](double x, double y) -> double {
      return (1 / (x * x)) - y;
    };
  } else if (operation == "VARIANCE_TO_INV_STD_DEV") {
    broadcastOperation = expr::BinaryOpType::VARIANCE_TO_INV_STD_DEV;
    broadcastHostFn = [](double x, double y) -> double {
      return 1 / sqrt(x + y);
    };
  } else {
    std::cerr << " Error: Operation " << operation << " not recognised\n";
    return 1;
  }
  if (castOutput) {
    if (broadcastOperation != expr::BinaryOpType::INV_STD_DEV_TO_VARIANCE &&
        broadcastOperation != expr::BinaryOpType::VARIANCE_TO_INV_STD_DEV) {
      std::cerr << " Error: Casting the output is not supported for "
                << operation << "\n";
      return 0;
    }
    if (in1Offset != 0 || outOffset != 0 || inPlace) {
      std::cerr << " Error: Casting the output is not supported for inPlace "
                   "operations. Testing is not supported with offsets as both "
                   "types would need to be the same."
                << "\n";
      return 0;
    }
    if (!doBroadcastOpTestCastOutput(
            deviceType, dataType, rows, columns, broadcastOperation,
            testSupervisor, bLength, broadcastHostFn, doCheck, doReport)) {
      return 1;
    }

  } else {
    if (!doBroadcastOpTest(deviceType, dataType, rows, columns,
                           broadcastOperation, testSupervisor, bLength, inPlace,
                           divideByRow, broadcastHostFn, doCheck, doReport,
                           in1Offset, outOffset)) {
      return 1;
    }
  }

  return 0;
}
