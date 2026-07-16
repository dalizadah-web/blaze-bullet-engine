# Clean-Room Blaze Foundation and Position Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a tested, independently authored chess position and legal move-generation core that no longer compiles or links Stockfish code.

**Architecture:** Add a parallel `src/blaze` implementation with one authoritative bitboard `Position`, generated attack tables, incremental make/unmake state, and a standalone test executable. Keep the legacy root engine untouched until the new core passes canonical and randomized correctness gates.

**Tech Stack:** C++20, MinGW g++ 15.2, GNU Make (`mingw32-make`), Python 3.13 with python-chess for differential verification.

## Global Constraints

- Do not copy or compile Stockfish source, headers, objects, constants, or NNUE weights.
- Implement production behavior only after a focused test has failed for the expected reason.
- Keep `Position` as the single authoritative chess state; do not mirror backend and public boards.
- Use deterministic generated attack and Zobrist tables.
- Do not modify or delete legacy user artifacts until the new baseline is proven.
- Build outputs belong under `build/blaze` and must remain untracked.

---

### Task 1: Bootstrap the Independent Test Target

**Files:**
- Create: `.gitignore`
- Create: `Makefile.blaze`
- Create: `tests/test_main.cpp`
- Create: `tests/test_support.h`

**Interfaces:**
- Consumes: MinGW `g++` and `mingw32-make`.
- Produces: `build/blaze/blaze_tests.exe` and the `TEST_CASE`, `CHECK`, and `CHECK_EQ` test APIs.

- [ ] **Step 1: Add a deliberately failing harness test**

```cpp
#include "test_support.h"

TEST_CASE(harness_reports_failures) {
    CHECK(false);
}

int main() { return blaze::test::run_all(); }
```

- [ ] **Step 2: Add the minimal header-only harness and bootstrap Makefile**

`test_support.h` registers named `void()` tests, records assertion failures with file and line, catches exceptions, prints a pass/fail summary, and returns non-zero when any check fails. `Makefile.blaze` compiles only `tests/test_main.cpp` initially with `-std=c++20 -Wall -Wextra -Wpedantic -Werror`.

- [ ] **Step 3: Run the harness and verify RED**

Run: `mingw32-make -f Makefile.blaze clean test`

Expected: the executable runs and exits non-zero with `harness_reports_failures` reported as failed.

- [ ] **Step 4: Change the assertion to `CHECK(true)` and verify GREEN**

Run: `mingw32-make -f Makefile.blaze clean test`

Expected: `1 passed, 0 failed` and exit code 0.

- [ ] **Step 5: Commit the bootstrap**

```powershell
git add -- .gitignore Makefile.blaze tests/test_main.cpp tests/test_support.h
git commit -m "build: bootstrap clean-room Blaze tests"
```

### Task 2: Define Independent Chess Value Types and Move Encoding

**Files:**
- Create: `src/blaze/core/types.h`
- Create: `src/blaze/core/move.h`
- Create: `src/blaze/core/move.cpp`
- Create: `tests/core/test_move.cpp`
- Modify: `Makefile.blaze`

**Interfaces:**
- Produces: `Color`, `PieceType`, `Piece`, `Square`, `MoveFlag`, `Move`, `square_from_string(std::string_view)`, `square_to_string(Square)`, `move_from_uci(std::string_view)`, and `move_to_uci(Move)` in namespace `blaze`.

- [ ] **Step 1: Write failing move-encoding tests**

Tests assert `a1 == 0`, `h8 == 63`, normal `e2e4` round-trips, all four promotion suffixes round-trip, and null/invalid text is rejected with `std::nullopt`.

- [ ] **Step 2: Run RED**

Run: `mingw32-make -f Makefile.blaze test`

Expected: compilation fails because `blaze/core/move.h` does not exist.

- [ ] **Step 3: Implement compact value types and move conversion**

