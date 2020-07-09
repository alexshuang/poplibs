// Copyright (c) 2017 Graphcore Ltd. All rights reserved.
#define BOOST_TEST_MODULE ConvPlanTest
#include "ConvPlan.hpp"
#include "ConvOptions.hpp"
#include "TestDevice.hpp"
#include "poplin/CanonicalConvParams.hpp"
#include "poplin/ConvUtil.hpp"
#include "poputil/exceptions.hpp"
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/test/unit_test.hpp>
#include <popnn/codelets.hpp>
#include <vector>

const auto testIpuName = deviceTypeToIPUName(TEST_TARGET);

static auto params = poplin::ConvParams{poplar::FLOAT, // Data type
                                        2,             // batch size
                                        {4, 4},        // input field shape
                                        {3, 3},        // kernel shape
                                        3,             // input channels
                                        4,             // output channels
                                        5};            // conv groups

static auto fcParams = poplin::ConvParams{poplar::FLOAT, // Data type
                                          1,             // batch size
                                          {4},           // input field shape
                                          {1},           // kernel shape
                                          3,             // input channels
                                          4,             // output channels
                                          5};            // conv groups

BOOST_AUTO_TEST_CASE(getPlan) {
  poplar::Graph graph(poplar::Target::createCPUTarget());
  auto &target = graph.getTarget();
  auto options = poplin::ConvOptions(target);

  poplin::getPlan(target, params, options, nullptr);
}

BOOST_AUTO_TEST_CASE(getCachedPlans) {
  poplar::Graph graph(poplar::Target::createIPUTarget(2, testIpuName));
  auto &target = graph.getTarget();

  poplin::PlanningCache cache;

  poplin::getPlan(target, params, poplin::ConvOptions(target), &cache);
  poplin::getPlan(target, params, poplin::ConvOptions(target), &cache);
}

BOOST_AUTO_TEST_CASE(StartTileIsPassOblivious) {
  poplar::Graph graph(poplar::Target::createIPUTarget(2, testIpuName));
  auto &target = graph.getTarget();

  poplin::PlanningCache cache;

  const auto getPlanForPass = [&](poplin::Pass pass,
                                  const poplin::ConvParams &params) {
    poplin::ConvOptions options(target);
    options.pass = pass;
    auto plan = poplin::getPlan(target, params, options, &cache);
    BOOST_TEST_MESSAGE(pass);
    return plan;
  };

  const auto checkStartTileAndDirection = [&](poplin::Pass pass,
                                              const poplin::ConvParams &params,
                                              const poplin::Plan &expected) {
    auto plan = getPlanForPass(pass, params);

    BOOST_CHECK_EQUAL(expected.startTile, plan.startTile);
    BOOST_CHECK_EQUAL(expected.linearizeTileDirection,
                      plan.linearizeTileDirection);
  };

  {
    // INFERENCE_FWD does not need to be invariant and so isn't guaranteed
    // to match.
    const auto expected = getPlanForPass(poplin::Pass::NONE, params);
    checkStartTileAndDirection(poplin::Pass::TRAINING_FWD, params, expected);
    checkStartTileAndDirection(poplin::Pass::TRAINING_BWD,
                               poplin::getGradientParams(params), expected);
    checkStartTileAndDirection(poplin::Pass::TRAINING_WU,
                               poplin::getWeightUpdateParams(params), expected);
  }

  {
    // Once T16758 is fixed we should be able to check that all of these plans
    // are the same, not just the FC / non-FC split between passes.
    // FC_INFERENCE_FWD does not need to be invariant and so isn't guaranteed
    // to match.
    const auto expected =
        getPlanForPass(poplin::Pass::FC_TRAINING_FWD, fcParams);

    auto bwdParams = fcParams;
    std::swap(bwdParams.inputFieldShape.front(),
              bwdParams.outputChannelsPerConvGroup);
    checkStartTileAndDirection(poplin::Pass::FC_TRAINING_BWD, bwdParams,
                               expected);

    auto wuParams = fcParams;
    std::swap(wuParams.inputChannelsPerConvGroup,
              wuParams.outputChannelsPerConvGroup);
    checkStartTileAndDirection(poplin::Pass::FC_TRAINING_WU, wuParams,
                               expected);
  }
}

