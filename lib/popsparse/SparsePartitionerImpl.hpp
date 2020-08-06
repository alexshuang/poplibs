// Copyright (c) 2020 Graphcore Ltd. All rights reserved.
//
// Contains public headers for Sparse Partitioner

#ifndef _poplibs_popsparse_SparsePartitionerImpl_hpp_
#define _poplibs_popsparse_SparsePartitionerImpl_hpp_

#include "SparsePartitionerOptions.hpp"
#include "SparseStorageInternal.hpp"
#include "poplar/Interval.hpp"
#include "poplar/OptionFlags.hpp"
#include "poplar/Target.hpp"
#include "poplar/Type.hpp"
#include "popsparse/SparseStorageFormats.hpp"
#include <string>
#include <vector>

namespace popsparse {

template <typename T> struct PNBucketsImpl {
  // Buckets with offset in nz values
  std::vector<PNBucket> pnBuckets;
  // Non zero values
  std::vector<T> nzValues;
};

// The partitioner partitions a Fully Connected layer or a standalone
// matrix multiplication.
//
// If Q = R * S is the matrix multiplication in stand-alone mode, or,  is the
// Fwd phase in a fully connected layer with dimensions of Q, R, S
// [X, Z], [X, Y], and [Y, Z] respectively, then the parition of X, Y and Z
// is defined by sets Ix, Iy, and Iz respectively.
//
// Consecutive entries in the set give the starting positions of the
// partitions of a dimensions.
class PartitionerImpl {
  // number of X dimensions - rows in  sparse matrix R
  std::size_t numX;

  // number of Y dimension - columns in sparse matrix R
  std::size_t numY;

  // number of Z dimension - columns in output matrix Q
  std::size_t numZ;

  // grain size for X dimension
  std::size_t grainX;

  // grain size of Y dimension
  std::size_t grainY;

  // grain size of Z dimension
  std::size_t grainZ;

  // The splits the planner created for X dimension
  std::vector<std::size_t> xSplits;

  // The splits the planner creates for Y dimension
  std::vector<std::size_t> ySplits;

  // The splits the planner creates for Z dimension
  std::vector<std::size_t> zSplits;

  // meta information bucket
  std::size_t metaInfoBucketElements;

  // meta information bucket elements for GradA if shared buckets are not
  // enabled
  std::size_t metaInfoBucketElementsGradA;

  // If set use meta-info format for block-sparsity otherwise uses
  // element-wise format.
  bool useBlockMetaInfoFormat;

  // If set uses actual worker split every time costs for a partition are
  // evaluated. This will give exact cost as the final "real" allocation, but
  // is expensive to compute. If not set, then all workers are assumed to be
  // used and the final allocation will actually be lower.
  bool useActualWorkerSplitCosts{false};

  // A test mode to force buckets to spill
  bool forceBucketSpills{false};

  // Optimise bucket overflow allocation for speed. Overflow allocation would
  // attempt to allocate buckets that have the shortest distance to travel
  bool optimiseForSpeed{true};

  // Number of workers per PN
  std::size_t numWorkerContexts;

  // Number of non-zero elements
  std::size_t nzElementsBucketElements;

  // Number of buckets per Z split. This is used in the case when we want
  // multiple buckets/tile. We can have only bucket per PN although on a
  // physical PN there could be at most this number of buckets.
  std::size_t bucketsPerZ{1};

  bool gradAEnabled{false};
  bool gradWEnabled{false};
  bool sharedBuckets{false};

  poplar::Type dataType{poplar::HALF};
  poplar::Type accumType{poplar::FLOAT};

  // creates a partition for each PN for a CSR representation.
  std::vector<TilePartition> getTilePartitions(const CSRInternal &matrix) const;

  void balanceBuckets(std::vector<PNBucket> &pnBuckets) const;

public:
  PartitionerImpl(const std::vector<std::size_t> &dimensions,
                  const std::vector<std::size_t> &grainSizes,
                  const std::vector<std::size_t> &xSplits_,
                  const std::vector<std::size_t> &ySplits_,
                  const std::vector<std::size_t> &zSplits_,
                  std::size_t metaInfoBucketElements_,
                  std::size_t metaInfoBucketElementsGradA_,
                  std::size_t nzElementsBucketElements_,
                  std::size_t numWorkerContexts_, std::size_t bucketsPerZ_,
                  bool useBlockMetaInfoFormat, bool includeGradA_,
                  bool includeGradW_, bool sharedBuckets_,
                  const poplar::Type &dataType_, const poplar::Type &accumType_,
                  const PartitionerOptions &options);

  // Create buckets for a CSC matrix
  template <typename T>
  PNBucketsImpl<T> createBuckets(const CSCMatrix<T> &matrix_) const;

