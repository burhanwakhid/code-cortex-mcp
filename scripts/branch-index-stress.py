#!/usr/bin/env python3
"""Multi-process stress test of the per-branch index and the cache expiry.

One shared cache directory (CBM_CACHE_DIR), one scratch git repository with
three branches whose sources differ in known symbols, and a linked worktree
of it (inside the repository, at .wt/w1, so the main checkout's discovery
must skip it). For --minutes, these run at the same time:

  servers   K stdio MCP servers (`code-cortex-mcp`, no arguments) with their
            cwd in the main checkout, and M in the worktree. Each loops over
            index_repository, a verifying query (search_graph or
            query_graph), index_status and list_projects. Some cycles restart
            the server (stdin closed), which runs initialize again, the point
            where a server starts its own cache sweep.
  mutator   `git checkout` between the branches of the main checkout, and
            sometimes in the worktree. Every checkout bumps a generation
            counter before and after it runs.
  sweeper   `cache expire`, `cache expire --dry-run`, `cache trash`,
            `cache restore`. It ages things so that they are due: the decoy
            projects' usage rows, the last-sweep time (so the servers sweep at
            initialize and after index runs), parked slot mtimes (slot expiry
            races the switches), and trash entry epochs (purge races
            restore).
  hooks     `code-cortex-mcp hook-augment` SessionStart / UserPromptSubmit in
            either checkout.
  monitor   polls the cache: trash entries, lock-file inodes, scratch files.

Two decoy projects must expire: `decoy` (unused: its usage row is aged past
cache_expiry_unused_days = 1) and `gone` (root_gone: its directory is renamed
away and its missing streak is aged).

Invariants (each violation is recorded with its time, process and context):

  crash      no process exits unexpectedly, no MCP call or CLI command takes
             longer than --call-timeout, no sanitizer report in any stderr or
             in the cache's log files.
  branch     an index_repository that returned "indexed" while the checkout
             was stable (same mutator generation from before the call until
             after the query answered) is followed by a query that shows
             exactly that branch's symbols: none of another branch
             ("wrong_branch") and all of its own ("stale"). Busy, "index
             pending" and "project lookup incomplete" answers are allowed.
  trash      the live projects (main checkout and worktree) never appear in
             <cache>/trash; both decoys do.
  locks      no *.index.lock / *.read.lock file ever disappears or changes
             inode.
  outside    no new files outside the cache directory: HOME, TMPDIR and the
             two checkouts' untracked files are compared with the start.
  scratch    no .db.unz*, .db.zst.tmp, .db.tmp or .import_tmp file in the
             cache after the run quiesces.
  settle     after the mutator stops, index_status reports
             index_matches_checkout: true for both checkouts, and the final
             verifying query is exact.

Every process has its own log under <out>/logs. The seed is printed; every
random choice of a thread comes from Random("<seed>-<thread>"), and every
event is in <out>/logs/events.jsonl with a monotonic timestamp, so the
interleaving of a failure can be read back. Nothing is deleted: --out must be
a fresh directory.

Example:
  scripts/branch-index-stress.py --bin build/s/code-cortex-mcp \\
      --out ~/stress-runs/r1 --minutes 10 --servers 4 --wt-servers 2
"""

import argparse
import json
import os
import random
import re
import select
import signal
import sqlite3
import subprocess
import sys
import threading
import time

BRANCHES = ("a", "b", "c")
N_FILES = 300          # src/mod_NNN.c in every branch
N_BGONE = 20           # src/bgone_NN.c: deleted on branch b
N_CEXTRA = 20          # src/cextra_NN.c: added on branch c
MARKER_EVERY = 10      # a marker_<b>_<i> function in every 10th module
SYM_RE = re.compile(r"\b(only_on_[abc]|marker_[abc]_\d{3}|cextra_\d{2}|bgone_\d{2})\b")
SAN_KEYS = (": runtime error:", "ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
            "SUMMARY: AddressSanitizer", "SUMMARY: UndefinedBehaviorSanitizer",
            "WARNING: ThreadSanitizer", "ERROR: ThreadSanitizer")
ALLOWED_RE = re.compile(r"index pending|project lookup incomplete|index_busy|\"busy\"|"
                        r"being indexed|Retry later|indexing is already active", re.I)
SCRATCH_RE = re.compile(r"(\.db\.unz(\.tmp)?(-wal|-shm|-journal)?|\.db\.zst\.tmp|\.db\.tmp|"
                        r"\.db\.import_tmp)$")
DAY = 86400


def expected(letter):
    """The marker symbols of a checkout of branch <letter>."""
    s = {"only_on_" + letter}
    s |= {"marker_%s_%03d" % (letter, i) for i in range(0, N_FILES, MARKER_EVERY)}
    if letter != "b":
        s |= {"bgone_%02d" % i for i in range(N_BGONE)}
    if letter == "c":
        s |= {"cextra_%02d" % i for i in range(N_CEXTRA)}
    return s


# ── Shared state ────────────────────────────────────────────────────────


