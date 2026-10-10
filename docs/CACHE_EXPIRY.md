# Cache expiry

This page records why and how Code Cortex removes index databases that nobody uses any more,
and which alternatives lost. The code is in `src/mcp/cache_expiry.{h,cpp}`, with the slot part
in `cbm_branch_slots_expire` (`src/pipeline/branch_slots.cpp`) and the command in
`code-cortex-mcp cache` (`src/cli/cli.cpp`).

## Problem

The cache directory only grew. Three kinds of files stayed forever:

- `<cache>/<project>.db` of a checkout that no longer exists. A `git worktree remove`, a deleted
  scratch clone or a benchmark directory under `/tmp` leaves its index behind, and nothing points
  at it again. One developer cache held more than 500 databases, and every scan of the cache
  directory (the `list_projects` fallback scan, the memo refill) paid for each of them.
- Parked branch slots (see [Branch index](BRANCH_INDEX.md)). They are bounded per checkout by
  count and size, but a branch deleted in git keeps its slot until the cap pushes it out, and the
  slots of a project whose database is gone are never pushed out at all.
- SQLite sidecars and temp files of a database that is gone, and scratch files of an
  interrupted park or restore.

The design has these requirements:

1. Remove only what is very likely dead, and say why.
2. Never remove the index of a project that a session is in, that is being indexed or watched,
   or that a reader is using.
3. Cost nothing measurable on the hot paths: tool calls and the hook processes.
4. Make a mistake recoverable for some time, and make every decision visible before it happens.

## What counts as stale

The rules, in order of confidence:

| Kind | Rule | Default |
|---|---|---|
| `root_gone` | The recorded checkout root is missing on at least 3 sweeps, the first of them at least the grace period ago, and nothing used the database in the grace period. | 7 days |
| `unused` | No tool call, hook or index run used the database for the threshold. | 90 days |
| `slot` | A parked slot was parked longer ago than the threshold. | 30 days |
| `sidecar` | A `-wal`, `-shm`, `-journal`, `.db.tmp` or `.db.import_tmp` file whose database is gone, or a scratch file (`.db.unz*`, `.db.zst.tmp`) beside the slots. | older than 1 hour |

### Root gone

The root comes from the database's own `projects` row, read once and then kept in the usage
table. An index run records it exactly, from the checkout it indexed.

A missing path is not always a deleted checkout. The sweep classifies each root as present,
missing or uncertain (`root_state`):

- **Present**: the directory exists. Any streak restarts.
- **Missing**: `stat` says "no such file", and the nearest existing ancestor is an ordinary
  directory. This is what `git worktree remove` or deleting a clone looks like. The probe is
  `cbm_stat`, which on Windows takes the wide-character path: the narrow `stat` there cannot
  name a path with characters outside the ANSI code page or past `MAX_PATH`, and would call
  such a live checkout missing.
- **Uncertain**: any other error (permission, a network mount timing out), a relative path, or a
  nearest existing ancestor that is `/` or a mount parent (`/Volumes`, `/mnt`, `/media`,
  `/media/<user>`, `/run/media/<user>`, `/net`, `/Network`, `/System/Volumes`, automount
  directories, a Windows drive root). An external disk that is not mounted looks like this. An
  uncertain root never counts.

A root must be missing on 3 sweeps, and the first of those must be at least 7 days old. Sweeps
run at most once a day, so the 3 sweeps are at least 2 days apart, and a single bad moment (a
remount, a checkout that is moved and moved back) does not count 3 times. The database must also
be unused for the same 7 days: a project that someone queries by name after its checkout moved
stays.

Two guards protect against a cache that is judged from the wrong place:

- **Environment change.** The sweep records HOME and the cache directory. When either differs
  from the previous sweep (a copied cache, a moved home, a sweep from another account), no root
  is judged on that sweep, and every streak restarts.
- **Mass absence.** When 5 or more roots are judged and every one is missing, the sweep counts
  nothing (`cache.expire.suspect` in the log). A cache whose checkouts all vanished at once is
  more likely a renamed home directory than a cleanup.

A root recorded by another machine (an artifact imported before its root was rebound) is missing
on this machine. It expires like a deleted checkout, unless it is used within the grace period.
`cbm_artifact_import` rebinds the root at import, so this is rare.

