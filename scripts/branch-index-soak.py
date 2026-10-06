#!/usr/bin/env python3
"""Soak the per-branch index (pipeline/branch_slots.h) on a real git checkout.

For a sequence of checkouts of one repository, each step:

  1. moves the checkout (git checkout / reset --hard / checkout --detach),
  2. runs index_repository in the "on" cache (branch index enabled) and in
     the "off" cache (CBM_BRANCH_INDEX=0, the plain incremental path),
  3. full-indexes the same checkout in a fresh ground-truth cache,
  4. diffs both incremental graphs against the ground truth: per-label node
     and per-type edge counts, the (label, qualified_name) node set, the
     (type, from_qn, to_qn) edge set, node locations, and the file_hashes
     table (a stale hash means a file the run did not re-read),
  5. records the branch block, wall time, DB / WAL / parked-slot sizes,
     index_status's branch facts, list_projects and the SessionStart text.

A difference found in BOTH the on and the off graph is incremental drift the
switch logic did not cause; a difference only in the on graph is a suspect
for the switch logic and must be explained.

Scenarios beyond the step sequence:
  --offline BRANCH   check out BRANCH with no server running, then start the
                     stdio server in the repo and wait for its watcher to
                     refresh the index (session-start pending check), then
                     switch back while the server runs (watcher HEAD poll).
  --worktrees        add a linked worktree inside the repo (.worktrees/soak
                     and nested-wt/soak) and a sibling outside it, check that
                     the hooks there do not answer from the parent's index,
                     and index each one independently.

The checkout at --repo is MOVED by this script. Use a scratch clone.
Every cache is a fresh directory under --out; nothing is deleted.

Steps (comma-separated):
  switch:<branch>          git checkout <branch>
  create:<branch>=<rev>    git checkout -B <branch> <rev>
  move:<rev>               git reset --hard <rev> (same branch moves: a pull)
  detach:<rev>             git checkout --detach <rev>

Example:
  scripts/branch-index-soak.py --bin build/c/code-cortex-mcp --repo ~/src/redis \\
      --out /tmp/soak-redis-1 \\
      --steps 'create:soak-old=HEAD~30,create:soak-new=HEAD,switch:soak-old,\\
switch:soak-new,move:HEAD~10,detach:HEAD~5,switch:soak-old'
"""

import argparse
import json
import os
import select
import sqlite3
import subprocess
import sys
import threading
import time

DEF_LABELS = {
    "Function", "Method", "Class", "Struct", "Interface", "Enum", "Field", "Variable",
    "Macro", "Declaration", "Module", "OverloadSet", "Route", "Type", "Trait", "Union",
    "Section", "EnvVar", "Decorator", "File", "Folder", "Package",
}
SAMPLE = 8


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def git(repo, *args, check=True):
    r = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout.strip()


def apply_step(repo, step):
    kind, _, arg = step.partition(":")
    if kind == "switch":
        git(repo, "checkout", "-q", arg)
    elif kind == "create":
        name, _, rev = arg.partition("=")
        git(repo, "checkout", "-q", "-B", name, rev)
    elif kind == "move":
        git(repo, "reset", "-q", "--hard", arg)
    elif kind == "detach":
        git(repo, "checkout", "-q", "--detach", arg)
    else:
        raise ValueError("unknown step kind: " + step)


def checkout_desc(repo):
    branch = git(repo, "symbolic-ref", "-q", "--short", "HEAD", check=False) or "(detached)"
    return branch, git(repo, "rev-parse", "HEAD")


def tool_text(stdout):
    """The text payload of a cli --json tool envelope, parsed when JSON."""
    try:
        env = json.loads(stdout)
        text = env["content"][0]["text"]
    except (ValueError, KeyError, IndexError, TypeError):
        return {"_raw": stdout[-2000:]}
    try:
        return json.loads(text)
    except ValueError:
        return {"_text": text}


class PeakSampler(threading.Thread):
    """Largest size seen of each watched file while a run is in flight."""

    def __init__(self, paths, every=0.05):
        super().__init__(daemon=True)
        self.paths = paths
        self.every = every
        self.peak = {p: -1 for p in paths}
        self.done = threading.Event()

    def run(self):
        while not self.done.is_set():
            for p in self.paths:
                self.peak[p] = max(self.peak[p], fsize(p))
            self.done.wait(self.every)

    def stop(self):
        self.done.set()
        self.join()
        return self.peak


