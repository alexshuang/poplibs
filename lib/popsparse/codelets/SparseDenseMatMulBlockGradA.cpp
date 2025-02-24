// Copyright (c) 2020 Graphcore Ltd. All rights reserved.

// Computes Q = R'S
// where R is transposed but the meta-info is created for R rather than R'

#include <poplar/AvailableVTypes.h>
#include <poplar/HalfFloat.hpp>
#include <poplar/Vertex.hpp>

#include "SparseMetaInfo.hpp"
#include "poplar/TileConstants.hpp"
#include "poplibs_support/ExternalCodelet.hpp"

using namespace poplar;

static constexpr auto ONE_PTR = VectorLayout::ONE_PTR;
static constexpr auto SHORT_SPAN = VectorLayout::SHORT_SPAN;

template <typename FPType, typename AccumType, std::size_t BlockRows,
          std::size_t BlockCols>
static constexpr inline bool hasAssemblyVersion() {
  constexpr bool is4x4 = BlockRows == 4 && BlockCols == 4;
  constexpr bool is8x8 = BlockRows == 8 && BlockCols == 8;
  constexpr bool is16x16 = BlockRows == 16 && BlockCols == 16;
  constexpr bool floatInputs = std::is_same<FPType, float>();
  return is4x4 || is8x8 || (!floatInputs && is16x16);
}

