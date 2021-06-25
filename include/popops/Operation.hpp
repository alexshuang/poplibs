// Copyright (c) 2019 Graphcore Ltd. All rights reserved.
/** \file
 *
 * Define types of operations used in a reduce.
 *
 */

#ifndef popops_Operation_hpp
#define popops_Operation_hpp

#include "popops/OperationDef.hpp"
#include <iosfwd>

namespace popops {

/// Parse token from input stream \is to \op. Valid input values are the
/// stringified enumerations, for example "ADD" or "MUL".
/// \return The original input stream.
std::istream &operator>>(std::istream &is, Operation &op);

/// Write \op to output stream \os. The value written is the stringified
/// enumeration, for example "ADD" or "MUL".
/// \return The original output stream.
std::ostream &operator<<(std::ostream &os, const Operation &op);

} // End namespace popops

#endif // popops_Operation_hpp
