// Copyright (c) 2018 Graphcore Ltd. All rights reserved.
#ifdef __IPU__
// No accumulator versions of reduction specialisations 0,1
// Covering operations such as max, min...
//
// The difference in these functions vs ReductionSpecial01Acc.S is that the accumulators
// aren't used and therefore also that the vectorwidth is 4 at most instead of 8.
//
// Fuller description in ReductionSpecail01Acc.S - but no vectorised 8, instead: 4 at most.
//
// Performance notes:
// Per reduction overhead is 25 cycles to unpack vertex state, find number of
// partials per reduction etc...
// There are around 10 cycles overhead per partial
// The largest inner loop can take between 7 and 9 cycles for 4 halves,
// depending on word alignment.
// Or 5 cycles for 2 floats, regardless of word alignment.


#include "poplar/TileConstants.hpp"
#include "poplar/AvailableVTypes.h"
#include "MathConstants.S"
#include "poplar/StackSizeDefs.hpp"

// Register, vertex state definitions
#include "ReductionsCommon.h.S"

// ld macros populate the arf (VALUES_0:4) with partial data that
// will be used as the input to accumulation instructions. Scratch
// holds the offset from an 8 byte aligned ptr
// ------------------------------------------------------- //

// ------------------------------------------------------- //
.macro ld64_MIS_2_
  and $SCRATCH2, $SCRATCH, 0x3
  brz $SCRATCH2, 1f
  ldb16step $VALUES_0, $IN_j_PTR, $SCRATCH+=, 1
  ld32step $ASCRATCH_0, $IN_j_PTR, $SCRATCH+=,1
  {ldb16step $VALUES_1, $IN_j_PTR, $SCRATCH+=, -3
   roll16 $VALUES_0, $VALUES_0, $ASCRATCH_0};
  {bri 2f; roll16 $VALUES_1, $ASCRATCH_0, $VALUES_1}
1:
  ld32 $VALUES_0, $IN_j_PTR, $SCRATCH, 0
  ld32 $VALUES_1, $IN_j_PTR, $SCRATCH, 1
2:
.endm

// Name mangling
#define REDUCE_HALF_HALF(prefix, specialisation) __runCodelet_popops__##prefix##___\
popops__\OP\()_half_half_false_popops__ReductionSpecialisation__##specialisation

#define REDUCE_FLOAT_FLOAT(prefix, specialisation) __runCodelet_popops__##prefix##___\
popops__\OP\()_float_float_false_popops__ReductionSpecialisation__##specialisation

// ------------------------------------------------------- //
// Macro to create half half
// ------------------------------------------------------- //

.macro INSTANTIATE_REDUCE_HALF_HALF INIT_HALF INSTRUCTION OP
.equ SIZE_OF_IN_TYPE, 2


.type REDUCE_HALF_HALF(Reduce,common), @function

DEF_STACK_USAGE 0 .text.REDUCE_HALF_HALF(Reduce,common)

.section .text.REDUCE_HALF_HALF(Reduce,common), "ax"
// Instantiate two variants which call the same common function
.globl REDUCE_HALF_HALF(Reduce,DEFAULT)
.type REDUCE_HALF_HALF(Reduce,DEFAULT), @function
.globl REDUCE_HALF_HALF(ScaledReduce,DEFAULT)
.type REDUCE_HALF_HALF(ScaledReduce,DEFAULT), @function
.globl REDUCE_HALF_HALF(Reduce,SCALAR___OUTPUT___REGIONS)
.type REDUCE_HALF_HALF(Reduce,SCALAR___OUTPUT___REGIONS), @function
.globl REDUCE_HALF_HALF(ScaledReduce,SCALAR___OUTPUT___REGIONS)
.type REDUCE_HALF_HALF(ScaledReduce,SCALAR___OUTPUT___REGIONS), @function