  // creates buckets for a CSR matrix
  template <typename T>
  PNBucketsImpl<T> createBuckets(const CSRMatrix<T> &matrix_) const;

  // creates buckets for a COO matrix
  template <typename T>
  PNBucketsImpl<T> createBuckets(const COOMatrix<T> &matrix_) const;

  // Build real metainformation and NZ value buckets from a single PN bucket
  // for Forward pass.
  template <typename T>
  std::pair<std::vector<std::size_t>, std::vector<T>>
  bucketForForward(const PNBucket &pnBucket, const std::vector<T> &nzValues,
                   const std::string &debugStr = "") const;

  // Build real buckets as required by implementation for Forward and GradW
  // The first in the output pair is the metainformation buckets and the
  // second the NZ bucket
  template <typename T>
  std::pair<std::vector<std::vector<std::size_t>>, std::vector<std::vector<T>>>
  bucketsForForward(const PNBucketsImpl<T> &pnBucketsImpl,
                    const std::string &debugStr = "") const;

  // Build real metainformation bucket for a single PN bucket for a GradA pass
  template <typename T>
  std::vector<std::size_t>
  bucketForGradA(const PNBucket &pnBuckets, const std::vector<T> &nzValues,
                 const std::string &debugStr = "") const;

  // Build buckets as required by implementation GradA. The NZ values are the
  // same the forward and onlythe meta information containing the transposition
  // information is included.
  template <typename T>
  std::vector<std::vector<std::size_t>>
  bucketsForGradA(const PNBucketsImpl<T> &pnBucketsImpl,
                  const std::string &debugStr = "") const;

  // Creates a pair of flat bucket for metaInfo and NZ values
  // The metaInfo bucket contains the following and in that order
  //   - distance triplet for Fwd
  //   - distance triplet for GradA (if GradA is enabled)
  //   - distance triplet for GradW (if GradW is enabled)
  //   - meta info for Fwd  (includes GradW info if enabled) for tile PN0
  //   - meta info for GradA (if GradA is enabled and Shared buckets are
  //                          disabled) for PN0
  //   - meta info for Fwd  (includes GradW info if enabled) for tile PN1
  //   - meta info for GradA (if GradA is enabled and Shared buckets are
  //                          disabled) for PN1
  //   - ... remaining PNs in order
  //
  // The NZ value bucket for each tile
  template <typename T>
  std::pair<std::vector<std::size_t>, std::vector<T>>
  bucketImplAllPasses(const PNBucketsImpl<T> &pnBucketsImpl,
                      const std::string &debugStr = "") const;

  // Overflow information for Fwd. This gives the implementation specific
  // information on the max distance of overflow bucket. The information is
  // represented as a 3-tuple with:
  //   first entry : max distance between ORGs
  //   second entry : max distance within an ORG
  //   third entry : max distance within S-ORG
  std::vector<std::size_t>
  overflowInfoForFwd(const std::vector<PNBucket> &pnBuckets) const;

  // Overflow information for GradA. This gives the implementation specific
  // information on the max distance of overflow bucket. The information is
  // represented as a 3-tuple with:
  //   first entry : max distance between ORGs
  //   second entry : max distance within an ORG
  //   third entry : max distance within S-ORG
  // The buckets given here must be the buckets for the FWD as we always use
  // joint plans.
  std::vector<std::size_t>
  overflowInfoForGradA(const std::vector<PNBucket> &pnBuckets) const;

  // Overflow information for GradW. This gives the implementation specific
  // information on the max distance of overflow bucket. The information is
  // represented as a 3-tuple with:
  //   first entry : max distance between ORGs
  //   second entry : max distance within an ORG
  //   third entry : max distance within S-ORG
  std::vector<std::size_t>
  overflowInfoForGradW(const std::vector<PNBucket> &pnBuckets) const;

  // create COO matrix from buckets
  template <typename T>
  COOMatrix<T> bucketsToCOOMatrix(const std::vector<std::size_t> &metaInfo,
                                  const std::vector<T> &nzValues) const;

  // create CSR matrix from buckets
  template <typename T>
  CSRMatrix<T> bucketsToCSRMatrix(const std::vector<std::size_t> &metaInfo,
                                  const std::vector<T> &nzValues) const;

  // create CSC matrix from buckets
  template <typename T>
  CSCMatrix<T> bucketsToCSCMatrix(const std::vector<std::size_t> &metaInfo,
                                  const std::vector<T> &nzValues) const;
};

// Fixed metainfo overhead in number of elements
std::size_t fixedMetaInfoCost(bool useBlockMetaInfoFormat,
                              std::size_t numWorkers, bool gradWEnabled);

} // namespace popsparse
#endif // _poplibs_popsparse_SparsePartitionerImpl_hpp_
