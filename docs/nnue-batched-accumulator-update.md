# Batched NNUE Accumulator Update

Profile-only histogram collection is enabled by `make -f Makefile.blaze profile`
and exposed through `bench_nnue_stats`. It is excluded from normal release
builds.

The one-thread, eight-position depth-8 pre-change profile collected 1,003,272
incremental moves. HalfKAv2 used one removal and one addition per perspective
for ordinary moves, two removals and one addition for captures, and two
removals plus two additions for castling. FullThreats commonly used two, three,
or four removals and additions per perspective. The profile recorded 19,349,290
separate 1024-wide accumulator passes, reading and writing 39,627,345,920 bytes
each way.

The retained implementation has scalar and separately compiled AVX2 fused
kernels for the measured `1+1`, `2+1`, and `2+2` HalfKAv2 distributions, plus
the measured `2+2` FullThreats distribution and fixed-capacity generic
fallbacks for larger FullThreats deltas. Each fused kernel loads an accumulator
block once, applies its removals then additions with the existing narrowing
semantics, and stores once. Runtime SIMD selection remains in `KernelSet`,
outside the inner 1024-wide loops.

Post-change profiling recorded 3,874,745 accumulator passes and 7,935,477,760
bytes read and written. On an AMD Ryzen 7 7700 with MinGW g++ 15.2.0, the
five-run, eight-position, one-thread depth-8 matrix changed from median
157,606 NPS (IQR 15,652) at `54980c7` to 165,203 NPS (IQR 13,332), a 4.82%
improvement. All positions returned the same best move and node count.