class Run:
    def __init__(self, a):
        self.a = a
        self.t0 = time.monotonic()
        self.lock = threading.Lock()
        self.events = open(os.path.join(a.logs, "events.jsonl"), "a", buffering=1)
        self.violations = []
        self.counts = {}
        self.stop = threading.Event()        # ends the stress phase
        self.stop_mut = threading.Event()    # ends the mutator early (settle)
        self.sanitizer = []
        self.checkouts = {}                  # path -> Checkout
        self.live_projects = set()
        self.decoys = {}                     # name -> dict(path=..., kind=...)
        self.decoy_trashed = {}              # name -> count of trash entries seen

    def now(self):
        return round(time.monotonic() - self.t0, 3)

    def event(self, who, kind, **kw):
        rec = {"t": self.now(), "who": who, "kind": kind}
        rec.update(kw)
        with self.lock:
            self.events.write(json.dumps(rec, default=str) + "\n")

    def count(self, key, n=1):
        with self.lock:
            self.counts[key] = self.counts.get(key, 0) + n

    def violate(self, who, kind, detail, **ctx):
        rec = {"t": self.now(), "who": who, "kind": kind, "detail": detail}
        rec.update(ctx)
        with self.lock:
            self.violations.append(rec)
        self.event(who, "VIOLATION", violation=kind, detail=detail, **ctx)
        log("VIOLATION %s %s: %s" % (who, kind, detail))

    def scan_sanitizer(self, who, text):
        lines = [ln[:300] for ln in text.splitlines() if any(k in ln for k in SAN_KEYS)]
        if lines:
            with self.lock:
                self.sanitizer.extend(lines)
            self.violate(who, "sanitizer", lines[0], lines=lines[:20])


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, file=sys.stderr, flush=True)


class Checkout:
    """A checkout the mutator moves; the generation is even when stable."""

    def __init__(self, path, branch, prefix):
        self.path = path
        self.branch = branch
        self.prefix = prefix   # "" for main, "w" for the worktree's branches
        self.gen = 0
        self.cv = threading.Lock()
        self.project = None

    def snap(self):
        with self.cv:
            return self.gen, self.branch

    def letter(self, branch):
        return branch[-1]


def env_for(a, extra=None):
    env = dict(os.environ)
    env["HOME"] = a.home
    env["TMPDIR"] = a.tmpdir
    env["CBM_CACHE_DIR"] = a.cache
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=0")
    env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1")
    env.pop("CBM_CACHE_EXPIRY", None)
    env.update(extra or {})
    return env


def run_cmd(run, who, argv, cwd, timeout, input_text=None, logf=None):
    """Run one CLI process; returns (rc, stdout, stderr, wall). rc None = hang."""
    t0 = time.monotonic()
    p = subprocess.Popen(argv, cwd=cwd, env=env_for(run.a), stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                         start_new_session=True)
    try:
        out, err = p.communicate(input_text, timeout=timeout)
        rc = p.returncode
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        out, err = p.communicate()
        rc = None
        run.violate(who, "hang", "%s took > %ss" % (" ".join(argv[1:4]), timeout), pid=p.pid)
    wall = time.monotonic() - t0
    if logf:
        logf.write("=== t=%.3f pid=%d rc=%s wall=%.2f %s\n%s%s\n" % (
            run.now(), p.pid, rc, wall, " ".join(argv[1:]), out[-4000:], err[-8000:]))
        logf.flush()
    run.scan_sanitizer(who, err)
    if rc is not None and rc < 0:
        run.violate(who, "crash", "%s died by signal %d" % (" ".join(argv[1:4]), -rc),
                    pid=p.pid, stderr=err[-2000:])
    return rc, out, err, wall


# ── Scratch repository ─────────────────────────────────────────────────


def git(path, *args, check=True, env=None):
    r = subprocess.run(["git", "-C", path, *args], capture_output=True, text=True, env=env)
    if check and r.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), r.stderr.strip()))
    return r


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def module_src(i, letter=None):
    """src/mod_NNN.c: a few functions that call into neighbours."""
    nxt = (i + 1) % N_FILES
    prv = (i + N_FILES - 1) % N_FILES
    lines = ["/* module %03d */" % i,
             "int mod_%03d_entry(int x);" % nxt,
             "int mod_%03d_helper(int x);" % prv,
             "static int mod_%03d_local(int x) { return x * %d + 1; }" % (i, i % 7 + 2),
             "int mod_%03d_helper(int x) { return mod_%03d_local(x) - %d; }" % (i, i, i % 5),
             "int mod_%03d_entry(int x) {" % i,
             "    if (x > %d) return mod_%03d_helper(x - 1);" % (i % 11 + 3, prv),
             "    return mod_%03d_local(x);" % i,
             "}"]
    for k in range(6):
        lines.append("int mod_%03d_fn%d(int a, int b) { int s = a; for (int j = 0; j < b; j++) "
                     "s += mod_%03d_helper(j + %d); return s; }" % (i, k, i, k))
    if letter and i % MARKER_EVERY == 0:
        lines.append("int marker_%s_%03d(void) { return mod_%03d_entry(%d); }" % (
            letter, i, i, ord(letter)))
    return "\n".join(lines) + "\n"


