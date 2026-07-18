# Search Correctness Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Blaze's search semantics trustworthy before replacing hot paths or adding aggressive pruning.

**Architecture:** Introduce compile-time root/PV/non-PV node semantics, verified non-PV null-move pruning, bounded selective extensions, safe maximum-ply and mate bounds, and rule-50-qualified TT cutoffs. Add deterministic fixed-node evidence so each later strength candidate can be compared with the immediately preceding accepted binary.

**Tech Stack:** C++20 engine and custom test harness, Python 3.13, python-chess, UCI, JSON/EPD, GNU Make, existing Blaze experiment manifests.

## Global Constraints

- Work only on `codex/bullet-beast` in the existing isolated worktree.
- Preserve accepted commit `297926e2700134eb0a6d7c16663b4a23ff4b1b34`.
- Do not merge or push unless the user later requests it.
- Stockfish 18 remains an external UCI opponent/oracle; do not inspect or copy its source, constants, implementation details, or network weights.
- Make one coherent mechanism per candidate commit.
- Run a 1,000-game color-paired local/cloud non-regression gate for each strength candidate before promoting the accepted baseline.
- Treat 1,000 games as a non-regression gate, not precise evidence for small Elo gains.
- Do not add unrelated evaluation, hot-path, SMP, or time-management changes in this plan.

## File map

- `src/blaze/search/search.h`: node types, debug-only counters, search-stack limits, and typed search declarations.
- `src/blaze/search/search.cpp`: node dispatch, null verification, mate bounds, depth-one root behavior, extensions, and maximum-ply handling.
- `src/blaze/search/stack.h`: cumulative extension state.
- `src/blaze/search/transposition_table.h/.cpp`: rule-50-qualified TT entries and probes.
- `tests/search/test_search.cpp`: tactical and semantic search regressions.
- `tests/search/test_transposition_table.cpp`: rule-50 and mate normalization tests.
- `tests/search/test_see.cpp`: promotion, en-passant, pin, and king-safety reference cases.
- `tools/search_signature.py`: persistent-UCI fixed-node signature generator.
- `tools/test_search_signature.py`: parser, identity, determinism, and malformed-output tests.
- `testdata/search/correctness-v1.epd`: versioned, independently selected deterministic corpus.
- `provenance/search-corpora.json`: corpus hash, purpose, generator, and clean-room declaration.
- `Makefile.blaze`: search-signature test target.

---

### Task 1: Add typed node semantics and mate-distance bounds

**Files:**
- Modify: `src/blaze/search/search.h`
- Modify: `src/blaze/search/search.cpp`
- Modify: `tests/search/test_search.cpp`

**Interfaces:**
- Produces: private `enum class NodeType { Root, PV, NonPV };`.
- Produces: `template<NodeType node_type> int negamax(...)`.
- Produces: mate-distance clamping at every full-search node.

- [ ] **Step 1: Add failing mate-distance regressions**

Append tests that require the shortest available mate and stable mate scores:

```cpp
TEST_CASE(search_prefers_the_shortest_forced_mate) {
    Position root = position("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 6});
    CHECK(result.score >= search_mate_threshold);
    CHECK_EQ(result.score, search_mate_score - 1);
}

TEST_CASE(mate_score_is_stable_across_deeper_completed_iterations) {
    Position root = position("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    TranspositionTable shallow_table(4);
    TranspositionTable deep_table(4);
    Searcher shallow(shallow_table);
    Searcher deep(deep_table);
    const SearchResult at_three = shallow.search(root, SearchLimits{.depth = 3});
    const SearchResult at_six = deep.search(root, SearchLimits{.depth = 6});
    CHECK_EQ(at_six.score, at_three.score);
    CHECK_EQ(at_six.best_move, at_three.best_move);
}
```

- [ ] **Step 2: Run the focused test and retain the failure**

Run:

```powershell
mingw32-make -f Makefile.blaze test
```

Expected: at least one new mate-distance assertion fails against the unbounded search.

- [ ] **Step 3: Add typed node dispatch**

In `Searcher`, replace the untyped declaration with:

```cpp
enum class NodeType : std::uint8_t { Root, PV, NonPV };

template<NodeType node_type>
[[nodiscard]] int negamax(
    Position& position,
    int depth,
    int alpha,
    int beta,
    int ply,
    Context& context,
    PvLine& pv,
    bool allow_null = true);
```

Call `negamax<NodeType::Root>` from normal root search,
`negamax<NodeType::PV>` for the first child of Root/PV nodes, and
`negamax<NodeType::NonPV>` for zero-window, null-move, ProbCut, and reduced
searches. A full-window re-search from a Root/PV node uses `NodeType::PV`.

- [ ] **Step 4: Clamp mate bounds before TT lookup**

After draw/check handling and before probing the TT, add:

```cpp
alpha = std::max(alpha, -search_mate_score + ply);
beta = std::min(beta, search_mate_score - ply - 1);
if (alpha >= beta) {
    return alpha;
}
```

Use `if constexpr (node_type == NodeType::Root)` for root-move restriction and
`constexpr bool pv_node = node_type != NodeType::NonPV` for pruning guards.

- [ ] **Step 5: Run focused and full tests**

Run:

```powershell
mingw32-make -f Makefile.blaze clean
mingw32-make -f Makefile.blaze test
```

Expected: all C++ tests pass, including both mate regressions.

- [ ] **Step 6: Commit the node-semantics candidate**

```powershell
git add src/blaze/search/search.h src/blaze/search/search.cpp tests/search/test_search.cpp
git commit -m "search: add typed nodes and mate-distance bounds"
```

### Task 2: Restrict null move to verified non-PV nodes

**Files:**
- Modify: `src/blaze/search/search.h`
- Modify: `src/blaze/search/search.cpp`
- Modify: `tests/search/test_search.cpp`

**Interfaces:**
- Consumes: `Searcher::NodeType` and typed `negamax` from Task 1.
- Produces: debug-only `SearchLimits::enable_null_move`.
- Produces: debug-only `SearchResult::{null_move_searches,null_move_pv_searches,null_move_verifications}`.
- Produces: dynamic reduction and high-depth verification with a recursion guard.

- [ ] **Step 1: Add debug controls and failing safety tests**

Under `#ifndef NDEBUG`, add the two counters to `Context` and `SearchResult`, and
add `bool enable_null_move = true` to `SearchLimits`. Append:

```cpp
TEST_CASE(null_move_is_used_only_at_non_pv_nodes) {
    Position root = position("r1bqk2r/pppp1ppp/2n2n2/4p3/2B1P3/2N2N2/PPPP1PPP/R1BQK2R w KQkq - 4 5");
    TranspositionTable table(4);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 8});
    CHECK(result.null_move_searches > 0);
    CHECK_EQ(result.null_move_pv_searches, 0U);
    CHECK(root.is_legal(result.best_move));
}

TEST_CASE(verified_null_move_preserves_zugzwang_score) {
    constexpr std::string_view fen = "8/8/8/2k5/8/2P5/2K5/8 w - - 0 1";
    Position enabled_root = position(fen);
    Position disabled_root = position(fen);
    TranspositionTable enabled_table(4);
    TranspositionTable disabled_table(4);
    Searcher enabled_searcher(enabled_table);
    Searcher disabled_searcher(disabled_table);
    SearchLimits enabled{.depth = 8};
    SearchLimits disabled{.depth = 8};
    disabled.enable_null_move = false;
    const SearchResult with_null = enabled_searcher.search(enabled_root, enabled);
    const SearchResult without_null = disabled_searcher.search(disabled_root, disabled);
    CHECK_EQ(with_null.score, without_null.score);
    CHECK_EQ(with_null.best_move, without_null.best_move);
}
```

- [ ] **Step 2: Run tests and confirm the PV/null assertion fails**

Run `mingw32-make -f Makefile.blaze test`.

Expected: the new instrumentation or PV restriction test fails before the typed
null-move guard is implemented.

- [ ] **Step 3: Implement guarded dynamic reduction**

Permit null move only inside:

```cpp
if constexpr (node_type == NodeType::NonPV) {
    const int static_eval = evaluate_position(position);
    const bool null_enabled =
#ifndef NDEBUG
        context.limits.enable_null_move;
#else
        true;
#endif
    if (null_enabled && allow_null && depth >= 3 && !checked &&
        position.rule50() < 90 && beta < search_mate_threshold &&
        static_eval >= beta &&
        has_non_pawn_material(position, position.side_to_move())) {
        const int eval_term = std::clamp((static_eval - beta) / 180, 0, 3);
        const int reduction = std::min(depth - 1, 3 + depth / 4 + eval_term);
        // make null, search a NonPV null window with allow_null=false, unmake
    }
}
```

Increment `null_move_searches` only in debug builds. The debug attempt helper
increments `null_move_pv_searches` if its compile-time node type is not
`NonPV`; the production guard remains inside `if constexpr
(node_type == NodeType::NonPV)`, so that count must remain zero. Keep null move
out of Root/PV nodes at compile time.

- [ ] **Step 4: Add high-depth verification**

When the null score fails high, return directly only below depth 10. At depth 10
or greater, search the original position with null move disabled:

```cpp
if (null_score >= beta) {
    if (depth < 10) {
        return std::min(null_score, search_mate_threshold - 1);
    }
#ifndef NDEBUG
    ++context.null_move_verifications;
#endif
    PvLine verification_pv;
    const int verification = negamax<NodeType::NonPV>(
        position, depth - reduction, beta - 1, beta, ply,
        context, verification_pv, false);
    if (verification >= beta) {
        return verification;
    }
}
```

The unchanged `ply` is intentional because verification searches the same
position. `allow_null=false` prevents verification inside verification.

- [ ] **Step 5: Run safety, deterministic, and release-build tests**

Run:

```powershell
mingw32-make -f Makefile.blaze clean
mingw32-make -f Makefile.blaze test
mingw32-make -f Makefile.blaze blaze
```

Expected: all tests pass and the release build contains no debug null controls.

- [ ] **Step 6: Commit the null-move candidate**

```powershell
git add src/blaze/search/search.h src/blaze/search/search.cpp tests/search/test_search.cpp
git commit -m "search: verify null move at non-PV nodes"
```

### Task 3: Search real root moves at parallel depth one

**Files:**
- Modify: `src/blaze/search/search.cpp`
- Modify: `tests/search/test_search.cpp`

**Interfaces:**
- Consumes: typed node search from Task 1.
- Produces: identical one-thread/four-thread depth-one best move and score.

- [ ] **Step 1: Add the hanging-queen regression**

```cpp
TEST_CASE(parallel_depth_one_searches_moves_instead_of_returning_generation_order) {
    Position root = position("4k3/8/8/8/8/8/q7/R3K3 w - - 0 1");
    TranspositionTable one_table(2);
    TranspositionTable four_table(2);
    Searcher one_searcher(one_table);
    Searcher four_searcher(four_table);
    SearchLimits one{.depth = 1};
    SearchLimits four{.depth = 1};
    four.threads = 4;
    const SearchResult single = one_searcher.search(root, one);
    const SearchResult parallel = four_searcher.search(root, four);
    CHECK_EQ(parallel.best_move, single.best_move);
    CHECK_EQ(parallel.score, single.score);
    CHECK(parallel.nodes > 0);
    CHECK_EQ(move_to_uci(parallel.best_move), "a1a2");
}
```

- [ ] **Step 2: Run and confirm failure**

Run `mingw32-make -f Makefile.blaze test`.

Expected: the parallel result has zero nodes or differs from the one-thread move.

- [ ] **Step 3: Remove the depth-one static-evaluation shortcut**

Delete the `if (depth == 1)` branch in `search_parallel`. Let the existing root
task loop search every legal child with child depth zero. Preserve the shared
node budget and aggregate child nodes before choosing the best completed task.

- [ ] **Step 4: Run all search tests**

Run `mingw32-make -f Makefile.blaze test`.

Expected: the new regression and existing parallel score/network tests pass.

- [ ] **Step 5: Commit the root-search candidate**

```powershell
git add src/blaze/search/search.cpp tests/search/test_search.cpp
git commit -m "fix: search parallel root moves at depth one"
```

### Task 4: Enforce selective cumulative extensions and safe maximum ply

**Files:**
- Modify: `src/blaze/search/search.h`
- Modify: `src/blaze/search/search.cpp`
- Modify: `src/blaze/search/stack.h`
- Modify: `tests/search/test_search.cpp`