def cli(binary, cache, tool, args, cwd, env_extra=None, timeout=7200, watch=()):
    env = dict(os.environ)
    env["CBM_CACHE_DIR"] = cache
    env.update(env_extra or {})
    sampler = PeakSampler(list(watch)) if watch else None
    if sampler:
        sampler.start()
    t0 = time.monotonic()
    r = subprocess.run([binary, "cli", "--json", tool], input=json.dumps(args), cwd=cwd,
                       capture_output=True, text=True, env=env, timeout=timeout)
    wall = time.monotonic() - t0
    if sampler:
        cli.last_peak = sampler.stop()
    out = tool_text(r.stdout)
    out["_rc"] = r.returncode
    if r.returncode != 0 or "_raw" in out:
        out["_stderr"] = r.stderr[-3000:]
    san = sanitizer_lines(r.stderr)
    if san:
        out["_sanitizer"] = san
        cli.sanitizer.extend(san)
    return out, wall


def sanitizer_lines(text):
    """Sanitizer reports in a process's stderr (a -DCBM_SANITIZE=ON build)."""
    # Report headers only: a coverage report lists file paths, and a repo
    # such as LLVM has directories named AddressSanitizer.
    keys = (": runtime error:", "ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
            "SUMMARY: AddressSanitizer", "SUMMARY: UndefinedBehaviorSanitizer",
            "WARNING: ThreadSanitizer")
    return [ln[:300] for ln in text.splitlines() if any(k in ln for k in keys)][:20]


cli.sanitizer = []


def hook(binary, cache, payload, cwd):
    env = dict(os.environ)
    env["CBM_CACHE_DIR"] = cache
    r = subprocess.run([binary, "hook-augment"], input=json.dumps(payload), cwd=cwd,
                       capture_output=True, text=True, env=env, timeout=120)
    cli.sanitizer.extend(sanitizer_lines(r.stderr))
    return r.stdout.strip()


def session_start(binary, cache, cwd):
    return hook(binary, cache, {"hook_event_name": "SessionStart", "cwd": cwd,
                                "session_id": "soak", "source": "startup"}, cwd)


def fsize(path):
    try:
        return os.stat(path).st_size
    except OSError:
        return -1


def slot_sizes(cache):
    out = {}
    base = os.path.join(cache, "branches")
    for root, _, files in os.walk(base):
        for f in files:
            if f.endswith(".db"):
                out[os.path.relpath(os.path.join(root, f), base)] = fsize(os.path.join(root, f))
    return out


def db_path(cache, project):
    return os.path.join(cache, project + ".db")


def snapshot(path, project):
    """The comparable content of one graph database."""
    con = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
    cur = con.cursor()
    snap = {"labels": {}, "types": {}, "nodes": set(), "loc": {}, "edges": set(), "hashes": {}}
    for label, n in cur.execute(
            "SELECT label, count(*) FROM nodes WHERE project=? GROUP BY label", (project,)):
        snap["labels"][label] = n
    for typ, n in cur.execute(
            "SELECT type, count(*) FROM edges WHERE project=? GROUP BY type", (project,)):
        snap["types"][typ] = n
    for label, qn, fp, sl, el in cur.execute(
            "SELECT label, qualified_name, file_path, start_line, end_line FROM nodes "
            "WHERE project=?", (project,)):
        snap["nodes"].add((label, qn))
        if label in DEF_LABELS:
            snap["loc"][(label, qn)] = (fp, sl, el)
    for typ, s, t in cur.execute(
            "SELECT e.type, a.qualified_name, b.qualified_name FROM edges e "
            "JOIN nodes a ON a.id=e.source_id JOIN nodes b ON b.id=e.target_id "
            "WHERE e.project=?", (project,)):
        snap["edges"].add((typ, s, t))
    for rel, sha in cur.execute(
            "SELECT rel_path, sha256 FROM file_hashes WHERE project=?", (project,)):
        snap["hashes"][rel] = sha
    con.close()
    return snap