// Test some simple aspects of plan constraining that we currently support
BOOST_AUTO_TEST_CASE(PartiallyConstrainPlan) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, testIpuName));
  const auto &target = graph.getTarget();

  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {"0": {"transform": {"swapOperands": true}}}
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  auto plan = poplin::getPlan(target, params, options, &cache);
  BOOST_CHECK_EQUAL(plan.transforms[0].swapOperands, true);
  ss.str("");
  ss.clear();
  ss << R"delim(
    {"0": {"partition": {"fieldSplit": {"0": 2, "1": 2}}}}
  )delim";
  t.clear();
  json_parser::read_json(ss, t);
  options.planConstraints = std::move(t);
  plan = poplin::getPlan(target, params, options, &cache);
  BOOST_CHECK_EQUAL(plan.partitions[0].fieldSplit[0], 2);
  BOOST_CHECK_EQUAL(plan.partitions[0].fieldSplit[1], 2);
}

BOOST_AUTO_TEST_CASE(CompletelyConstrainPlan) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, testIpuName));
  const auto &target = graph.getTarget();

  poplin::PlanningCache cache;

  const auto params = poplin::ConvParams{poplar::FLOAT, // Data type
                                         32,            // batch size
                                         {32, 32},      // input field shape
                                         {6, 6},        // kernel shape
                                         16,            // input channels
                                         16,            // output channels
                                         1};            // conv groups

  using namespace boost::property_tree;
  std::stringstream ss;
  // Constrain this to a plan the planner is extremely unlikely to choose
  // on its own.
  ss << R"delim(
    {"method": "MAC",
     "inChansPerGroup": 1,
     "partialChansPerGroup": 1,
     "0":
      {"transform": {"swapOperands": false,
                     "expandDims": [],
                     "outChanFlattenDims": []
                    },
       "partition": {"fieldSplit": {"0": 1, "1": 1},
                     "batchSplit": 1,
                     "outChanSplit": {"parallel": 1, "serial": 1},
                     "kernelSplit": {"0": 1, "1": 1},
                     "inChanSplit": {"parallel": 1, "serial": 1},
                     "convGroupSplit": 1
                    }
      }
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  auto plan = poplin::getPlan(target, params, options, &cache);
  BOOST_TEST_MESSAGE(plan << "\n");
  const auto &transforms = plan.transforms[0];
  BOOST_CHECK_EQUAL(transforms.swapOperands, false);
  BOOST_CHECK(transforms.expandDims.empty());
  BOOST_CHECK(transforms.outChanFlattenDims.empty());

  const std::vector<unsigned> expectedFieldSplit = {1, 1};
  const std::vector<unsigned> expectedKernelSplit = {1, 1};
  const auto &partition = plan.partitions[0];
  BOOST_CHECK_EQUAL_COLLECTIONS(
      partition.fieldSplit.begin(), partition.fieldSplit.end(),
      expectedFieldSplit.begin(), expectedFieldSplit.end());
  BOOST_CHECK_EQUAL(partition.batchSplit, 1);
  BOOST_CHECK_EQUAL(partition.outChanSplit.parallel, 1);
  BOOST_CHECK_EQUAL(partition.outChanSplit.serial, 1);
  BOOST_CHECK_EQUAL_COLLECTIONS(
      partition.kernelSplit.begin(), partition.kernelSplit.end(),
      expectedKernelSplit.begin(), expectedKernelSplit.end());
  BOOST_CHECK_EQUAL(partition.inChanSplit.parallel, 1);
  BOOST_CHECK_EQUAL(partition.inChanSplit.serial, 1);
  BOOST_CHECK_EQUAL(partition.convGroupSplit, 1);
}

