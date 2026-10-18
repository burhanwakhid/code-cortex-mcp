# Branch-index soak, 2026-10-06

This page records the first run of the per-branch index (`pipeline/branch_slots.h`)
on real repositories. The runs used `scripts/branch-index-soak.py` on branch
`feat/per-checkout-index`.

## Method

For each repository, the script moves one checkout through a sequence of steps.
At each step it does these things:

1. It runs `index_repository` in the **on** cache (branch index enabled).
2. It runs `index_repository` in the **off** cache (`CBM_BRANCH_INDEX=0`).
   This is the plain incremental path, so it is the timing baseline for the
   same delta.
3. It runs a full index of the same checkout in a fresh **ground-truth** cache.
4. It compares the on graph and the off graph with the ground truth:
   - node counts per label and edge counts per type,
   - the `(label, qualified_name)` node set and the node locations,
   - the `(type, from_qn, to_qn)` edge set (all edge types, CALLS and IMPORTS
     included),
   - the `file_hashes` table. A stale hash would mean that the run did not
     re-read a changed file.
5. It checks `index_status` (`index_matches_checkout`, `head_sha`,
   `parked_branches`), `list_projects` (one project, no parked slots), and the
   SessionStart hook text (its node and edge counts must equal the index).
6. On a restore, it checks that the restored graph is the graph that was
   parked. The ground-truth diff must be equal, item for item, to the diff that
   the same checkout had when it was last indexed.

Step kinds: `seeded` (a new branch; the outgoing graph is parked and kept in
place as the seed), `restored` (a branch that was parked before), `none` with a
moved HEAD (a pull or a commit on the same branch), and a detached HEAD.

The script also runs two scenarios:

- **Offline checkout.** It indexes, checks out another branch with no server
  running, reads `index_status` and the SessionStart text, then starts the
  stdio server in the repository. It waits until `index_matches_checkout` is
  true (the session-start pending mark plus the watcher), switches back while
  the server runs, and waits again.
- **Worktrees.** It adds linked worktrees at `.worktrees/<x>`, at
  `nested-wt-<x>` (inside the repository, not under `.worktrees`), and as a
  sibling directory. It checks that the parent's index holds none of their
  files, that SessionStart and `index_status` in an unindexed worktree do not
  answer from the parent, and that each worktree indexes as its own project.

Hosts: macOS (Apple silicon, 36 GB) for this repository and redis, with the
`-O2` build `build/c` and the sanitizer build `build/int`. Linux `sim3` (32
cores, 62 GB) with the `-O2` build for redis, rocksdb, django, cpython and llvm.
The clones are shallow (`--depth 80`; llvm `--depth 40`). The "branches" are
local branches at commits 5 to 60 apart on the default branch.

Wall times are the CLI's wall time, which includes the supervised worker's
start and reap. On Linux the reap poll rounds times up by as much as about 1 s.

## Results

Columns: `on`, `off` and `full` are wall seconds. `DB` is the live database
after the run. `WAL peak` is the largest `-wal` size seen during the run. `slots`
is the total size of the parked databases. `on-only / both / off-only` count
the ground-truth difference items found only in the on graph, in both graphs,
or only in the off graph.

### This repository (macOS, `-O2`, 621 source files)