def make_repo(a, path):
    os.makedirs(path)
    genv = dict(os.environ, HOME=a.home, GIT_AUTHOR_NAME="stress", GIT_AUTHOR_EMAIL="s@x",
                GIT_COMMITTER_NAME="stress", GIT_COMMITTER_EMAIL="s@x")
    git(path, "init", "-q", "-b", "main", env=genv)
    for i in range(N_FILES):
        write(os.path.join(path, "src", "mod_%03d.c" % i), module_src(i))
    for i in range(N_BGONE):
        write(os.path.join(path, "src", "bgone_%02d.c" % i),
              "int mod_000_entry(int x);\nint bgone_%02d(void) { return mod_000_entry(%d); }\n"
              % (i, i))
    write(os.path.join(path, "README.md"), "# stress scratch repository\n")
    git(path, "add", "-A", env=genv)
    git(path, "commit", "-q", "-m", "base", env=genv)
    for letter in BRANCHES:
        git(path, "checkout", "-q", "-b", letter, "main", env=genv)
        for i in range(0, N_FILES, MARKER_EVERY):
            write(os.path.join(path, "src", "mod_%03d.c" % i), module_src(i, letter))
        write(os.path.join(path, "src", "only_%s.c" % letter),
              "int mod_001_entry(int x);\nint only_on_%s(void) { return mod_001_entry(1); }\n"
              % letter)
        if letter == "b":
            for i in range(N_BGONE):
                os.unlink(os.path.join(path, "src", "bgone_%02d.c" % i))
        if letter == "c":
            for i in range(N_CEXTRA):
                write(os.path.join(path, "src", "cextra_%02d.c" % i),
                      "int mod_002_entry(int x);\nint cextra_%02d(void) "
                      "{ return mod_002_entry(%d); }\n" % (i, i))
        git(path, "add", "-A", env=genv)
        git(path, "commit", "-q", "-m", "branch " + letter, env=genv)
        # The worktree's branches: same commits, other names (a branch can
        # be checked out in one worktree only).
        git(path, "branch", "-q", "w" + letter, letter, env=genv)
    git(path, "checkout", "-q", "a", env=genv)
    with open(os.path.join(path, ".git", "info", "exclude"), "a") as f:
        f.write(".wt/\n")
    wt = os.path.join(path, ".wt", "w1")
    git(path, "worktree", "add", "-q", wt, "wb", env=genv)
    return wt


def make_decoy(path, name):
    for i in range(12):
        write(os.path.join(path, "d_%02d.c" % i), "int %s_fn_%02d(void) { return %d; }\n" % (
            name, i, i))


# ── Mutator ────────────────────────────────────────────────────────────


def mutator(run):
    a = run.a
    rng = random.Random("%s-mutator" % a.seed)
    logf = open(os.path.join(a.logs, "mutator.log"), "a", buffering=1)
    main = run.checkouts["main"]
    wt = run.checkouts["wt"]
    while not run.stop.is_set() and not run.stop_mut.is_set():
        # Mostly stable windows long enough for an index + query cycle,
        # sometimes a burst of quick switches.
        burst = rng.random() < 0.12
        wait = rng.uniform(0.05, 0.8) if burst else rng.uniform(a.min_stable, a.max_stable)
        if run.stop_mut.wait(wait) or run.stop.is_set():
            break
        co = wt if rng.random() < 0.3 else main
        with co.cv:
            cur = co.branch
            target = co.prefix + rng.choice([b for b in BRANCHES if co.prefix + b != cur])
            co.gen += 1
        ok = False
        err = ""
        for attempt in range(50):
            r = git(co.path, "checkout", "-q", target, check=False)
            if r.returncode == 0:
                ok = True
                break
            err = r.stderr.strip()
            time.sleep(0.1)
        with co.cv:
            if ok:
                co.branch = target
            co.gen += 1
        run.count("switch_" + ("wt" if co is wt else "main"))
        run.event("mutator", "checkout", co=("wt" if co is wt else "main"), frm=cur, to=target,
                  ok=ok, gen=co.gen, burst=burst)
        logf.write("t=%.3f %s %s -> %s ok=%s gen=%d %s\n" % (
            run.now(), co.path, cur, target, ok, co.gen, err))
        if not ok:
            run.violate("mutator", "harness", "git checkout %s failed: %s" % (target, err))


# ── MCP servers ────────────────────────────────────────────────────────


class Server:
    def __init__(self, run, name, cwd):
        self.run = run
        self.name = name
        self.cwd = cwd
        self.incarnation = 0
        self.p = None
        self.errf = None
        self.next_id = 1

    def start(self):
        self.incarnation += 1
        a = self.run.a
        self.stderr_path = os.path.join(a.logs, "%s.%d.stderr" % (self.name, self.incarnation))
        self.errf = open(self.stderr_path, "a")
        self.p = subprocess.Popen([a.bin], cwd=self.cwd, env=env_for(a), stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=self.errf, text=True,
                                  start_new_session=True)
        self.run.event(self.name, "start", pid=self.p.pid, incarnation=self.incarnation)
        self.run.count("server_starts")
        self.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                 "clientInfo": {"name": "stress", "version": "1"}})
        self.notify("notifications/initialized")

    def call(self, method, params=None):
        mid = self.next_id
        self.next_id += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": mid, "method": method,
                                       "params": params or {}}) + "\n")
        self.p.stdin.flush()
        deadline = time.monotonic() + self.run.a.call_timeout
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.p.stdout], [], [], 1.0)
            if not ready:
                if self.p.poll() is not None:
                    raise ServerDied(self.p.returncode)
                continue
            line = self.p.stdout.readline()
            if not line:
                raise ServerDied(self.p.wait())
            try:
                msg = json.loads(line)
            except ValueError:
                continue
            if msg.get("id") == mid:
                return msg
        raise TimeoutError(method)

    def notify(self, method):
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.p.stdin.flush()

    def tool(self, name, args):
        msg = self.call("tools/call", {"name": name, "arguments": args})
        res = msg.get("result") or {}
        text = ""
        try:
            text = res["content"][0]["text"]
        except (KeyError, IndexError, TypeError):
            text = json.dumps(msg)[:4000]
        try:
            parsed = json.loads(text)
        except ValueError:
            parsed = None
        return text, parsed, bool(res.get("isError")) or "error" in msg

    def close(self, kill=False):
        if not self.p:
            return
        pid = self.p.pid
        try:
            if kill:
                self.p.kill()
            else:
                self.p.stdin.close()
            rc = self.p.wait(timeout=self.run.a.call_timeout)
        except subprocess.TimeoutExpired:
            self.run.violate(self.name, "hang", "server did not exit after stdin closed", pid=pid)
            os.killpg(pid, signal.SIGKILL)
            rc = self.p.wait()
        except (BrokenPipeError, OSError):
            rc = self.p.wait()
        self.errf.close()
        with open(self.stderr_path, errors="replace") as f:
            self.run.scan_sanitizer(self.name, f.read())
        if not kill and rc != 0:
            self.run.violate(self.name, "crash", "server exit status %s at close" % rc, pid=pid)
        self.run.event(self.name, "exit", pid=pid, rc=rc, killed=kill)
        self.p = None


