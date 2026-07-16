# Clean-Room Blaze Engine Design

**Date:** 2026-07-16

**Status:** Approved architecture; pending written-spec review

## Objective

Build an independently implemented Blaze chess engine that contains no Stockfish source code, headers, compiled objects, or Stockfish-distributed evaluation weights. The engine must first become correct, deterministic, reproducible, and safe under concurrency, then progress through statistically controlled strength experiments until it passes both the 2950-strength gate and the unrestricted-Stockfish superiority gate defined below.

This is a replacement architecture, not a promise that a specific search patch will manufacture a predetermined rating. Strength claims are valid only after the binding match gates accept them.

## Binding Success Criteria

### Clean-room gate

The release build and its transitive source dependency graph must contain no files from `vendor/Stockfish-master`, no Stockfish-derived source copied into Blaze modules, and no Stockfish-distributed NNUE network. Stockfish may be run only as an external opponent or consulted as a conceptual reference. Algorithms described in public chess-programming literature may be independently implemented without copying source expressions or constants.

The repository will contain a machine-readable provenance manifest for every compiled dependency and shipped data asset. A source scan, build dependency report, and binary-symbol/string scan must all pass before a release is eligible for rating.

### Correctness gate

The new position core must pass:

- canonical start-position perft through depth 6;
- the standard multi-position perft suite through each position's practical verification depth;
- randomized differential perft against python-chess on at least 10,000 legal positions;
- make/unmake round-trip checks for board state, Zobrist key, castling rights, en-passant state, rule-50 counter, and repetition history;
- targeted castling, promotion, en-passant, check-evasion, stalemate, mate, and repetition tests;
- deterministic bench signatures for one-thread builds.

No strength testing begins while any correctness gate is red.

### Reliability gate

The canonical executable must:

- complete 1,000 repeated UCI initialize/ready/quit cycles without crash or hang;
- complete 1,000 tournament games without illegal moves, protocol failures, or time forfeits attributable to Blaze;
- pass address/undefined-behavior sanitizers on supported builds;
- pass a thread-safety stress suite for the transposition table and search coordinator;
- fail initialization with a non-zero exit status when a required evaluation network is absent, corrupt, or incompatible;
- produce a manifest containing source revision, compiler, flags, binary SHA-256, network SHA-256, opening-set SHA-256, and hardware identity for every rating run.

### 2950-comfort gate

Blaze must score at least 55% against Stockfish 17.1 configured with `UCI_LimitStrength=true` and `UCI_Elo=2950`, using 120+1, one search thread per engine, 256 MB hash, no pondering, no tablebases, and paired UHO openings. The 95% lower confidence bound on Blaze's score must exceed 52%. Games continue until the confidence requirement is met or the candidate is rejected.

### Better-than-Stockfish gate

Blaze must defeat the current official stable Stockfish available when the final test begins, using identical hardware allocation, one search thread, 256 MB hash, no pondering, no tablebases, and paired UHO openings. It must pass at both 10+0.1 and 60+0.6. Each time control uses an SPRT with Elo0=0, Elo1=+10, alpha=0.05, and beta=0.05; success requires acceptance of the +10 Elo hypothesis. A result against an obsolete release, a limited-strength setting, unequal resources, or a narrow opening subset does not satisfy this gate.

## Chosen Architecture

The new implementation is built in parallel under `src/blaze` and `tests`, while the current root-level engine remains a frozen comparison artifact. The new build never links the current Stockfish-backed `thc.cpp` or any vendored Stockfish source. When the new core passes correctness and protocol parity, `Blaze.exe` becomes the canonical executable and the old integration scripts and recommended binaries are retired.

The inactive MIT-licensed THC library may be used as a test oracle during early development, but it is not part of the production engine because its general-purpose board representation is not an appropriate long-term performance foundation.

### Module boundaries

