# NNUE validation environments

`tools/run_sanitizers.sh` is the Linux/WSL sanitizer entrypoint. It deliberately
keeps ASan, UBSan, and ThreadSanitizer flags outside `Makefile.blaze`, which is
the production Windows build.

The local Windows host used for the initial Big-NNUE profiling run could not
start its configured WSL distribution: `wsl.exe` reported that its configured
`ext4.vhdx` path could not be found. This is a local environment limitation,
not sanitizer validation. The GitHub Actions `NNUE sanitizer validation`
workflow is the authoritative Linux sanitizer path.