class ServerDied(Exception):
    pass


def verify_query(run, who, co, letter, text, ctx):
    if ALLOWED_RE.search(text):
        run.count("query_allowed_nonanswer")
        return "allowed"
    got = set(SYM_RE.findall(text))
    exp = expected(letter)
    foreign = sorted(got - exp)
    missing = sorted(exp - got)
    if foreign:
        run.violate(who, "wrong_branch", "checkout %s=%s shows %d foreign symbols: %s" % (
            co.path, co.prefix + letter, len(foreign), foreign[:8]), missing=missing[:8],
            text=text[:600], **ctx)
        return "wrong"
    if missing:
        run.violate(who, "stale", "checkout %s=%s misses %d of its symbols: %s" % (
            co.path, co.prefix + letter, len(missing), missing[:8]), text=text[:600], **ctx)
        return "stale"
    run.count("query_verified_ok")
    return "ok"


def query(srv, rng, project):
    if rng.random() < 0.5:
        args = {"name_pattern": "(only_on|marker|cextra|bgone)_.*", "label": "Function",
                "limit": 500}
        if project and rng.random() < 0.5:
            args["project"] = project
        return "search_graph", srv.tool("search_graph", args)
    args = {"query": "MATCH (f:Function) WHERE f.name =~ '(only_on|marker|cextra|bgone)_.*' "
                     "RETURN f.name LIMIT 500"}
    if project:
        args["project"] = project
    return "query_graph", srv.tool("query_graph", args)


def server_loop(run, name, co):
    a = run.a
    rng = random.Random("%s-%s" % (a.seed, name))
    srv = Server(run, name, co.path)
    who = name
    while not run.stop.is_set():
        try:
            if srv.p is None:
                srv.start()
            if a.kill_rate and rng.random() < a.kill_rate:
                kill_mid_index(run, srv, co, rng)
                continue
            r = rng.random()
            if r < 0.06:
                srv.close()
                continue
            if r < 0.70:
                server_cycle(run, srv, co, rng)
            elif r < 0.85:
                _, (text, _, _) = query(srv, rng, co.project)
                run.count("queries")
            elif r < 0.95:
                text, parsed, _ = srv.tool("index_status", {"project": co.project}
                                           if co.project else {})
                run.count("index_status")
            else:
                srv.tool("list_projects", {})
                run.count("list_projects")
        except ServerDied as e:
            run.violate(who, "crash", "server died, exit status %s" % e.args[0],
                        incarnation=srv.incarnation, stderr=srv.stderr_path)
            srv.close(kill=True)
        except TimeoutError as e:
            run.violate(who, "hang", "MCP call %s exceeded %ss" % (e.args[0], a.call_timeout),
                        incarnation=srv.incarnation, pid=srv.p.pid if srv.p else None)
            srv.close(kill=True)
        except (BrokenPipeError, OSError) as e:
            run.violate(who, "crash", "server pipe broke: %s" % e, incarnation=srv.incarnation)
            srv.close(kill=True)
        time.sleep(rng.uniform(0, a.server_pause))
    return srv


def kill_mid_index(run, srv, co, rng):
    """SIGKILL a server's process group (its index worker too) mid-run."""
    mid = srv.next_id
    srv.next_id += 1
    srv.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": mid, "method": "tools/call",
                                  "params": {"name": "index_repository",
                                             "arguments": {"repo_path": co.path}}}) + "\n")
    srv.p.stdin.flush()
    delay = rng.uniform(0, 2.0)
    time.sleep(delay)
    pid = srv.p.pid
    try:
        os.killpg(pid, signal.SIGKILL)
    except OSError:
        pass
    run.count("kills")
    run.event(srv.name, "killed", pid=pid, after=round(delay, 3))
    srv.close(kill=True)


def server_cycle(run, srv, co, rng, settle=False):
    """index_repository, then a verifying query when the checkout held still."""
    who = srv.name
    gen0, br0 = co.snap()
    t0 = time.monotonic()
    text, parsed, is_err = srv.tool("index_repository", {"repo_path": co.path})
    wall = time.monotonic() - t0
    run.count("index_calls")
    status = (parsed or {}).get("status")
    branch = (parsed or {}).get("branch") or {}
    if parsed and parsed.get("project"):
        co.project = parsed["project"]
        run.live_projects.add(parsed["project"])
    run.event(who, "index", co=co.path, gen=gen0, checkout=br0, status=status,
              branch=branch, wall=round(wall, 2), err=is_err,
              text=None if status in ("indexed", "busy") else text[:400])
    if status == "busy":
        run.count("index_busy")
        return "busy"
    if status != "indexed":
        run.count("index_other")
        if not ALLOWED_RE.search(text):
            run.violate(who, "index_error", text[:300], gen=gen0, checkout=br0)
        return "error"
    run.count("index_ok")
    run.count("switch_" + str(branch.get("switch")))
    kind, (qtext, _, _) = query(srv, rng, co.project)
    run.count("queries")
    gen1, br1 = co.snap()
    stable = gen0 == gen1 and gen0 % 2 == 0
    if not stable:
        run.count("query_unverifiable")
        return "moved"
    ctx = {"gen": gen0, "index_branch": branch, "query": kind, "incarnation": srv.incarnation}
    if branch.get("indexed") and branch.get("indexed") != br0:
        run.violate(who, "wrong_branch", "index_repository says it indexed %s, checkout is %s"
                    % (branch.get("indexed"), br0), **ctx)
    res = verify_query(run, who, co, co.letter(br0), qtext, ctx)
    run.event(who, "verify", co=co.path, checkout=br0, result=res, query=kind)
    if res in ("wrong", "stale"):
        diagnose(run, srv, co, rng, br0)
    return res