`src/blaze/core` owns chess state and rules. It exposes value types for color, piece, square, move, castling rights, and search state. `Position` is the single authoritative state representation; there is no mirrored public board and backend board.

`src/blaze/search` owns iterative deepening, alpha-beta/PVS, quiescence, move ordering, pruning, the transposition table, search histories, root results, and parallel coordination. Search consumes only the public core and evaluation interfaces.

`src/blaze/eval` owns classical bootstrap evaluation, the Blaze network loader, incremental accumulators, and inference. Search cannot continue if the configured production network fails validation.

`src/blaze/uci` owns command parsing, options, asynchronous search lifecycle, time management, protocol output, opening-book policy, and tablebase adapters. The input loop never blocks behind a running search.

`tools` owns test orchestration, match manifests, SPRT calculations, network training/export, and tuning. Rating tools never silently fall back to old executables.

## Independent Position Core

The board uses color and piece bitboards plus a square-indexed piece array for constant-time piece lookup. Leaper attacks are generated from chess geometry at initialization. Slider masks and occupancy-indexed attack tables are generated by Blaze code from rank/file/diagonal rays. BMI2/PEXT may be used as an optional query accelerator, with a portable software index fallback generated from the same masks.

`Position::make_move` updates piece placement, side to move, castling rights, en-passant square, rule-50 counter, ply count, Zobrist key, pawn key, material state, and repetition history incrementally. `Position::unmake_move` restores a compact `StateInfo` record. Null moves have their own make/unmake path and never alter the public game-history repetition chain as if they were legal moves.

Move generation produces pseudo-legal moves by type and filters or constructs legal moves using king-attack and pin information. Special-move legality is tested explicitly. The implementation favors correctness and transparent invariants first; attack generation and legality hot paths are optimized only after deterministic benchmarks identify the cost.

## Race-Free Clustered Transposition Table

The table contains power-of-two clusters with four replacement candidates per cluster. Entries carry a partial key, move, score, static evaluation, depth, bound type, principal-variation flag, and generation.

Each entry uses atomic storage and a sequence guard. Writers acquire an odd sequence value, update atomic key/payload words, then publish the next even sequence with release ordering. Readers acquire the sequence, read the atomic words, and accept the snapshot only when the same even sequence is observed afterward. This prevents C++ data races and torn logical snapshots without a global lock.

Replacement favors matching keys, empty entries, older generations, shallower depth, and non-PV entries. Mate scores are normalized by ply on store and restored on probe. `new_search()` advances the generation, and `hashfull()` samples entries from the current generation.

## Search and Parallelism

The first correct milestone runs one thread. Multi-thread mode is enabled only after the TT stress suite passes.

Iterative deepening owns a `RootMove` array containing move, score, average score, completed depth, node effort, and full principal variation. Every completed iteration publishes a coherent root result. UCI output reports the actual PV rather than only the first move.

Parallel search uses a persistent worker pool with thread-local history, accumulator, stack, node counters, and search state. At each depth, the coordinator assigns root work, collects only completed results, and chooses the final move from the deepest completed iteration. Helper results therefore affect the selected move. The coordinator stops workers through atomics and condition variables, not detached threads or stdin polling.

The initial search feature set is aspiration PVS, check extensions, null-move pruning with verification, reverse futility, razoring, futility/LMP, LMR, staged move ordering, quiescence SEE pruning, and history updates. ProbCut, singular extensions, correction histories, and other selective mechanisms are added individually only after a focused test and an accepted SPRT result. Constants are Blaze-owned and tuned from Blaze match data rather than copied from Stockfish.

## UCI and Time Management

The UCI controller has a dedicated command loop and a separately owned search task. It supports `go` clocks, increment, moves-to-go, movetime, depth, nodes, mate, infinite, ponder, and searchmoves; `stop`, `quit`, and `ponderhit` are handled promptly while search is active.

