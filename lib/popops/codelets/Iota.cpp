// Copyright (c) 2019 Graphcore Ltd. All rights reserved.
#include "poplibs_support/ExternalCodelet.hpp"
#include <poplar/VectorTypes.hpp>
#include <poplar/Vertex.hpp>

using namespace poplar;

namespace popops {

static constexpr auto ONE_PTR = poplar::VectorLayout::ONE_PTR;

template <typename OutType> class Iota : public Vertex {
public:
  Iota();

  IS_EXTERNAL_CODELET(false);
  Vector<Output<Vector<OutType>>> out;
  Input<Vector<OutType, ONE_PTR>> offsets;

  bool compute() {
    for (unsigned i = 0; i != out.size(); ++i) {
      auto base = offsets[i];
      auto n = out[i].size();
      for (unsigned j = 0; j != n; ++j) {
        out[i][j] = base + static_cast<OutType>(j);
      }
    }
    return true;
  }
};

template class Iota<unsigned>;
template class Iota<int>;

} // namespace popops