def db_symbols(run, project):
    """The marker symbols in the live database, read directly (no server)."""
    path = os.path.join(run.a.cache, project + ".db")
    try:
        st = os.stat(path)
        con = sqlite3.connect("file:%s?mode=ro" % path, uri=True, timeout=10)
        names = [r[0] for r in con.execute(
            "SELECT name FROM nodes WHERE label='Function' AND (name LIKE 'only_on_%' OR "
            "name LIKE 'marker_%' OR name LIKE 'cextra_%' OR name LIKE 'bgone_%')")]
        stamp = con.execute("SELECT name FROM nodes WHERE label='Branch'").fetchall()
        con.close()
        return {"ino": st.st_ino, "size": st.st_size, "mtime_ns": st.st_mtime_ns,
                "branch_nodes": [r[0] for r in stamp],
                "letters": sorted({n.split("_")[2] if n.startswith("only") else
                                   n.split("_")[1] if n.startswith("marker") else n.split("_")[0]
                                   for n in names}), "count": len(names)}
    except (OSError, sqlite3.Error) as e:
        return {"error": str(e)}


def diagnose(run, srv, co, rng, branch):
    """After a bad answer: is the database wrong, or only this server's view?"""
    info = {"db_direct": db_symbols(run, co.project)}
    for k in range(2):
        time.sleep(0.5)
        _, (t, _, _) = query(srv, rng, co.project)
        got = set(SYM_RE.findall(t))
        exp = expected(co.letter(branch))
        info["requery_%d" % k] = {"foreign": len(got - exp), "missing": len(exp - got)}
    info["db_direct_after"] = db_symbols(run, co.project)
    try:
        pids = subprocess.run(["pgrep", "-g", str(os.getpgid(srv.p.pid))],
                              capture_output=True, text=True).stdout.split()
        fds = {}
        for pid in pids:
            fd_dir = "/proc/%s/fd" % pid
            for fd in os.listdir(fd_dir):
                try:
                    tgt = os.readlink(os.path.join(fd_dir, fd))
                except OSError:
                    continue
                if ".db" in tgt:
                    ino = None
                    try:
                        ino = os.stat(os.path.join(fd_dir, fd)).st_ino
                    except OSError:
                        pass
                    fds.setdefault(pid, []).append("%s ino=%s" % (tgt, ino))
        info["server_db_fds"] = fds
    except OSError:
        pass
    run.event(srv.name, "diagnose", **info)
    log("diagnose %s: %s" % (srv.name, json.dumps(info)))


# ── Sweeper, hooks, monitor ─────────────────────────────────────────────


def config_db(run):
    return sqlite3.connect(os.path.join(run.a.cache, "_config.db"), timeout=10)


def age_usage(run, dbname, last_used=None, missing=None):
    """Age a usage row so the next sweep finds the project due."""
    try:
        con = config_db(run)
        if last_used is not None:
            con.execute("UPDATE cache_usage SET last_used=? WHERE db_name=?", (last_used, dbname))
        if missing is not None:
            con.execute("UPDATE cache_usage SET missing_since=?, missing_checks=? "
                        "WHERE db_name=?", (missing[0], missing[1], dbname))
        con.commit()
        con.close()
        return True
    except sqlite3.Error as e:
        run.event("sweeper", "age_failed", db=dbname, err=str(e))
        return False


def clear_last_sweep(run):
    try:
        con = config_db(run)
        con.execute("DELETE FROM cache_sweep WHERE key='last_sweep'")
        con.commit()
        con.close()
    except sqlite3.Error:
        pass


def trash_entries(run):
    t = os.path.join(run.a.cache, "trash")
    try:
        return sorted(os.listdir(t))
    except OSError:
        return []


