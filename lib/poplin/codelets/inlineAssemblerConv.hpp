// Copyright (c) 2021 Graphcore Ltd. All rights reserved.

#if __IPU_ARCH_VERSION__ >= 21

// Bit to force all accumulators to zero when written in FP_CLR register
#define ZAACC_BITMASK (CSR_W_FP_CLR__ZAACC__MASK << CSR_W_FP_CLR__ZAACC__SHIFT)

#define DELTAN_OFFSET_BITS 20
#define DELTAN_OFFSET_MASK ((1 << DELTAN_OFFSET_BITS) - 1)

static __attribute__((always_inline)) void
setFp8Format(const MetadataType weightsMetadata,
             const MetadataType inMetadata) {
  auto format = (0x2 & (inMetadata >> 6)) | (weightsMetadata >> 7);
  __builtin_ipu_put(format, CSR_S_FP_INFMT__INDEX);
}

static __attribute__((always_inline)) void
setFp8Scale(const MetadataType weightsMetadata, const MetadataType inMetadata) {
  // Scale is the sum of scales, as
  // we compute: half(input*weights) * 2^(scaleIn+scaleWeights)
  __builtin_ipu_put(weightsMetadata + inMetadata, CSR_S_FP_ISCL__INDEX);
}

template <bool use128BitLoad, unsigned convUnits>
static __attribute__((always_inline)) void
ampLoadWeights(const quarter *weights) {}