| # | step | switch | reparsed | on | off | full | DB MB | WAL peak MB | slots MB | on-only / both / off-only |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | start `main` | none | 621 | 6.7 | 3.9 | 3.6 | 103 | - | 0 | 0 / 0 / 0 |
| 1 | `pci` (new) | seeded | 29 | 10.3 | 10.2 | 4.8 | 105 | 66 | 103 | 0 / 673 / 0 |
| 2 | `old` = main~30 | seeded | 73 | 2.5 | 2.1 | 3.5 | 98 | 66 | 208 | 0 / 782 / 0 |
| 3 | back to `main` | restored | 0 | 0.5 | 2.1 | 3.5 | 103 | 64 | 204 | 0 / 0 / 1531 |
| 4 | `main` reset to main~15 | none | 35 | 19.8 | 19.7 | 3.6 | 99 | 66 | 204 | 121 / 689 / 297 |
| 5 | `main` forward again (pull) | none | 51 | 2.1 | 2.0 | 3.6 | 103 | 66 | 204 | 99 / 1589 / 289 |
| 6 | detached main~8 | seeded | 33 | 15.3 | 15.3 | 3.5 | 100 | 66 | 306 | 180 / 779 / 251 |
| 7 | `pci` | restored | 0 | 0.6 | 2.0 | 3.6 | 105 | 105 | 301 | 341 / 332 / 1643 |
| 8 | `old` | restored | 0 | 0.6 | 2.1 | 3.7 | 98 | 59 | 307 | 27 / 755 / 167 |
| 9 | `pc` (new) | seeded | 72 | 2.2 | 2.1 | 3.6 | 102 | 66 | 406 | 24 / 1546 / 131 |
| 10 | `main` | restored | 0 | 0.6 | 15.9 | 4.0 | 103 | 103 | 405 | 298 / 1390 / 826 |

Full against full (step 0, indexed twice) is identical.

### redis (macOS, `-O2`, 1753 files)

| # | step | switch | reparsed | on | off | full | DB MB | WAL peak MB | slots MB |
|---|---|---|---|---|---|---|---|---|---|
| 0 | start `soak-main` = tip~20 | none | 1753 | 3.3 | 3.3 | 3.3 | 218 | - | 0 |
| 1 | `soak-old` = tip~60 | seeded | 84 | 3.8 | 3.7 | 3.3 | 216 | 66 | 218 |
| 2 | `soak-feat` = tip~10 | seeded | 134 | 4.0 | 4.1 | 3.6 | 218 | 66 | 434 |
| 3 | `soak-main` | restored | 0 | 1.1 | 2.5 | 4.4 | 218 | 218 | 434 |
| 4 | `soak-main` pulled to tip | none | 65 | 3.7 | 3.5 | 3.5 | 218 | 66 | 434 |
| 5 | detached tip~5 | seeded | 8 | 4.4 | 4.2 | 3.3 | 218 | 66 | 652 |
| 6 | `soak-old` | restored | 0 | 0.9 | 4.1 | 3.4 | 216 | 216 | 653 |
| 7 | `soak-main` | restored | 0 | 0.8 | 4.1 | 3.5 | 218 | 218 | 652 |
| 8 | `soak-feat` | restored | 0 | 0.9 | 8.9 | 3.4 | 218 | 218 | 652 |

### Linux sim3 (`-O2`), same sequence as redis above

Restore rows are steps 3, 6, 7 and 8. Seeded rows are 1, 2 and 5. Step 4 is
the pull.

| repo (files) | full s | DB MB | seeded on s (off s) | pull on s (off s) | restore on s (off s) | restore WAL peak MB | slots MB at end |
|---|---|---|---|---|---|---|---|
| redis (1753) | 2.2 | 116 | 3.3-4.2 (3.2-5.2) | 3.5 (3.6) | 0.4-1.4 (3.4-6.9) | 115-116 | 347 |
| rocksdb (2072) | 5.9 | 261 | 7.0-11.7 (6.8-11.6) | 9.9 (9.6) | 0.8-1.0 (7.6-12.5) | 255-261 | 777 |
| django (4791), before bug 3 | 3.4 | 210 | 9.4-10.3 (9.7-10.2) | 9.8 (9.4) | 2.9-3.5 (9.1-11.3) | 210 | 629 |
| django (4791), after bug 3 | 3.4 | 210 | 5.4-5.9 (4.9-5.7) | 6.6 (6.6) | 0.7 (4.1-6.7) | 210 | 629 |
| cpython (5655) | 12.1 | 586 | 10.9-37.2 (10.6-36.0) | 26.9 (26.1) | 1.7 (18.0-40.9) | 585-587 | 1758 |
| llvm (159199) | 182-192 | 6892 | 383 (356) | 392 (371) | 22.0-28.1 (392-574) | 6894-6897 | 6895 (1 slot) |

