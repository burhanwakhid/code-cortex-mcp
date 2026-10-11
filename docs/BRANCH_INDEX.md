# Per-checkout and per-branch index

This page records why the index is kept per git worktree and per git branch, and why it has its
present shape. It lists the alternatives that lost and the limits that remain. The code is in
`src/pipeline/branch_slots.{h,cpp}`, `src/pipeline/pipeline.cpp`,
`src/pipeline/pipeline_incremental.cpp`, `src/pipeline/artifact.cpp` and the project resolution
in `src/mcp/mcp.cpp` and the hooks.

## Problem

Code Cortex stores one SQLite graph per project at `<cache>/<project>.db`. The project name
comes from the checkout path. Before this design, three things went wrong with that rule:

- A git worktree or nested repository inside an indexed checkout had no index of
  its own. Project resolution climbed the parent directories and answered from the parent's
  graph, which describes other code and often another branch. Nothing warned the agent.
- A linked worktree added inside the checkout (`git worktree add ./wt`) was indexed into the
  parent's graph. On this repository the parent gained 660 duplicate files.
- A branch switch made the next incremental run overwrite the previous branch's graph. A
  switch back paid the whole difference again. On LLVM that is 6 to 10 minutes per switch.

The design has three requirements:

1. Every checkout directory answers from its own graph, or says that it has none.
2. A branch that was indexed before comes back without re-extraction.
3. Readers never change. Tool workers, the hook binary and `list_projects` keep opening
   `<cache>/<project>.db` and never see a torn, mixed or stale-WAL file.

## Worktree identity

A worktree is identified by its path, like any other checkout. Each linked worktree therefore
gets its own project and its own database, with no extra mechanism.

Project resolution walks up from the working directory and takes the first level that has an
index. The walk now stops at a git checkout boundary. When a level does not resolve and holds a
`.git` directory or a linked worktree's `.git` file, the walk ends there
(`cbm_git_checkout_root_kind` and `cbm_git_is_linked_worktree`: one `stat` and one small file
read, no git subprocess). SessionStart then names that directory as the `repo_path` to index,
and for a linked worktree it says that the main checkout's index is not used. A submodule's
`.git` file does not end the walk: discovery indexes the submodule's files into the enclosing
checkout's graph, so the enclosing checkout answers for them.

Derived names collide, because separators fold to `-`: `/x/a-b/repo` and `/x/a/b-repo` both
become `x-a-b-repo`. A derived-name hit is therefore accepted only from a database whose
`projects.root_path` is that directory (`cbm_root_matches`: lexical comparison after
normalization, then the canonical path). A legacy database without a recorded root is accepted.
The MCP server memoizes its working-directory hit only for the generation of the database file
that it checked (`cbm_file_generation`: size, mtime and inode). When that file is replaced, for
example when it expired to trash and a colliding directory was then indexed under the same name,
the next call walks up again and checks ownership against the new file.
An index run that finds a live database of another directory under its name rebuilds in full. It
carries nothing over from that database: no file hashes, no ADR (the architecture decision
record that `manage_adr` stores) and no slot.

Discovery skips a directory whose `.git` file points into `<common dir>/worktrees/<name>`
when that admin directory holds the `commondir` file git writes for every linked worktree,
because that directory is a linked worktree with its own index. Submodules (`.../modules/...`,
also one checked out at `worktrees/<name>`, whose git dir is `.git/modules/worktrees/<name>`)
and nested clones (a `.git` directory) are walked as before.

Alternatives that lost:

- **Key the index by repository instead of path** (for example the common git dir). All
  worktrees of a repository would share one graph. That is the defect itself: each worktree
  has its own branch and its own files.
- **Make project names collision-free** (hash the path into the name). That renames every
  existing database and every name that users and agents already use. The ownership check
  costs one row read on the handle that the lookup opens anyway.