template <> void ampLoadWeights<false, 16>(const quarter *weights) {

  __builtin_ipu_put(reinterpret_cast<unsigned>(weights), CSR_S_CCCSLOAD__INDEX);
  asm volatile(
      R"l(  // 0th conv unit, 0th out channel
            ld64putcs    0    // Phase 0
            ld64putcs    1    // Phase 1
            ld64putcs    2    // Phase 2
            ld64putcs    3    // Phase 3
            // 0th conv unit,  1st out channel
            ld64putcs    4  // Phase 0
            ld64putcs    5  // Phase 1
            ld64putcs    6  // Phase 2
            ld64putcs    7  // Phase 3
            // 0th conv unit, 2nd out channel
            ld64putcs    32  // Phase 0
            ld64putcs    33  // Phase 1
            ld64putcs    34  // Phase 2
            ld64putcs    35  // Phase 3
            // 0th conv unit, 3rd out channel
            ld64putcs    32+4  // Phase 0
            ld64putcs    33+4  // Phase 1
            ld64putcs    34+4  // Phase 2
            ld64putcs    35+4  // Phase 3

            // 1st conv unit, 0th out channel
            ld64putcs    8    // Phase 0
            ld64putcs    9    // Phase 1
            ld64putcs    10    // Phase 2
            ld64putcs    11    // Phase 3
            // 1st conv unit,  1st out channel
            ld64putcs    12  // Phase 0
            ld64putcs    13  // Phase 1
            ld64putcs    14  // Phase 2
            ld64putcs    15  // Phase 3
            // 1st conv unit, 2nd out channel
            ld64putcs    40  // Phase 0
            ld64putcs    41  // Phase 1
            ld64putcs    42  // Phase 2
            ld64putcs    43  // Phase 3
            // 1st conv unit, 3rd out channel
            ld64putcs    44  // Phase 0
            ld64putcs    45  // Phase 1
            ld64putcs    46  // Phase 2
            ld64putcs    47  // Phase 3

            // 2nd conv unit, 0th out channel
            ld64putcs    16    // Phase 0
            ld64putcs    17    // Phase 1
            ld64putcs    18    // Phase 2
            ld64putcs    19    // Phase 3
            // 2nd conv unit,  1st out channel
            ld64putcs    20  // Phase 0
            ld64putcs    21  // Phase 1
            ld64putcs    22  // Phase 2
            ld64putcs    23  // Phase 3
            // 2nd conv unit, 2nd out channel
            ld64putcs    48  // Phase 0
            ld64putcs    49  // Phase 1
            ld64putcs    50  // Phase 2
            ld64putcs    51  // Phase 3
            // 2nd conv unit, 3rd out channel
            ld64putcs    52  // Phase 0
            ld64putcs    53  // Phase 1
            ld64putcs    54  // Phase 2
            ld64putcs    55  // Phase 3

            // 3rd conv unit, 0th out channel
            ld64putcs    24    // Phase 0
            ld64putcs    25    // Phase 1
            ld64putcs    26    // Phase 2
            ld64putcs    27    // Phase 3
            // 3rd conv unit,  1st out channel
            ld64putcs    28  // Phase 0
            ld64putcs    29  // Phase 1
            ld64putcs    30  // Phase 2
            ld64putcs    31  // Phase 3
            // 3rd conv unit, 2nd out channel
            ld64putcs    56  // Phase 0
            ld64putcs    57  // Phase 1
            ld64putcs    58  // Phase 2
            ld64putcs    59  // Phase 3
            // 3rd conv unit, 3rd out channel
            ld64putcs    60  // Phase 0
            ld64putcs    61  // Phase 1
            ld64putcs    62  // Phase 2
            ld64putcs    63  // Phase 3
      )l"
      :
      :
      :);
}
template <> void ampLoadWeights<true, 16>(const quarter *weights) {
  __builtin_ipu_put(reinterpret_cast<unsigned>(weights), CSR_S_CCCSLOAD__INDEX);
  asm volatile(
      R"l(
            // 0th conv unit, 0th out channel
            ld128putcs    0    // Phase 0,1
            ld128putcs    2    // Phase 2,3
            // 0th conv unit,  1st out channel
            ld128putcs    4  // Phase 0,1
            ld128putcs    6  // Phase 2,3
            // 0th conv unit, 2nd out channel
            ld128putcs    32  // Phase 0,1
            ld128putcs    34  // Phase 2,3
            // 0th conv unit, 3rd out channel
            ld128putcs    32+4  // Phase 0,1
            ld128putcs    34+4  // Phase 2,3

            // 1st conv unit, 0th out channel
            ld128putcs    8    // Phase 0,1
            ld128putcs    10    // Phase 2,3
            // 1st conv unit,  1st out channel
            ld128putcs    12  // Phase 0,1
            ld128putcs    14  // Phase 2,3
            // 1st conv unit, 2nd out channel
            ld128putcs    40  // Phase 0,1
            ld128putcs    42  // Phase 2,3
            // 1st conv unit, 3rd out channel
            ld128putcs    44  // Phase 0,1
            ld128putcs    46  // Phase 2,3

            // 2nd conv unit, 0th out channel
            ld128putcs    16    // Phase 0,1
            ld128putcs    18    // Phase 2,3
            // 2nd conv unit,  1st out channel
            ld128putcs    20  // Phase 0,1
            ld128putcs    22  // Phase 2,3
            // 2nd conv unit, 2nd out channel
            ld128putcs    48  // Phase 0,1
            ld128putcs    50  // Phase 2,3
            // 2nd conv unit, 3rd out channel
            ld128putcs    52  // Phase 0,1
            ld128putcs    54  // Phase 2,3

            // 3rd conv unit, 0th out channel
            ld128putcs    24    // Phase 0,1
            ld128putcs    26    // Phase 2,3
            // 3rd conv unit,  1st out channel
            ld128putcs    28  // Phase 0,1
            ld128putcs    30  // Phase 2,3
            // 3rd conv unit, 2nd out channel
            ld128putcs    56  // Phase 0,1
            ld128putcs    58  // Phase 2,3
            // 3rd conv unit, 3rd out channel
            ld128putcs    60  // Phase 0,1
            ld128putcs    62  // Phase 2,3
      )l"
      :
      :
      :);
}

template <typename UnsignedType> struct WorkerState1x1 {
  const quarter *inChanPtr;
  half *outChanPtr;
  unsigned strides;
  const UnsignedType *partition;
};

struct WorkerStateNx1 {
  const quarter *inChanPtr;
  half *outChanPtr;
  unsigned strides;
  const unsigned *partitionList;
  const unsigned *partitionBase;
};