`Move` stores from, to, promotion type, and flags in a 32-bit value. Square numbering is little-endian rank-file (`a1=0`, `h8=63`). Parsing validates exactly four characters for normal moves and five for promotions.

- [ ] **Step 4: Run GREEN and warning-clean build**

Run: `mingw32-make -f Makefile.blaze clean test`

Expected: all tests pass with no compiler warnings.

- [ ] **Step 5: Commit value types**

```powershell
git add -- src/blaze/core/types.h src/blaze/core/move.h src/blaze/core/move.cpp tests/core/test_move.cpp Makefile.blaze
git commit -m "feat: add independent chess value types"
```

### Task 3: Generate Attack Tables from Chess Geometry

**Files:**
- Create: `src/blaze/core/attacks.h`
- Create: `src/blaze/core/attacks.cpp`
- Create: `tests/core/test_attacks.cpp`
- Modify: `Makefile.blaze`

**Interfaces:**
- Consumes: `Square`, `Color`, and `Bitboard` from `types.h`.
- Produces: `Attacks::initialize()`, `pawn(Color, Square)`, `knight(Square)`, `king(Square)`, `bishop(Square, Bitboard)`, `rook(Square, Bitboard)`, and `queen(Square, Bitboard)`.

- [ ] **Step 1: Write failing geometry tests**

Tests compare exact masks for a1/d4/h8 pawn, knight, and king attacks; slider tests place blockers on both sides of each ray and assert that the first blocker is included while squares beyond it are excluded.

- [ ] **Step 2: Run RED**

Run: `mingw32-make -f Makefile.blaze test`

Expected: compilation fails because `blaze/core/attacks.h` does not exist.

- [ ] **Step 3: Implement generated leaper tables and portable ray attacks**

Initialization walks board coordinates, never uses hard-coded external attack arrays, and fills 64-entry pawn/knight/king tables. Bishop and rook queries walk four rays each until the board edge or first occupied square.

- [ ] **Step 4: Run GREEN**

Run: `mingw32-make -f Makefile.blaze clean test`

Expected: all geometry and prior tests pass.

- [ ] **Step 5: Commit attacks**

```powershell
git add -- src/blaze/core/attacks.h src/blaze/core/attacks.cpp tests/core/test_attacks.cpp Makefile.blaze
git commit -m "feat: generate independent attack tables"
```

### Task 4: Parse and Serialize Authoritative Position State

**Files:**
- Create: `src/blaze/core/position.h`
- Create: `src/blaze/core/position.cpp`
- Create: `tests/core/test_position_fen.cpp`
- Modify: `Makefile.blaze`

**Interfaces:**
- Produces: `Position::from_fen(std::string_view) -> std::optional<Position>`, `Position::to_fen()`, `piece_on(Square)`, `side_to_move()`, `castling_rights()`, `ep_square()`, `rule50()`, `fullmove_number()`, `occupied()`, `pieces(Color, PieceType)`, and `is_consistent()`.

- [ ] **Step 1: Write failing FEN tests**

Tests cover start position round-trip, empty-board rejection, missing king rejection, invalid side/castling/EP fields, adjacent kings rejection, exact piece counts, and preservation of clocks.

- [ ] **Step 2: Run RED**

Run: `mingw32-make -f Makefile.blaze test`

Expected: compilation fails because `blaze/core/position.h` does not exist.

- [ ] **Step 3: Implement the single-state Position**

Store `std::array<Bitboard, 12>`, `std::array<Piece, 64>`, side, castling mask, EP square, rule-50 count, and fullmove number. Parsing builds all representations once and validates agreement through `is_consistent()`.

- [ ] **Step 4: Run GREEN**

Run: `mingw32-make -f Makefile.blaze clean test`

Expected: all FEN and earlier tests pass.

- [ ] **Step 5: Commit position state**

```powershell
git add -- src/blaze/core/position.h src/blaze/core/position.cpp tests/core/test_position_fen.cpp Makefile.blaze
git commit -m "feat: add authoritative Blaze position state"
```

