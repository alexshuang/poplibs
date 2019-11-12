#include "poplibs_support/logging.hpp"
#include <poplar/exceptions.hpp>
#include <poplin/MeshGrid.hpp>

namespace {
// Linspace values are actually generated by this funciton.
std::vector<float> linspaceValues(float left, float right, size_t count) {
  std::vector<float> values(count);
  float current = left;
  const float step = (right - left) / (count - 1.f);
  for (auto &v : values) {
    v = current;
    current += step;
  }
  return values;
}
} // namespace

namespace poplin {

namespace logging = poplibs_support::logging;

poplar::Tensor linspace(poplar::Graph &graph, const poplar::Type &type,
                        float left, float right, size_t count,
                        const std::string &debugPrefix) {
  const auto fnPrefix = debugPrefix + "/linspace";
  logging::info("linspace type={}, left={}, right={}, count={}, name={}", type,
                left, right, count, fnPrefix);

  if (type != poplar::FLOAT && type != poplar::HALF) {
    throw poplar::poplar_error("linspace only supports FLOAT or HALF");
  }

  const std::vector<float> values = linspaceValues(left, right, count);
  const std::vector<size_t> shape = {count};
  auto t =
      graph.addConstant(type, shape, poplar::ArrayRef<float>(values), fnPrefix);
  graph.setTileMapping(t, 0);
  return t;
}

std::vector<poplar::Tensor> meshgrid2d(poplar::Graph &graph, poplar::Tensor x,
                                       poplar::Tensor y) {
  if (x.rank() != 1 || y.rank() != 1) {
    throw poplar::poplar_error("Meshgrid inputs must have be rank 1 tensors");
  }
  if (x.elementType() != y.elementType()) {
    throw poplar::poplar_error(
        "Meshgrid inputs must have the same element type");
  }

  const auto nx = x.numElements();
  const auto ny = y.numElements();

  // The output grids have nx columns and ny rows:
  logging::info("meshgrid2d x={}, y={}", nx, ny);
  return {x.reshape({1, nx}).broadcast(ny, 0),
          y.reshape({ny, 1}).broadcast(nx, 1)};
}

} // end namespace poplin
