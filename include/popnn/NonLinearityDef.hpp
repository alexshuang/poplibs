// Copyright (c) 2016 Graphcore Ltd. All rights reserved.
/** \file
 *  Definitions for non-linearity operations.
 */

#ifndef popnn_NonLinearityDef_hpp
#define popnn_NonLinearityDef_hpp

namespace popnn {

enum class NonLinearityType {
  /// Sigmoid:
  ///  * y = 1 / (1 + e^(-x))
  SIGMOID,
  /// Hard Sigmoid:
  ///  * y = max(0, min(1, 0.2*x + 0.5)
  HARD_SIGMOID,
  /// Rectified Linear Unit:
  ///  * x >= 0 -> y = x
  ///  * x < 0 -> y = 0
  RELU,
  /// Hyperbolic tangent:
  ///  * y = tanh(x)
  TANH,
  /// Gaussian Error Linear Unit:
  ///  * y = x * Phi(x)
  /// where Phi(x) is the cumulative distribution function of normal gaussian
  /// distribution. Phi(x) is approximated as:
  ///  * Phi(x) = 0.5 * (1 + (tanh(x * 0.7978845608 * (1 + 0.044715 * x * x))))
  GELU,
  /// Gaussian Error Linear Unit:
  /// More precise version that uses erf instead of tanh
  GELU_ERF,
  // SWISH:
  // * y = x / (1 + e^(-x))
  SWISH,
  /// Softmax:
  ///  * Always applied over the innermost dimension of the given tensor.
  ///    Outer dimensions are independent of one another.
  SOFTMAX,
  /// Same as SOFTMAX, but slower more numerically stable algorithm used.
  SOFTMAX_STABLE,
  /// Same as SOFTMAX, but slower more numerically stable algorithm used.
  /// Outputs are scaled to allow use of greater dynamic range in outputs.
  SOFTMAX_SCALED
};

} // end namespace popnn

#endif // popnn_NonLinearityDef_hpp
