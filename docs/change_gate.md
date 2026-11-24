# Proposal: a change gate for module boundaries, function complexity and doubled text

**Status: a proposal for the maintainer's decision; nothing on this page is implemented, and the pull request that
adds it changes no other file. The numbers were measured at `dde82f2` on the tree and on the 300 squash merges
that end with it (one merge round on 2026-11-23 in the original repository, whose history this one carries), with a standard-library prototype that is
not part of this pull request.**

GitHub's search counts 3,680 pull requests merged in the original repository from 2026-11-21 to 2026-11-23. [AGENTS.md](../AGENTS.md) is
written for the coding agents that many contributors use, and the template asks about them. Review and CI catch
what a diff shows and what a test exercises. Three things a diff does not show: a new dependency on another part of
the tree, hidden behind a `sys.path` insert; a function that has grown past the point where a person or an agent can
change it without breaking a case it did not look at; and text that a merge doubled. This page proposes one
deterministic check for those three. It compares a pull request with its base, blocks only what the pull request
adds, tolerates what is already on `main`, and tells the author why a finding matters and how to fix it. Because CI
had finished on few pull requests when they were merged (section 1.4), it runs where the code is written and in the
merge round as well as in CI.

Claims are marked:

- **Measured** — counted on the tree or the history at `dde82f2`; section 6 says how.
- **Proposed** — what this page asks the maintainer to accept.
- **Open** — a question for the maintainer, collected in section 5.

## 1. What was measured

### 1.1 Dependencies between parts of the tree

**Measured.** Python files reach each other through 1,262 `sys.path.insert` calls, which is why 1,527 imports carry
`# noqa: E402`. Resolving those paths gives 2,182 import edges between repository files.
These boundaries hold today with no exception:

- `make_game.py` imports only `tools/`.
- `viewer/` imports nothing from the repository.
- `tools/` never imports `scripts/`, `make_game.py` or `viewer/`; `scripts/` never imports `tests/`.
- No import cycle crosses two areas; inside `tools/` there are two, each of two files
  (`tools/calibrated_texture.py` with `tools/ttpf_mips.py`, `tools/hw_oracle/drm.py` with `tools/hw_oracle/runner.py`).
- No production C++ file (`runtime/src`, `runtime/shims`, `runtime/xma_decoder`) includes a `runtime/test` file, and
  no file-level `#include` cycle exists.

One boundary is crossed: production code imports test code through 39 edges from 32 files. 26 edges go from
`tools/` to `tests/`, 22 of them to `tests/semantic/spirv_mini_interp.py`, which is in effect a shared reference
interpreter kept in a test folder; 8 go from `tools/` and 5 from `scripts/` to the GPU fixture helpers in
`runtime/test/shaders`. The `scripts/` ones are test runners (`scripts/run_gpu_tests.py`,
`scripts/bench_wave64.py`) using the fixture helpers they run, and the rules below allow them. Two of the 300 merges
added another edge from `tools/`, both new tools that copied the pattern (`tools/wave_diff.py:53` in `db77244`,
`tools/div_fixup_diff.py:104` in `5e54bda`).

To be fair to the current setup: 31 of the 32 importing files are themselves imported by a test, so an edit to
`spirv_mini_interp.py` that breaks a tool makes a test fail once CI runs (section 1.4 shows how often it has not
run by the merge). What the edge costs even then is legibility. The file looks like test code, and nothing beside it says that 22 tools depend on
it; an author, human or agent, edits it with that context missing.

The C++ runtime has one directory-level cycle: `runtime/shims` includes `runtime/src` 27 times, and `runtime/src`
includes `runtime/shims` 7 times, all from `runtime/src/main.cpp` and `runtime/src/pe_loader.cpp`. Whether that is the
intended layering is question 5 in section 5.

### 1.2 Function complexity

