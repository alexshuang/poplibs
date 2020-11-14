// Copyright (c) 2018 Graphcore Ltd. All rights reserved.
#include "ComputeSetList.hpp"

#include <cassert>

using namespace poplar;

ComputeSetList::ComputeSetList(std::vector<ComputeSet> &css) : css(css) {}

ComputeSet ComputeSetList::add(Graph &graph,
                               const poplar::DebugNameAndId &dnai) {
  if (pos_ > css.size()) {
    throw std::logic_error("ComputeSetList::add() with pos " +
                           std::to_string(pos_) + " and size " +
                           std::to_string(css.size()));
  } else if (pos_ == css.size()) {
    // Add a new compute set.
    css.emplace_back(graph.addComputeSet({dnai}));
  }
  return css[pos_++];
}

std::size_t ComputeSetList::pos() const { return pos_; }

void ComputeSetList::setPos(std::size_t newPos) {
  if (newPos > css.size())
    throw std::logic_error("ComputeSetList::setPos(" + std::to_string(newPos) +
                           ")" + " which is > " + std::to_string(css.size()));

  pos_ = newPos;
}

poplar::ComputeSet &ComputeSetList::getCs1(const unsigned computeSets) {
  if (computeSets != 1 && computeSets != 2) {
    throw std::logic_error("ComputeSetList::getCs1 with" +
                           std::to_string(computeSets) + " which is != 1 or 2");
  }
  return css[pos_ - computeSets];
}
poplar::ComputeSet &ComputeSetList::getCs2(const unsigned computeSets) {
  if (computeSets != 2) {
    throw std::logic_error("ComputeSetList::getCs2 with" +
                           std::to_string(computeSets) + " which is != 2");
  }
  return css[pos_ - 1];
}