.align 4
// ************************************************* //
// Load vertex state
// ************************************************* //
REDUCE_HALF_HALF(Reduce,common):
#if defined(VECTOR_AVAIL_SCALED_PTR32) && defined(VECTORLIST_AVAIL_DELTAN)
REDUCE_HALF_HALF(Reduce,DEFAULT):
REDUCE_HALF_HALF(Reduce,SCALAR___OUTPUT___REGIONS):
#else
REDUCE_HALF_HALF(Reduce,DEFAULT):
  setzi      $SCRATCH, _Reduce_outer_loop_setup_out_align64
  bri        1f

REDUCE_HALF_HALF(Reduce,SCALAR___OUTPUT___REGIONS):
  setzi      $SCRATCH, _Reduce_outer_loop_setup_out_align32

1:
  st32       $SCRATCH, $mworker_base, $mzero, FN_REDUCE_OUTER_LOOP_SCRATCH
#endif
{
  bri        2f
  or         $SCALE, $azero, FLOAT_1_0
}
#if defined(VECTOR_AVAIL_SCALED_PTR32) && defined(VECTORLIST_AVAIL_DELTAN)
REDUCE_HALF_HALF(ScaledReduce,DEFAULT):
REDUCE_HALF_HALF(ScaledReduce,SCALAR___OUTPUT___REGIONS):
  ldz16      $SCRATCH, $mvertex_base, $mzero, SCALE_OFF/2
  setzi      $SCRATCH2, TMEM_REGION0_BASE_ADDR
  ld32       $SCALE, $SCRATCH2, $mzero, $SCRATCH
#else
REDUCE_HALF_HALF(ScaledReduce,DEFAULT):
  setzi      $SCRATCH, _Reduce_outer_loop_setup_out_align64
  bri        1f

REDUCE_HALF_HALF(ScaledReduce,SCALAR___OUTPUT___REGIONS):
  setzi      $SCRATCH, _Reduce_outer_loop_setup_out_align32

1:
  st32       $SCRATCH, $mworker_base, $mzero, FN_REDUCE_OUTER_LOOP_SCRATCH
  ld32       $SCRATCH, $mvertex_base, $mzero, SCALE_OFF/4
  ld32       $SCALE, $mzero, $SCRATCH, 0
#endif
2:
 call       $IN_j_SIZE, _Reduce_load_state_process_common

_loop_over_reductions.\@:
// ************************************************* //
// unpack offset and size
// ************************************************* //
#if defined(VECTOR_AVAIL_SCALED_PTR32) && defined(VECTORLIST_AVAIL_DELTAN)
  call       $IN_j_SIZE, _Reduce_outer_loop_setup
#else
  setzi      $IN_j_SIZE, 3f
  ld32       $SCRATCH, $mworker_base, $mzero, FN_REDUCE_OUTER_LOOP_SCRATCH
  br         $SCRATCH
#endif
3:

  and        $SCRATCH, $OUT_j_SIZE, 0x3
  st32       $SCRATCH, $mworker_base, $mzero, 0
  mul        $NUM_ELEM, $OUT_j_SIZE, SIZE_OF_IN_TYPE
  shr        $OUT_j_SIZE, $OUT_j_SIZE, 2

  brnzdec    $OUT_j_SIZE, _skip2.\@
  bri        _out_j_size_remainder.\@
_skip2.\@:

_out_j_loop.\@:
  {
    ld32       $IN_i_PTR, $mworker_base, $mzero, 1
    setzi      $ACC_0, \INIT_HALF & LDCONST_MASK
  }
  {
    ld32       $NUM_PART, $mzero, $mworker_base, 5
    or         $ACC_0, $ACC_0, \INIT_HALF & ~LDCONST_MASK
  }
  mov        $ACC_1, $ACC_0

// ************************************************* //
// Loop over iputs accumulating
// ************************************************* //
_start_num_partials_loop.\@:
  call       $SCRATCH2, _Reduce_ptr_fetch
  mul        $IN_j_SIZE, $IN_j_SIZE, SIZE_OF_IN_TYPE

_in_j_loop_start.\@:
  ld64_MIS_2_      // trashes scratch2
  {
    add        $SCRATCH, $SCRATCH, $NUM_ELEM  // need to keep track of j delta
    f16v4\INSTRUCTION $ACC_0:3, $ACC_0:3, $VALUES_0:1
  }
  cmpult     $SCRATCH2, $SCRATCH, $IN_j_SIZE
  brnz       $SCRATCH2, _in_j_loop_start.\@
  brnzdec    $NUM_PART, _start_num_partials_loop.\@