**Interfaces:**
- Produces: `constexpr int maximum_extensions = 2` per path.
- Produces: debug-only `SearchResult::maximum_extension_count`.
- Produces: debug-only `SearchLimits::maximum_ply = 128` for bounded regression tests.
- Produces: `Searcher::maximum_ply_score(Position&, int)`.

- [ ] **Step 1: Add extension and maximum-ply regressions**

```cpp
TEST_CASE(check_and_recapture_extensions_respect_the_path_budget) {
    Position root = position("6k1/5ppp/8/8/8/8/5PPP/3Q2K1 w - - 0 1");
    TranspositionTable table(8);
    Searcher searcher(table);
    const SearchResult result = searcher.search(root, SearchLimits{.depth = 10});
    CHECK(result.maximum_extension_count <= 2);
    CHECK(root.is_legal(result.best_move));
}

TEST_CASE(maximum_ply_checked_position_is_not_scored_as_ordinary_static_eval) {
    Position root = position("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    TranspositionTable table(1);
    Searcher searcher(table);
    SearchLimits limits{.depth = 4};
    limits.maximum_ply = 1;
    const SearchResult result = searcher.search(root, limits);
    CHECK_EQ(result.score, search_mate_score - 1);
}
```

- [ ] **Step 2: Run tests and retain the extension failure**

Run `mingw32-make -f Makefile.blaze test`.

Expected: the extension counter is absent or the existing stacked extension path
exceeds the budget.

- [ ] **Step 3: Select at most one extension per move**

Initialize `context.stack[0].extension_count = 0`. Replace additive check plus
recapture depth with:

```cpp
constexpr int maximum_extensions = 2;
const int used = context.stack[static_cast<std::size_t>(ply)].extension_count;
const int see_score = recaptures
    ? static_exchange_evaluation(position, move)
    : std::numeric_limits<int>::min();
int extension = 0;
if (used < maximum_extensions) {
    const bool selective_check = gives_check && depth >= 3 && move_count < 4;
    const bool sound_recapture = recaptures && depth <= 8 && see_score >= 0;
    extension = selective_check || sound_recapture ? 1 : 0;
}
context.stack[static_cast<std::size_t>(ply + 1)].extension_count = used + extension;
const int full_depth = depth - 1 + extension;
```

Compute `see_score` before `make_move`; never call parent-position SEE on the
already-mutated child. Increment the debug maximum from the child cumulative
count. In debug builds, use `context.limits.maximum_ply`; release builds retain
the compile-time value 128.

- [ ] **Step 4: Add safe maximum-ply resolution**

Implement:

```cpp
int Searcher::maximum_ply_score(Position& position, int ply) const {
    if (!in_check(position)) {
        return evaluate_position(position);
    }
    MoveList evasions;
    generate_legal(position, evasions);
    return evasions.empty() ? -search_mate_score + ply : 0;
}
```

Call it from full search and qsearch when `ply >= maximum_ply`. The zero score is
a bounded non-mate fallback for an unresolved checked position, never an
ordinary static evaluation.

- [ ] **Step 5: Run full tests and clean-room verification**

Run:

```powershell
mingw32-make -f Makefile.blaze clean
mingw32-make -f Makefile.blaze test
mingw32-make -f Makefile.blaze verify-clean-room
```

Expected: all tests and clean-room verification pass.

- [ ] **Step 6: Commit the extension/max-ply candidate**

```powershell
git add src/blaze/search/search.h src/blaze/search/search.cpp src/blaze/search/stack.h tests/search/test_search.cpp
git commit -m "search: bound extensions and resolve max-ply checks"
```

### Task 5: Qualify TT cutoffs by the rule-50 count

**Files:**
- Modify: `src/blaze/search/transposition_table.h`
- Modify: `src/blaze/search/transposition_table.cpp`
- Modify: `src/blaze/search/search.cpp`
- Modify: `tests/search/test_transposition_table.cpp`
- Modify: `tests/search/test_search.cpp`

**Interfaces:**
- Produces: `TTData::rule50`.
- Produces: `store(..., int ply, int rule50)` and `probe(key, int ply)` where the
  caller may use the move but must match `TTData::rule50` before a cutoff.