BOOST_AUTO_TEST_CASE(InvalidConstraints) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, testIpuName));
  const auto &target = graph.getTarget();

  poplin::PlanningCache cache;

  const auto params = poplin::ConvParams{poplar::FLOAT, // Data type
                                         32,            // batch size
                                         {32, 32},      // input field shape
                                         {6, 6},        // kernel shape
                                         16,            // input channels
                                         16,            // output channels
                                         1};            // conv groups

  BOOST_TEST_MESSAGE("Test params: " << params);

  auto testFails = [&](const std::string &s) {
    using namespace boost::property_tree;
    std::stringstream ss;
    ss << s;
    ptree t;
    json_parser::read_json(ss, t);
    auto options = poplin::ConvOptions(target);
    options.planConstraints = std::move(t);
    BOOST_TEST_MESSAGE("Trying constraints: " << s);
    poplin::Plan plan;
    BOOST_CHECK_THROW(plan = poplin::getPlan(target, params, options, &cache),
                      poputil::poplibs_error);
    BOOST_TEST_MESSAGE(plan << "\n");
  };

  // A random assortment of constraints we'd expect to fail to produce a valid
  // plan.

  // Can't use outer product method for this convolution
  testFails(
      R"delim(
      {"method": "OUTER_PRODUCT",
       "inChansPerGroup": 1,
       "partialChansPerGroup": 1}
    )delim");
  // MAC method only supports 1 partial chan per group
  testFails(
      R"delim(
      {"method": "MAC",
       "inChansPerGroup": 1,
       "partialChansPerGroup": 2}
    )delim");
  // AMP method only supports certain partialChansPerGroup
  testFails(
      R"delim(
      {"method": "AMP",
       "inChansPerGroup": 4,
       "partialChansPerGroup": 15}
    )delim");
  // inChanSplit exceeds number of input channels.
  testFails(
      R"delim(
      {"method": "MAC",
       "0": {"partition":{"inChanSplit":{"parallel": 256, "serial": 256}}}
      }
    )delim");
  // Product of outChanSplits exceeds number of output channels.
  testFails(
      R"delim(
      {"method": "MAC",
       "0": {"partition":{"outChanSplit":{"parallel": 16, "serial": 16}}}
      }
    )delim");
  // Product of batch splits exceeds number of batches.
  testFails(
      R"delim(
      {"method": "MAC",
       "0": {"partition":{"batchSplit": 256}}
      }
    )delim");
  // Total split greater than the number of available tiles.
  testFails(
      R"delim(
      {"method": "MAC",
       "0": {"transform":{"swapOperands": false,
                          "expandDims": [],
                          "outChanFlattenDims": []},
             "partition":{"fieldSplit": {"0": 1217}}}
      }
    )delim");
}

BOOST_AUTO_TEST_CASE(ValidOuterProduct1) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, 4, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
       "method": "OUTER_PRODUCT",
       "0": {"transform":{"swapOperands": false}}
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  poplin::Plan plan;
  BOOST_CHECK_NO_THROW(
      plan = poplin::getPlan(target,
                             poplin::ConvParams{
                                 poplar::FLOAT, // Data type
                                 4, // batch size (OK because we have 4 tiles)
                                 {1, 1}, // input field shape
                                 {1, 1}, // kernel shape
                                 1,      // input channels
                                 1,      // output channels
                                 1       // conv groups
                             },
                             options, &cache));
  BOOST_TEST_MESSAGE(plan << "\n");
}