// ************************************************* //
// end of 4 vector accumulating, scale and store
// ************************************************* //

  // Always apply scale as a float to maintain accuracy
  {
    add $IN_j_DELTA, $IN_j_DELTA, 8
    f16v2tof32 $VALUES_0:1, $ACC_0
  }
  f32v2mul   $VALUES_0:1, $SCALE:B, $VALUES_0:1
  f16v2tof32 $VALUES_2:3, $ACC_1
  f32v2mul   $VALUES_2:3, $SCALE:B, $VALUES_2:3
  f32v4tof16 $VALUES_0:1, $VALUES_0:3
  st64step   $VALUES_0:1, $mzero, $OUT_j_PTR+=, 1

  brnzdec    $OUT_j_SIZE, _out_j_loop.\@

// ************************************************* //
// 4 vector remainder accumulate, scale and store
// ************************************************* //
_out_j_size_remainder.\@:
  ld32       $OUT_j_SIZE, $mworker_base, $mzero, 0

// ************************************************* //
// 2 vector remainder accumulate, scale and store
// ************************************************* //
_out_j_2_remainder.\@:
  and        $SCRATCH, $OUT_j_SIZE, 2
  brz        $SCRATCH, _out_j_1_remainder.\@

  {
    ld32       $IN_i_PTR, $mworker_base, $mzero, 1
    setzi      $ACC_0, \INIT_HALF & LDCONST_MASK
  }
  {
    ld32       $NUM_PART, $mzero, $mworker_base, 5
    or         $ACC_0, $ACC_0, \INIT_HALF & ~LDCONST_MASK
  }

_start_num_partials_loop_2_rem.\@:
  {
    call       $SCRATCH2, _Reduce_ptr_fetch
    mov        $ACC_1, $ACC_0 //clear to avoid exception despite nonuse
  }
  {
    mul        $IN_j_SIZE, $IN_j_SIZE, SIZE_OF_IN_TYPE
    // clear $VALUES_1 to avoid FP exceptions even though the result is unused
    setzi      $VALUES_1, 0
  }

_in_j_loop_start_2_rem.\@:
  ldb16 $VALUES_0, $IN_j_PTR, $SCRATCH, 0
  ldb16 $ASCRATCH_0, $IN_j_PTR, $SCRATCH, 1
  roll16 $VALUES_0, $VALUES_0, $ASCRATCH_0
  {
    add        $SCRATCH, $SCRATCH, $NUM_ELEM  // need to keep track of j delta
    f16v4\INSTRUCTION  $ACC_0:3, $ACC_0:3, $VALUES_0:1
  }
  cmpult     $SCRATCH2, $SCRATCH, $IN_j_SIZE
  brnz       $SCRATCH2, _in_j_loop_start_2_rem.\@
  brnzdec    $NUM_PART, _start_num_partials_loop_2_rem.\@

// ************************************************* //
// end of 2 vector accumulating, scale and store
// ************************************************* //

  {
    add $IN_j_DELTA, $IN_j_DELTA, 4
    f16v2tof32 $ACC_0:3, $ACC_0
  }
  // Always apply scale as a float to maintain accuracy
  f32v2mul   $VALUES_0:1, $SCALE:B, $ACC_0:3
  f32v2tof16 $VALUES_0, $VALUES_0:1
  st32step   $VALUES_0, $mzero, $OUT_j_PTR+=, 1

// ************************************************* //
// 1 vector remainder accumulate, scale and store
// ************************************************* //
_out_j_1_remainder.\@:
  and        $SCRATCH, $OUT_j_SIZE, 1
  brz        $SCRATCH, _out_j_size_end.\@

  {
    ld32       $IN_i_PTR, $mworker_base, $mzero, 1
    setzi      $ACC_0, \INIT_HALF & LDCONST_MASK
  }
  {
    ld32       $NUM_PART, $mzero, $mworker_base, 5
    or         $ACC_0, $ACC_0, \INIT_HALF & ~LDCONST_MASK
  }