template <bool ZeroPartials>
static __attribute__((always_inline)) void
convQuarterHalfLoop(const quarter *inPtr, half *outPtr, unsigned loops,
                    unsigned strides) {
  // Packed strides in `strides`:
  // b[0,10) = inputsStride  (inputs read only, select with 01)
  // b[10,20) = partialsInOutStride (partials in/out only, select with 10)
  // b[20,30) = 0 for no stride (reading dummy partials,inputs select with 11)
  auto triAddr = __builtin_ipu_tapack(inPtr, outPtr, outPtr);
  if constexpr (ZeroPartials == false) {
    asm volatile(
        R"l(
              .macro amp OP1 OP2 OP3 OP4
                f8v8hihov4amp \OP1 , \OP2 , \OP3 , \OP4
              .endm
              .equ ZERO_PARTIALS, 0
    )l" ::
            :);
  } else {
    asm volatile(
        R"l(
              .macro amp OP1 OP2 OP3 OP4
                f8v8hihov4amp \OP1 , \OP2 , $azeros, \OP4
              .endm
              .equ ZERO_PARTIALS, 1
    )l" ::
            :);
  }
  asm volatile(
      R"l(
            // Decrement the counter, exit if nothing to do
            // Use FP_CLR to clear the accumulators
            {brnzdec %[loops], 1f
             setzi $a0, %[ZAACC_MASK]}
             bri 8f
          1:
          .if ZERO_PARTIALS == 0
            // General addressing pattern for partials, outputs:
            // Forward 1 (3 times), back inOutStride
            //  8 9 a b, 4 5 6 7,  0 1 2 3

            // Prime with partials - Each is a read of the partials,
            // a dummy read of the input with no pointer increment,
            // and a call to the amp instruction with phase!=0
            // Loads to $a0:1 are dummy loads as we can't write
            // twice to $azeros in one bundle
            // ld2x64pace: 0bxxyy stride select:
            // xx=partialsInPtr, yy=inPtr

            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b0011
              uput $FP_CLR, $a0}
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b0011
             amp $azeros, $azeros, $a2:3, %[TAMP_F16V4_E4_P1]}

            {ld2x64pace $a0:1, $a2:3,  %[triAddr]+=, %[strides], 0b0011
             amp $azeros, $azeros, $a2:3, %[TAMP_F16V4_E4_P1]}
            // Check for the case of 1 output
            brnzdec %[loops], 1f
            // There is only 1 output - avoid the stride in the partials load
            // to avoid overreads when we fetch unused partials
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b1111
             amp $azeros, $azeros, $a2:3, %[TAMP_F16V4_E4_P1]}
          .else
            // Check for the case of 1 output
            {brnzdec %[loops], 2f
             uput $FP_CLR, $a0}
          .endif

            // This is the first genuine load of the input, and increments the
            // pointer
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b0000
             amp $azeros, $azeros, $a2:3, %[TAMP_F16V4_E4_P1]}

             // Push in a genuine input (and next set of partials)
             // Phase 0..3
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b0000
             amp $azeros, $a0:1, $a2:3, %[TAMP_F16V4_E4_P0]}

            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b0000
             amp $azeros, $a0:1, $a2:3, %[TAMP_F16V4_E4_P1]}

            // For 1 output avoid striding the partials pointer and then
            // skip the loop body
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b1101
             amp $azeros, $a0:1, $a2:3, %[TAMP_F16V4_E4_P2]}

            // $a0:1 read, $a2:3 dummy read (Can't write $azeros twice)
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b1100
             amp $azeros, $a0:1, $a2:3, %[TAMP_F16V4_E4_P3]}

            {bri 7f
             amp $a4:5, $azeros, $azeros, %[TAMP_F16V4_E4_P0]}


          // Continue: 2 or more outputs
          1:
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b1011
             amp $azeros, $azeros, $a2:3, %[TAMP_F16V4_E4_P1]}
          2:
            // This is the first genuine load of the input, and increments the
            // pointer
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b0000
             amp $azeros, $azeros, $a2:3, %[TAMP_F16V4_E4_P1]}

             // Push in a genuine input (and next set of partials)
             // Phase 0..3
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b0000
             amp $azeros, $a0:1, $a2:3, %[TAMP_F16V4_E4_P0]}

            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b0000
             amp $azeros, $a0:1, $a2:3, %[TAMP_F16V4_E4_P1]}


          // Check for the case of 2 outputs
            brnzdec %[loops], 1f

            // There are 2 outputs - avoid the stride in the partials load
            // to avoid overreads when we fetch unused partials
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b1001
             amp $azeros, $a0:1, $a2:3, %[TAMP_F16V4_E4_P2]}

            // $a0:1 read, $a2:3 dummy read (Can't write $azeros twice)
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b1100
             amp $azeros, $a0:1, $a2:3, %[TAMP_F16V4_E4_P3]}

            {ld2x64pace $a4:5, $a6:7, %[triAddr]+=, %[strides], 0b0000
             amp $a0:1, $a0:1, $a4:5, %[TAMP_F16V4_E4_P0]}
            bri 6f
          .align 8
            nop // Repeat alignment

          1:
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b1001
             amp $azeros, $a0:1, $a2:3, %[TAMP_F16V4_E4_P2]}

            // $a0:1 read, $a2:3 dummy read (Can't write $azeros twice)
            {ld2x64pace $a0:1, $a2:3, %[triAddr]+=, %[strides], 0b1100
             amp $azeros, $a0:1, $a2:3, %[TAMP_F16V4_E4_P3]}

            ld2x64pace $azeros, $a4:5, %[triAddr]+=, %[strides], 0b0011

            // One more partials read to move to an alternate memory segment
            // to the writes so we can use ld2xst64pace in the inner loop
            ld2x64pace $azeros, $a2:3, %[triAddr]+=, %[strides], 0b0011

            {ld2x64pace $a4:5, $a6:7, %[triAddr]+=, %[strides], 0b0000
             amp $a0:1, $a0:1, $a4:5, %[TAMP_F16V4_E4_P0]}

            // Loop is the first point the output is actually stored,
            // Continue loading inputs and partials and striding pointers
            rpt %[loops], (2f - 1f) / 8 - 1
          1:
            // ld2xst64pace: 0bxxyyzz stride select:
            // xx=outPtr, yy=partialsInPtr, zz=inPtr
            {ld2xst64pace $a0:3, $a0:1, %[triAddr]+=, %[strides], 0b001000
             amp $a4:5, $a4:5, $a2:3, %[TAMP_F16V4_E4_P1]}

            {ld2xst64pace $a4:7, $a4:5, %[triAddr]+=, %[strides], 0b000001
             amp $a0:1, $a0:1, $a6:7, %[TAMP_F16V4_E4_P2]}

            {ld2xst64pace $a0:3, $a0:1, %[triAddr]+=, %[strides], 0b000000
             amp $a4:5, $a4:5, $a2:3, %[TAMP_F16V4_E4_P3]}

            {ld2xst64pace $a4:7, $a4:5, %[triAddr]+=, %[strides], 0b100000
             amp $a0:1, $a0:1, $a6:7, %[TAMP_F16V4_E4_P0]}
          2:

            {ld2xst64pace $a0:3, $a0:1, %[triAddr]+=, %[strides], 0b001000
             amp $a4:5, $a4:5, $a2:3, %[TAMP_F16V4_E4_P1]}

            // Now we have read all the partials that are needed so
            // don't overread (Different to loop body)
            // ldst64pace: 0bxxyy stride select:
            // xx=inPtr, yy=outPtr
            {ldst64pace $a4:5, $a4:5, %[triAddr]+=, %[strides], 0b0001
             amp $a0:1, $a0:1, $a6:7, %[TAMP_F16V4_E4_P2]}

            {ldst64pace $a0:1, $a0:1, %[triAddr]+=, %[strides], 0b0000
             amp $a4:5, $a4:5, $a2:3, %[TAMP_F16V4_E4_P3]}

            {ldst64pace $a4:5, $a4:5, %[triAddr]+=, %[strides], 0b1000
             amp $a0:1, $a0:1, $azeros, %[TAMP_F16V4_E4_P0]}

          6:
            {ldst64pace $a0:1, $a0:1, %[triAddr]+=, %[strides], 0b0000
             amp $a4:5,  $a4:5, $azeros, %[TAMP_F16V4_E4_P1]}
            {ldst64pace $a0:1, $a4:5, %[triAddr]+=, %[strides], 0b0011
              amp $a4:5,  $a0:1, $azeros, %[TAMP_F16V4_E4_P2]}
            // Use the last input, no more need to load
            {st64pace $a4:5, %[triAddr]+=, %[strides], 0b00
              amp $a4:5,  $a0:1, $azeros, %[TAMP_F16V4_E4_P3]}

            // Result output only
            {st64pace $a4:5, %[triAddr]+=, %[strides], 0b10
             amp $a4:5, $azeros, $azeros, %[TAMP_F16V4_E4_P0]}
         7:
            {st64pace $a4:5, %[triAddr]+=, %[strides], 0b00
              amp $a4:5,  $azeros, $azeros, %[TAMP_F16V4_E4_P1]}
            {st64pace $a4:5, %[triAddr]+=, %[strides], 0b00
              amp $a4:5,  $azeros, $azeros, %[TAMP_F16V4_E4_P2]}
            {st64pace $a4:5, %[triAddr]+=, %[strides], 0b00
              amp $a4:5,  $azeros, $azeros, %[TAMP_F16V4_E4_P3]}
            st64pace $a4:5, %[triAddr]+=, %[strides], 0b00
          8:

          // Remove macro definition to avoid later re-definition issues
          .purgem amp
        )l"
      : [loops] "+r"(loops), [strides] "+r"(strides), [triAddr] "+r"(triAddr)
      : [TAMP_F16V4_E4_P0] "i"(TAMP_F16V4_E4_P0),
        [TAMP_F16V4_E4_P1] "i"(TAMP_F16V4_E4_P1),
        [TAMP_F16V4_E4_P2] "i"(TAMP_F16V4_E4_P2),
        [TAMP_F16V4_E4_P3] "i"(TAMP_F16V4_E4_P3),
        [ZAACC_MASK] "i"(ZAACC_BITMASK)
      // As we want to access a group of 4 registers and also
      // the first/second 2 of the group we can't use C variables of types
      // float4, float2 or similar.  So just clobber all a-registers
      : "memory", "$a0:1", "$a2:3", "$a4:5", "$a6:7");
}