### Task 5: Add Deterministic Zobrist Keys and Reversible State

**Files:**
- Create: `src/blaze/core/zobrist.h`
- Create: `src/blaze/core/zobrist.cpp`
- Create: `tests/core/test_position_state.cpp`
- Modify: `src/blaze/core/position.h`
- Modify: `src/blaze/core/position.cpp`
- Modify: `Makefile.blaze`

**Interfaces:**
- Produces: `Position::key()`, `StateInfo`, `Position::make_move(Move, StateInfo&)`, `Position::unmake_move(Move, const StateInfo&)`, `make_null(StateInfo&)`, and `unmake_null(const StateInfo&)`.

- [ ] **Step 1: Write failing state tests**

For normal moves, captures, double pawn pushes, en passant, each castling side, and every promotion type, tests snapshot FEN and key, make the move, unmake it, and require byte-for-byte FEN plus key restoration. A second test builds the same position through different legal move orders and requires equal keys.

- [ ] **Step 2: Run RED**

Run: `mingw32-make -f Makefile.blaze test`

Expected: compilation fails because state APIs are absent.

- [ ] **Step 3: Implement deterministic keys and reversible updates**

Generate keys with a locally implemented fixed-seed SplitMix64 sequence. `StateInfo` stores only fields required to reverse a move. Update keys incrementally for pieces, side, castling, and effective en-passant file; debug builds recompute and assert the incremental key after every transition.

- [ ] **Step 4: Run GREEN**

Run: `mingw32-make -f Makefile.blaze clean test`

Expected: all round-trip and transposition tests pass.

- [ ] **Step 5: Commit reversible state**

```powershell
git add -- src/blaze/core/zobrist.h src/blaze/core/zobrist.cpp src/blaze/core/position.h src/blaze/core/position.cpp tests/core/test_position_state.cpp Makefile.blaze
git commit -m "feat: add reversible position state and hashing"
```

### Task 6: Generate Complete Legal Moves

**Files:**
- Create: `src/blaze/core/movegen.h`
- Create: `src/blaze/core/movegen.cpp`
- Create: `tests/core/test_movegen.cpp`
- Modify: `Makefile.blaze`

**Interfaces:**
- Produces: `generate_pseudo_legal(const Position&, MoveList&)`, `generate_legal(Position&, MoveList&)`, `is_square_attacked(const Position&, Square, Color)`, `in_check(const Position&)`, and `Position::is_legal(Move)`.

- [ ] **Step 1: Write failing targeted legality tests**

Tests cover start-position 20 moves, pinned pieces, double check permitting king moves only, illegal EP exposing a rook attack, castling through check, castling with missing rook, underpromotions, check evasions, mate, and stalemate.

- [ ] **Step 2: Run RED**

Run: `mingw32-make -f Makefile.blaze test`

Expected: compilation fails because move generation APIs are absent.

- [ ] **Step 3: Implement pseudo-legal generation and legal filtering**

Generate pawn, leaper, slider, promotion, EP, and castling moves from Blaze attack APIs. For each candidate, make it, reject positions where the moving side's king is attacked, then unmake it. This correctness-first filter is replaced by pin/check masks only after perft is green and a benchmark proves the need.

- [ ] **Step 4: Run GREEN**

Run: `mingw32-make -f Makefile.blaze clean test`

Expected: all targeted legality tests pass.

- [ ] **Step 5: Commit move generation**

```powershell
git add -- src/blaze/core/movegen.h src/blaze/core/movegen.cpp tests/core/test_movegen.cpp Makefile.blaze
git commit -m "feat: generate complete legal chess moves"
```

### Task 7: Establish Perft and Randomized Differential Gates

**Files:**
- Create: `src/blaze/core/perft.h`
- Create: `src/blaze/core/perft.cpp`
- Create: `tests/core/test_perft.cpp`
- Create: `tools/differential_perft.py`
- Create: `tools/perft_driver.cpp`
- Modify: `Makefile.blaze`