def sweeper(run):
    a = run.a
    rng = random.Random("%s-sweeper" % a.seed)
    logf = open(os.path.join(a.logs, "sweeper.log"), "a", buffering=1)
    b = a.bin
    while not run.stop.is_set():
        r = rng.random()
        now = int(time.time())
        if r < 0.30:
            argv, what = [b, "cache", "expire", "--dry-run"], "expire_dry"
        elif r < 0.55:
            argv, what = [b, "cache", "expire"], "expire"
        elif r < 0.62:
            argv, what = [b, "cache", "trash"], "trash"
        elif r < 0.72:
            name = rng.choice(sorted(run.decoys) + sorted(run.live_projects)[:1])
            argv, what = [b, "cache", "restore", name], "restore"
        elif r < 0.84:
            # Age both decoys again (a restore restarted their clocks) and let
            # every server sweep at its next initialize / index run.
            age_usage(run, run.decoy_kind["decoy"] + ".db", last_used=now - 3 * DAY)
            age_usage(run, run.decoy_kind["gone"] + ".db", last_used=now - 3 * DAY,
                      missing=(now - 10 * DAY, 5))
            clear_last_sweep(run)
            run.count("ages")
            run.event("sweeper", "aged_decoys")
            argv = None
        elif r < 0.93:
            # Age a parked slot of a live project past cache_expiry_slot_days.
            slots = []
            for root, _, files in os.walk(os.path.join(a.cache, "branches")):
                slots += [os.path.join(root, f) for f in files if f.endswith(".db.zst")]
            if slots:
                s = rng.choice(slots)
                try:
                    os.utime(s, (now - 3 * DAY, now - 3 * DAY))
                    run.count("slot_aged")
                    run.event("sweeper", "slot_aged", path=s)
                except OSError:
                    pass
            argv = None
        else:
            # Age a trash entry past the trash period, then race the purge
            # (a real sweep) with a restore of the same project.
            argv = None
            ents = [e for e in trash_entries(run) if "-" in e]
            if ents:
                e = rng.choice(ents)
                epoch, _, proj = e.partition("-")
                old = os.path.join(a.cache, "trash", e)
                new = os.path.join(a.cache, "trash", "%d-%s" % (int(epoch) - 3 * DAY, proj))
                try:
                    os.rename(old, new)
                except OSError:
                    continue
                run.count("purge_races")
                run.event("sweeper", "purge_race", entry=e, project=proj)
                th = threading.Thread(target=run_cmd, args=(
                    run, "sweeper-restore", [b, "cache", "restore", proj], a.root,
                    a.call_timeout, None, logf))
                th.start()
                run_cmd(run, "sweeper", [b, "cache", "expire"], a.root, a.call_timeout,
                        None, logf)
                th.join()
                check_decoy_db(run, proj)
                run.count("sweeps")
        if argv:
            rc, out, err, wall = run_cmd(run, "sweeper", argv, a.root, a.call_timeout,
                                         None, logf)
            run.count("cmd_" + what)
            run.event("sweeper", what, rc=rc, wall=round(wall, 2), argv=argv[2:],
                      out=out[-600:])
            if what in ("expire", "expire_dry"):
                run.count("sweeps" if what == "expire" else "dry_runs")
                for proj in run.live_projects:
                    for ln in out.splitlines():
                        if what == "expire" and proj in ln and re.search(
                                r"\b(trash|expired|removed|moved)\b", ln, re.I) and \
                                "kept" not in ln.lower() and "skip" not in ln.lower():
                            run.event("sweeper", "live_mentioned", line=ln[:300])
        run.stop.wait(rng.uniform(0.2, a.sweep_pause))


def check_decoy_db(run, proj):
    """After a purge/restore race the database is either gone or whole."""
    if proj not in run.decoys:
        return
    dbp = os.path.join(run.a.cache, proj + ".db")
    if not os.path.exists(dbp):
        return
    try:
        con = sqlite3.connect("file:%s?mode=ro" % dbp, uri=True, timeout=10)
        n = con.execute("SELECT count(*) FROM nodes WHERE name LIKE ?",
                        (run.decoys[proj]["sym"] + "%",)).fetchone()[0]
        ok = con.execute("PRAGMA quick_check").fetchone()[0]
        con.close()
    except sqlite3.Error as e:
        run.violate("sweeper", "restore_torn", "%s unreadable after purge/restore race: %s"
                    % (dbp, e))
        return
    if n != 12 or ok != "ok":
        run.violate("sweeper", "restore_torn", "%s after purge/restore race: %d symbols, %s"
                    % (dbp, n, ok))


def hooks(run):
    a = run.a
    rng = random.Random("%s-hooks" % a.seed)
    logf = open(os.path.join(a.logs, "hooks.log"), "a", buffering=1)
    while not run.stop.is_set():
        co = rng.choice(list(run.checkouts.values()))
        cwd = os.path.join(co.path, "src") if rng.random() < 0.5 else co.path
        if rng.random() < 0.5:
            payload = {"hook_event_name": "SessionStart", "cwd": cwd, "session_id": "stress",
                       "source": "startup"}
        else:
            letter = rng.choice(BRANCHES)
            payload = {"hook_event_name": "UserPromptSubmit", "cwd": cwd,
                       "session_id": "stress",
                       "prompt": "Where is only_on_%s defined and what calls mod_001_entry?"
                                 % letter}
        rc, out, err, wall = run_cmd(run, "hooks", [a.bin, "hook-augment"], cwd,
                                     a.call_timeout, json.dumps(payload), logf)
        run.count("hooks")
        run.event("hooks", payload["hook_event_name"], cwd=cwd, rc=rc, wall=round(wall, 2))
        if rc not in (0, None):
            run.violate("hooks", "crash", "hook-augment exit status %s" % rc, cwd=cwd)
        run.stop.wait(rng.uniform(0.2, 2.0))


def monitor(run):
    """Lock files never vanish; live projects never reach the trash."""
    a = run.a
    locks = {}
    seen_trash = set()
    logf = open(os.path.join(a.logs, "monitor.log"), "a", buffering=1)
    while True:
        done = run.stop.is_set() and getattr(run, "monitor_final", False)
        try:
            names = os.listdir(a.cache)
        except OSError:
            names = []
        for n in names:
            if n.endswith(".index.lock") or n.endswith(".read.lock"):
                p = os.path.join(a.cache, n)
                try:
                    ino = os.stat(p).st_ino
                except OSError:
                    continue
                if n in locks and locks[n] != ino:
                    run.violate("monitor", "locks", "%s changed inode" % n)
                locks[n] = ino
        present = set(names)
        for n in list(locks):
            if n not in present:
                run.violate("monitor", "locks", "%s disappeared" % n)
                del locks[n]
        for e in trash_entries(run):
            if e in seen_trash:
                continue
            seen_trash.add(e)
            proj = e.partition("-")[2]
            logf.write("t=%.3f trash entry %s\n" % (run.now(), e))
            run.event("monitor", "trash_entry", entry=e)
            if proj in run.live_projects:
                run.violate("monitor", "trash", "live project %s moved to trash (%s)"
                            % (proj, e))
            if proj in run.decoys:
                with run.lock:
                    run.decoy_trashed[proj] = run.decoy_trashed.get(proj, 0) + 1
        run.monitor_locks = locks
        if done:
            break
        time.sleep(0.2)


