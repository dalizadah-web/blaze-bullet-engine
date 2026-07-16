# Blaze Clean-Room Boundary

The Blaze production tree is `src/blaze`. Its code is independently authored and may depend only on the C++ standard library and dependencies explicitly recorded in `provenance/core-dependencies.json`.

Stockfish is an external measurement opponent and conceptual reference only. Its source, headers, object files, implementation constants, and distributed evaluation networks are not Blaze build inputs and must not be copied into Blaze modules. Public chess algorithms may be independently implemented from published descriptions.

`mingw32-make -f Makefile.blaze verify-clean-room` enforces the current boundary. It inspects the effective build commands, rejects compiled paths outside the Blaze/test-driver allowlist, scans source includes and evaluation-network names, validates every declared external file by SHA-256, and scans produced binaries for forbidden identifiers.

The empty dependency and asset arrays are deliberate for the current source-only release build. Blaze contains an independently specified quantized network evaluator and a Polyglot reader, but no network or book is shipped, enabled, or trusted by default. `tools/train_network.py` creates optional networks from generated legal positions and a transparent teacher; generated output remains a local experiment unless separately recorded. Any future library, network, book, or tablebase asset must record its origin, license, relative path, and SHA-256 before it may enter a release build. Rating-run manifests additionally bind the source revision, compiler and flags, executable, datasets, and hardware used for the run.