def diff(got, truth):
    """Every difference of got from truth, grouped; empty dict when equal."""
    d = {}
    lab = {k: (got["labels"].get(k, 0), truth["labels"].get(k, 0))
           for k in set(got["labels"]) | set(truth["labels"])
           if got["labels"].get(k, 0) != truth["labels"].get(k, 0)}
    if lab:
        d["label_counts"] = lab
    typ = {k: (got["types"].get(k, 0), truth["types"].get(k, 0))
           for k in set(got["types"]) | set(truth["types"])
           if got["types"].get(k, 0) != truth["types"].get(k, 0)}
    if typ:
        d["edge_counts"] = typ
    extra = got["nodes"] - truth["nodes"]
    missing = truth["nodes"] - got["nodes"]
    if extra:
        d["nodes_extra"] = sorted(extra)
    if missing:
        d["nodes_missing"] = sorted(missing)
    moved = sorted(k for k in got["loc"].keys() & truth["loc"].keys()
                   if got["loc"][k] != truth["loc"][k])
    if moved:
        d["nodes_moved"] = [(k, got["loc"][k], truth["loc"][k]) for k in moved]
    eextra = got["edges"] - truth["edges"]
    emissing = truth["edges"] - got["edges"]
    if eextra:
        d["edges_extra"] = sorted(eextra)
    if emissing:
        d["edges_missing"] = sorted(emissing)
    stale = sorted(k for k in got["hashes"].keys() & truth["hashes"].keys()
                   if got["hashes"][k] != truth["hashes"][k])
    hx = sorted(got["hashes"].keys() - truth["hashes"].keys())
    hm = sorted(truth["hashes"].keys() - got["hashes"].keys())
    if stale:
        d["hash_stale"] = stale
    if hx:
        d["hash_extra"] = hx
    if hm:
        d["hash_missing"] = hm
    return d


def diff_items(d):
    """Flat set of comparable difference items, for on/off classification."""
    items = set()
    for key in ("nodes_extra", "nodes_missing", "edges_extra", "edges_missing", "hash_stale",
                "hash_extra", "hash_missing"):
        for v in d.get(key, []):
            items.add((key, v if isinstance(v, str) else tuple(v)))
    for v in d.get("nodes_moved", []):
        items.add(("nodes_moved", tuple(v[0]), tuple(v[1])))
    return items


def diff_summary(d):
    if not d:
        return "identical"
    parts = []
    for key in ("nodes_extra", "nodes_missing", "nodes_moved", "edges_extra", "edges_missing",
                "hash_stale", "hash_extra", "hash_missing"):
        if d.get(key):
            by = {}
            for v in d[key]:
                k = v[0][0] if key == "nodes_moved" else (v[0] if not isinstance(v, str) else "file")
                by[k] = by.get(k, 0) + 1
            parts.append("%s %d (%s)" % (key, len(d[key]),
                                         ", ".join("%s %d" % kv for kv in sorted(by.items()))))
    return "; ".join(parts)


def trim(d):
    return {k: (v[:SAMPLE] if isinstance(v, list) else v) for k, v in d.items()}


def brief_counts(text):
    """(nodes, edges) the SessionStart brief states, or None."""
    marker = " symbols, "
    i = text.find(marker)
    if i < 0:
        return None
    j = text.rfind("(", 0, i)
    k = text.find(" edges", i)
    try:
        return int(text[j + 1:i]), int(text[i + len(marker):k])
    except ValueError:
        return None