# ── Setup, snapshots, settle ───────────────────────────────────────────


def tree(path, skip=()):
    out = set()
    for root, dirs, files in os.walk(path):
        dirs[:] = [d for d in dirs if os.path.join(root, d) not in skip]
        for f in files + [d for d in dirs]:
            out.add(os.path.relpath(os.path.join(root, f), path))
    return out


def untracked(path):
    r = git(path, "status", "--porcelain", "--ignored", "-uall", check=False)
    return set(r.stdout.splitlines())


def cli_index(run, path, who):
    rc, out, err, _ = run_cmd(run, who, [run.a.bin, "cli", "--json", "index_repository"], path,
                              run.a.call_timeout, json.dumps({"repo_path": path}))
    try:
        env = json.loads(out)
        return json.loads(env["content"][0]["text"])
    except (ValueError, KeyError, IndexError, TypeError):
        raise RuntimeError("index of %s failed: rc=%s %s %s" % (path, rc, out[-500:],
                                                               err[-1000:]))


def setup(run):
    a = run.a
    for d in (a.home, a.tmpdir, a.cache, a.logs):
        os.makedirs(d, exist_ok=True)
    repo = os.path.join(a.root, "repo")
    log("creating scratch repository %s" % repo)
    wt = make_repo(a, repo)
    run.checkouts["main"] = Checkout(repo, "a", "")
    run.checkouts["wt"] = Checkout(wt, "wb", "w")
    for key in ("cache_expiry_unused_days", "cache_expiry_slot_days",
                "cache_expiry_root_grace_days"):
        run_cmd(run, "setup", [a.bin, "config", "set", key, "1"], a.root, 120)
    run_cmd(run, "setup", [a.bin, "config", "set", "branch_index_keep", str(a.keep)], a.root,
            120)
    # Decoys: "decoy" expires as unused, "gone" as root_gone.
    # Project names are path-derived: run.decoys is keyed by project name,
    # run.decoy_kind maps "decoy"/"gone" to it.
    run.decoy_kind = {}
    for kind in ("decoy", "gone"):
        path = os.path.join(a.root, kind)
        make_decoy(path, kind)
        res = cli_index(run, path, "setup")
        proj = res["project"]
        run.decoys[proj] = {"path": path, "db": proj + ".db", "sym": kind + "_fn_",
                            "kind": kind, "project": proj}
        run.decoy_kind[kind] = proj
    gone = run.decoys[run.decoy_kind["gone"]]
    os.rename(gone["path"], gone["path"] + ".moved-away")
    run.decoy_names = set(run.decoys)
    # Usage rows exist once a sweep has seen the databases.
    run_cmd(run, "setup", [a.bin, "cache", "expire", "--dry-run"], a.root, 120)
    run_cmd(run, "setup", [a.bin, "cache", "expire"], a.root, 120)
    # Initial indexes, so the decoy's slot dir and the live projects exist.
    for key, co in run.checkouts.items():
        res = cli_index(run, co.path, "setup")
        co.project = res["project"]
        run.live_projects.add(co.project)
    log("live projects: %s; decoys: %s" % (sorted(run.live_projects), sorted(run.decoy_names)))
    a.snap_home = tree(a.home)
    a.snap_tmp = tree(a.tmpdir)
    a.snap_untracked = {k: untracked(co.path) for k, co in run.checkouts.items()}


def settle(run, servers):
    """Mutator stopped: every checkout must reach index_matches_checkout."""
    a = run.a
    rng = random.Random("%s-settle" % a.seed)
    out = {}
    for key, co in run.checkouts.items():
        srv = next(s for s in servers if s.cwd == co.path)
        if srv.p is None:
            srv.start()
        deadline = time.monotonic() + a.settle_timeout
        matched = None
        verified = None
        while time.monotonic() < deadline:
            res = server_cycle(run, srv, co, rng, settle=True)
            text, st, _ = srv.tool("index_status", {"project": co.project})
            g = (st or {}).get("git") or {}
            if res in ("ok", "wrong", "stale"):
                verified = res
            if g.get("index_matches_checkout") is True and verified:
                matched = round(time.monotonic() - (deadline - a.settle_timeout), 1)
                break
            time.sleep(1)
        out[key] = {"checkout": co.branch, "matched_after_s": matched, "verified": verified,
                    "git": {k: g.get(k) for k in ("branch", "indexed_branch",
                                                    "index_matches_checkout",
                                                    "parked_branches")}}
        if matched is None:
            run.violate("settle", "settle", "%s never reached index_matches_checkout "
                        "(last %s)" % (co.path, g))
        elif verified != "ok":
            run.violate("settle", "settle", "%s final query %s" % (co.path, verified))
    return out