_start_num_partials_loop_1_rem.\@:
  {
    call       $SCRATCH2, _Reduce_ptr_fetch
    mov        $ACC_1, $ACC_0 //clear to avoid exception despite nonuse
  }
  {
    mul        $IN_j_SIZE, $IN_j_SIZE, SIZE_OF_IN_TYPE
    // clear $VALUES_1 to avoid FP exceptions even though the result is unused
    setzi      $VALUES_1, 0
  }

_in_j_loop_start_1_rem.\@:
  ldb16      $VALUES_0, $IN_j_PTR, $SCRATCH, 0
  {
    add        $SCRATCH, $SCRATCH, $NUM_ELEM  // need to keep track of j delta
    f16v4\INSTRUCTION   $ACC_0:3, $ACC_0:3, $VALUES_0:1
  }
  cmpult     $SCRATCH2, $SCRATCH, $IN_j_SIZE
  brnz       $SCRATCH2, _in_j_loop_start_1_rem.\@
  brnzdec    $NUM_PART, _start_num_partials_loop_1_rem.\@

// ************************************************* //
// end of 1 vector accumulating, scale and store
// ************************************************* //

  {
    ldb16 $ASCRATCH_0, $OUT_j_PTR, $mzero, 1
    f16tof32  $VALUES_0, $ACC_0
  }
  // Always apply scale as a float to maintain accuracy
  f32mul $VALUES_0, $SCALE, $VALUES_0
  f32tof16 $VALUES_0, $VALUES_0
  {
    add $IN_j_DELTA, $IN_j_DELTA, 2
    sort4x16lo $VALUES_0, $VALUES_0, $ASCRATCH_0
  }
  st32 $VALUES_0, $OUT_j_PTR, $mzero, 0

_out_j_size_end.\@:
  // add num_partials to IN_i_ptr and store
  st32       $IN_i_PTR, $mworker_base, $mzero, 1
  brnzdec    $OUT_i_SIZE, _loop_over_reductions.\@
// ************************************************* //
// End of loops
// ************************************************* //
_exit.\@:
  exitz      $mzero

.size REDUCE_HALF_HALF(Reduce,common),\
              .-REDUCE_HALF_HALF(Reduce,common)

.endm
// ------------------------------------------------------- //
// ------------------------------------------------------- //
// Float Float
// ------------------------------------------------------- //
// ------------------------------------------------------- //

.macro INSTANTIATE_REDUCE_FLOAT_FLOAT INIT_FLOAT INSTRUCTION OP
.equ SIZE_OF_IN_TYPE, 4


.type REDUCE_FLOAT_FLOAT(Reduce,common), @function

DEF_STACK_USAGE 0 .text.REDUCE_FLOAT_FLOAT(Reduce,common)
.section .text.REDUCE_FLOAT_FLOAT(Reduce,common), "ax"
// Instantiate two variants which call the same common function
.globl REDUCE_FLOAT_FLOAT(Reduce,DEFAULT)
.type REDUCE_FLOAT_FLOAT(Reduce,DEFAULT), @function
.globl REDUCE_FLOAT_FLOAT(ScaledReduce,DEFAULT)
.type REDUCE_FLOAT_FLOAT(ScaledReduce,DEFAULT), @function
.globl REDUCE_FLOAT_FLOAT(Reduce,SCALAR___OUTPUT___REGIONS)
.type REDUCE_FLOAT_FLOAT(Reduce,SCALAR___OUTPUT___REGIONS), @function
.globl REDUCE_FLOAT_FLOAT(ScaledReduce,SCALAR___OUTPUT___REGIONS)
.type REDUCE_FLOAT_FLOAT(ScaledReduce,SCALAR___OUTPUT___REGIONS), @function