### Unused

`last_used` is recorded in table `cache_usage` of `<cache>/_config.db`, beside the
`store_meta` memo. Three events record it:

- `resolve_store`, which every tool call and every hook (SessionStart, the prompt and read hooks)
  goes through to open a project's database. It records use on the file it opened, which for a
  renamed or legacy database that the fallback scan finds is not `<project>.db`;
- every index run (`index_repository`, the watcher's in-process run), which also records the
  root and clears a missing streak. The run records it before it releases the index lease
  (`cbm_pipeline_set_on_indexed`), so a sweep that judged the database stale before the run and
  takes the lease right after it re-reads the row, finds the run's use, and keeps the database;
- `code-cortex-mcp cache restore`.

The write is throttled twice. A process remembers, per database, when it last recorded use and
skips the next hour of calls with one map lookup. The first call of a process (each hook is its
own process) reads the row from `_config.db` and writes only when the row is an hour old. A read
never takes `_config.db`'s write lock, so a hook pays one SQLite open and one indexed read, once
per database it opens. The in-process throttle starts only after the row is current: when the
write fails because `_config.db` is busy (a sweep holds its transaction), the next call tries
again.

A database seen for the first time starts its clock at that sweep. Its file mtime is not used:
the mtime is the last index run, and a database queried daily but indexed long ago would then
expire on the first sweep after an upgrade. With the clock starting at first sight, nothing
expires as unused before it has had 90 days to be used under a build that records use.

### Parked slots

A restore deletes the slot it restores, so a slot that exists has not been restored since it
was parked, and its file mtime is the park time. A slot older than 30 days goes. This is how the
slot of a branch deleted in git goes away. Git branches are not enumerated: a deleted branch can
be recreated, a remote branch can be checked out again later, and a time rule needs no git
subprocess. Slots of a project whose database is already gone go by the same rule.

### Sidecars

The sweep removes a sidecar or temp file only when its database is absent, when it is older than
1 hour, and while it holds that database's lease, so an index run that is about to create the
database cannot race it. `<x>.db.index.lock` and `<x>.db.read.lock` files stay: the lease
contract (`compat_fs.h`) says that a lock file is never unlinked, because a process that opens
the path after an unlink locks a new file and both processes would own the lock. The lock files
are empty.

## Where the sweep runs

A sweep is due when the last one recorded in `_config.db` is at least a day old. The MCP server
checks this at `initialize` and after each `index_repository` call, and starts the sweep on a
background thread, so neither the handshake nor the index reply waits for it. The check is one
read of `_config.db`. One sweep thread runs per server. The sweep reads the watcher, which the
server does not own, so the server joins the sweep before the watcher changes
(`cbm_mcp_server_set_watcher`) and in `cbm_mcp_server_join_threads`. The stdio server calls
`cbm_mcp_server_join_threads` before it frees the watcher and the config. Across processes, a sweep holds the index lease of `_config.db` (a lease nothing else
takes), so a second server skips its sweep.

The tool workers and hook processes never sweep. The CLI command `code-cortex-mcp cache expire`
runs a sweep on demand, whatever the interval.

A sweep is one scan of the cache directory, one usage-table read per database (in one
transaction) and a `stat` of each recorded root. It opens a project database only to learn its
project name or a root the usage table does not have yet (first from the `store_meta` memo, else
one read of its `projects` row) and to quiesce a database it is about to remove.

A file is judged as the project it holds, not as its filename. A renamed or legacy file
`<cache>/<other>.db` that holds project `X` is served as `X` by the fallback scan, so the
session, watch and slot checks use `X`, and its slots are those under `branches/X/`. Reports
name it `X (<other>.db)`. The filename does not have to be a valid project name (`old
project.db` is judged as the `X` it holds); the project name does, because it names the trash
entry and the slot directory. When two files hold the same project, both keep their filenames,
and a file whose filename is not a valid project name is then not judged.

## Removal

A database that expires is removed under its index lease, so it is never removed while it is
being indexed. Before the move, the sweep opens it and runs a `PASSIVE` checkpoint. When the
checkpoint cannot copy every WAL frame, a reader holds a snapshot: the database is in use, and
the sweep skips it. Otherwise the main file is the whole database.

A renamed file shares its slots, under `branches/X/`, with the project's own `<cache>/X.db`,
which an index run of `X` writes under that file's lease. The sweep therefore also takes the
lease of `X.db`, without waiting. It skips the renamed file as `its project being indexed`
when another process holds that lease, and as `its project's own database appeared` when
`X.db` exists by then (the next scan files the renamed one under its filename).

The lease keeps index runs out, not readers. A reader that keeps a connection open does not
need a transaction to depend on the file: on POSIX a move leaves the connection on the moved
inode, so a server that cached the project's store would read the trash, and its next open
would find no project. Readers therefore hold the database's **reader gate**, an advisory lock
on `<x>.db.read.lock` beside the lease's `<x>.db.index.lock`:

- `resolve_store`, the path of every tool call and hook, opens the project's database with
  `cbm_store_open_path_reader`, which takes the gate shared before the SQLite open and keeps
  it until `cbm_store_close`. A store the fallback scan adopts takes it after the read that
  identified the file, and is dropped when the file has left its path by then. When the gate
  is still held exclusive after the wait, the open fails: the reader gets no connection, and
  the lookup reports `project lookup incomplete`, never a connection that the move could
  leave on the trashed file. That is one `flock` (or `LockFileEx`) per store open, nothing
  per query.
- The sweep takes the gate exclusive, without waiting, after the lease and before the
  checkpoint. When a reader holds it, the database is skipped as `in use by a reader`. While
  the sweep holds it, a reader that opens waits (up to 2 seconds, polling) and then finds the
  file gone, which is what a removed project looks like.

The gate is its own file, not a lock on `<x>.db`: SQLite's POSIX locks on the database are per
process, so closing any other descriptor of that file would drop them, and on some systems
(the BSDs) a `flock` and SQLite's `fcntl` locks on one file interact. Lock order is lease, then gate; every lease and
every exclusive gate attempt is non-blocking, and only a shared gate waits, for a bounded time,
so no cycle can form. A reader that cannot open the gate's lock file at all (a read-only cache
directory without it) opens without the gate, as before the gate existed: a sweep cannot take
that gate either, so it moves nothing. Short-lived reads that close at once (the ownership and
branch-identity checks) are not gated: a move cannot strand them. On Windows the gate works the same way; there a move of an
open database also fails with a sharing violation, which the sweep reports as `could not be
moved`.