Limitation: a derived-name collision still means that two checkouts share one live database
file. The second one to index rebuilds over the first. Their parked slots stay apart (see [Slot
layout](#slot-layout)).

## Branch identity

Each run writes the checkout it indexed on the graph's `Branch` node: the qualified name
`<project>.__branch__.<slug>` and properties with `branch`, `is_detached`, `head_sha` and
`is_git` (`cbm_pipeline_stamp_branch_store`). The node, its `HAS_BRANCH` edge and its full-text
entry are written in one transaction, and only when one of them differs. A `Branch` node that
has lost its `HAS_BRANCH` edge (a legacy or torn database) also counts as different: the stamp
recreates the edge from the `Project` node.

Two comparisons use that identity (`cbm_branch_id_same` and `cbm_branch_id_same_strict`):

| Comparison | Rule | Used by |
|---|---|---|
| Lenient | Same branch name (or slug when no name is recorded), same detached state, and the same commit when both sides know one. | Session start, which reads HEAD from files only. |
| Strict | As lenient, but a graph with no recorded commit does not match a checkout that has one. | The pipeline's content check, `index_status`, and session start when the commit is readable from files. |

Session start must not start a git subprocess. It reads `HEAD`, `commondir`, the loose ref and
`packed-refs` directly, and it walks up to the nearest `.git` as git does. When that read gives
no commit (a reftable repository, a symbolic branch ref), it compares the branch name alone.
That is why session start keeps the lenient rule there: the strict rule would mark every such
session pending. The pipeline resolves the checkout through git, which has a commit on every
born branch, so the strict rule never verifies a repository on every run.

The slug alone was not enough to decide whether a graph is current:

- Every detached HEAD has the slug `detached`. A moved detached HEAD looked current.
- Two branch names can slug alike (`a/b` and `a-b`).
- A pull keeps the branch name and moves the commit.
- A branch name can contain the marker `.__branch__.`. The slug is the text after the
  `<project>.__branch__.` prefix, never after the last marker.

The slug still names the parked file, because a file name must be short and safe. A slot shared
by two identities costs re-work, never a wrong answer: the incremental pass decides what to
re-extract from file hashes, not from the label.

Legacy databases: a `Branch` node with no properties gives the slug from its qualified name and
no commit. The strict rule marks such a graph pending once, and the run that follows writes the
full identity. A node with `is_git` false (git refused the checkout, for example through
`safe.directory`) counts as unknown, so it never mismatches.

## Park and restore

A run can start on another branch than the one that the live database records. The run then
parks the outgoing graph, and it restores the incoming graph if that branch was parked earlier.
Otherwise the outgoing graph stays in place as the seed, and the incremental pass pays only the
difference. This happens under the index lease (`cbm_pipeline_claim_index`), before routing, so
the incremental pass sees the restored or seeded graph (`pipeline_branch_switch`).

### Stable live file name

The live database keeps the name `<cache>/<project>.db` for every branch. Per-branch file names
(`<project>@<branch>.db`) would make every reader resolve the branch first: the hook binary,
tool workers, `list_projects`, the cached store of a server, the artifact export and the
watcher. A reader that resolved before a switch would also keep reading a file that is no longer
current. A stable name keeps all readers unchanged and puts the whole feature behind the index
lease.

### Restore by content

A restore copies the slot's content into the live database as one SQLite write transaction: the
backup API writes into `db_path`'s own connection (`restore_into`). The file, its WAL and its
shared memory stay in place:

- A reader mid-query keeps its snapshot.
- The next read transaction sees the whole restored graph.
- A crash before the commit leaves the outgoing graph intact.
- No file is renamed over a file that another process has open.

The first version of this feature did a `TRUNCATE` checkpoint and renamed the parked file over
the live one. A `TRUNCATE` checkpoint calls `ftruncate()` on the WAL under sibling processes. On
macOS that can raise `SIGBUS` in a process that has the database memory-mapped
(`cbm_store_checkpoint`). A rename also leaves a window in which the old WAL sits beside the new
main file, and SQLite replays that WAL onto the new file. After a crash, that state stays.

With no live database (first index, an interrupted rebuild, a manual delete) there is no reader
to protect. The slot is installed like a fresh database: stale sidecars removed, then one
rename.

### Durability order

A switch survives a power loss at any step. Each branch's graph is always in the live database or
in its slot:

1. The park syncs `<slot>.db.zst.tmp` to disk, renames it into place and syncs the slot
   directory (`compress_file`). Only then does the restore overwrite the outgoing graph.
2. The restore runs its connection at `synchronous = FULL`, so the WAL is synced at the commit.
   Write connections otherwise run at `NORMAL`, under which a WAL commit is durable only at the
   next checkpoint.
3. Only after the commit is the incoming slot removed.

With no live database, the decompressed copy is synced before it is renamed to `db_path`, and
the cache directory after. A legacy `.db` slot copied in is synced the same way. The slot is
removed after that.

### Slot layout

```text
<cache>/branches/<project>/<root key>/<slot>.db.zst
```

- `<root key>` is the first 12 hex digits of the SHA-256 of the canonical checkout root. Two
  checkouts with the same derived name never share slots, and every spelling of one directory
  gets the same key: separators, `.`/`..` and trailing separators are folded, symlinks resolved
  (`realpath`); on Windows the directory's final path (a junction or symlink resolved, the
  volume's own case) with Unicode case folding; when both directories exist, `cbm_root_matches`
  lets the volume serial + file index decide outright (a directory opted into per-directory case
  sensitivity makes `Repo` and `repo` two checkouts), and only a vanished one is compared by
  its folded spelling. A slot from the earlier flat layout (`branches/<project>/<slot>.db`) is
  moved into the root's directory when it records that root.
- `<slot>` is the slug when it fits in 200 bytes. A longer slug becomes a prefix plus `-` and
  16 hex digits of the SHA-256 of the whole slug. The file name thus stays one path component
  with room for every suffix (`.db.unz.tmp-journal`), and distinct slugs keep distinct files.
- The `branches/` subdirectory keeps parked files out of every cache scan, which reads only
  top-level `*.db` files.

### ADR carry under the write lock

The ADR is the one table that is not derived from source. `manage_adr` writes it for the
project, whatever branch is checked out, so the outgoing live database is the authority. The
first backup step of the restore takes the live database's write lock. The restore then reads
the live ADR on another connection and writes that state into the slot copy, and the rest of the
backup copies it. A concurrent `manage_adr` write either committed before the lock and is
carried, or waits on the lock and lands on top of the restored graph.

The carry has three states:

| Live ADR | Result |
|---|---|
| Present | The restored database gets this ADR. |
| Absent (deleted or never set) | The restored database gets no ADR, so a delete does not come back. |
| Unreadable | The restore is cancelled. The live database stays as the seed, because it can hold the only copy of the newest ADR. |

### Artifact freshness

The `.code-cortex/graph.db.zst` artifact (team sharing) must follow the live graph. Its metadata
records the graph's `Branch` node, a content fingerprint (the SHA-256 over the `file_hashes`
paths and digests, `cbm_store_content_fingerprint`) and the graph's generation: the index
format (`PRAGMA user_version`) and the `indexed_at` of the dump that wrote it. A run ends by
comparing them with the live database and exports when they differ. A failed export is
therefore repaired by any later run, a count-preserving edit is not missed, and neither is a
rebuild from unchanged sources after a format or extractor upgrade. Metadata without these
fields is never current, so one export brings an older artifact up to date. An imported
artifact records the importing checkout as its root, so the first run treats it as this
checkout's graph and compares the exporter's identity with the checkout. The artifact directory
is never indexed, and a `.cbmignore` negation cannot change that: an indexed `artifact.json`
would change on every export and start another.

### Content verification after an identity change

The incremental pass normally trusts a stored row when size and mtime match. After a switch, a
failed restore, two branches that share a slot, or a pull, the stored graph comes from another
checkout. There a file can change under the same size and mtime: timestamps have whole seconds,
and `git checkout` can write two branches in the same second. When the identity that the
database records differs from the checkout under the strict rule, that run settles same-size
files by content (`pipeline_decide_content_check`, `classify_files`). Runs on an unchanged
checkout keep the fast path.

Two cases needed their own rule:

- **Mode-skipped files.** A stored file that the current mode does not discover (for example
  `docs/` in fast mode) cannot be re-extracted. It is kept only when its bytes still match,
  and purged otherwise.
- **Empty files.** Extraction stores no digest for an empty file. Git deletes and recreates an
  empty `__init__.py` that only one branch has. Every restore on django therefore re-extracted
  one file and took 2.9 to 3.5 s instead of 0.7 s. Two empty files now compare equal.

The same rule makes a restore fast. Git rewrites every file that differs between the branches,
so those files get new mtimes over the bytes that the parked graph was built from. Hashing them
is a read; re-extracting them is a parse. A restore re-extracted 0 files in every soak step.

## Storage of parked slots

### Compressed full slots

A parked slot is a zstd-compressed snapshot of the whole live database, with its indexes,
full-text tables and vectors (`park_db`):

1. A backup-API copy of the live database into the scratch file `<slot>.db.unz`. The copy is
   one read transaction: it does not block readers and needs no checkpoint.
2. Stream compression (zstd level 3, one frame with a content checksum) into
   `<slot>.db.zst.tmp`, then a rename to `<slot>.db.zst`.
3. The scratch file is removed. A legacy uncompressed `<slot>.db` of the same branch is
   superseded and removed.

A restore decompresses the slot into `<slot>.db.unz` in the slot directory, carries the ADR into
that copy, and installs it by content as before. A slot that does not decompress (truncated,
corrupt, failed checksum) is not restored: the run seeds instead. The next switch removes
scratch files that a crashed park or restore left behind (`sweep_scratch`). Scratch files are
matched by their exact suffix (`.db.unz`, `.db.unz.tmp` and their SQLite sidecars, and
`.db.zst.tmp`), never by a substring: a branch named `fix.db.unz` has the slot
`fix.db.unz.db.zst`, which the sweep keeps.

Level 3 is the fast level. On two index databases (zstd 1.5.7, one thread, macOS, measured) it
gave 9.0x on redis (216 MB) and 5.9x on opencv (462 MB) at about 600 MB/s. Level 9 gave 10.0x
and 6.7x at one fifth of the speed. Parking sits on the branch-switch path, so the fast level
won. The compressor asks for 4 worker threads, which takes effect only in a zstd build with
multithreading; the vendored build has none today.

Uncompressed slots that earlier builds of this feature wrote (`<slot>.db`) still restore as they
are. They are listed, counted, evicted and deleted like compressed ones. They are not converted
in place; the next park of that branch writes a `.db.zst` and removes the `.db`.

Costs that remain:

- A park writes an uncompressed scratch copy first, so it needs the database's size in free
  space for a moment, plus the compressed slot.
- A restore decompresses before it installs, so it needs the same scratch space.

### Size budget

Slots are capped per checkout by count and by total size (`evict_slots`). The least recently
parked slot goes first, until at most `branch_index_keep` slots remain (default 8) and their
files total at most `branch_index_budget` bytes (default 2 GiB). The slot that was just parked
is never evicted, so a slot larger than the whole budget still serves one switch back. A budget
of 0 turns the size cap off.

At the measured ratios (4.5x to 6.4x), the 2 GiB default keeps all 8 slots for an index up to
about 1.2 GB. It keeps one slot for LLVM, whose 6.9 to 7.0 GB index compresses to 1.09 to
1.10 GB. Before this change, LLVM could take 8 x 6.9 GB, about 55 GB, per
checkout.

### WAL during a restore

A restore commits the whole database through the WAL, so the WAL grows to the database's size
(6.9 GB on LLVM). Three ways to bound it were examined:

| Candidate | Result |
|---|---|
| Backup in page batches (`sqlite3_backup_step(n)`) | Does not bound the WAL. The destination keeps one write transaction and its lock from the first step to the last. Measured (macOS, Python `sqlite3` module, SQLite 3.53.1): 33 steps of 100 pages into a 216 MB database grew the WAL steadily to 216 MB. |
| `ATTACH` the snapshot and copy table by table in several transactions | Bounds the WAL, but breaks the guarantees: a reader between the transactions sees a mix of two branches' graphs, and a crash leaves that mix as the live database. Indexes and the FTS shadow tables would also need a rebuild or a page-level copy. Rejected. |
| One transaction, then a `PASSIVE` checkpoint and a WAL reset | Does not lower the peak, but gives the space back. Chosen. |

The peak therefore stays a known cost. What changed is the residue. SQLite removes the WAL when
the last connection closes. A process that keeps the database open (a server's cached store)
used to hold a database-sized WAL until it closed. Measured on rocksdb: 266 MB stayed on disk
after every restore while a reader was open. After the commit, the restore now runs a `PASSIVE`
checkpoint, which copies the frames into the database file and never waits for or blocks a
reader. If that checkpoint copied every frame, the restore commits one write that rewrites the
unchanged `user_version` with `journal_size_limit = 0`. SQLite then restarts the WAL at its
start and truncates the file to what that commit wrote (`shrink_wal`).

SQLite restarts the WAL only when no reader uses its frames. A reader mid-query keeps its
snapshot, and the space then comes back later. This is not a `TRUNCATE` or `RESTART` checkpoint:
nothing waits on readers. Truncating the `-wal` is safe for other processes: SQLite reads it with
`pread` and never memory-maps it, and it truncates it only under the WAL read locks that exclude
every reader still using the removed part. A separate investigation confirmed this in the vendored
SQLite 3.51.3 sources and in 100-iteration multi-process runs on Linux and macOS: 0 `SIGBUS`,
0 errors.

The restore's own shrink is opportunistic. With readers that loop over transactions it found the
WAL free in only 0 to 3% of restores on Linux and 5 to 15% on macOS (measured), because the reset
needs a moment with no reader mark, and `journal_size_limit = 0` exists only on the restore's
short-lived connection. The backstop is a 256 MiB `journal_size_limit` on every write connection
(upstream #1083, `CBM_SQLITE_JOURNAL_SIZE_LIMIT`): the next write after a full checkpoint, on any
connection, cuts the `-wal` back to 256 MiB. A WAL left at the database's size therefore shrinks
to at most 256 MiB at the next write that finds the log free, and to nothing at the last close.

### Alternatives for slot storage

**(a) Strip indexes, full-text tables and vectors from the slot, and rebuild them on restore.**
This is what the artifact export does at its best quality. One analysis of an opencv database
gave these shares of the file:

- tables that a restore cannot recompute (nodes, edges, file hashes, projects, summaries,
  index coverage): about 35%,
- edge and node indexes: about 45%,
- vectors: about 15%,
- the full-text index: about 3%.

A stripped slot would keep about a third of the bytes before compression. After compression it
would be about 10x smaller than the live database (estimated), against the 4.5x to 6.4x that
compressed full slots reach (measured). The restore would then rebuild every index of the graph.
On a large graph the index rebuild dominates, so a restore would cost a large part of a full
index instead of a copy (estimated, not measured). We chose to keep restore time close to the
uncompressed design, so this stays a possible follow-up for repositories where disk matters more
than switch time.

**(b) The default branch as a base, plus a logical delta per branch keyed by qualified name.** A
delta is tiny on disk. It loses when the base moves: after a pull on the default branch, every
delta needs a rebase or a fall-back to seeding. Materializing a branch still pays the index
rebuild. And several passes are whole-graph, not per file (cross-file call resolution,
`SEMANTICALLY_RELATED`, `SIMILAR_TO`, git-history edges), so a per-file delta does not describe
them.

**(c) Base plus delta at query time, through views.** Every query of every tool would read
through the overlay, including the hot paths that the hooks run at prompt time. Rejected as too
slow and too invasive; not measured.

**(d) Page-level deduplication between slots.** An incremental run ends with a re-dump, which
renumbers rowids and lays out pages again. Two branches with nearly the same graph therefore
share few identical pages. Rejected.

## Measured numbers

### Soak, before compression

The 2026-10-06 soak (`scripts/branch-index-soak.py`, results in the gitignored
`docs/benchmarks/2026-10-06-branch-index-soak/`) measured the uncompressed design on six
repositories. Wall times include the supervised worker's start and reap. All values are
measured.

| Repository (files), host | Full index s | DB MB | Restore s | Same delta, plain incremental s | Restore WAL peak MB |
|---|---|---|---|---|---|
| this repository (621), macOS | 3.5-4.0 | 98-105 | 0.5-0.6 | 2.0-15.9 | 59-105 |
| redis (1753), macOS | 3.3-4.4 | 216-218 | 0.8-1.1 | 2.5-8.9 | 216-218 |
| redis (1753), sim3 | 2.2 | 116 | 0.4-1.4 | 3.4-6.9 | 115-116 |
| rocksdb (2072), sim3 | 5.9 | 261 | 0.8-1.0 | 7.6-12.5 | 255-261 |
| django (4791), sim3 | 3.4 | 210 | 0.7 | 4.1-6.7 | 210 |
| cpython (5655), sim3 | 12.1 | 586 | 1.7 | 18.0-40.9 | 585-587 |
| llvm (159199), sim3 | 182-192 | 6892 | 22.0-28.1 | 392-574 | 6894-6897 |

Every restore brought back exactly the graph that was parked (ground-truth diff equal item for
item). A seeded switch costs what a plain incremental run of the same delta costs. On LLVM a
seeded switch of 256 files took 383 s, longer than a 183 s full index; see [Known
limitations](#known-limitations).

### Compressed slots

Measured on 2026-10-08 with `scripts/branch-index-soak.py --measure --hold-reader`, `-O2` builds
of the parent commit (before) and of this change (after), on the same clones and the same branch
sequence: a new branch (seeded), back (restored), across again (restored), a third branch
(seeded), back (restored). LLVM had no third branch and one more restore instead, and no
ground-truth check. Hosts: sim2, Linux, 32 cores, 62 GB, otherwise idle. Park and restore times
come from the `branch.park` and `branch.restore` log lines and cover only that step. "Run" is
the whole `index_repository` wall time of a restoring run, in process. Every restore of the
repositories other than LLVM was exact against a fresh full index (ground-truth check), before
and after. Values are ranges over the steps.

| Repository, host | DB MB | Slot MB before / after (ratio) | Park s before / after | Restore s before / after | Restoring run s before / after | WAL left under an open reader, MB before / after |
|---|---|---|---|---|---|---|
| this repository, sim2 | 62-66 | 62-66 / 13.7-14.6 (4.5x) | 0.04-0.05 / 0.10-0.11 | 0.06-0.07 / 0.11-0.13 | 0.20-1.04 / 0.30-0.34 | 65 / 4-6 |
| redis, sim2 | 118 | 118 / 23.4-23.6 (5.0x) | 0.08-0.09 / 0.16-0.19 | 0.11-0.12 / 0.21-1.16 | 0.40-1.39 / 0.59-2.52 | 118 / 9 |
| django, sim2 | 214 | 214 / 38.0-38.1 (5.6x) | 0.13-0.14 / 0.25-0.27 | 0.20-0.21 / 0.37-0.38 | 0.60-0.66 / 0.91-0.93 | 214 / 8 |
| rocksdb, sim2 | 259-266 | 259-266 / 48.3-49.4 (5.4x) | 0.16 / 0.30-0.33 | 0.24-0.28 / 0.44-0.45 | 0.76-0.81 / 1.11-1.14 | 266 / 19-21 |
| cpython, sim2 | 595-597 | 595-597 / 110.0-110.3 (5.4x) | 0.34-0.36 / 0.63-0.82 | 0.55-0.60 / 0.99-1.88 | 1.64-2.21 / 2.54-3.34 | 597 / 12-15 |
| llvm, sim2 | 7028-7033 | 7028-7033 / 1094.9-1095.6 (6.4x) | 3.36-3.43 / 6.24-8.77 | 5.9-7.3 / 10.2-10.5 | 16.6-18.2 / 23.9-26.5 | 7036 / 5-6 |

On LLVM the decompression took 3.9 to 4.0 s of each restore. The same LLVM sequence on sim3, a
host that other jobs loaded at the time (load average 9 to 11), gave slots of 1090 MB and much
wider ranges: park 3.4 to 11.6 s before and 6.3 to 25.2 s after, restore 6.6 to 44.3 s before
and 10.6 to 76.3 s after. The sim2 rows are the clean measurement.

The WAL peak during a restore did not change: it equals the database size in every row, before
and after. The WAL left behind holds the writes that the rest of the run made after the restore,
not the restore's frames.

A restore now costs about twice the uncompressed one. The decompression writes a scratch copy of
the database. The restore's own checkpoint also copies the frames into the database file, a cost
that the run-end checkpoint paid before. On repositories up to 600 MB the restoring run stays
between 0.3 and 3.3 s, against 1.6 to 47 s for the seeded runs of the same sequences.

## Known limitations

- **Windows is not verified.** No Windows host ran the soak. Root normalization and the slot
  key are tested as pure string functions on every platform.
- **Reftable HEAD is not readable at session start.** Session start reads HEAD from files
  only. With a reftable repository it gets no commit and compares the branch name alone. A
  pull on the same branch is then not noticed until the next index run.
- **`manage_adr` has no index lease.** It takes only SQLite's write lock. The restore carries
  the ADR under that lock, but no other part of an index run excludes a concurrent ADR write.
  Concurrent writers during a restore were not soaked.
- **Git-history data is not recomputed incrementally.** `FILE_CHANGES_WITH` edges keep the
  state of the last full run, on every branch.
- **Incremental runs drift from a full index**, mostly `CALLS` and `USAGE` edges, because
  incremental runs skip cross-file LSP resolution: about 4000 to 5000 `CALLS` edges on rocksdb
  and LLVM, about 1500 on cpython. A restore does not add drift, but it brings back the parked
  graph's drift.
- **A small incremental run can be slower than a full index.** Fewer than 51 changed files run
  on one thread (`MIN_FILES_FOR_PARALLEL_INCR`). On LLVM a 239-file pull took 392 s against a
  183 s full index; on this repository 35 changed files took 19.8 s against 3.6 s. A seeded
  switch pays this; a restore does not.
- **The restore's WAL peak equals the database size** (see [WAL during a restore](#wal-during-a-restore)).
- **Park and restore need scratch space** equal to the database size, in the cache directory.
- **Every detached HEAD shares one slot** (`detached`), and branch names that slug alike share
  one slot. Both cost re-work only.
- **Branch names that differ only in case share one slot file on a case-insensitive
  filesystem** (macOS and Windows defaults): `Feature` and `feature` both name
  `Feature.db.zst`. A switch between them parks the outgoing graph over the incoming slot. The
  switch detects that the incoming slot is now the same file as the one it just parked
  (`same_file`: device and inode, or a case-insensitive name compare on Windows), skips the
  restore and seeds. The incoming branch's earlier park is lost; that costs re-work only.
- **The SessionStart text is read before the server's refresh.** After a switch made while no
  server ran, the brief names the indexed and the checked-out branch, and graph answers
  describe the indexed branch until the refresh finishes (about 6 minutes for a seeded LLVM
  switch).

## Operations

| Setting | Default | Effect |
|---|---|---|
| `branch_index` (config) | `true` | `false` turns parking off: a switch reindexes the one database in place. |
| `branch_index_keep` (config) | `8` | Parked slots kept per checkout. |
| `branch_index_budget` (config) | `2G` | Total bytes of a checkout's slots. Bytes with an optional `K`, `M`, `G` or `T` suffix (powers of 1024). `0` turns the size cap off. |
| `CBM_BRANCH_INDEX=0` (environment) | unset | Turns parking off for one process; wins over the config. |
| `CBM_BRANCH_INDEX_BUDGET` (environment) | unset | Size budget for one process; wins over the config. |

Set the config keys with `code-cortex-mcp config set <key> <value>`.

`index_status` reports these fields in its `git` block:

| Field | Meaning |
|---|---|
| `indexed_branch` | The branch the graph was built for. |
| `index_matches_checkout` | The strict comparison of the graph's identity with the checkout. `false` until the next index run after a checkout. |
| `parked_branches` | The slot names of this checkout. |
| `parked_slots` | Each slot file: `branch`, `bytes`, `compressed` (`false` for a legacy `.db`). |
| `parked_bytes` | The total size of this checkout's slot files. |
| `branch_index_keep`, `branch_index_budget_bytes` | The limits that the next park applies. |

`index_repository` reports a `branch` block with `switch` (`none`, `restored`, `seeded` or
`in_place`), `previous`, `indexed` and `files_reparsed`. With `CBM_INDEX_SUPERVISOR=0` the run
logs `branch.park` (`elapsed_ms`, `slot_bytes`, `db_bytes`) and `branch.restore` (`elapsed_ms`,
`unpack_ms`, `wal_bytes`) to standard error.

To remove parked slots, use `delete_project` (the project's slots of every root go with it) or
the index reset of `install`, `uninstall` and `update`, which also clears slots whose
`<project>.db` is gone. `delete_project` and the reset keep a project's slots when its live
database cannot be removed. The watcher's prune of a deleted checkout removes its slots too.