def index_once(a, cache, mode, repo, branch_off):
    env = {"CBM_BRANCH_INDEX": "0"} if branch_off else {}
    os.makedirs(cache, exist_ok=True)
    # The project name is the path-derived one; read it from an earlier run
    # of this cache when there is one, so the live WAL can be sampled.
    known = [f[:-3] for f in os.listdir(cache) if f.endswith(".db") and f != "_config.db"]
    watch = [db_path(cache, known[0]) + "-wal"] if len(known) == 1 else []
    cli.last_peak = {}
    res, wall = cli(a.bin, cache, "index_repository", {"repo_path": repo}, repo, env,
                    watch=watch)
    project = res.get("project")
    row = {"mode": mode, "wall_s": round(wall, 2), "rc": res["_rc"], "project": project,
           "status": res.get("status"), "nodes": res.get("nodes"), "edges": res.get("edges"),
           "branch": res.get("branch")}
    if res["_rc"] != 0 or not project:
        row["error"] = res
        return row
    dbp = db_path(cache, project)
    row["db_bytes"] = fsize(dbp)
    row["wal_bytes"] = fsize(dbp + "-wal")
    row["wal_peak_bytes"] = max(cli.last_peak.values()) if cli.last_peak else -1
    row["slots"] = slot_sizes(cache)
    st, _ = cli(a.bin, cache, "index_status", {"project": project}, repo, env)
    g = st.get("git", {})
    row["status_git"] = {k: g.get(k) for k in ("branch", "head_sha", "indexed_branch",
                                                "index_matches_checkout", "parked_branches")}
    lp, _ = cli(a.bin, cache, "list_projects", {}, repo, env)
    names = [p.get("name") for p in lp.get("projects", [])] if isinstance(lp, dict) else []
    row["list_projects"] = names
    text = session_start(a.bin, cache, repo)
    row["session_start"] = text[:300]
    row["brief_counts"] = brief_counts(text)
    return row


def check_row(row, branch, sha, project_count=1):
    """Assertions every step's row must meet; returns a list of failures."""
    bad = []
    if row.get("rc") != 0:
        return ["index rc=%s" % row.get("rc")]
    sg = row.get("status_git", {})
    if sg.get("index_matches_checkout") is not True:
        bad.append("index_matches_checkout=%s" % sg.get("index_matches_checkout"))
    if sg.get("head_sha") != sha:
        bad.append("status head_sha %s != %s" % (sg.get("head_sha"), sha))
    if len(row.get("list_projects", [])) != project_count:
        bad.append("list_projects=%s" % row.get("list_projects"))
    bc = row.get("brief_counts")
    if bc and (bc[0] != row.get("nodes") or bc[1] != row.get("edges")):
        bad.append("brief says %s nodes/edges, index has %s/%s" % (bc, row.get("nodes"),
                                                                  row.get("edges")))
    if not row.get("session_start", "").startswith("code-cortex: this repository is indexed"):
        bad.append("session_start: " + row.get("session_start", "")[:120])
    return bad


def run_sequence(a, results):
    os.makedirs(a.out, exist_ok=False)
    on_cache = os.path.join(a.out, "cache-on")
    off_cache = os.path.join(a.out, "cache-off")
    steps = ["start"] + [s for s in a.steps.split(",") if s]
    # Ground-truth diff of the "on" graph per checkout as last indexed: a
    # restore must bring back exactly that graph (its drift included).
    parked_state = {}
    for i, step in enumerate(steps):
        if step != "start":
            apply_step(a.repo, step)
        branch, sha = checkout_desc(a.repo)
        log("[%d] %s -> %s @ %s" % (i, step, branch, sha[:10]))
        rec = {"i": i, "step": step, "checkout": branch, "head": sha}
        rec["on"] = index_once(a, on_cache, "on", a.repo, False)
        log("    on : %.1fs %s" % (rec["on"]["wall_s"], json.dumps(rec["on"].get("branch"))))
        if not a.no_off:
            rec["off"] = index_once(a, off_cache, "off", a.repo, True)
            log("    off: %.1fs %s" % (rec["off"]["wall_s"],
                                       json.dumps(rec["off"].get("branch"))))
        rec["failures"] = check_row(rec["on"], branch, sha)
        if "off" in rec:
            off = rec["off"]
            rec["failures"] += ["off: " + f for f in check_row(off, branch, sha)]
            if off.get("slots") or (off.get("branch") or {}).get("switch") != "none":
                rec["failures"].append("off: parked %s, switch %s" % (
                    off.get("slots"), (off.get("branch") or {}).get("switch")))
        if not a.no_ground_truth:
            gt_cache = os.path.join(a.out, "gt-%02d" % i)
            gt = index_once(a, gt_cache, "gt", a.repo, False)
            rec["gt"] = {k: gt.get(k) for k in ("wall_s", "nodes", "edges", "db_bytes", "rc")}
            log("    gt : %.1fs" % gt["wall_s"])
            project = rec["on"].get("project")
            truth = snapshot(db_path(gt_cache, project), project)
            don = diff(snapshot(db_path(on_cache, project), project), truth)
            rec["diff_on"] = diff_summary(don)
            rec["diff_on_detail"] = trim(don)
            ion = diff_items(don)
            key = (branch, sha)
            if (rec["on"].get("branch") or {}).get("switch") == "restored":
                rec["restore_exact"] = parked_state.get(key) == ion
                if key in parked_state and not rec["restore_exact"]:
                    rec["failures"].append("restored graph differs from the parked one by %d "
                                           "items" % len(parked_state[key] ^ ion))
            parked_state[key] = ion
            if not a.no_off:
                doff = diff(snapshot(db_path(off_cache, project), project), truth)
                ioff = diff_items(doff)
                rec["diff_off"] = diff_summary(doff)
                rec["diff_off_detail"] = trim(doff)
                only_on = sorted(ion - ioff)
                rec["only_on"] = len(only_on)
                rec["only_on_sample"] = [list(map(str, x)) for x in only_on[:SAMPLE * 2]]
                rec["only_off"] = len(ioff - ion)
                rec["both"] = len(ion & ioff)
            if a.gt_twice and i == 0:
                gt2 = os.path.join(a.out, "gt-%02d-again" % i)
                index_once(a, gt2, "gt", a.repo, False)
                rec["gt_vs_gt"] = diff_summary(diff(snapshot(db_path(gt2, project), project),
                                                    truth))
            log("    diff on : %s" % rec["diff_on"])
            if "diff_off" in rec:
                log("    diff off: %s  (only-on %d, both %d, only-off %d)" % (
                    rec["diff_off"], rec["only_on"], rec["both"], rec["only_off"]))
        if rec["failures"]:
            log("    FAIL: %s" % "; ".join(rec["failures"]))
        results["steps"].append(rec)
        save(a, results)