The llvm sequence was shorter: start at tip~10, `soak-old` = tip~30 (seeded),
back (restored), pull to the tip, then two more restores.

### Ground truth, all repositories

- **Every restore brought back exactly the graph that was parked.** Its
  ground-truth diff equals, item for item, the diff of that checkout when it
  was last indexed: 4 restores here, 4 on redis (macOS), 4 under the
  sanitizer, and 4 + 4 + 4 + 3 on redis, rocksdb, cpython and llvm on Linux.
  The exception was django before the fix for bug 3 (see below). After the fix, its 4 restores are exact too. A restore of
  a graph that a full run built is identical to the ground truth.
- **Seeded switches, pulls and detached checkouts are as exact as a plain
  incremental run.** On every seeded step that starts from the same graph as
  the off run, the on graph and the off graph have the same differences from
  the ground truth (`on-only = off-only = 0`).
- **No stale file hashes, no stale node locations** in any step.
- The `on-only` items on later steps exist because the on graph and the off
  graph have different histories. After a restore, the on graph carries the
  drift of the run that built the parked graph. The off graph carries the
  drift of every run since step 0. The restore check above shows that none of
  these items comes from the switch itself.

The remaining differences are **pre-existing incremental-against-full drift**.
They are in both graphs. With the same sequence, `main`'s binary
(`3230252c`, no branch index) has the same counts on every step. For example,
at step 5 it is missing CALLS 274, USAGE 572 and WRITES 189 edges, which
equals the off run of this branch. The drift has these parts:

- CALLS and USAGE edges. Incremental runs skip cross-file LSP resolution
  (`pipeline_incremental.cpp`), so C++ repositories drift most: about 4000 to
  5000 CALLS edges on rocksdb and llvm, and about 1500 on cpython.
- `SEMANTICALLY_RELATED` and `SIMILAR_TO` edges, which are recomputed over a
  different input.
- `FILE_CHANGES_WITH` edges. Git history is not recomputed incrementally
  (a known limitation).
- `CONTAINS_FILE`, `CONTAINS_FOLDER`, `Route` and `EnvVar` items for added or
  removed files.

### Index status, hooks and list_projects

On every step of every repository, `index_matches_checkout` was true and
`head_sha` was the checkout's HEAD. `list_projects` showed only the one
project. The SessionStart brief's node and edge counts equalled the index,
because the brief is recomputed on a switch. The off runs never parked a
database and always reported `switch: none`.

### Offline checkout and the watcher

| run | before the server: `index_matches_checkout` | server refresh after start | watcher refresh after an online switch |
|---|---|---|---|
| this repo, macOS `-O2` | false | 16.2 s | 5.5 s |
| this repo, macOS sanitizer | false | 52.6 s | 5.5 s |
| redis, macOS `-O2` | false | 9.8 s | 5.5 s |
| redis, Linux `-O2` | false | 9.3 s | 5.3 s |
| django, Linux `-O2` | false | 11.6 s | 14.7 s |

Before the fix for bug 2, the SessionStart text before the refresh described
the previous branch's graph as current. After it, the text ends with, for
example: "Note: this index was built for branch `main` at 3230252, but the
checkout is now branch `pci` at 1f5db1a. Until the next index run (...),
graph answers describe the indexed code, not the files on disk." After the
refresh, the note is gone.

### Worktrees

In every run, each of the three worktrees indexed as its own project, and
`index_status` in each one matched its checkout. In an unindexed worktree,
SessionStart said "not indexed" with the linked-worktree hint, and
`index_status` without a project argument did not resolve to the parent.
Before the fix for bug 1, the parent's index held all 660 files of the
`nested-wt-*` worktree.

### Sanitizer

