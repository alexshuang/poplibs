#!/bin/sh

# arguments: m, n, k, do_dense_sparse, sparsity_factor, block_size, amp, iters
sparse_matmul () {
  option_args=""
  if (( $# >= 7 )); then
    option_args+=" --available-memory-proportion $7"
  fi
  if (( $# >= 8 )); then
    option_args+=" --iters $8"
  fi

  ./build/tools/static_sparse_matmul --m $1 --n $2 --k $3 --do-dense-sparse $4 \
      --sparsity-factor $5 --block-length $6 $option_args
}

# arguments: sparsity_factor, block_size, amp, iters
gpt2_sparse_matmul_perf () {
  option_args=""
  if (( $# >= 3 )); then
    option_args+=" $3"
  fi
  if (( $# >= 4 )); then
    option_args+=" $4"
  fi

  sparse_matmul 1600 1 512 true $1 $2 $option_args
  sparse_matmul 4800 1 1600 true $1 $2 $option_args
  sparse_matmul 1600 1 1600 true $1 $2 $option_args
  sparse_matmul 6400 1 1600 true $1 $2 $option_args
  sparse_matmul 1600 1 6400 true $1 $2 $option_args
  sparse_matmul 512 1 1600 true $1 $2 $option_args
  sparse_matmul 72832 1 512 false $1 $2 $option_args
}

# arguments: m, n, k, amp, iters
general_matmul () {
  option_args=""
  if (( $# >= 4 )); then
    option_args+=" --available-memory-proportion $4"
  fi
  if (( $# >= 5 )); then
    option_args+=" --numExecutions $5"
  fi

  ./build/tools/general_matrix_multiply --m $1 --n $2 --k $3 $option_args
}

# arguments: amp, iters
gpt2_general_matmul_perf () {
  option_args=""
  if (( $# >= 1 )); then
    option_args+=" $1"
  fi
  if (( $# >= 2 )); then
    option_args+=" $2"
  fi

  general_matmul 1 1600 512 $option_args
  general_matmul 1 4800 1600 $option_args
  general_matmul 1 1600 1600 $option_args
  general_matmul 1 6400 1600 $option_args
  general_matmul 1 1600 6400 $option_args
  general_matmul 1 512 1600 $option_args
  general_matmul 72832 1 512 $option_args
}

gpt2_sparse_matmul_benchmark () {
  sparsity_factor=(0.7 0.5 0.3)
  block_size=(1 4 8 16)
  # amp=(0.9 0.6 0.3)

  # for amp_val in ${amp[*]}
  # do
    gpt2_general_matmul_perf

    for factor in ${sparsity_factor[*]}
    do
      for bs in ${block_size[*]}
      do
        gpt2_sparse_matmul_perf $factor $bs
      done
    done
  # done
}

# main
gpt2_sparse_matmul_benchmark