template <typename WorkerStateType>
static __attribute__((always_inline)) WorkerStateType *workerState(void) {
  WorkerStateType *state;
  asm volatile(" mov %[state], $mvertex_base\n" : [state] "=r"(state) : :);
  return state;
}

#define RUN_ALL(NAME_STR, STATE)                                               \
  {                                                                            \
    unsigned workerAddress;                                                    \
    asm volatile(" setzi  %[workerAddress], " NAME_STR "\n"                    \
                 " runall %[workerAddress], %[state], 0\n"                     \
                 : [workerAddress] "=&r"(workerAddress)                        \
                 : [state] "r"(STATE)                                          \
                 :);                                                           \
  }

#define SET_ADDR(RESULT, NAME_STR)                                             \
  asm volatile(" setzi  %[workerAddress], " NAME_STR "\n"                      \
               : [workerAddress] "=r"(RESULT)                                  \
               :                                                               \
               :);

template <typename T>
static __attribute__((always_inline)) void runAll(const unsigned *workerAddress,
                                                  const T *state) {
  asm volatile(" runall %[workerAddress], %[state], 0\n"
               :
               : [workerAddress] "r"(workerAddress), [state] "r"(state)
               :);
}

static __attribute__((always_inline)) void syncWorkers(void) {
  asm volatile(
      " sync   %[sync_zone]\n" ::[sync_zone] "i"(TEXCH_SYNCZONE_LOCAL));
}

static __attribute__((always_inline)) unsigned getWid(void) {
  unsigned result;
  return __builtin_ipu_get(CSR_W_WSR__INDEX) & CSR_W_WSR__CTXTID_M1__MASK;
}

static __attribute__((always_inline)) unsigned long long
packStrides(unsigned stride0, unsigned stride1, unsigned numStrideBits) {
  return stride0 | (stride1 << numStrideBits);
}

static __attribute__((always_inline)) unsigned long long
packStrides(unsigned stride0, unsigned stride1, unsigned stride2,
            unsigned numStrideBits) {
  return stride0 | (stride1 << numStrideBits) |
         (stride2 << (numStrideBits * 2));
}

constexpr unsigned stocasticRoundingMask =
    ~(CSR_S_FP_ICTL__ESR__MASK << CSR_S_FP_ICTL__ESR__SHIFT);

static __attribute__((always_inline)) unsigned getFPICTL(void) {
  return __builtin_ipu_get(CSR_S_FP_ICTL__INDEX);
}

static __attribute__((always_inline)) void putFPICTL(unsigned value) {
  __builtin_ipu_put(value, CSR_S_FP_ICTL__INDEX);
}

#endif
