#include "poputil/exceptions.hpp"
#include "poputil/TileMapping.hpp"
#include <cassert>

using namespace poplar;

namespace popnn {

void checkTensorShape(Tensor acts) {
  const auto rank = acts.rank();
  if (rank < 2 ) {
    throw poputil::poplibs_error(
      "Norm supported for tensors of rank > 1");
  }
}

Tensor preProcessNormActs(const Tensor &acts) {
  return acts.rank() == 2 ? acts.expand({2}) : acts;
}

Tensor postProcessNormActs(const Tensor &acts, unsigned originalActsRank) {
  if (originalActsRank == 2) {
    assert(acts.rank() == 3 && acts.dim(2) == 1);
    return acts.squeeze({2});
  }
  return acts;
}
} // namespace popnn