class Server:
    """A stdio MCP server with newline-delimited JSON-RPC."""

    def __init__(self, binary, cache, cwd):
        env = dict(os.environ)
        env["CBM_CACHE_DIR"] = cache
        self.p = subprocess.Popen([binary], cwd=cwd, env=env, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=open(
                                      os.path.join(cache, "server.stderr"), "a"), text=True)
        self.next_id = 1

    def call(self, method, params=None, timeout=600):
        mid = self.next_id
        self.next_id += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": mid, "method": method,
                                       "params": params or {}}) + "\n")
        self.p.stdin.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.p.stdout], [], [], 1.0)
            if not ready:
                continue
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("server closed stdout")
            msg = json.loads(line)
            if msg.get("id") == mid:
                return msg
        raise TimeoutError(method)

    def notify(self, method):
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.p.stdin.flush()

    def tool(self, name, args):
        msg = self.call("tools/call", {"name": name, "arguments": args})
        return tool_text(json.dumps(msg.get("result", {})))

    def close(self):
        self.p.stdin.close()
        try:
            self.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.p.terminate()
            self.p.wait()


def wait_matches(srv, project, deadline_s):
    t0 = time.monotonic()
    while time.monotonic() - t0 < deadline_s:
        st = srv.tool("index_status", {"project": project})
        if st.get("git", {}).get("index_matches_checkout") is True:
            return round(time.monotonic() - t0, 1), st
        time.sleep(1)
    return None, st


