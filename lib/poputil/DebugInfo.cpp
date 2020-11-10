// Copyright (c) 2020 Graphcore Ltd. All rights reserved.

#include <poputil/DebugInfo.hpp>

#include <sstream>

namespace poputil {

template <> poplar::ProfileValue toProfileValue(const poplar::ComputeSet &t) {
  return poplar::ProfileValue(t.getId());
}

template <> poplar::ProfileValue toProfileValue(const poplar::Tensor &t) {
  poplar::ProfileValue::Map v;

  std::stringstream ss;
  ss << "[";
  const auto &shape = t.shape();
  for (size_t i = 0; i < shape.size(); ++i) {

    if (i != 0) {
      ss << ", ";
    }

    ss << shape[i];
  }
  ss << "]";

  v.insert({"shape", poplar::ProfileValue(ss.str())});
  v.insert({"type", poplar::ProfileValue(t.elementType().toString())});
  return v;
}

template <> poplar::ProfileValue toProfileValue(const poplar::Type &t) {
  return poplar::ProfileValue(t.toString());
}

template <> poplar::ProfileValue toProfileValue(const bool &t) {
  return poplar::ProfileValue(t);
}

template <> poplar::ProfileValue toProfileValue(const float &t) {
  return poplar::ProfileValue(t);
}

template <> poplar::ProfileValue toProfileValue(const unsigned int &t) {
  return poplar::ProfileValue(t);
}

OpDebugInfo::OpDebugInfo(const poplar::DebugContext &debugContext,
                         std::string api)
    : poplar::DebugInfo(debugContext, "poplibs") {
  setValue("api", api);
}

void OpDebugInfo::add(const std::string &name,
                      const std::vector<ArgType> &args) {
  if (args.size() > 0) {
    poplar::ProfileValue::Map argsPV;
    for (auto &a : args) {
      argsPV.insert({a.n, a.pv});
    }
    setValue(name, argsPV);
  }
}

PoplibsOpDebugInfo::PoplibsOpDebugInfo(const poplar::DebugContext &debugContext,
                                       const std::vector<ArgType> &args,
                                       const std::string &api)
    : OpDebugInfo(debugContext, api) {
  add("args", args);
}

void PoplibsOpDebugInfo::addOutputs(const std::vector<ArgType> &outputs) {
  add("outputs", outputs);
}

void PoplibsOpDebugInfo::addOutput(const poplar::Tensor &output) {
  setValue("output", toProfileValue(output));
}

} // namespace poputil