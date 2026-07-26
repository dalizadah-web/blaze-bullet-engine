# NNUE Thread Scaling Audit

`make -f Makefile.blaze nnue-thread-scaling` builds the evaluation-only audit.
It gives every worker an independent `Position` copy and `NnueThreadState`, and
reports operations per second and efficiency at 1, 2, 4, and 8 threads for
inference, fresh refresh, full direct evaluation, and incremental updates.

On the Ryzen 7 7700 with the pinned Big network and a 500 ms sample duration:

| Evaluator / mode | 1 thread ops/s | 8 thread ops/s | 8-thread efficiency |
| --- | ---: | ---: | ---: |
| Scalar inference | 763,062 | 3,720,204 | 61% |
| Scalar fresh refresh | 140,694 | 772,946 | 69% |
| Scalar full direct | 100,062 | 609,376 | 76% |
| Scalar incremental | 141,476 | 1,290,436 | 114% |
| AVX2 inference | 1,018,662 | 5,573,244 | 68% |
| AVX2 fresh refresh | 127,422 | 939,524 | 92% |
| AVX2 full direct | 130,014 | 749,712 | 72% |
| AVX2 incremental | 318,274 | 2,287,020 | 90% |

`NnueThreadState::Impl` owns its accumulator stack, transformed-input scratch,
and inference scratch through a distinct heap allocation per worker. The large
accumulator and inference scratch types are 64-byte aligned. The scaling data
does not indicate writable false sharing: the strong AVX2 incremental and
refresh results would be the first affected. The weaker scalar inference and
full-direct results are consistent with shared read-only network-weight cache
and memory-bandwidth pressure. No NNUE-local padding or layout change was
retained. Any remaining search 4-to-8 plateau is therefore outside NNUE and is
not addressed by this task.