def run_offline(a, results):
    """Check out another branch with no server, then let a server catch up."""
    cache = os.path.join(a.out, "cache-offline")
    rec = {"scenario": "offline", "failures": []}
    start_branch, start_sha = checkout_desc(a.repo)
    row = index_once(a, cache, "on", a.repo, False)
    project = row["project"]
    git(a.repo, "checkout", "-q", a.offline)
    branch, sha = checkout_desc(a.repo)
    st, _ = cli(a.bin, cache, "index_status", {"project": project}, a.repo)
    rec["status_before"] = st.get("git", {}).get("index_matches_checkout")
    text = session_start(a.bin, cache, a.repo)
    rec["session_start_before"] = text[:300]
    rec["session_start_note_before"] = next(
        (ln for ln in text.splitlines() if "this index was built for" in ln), None)
    if not rec["session_start_note_before"]:
        rec["failures"].append("SessionStart did not note the moved checkout")
    if rec["status_before"] is not False:
        rec["failures"].append("index_status before refresh: matches=%s" % rec["status_before"])
    srv = Server(a.bin, cache, a.repo)
    try:
        srv.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                "clientInfo": {"name": "soak", "version": "1"}})
        srv.notify("notifications/initialized")
        took, st = wait_matches(srv, project, a.wait)
        rec["offline_refresh_s"] = took
        rec["offline_status"] = st.get("git")
        if took is None:
            rec["failures"].append("server never refreshed the offline checkout")
        text = session_start(a.bin, cache, a.repo)
        rec["session_start_after"] = text[:300]
        if "this index was built for" in text:
            rec["failures"].append("SessionStart still notes a moved checkout after refresh")
        # Online switch: back to the start branch while the server watches.
        git(a.repo, "checkout", "-q", start_branch)
        time.sleep(0.5)
        took, st = wait_matches(srv, project, a.wait)
        rec["online_refresh_s"] = took
        rec["online_status"] = st.get("git")
        if took is None:
            rec["failures"].append("watcher never refreshed the online switch")
        if st.get("git", {}).get("head_sha") != start_sha:
            rec["failures"].append("online status head %s" % st.get("git", {}).get("head_sha"))
    finally:
        srv.close()
    rec["slots"] = slot_sizes(cache)
    results["offline"] = rec
    log("offline: %s" % json.dumps({k: rec[k] for k in rec if k.endswith("_s") or
                                    k == "failures"}))
    save(a, results)


def run_worktrees(a, results):
    cache = os.path.join(a.out, "cache-worktrees")
    rec = {"scenario": "worktrees", "failures": []}
    parent = index_once(a, cache, "on", a.repo, False)
    tag = os.path.basename(a.out.rstrip("/"))
    inner = os.path.join(a.repo, ".worktrees", tag)
    nested = os.path.join(a.repo, "nested-wt-" + tag)
    sibling = a.repo.rstrip("/") + "-wt-" + tag
    rev = a.worktree_rev
    for path in (inner, nested, sibling):
        git(a.repo, "worktree", "add", "-q", "--detach", path, rev)
    rec["paths"] = [inner, nested, sibling]
    # The parent's index must not take in a nested worktree's files.
    reindex = index_once(a, cache, "on", a.repo, False)
    con = sqlite3.connect("file:%s?mode=ro" % db_path(cache, parent["project"]), uri=True)
    leaked = {}
    for path in (inner, nested):
        rel = os.path.relpath(path, a.repo)
        leaked[rel] = con.execute(
            "SELECT count(*) FROM nodes WHERE label='File' AND file_path LIKE ?",
            (rel + "/%",)).fetchone()[0]
    con.close()
    rec["parent_files_from_worktrees"] = leaked
    rec["parent_reindex_branch"] = reindex.get("branch")
    for rel, n in leaked.items():
        if n:
            rec["failures"].append("parent index holds %d files of worktree %s" % (n, rel))
    # Hooks in an unindexed worktree must not answer from the parent.
    for path in (inner, nested, sibling):
        sub = path
        for cand in ("src", "lib", "Lib", "include"):
            if os.path.isdir(os.path.join(path, cand)):
                sub = os.path.join(path, cand)
                break
        text = session_start(a.bin, cache, sub)
        rec.setdefault("session_start_unindexed", {})[path] = text[:400]
        if "not indexed" not in text:
            rec["failures"].append("SessionStart in unindexed %s: %s" % (path, text[:120]))
        st, _ = cli(a.bin, cache, "index_status", {}, sub)
        rec.setdefault("status_unindexed", {})[path] = st.get("project") or st.get(
            "_text", st.get("error", ""))[:200]
        if st.get("project") == parent["project"]:
            rec["failures"].append("index_status in %s answered from the parent" % path)
    # Each worktree indexes on its own.
    projects = {parent["project"]}
    for path in (inner, nested, sibling):
        row = index_once(a, cache, "on", path, False)
        rec.setdefault("indexed", {})[path] = {k: row.get(k) for k in (
            "project", "wall_s", "nodes", "edges", "status_git", "branch")}
        projects.add(row.get("project"))
        if row.get("status_git", {}).get("index_matches_checkout") is not True:
            rec["failures"].append("worktree %s: %s" % (path, row.get("status_git")))
        text = session_start(a.bin, cache, path)
        if row.get("project") and ('"%s"' % row["project"]) not in text:
            rec["failures"].append("SessionStart in indexed %s: %s" % (path, text[:120]))
    rec["projects"] = sorted(p for p in projects if p)
    if len(rec["projects"]) != 4:
        rec["failures"].append("expected 4 projects, got %s" % rec["projects"])
    lp, _ = cli(a.bin, cache, "list_projects", {}, a.repo)
    rec["list_projects"] = sorted(p.get("name") for p in lp.get("projects", []))
    if rec["list_projects"] != rec["projects"]:
        rec["failures"].append("list_projects %s" % rec["list_projects"])
    for path in (inner, nested, sibling):
        git(a.repo, "worktree", "remove", "--force", path)
    results["worktrees"] = rec
    log("worktrees: failures=%s" % rec["failures"])
    save(a, results)