- [ ] **Step 1: Add failing TT and search regressions**

```cpp
TEST_CASE(transposition_table_preserves_rule50_identity) {
    TranspositionTable table(1);
    const Move move(Square::E2, Square::E4, MoveFlag::DoublePush);
    table.store(99, move, 120, 8, Bound::Lower, 2, 7);
    const auto hit = table.probe(99, 2);
    CHECK(hit.has_value());
    CHECK_EQ(hit->rule50, 7);
}

TEST_CASE(search_does_not_reuse_unsafe_rule50_cutoff) {
    TranspositionTable table(4);
    Searcher searcher(table);
    Position low = position("4k3/8/8/8/8/8/3Q4/4K3 b - - 0 1");
    Position near_draw = position("4k3/8/8/8/8/8/3Q4/4K3 b - - 99 1");
    const SearchResult first = searcher.search(low, SearchLimits{.depth = 4});
    const SearchResult second = searcher.search(near_draw, SearchLimits{.depth = 4});
    CHECK(first.score != 0);
    CHECK_EQ(second.score, 0);
}
```

- [ ] **Step 2: Run and confirm compile/test failure**

Run `mingw32-make -f Makefile.blaze test`.

Expected: the new seven-argument `store` call does not compile.

- [ ] **Step 3: Store compressed rule-50 identity**

Add `std::uint8_t rule50` to `Entry` and `TTData`. Clamp the supplied value:

```cpp
const auto stored_rule50 = static_cast<std::uint8_t>(std::clamp(rule50, 0, 100));
```

Pass `position.rule50()` to every `table_.store`. A TT hit may always contribute
its move for ordering, but its bound may cut off only when:

```cpp
const bool rule50_safe = tt_hit->rule50 == std::min(position.rule50(), 100);
if (rule50_safe && tt_hit->depth >= depth && /* bound condition */) {
    return tt_hit->score;
}
```

This intentionally conservative exact-count rule remains until the packed TT
plan introduces a separately validated bucket policy.

- [ ] **Step 4: Update every TT test call explicitly**

Pass `0` to existing `store` calls unless the test targets rule-50 behavior.
Verify mate-distance normalization still round-trips independently of rule50.

- [ ] **Step 5: Run all C++ tests**

Run `mingw32-make -f Makefile.blaze clean; mingw32-make -f Makefile.blaze test`.

Expected: all TT, mate, rule-50, and search tests pass.

- [ ] **Step 6: Commit the TT-safety candidate**

```powershell
git add src/blaze/search/transposition_table.h src/blaze/search/transposition_table.cpp src/blaze/search/search.cpp tests/search/test_transposition_table.cpp tests/search/test_search.cpp
git commit -m "search: qualify TT cutoffs by rule-50 state"
```

### Task 6: Expand SEE reference coverage

**Files:**
- Modify: `tests/search/test_see.cpp`

**Interfaces:**
- Consumes: current `static_exchange_evaluation(const Position&, Move)`.
- Produces: immutable regression cases for the later bitboard `see_ge` rewrite.

- [ ] **Step 1: Add exact promotion and legality cases**

```cpp
TEST_CASE(see_accounts_for_promoting_recaptures) {
    Position root = position("4k3/8/8/8/8/8/Pr6/1R2K3 b - - 0 1");
    const Move capture = find_move(root, "b2b1");
    CHECK(capture.is_valid());
    CHECK(static_exchange_evaluation(root, capture) < 0);
}

TEST_CASE(see_excludes_pinned_recaptures) {
    Position root = position("4k3/4n3/8/5p2/4B3/8/8/4R1K1 w - - 0 1");
    const Move capture = find_move(root, "e4f5");
    CHECK(capture.is_valid());
    CHECK(static_exchange_evaluation(root, capture) >= 100);
}

TEST_CASE(see_handles_en_passant_discovered_lines) {
    Position root = position("4r1k1/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    CHECK(!find_move(root, "e5d6").is_valid());
}
```

- [ ] **Step 2: Run and classify each failure**

Run `mingw32-make -f Makefile.blaze test`.

Expected: the promoting-recapture case exposes the known promotion semantics
defect; legality cases either pass or document an incorrect attacker sequence.

