#!/usr/bin/env bash
# Linux/WSL-only validation.  Keep sanitizer flags out of Makefile.blaze.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

cxx=${CXX:-g++}
build_dir=${BLAZE_SANITIZER_BUILD_DIR:-build/sanitizers}
mkdir -p "$build_dir"

mapfile -t tests < <(find tests -name '*.cpp' -print | sort)
mapfile -t engine < <(find src/blaze -name '*.cpp' ! -name 'nnue_kernels_avx2.cpp' -print | sort)
stockfish=(
  vendor/Stockfish-master/src/bitboard.cpp
  vendor/Stockfish-master/src/movegen.cpp
  vendor/Stockfish-master/src/position.cpp
  vendor/Stockfish-master/src/misc.cpp
  vendor/Stockfish-master/src/nnue/network.cpp
  vendor/Stockfish-master/src/nnue/nnue_accumulator.cpp
  vendor/Stockfish-master/src/nnue/features/half_ka_v2_hm.cpp
  vendor/Stockfish-master/src/nnue/features/full_threats.cpp
)
common=(-std=c++20 -O1 -g -DNNUE_EMBEDDING_OFF -Wall -Wextra -Wpedantic -Werror
        -Wno-empty-body -Isrc -Itests -Ivendor/Stockfish-master/src -pthread)

# AVX2 remains a distinct TU. The runtime dispatcher prevents execution on
# non-AVX2 hosts, while the rest of the binary remains baseline x86.
"$cxx" "${common[@]}" -mavx2 -c src/blaze/eval/nnue_kernels_avx2.cpp \
  -o "$build_dir/nnue_kernels_avx2.o"

run_suite() {
  local name=$1
  shift
  "$cxx" "${common[@]}" "$@" "${tests[@]}" "${engine[@]}" "${stockfish[@]}" \
    "$build_dir/nnue_kernels_avx2.o" -o "$build_dir/blaze_tests_$name"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$build_dir/blaze_tests_$name"
}

modes=${BLAZE_SANITIZER_MODES:-"asan_ubsan tsan"}
if [[ " $modes " == *" asan_ubsan "* ]]; then
  run_suite asan_ubsan -fsanitize=address,undefined -fno-omit-frame-pointer
fi
# TSan is deliberately separate: it is incompatible with ASan and reports
# races only when it owns the instrumentation runtime.
if [[ " $modes " == *" tsan "* ]]; then
  run_suite tsan -fsanitize=thread -fno-omit-frame-pointer
fi
