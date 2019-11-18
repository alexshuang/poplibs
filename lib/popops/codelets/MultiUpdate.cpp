#include "poplibs_support/ExternalCodelet.hpp"
#include "poplibs_support/TileConstants.hpp"
#include <cassert>
#include <cmath>
#include <poplar/HalfFloat.hpp>
#include <poplar/Vertex.hpp>
#include <type_traits>

using namespace poplar;

static constexpr auto ONE_PTR = poplar::VectorLayout::ONE_PTR;
static constexpr auto DELTAN = poplar::VectorListLayout::DELTAN;
static constexpr auto SCALED_PTR32 = poplar::VectorLayout::SCALED_PTR32;

namespace popops {

// Update single slices from multiple offsets \a baseT to \a subT.
// This variant takes a 2d input and calculates the offsets given the start
// address of the base and sub Tensors. the updates are added to the core tensor
// indices that are not within the range of [baseOffset,
// baseOffset + numBaseElements) are ignored.
template <typename Type> class MultiUpdate : public Vertex {
public:
  MultiUpdate();

  Input<Vector<unsigned>> offsets; // in \a baseT
  InOut<Vector<Type, ONE_PTR>> baseT;
  Input<Vector<Type, ONE_PTR>> subT;
  const unsigned baseOffset;       // in the slice dimension
  const unsigned numBaseElements;  // in the slice dimension
  const unsigned short regionSize; // stride between slices

  bool compute() {
    for (unsigned o = 0; o != offsets.size(); ++o) {
      auto baseIdx = offsets[o];
      if (baseIdx < baseOffset || baseIdx >= baseOffset + numBaseElements) {
        // this slice is not a part of baseT so we can skip it.
        continue;
      }
      baseIdx -= baseOffset;

      for (unsigned e = 0; e != regionSize; ++e) {
        baseT[baseIdx * regionSize + e] = subT[o * regionSize + e];
      }
    }
    return true;
  }
};

template class MultiUpdate<float>;
template class MultiUpdate<half>;
template class MultiUpdate<int>;
template class MultiUpdate<unsigned>;

} // namespace popops