**Measured**, cyclomatic complexity (McCabe for Python, lizard's CCN for C++; section 6):

| Area | Functions | Complexity 11-20 | 21-50 | Above 50 | Largest |
|---|---|---|---|---|---|
| `tools/` (Python) | 5,579 | 774 | 399 | 95 | 1,261 |
| `make_game.py` and `scripts/` | 291 | 37 | 15 | 3 | 280 |
| `runtime/src` and `runtime/shims` (C++) | 2,514 | 165 | 88 | 27 | 370 |

The largest is `Gen._insn` in `tools/rdna2spirv.py` (line 6722): 4,889 lines and a complexity of 1,261 in an
18,229-line file. One `if`/`elif` chain of 182 arms holds nearly all of that; section 2.3 judges such a function arm
by arm.

What the 300 merges did to production functions, checked with the rule proposed in section 2.3:
89 findings in 48 pull requests. Each was labelled after reading its diff
(section 6 says by whom): 66 made a function measurably harder to change (new nested logic, a 9 -> 20 or 10 -> 27
jump, an optional feature threaded through an existing function), 21 are correct measurements whose value is
arguable, and 2 are artefacts of the tool (section 2.3 lists them). Examples of the first kind:

| Commit | Function | Complexity before -> after |
|---|---|---|
| `0a8a552` | `export_bank` in `tools/awc_opus.py` | 19 -> 30 |
| `9a9ca1e` | `ExecuteIndirect` in `runtime/shims/d3d12_com.cpp` | 10 -> 27 |
| `21e72aa` | `scan_offsets` in `tools/smh_groom.py` | 9 -> 20 |
| `86d1c86` | `verify_manifest` in `tools/manifest_verifier.py` | 23 -> 33 |
| `c1aa89e` | `verify` in `tools/world_drawable_verify.py` (new) | 34 |

### 1.3 Text doubled by a merge

**Measured.** Eight blocks of six or more lines repeat within the same file on `main`, the trace of a pull request
merged twice or a conflict resolved by keeping both sides (line numbers of the first and last non-blank line of each
copy; the last row is the first nine lines of the row four above it, which appear a third time):

| File | First copy | Second copy |
|---|---|---|
| `docs/models.md` | 568-651 | 653-736 |
| `docs/animation.md` | 971-1023 | 1052-1104 |
| `docs/animation.md` | 1025-1050 | 1106-1131 |
| `docs/shader_validation.md` | 1998-2040 | 2061-2103 |
| `docs/gbuffer.md` | 13-49 | 52-88 |
| `docs/testing.md` | 337-353 | 355-371 |
| `docs/design.md` | 576-590 | 592-606 |
| `docs/shader_validation.md` | 2061-2069 | 2174-2182 |

The existing guards pass on all of them: `tests/test_docs_repeated_lines.py` compares prose lines at most three lines
apart and `tests/test_docs_list_items.py` list items at most eight apart, which a doubled section of 84 lines never
is. `scripts/merge_check.py` finds the doubling when it is run before a merge round; nothing finds it after. No new
doubled block appeared in the 300 merges, so this part of the check guards a failure that is rare now, not frequent.

### 1.4 Whether a check runs before the merge

**Measured** in the original repository. GitHub's search returned 1,000 of the 1,252 pull requests merged there on 2026-11-23 (it stops at 1,000);
all of them came from forks. The CI workflow on the head commit each was merged with had:

| CI workflow at merge | Pull requests |
|---|---|
| passed | 55 |
| failed | 282 |
| queued, pending or still running | 661 |
| cancelled, or no run | 2 |

After a merge the push run does not finish either: 164 of the last 165 CI runs for pushes to `main` were cancelled
by the next push, and the last one was still pending when read (2026-11-23, about 20:00 UTC). At `867d922` three
documentation tests fail on `main`. `tests/test_docs_cli_flags.py` and `tests/test_docs_links.py` have failed since `cc67780`, and later
merges changed the items they report; `tests/test_docs_design_rows.py` has failed since `0980bea` (both found with
`git bisect`). Neither pull request had a finished CI run when it was merged.

So a check that lives only in a CI job would have run before the merge for about one pull request in eighteen. This repository merges the same way: of
its 40 most recently merged pull requests (read on 2026-11-24 at about 03:20 UTC), 19 had no CI run at merge, 18 had
at least one failing CI job, 2 had jobs still pending and 1 had only passing or skipped jobs.
Section 2.1 therefore puts it where the code is written and where merges are decided, not only in CI.

## 2. The proposed check

### 2.1 How it runs

**Proposed.** One standard-library script under `scripts/`, run with `python -I` like the other tools. The same
command gives the same verdict in three places:

- **By the author, before opening or updating the pull request.** It compares the working tree with the merge base
  of `origin/main`. [AGENTS.md](../AGENTS.md) gives an agent the command, and the pull request's `Tested` section
  names the result, the way it names the tests that were run.
- **In the merge round, before the push.** One command takes the round's pull requests in merge order and checks each
  against `main` plus the ones before it, the way `scripts/merge_check.py` checks a round for repeated merges. It
  needs no runner, so it does not wait for CI approval or a free runner slot.
- **In CI, when the pull request's run is approved.** It compares the test merge with its first parent, as the
  `change-scope` job of `ci.yml` already does.
- It looks only at what the pull request changed: the files it touched are parsed again, the import and include
  graphs are re-linked, and the functions whose lines changed are measured on both sides. On the 300 merges the
  prototype took 80 ms per pull request (90th percentile 170 ms, slowest 1.9 s); building the state
  of a whole tree from cold takes 8.2 s, so a CI job without a cache costs about ten seconds.
- What is on the base is tolerated by construction, so there is no baseline list for every pull request to edit and
  conflict on. The one shared file is the exception list (section 2.5), which changes only when a reviewer approves an
  exception.
- No model, no network, no service: the verdict is the same on every machine. C++ complexity uses lizard, pinned in
  the CI job; everything else is the standard library.

### 2.2 The rules

**Proposed.** "Blocks" means the CI job fails; "reports" means the finding is listed and the job passes.

| ID | Rule | Kind | On `main` today | Findings in the 300 merges |
|---|---|---|---|---|
| P1 | `make_game.py` imports only `tools/` | blocks | 0 | 0 |
| P2 | `viewer/` imports nothing from the repository | blocks | 0 | 0 |
| P3 | `tools/` does not import `scripts/`, `make_game.py` or `viewer/` | blocks | 0 | 0 |
| P4 | `tools/` does not import `tests/` or `runtime/` | blocks | 34 edges, tolerated | 2 |
| P5 | `scripts/` does not import `tests/`, `make_game.py` or `viewer/` | blocks | 0 | 0 |
| P6 | no new or larger Python import cycle | blocks | 2 cycles, tolerated | 0 |
| C1 | production C++ does not include `runtime/test` | blocks | 0 | 0 |
| C2 | no new or larger `#include` cycle | blocks | 0 | 0 |
| D1 | no new block of six lines repeated within one Markdown file | blocks | 8 blocks, tolerated | 0 |
| X1 | a new production function has complexity 10 or lower | blocks | — | 44 in 23 pull requests |
| X2 | a changed production function at 10 or lower stays at 12 or lower (`main`: 15) | blocks | — | 4 in 4 pull requests |
| X3 | a changed production function above 10 grows by at most 2 (`main`: 5) | blocks | — | 41 in 31 pull requests |
| R1 | a block of six or more code lines that duplicates code elsewhere | reports | — | 138 |
| R2 | an added skip or suppression (`unittest.skip`, `# noqa` other than E402, `# type: ignore`, ...) | reports | — | 24 |

An edge counts as new when it exists in the pull request and not in its base; a cycle counts as new when it is not
contained in one cycle of the base. Existing edges and cycles are never reported again.

### 2.3 The complexity rules in detail

**Proposed.**

- Production code only: `tools/`, `scripts/`, `make_game.py`, `viewer/`, `runtime/src`, `runtime/shims` and
  `runtime/xma_decoder`. Tests are left out; a test that checks forty cases in a row is long, not tangled.
- Python complexity is McCabe's: one plus each `if`/`elif`, conditional expression, `for`, `while`, `except`, each
  extra operand of `and`/`or`, each `for` and `if` of a comprehension, and each `case`. Nested functions are measured
  on their own. C++ uses lizard's CCN.
- A function counts as changed when any of its lines changed; it is compared with the function of the same qualified
  name in the base.
- Dispatch functions are judged one arm at a time. A function is a dispatch function when one `if`/`elif` chain
  testing a single subject against constants (`op == "..."`, `name in (...)`, `isinstance(x, ...)`) has at least
  eight arms and holds at least half of its decision points. A new arm then needs a complexity of 10 or lower, a
  changed arm keeps the +2 allowance, and code outside the arms may grow by 2. Adding a translator rule as one more
  arm of `Gen._insn` passes; adding a 14-branch arm does not.
- `main` and `wmain` may grow by 5 instead of 2, because argument and input checks land there. In the 300 merges
  this let 18 additions through, all labelled arguable: argument and input checks, and one +3 of other logic in
  `tools/hashdict.py`. `make_game.py` `main` going from 53 to 59 in one pull request still fails.
- Known artefacts, both from the 300 merges: `slot_name` in `runtime/shims/d3d12_xs_device.cpp` grew by five `case`
  labels of a name table (a C++ function is judged whole, since lizard reports no arms); the `VulkanPresenter`
  constructor in `runtime/src/vulkan_present.h` gained a parameter, so its name changed and it was judged as a new
  function. A renamed or moved function is judged as new in the same way.

### 2.4 What the author sees

**Proposed.** Every finding carries four parts: why the rule exists, exactly what triggered it, a fix chosen from the
shape of the change, and how to ask for an exception. The same findings are written three ways: a GitHub annotation on
the line in the pull request's diff, a job summary with the full text, and plain text for a local run. The prototype's
output for `0a8a552`, which grew `export_bank` from 19 to 30:

```text
ERROR  X3 tools/awc_opus.py:381  export_bank went from complexity 19 to 30; a changed function may end at 21
  why:  A function this branchy is too large to change safely: neither a person nor a coding agent keeps all its cases in view, so an edit breaks a case it did not look at.
  what: export_bank at tools/awc_opus.py:381-439: complexity 19 -> 30 (limit 10)
  what: added lines that carry decision points (11 on 8 lines): 383, 385, 397, 403, 432-433, 435, 437
  what:   tools/awc_opus.py:383 (+1): if cue_sheet:
  what:   tools/awc_opus.py:385 (+1): if os.path.exists(sheet):
  what:   tools/awc_opus.py:397 (+1): if cue_sheet:
  what:   tools/awc_opus.py:403 (+3): if cue_sheet and rate == 48000 and samples > 0:
  what:   tools/awc_opus.py:435 (+2): verified = {cid: limit for cid, limit in frame_limits.items() if channel_counts[cid] == 1}
  fix:  4 of the added branches test `cue_sheet` (lines 383, 397, 403, 432): that is one optional feature threaded through export_bank. Put its steps into a function of their own and call it where the feature applies.
  fix:  It passes at complexity 21 or lower (now 30): moving 9 decision points out is enough.
  else: If the rule is wrong for this case, add {"rule": "X3", "path": "tools/awc_opus.py", "key": "export_bank", "reason": "..."} to gate_rules.json in this pull request; it counts once a reviewer approves it.

```

and for `5e54bda`, the second of the two new imports of test code:

```text
ERROR  P4 tools/div_fixup_diff.py:104  new import tools/div_fixup_diff.py -> tests/semantic/spirv_mini_interp.py: tools/ must not import tests/ or runtime/ files (existing edges are tolerated, new ones are not)
  why:  Hidden cross-module coupling: the imported file is changed as if nothing on this side depended on it, so an edit there can break this file without the author or reviewer expecting it.
  what: tools/div_fixup_diff.py:104 imports tests/semantic/spirv_mini_interp.py (import spirv_mini_interp)
  fix:  `spirv_mini_interp` is shared code kept in a test folder: 21 other tools/ files already import it (tools/buffer_swizzle_probe.py, tools/bvh_tri_probe.py, tools/f32_fuzz.py, ...). Move tests/semantic/spirv_mini_interp.py to tools/ in its own pull request (the tests then import it from tools/), and import it from there.
  fix:  Do not add tests/semantic to sys.path from tools/div_fixup_diff.py: that is the hidden edge this rule stops.
  else: If the rule is wrong for this case, add {"rule": "P4", "path": "tools/div_fixup_diff.py", "key": "tests/semantic/spirv_mini_interp.py", "reason": "..."} to gate_rules.json in this pull request; it counts once a reviewer approves it.

```

### 2.5 Exceptions

**Proposed.** A finding that is wrong for a case is not fixed by working around the check. The author adds an entry
with the rule, the path, the edge or function and a reason to one exception file at the repository root, in the same
pull request; the finding then turns into a note, and the entry is visible in the diff for the reviewer to approve or
refuse. An entry that no longer matches anything is reported so the file does not grow stale.

## 3. What it does not do

- It changes no existing code and asks nobody to fix what is already on `main`. The 34 imports of test code from
  `tools/`, the two import cycles and the eight doubled blocks stay until someone chooses to fix them (section 4).
- It does not judge whether code is correct; the tests and the reviewer still do that.
- It blocks nothing on code duplication or on skips: the tests copy their templates on purpose, and nearly every skip
  added in the 300 merges was a missing tool, platform or numpy, which is legitimate.
- It needs no model and no service. The author adapted it from a gate they use in their own projects; there, a local
  model chose which documents an agent should read before a change. That part is left out here: the rules file and
  the messages say what applies to a change, and the same verdict comes out on every machine.

## 4. Rollout

**Proposed**, each step its own pull request:

1. This page, for the decision.
2. The implementation: the script with its merge-round mode, the rules and exception files, a standard-library test
   file under `tests/` (the CI loop picks it up, as `tests/test_ci_registration.py` requires), a job on
   `ubuntu-24.04`, a few lines in [AGENTS.md](../AGENTS.md) that give an agent the local command and the map of which
   area may import which, and one line in the pull request template for the result.
3. Follow-up items for the coordination issue, for anyone to claim: move `tests/semantic/spirv_mini_interp.py`,
   `tests/semantic/spirv_wave_interp.py` and `tests/spirv_interp.py` under `tools/` (removes 25 of the 34 imports
   of test code from `tools/` at once); delete the second copy of each doubled block in section 1.3; split the largest production
   functions, starting with those changed most often.

## 5. Decisions for the maintainer

**Open.**

1. Are the boundaries in section 2.2 the intended ones? In particular P4: should the reference interpreters move
   under `tools/` (proposed), or should `tests/semantic` be declared a library that tools may import?
2. Complexity: production code only, limit 10 for a new function, +2 for a changed one (+5 for `main`), dispatch
   arms judged one by one — accept, or which number to change?
3. Should the job block from the first day (proposed), given the 48 of 300 pull requests it would have
   stopped and the share of those that were real?
4. Should the merge round run it before each push (proposed, since CI has not finished for most merges; section 1.4),
   and should a pull request whose author reports no result be sent back? Where should the CI job live, in `ci.yml`
   or a workflow of its own, and who approves an entry in the exception file?
5. `runtime/src` includes `runtime/shims` from `main.cpp` and `pe_loader.cpp`, and the shims include `runtime/src`:
   is the loader the composition root that may include both, or should one direction be forbidden?

## 6. How the numbers were measured

- Tree: `dde82f2`. History: the 300 single-parent squash merges that end with it (`dde82f2` and the 299 before it),
  each compared with its parent; the commit below them is the root of the shallow clone and only served as a base.
- Python imports: resolved from the AST, through each file's own directory and every `sys.path` set-up it performs
  (`insert`, `append`, slice assignment, `extend`, paths from `conftest.py` and from an imported helper).
  `importlib` loads with a constant path count as edges. The resolver is a heuristic; it left no import of a
  repository module unresolved in the measured areas.
- C++ includes: quoted includes only, resolved against the including file's directory and then the union of the
  include directories in `runtime/CMakeLists.txt`.
- Complexity: Python on the AST with the definition in section 2.3; C++ with lizard 1.24.1.
- Duplication: windows of six significant lines (whitespace normalised; blank lines, comments and lines with fewer
  than four non-space characters skipped).
- Every blocking finding was labelled by reading its diff, by one reader: the coding agent that ran the backtest.
  Each label has a one-line reason (most generated from the label's class and the measured change, five written by
  hand), and the list can be posted here on request. "Genuine" and "arguable" are a
  judgement about value; every finding except the 2 artefacts is a correct measurement under the rule as written.
- Not measured: how often the rules with no finding in the window (P1 to P3, P5, P6, C1, C2, D1) would be wrong.
  The prototype's own test builds a small repository in which every rule fires once.
- The 300 merges are one merge round of the original repository (committed between 09:35 and 11:23 UTC-6 on 2026-11-23) of pull requests opened
  over a longer time; rates per pull request may differ in other rounds.
- The merge counts in the introduction come from GitHub's search (`is:pr is:merged merged:<date>`), read on
  2026-11-23 at about 20:00 UTC.
- Section 1.4: the GraphQL search for pull requests merged on 2026-11-23, with the check suites of each one's last
  commit, taking the suite of the workflow named CI; the push runs from the Actions list for `main`; the first failing
  commit of each documentation test by `git bisect` between the oldest commit of the window and `867d922`, and each
  pull request's checks from its page. All read on 2026-11-23 at about 20:00 UTC, before that repository went private. The
  figures for this repository come from the checks of its 40 most recently merged pull requests.