- [ ] **Step 3: Correct only SEE promotion semantics if needed**

When constructing a pawn recapture to the back rank inside the recursive SEE
reference, enumerate queen, rook, bishop, and knight promotions and use the
maximizing legal promotion value. Do not implement the future bitboard swap-list
in this task.

- [ ] **Step 4: Run SEE and full regression suites**

Run `mingw32-make -f Makefile.blaze clean; mingw32-make -f Makefile.blaze test`.

Expected: all SEE cases and the full suite pass.

- [ ] **Step 5: Commit the SEE reference candidate**

```powershell
git add src/blaze/search/see.cpp tests/search/test_see.cpp
git commit -m "test: cover SEE promotion and legality edges"
```

### Task 7: Add deterministic fixed-node search signatures

**Files:**
- Create: `tools/search_signature.py`
- Create: `tools/test_search_signature.py`
- Create: `testdata/search/correctness-v1.epd`
- Create: `provenance/search-corpora.json`
- Modify: `Makefile.blaze`

**Interfaces:**
- Produces: `parse_info(line: str) -> dict[str, object]`.
- Produces: `UciEngine` persistent process with `search(fen, nodes)` and `close()`.
- Produces: JSON containing engine/corpus SHA-256, UCI identity/options, settings,
  per-position result, aggregate nodes/time, and a canonical signature hash.

- [ ] **Step 1: Add parser and malformed-output tests**

```python
class SearchSignatureTests(unittest.TestCase):
    def test_parse_info_extracts_score_nodes_and_pv(self):
        parsed = parse_info(
            "info depth 7 seldepth 11 score cp 34 nodes 1000 time 5 pv e2e4 e7e5"
        )
        self.assertEqual(parsed["depth"], 7)
        self.assertEqual(parsed["seldepth"], 11)
        self.assertEqual(parsed["score"], {"kind": "cp", "value": 34})
        self.assertEqual(parsed["nodes"], 1000)
        self.assertEqual(parsed["pv"], ["e2e4", "e7e5"])

    def test_parse_info_rejects_incomplete_score(self):
        with self.assertRaisesRegex(ValueError, "score"):
            parse_info("info depth 4 score cp nodes 100")

    def test_signature_changes_when_engine_identity_changes(self):
        first = canonical_signature({"engine_sha256": "a" * 64, "positions": []})
        second = canonical_signature({"engine_sha256": "b" * 64, "positions": []})
        self.assertNotEqual(first, second)
```

- [ ] **Step 2: Run and confirm import failure**

Run:

```powershell
python -m unittest tools.test_search_signature -v
```

Expected: import failure because `tools/search_signature.py` does not exist.

- [ ] **Step 3: Implement strict parsing and canonical hashing**

Use a token cursor that rejects missing numeric values, accepts `score cp` and
`score mate`, retains the final complete `info` before `bestmove`, and validates
the best move with python-chess. Hash canonical JSON with sorted keys and compact
separators:

```python
def canonical_signature(payload: dict[str, object]) -> str:
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()
```

The persistent process must complete `uci` and `isready`, record every advertised
option line, issue `ucinewgame` plus `isready` between positions, enforce a
timeout, and always terminate with `quit` then kill-on-timeout.

- [ ] **Step 4: Add the deterministic corpus and provenance**

Create at least 64 legal EPD lines covering opening, middlegame, tactical,
quiet, checked, castling, en-passant, promotion, low-material, and near-rule-50
positions. Each line includes a stable `id` operation. Generate positions only
from independently selected legal sequences and hand-authored edge cases; do
not use Stockfish test books.

Record schema version, path, SHA-256, position count, source method, license
`CC0-1.0`, purpose `deterministic search correctness and node signatures`, and
`stockfish_derived: false` in `provenance/search-corpora.json`.

- [ ] **Step 5: Add CLI and Make target**

The CLI accepts:

```text
--engine PATH --corpus PATH --nodes 10000 --threads 1 --hash-mb 64 --output PATH
```

Add:

```make
search-signature-test:
	python -m unittest tools.test_search_signature -v
```

- [ ] **Step 6: Run tests and two identical signatures**

Run:

