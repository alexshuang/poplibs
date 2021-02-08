// Copyright (c) 2019 Graphcore Ltd. All rights reserved.
#ifndef DYNAMIC_SLICE_INTERNAL_HPP
#define DYNAMIC_SLICE_INTERNAL_HPP
#include <iostream>
#include <memory>
#include <vector>

namespace popops {
namespace sliceInternal {
// How to partition work across tiles.
struct Partition {
  // How much to split processing of lookup indices between tiles.
  std::size_t lookupSplit;
  // How much to split the sliced/updated dimension of the
  // tensor to be sliced/updated between tiles.
  std::size_t slicedDimSplit;
  // How much to split the product of dimensions that are not
  // sliced/updated between tiles.
  std::size_t unslicedDimSplit;
  // Grain size for no. of elements in the product of dimensions that
  // are not sliced/updated on each tile.
  std::size_t unslicedGrainSize;
};

bool operator<(const Partition &a, const Partition &b) {
  return std::tie(a.lookupSplit, a.slicedDimSplit, a.unslicedDimSplit,
                  a.unslicedGrainSize) <
         std::tie(b.lookupSplit, b.slicedDimSplit, b.unslicedDimSplit,
                  b.unslicedGrainSize);
}

bool operator==(const Partition &a, const Partition &b) {
  return std::tie(a.lookupSplit, a.slicedDimSplit, a.unslicedDimSplit,
                  a.unslicedGrainSize) ==
         std::tie(b.lookupSplit, b.slicedDimSplit, b.unslicedDimSplit,
                  b.unslicedGrainSize);
}

} // namespace sliceInternal

class SlicePlanInternal {
public:
  SlicePlanInternal() : isNull(true) {}

public:
  bool isNull;
  sliceInternal::Partition partition;

  // For validation, to identify the restrictions on what this
  // plan can be used to implement,
  std::size_t rank;
  std::vector<std::size_t> slicedDims;
  std::vector<std::size_t> slicedDimSizes;

  std::unique_ptr<SlicePlanInternal> clone() const {
    return std::make_unique<SlicePlanInternal>(*this);
  };
};

bool operator<(const SlicePlanInternal &a,
               const SlicePlanInternal &b) noexcept {
  return std::tie(a.isNull, a.partition, a.rank, a.slicedDims,
                  a.slicedDimSizes) < std::tie(b.isNull, b.partition, b.rank,
                                               b.slicedDims, b.slicedDimSizes);
}

bool operator==(const SlicePlanInternal &a,
                const SlicePlanInternal &b) noexcept {
  return std::tie(a.isNull, a.partition, a.rank, a.slicedDims,
                  a.slicedDimSizes) == std::tie(b.isNull, b.partition, b.rank,
                                                b.slicedDims, b.slicedDimSizes);
}

std::ostream &operator<<(std::ostream &o, const SlicePlanInternal &p);

} // namespace popops
#endif // DYNAMIC_SLICE_INTERNAL_HPP