The sequence on this repository, plus the offline and worktree scenarios,
ran with the ASan and UBSan build (`build/int`, `-DCBM_SANITIZE=ON`). The
script scanned the CLI, hook and server stderr and the worker logs for
sanitizer reports. There were none.

## Bugs found and fixed

1. **A linked worktree nested in the checkout was indexed into the parent**
   (`75785c30`). `git worktree add ./wt` (any directory name except
   `.worktrees`) put the whole worktree into the parent's graph. Discovery now
   skips a directory whose `.git` gitlink points into `<common>/worktrees/`.
   Submodules and nested clones are walked as before. This was already true on
   `main`. Test: `discover_skips_nested_linked_worktree`.
2. **SessionStart described another branch's graph as current**
   (`38b99dfa`). After an offline checkout, the brief now ends with one
   sentence that names the indexed branch and commit and the checkout's.
   The server refreshes the index on its first poll, but the hook text is read
   before that, and on llvm a seeded refresh takes about 6 minutes. Test:
   `branch_session_start_notes_moved_checkout`.
3. **A recreated empty file made every restore run the incremental pass**
   (`9c1c1c81`). Extraction stores no digest for an empty
   file, so an empty `__init__.py` that only one branch has, which git deletes
   and recreates on each switch, was "changed" on every restore. On django
   each restore re-extracted 1 file, took 2.9 to 3.5 s instead of about 0.4 s,
   and added incremental drift to the restored graph. The same gap made every
   run whose graph came from another checkout (a seeded switch or a pull,
   where content is verified) re-extract all 618 empty files of django. Two
   empty files now compare equal. After the fix, django restores take 0.7 s,
   re-extract nothing and are exact, and its seeded switches re-extract 79
   to 100 files instead of 696 to 717 (5.4 s instead of 9.8 s). Test:
   `branch_restore_keeps_recreated_empty_file`.

Each test fails without its fix. The full suite passes with all three fixes.

## Measured costs

- **Restore** costs about 0.4 to 1.7 s for databases up to 600 MB, and 22 to
  28 s for the 6.9 GB llvm database. A plain incremental run of the same delta
  takes 3 to 41 s, and 6 to 10 minutes on llvm.
- **WAL during a restore** peaks at the size of the database (6.9 GB on llvm),
  because the restore is one write transaction on the live database. Other
  runs peak at about 66 MB, the auto-checkpoint size (about 400 MB on llvm).
  The WAL is checkpointed when the run ends.
- **Parked slots** take one database size each. The default keep of 8 means
  up to 8 more copies per checkout: about 55 GB for llvm.

## Pre-existing problems that the soak exposed (not fixed here)

- **An incremental run is often slower than a full index.** On llvm a pull of
  239 files took 392 s, and the full index took 183 s. On cpython, 26.9 s
  against 11.7 s. On this repository, 35 changed files took 19.8 s against
  3.6 s, because fewer than 51 changed files run sequentially
  (`MIN_FILES_FOR_PARALLEL_INCR`), and a few 5000 to 12000 line C++ files
  dominate. A seeded switch pays this cost. A restore does not.
- **Incremental against full drift** (described above). It is larger for C++
  and it accumulates over runs. A restore does not add drift, but it brings
  back the parked graph's drift.

## Not run

- Windows. No host was available.
- The Linux kernel. A full index needs about 61 GB of RSS.
- Real long-lived branches with large divergence. The shallow clones have
  only the default branch, so the "branches" are commits 5 to 60 apart.
- Concurrent writers during a restore (`manage_adr` has no index lease).

## Re-running

```sh
scripts/branch-index-soak.py --bin build/c/code-cortex-mcp --repo <scratch clone> \
    --out <new dir> --steps 'create:soak-old=<tip>~60,create:soak-feat=<tip>~10,switch:soak-main,move:<tip>,detach:<tip>~5,switch:soak-old,switch:soak-main,switch:soak-feat' \
    --offline soak-old --worktrees
```

The script moves the checkout at `--repo`. Use a scratch clone.