def final_checks(run):
    a = run.a
    res = {}
    scratch = []
    for root, _, files in os.walk(a.cache):
        for f in files:
            if SCRATCH_RE.search(f):
                p = os.path.join(root, f)
                scratch.append((os.path.relpath(p, a.cache), int(time.time() - os.stat(p).st_mtime)))
    res["scratch"] = scratch
    for p, age in scratch:
        if a.kill_rate:
            run.event("final", "scratch_after_kills", path=p, age=age)
        else:
            run.violate("final", "scratch", "%s left in the cache (age %ds)" % (p, age))
    home_new = sorted(tree(a.home) - a.snap_home)
    tmp_new = sorted(tree(a.tmpdir) - a.snap_tmp)
    res["home_new"] = home_new
    res["tmp_new"] = tmp_new
    if home_new:
        run.violate("final", "outside", "new files in HOME: %s" % home_new[:10])
    if tmp_new:
        run.violate("final", "outside", "new files in TMPDIR: %s" % tmp_new[:10])
    for k, co in run.checkouts.items():
        new = sorted(untracked(co.path) - a.snap_untracked[k])
        res["untracked_new_" + k] = new
        if new:
            run.violate("final", "outside", "new untracked files in %s: %s" % (co.path, new[:10]))
    for proj in run.live_projects:
        if not os.path.exists(os.path.join(a.cache, proj + ".db")):
            run.violate("final", "trash", "live project %s.db is missing at the end" % proj)
    res["decoy_trashed"] = dict(run.decoy_trashed)
    for proj in run.decoy_names:
        if not run.decoy_trashed.get(proj):
            run.violate("final", "trash", "decoy %s never expired to the trash" % proj)
    # Supervised index workers and the cache's own logs.
    for root, _, files in os.walk(a.cache):
        for f in files:
            if f.endswith(".log"):
                with open(os.path.join(root, f), errors="replace") as fh:
                    run.scan_sanitizer("cache-log:" + f, fh.read())
    locks = sorted(n for n in os.listdir(a.cache) if n.endswith(".lock"))
    res["lock_files"] = locks
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--out", required=True, help="fresh directory (must not exist)")
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--servers", type=int, default=4, help="servers in the main checkout")
    ap.add_argument("--wt-servers", type=int, default=2, help="servers in the worktree")
    ap.add_argument("--seed", default=None)
    ap.add_argument("--keep", type=int, default=1, help="branch_index_keep (1 forces eviction)")
    ap.add_argument("--min-stable", type=float, default=2.0)
    ap.add_argument("--max-stable", type=float, default=15.0)
    ap.add_argument("--sweep-pause", type=float, default=2.0)
    ap.add_argument("--server-pause", type=float, default=0.5)
    ap.add_argument("--call-timeout", type=float, default=300)
    ap.add_argument("--settle-timeout", type=float, default=300)
    ap.add_argument("--no-hooks", action="store_true")
    ap.add_argument("--kill-rate", type=float, default=0.0,
                    help="chance per server cycle of a SIGKILL mid index_repository; scratch "
                         "files left by a killed park/restore are then reported, not failed")
    a = ap.parse_args()
    a.bin = os.path.abspath(a.bin)
    a.root = os.path.abspath(a.out)
    os.makedirs(a.root, exist_ok=False)
    a.seed = a.seed or str(random.SystemRandom().randrange(1 << 32))
    a.home = os.path.join(a.root, "home")
    a.tmpdir = os.path.join(a.root, "tmp")
    a.cache = os.path.join(a.root, "cache")
    a.logs = os.path.join(a.root, "logs")
    os.makedirs(a.logs)
    log("seed %s, out %s" % (a.seed, a.root))
    run = Run(a)
    setup(run)

    mon = threading.Thread(target=monitor, args=(run,), daemon=True)
    mon.start()
    servers = []
    threads = []

    def spawn(name, co):
        holder = {}

        def body():
            holder["srv"] = server_loop(run, name, co)
        th = threading.Thread(target=body, name=name, daemon=True)
        th.start()
        threads.append((th, holder))

    for i in range(a.servers):
        spawn("srv-main-%d" % i, run.checkouts["main"])
    for i in range(a.wt_servers):
        spawn("srv-wt-%d" % i, run.checkouts["wt"])
    aux = [threading.Thread(target=mutator, args=(run,), daemon=True),
           threading.Thread(target=sweeper, args=(run,), daemon=True)]
    if not a.no_hooks:
        aux.append(threading.Thread(target=hooks, args=(run,), daemon=True))
    for t in aux:
        t.start()
    end = time.monotonic() + a.minutes * 60
    while time.monotonic() < end:
        time.sleep(min(30, max(0.1, end - time.monotonic())))
        log("t=%.0fs counts %s violations %d" % (run.now(), json.dumps(run.counts, sort_keys=True),
                                                  len(run.violations)))
    log("stopping the mutator, sweeper and hooks")
    run.stop_mut.set()
    run.stop.set()
    for t in aux:
        t.join(a.call_timeout + 30)
    for th, _ in threads:
        th.join(a.call_timeout + 30)
    servers = [h["srv"] for _, h in threads if "srv" in h]
    log("settling")
    settled = settle(run, servers)
    for s in servers:
        s.close()
    run.monitor_final = True
    mon.join(10)
    log("final checks")
    final = final_checks(run)
    summary = {"seed": a.seed, "minutes": a.minutes, "servers": a.servers,
               "wt_servers": a.wt_servers, "bin": a.bin, "counts": run.counts,
               "settle": settled, "final": final, "violations": run.violations,
               "live_projects": sorted(run.live_projects), "decoys": sorted(run.decoy_names),
               "sanitizer": sorted(set(run.sanitizer))}
    with open(os.path.join(a.root, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1, default=str)
    by = {}
    for v in run.violations:
        by[v["kind"]] = by.get(v["kind"], 0) + 1
    print(json.dumps({"seed": a.seed, "counts": run.counts, "violations": by,
                      "settle": settled, "decoy_trashed": final["decoy_trashed"]},
                     indent=1, sort_keys=True))
    print("RESULT: %s (%d violations) — details in %s" % (
        "PASS" if not run.violations else "FAIL", len(run.violations),
        os.path.join(a.root, "summary.json")))
    return 1 if run.violations else 0


if __name__ == "__main__":
    sys.exit(main())
