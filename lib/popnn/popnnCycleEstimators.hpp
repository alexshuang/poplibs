// Copyright (c) 2017 Graphcore Ltd. All rights reserved.
#ifndef __popnnCycleEstimators_hpp__
#define __popnnCycleEstimators_hpp__

#include "poputil/exceptions.hpp"

#include <poplibs_support/cyclesTables.hpp>

namespace popnn {

poplibs::PerfEstimatorTable makePerfFunctionTable();
}

#endif