namespace popsparse {

template <typename FPType, typename AccumType, std::size_t BlockRows,
          std::size_t BlockCols>
class [[poplar::constraint("elem(*q) != elem(*s)")]] SparseDenseMatMulBlockGradA
    : public SupervisorVertexIf<
          hasAssemblyVersion<FPType, AccumType, BlockRows, BlockCols>() &&
          ASM_CODELETS_ENABLED> {

  using MetaInfoType = unsigned short;
  using MetaInfo = popsparse::BlockMetaInfo<MetaInfoType>;
  constexpr static std::size_t rAlignmentRequirement = 8;
  constexpr static std::size_t qAlignmentRequirement = 8;
  constexpr static bool qInInterleavedMem = std::is_same<FPType, half>();

public:
  SparseDenseMatMulBlockGradA();

  // Pointers to buckets of sparse input values in r.
  Vector<Input<Vector<FPType, ONE_PTR, rAlignmentRequirement>>, ONE_PTR> r;
  // Pointers to buckets of meta-info describing how to process the given
  // inputs.
  Vector<Input<Vector<MetaInfoType, ONE_PTR>>, SHORT_SPAN> metaInfo;
  // Single pointer to dense grad s. Layout of elements in memory expected to
  // be {Y,Z}.
  Input<Vector<FPType, ONE_PTR, 8>> s;
  // Single pointer to dense output q. Layout of elements in memory expected to
  // be {X,Z}.
  // We may use this in multiple passes so this needs to be an InOut edge.
  InOut<Vector<AccumType, ONE_PTR, qAlignmentRequirement, qInInterleavedMem>> q;
  // The sub-group id that should be processed by this vertex.
  const MetaInfoType subGroupIdToProcess;
  // Multiple of 32-bits in q to zero. Set to zero if no zeroing required.
  const unsigned short zeroInfo;

  // NOTE! This entry must be at this position relative to the ones above
  // as the ASM codelets assume this for zeroing partials
  Input<Vector<unsigned short, ONE_PTR>> offsetAndNumZByWorker;

  // zStrideInQ : Stride in multiples of 64-bits between elements of Z in Q
  unsigned short zStrideInQ;
  // zStrideInS : Stride in multiples of 64-bits between elements of Z in S
  unsigned short zStrideInS;

  IS_EXTERNAL_CODELET(
      (hasAssemblyVersion<FPType, AccumType, BlockRows, BlockCols>()));

  static constexpr auto fpTypeSize = std::is_same<FPType, float>::value ? 4 : 2;
  static constexpr auto accumTypeSize =
      std::is_same<AccumType, float>::value ? 4 : 2;

  static void workerCompute(
      unsigned wid, unsigned zStrideInQ, unsigned zStrideInS, unsigned offsetZ,
      unsigned numZ, unsigned offsetXInQ, unsigned offsetYInS, AccumType *q,
      const FPType *r, const FPType *s) {
    // The following pointers could be calculated once and retained for each
    // worker
    q += (zStrideInQ * (8 / accumTypeSize) * offsetZ);
    s += (zStrideInS * (8 / fpTypeSize) * offsetZ);

    for (std::size_t z = 0; z < numZ; ++z) {
      for (std::size_t blockRow = 0; blockRow < BlockRows; ++blockRow) {
        auto sum = q[blockRow + offsetXInQ];
        for (std::size_t blockCol = 0; blockCol < BlockCols; ++blockCol) {
          sum += static_cast<float>(r[blockRow * BlockCols + blockCol]) *
                 static_cast<float>(s[offsetYInS + blockCol]);
        }
        q[blockRow + offsetXInQ] = sum;
      }
      q += zStrideInQ * (8 / accumTypeSize);
      s += zStrideInS * (8 / fpTypeSize);
    }
  }

  bool compute() {
    // Zero outputs if requested.
    const unsigned bytesPerElem = 8;
    static_assert((accumTypeSize * BlockRows * BlockCols) % bytesPerElem == 0,
                  "Size in bytes of q not divisible by bytes per zero elem");
    for (unsigned i = 0; i < zeroInfo * (bytesPerElem / accumTypeSize); ++i) {
      q[i] = 0;
    }

    for (std::size_t bucket = 0; bucket < metaInfo.size(); ++bucket) {
      const MetaInfoType *metaInfoBucketIter = &metaInfo[bucket][0];
      const FPType *rBucketIter = &r[bucket][0];

      for (;;) {
        const auto *subGroupEntry =
            reinterpret_cast<const MetaInfo::SubGroupEntry *>(
                metaInfoBucketIter);
        if (subGroupEntry->id == MetaInfo::endSubGroupId) {
          break;
        }

        if (subGroupEntry->id == subGroupIdToProcess) {
          const auto *r = rBucketIter;
          const auto *gradWWorkerEntries =
              reinterpret_cast<const MetaInfo::GradWWorkerEntry *>(
                  subGroupEntry + 1);
          const auto *outputEntry =
              reinterpret_cast<const MetaInfo::OutputEntry *>(
                  gradWWorkerEntries + subGroupEntry->numGradWWorkers);
          for (std::size_t xBlock = 0; xBlock < subGroupEntry->numXm1 + 1;
               ++xBlock) {
            const auto *inputEntry =
                reinterpret_cast<const MetaInfo::InputEntry *>(outputEntry + 1);
            for (std::size_t yBlock = 0; yBlock < outputEntry->numYm1 + 1;
                 ++yBlock) {
              for (unsigned wid = 0; wid < CTXT_WORKERS; ++wid) {
                const auto offsetZ = offsetAndNumZByWorker[wid * 2];
                const auto numZ = offsetAndNumZByWorker[wid * 2 + 1];
                workerCompute(wid, zStrideInQ, zStrideInS, offsetZ, numZ,
                              inputEntry->offsetYInS, outputEntry->offsetXInQ,
                              &q[0], r, &s[0]);
              }
              ++inputEntry;
              r += BlockRows * BlockCols;
            }
            outputEntry =
                reinterpret_cast<const MetaInfo::OutputEntry *>(inputEntry);
          }
        }
        rBucketIter += subGroupEntry->offsetToNextSubGroupSparseEntries;
        metaInfoBucketIter += subGroupEntry->offsetToNextSubGroupMetaInfo;
      }
    }
    return true;
  }
};

template class SparseDenseMatMulBlockGradA<half, float, 4, 4>;
template class SparseDenseMatMulBlockGradA<half, half, 4, 4>;
template class SparseDenseMatMulBlockGradA<float, float, 4, 4>;
template class SparseDenseMatMulBlockGradA<half, float, 8, 8>;
template class SparseDenseMatMulBlockGradA<half, half, 8, 8>;
template class SparseDenseMatMulBlockGradA<float, float, 8, 8>;
template class SparseDenseMatMulBlockGradA<half, half, 16, 16>;
template class SparseDenseMatMulBlockGradA<half, float, 16, 16>;
template class SparseDenseMatMulBlockGradA<float, float, 16, 16>;

} // end namespace popsparse