.align 4
// ************************************************* //
// Load vertex state
// ************************************************* //
REDUCE_FLOAT_FLOAT(Reduce,common):
#if defined(VECTOR_AVAIL_SCALED_PTR32) && defined(VECTORLIST_AVAIL_DELTAN)
REDUCE_FLOAT_FLOAT(Reduce,DEFAULT):
REDUCE_FLOAT_FLOAT(Reduce,SCALAR___OUTPUT___REGIONS):
#else
REDUCE_FLOAT_FLOAT(Reduce,DEFAULT):
  setzi      $SCRATCH, _Reduce_outer_loop_setup_out_align64
  bri        1f

REDUCE_FLOAT_FLOAT(Reduce,SCALAR___OUTPUT___REGIONS):
  setzi      $SCRATCH, _Reduce_outer_loop_setup_out_align32

1:
  st32       $SCRATCH, $mworker_base, $mzero, FN_REDUCE_OUTER_LOOP_SCRATCH
#endif
{
  bri        2f
  or         $SCALE, $azero, FLOAT_1_0
}
#if defined(VECTOR_AVAIL_SCALED_PTR32) && defined(VECTORLIST_AVAIL_DELTAN)
REDUCE_FLOAT_FLOAT(ScaledReduce,DEFAULT):
REDUCE_FLOAT_FLOAT(ScaledReduce,SCALAR___OUTPUT___REGIONS):
  ldz16      $SCRATCH, $mvertex_base, $mzero, SCALE_OFF/2
  setzi      $SCRATCH2, TMEM_REGION0_BASE_ADDR
  ld32       $SCALE, $SCRATCH2, $mzero, $SCRATCH
#else
REDUCE_FLOAT_FLOAT(ScaledReduce,DEFAULT):
  setzi      $SCRATCH, _Reduce_outer_loop_setup_out_align64
  bri        1f

REDUCE_FLOAT_FLOAT(ScaledReduce,SCALAR___OUTPUT___REGIONS):
  setzi      $SCRATCH, _Reduce_outer_loop_setup_out_align32

1:
  st32       $SCRATCH, $mworker_base, $mzero, FN_REDUCE_OUTER_LOOP_SCRATCH
  ld32       $SCRATCH, $mvertex_base, $mzero, SCALE_OFF/4
  ld32       $SCALE, $mzero, $SCRATCH, 0
#endif
2:
  call       $IN_j_SIZE, _Reduce_load_state_process_common

_loop_over_reductions.\@:
// ************************************************* //
// unpack offset and size
// ************************************************* //
#if defined(VECTOR_AVAIL_SCALED_PTR32) && defined(VECTORLIST_AVAIL_DELTAN)
  call       $IN_j_SIZE, _Reduce_outer_loop_setup
#else
  setzi      $IN_j_SIZE, 3f
  ld32       $SCRATCH, $mworker_base, $mzero, FN_REDUCE_OUTER_LOOP_SCRATCH
  br         $SCRATCH
#endif
3:
  and        $SCRATCH, $OUT_j_SIZE, 0x1
  st32       $SCRATCH, $mworker_base, $mzero, 0
  mul        $NUM_ELEM, $OUT_j_SIZE, SIZE_OF_IN_TYPE
  shr        $OUT_j_SIZE, $OUT_j_SIZE, 1

  brnzdec    $OUT_j_SIZE, _skip2.\@
  bri        _out_j_size_remainder.\@
_skip2.\@:

_out_j_loop.\@:

  {
    ld32       $IN_i_PTR, $mworker_base, $mzero, 1
    setzi      $ACC_0, \INIT_FLOAT & LDCONST_MASK
  }
  {
    ld32       $NUM_PART, $mzero, $mworker_base, 5
    or         $ACC_0, $ACC_0, \INIT_FLOAT & ~LDCONST_MASK
  }
  mov        $ACC_1, $ACC_0


// ************************************************* //
// Loop over iputs accumulating
// ************************************************* //
_start_num_partials_loop.\@:
  call       $SCRATCH2, _Reduce_ptr_fetch

  mul        $IN_j_SIZE, $IN_j_SIZE, SIZE_OF_IN_TYPE