```powershell
mingw32-make -f Makefile.blaze blaze search-signature-test
python tools/search_signature.py --engine build/blaze/blaze.exe --corpus testdata/search/correctness-v1.epd --nodes 1000 --threads 1 --hash-mb 16 --output build/blaze/signature-a.json
python tools/search_signature.py --engine build/blaze/blaze.exe --corpus testdata/search/correctness-v1.epd --nodes 1000 --threads 1 --hash-mb 16 --output build/blaze/signature-b.json
```

Expected: both commands succeed and the canonical per-position chess signature
(excluding wall-clock fields) is identical. NPS/time are reported separately and
are not part of the deterministic signature.

- [ ] **Step 7: Commit measurement infrastructure**

```powershell
git add tools/search_signature.py tools/test_search_signature.py testdata/search/correctness-v1.epd provenance/search-corpora.json Makefile.blaze
git commit -m "bench: add deterministic fixed-node search signatures"
```

### Task 8: Run the correctness-foundation promotion gate

**Files:**
- Create: `docs/experiments/correctness-foundation-2026-07-18.md`
- Update after acceptance: accepted baseline manifest location selected by the existing experiment framework.

**Interfaces:**
- Consumes: exact pre-series accepted binary and final candidate binary.
- Produces: complete deterministic, performance, local/cloud match, and artifact evidence.

- [ ] **Step 1: Freeze both binaries and all identities**

Build the accepted base in the detached baseline worktree and the candidate in
the feature worktree with identical compiler and flags. Copy neither source tree
into the other. Record Git commit, executable SHA-256, compiler identity, flags,
CPU/OS, Threads, Hash, opening hash, runner hash, and network identity.

- [ ] **Step 2: Run complete verification**

```powershell
mingw32-make -f Makefile.blaze clean
mingw32-make -f Makefile.blaze test
mingw32-make -f Makefile.blaze experiment-test cloud-test
mingw32-make -f Makefile.blaze perft-driver
python tools/differential_perft.py --engine build/blaze/perft_driver.exe --positions 1000 --depth 3 --seed 20260718
mingw32-make -f Makefile.blaze uci-stress
mingw32-make -f Makefile.blaze verify-clean-room
git diff --check HEAD~7..HEAD
```

Expected: all commands pass; canonical perft totals remain unchanged; no illegal
move, crash, timeout, or stale UCI result occurs.

- [ ] **Step 3: Compare deterministic and performance evidence**

Run both binaries on the complete correctness corpus at 1k, 5k, 10k, 50k, and
100k nodes. Also run at fixed 50 ms and 200 ms. Record best-move agreement,
score/PV changes, completed depths, node counts, median NPS, IQR, and total
wall-clock cost. Manually classify every changed tactical result before match
testing.

- [ ] **Step 4: Run 1,000 paired games across local and cloud lanes**

Use 500 color-paired games locally and 500 through frozen cloud shards, or the
closest even split supported by available workers. Face the immediately previous
accepted binary using the same large versioned confirmation suite, one thread,
fixed Hash, and identical network settings. Validate all shard identities before
aggregation.

Report wins, draws, losses, pentanomial counts, crashes, illegal moves, time
losses, infrastructure failures, platform strata, binary hashes, opening hash,
runner hash, and compute cost. Do not state a precise Elo gain from this gate.

- [ ] **Step 5: Promote or reject each candidate independently**

Promote only candidates with green correctness evidence, no material fixed-node
regression, no unexplained performance loss, and no match evidence of material
strength regression. For small uncertain effects, run the existing sequential
STC framework on disjoint openings before promotion. Preserve rejected evidence
in the experiment report.

- [ ] **Step 6: Commit the evidence report**

```powershell
git add docs/experiments/correctness-foundation-2026-07-18.md
git commit -m "docs: record correctness foundation qualification"
```

## Plan self-review

- The plan covers every item in the design's initial execution boundary.
- Typed interfaces and TT signatures are consistent across tasks.
- Debug controls are excluded from release builds.
- The slow SEE and move-generation paths remain reference oracles only.
- The 1,000-game requirement is enforced without overstating statistical power.
- Hot-path replacement, advanced pruning, evaluation, SMP, and time management
  remain in later milestone plans and are not silently dropped from the program.