**Interfaces:**
- Produces: `perft(Position&, int) -> std::uint64_t` and `build/blaze/perft_driver.exe`, accepting `FEN<TAB>depth` on stdin and returning `nodes` per line.

- [ ] **Step 1: Write failing canonical perft tests**

Tests include start position depths 1-5 (`20, 400, 8902, 197281, 4865609`) and Kiwipete depths 1-4 (`48, 2039, 97862, 4085603`).

- [ ] **Step 2: Run RED**

Run: `mingw32-make -f Makefile.blaze test`

Expected: compilation fails because `perft` is absent.

- [ ] **Step 3: Implement recursive perft and driver**

`perft` returns one at depth zero, counts the move list directly at depth one, and otherwise makes/unmakes every legal move. The driver rejects malformed lines and emits deterministic decimal counts.

- [ ] **Step 4: Run GREEN and canonical depth-6 verification**

Run: `mingw32-make -f Makefile.blaze clean test perft-driver`

Run: `$line = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1`t6"; $line | build/blaze/perft_driver.exe`

Expected: `119060324`.

- [ ] **Step 5: Run randomized python-chess differential test**

`differential_perft.py` uses a fixed seed, generates legal random playout positions, computes python-chess perft at depth 3, batches them through the Blaze driver, and exits non-zero on the first mismatch. Initial gate: 1,000 positions; final design gate: 10,000.

Run: `python tools/differential_perft.py --positions 1000 --depth 3 --seed 20260716`

Expected: `1000/1000 positions matched`.

- [ ] **Step 6: Commit correctness gates**

```powershell
git add -- src/blaze/core/perft.h src/blaze/core/perft.cpp tests/core/test_perft.cpp tools/differential_perft.py tools/perft_driver.cpp Makefile.blaze
git commit -m "test: establish clean-room perft gates"
```

### Task 8: Prove the Core Build Is Stockfish-Free

**Files:**
- Create: `tools/verify_clean_room.ps1`
- Create: `provenance/core-dependencies.json`
- Create: `docs/clean-room-boundary.md`
- Modify: `Makefile.blaze`

**Interfaces:**
- Produces: `mingw32-make -f Makefile.blaze verify-clean-room`.

- [ ] **Step 1: Write the failing verification invocation**

Add a make target that invokes the nonexistent script, then run it.

Run: `mingw32-make -f Makefile.blaze verify-clean-room`

Expected: failure because `tools/verify_clean_room.ps1` does not exist.

- [ ] **Step 2: Implement boundary verification**

The script parses the make dependency list, rejects any compiled path under `vendor`, rejects source includes containing `Stockfish`, rejects forbidden network filenames, verifies the dependency manifest hashes, and scans the test/perft binaries for `Stockfish` strings.

- [ ] **Step 3: Run GREEN plus complete core verification**

Run: `mingw32-make -f Makefile.blaze clean test perft-driver verify-clean-room`

Expected: tests pass and the script prints `clean-room verification passed`.

- [ ] **Step 4: Commit the boundary gate**

```powershell
git add -- tools/verify_clean_room.ps1 provenance/core-dependencies.json docs/clean-room-boundary.md Makefile.blaze
git commit -m "build: enforce clean-room dependency boundary"
```

## Plan Self-Review

- Spec coverage in this phase: repository bootstrap, independent types, attacks, authoritative position, reversible hashing, legal move generation, canonical/randomized correctness, and clean-room provenance.
- Deferred to separate implementation plans after this phase is green: asynchronous UCI/time management, race-free clustered TT, coordinated SMP, search features/PV, Blaze evaluation training, book/tablebases, tournament reliability, and Elo gates.
- API consistency: all position-mutating tests use `Position`, `Move`, and `StateInfo`; move generation and perft share the same `MoveList` and make/unmake APIs.
- No task compiles or links a legacy root source or a vendored Stockfish path.