BOOST_AUTO_TEST_CASE(ValidOuterProduct2) {
  poplar::Graph graph(poplar::Target::createIPUTarget(4, 1, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
       "method": "OUTER_PRODUCT",
       "0": {"transform":{"swapOperands": false}}
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  poplin::Plan plan;
  BOOST_CHECK_NO_THROW(
      plan = poplin::getPlan(target,
                             poplin::ConvParams{
                                 poplar::FLOAT, // Data type
                                 4, // batch size (OK because we have 4 IPUs)
                                 {1, 1}, // input field shape
                                 {1, 1}, // kernel shape
                                 1,      // input channels
                                 1,      // output channels
                                 1       // conv groups
                             },
                             options, &cache));
  BOOST_TEST_MESSAGE(plan << "\n");
}

BOOST_AUTO_TEST_CASE(ValidOuterProduct3) {
  poplar::Graph graph(poplar::Target::createIPUTarget(2, 2, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
       "method": "OUTER_PRODUCT",
       "0": {"transform":{"swapOperands": false}}
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  poplin::Plan plan;
  BOOST_CHECK_NO_THROW(
      plan = poplin::getPlan(
          target,
          poplin::ConvParams{
              poplar::FLOAT, // Data type
              4,      // batch size (OK as we have 2 IPUs with 2 tiles each)
              {1, 1}, // input field shape
              {1, 1}, // kernel shape
              1,      // input channels
              1,      // output channels
              1       // conv groups
          },
          options, &cache));
  BOOST_TEST_MESSAGE(plan << "\n");
}

BOOST_AUTO_TEST_CASE(InvalidOuterProduct1) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, 1, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
       "method": "OUTER_PRODUCT",
       "0": {"transform":{"swapOperands": false}}
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  poplin::Plan plan;
  BOOST_CHECK_THROW(plan = poplin::getPlan(
                        target,
                        poplin::ConvParams{
                            poplar::FLOAT, // Data type
                            4, // batch size (invalid! Not enough tiles/IPUs)
                            {1, 1}, // input field shape
                            {1, 1}, // kernel shape
                            1,      // input channels
                            1,      // output channels
                            1       // conv groups
                        },
                        options, &cache),
                    poputil::poplibs_error);
  BOOST_CHECK_THROW(
      plan = poplin::getPlan(target,
                             poplin::ConvParams{
                                 poplar::FLOAT, // Data type
                                 1,             // batch size
                                 {1, 1},        // input field shape
                                 {2, 1}, // kernel shape (Invalid! Must be 1)
                                 1,      // input channels
                                 1,      // output channels
                                 1       // conv groups
                             },
                             options, &cache),
      poputil::poplibs_error);

  BOOST_CHECK_THROW(plan = poplin::getPlan(
                        target,
                        poplin::ConvParams{
                            poplar::FLOAT, // Data type
                            1,             // batch size
                            {2, 1}, // input field shape (Invalid! Must be 1)
                            {1, 1}, // kernel shape
                            1,      // input channels
                            1,      // output channels
                            1       // conv groups
                        },
                        options, &cache),
                    poputil::poplibs_error);
}

BOOST_AUTO_TEST_CASE(InvalidCombineConvGroups_2InputChannels) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, 1, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
      "0": {
        "transform": {
          "combineConvGroupsFactor": [2, 4, 8, 16]
        }
      }
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  poplin::Plan plan;

  // must have one input channel per group for the combineConvGroup transform
  BOOST_CHECK_THROW(
      plan = poplin::getPlan(target,
                             poplin::ConvParams{
                                 poplar::FLOAT, // Data type
                                 1,             // batch size
                                 {1, 1},        // input field shape
                                 {1, 1},        // kernel shape
                                 2, // input channels (invalid! must be 1)
                                 1, // output channels
                                 1  // conv groups
                             },
                             options, &cache),
      poputil::poplibs_error);
}

BOOST_AUTO_TEST_CASE(ValidCombineConvGroups_ExpandDims) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, 1, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
       "0": {
          "transform":{
            "combineConvGroupsFactor": [2, 4, 8, 16],
            "expandDims": []
        }
      }
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);

  // expandDims is constrained but constrained to be empty so this is fine.
  poplin::getPlan(target,
                  poplin::ConvParams{
                      poplar::FLOAT, // Data type
                      1,             // batch size
                      {1, 1},        // input field shape
                      {1, 1},        // kernel shape
                      1,             // input channels
                      1,             // output channels
                      2              // conv groups
                  },
                  options, &cache);
}

BOOST_AUTO_TEST_CASE(InvalidCombineConvGroups_ExpandDims) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, 1, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
       "0": {
          "transform":{
            "combineConvGroupsFactor": [2, 4, 8, 16],
            "expandDims": [0, 1]
        }
      }
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  poplin::Plan plan;

  // Sometimes we cannot have both combineConvGroups and expandDims constrained.
  BOOST_CHECK_THROW(plan = poplin::getPlan(target,
                                           poplin::ConvParams{
                                               poplar::FLOAT, // Data type
                                               1,             // batch size
                                               {1, 1}, // input field shape
                                               {1, 1}, // kernel shape
                                               3,      // input channels
                                               1,      // output channels
                                               2       // conv groups
                                           },
                                           options, &cache),
                    poputil::poplibs_error);
}

BOOST_AUTO_TEST_CASE(InvalidCombineConvGroups_OutChanFlattenDims) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, 1, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
       "0": {
          "transform":{
            "combineConvGroupsFactor": [2, 4, 8, 16],
            "outChanFlattenDims": [0, 1]
        }
      }
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  poplin::Plan plan;

  // Sometimes we cannot have both combineConvGroups and outChanFlattenDims
  // constrained.
  BOOST_CHECK_THROW(plan = poplin::getPlan(target,
                                           poplin::ConvParams{
                                               poplar::FLOAT, // Data type
                                               1,             // batch size
                                               {1, 1}, // input field shape
                                               {1, 1}, // kernel shape
                                               3,      // input channels
                                               1,      // output channels
                                               2       // conv groups
                                           },
                                           options, &cache),
                    poputil::poplibs_error);
}

