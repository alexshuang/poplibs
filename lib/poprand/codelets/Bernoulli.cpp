// Copyright (c) 2019 Graphcore Ltd. All rights reserved.
#include "RandomUtils.hpp"

using namespace poplar;

namespace poprand {

template <typename OutType> class Bernoulli : public MultiVertex {
public:
  Bernoulli();

  Output<Vector<OutType, SPAN, 8>> out;
  const unsigned prob;

  IS_EXTERNAL_CODELET(true);

  bool compute(unsigned wid) {
    if (wid == 0) {
      uint32_t seed[2] = {0xDEADBEEF, 0xBEEFDEAD};
      uint32_t seedModifier = 0x900DDEED;

      uint64_t seedH = seed[0] + (static_cast<uint64_t>(seed[1]) << 32);
      uint64_t seedL = seed[1] + (static_cast<uint64_t>(seed[0]) << 32);
      auto s = initialiseAndPrime({seedL, seedH});
      bool isHalf = std::is_same<OutType, half>::value;
      const unsigned maxPerCall = isHalf ? 4 : 2;
      const unsigned bitsPerVal = isHalf ? 16 : 32;

      // rmask instruction takes the probability as int16
      uint64_t probToCode = prob * (1ULL << (bitsPerVal - 16));

      unsigned n = out.size();
      unsigned idx = 0;
      while (n) {
        const unsigned genSamples = min(n, maxPerCall);
        auto r = next(s);
        for (auto k = 0; k != genSamples; ++k, ++idx, r >>= bitsPerVal) {
          const uint64_t thisVal = r & ((1ULL << bitsPerVal) - 1);
          out[idx] = (thisVal < probToCode);
        }
        n -= genSamples;
      }
    }
    return true;
  }
};

template class Bernoulli<float>;
template class Bernoulli<half>;
template class Bernoulli<int>;

} // namespace poprand
