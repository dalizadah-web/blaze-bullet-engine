# NNUE validation environments

`tools/run_sanitizers.sh` is the Linux/WSL sanitizer entrypoint. It deliberately
keeps ASan, UBSan, and ThreadSanitizer flags outside `Makefile.blaze`, which is
the production Windows build.

The local Windows host used for the initial Big-NNUE profiling run could not
start its configured WSL distribution: `wsl.exe` reported that its configured
`ext4.vhdx` path could not be found. This is a local environment limitation,
not sanitizer validation. The GitHub Actions `NNUE sanitizer validation`
workflow is the authoritative Linux sanitizer path.

## GitHub Actions result

The final Linux sanitizer validation for `2866ccba7b0ce6bd60615f74823d95d2cddc3385`
is green: [run 30222336461](https://github.com/dalizadah-web/blaze-bullet-engine/actions/runs/30222336461).

| Job | Status | Duration |
| --- | --- | ---: |
| ASan + UBSan full NNUE suite | passed | 7m58s |
| ThreadSanitizer multithread NNUE suite | passed | 28m14s |

The workflow downloaded and hash-verified the pinned Big network, then ran the
full test suite, legacy/fresh/incremental and special-move NNUE oracles,
one-million-ply scalar/AVX2 differential oracle, repeated multithread NNUE
tests, scalar fallback, and runtime-dispatched AVX2 path. No sanitizer,
alignment, lifetime, undefined-behavior, or race finding was reported.