BOOST_AUTO_TEST_CASE(InvalidLevel) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, 1, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
       "3": {
         "transform":{
           "combineConvGroups": true
         }
       }
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  poplin::Plan plan;

  // Hierarchy level 3 is invalid
  BOOST_CHECK_THROW(plan = poplin::getPlan(target,
                                           poplin::ConvParams{
                                               poplar::FLOAT, // Data type
                                               1,             // batch size
                                               {1, 1}, // input field shape
                                               {1, 1}, // kernel shape
                                               1,      // input channels
                                               1,      // output channels
                                               2       // conv groups
                                           },
                                           options, &cache),
                    poputil::poplibs_error);
}

BOOST_AUTO_TEST_CASE(InvalidFieldDimensionIndex) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, 1, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
       "0": {
         "transform":{
           "outChanFlattenDims": [0, 3]
         }
       }
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  poplin::Plan plan;

  // Field dimension 3 is invalid
  BOOST_CHECK_THROW(plan = poplin::getPlan(target,
                                           poplin::ConvParams{
                                               poplar::FLOAT, // Data type
                                               1,             // batch size
                                               {1, 1}, // input field shape
                                               {1, 1}, // kernel shape
                                               1,      // input channels
                                               1,      // output channels
                                               2       // conv groups
                                           },
                                           options, &cache),
                    poputil::poplibs_error);
}

BOOST_AUTO_TEST_CASE(InvalidKernelDimensionIndex) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, 1, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
       "0": {
         "partition": {
           "kernelSplit": {
             "0": "1",
             "4": "1"
           }
         }
       }
    }
  )delim";
  ptree t;
  json_parser::read_json(ss, t);
  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);
  poplin::Plan plan;

  // Kernel dimension 4 is invalid
  BOOST_CHECK_THROW(plan = poplin::getPlan(target,
                                           poplin::ConvParams{
                                               poplar::FLOAT, // Data type
                                               1,             // batch size
                                               {1, 1}, // input field shape
                                               {1, 1}, // kernel shape
                                               1,      // input channels
                                               1,      // output channels
                                               2       // conv groups
                                           },
                                           options, &cache),
                    poputil::poplibs_error);
}

BOOST_AUTO_TEST_CASE(GetSLICPlan) {
  poplar::Graph graph(poplar::Target::createIPUTarget(2, 2, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  using namespace boost::property_tree;
  std::stringstream ss;
  ss << R"delim(
    {
       "method": "SLIC"
    }
  )delim";

  ptree t;
  json_parser::read_json(ss, t);

  auto options = poplin::ConvOptions(target);
  options.planConstraints = std::move(t);

  poplin::Plan plan;
  BOOST_CHECK_NO_THROW(plan = poplin::getPlan(target,
                                              poplin::ConvParams{
                                                  poplar::HALF, // Data type
                                                  1,            // batch size
                                                  {1, 1}, // input field shape
                                                  {1, 1}, // kernel shape
                                                  1,      // input channels
                                                  1,      // output channels
                                                  2       // conv groups
                                              },
                                              options, &cache));

  // currently only SLIC 1x4 is supported in the planner.
  BOOST_CHECK(plan.method == poplin::Plan::Method::SLIC);
  BOOST_CHECK(plan.slicWindowWidth == 4);

  BOOST_TEST_MESSAGE(plan << "\n");
}

// Check the mk1-only enableAmpHalfEnginesPlan option works
// (the option is ignored for IpuModel2)
BOOST_AUTO_TEST_CASE(
    GetAMP4Plan,
    *boost::unit_test::enable_if<(DeviceType::IpuModel == TEST_TARGET)>()) {
  poplar::Graph graph(poplar::Target::createIPUTarget(1, 1, testIpuName));
  const auto &target = graph.getTarget();
  poplin::PlanningCache cache;

  auto options = poplin::ConvOptions(target);
  options.enableAmpHalfEnginesPlan = true;

  poplin::Plan plan;
  BOOST_CHECK_NO_THROW(plan = poplin::getPlan(target,
                                              poplin::ConvParams{
                                                  poplar::HALF, // Data type
                                                  1,            // batch size
                                                  {4, 4}, // input field shape
                                                  {1, 1}, // kernel shape
                                                  8,      // input channels
                                                  4,      // output channels
                                                  1       // conv groups
                                              },
                                              options, &cache));

  BOOST_CHECK(plan.method == poplin::Plan::Method::AMP);
  BOOST_CHECK(plan.numConvUnitsRequired == 4);
  BOOST_TEST_MESSAGE(plan << "\n");
}