Under the gate, the sweep reads the usage row again and skips the database when its `last_used`
moved forward (or its missing-root streak went back) since the scan: a reader that opened,
recorded its use and closed between the scan and the gate. When it cannot read the row, it also
skips the database: the next sweep decides again.

The removal is `cbm_cache_remove_project`, which `delete_project` also uses: the database, its
WAL and SHM, its parked slots of every root, its `store_meta` row and its usage row go together.
The one difference is the destination.

### Trash

An expired database is moved, not deleted, into `<cache>/trash/<epoch>-<project>/` with its WAL
and its slot directory, beside a file `EXPIRED` that records the reason. A sweep at least a day
later deletes the entry. Until then, `code-cortex-mcp cache restore <project>` puts the
database and its slots back, if the project has no new database, and restarts its usage clock.
A renamed file comes back as `<project>.db`, the name it is served under.

A restore moves the database, its WAL and its slots back one by one, under the project's index
lease. A purge takes the same lease, without waiting, so it never deletes an entry in the
middle of a restore. When the lease is held (a restore, or an index run of the project), the
purge skips the entry as `its project being restored or indexed`, and the next sweep purges it.
A restore that finds the lease held by a purge reports `being indexed or purged; retry later`.

We chose a trash step because the two costly mistakes are an uncertain root that the guards did
not catch (an unusual mount layout) and a threshold set too low by hand. A trash entry costs disk
space for one more day. Slots and sidecars are deleted directly: they are caches of something
that still exists (the project's graph) or of nothing.

`trash/` is a subdirectory, so scans of top-level `*.db` files never see trashed databases.

Sizing and deleting a trash entry never follows a link. A symlink (POSIX), or any reparse point
on Windows (a directory junction, a directory or file symlink), is removed itself, and the walk
does not descend into it: on Windows the directory listing reports a junction as a directory,
and following one would delete files outside the cache, or loop on a cyclic junction. `cache
trash` and `cache restore` do not list an entry that is itself a link.

### Never removed

- The project of the server's own session, and every project its watcher watches.
- A project whose index lease is held (being indexed or deleted elsewhere), also when the
  database is a renamed file and the lease is that of the project's own `<project>.db`.
- A database that a reader holds open (its reader gate), or with a reader holding a WAL
  snapshot.
- A database used after the sweep read the usage rows.
- A trash entry whose project's index lease is held (a restore or an index run in progress).
- Anything when the cache directory has no `_config.db`, when HOME is not a directory, or when
  another sweep runs.
- `_config.db`, `.index.lock` and `.read.lock` files, and files whose names this module does
  not write.

## Alternatives that lost

- **File atime as the last-used time.** Most systems mount with `noatime` or `relatime`, and
  every scan and backup touches it. It says nothing reliable.
- **A `last_used` column in `store_meta_v2`.** That table is a cache keyed by the file's
  generation: a re-index replaces the row, and a stale row is dropped. Usage must survive both,
  so it has its own table.
- **Writing `last_used` on every tool call.** That is a `_config.db` write lock per call, shared
  by every hook process. The two-level throttle makes the cost a map lookup in the steady state.
- **Using `projects.indexed_at` or the file mtime.** Both record the last index, not the last
  use. A stable repository is indexed rarely and queried daily.
- **Deleting immediately when the root is missing.** A disk that is not mounted, a network mount,
  or a checkout moved and moved back would lose its index at once. The watcher already prunes a
  watched root that stays missing for 10 minutes; that is a watched project, with a server
  observing it the whole time. The sweep judges unwatched projects, so it waits days.
- **Enumerating git branches to expire slots.** A deleted branch can come back, and it needs a
  git subprocess per checkout. Age is enough.
- **A global size cap for the whole cache.** It would evict the least recently used project of
  a user who simply has many, which is a policy decision the user did not make. The per-checkout
  slot budget and the time rules bound growth without it.

## Operations

| Setting | Default | Effect |
|---|---|---|
| `cache_expiry` (config) | `true` | `false` turns the automatic daily sweep off. `cache expire` still runs on demand. |
| `cache_expiry_unused_days` (config) | `90` | Days without use before a database expires. `0` turns the rule off. |
| `cache_expiry_root_grace_days` (config) | `7` | Grace period of the missing-root rule. `0` turns the rule off. |
| `cache_expiry_slot_days` (config) | `30` | Days after which a parked slot goes. `0` turns the rule off. |
| `CBM_CACHE_EXPIRY=0` (environment) | unset | No automatic sweep in this process. |

Fixed values: 3 missing-root sweeps, a sweep interval of 1 day, trash kept for at least 1 day,
and sidecars and scratch files older than 1 hour.

Commands:

```sh
code-cortex-mcp cache expire --dry-run   # what a sweep would do now, with reasons and sizes
code-cortex-mcp cache expire             # sweep now
code-cortex-mcp cache trash              # list trashed databases
code-cortex-mcp cache restore <project>  # put the newest trashed database of <project> back
```

`--dry-run` decides with the same rules and writes nothing: no removal, no usage rows, no
missing streaks, no sweep time. It also lists the roots that are missing but still in their
grace period (`watch`). `--now EPOCH` judges as of another time, for tests and for previewing.

Each removal logs one `cache.expire` line (kind, name, path, reason), each skip a
`cache.expire.skip` line, and each sweep a `cache.expire.done` summary.

## Known limitations

- A long-running server sweeps only at `initialize` and after index runs; a server that does
  neither for days does not sweep. Another session or `cache expire` does.
- A project whose server stays up and idle for 90 days, with nothing queried and nothing
  re-indexed, is excluded only from that server's own sweep (as its session or watched
  project). A sweep from another process can expire it as unused.
- The watcher's own prune of a watched root that stays missing for 10 minutes (`watcher.cpp`)
  still deletes directly, not through the trash, and leaves the database's usage row behind (a
  few bytes in `_config.db`).
- Windows is not verified. Root classification treats a drive root as a mount parent.