def save(a, results):
    with open(os.path.join(a.out, "results.json"), "w") as f:
        json.dump(results, f, indent=1, default=str)


def table(results):
    lines = ["| # | step | checkout | switch | reparsed | on s | off s | full s | DB MB | WAL peak MB "
             "| slots MB | diff on vs full | only-on | checks |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    mb = lambda b: "%.1f" % (b / 1e6) if isinstance(b, int) and b >= 0 else "-"
    for r in results["steps"]:
        on = r["on"]
        br = on.get("branch") or {}
        lines.append("| %d | %s | %s@%s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["i"], r["step"], r["checkout"], r["head"][:8], br.get("switch", "-"),
            br.get("files_reparsed", "-"), on.get("wall_s"),
            r.get("off", {}).get("wall_s", "-"), r.get("gt", {}).get("wall_s", "-"),
            mb(on.get("db_bytes")), mb(on.get("wal_peak_bytes")),
            mb(sum(on.get("slots", {}).values())), r.get("diff_on", "-"),
            r.get("only_on", "-"), "ok" if not r["failures"] else "; ".join(r["failures"])))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--out", required=True, help="fresh results directory (must not exist)")
    ap.add_argument("--steps", default="")
    ap.add_argument("--no-off", action="store_true", help="skip the CBM_BRANCH_INDEX=0 run")
    ap.add_argument("--no-ground-truth", action="store_true")
    ap.add_argument("--gt-twice", action="store_true",
                    help="full-index step 0 twice to measure full-vs-full nondeterminism")
    ap.add_argument("--offline", metavar="BRANCH", help="run the offline-checkout scenario")
    ap.add_argument("--worktrees", action="store_true", help="run the worktree scenario")
    ap.add_argument("--worktree-rev", default="HEAD")
    ap.add_argument("--wait", type=int, default=300, help="seconds to wait for a server refresh")
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)
    a.repo = os.path.realpath(a.repo)
    a.out = os.path.abspath(a.out)
    results = {"repo": a.repo, "bin": a.bin, "steps": [],
               "started": time.strftime("%Y-%m-%d %H:%M:%S")}
    if a.steps or not (a.offline or a.worktrees):
        run_sequence(a, results)
    else:
        os.makedirs(a.out, exist_ok=False)
    if a.offline:
        run_offline(a, results)
    if a.worktrees:
        run_worktrees(a, results)
    # Supervised index workers write their stderr to the cache's log files.
    for root, _, files in os.walk(a.out):
        for f in files:
            if f.endswith(".log") or f == "server.stderr":
                with open(os.path.join(root, f), errors="replace") as fh:
                    cli.sanitizer.extend(sanitizer_lines(fh.read()))
    results["sanitizer"] = sorted(set(cli.sanitizer))
    results["finished"] = time.strftime("%Y-%m-%d %H:%M:%S")
    save(a, results)
    if results["steps"]:
        print(table(results))
    fails = [f for r in results["steps"] for f in r["failures"]]
    print("sanitizer: %s" % ("clean" if not results["sanitizer"]
                             else "%d report lines" % len(results["sanitizer"])))
    fails += results["sanitizer"]
    for key in ("offline", "worktrees"):
        if key in results:
            fails += results[key]["failures"]
            print("%s: %s" % (key, "ok" if not results[key]["failures"]
                              else "; ".join(results[key]["failures"])))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