`Move Overhead` is a UCI option. The time manager tracks remaining time, increment, moves-to-go estimate, completed-iteration cost, score volatility, root-move stability, and measured stop latency. It reserves overhead before allocation, never spends into the reserve, and checks time by both elapsed duration and a small adaptive node interval. Hard deadlines are enforced independently of soft iteration deadlines.

## Evaluation Ownership

The first clean-room executable uses a deterministic classical evaluation so core/search development is not blocked on training. It includes independently authored material, piece-square, mobility, pawn-structure, king-safety, threat, and endgame terms.

The production evaluator uses a Blaze-defined incrementally updatable neural network and file format. Training data is generated from licensed game data and Blaze self-play; labels, sampling policy, trainer version, and dataset hashes are recorded. The exporter quantizes weights and writes a versioned header with architecture, dimensions, checksum, and feature-set identifier. The loader validates every field and terminates startup on failure. The old `nn-04cf2b4ed1da.nnue` file is not a production dependency.

## Book and Tablebases

Opening-book use is disabled by default and controlled by `OwnBook`, `BookFile`, and selection-policy options. The Polyglot reader validates file structure, translates Polyglot castling encodings, filters against legal moves, and supports deterministic-best and seeded weighted selection. Unknown-provenance books are excluded from release packages and rating gates.

Tablebases are an optional adapter around a provenance-audited upstream Fathom distribution that contains no Stockfish source. Probes receive the actual rule-50 counter, castling state, en-passant square, and side to move. Results remain in side-to-move score orientation. Root and in-search probing have separate options and are disabled in rating gates unless both opponents receive identical tablebase access.

## Repository and Reproducibility

The repository gains a focused `.gitignore`, one canonical CMake build, one canonical executable name, test targets, and versioned documentation. Generated binaries, object files, DLL bundles, match PGNs, tablebases, books, and large networks are not committed. Historical root artifacts are cataloged and then moved outside the production source tree or removed after the replacement baseline is preserved.

Every strength change is isolated in its own commit and compared to its immediate parent. Fast smoke matches may reject obviously bad candidates, but acceptance requires paired openings and SPRT. Parameter tuning uses independent training and validation opening sets and records every tested configuration.

## Error Handling

Invalid FEN, illegal position commands, malformed UCI values, corrupt books, unavailable tablebases, incompatible networks, and impossible search limits produce explicit `info string` diagnostics. Required-resource failures terminate initialization; optional-resource failures disable only that feature. Search shutdown is idempotent, joins all workers, and cannot leave a worker accessing released TT, network, or position memory.

## Migration Sequence

1. Establish Git hygiene, canonical build/test targets, and frozen baseline manifests.
2. Implement and validate the independent position core with tests written before production code.
3. Implement asynchronous UCI and deterministic one-thread search on the new core.
4. Implement the race-free clustered TT and enable multi-threaded root coordination after stress tests pass.
5. Add staged move ordering, complete PV handling, selective search features, and benchmark gates.
6. Replace the legacy NNUE dependency with the Blaze evaluator and independently trained network.
7. Reintroduce opt-in book and provenance-audited tablebases.
8. Retire broken integrated binaries, stale scripts, and Stockfish-linked production sources.
9. Run reliability, 2950, and unrestricted-Stockfish gates; continue controlled strength experiments until both rating gates accept.

## Key Risks and Controls

An independent engine exceeding current Stockfish is a research outcome, not a deterministic software-delivery estimate. The project controls this risk by making every claim falsifiable, keeping the unrestricted gate separate from the 2950 limited-strength gate, and refusing to relabel a handicap or narrow-domain result as general superiority.

The clean-room rewrite temporarily lowers playing strength. The parallel architecture controls this by preserving a frozen baseline for comparison while correctness and search capability are rebuilt in measurable stages.

Neural training can dominate compute and calendar cost. The classical bootstrap evaluator keeps engine work moving, while versioned datasets and reproducible exporters prevent untracked network changes from contaminating match conclusions.