_in_j_loop_start.\@:
  ld32 $VALUES_0, $IN_j_PTR, $SCRATCH, 0
  ld32 $VALUES_1, $IN_j_PTR, $SCRATCH, 1
  {
    add        $SCRATCH, $SCRATCH, $NUM_ELEM  // need to keep track of j delta
    f32v2\INSTRUCTION $ACC_0:3, $ACC_0:3, $VALUES_0:1
  }
  cmpult     $SCRATCH2, $SCRATCH, $IN_j_SIZE
  brnz       $SCRATCH2, _in_j_loop_start.\@
  brnzdec    $NUM_PART, _start_num_partials_loop.\@

// ************************************************* //
// end of 2 vector accumulating, scale and store
// ************************************************* //

  {
    add $IN_j_DELTA, $IN_j_DELTA, 8
    f32v2mul   $VALUES_0:1, $SCALE:B, $ACC_0:3
  }
  st64step   $VALUES_0:1, $mzero, $OUT_j_PTR+=, 1

  brnzdec    $OUT_j_SIZE, _out_j_loop.\@

_out_j_size_remainder.\@:
  ld32       $OUT_j_SIZE, $mworker_base, $mzero, 0

// ************************************************* //
// 1 vector remainder accumulate, scale and store
// ************************************************* //
_out_j_1_remainder.\@:
  and        $SCRATCH, $OUT_j_SIZE, 1
  brz        $SCRATCH, _out_j_size_end.\@

  {
    ld32       $IN_i_PTR, $mworker_base, $mzero, 1
    setzi      $ACC_0, \INIT_FLOAT & LDCONST_MASK
  }
  {
    ld32       $NUM_PART, $mzero, $mworker_base, 5
    or         $ACC_0, $ACC_0, \INIT_FLOAT & ~LDCONST_MASK
  }

_start_num_partials_loop_1_rem.\@:
  call       $SCRATCH2, _Reduce_ptr_fetch
  mul        $IN_j_SIZE, $IN_j_SIZE, SIZE_OF_IN_TYPE

_in_j_loop_start_1_rem.\@:
  ld32      $VALUES_0, $IN_j_PTR, $SCRATCH, 0
  {
    add        $SCRATCH, $SCRATCH, $NUM_ELEM  // need to keep track of j delta
    f32\INSTRUCTION   $ACC_0, $ACC_0, $VALUES_0
  }
  cmpult     $SCRATCH2, $SCRATCH, $IN_j_SIZE
  brnz       $SCRATCH2, _in_j_loop_start_1_rem.\@
  brnzdec    $NUM_PART, _start_num_partials_loop_1_rem.\@

// ************************************************* //
// end of 1 vector accumulating, scale and store
// ************************************************* //

  {
    add $IN_j_DELTA, $IN_j_DELTA, 2
    f32mul   $VALUES_0, $SCALE, $ACC_0
  }
  st32step   $VALUES_0, $mzero, $OUT_j_PTR+=, 1

_out_j_size_end.\@:
  // add num_partials to IN_i_ptr and store
  st32       $IN_i_PTR, $mworker_base, $mzero, 1
  brnzdec    $OUT_i_SIZE, _loop_over_reductions.\@
// ************************************************* //
// End of loops
// ************************************************* //
_exit.\@:
  exitz      $mzero

.size REDUCE_FLOAT_FLOAT(Reduce,common),\
              .-REDUCE_FLOAT_FLOAT(Reduce,common)
.endm

//*****************************************************************************
// Create vertices with the macros above

INSTANTIATE_REDUCE_HALF_HALF HALF_1_0_BROADCAST mul ReduceMul
INSTANTIATE_REDUCE_HALF_HALF MIN_HALF_BROADCAST max ReduceMax
INSTANTIATE_REDUCE_HALF_HALF MAX_HALF_BROADCAST min ReduceMin

INSTANTIATE_REDUCE_FLOAT_FLOAT FLOAT_1_0 mul ReduceMul
INSTANTIATE_REDUCE_FLOAT_FLOAT MIN_FLOAT max ReduceMax
INSTANTIATE_REDUCE_FLOAT_FLOAT MAX_FLOAT min ReduceMin

#endif
