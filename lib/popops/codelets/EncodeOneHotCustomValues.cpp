// Copyright (c) 2019 Graphcore Ltd. All rights reserved.
#include "poplibs_support/ExternalCodelet.hpp"
#include <cassert>
#include <poplar/HalfFloat.hpp>
#include <poplar/VectorTypes.hpp>
#include <poplar/Vertex.hpp>

using namespace poplar;

namespace popops {

static constexpr auto ONE_PTR = poplar::VectorLayout::ONE_PTR;

template <typename IndexType, typename OutType>
class EncodeOneHotCustomValues : public Vertex {
  constexpr static bool isExternal() { return false; }

public:
  EncodeOneHotCustomValues();

  IS_EXTERNAL_CODELET(isExternal());

  Input<Vector<IndexType>> indices;
  Output<Vector<OutType, ONE_PTR, 8>> out;
  // the output tensor has been flattened, so this field states how many
  // elements to be processed for each index.
  Input<Vector<unsigned, ONE_PTR>> sliceLength;
  Input<Vector<unsigned, ONE_PTR>> offsets;

  Input<OutType> On;
  Input<OutType> Off;

  // This field could be removed as it is sum of the total slice Lengths
  const unsigned outLength;

  bool compute() {
    for (unsigned i = 0; i < outLength; ++i) {
      out[i] = Off;
    }
    unsigned begin = 0;
    for (unsigned i = 0; i < indices.size(); ++i) {
      if (indices[i] >= offsets[i] &&
          (indices[i] < offsets[i] + sliceLength[i])) {
        const auto index = begin + indices[i] - offsets[i];
        assert(index < outLength);
        out[index] = On;
      }
      begin += sliceLength[i];
    }
    return true;
  }
};

template class EncodeOneHotCustomValues<unsigned, float>;
template class EncodeOneHotCustomValues<unsigned, half>;
template class EncodeOneHotCustomValues<unsigned, unsigned>;
template class EncodeOneHotCustomValues<unsigned, int>;
template class EncodeOneHotCustomValues<int, float>;
template class EncodeOneHotCustomValues<int, half>;
template class EncodeOneHotCustomValues<int, unsigned>;
template class EncodeOneHotCustomValues<int, int>;

} // namespace popops
