#!/usr/bin/env python3
################################################################################
# git_benchmark_parallel_phase_dstat.py
#
# Parallel Git aging benchmark with per-phase dstat summaries.
#
# This version restores phase_summary.tsv style output:
#   round_start round_end target phase wall_sec stat_device read_mib write_mib
#   read_mib_s write_mib_s read_ios write_ios dstat_csv
#
# Phase model per pulls_per_test range, matching the original Git-Benchmark role:
#   1. Measure aged git_pull for commits [round_start, round_end].
#      In per-core mode, thread N pulls from <src_repo_root>/<prefix>N.
#   2. Cold-cache/remount and measure aged grep.
#   3. Prepare a freshly formatted unaged filesystem.
#   4. Copy the aged repos after the pulls into the unaged filesystem.
#   5. Cold-cache/remount and measure unaged grep.
#
# There is intentionally NO unaged git_pull phase.  The unaged result is the
# grep result on a freshly formatted copy of the aged repository state.
#
# The regular output_file still contains per-thread grep times:
#   pull thread size_kib aged_sec fresh_sec aged_sec_per_gib fresh_sec_per_gib
################################################################################

import argparse
import csv
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time


####################
# globals

print_lock = threading.Lock()
output_lock = threading.Lock()
phase_summary_lock = threading.Lock()
error_lock = threading.Lock()

stop_event = threading.Event()
first_error = None

phase_barrier = None
output_file = None
phase_summary_file = None

src_repo = None
src_repo_prefix = None
src_repos = []
dest = None
output_file_path = None
phase_summary_path = None
fresh_mount = None
aged_blkdev = None
fresh_blkdev = None

total_pulls = None
pulls_per_test = None
num_threads = None
fs_type = None
grep_pattern = None
workspace = None
mount_opts = None
cache_clear_method = None
mkfs_cmd = None

repo_name = None
aged_root = None
fresh_root = None
thread_output_dir = None
dest_repos = []
rev_list = []

run_tag = None
dstat_dir = None
dstat_cmd = None
dstat_devices = []
dstat_interval = 1
dstat_start_delay = 1.0
disable_dstat = False
include_first_dstat_sample = False
aged_stat_device = None
fresh_stat_device = None


####################
# command helpers

def cmd_to_str(cmd):
    if isinstance(cmd, str):
        return cmd
    return " ".join(shlex.quote(str(x)) for x in cmd)


def normalize_cmd(cmd):
    if isinstance(cmd, str):
        return shlex.split(cmd)
    return [str(x) for x in cmd]


def run_cmd(cmd, cwd=None, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=True):
    cmd = normalize_cmd(cmd)
    result = subprocess.run(
        cmd,
        cwd=cwd,
        stdout=stdout,
        stderr=stderr,
        check=False,
        text=True
    )

    if check and result.returncode != 0:
        msg = "command failed rc={}: {}".format(result.returncode, cmd_to_str(cmd))
        if cwd is not None:
            msg += "  cwd={}".format(cwd)

        if isinstance(result.stderr, str) and result.stderr.strip():
            msg += "\nstderr:\n{}".format(result.stderr[-4000:])

        if isinstance(result.stdout, str) and result.stdout.strip():
            msg += "\nstdout:\n{}".format(result.stdout[-4000:])

        raise RuntimeError(msg)

    return result


def check_output(cmd, cwd=None):
    result = run_cmd(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return result.stdout.strip()


####################
# error/barrier helpers

def abort_barrier():
    global phase_barrier
    if phase_barrier is not None:
        try:
            phase_barrier.abort()
        except Exception:
            pass


def fail(msg):
    global first_error
    with error_lock:
        if first_error is None:
            first_error = msg
            stop_event.set()
            print("\nERROR: {}".format(msg), flush=True)
            abort_barrier()


def wait_all(label):
    if stop_event.is_set():
        raise RuntimeError("stop requested before barrier: {}".format(label))

    try:
        phase_barrier.wait()
    except threading.BrokenBarrierError:
        raise


####################
# filesystem helpers

def is_mountpoint(path):
    result = subprocess.run(
        ["mountpoint", "-q", path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    return result.returncode == 0


def umount_if_mounted(path):
    if is_mountpoint(path):
        run_cmd(["umount", path])


def mount_fs(dev, path):
    os.makedirs(path, exist_ok=True)

    cmd = ["mount", "-t", fs_type]
    if mount_opts:
        cmd += ["-o", mount_opts]
    cmd += [dev, path]

    run_cmd(cmd)


def mkfs_fs(dev):
    if fs_type not in ("f2fs", "delfs"):
        raise RuntimeError(
            "mkfs is supported only for f2fs/delfs in this script. "
            "Current fs_type={}".format(fs_type)
        )

    run_cmd([mkfs_cmd, "-f", dev])


def drop_caches_once():
    # This is intentionally called only by thread 0, never by every worker.
    run_cmd(["sync"])
    with open("/proc/sys/vm/drop_caches", "w") as f:
        f.write("3\n")


def clear_cache_or_remount(label, mount_path, blkdev, pull):
    if cache_clear_method == "none":
        with print_lock:
            print("[round {}] {} cache clear skipped".format(pull, label), flush=True)
        return

    if cache_clear_method == "drop_caches":
        drop_caches_once()
        with print_lock:
            print("[round {}] {} cache cleared by one global drop_caches".format(pull, label), flush=True)
        return

    if cache_clear_method == "remount":
        umount_if_mounted(mount_path)
        mount_fs(blkdev, mount_path)
        with print_lock:
            print("[round {}] {} remounted for cold-cache measurement".format(pull, label), flush=True)
        return

    raise RuntimeError("unknown cache_clear_method={}".format(cache_clear_method))


def path_is_inside(path, parent):
    path = os.path.realpath(path)
    parent = os.path.realpath(parent)
    try:
        return os.path.commonpath([path, parent]) == parent
    except ValueError:
        return False


def safe_rmtree_under(path, parent):
    path_abs = os.path.realpath(path)
    parent_abs = os.path.realpath(parent)

    if path_abs == parent_abs or not path_is_inside(path_abs, parent_abs):
        raise RuntimeError("refusing to remove unsafe path: {}".format(path))

    if os.path.exists(path_abs):
        shutil.rmtree(path_abs)


def validate_workspace_name(name):
    if name is None or name.strip() == "":
        raise RuntimeError("--workspace must not be empty")

    if os.path.isabs(name):
        raise RuntimeError("--workspace must be a relative directory name, not an absolute path")

    parts = name.split("/")
    bad_parts = ("", ".", "..")
    for part in parts:
        if part in bad_parts:
            raise RuntimeError("--workspace contains an unsafe path component: {}".format(name))


####################
# benchmark helpers

def du_kib(path):
    out = check_output(["du", "-s", path])
    return int(out.split()[0])


def grep_time_sec(path, pattern):
    start = time.perf_counter()
    subprocess.run(
        ["grep", "-r", "-F", pattern, path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
        text=True
    )
    end = time.perf_counter()
    return end - start


def sec_per_gib(size_kib, sec):
    size_gib = float(size_kib) / 1024.0 / 1024.0
    if size_gib == 0:
        return 0.0
    return sec / size_gib


def parse_git_version(s):
    m = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", s)
    if not m:
        return (0, 0, 0)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3) or 0))


def git_pull_commit(repo_path, pull_number, tid):
    source_repo = src_repos[tid]
    git_pull_cmd = [
        "git", "pull",
        "--no-rebase",
        "--no-edit",
        "-q",
        "-s", "recursive",
        "-X", "theirs",
        source_repo,
        rev_list[pull_number - 1].strip()
    ]
    run_cmd(git_pull_cmd, cwd=repo_path)


####################
# dstat helpers

def blkdev_to_stat_device(dev):
    """Convert /dev/nvme5n1p1 -> nvme5n1, /dev/sda1 -> sda."""
    base = os.path.basename(str(dev))

    m = re.match(r"^(nvme\d+n\d+)p\d+$", base)
    if m:
        return m.group(1)

    m = re.match(r"^(mmcblk\d+)p\d+$", base)
    if m:
        return m.group(1)

    m = re.match(r"^((?:sd|vd|xvd|hd)[a-z]+)\d+$", base)
    if m:
        return m.group(1)

    return base


def sanitize_tag(s):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", str(s)).strip("_") or "run"


def unique_numbered_path(directory, basename, ext):
    os.makedirs(directory, exist_ok=True)
    for i in range(1, 1000000):
        p = os.path.join(directory, "{}_{}.{}".format(basename, i, ext))
        if not os.path.exists(p):
            return p
    raise RuntimeError("could not allocate unique filename for {}".format(basename))


def dstat_csv_filename(round_start, round_end, target, phase):
    base = "{}_r{:06d}-{:06d}_{}_{}".format(
        sanitize_tag(run_tag),
        round_start,
        round_end,
        sanitize_tag(target),
        sanitize_tag(phase)
    )
    return unique_numbered_path(dstat_dir, base, "csv")


def parse_dstat_value(s):
    if s is None:
        return 0.0
    s = str(s).strip().strip('"').strip()
    if s == "" or s == "-":
        return 0.0
    s = s.replace(",", "")

    # Common dstat suffixes: B, k/K, M, G, T, P.  Some dstat
    # versions also emit scientific notation or a trailing "/s".
    s = re.sub(r"/s$", "", s, flags=re.IGNORECASE)
    m = re.match(
        r"^([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)(?:\s*([A-Za-z]+))?$",
        s
    )
    if not m:
        return 0.0

    value = float(m.group(1))
    suffix = (m.group(2) or "").lower()
    mult = 1.0
    if suffix in ("b", "byte", "bytes"):
        mult = 1.0
    elif suffix in ("k", "kb", "kib"):
        mult = 1024.0
    elif suffix in ("m", "mb", "mib"):
        mult = 1024.0 ** 2
    elif suffix in ("g", "gb", "gib"):
        mult = 1024.0 ** 3
    elif suffix in ("t", "tb", "tib"):
        mult = 1024.0 ** 4
    elif suffix in ("p", "pb", "pib"):
        mult = 1024.0 ** 5
    return value * mult


def normalize_csv_cell(cell):
    return str(cell).strip().strip('"').strip()


def dstat_header_group_metric(header_cell, inherited_group=""):
    """Normalize dstat CSV header cells.

    dstat CSV headers differ across versions.  Common forms are:

        row N-1: ..., "dsk/nvme5n1",,
        row N:   ..., "read","writ",

    and dstat 0.8.0 often emits:

        row N-1: ..., "dsk/nvme5n1",,
        row N:   ..., "dsk/nvme5n1:read","dsk/nvme5n1:writ",

    This returns ("dsk/nvme5n1", "read") for both.
    """
    h = normalize_csv_cell(header_cell).lower()
    if ":" in h:
        group, metric = h.rsplit(":", 1)
        if group.startswith(("dsk/", "io/")):
            return group, metric

    return normalize_csv_cell(inherited_group).lower(), h


def dstat_metric_name(cell):
    c = normalize_csv_cell(cell).lower()
    if ":" in c:
        c = c.rsplit(":", 1)[1]
    return c


def dstat_cell_is_read(cell):
    c = dstat_metric_name(cell)
    return c.startswith("read") or c in ("recv", "rx")


def dstat_cell_is_write(cell):
    c = dstat_metric_name(cell)
    return c.startswith("writ") or c.startswith("write") or c in ("send", "tx")


def dstat_row_looks_like_metric_header(row):
    return any(dstat_cell_is_read(c) for c in row) and any(dstat_cell_is_write(c) for c in row)


def parse_dstat_csv(csv_path, stat_device, wall_sec):
    """Return aggregate MiB and IO counts for stat_device from a dstat CSV."""
    zero = {
        "read_mib": 0.0,
        "write_mib": 0.0,
        "read_mib_s": 0.0,
        "write_mib_s": 0.0,
        "read_ios": 0,
        "write_ios": 0,
    }

    if disable_dstat or not csv_path or not os.path.exists(csv_path):
        return zero

    try:
        with open(csv_path, newline="") as f:
            rows = list(csv.reader(f))
    except Exception:
        return zero

    # Do not require a "time" or "epoch" column.  dstat 0.8.0 may emit a
    # metric header that starts directly with "usr,sys,..." and contains
    # cells like "dsk/nvme5n1:read".
    header_idx = None
    for i, row in enumerate(rows):
        if dstat_row_looks_like_metric_header(row):
            header_idx = i
            break

    if header_idx is None:
        with print_lock:
            print("WARNING: could not find dstat metric header in {}".format(csv_path), flush=True)
        return zero

    top = [normalize_csv_cell(c) for c in (rows[header_idx - 1] if header_idx > 0 else [])]
    header = [normalize_csv_cell(c) for c in rows[header_idx]]

    width = max(len(top), len(header))
    groups = []
    cur_group = ""
    for i in range(width):
        g = top[i] if i < len(top) else ""
        if g:
            cur_group = g
        groups.append(cur_group)

    dev = str(stat_device).lower()
    dsk_group = "dsk/{}".format(dev)
    io_group = "io/{}".format(dev)

    read_b_col = None
    write_b_col = None
    read_io_col = None
    write_io_col = None
    seen_dsk_groups = []
    seen_io_groups = []

    for i in range(width):
        h_cell = header[i] if i < len(header) else ""
        inherited = groups[i] if i < len(groups) else ""
        g, h = dstat_header_group_metric(h_cell, inherited)

        if g.startswith("dsk/") and g not in seen_dsk_groups:
            seen_dsk_groups.append(g)
        if g.startswith("io/") and g not in seen_io_groups:
            seen_io_groups.append(g)

        if g == dsk_group:
            if h.startswith("read") and read_b_col is None:
                read_b_col = i
            elif (h.startswith("writ") or h.startswith("write")) and write_b_col is None:
                write_b_col = i

        if g == io_group:
            if h.startswith("read") and read_io_col is None:
                read_io_col = i
            elif (h.startswith("writ") or h.startswith("write")) and write_io_col is None:
                write_io_col = i

    if read_b_col is None and write_b_col is None:
        with print_lock:
            print(
                "WARNING: dstat device {} not found in {}. dsk groups present: {}".format(
                    stat_device, csv_path, ",".join(seen_dsk_groups) if seen_dsk_groups else "(none)"
                ),
                flush=True
            )
        return zero

    byte_samples = []
    io_samples = []

    for row in rows[header_idx + 1:]:
        if not row:
            continue

        # Skip repeated headers or metadata.
        if dstat_row_looks_like_metric_header(row):
            continue
        first = normalize_csv_cell(row[0]).lower() if row else ""
        if first.startswith(("dstat", "author:", "host:", "cmdline:")):
            continue

        have_byte = False
        read_bps = 0.0
        write_bps = 0.0
        if read_b_col is not None and read_b_col < len(row):
            read_bps = parse_dstat_value(row[read_b_col])
            have_byte = True
        if write_b_col is not None and write_b_col < len(row):
            write_bps = parse_dstat_value(row[write_b_col])
            have_byte = True
        if have_byte:
            byte_samples.append((read_bps, write_bps))

        have_io = False
        read_iops = 0.0
        write_iops = 0.0
        if read_io_col is not None and read_io_col < len(row):
            read_iops = parse_dstat_value(row[read_io_col])
            have_io = True
        if write_io_col is not None and write_io_col < len(row):
            write_iops = parse_dstat_value(row[write_io_col])
            have_io = True
        if have_io:
            io_samples.append((read_iops, write_iops))

    if not include_first_dstat_sample:
        if len(byte_samples) > 1:
            byte_samples = byte_samples[1:]
        if len(io_samples) > 1:
            io_samples = io_samples[1:]

    read_mib_s = 0.0
    write_mib_s = 0.0
    if byte_samples:
        avg_read_bps = sum(x[0] for x in byte_samples) / len(byte_samples)
        avg_write_bps = sum(x[1] for x in byte_samples) / len(byte_samples)
        read_mib_s = avg_read_bps / 1024.0 / 1024.0
        write_mib_s = avg_write_bps / 1024.0 / 1024.0

    read_ios = 0
    write_ios = 0
    if io_samples:
        avg_read_iops = sum(x[0] for x in io_samples) / len(io_samples)
        avg_write_iops = sum(x[1] for x in io_samples) / len(io_samples)
        read_ios = int(round(avg_read_iops * wall_sec))
        write_ios = int(round(avg_write_iops * wall_sec))

    return {
        "read_mib": read_mib_s * wall_sec,
        "write_mib": write_mib_s * wall_sec,
        "read_mib_s": read_mib_s,
        "write_mib_s": write_mib_s,
        "read_ios": read_ios,
        "write_ios": write_ios,
    }


def start_dstat(csv_path):
    if disable_dstat:
        return None

    if not shutil.which(dstat_cmd):
        raise RuntimeError("dstat command not found: {}".format(dstat_cmd))

    devices_arg = ",".join(dstat_devices)
    cmd = [
        dstat_cmd,
        "-t",
        "--disk",
        "--io",
        "-D", devices_arg,
        "--output", csv_path,
        str(dstat_interval),
    ]

    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=os.setsid,
    )

    if dstat_start_delay > 0:
        time.sleep(dstat_start_delay)

    if proc.poll() is not None:
        try:
            _, err = proc.communicate(timeout=1)
        except Exception:
            err = ""
        raise RuntimeError("dstat failed to start: {}\n{}".format(cmd_to_str(cmd), err))

    return proc


def stop_dstat(proc):
    if proc is None:
        return

    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGINT)
    except Exception:
        try:
            proc.terminate()
        except Exception:
            pass

    try:
        proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        try:
            proc.communicate(timeout=2)
        except Exception:
            pass


def phase_begin(round_start, round_end, target, phase, stat_device):
    csv_path = ""
    proc = None
    if not disable_dstat:
        csv_path = os.path.abspath(dstat_csv_filename(round_start, round_end, target, phase))
        proc = start_dstat(csv_path)

    return {
        "round_start": round_start,
        "round_end": round_end,
        "target": target,
        "phase": phase,
        "stat_device": stat_device,
        "dstat_csv": csv_path,
        "proc": proc,
        "start": time.perf_counter(),
    }


def phase_end(ctx, rows=None):
    """Stop dstat and write one or more summary rows for the measured phase.

    rows may be used when one measured wall-clock phase touches two target
    devices.  For example, cp -a from aged to unaged reads aged and writes
    unaged.  In that case we stop one dstat process once, parse the same CSV
    for both devices, and write two rows with the same wall_sec and dstat_csv.
    """
    wall_sec = time.perf_counter() - ctx["start"]
    stop_dstat(ctx["proc"])

    if rows is None:
        rows = [(ctx["target"], ctx["phase"], ctx["stat_device"])]

    last_stats = None
    for target, phase, stat_device in rows:
        stats = parse_dstat_csv(ctx["dstat_csv"], stat_device, wall_sec)
        last_stats = stats

        line = (
            "{round_start}\t{round_end}\t{target}\t{phase}\t"
            "{wall_sec:.6f}\t{stat_device}\t"
            "{read_mib:.5f}\t{write_mib:.5f}\t"
            "{read_mib_s:.6f}\t{write_mib_s:.6f}\t"
            "{read_ios}\t{write_ios}\t{dstat_csv}\n"
        ).format(
            round_start=ctx["round_start"],
            round_end=ctx["round_end"],
            target=target,
            phase=phase,
            wall_sec=wall_sec,
            stat_device=stat_device,
            dstat_csv=ctx["dstat_csv"],
            **stats
        )

        with phase_summary_lock:
            phase_summary_file.write(line)
            phase_summary_file.flush()

        with print_lock:
            print(
                "[round {}-{}] {} {} wall={:.3f}s read={:.2f}MiB/s write={:.2f}MiB/s".format(
                    ctx["round_start"],
                    ctx["round_end"],
                    target,
                    phase,
                    wall_sec,
                    stats["read_mib_s"],
                    stats["write_mib_s"],
                ),
                flush=True
            )

    return wall_sec, last_stats



####################
# round helpers

def prepare_fresh_round(round_end):
    # Fresh/unaged device is fully recreated after the aged grep for this range.
    umount_if_mounted(fresh_mount)
    mkfs_fs(fresh_blkdev)
    mount_fs(fresh_blkdev, fresh_mount)

    if os.path.exists(fresh_root):
        safe_rmtree_under(fresh_root, fresh_mount)
    os.makedirs(fresh_root, exist_ok=True)

    for tid in range(num_threads):
        os.makedirs(os.path.join(fresh_root, "thread_{:03d}".format(tid)), exist_ok=True)

    with print_lock:
        print("[round {}] fresh/unaged device prepared".format(round_end), flush=True)


def finish_fresh_round(round_end):
    umount_if_mounted(fresh_mount)
    with print_lock:
        print("[round {}] fresh/unaged device unmounted".format(round_end), flush=True)


####################
# worker

def worker(tid):
    try:
        dest_repo = dest_repos[tid]
        per_thread_output = os.path.join(thread_output_dir, "thread_{:03d}.txt".format(tid))

        current_pull = 0
        while current_pull < total_pulls:
            if stop_event.is_set():
                return

            round_start = current_pull + 1
            round_end = min(current_pull + pulls_per_test, total_pulls)

            # 1) Aged git_pull phase.  This is the only git-pull workload.
            #    Thread tid pulls from src_repos[tid] into its aged dest_repo.
            if tid == 0:
                ctx = phase_begin(round_start, round_end, "aged", "git_pull", aged_stat_device)
            else:
                ctx = None
            wait_all("round {}-{} start aged git_pull".format(round_start, round_end))

            for pull in range(round_start, round_end + 1):
                git_pull_commit(dest_repo, pull, tid)

            wait_all("round {}-{} end aged git_pull".format(round_start, round_end))
            if tid == 0:
                phase_end(ctx)
            wait_all("round {}-{} after aged git_pull summary".format(round_start, round_end))

            # Compute size before cold-cache preparation so du does not warm
            # the cache used by the measured grep.
            size_kib = du_kib(dest_repo)
            wait_all("round {}-{} after du".format(round_start, round_end))

            # 2) Aged grep phase after cache preparation/remount.
            if tid == 0:
                clear_cache_or_remount("aged", dest, aged_blkdev, round_end)
            wait_all("round {}-{} after aged cache clear/remount".format(round_start, round_end))

            if tid == 0:
                ctx = phase_begin(round_start, round_end, "aged", "grep", aged_stat_device)
            else:
                ctx = None
            wait_all("round {}-{} start aged grep".format(round_start, round_end))

            aged_sec = grep_time_sec(dest_repo, grep_pattern)

            wait_all("round {}-{} end aged grep".format(round_start, round_end))
            if tid == 0:
                phase_end(ctx)
            wait_all("round {}-{} after aged grep summary".format(round_start, round_end))

            # 3) Prepare fresh/unaged filesystem AFTER the aged pulls and aged grep.
            #    This phase is measured separately because mkfs can create large writes.
            if tid == 0:
                ctx = phase_begin(round_start, round_end, "unaged", "mkfs_mount", fresh_stat_device)
                prepare_fresh_round(round_end)
                phase_end(ctx)
            wait_all("round {}-{} after unaged mkfs/mount".format(round_start, round_end))

            # 4) Copy the aged repo state into the freshly formatted unaged filesystem.
            #    This is the README-style unaged setup: unaged does NOT run git pull.
            fresh_thread_dir = os.path.join(fresh_root, "thread_{:03d}".format(tid))

            if tid == 0:
                ctx = phase_begin(round_start, round_end, "copy", "aged_to_unaged", fresh_stat_device)
            else:
                ctx = None
            wait_all("round {}-{} start aged-to-unaged copy".format(round_start, round_end))

            run_cmd(["cp", "-a", dest_repo, fresh_thread_dir])
            fresh_repo = os.path.join(fresh_thread_dir, repo_name)

            wait_all("round {}-{} end aged-to-unaged copy".format(round_start, round_end))
            if tid == 0:
                # Include delayed writeback from cp in the copy phase rather than
                # leaking it into the following unaged grep measurement.
                run_cmd(["sync"])
                # Same wall-clock/csv, summarized for both devices:
                #   aged   : source-side reads during cp
                #   unaged : destination-side writes during cp
                phase_end(ctx, rows=[
                    ("aged", "copy_aged_to_unaged", aged_stat_device),
                    ("unaged", "copy_aged_to_unaged", fresh_stat_device),
                ])
                with print_lock:
                    print("[round {}-{}] aged state copied to unaged and synced".format(round_start, round_end), flush=True)
            wait_all("round {}-{} after aged-to-unaged copy summary".format(round_start, round_end))

            # 5) Unaged grep phase.  This is grep on the freshly formatted copy
            #    of the aged state, not a repo that performed git pull.
            if tid == 0:
                clear_cache_or_remount("unaged", fresh_mount, fresh_blkdev, round_end)
            wait_all("round {}-{} after unaged cache clear/remount".format(round_start, round_end))

            if tid == 0:
                ctx = phase_begin(round_start, round_end, "unaged", "grep", fresh_stat_device)
            else:
                ctx = None
            wait_all("round {}-{} start unaged grep".format(round_start, round_end))

            fresh_sec = grep_time_sec(fresh_repo, grep_pattern)

            wait_all("round {}-{} end unaged grep".format(round_start, round_end))
            if tid == 0:
                phase_end(ctx)
            wait_all("round {}-{} after unaged grep summary".format(round_start, round_end))

            aged_sec_per_gib = sec_per_gib(size_kib, aged_sec)
            fresh_sec_per_gib = sec_per_gib(size_kib, fresh_sec)

            output_line = "{} {} {} {:.6f} {:.6f} {:.6f} {:.6f}\n".format(
                round_end,
                tid,
                size_kib,
                aged_sec,
                fresh_sec,
                aged_sec_per_gib,
                fresh_sec_per_gib
            )

            with output_lock:
                output_file.write(output_line)
                output_file.flush()

                with open(per_thread_output, "a") as f:
                    f.write(output_line)

            if tid == 0:
                with print_lock:
                    print(
                        "[round {}-{}] example(thread 0): size_kib={} aged_grep={:.6f}s unaged_grep={:.6f}s".format(
                            round_start,
                            round_end,
                            size_kib,
                            aged_sec,
                            fresh_sec
                        ),
                        flush=True
                    )

            # Do not unmount fresh until all fresh grep/output writes finish.
            wait_all("round {}-{} after grep/output".format(round_start, round_end))

            if tid == 0:
                finish_fresh_round(round_end)
            wait_all("round {}-{} after fresh unmount".format(round_start, round_end))

            current_pull = round_end

    except threading.BrokenBarrierError:
        fail("barrier broken in thread {}".format(tid))
    except Exception as e:
        fail("thread {} failed: {}".format(tid, str(e)))



####################
# main

def main():
    global phase_barrier
    global output_file, phase_summary_file
    global src_repo, src_repo_prefix, src_repos, dest, output_file_path, phase_summary_path, fresh_mount, aged_blkdev, fresh_blkdev
    global total_pulls, pulls_per_test, num_threads, fs_type, grep_pattern, workspace
    global mount_opts, cache_clear_method, mkfs_cmd
    global repo_name, aged_root, fresh_root, thread_output_dir, dest_repos
    global rev_list
    global run_tag, dstat_dir, dstat_cmd, dstat_devices, dstat_interval, dstat_start_delay
    global disable_dstat, include_first_dstat_sample, aged_stat_device, fresh_stat_device

    parser = argparse.ArgumentParser()
    parser.add_argument("src_repo", help="source repository, or source repository root when --src_repo_prefix is used")
    parser.add_argument("dest", help="destination location to be aged; mounted aged filesystem")
    parser.add_argument("output_file", help="combined output file")
    parser.add_argument("total_pulls", help="number of pulls per worker thread", type=int)
    parser.add_argument("pulls_per_test", help="run phase summary/grep every this many pulls", type=int)
    parser.add_argument("fresh_mount", help="fresh/unaged shared mount path")
    parser.add_argument("aged_blkdev", help="aged block device")
    parser.add_argument("fresh_blkdev", help="fresh/unaged block device")
    parser.add_argument("--threads", help="number of worker threads", type=int, default=128)
    parser.add_argument("--fs_type", help="filesystem type for mount", default="f2fs")
    parser.add_argument("--grep_pattern", help="pattern for grep", default="t26EdaovJD")
    parser.add_argument("--workspace", help="subdirectory under mount points", default="benchmark_parallel")
    parser.add_argument(
        "--src_repo_prefix",
        default="",
        help=(
            "Enable per-thread source repositories. When set, positional src_repo "
            "is treated as a root directory and thread N pulls from "
            "<src_repo>/<src_repo_prefix>N, e.g. /mnt/git_bench_linux/linux0."
        )
    )
    parser.add_argument(
        "--cache_clear_method",
        choices=["remount", "drop_caches", "none"],
        default="remount",
        help=(
            "cold-cache method for grep phases. "
            "remount: unmount/mount target FS once per grep phase; "
            "drop_caches: one global sync/drop_caches by thread 0; "
            "none: no cache clearing"
        )
    )
    parser.add_argument(
        "--mount_opts",
        default="",
        help="optional mount options, e.g. checkpoint_merge,flush_merge"
    )
    parser.add_argument(
        "--mkfs_cmd",
        default="mkfs.f2fs",
        help="mkfs command used for the fresh device; default: mkfs.f2fs"
    )
    parser.add_argument(
        "--run_tag",
        default="",
        help="prefix for dstat csv filenames; default is output filename stem"
    )
    parser.add_argument(
        "--dstat_dir",
        default="",
        help="directory for per-phase dstat csv files; default: <output_file>.dstat"
    )
    parser.add_argument(
        "--dstat_devices",
        default="",
        help="comma-separated dstat device list, e.g. nvme5n1,nvme0n1,total"
    )
    parser.add_argument(
        "--dstat_cmd",
        default="dstat",
        help="dstat command path/name"
    )
    parser.add_argument(
        "--dstat_interval",
        default=1,
        type=int,
        help="dstat sampling interval in seconds"
    )
    parser.add_argument(
        "--dstat_start_delay",
        default=1.0,
        type=float,
        help="seconds to wait after starting dstat before phase begins"
    )
    parser.add_argument(
        "--disable_dstat",
        action="store_true",
        help="write phase_summary.tsv with wall_sec only; no dstat csv/stat parsing"
    )
    parser.add_argument(
        "--include_first_dstat_sample",
        action="store_true",
        help="include first dstat sample when averaging; default skips first sample"
    )

    args = parser.parse_args()

    if os.geteuid() != 0:
        print("This script must be run as root.", flush=True)
        sys.exit(1)

    src_repo = os.path.abspath(args.src_repo)
    dest = os.path.abspath(args.dest)
    output_file_path = os.path.abspath(args.output_file)
    phase_summary_path = output_file_path + ".phase_summary.tsv"
    fresh_mount = os.path.abspath(args.fresh_mount)
    aged_blkdev = args.aged_blkdev
    fresh_blkdev = args.fresh_blkdev

    total_pulls = args.total_pulls
    pulls_per_test = args.pulls_per_test
    num_threads = args.threads
    fs_type = args.fs_type
    grep_pattern = args.grep_pattern
    workspace = args.workspace
    src_repo_prefix = args.src_repo_prefix
    mount_opts = args.mount_opts
    cache_clear_method = args.cache_clear_method

    run_tag = args.run_tag or os.path.splitext(os.path.basename(output_file_path))[0]
    dstat_dir = os.path.abspath(args.dstat_dir or (output_file_path + ".dstat"))
    dstat_cmd = args.dstat_cmd
    dstat_interval = args.dstat_interval
    dstat_start_delay = args.dstat_start_delay
    disable_dstat = args.disable_dstat
    include_first_dstat_sample = args.include_first_dstat_sample

    aged_stat_device = blkdev_to_stat_device(aged_blkdev)
    fresh_stat_device = blkdev_to_stat_device(fresh_blkdev)

    if args.dstat_devices.strip():
        dstat_devices = [x.strip() for x in args.dstat_devices.split(",") if x.strip()]
    else:
        dstat_devices = [aged_stat_device, fresh_stat_device, "total"]

    # Make sure the devices we summarize are actually collected.
    for d in (aged_stat_device, fresh_stat_device):
        if d and d not in dstat_devices:
            dstat_devices.append(d)

    # Respect an explicitly supplied --mkfs_cmd. If the user leaves it at the
    # default, choose the built-in default for the selected filesystem.
    if args.mkfs_cmd != parser.get_default("mkfs_cmd"):
        mkfs_cmd = args.mkfs_cmd
    elif fs_type == "f2fs":
        mkfs_cmd = "mkfs.f2fs"
    elif fs_type == "delfs":
        mkfs_cmd = "/home/syslab/workspace_hwan/hwan_working/DeLFS/delfs-mkfs/mkfs/mkfs.delfs"
    else:
        mkfs_cmd = args.mkfs_cmd

    if total_pulls <= 0:
        print("total_pulls must be positive.", flush=True)
        sys.exit(1)

    if pulls_per_test <= 0:
        print("pulls_per_test must be positive.", flush=True)
        sys.exit(1)

    if num_threads <= 0:
        print("--threads must be positive.", flush=True)
        sys.exit(1)

    if dstat_interval <= 0:
        print("--dstat_interval must be positive.", flush=True)
        sys.exit(1)

    try:
        validate_workspace_name(workspace)
    except Exception as e:
        print("Invalid workspace: {}".format(e), flush=True)
        sys.exit(1)

    if os.path.realpath(dest) == os.path.realpath(fresh_mount):
        print("dest and fresh_mount must be different paths.", flush=True)
        sys.exit(1)

    if aged_blkdev == fresh_blkdev:
        print("aged_blkdev and fresh_blkdev must be different devices.", flush=True)
        sys.exit(1)

    if path_is_inside(src_repo, dest) or path_is_inside(src_repo, fresh_mount):
        print("src_repo must not be inside dest or fresh_mount.", flush=True)
        sys.exit(1)

    if fs_type not in ("f2fs", "delfs"):
        print("This script currently supports fs_type f2fs or delfs only.", flush=True)
        sys.exit(1)

    if not disable_dstat and not shutil.which(dstat_cmd):
        print("dstat command not found: {}".format(dstat_cmd), flush=True)
        sys.exit(1)

    if src_repo_prefix:
        # Per-core/per-thread mode: src_repo is a root directory and each thread
        # pulls from <src_repo>/<src_repo_prefix><tid>, e.g. linux0...linux127.
        src_repos = [
            os.path.abspath(os.path.join(src_repo, "{}{}".format(src_repo_prefix, tid)))
            for tid in range(num_threads)
        ]
        repo_name = src_repo_prefix
    else:
        # Single-source mode: every worker pulls from the same source repo.
        src_repos = [src_repo for _ in range(num_threads)]
        repo_name = os.path.basename(os.path.normpath(src_repo))

    primary_src_repo = src_repos[0]

    aged_root = os.path.join(dest, workspace)
    fresh_root = os.path.join(fresh_mount, workspace)
    thread_output_dir = output_file_path + ".threads"

    phase_barrier = threading.Barrier(num_threads)

    # Check git version.
    git_version_str = check_output(["git", "--version"])
    git_version = parse_git_version(git_version_str)

    if git_version < (2, 5, 0):
        print("Git version must be 2.5+, currently {}".format(git_version_str), flush=True)
        sys.exit(1)
    else:
        print("Git version {} OK".format(".".join(map(str, git_version))), flush=True)

    # Validate source repositories. In per-thread mode this checks linux0...linuxN.
    seen_src_repos = []
    for p in src_repos:
        if p not in seen_src_repos:
            seen_src_repos.append(p)

    for p in seen_src_repos:
        if not os.path.isdir(p):
            print("source repo does not exist or is not a directory: {}".format(p), flush=True)
            sys.exit(1)

        if not os.path.isdir(os.path.join(p, ".git")):
            print("source repo does not look like a non-bare Git repository: {}".format(p), flush=True)
            sys.exit(1)

    # Ensure mount directories exist.
    os.makedirs(dest, exist_ok=True)
    os.makedirs(fresh_mount, exist_ok=True)

    # The aged filesystem is normally mounted by the caller.
    # If it is not mounted, mount it here.
    if not is_mountpoint(dest):
        print("{} is not mounted; mounting {} on it".format(dest, aged_blkdev), flush=True)
        mount_fs(aged_blkdev, dest)

    # Avoid accidental stale fresh mount from a previous run.
    umount_if_mounted(fresh_mount)

    # Prepare output files.
    os.makedirs(os.path.dirname(output_file_path) or ".", exist_ok=True)
    os.makedirs(dstat_dir, exist_ok=True)

    if os.path.exists(thread_output_dir):
        shutil.rmtree(thread_output_dir)
    os.makedirs(thread_output_dir, exist_ok=True)

    output_file = open(output_file_path, "w")
    output_file.write("pull thread size_kib aged_sec unaged_sec aged_sec_per_gib unaged_sec_per_gib\n")
    output_file.flush()

    phase_summary_file = open(phase_summary_path, "w")
    phase_summary_file.write(
        "round_start\tround_end\ttarget\tphase\twall_sec\tstat_device\t"
        "read_mib\twrite_mib\tread_mib_s\twrite_mib_s\tread_ios\twrite_ios\tdstat_csv\n"
    )
    phase_summary_file.flush()

    try:
        # Prepare source repository/repositories.
        if src_repo_prefix:
            print("Configuring per-thread source repositories under {} with prefix {}".format(src_repo, src_repo_prefix), flush=True)
        else:
            print("Configuring source repository {}".format(src_repo), flush=True)

        for p in seen_src_repos:
            run_cmd(["git", "config", "uploadpack.allowReachableSHA1InWant", "True"], cwd=p)

        # Generate commit list from thread 0's source repo. Per-thread source repos
        # are expected to contain the same first-parent commit sequence.
        print("Generating commit list", flush=True)
        rev_list = check_output(
            ["git", "rev-list", "--reverse", "--first-parent", "HEAD"],
            cwd=primary_src_repo
        ).splitlines()

        if len(rev_list) < total_pulls:
            print("Source repository does not have enough commits.", flush=True)
            print("Have {}, test requires {}".format(len(rev_list), total_pulls), flush=True)
            sys.exit(1)

        # Prepare aged destination root.
        if os.path.exists(aged_root):
            safe_rmtree_under(aged_root, dest)
        os.makedirs(aged_root, exist_ok=True)

        # Initialize one aged destination repo per worker.
        print("Initializing aged destination repositories", flush=True)
        dest_repos = []

        for tid in range(num_threads):
            thread_dir = os.path.join(aged_root, "thread_{:03d}".format(tid))
            dest_repo = os.path.abspath(os.path.join(thread_dir, repo_name))
            os.makedirs(dest_repo, exist_ok=True)

            run_cmd(["git", "init"], cwd=dest_repo)
            run_cmd(["git", "config", "user.name", "Git Benchmark"], cwd=dest_repo)
            run_cmd(["git", "config", "user.email", "git-benchmark@example.com"], cwd=dest_repo)
            run_cmd(["git", "config", "gc.auto", "0"], cwd=dest_repo)
            run_cmd(["git", "config", "gc.autodetach", "False"], cwd=dest_repo)
            run_cmd(["git", "config", "pull.rebase", "false"], cwd=dest_repo)

            per_thread_output = os.path.join(thread_output_dir, "thread_{:03d}.txt".format(tid))
            with open(per_thread_output, "w") as f:
                f.write("pull thread size_kib aged_sec unaged_sec aged_sec_per_gib unaged_sec_per_gib\n")

            dest_repos.append(dest_repo)

        print("--------------------------------------------------------------------------------", flush=True)
        if src_repo_prefix:
            print("Parallel Git-aging on {} from per-thread repositories {}{}<tid>".format(dest, src_repo + os.sep, src_repo_prefix), flush=True)
        else:
            print("Parallel Git-aging on {} from local repository {}".format(dest, src_repo), flush=True)
        print("Threads: {}".format(num_threads), flush=True)
        print("Pulls per worker thread: {}".format(total_pulls), flush=True)
        print("Pull/grep interval: {} pulls".format(pulls_per_test), flush=True)
        print("Aged device: {} mounted on {} ; dstat device={}".format(aged_blkdev, dest, aged_stat_device), flush=True)
        print("Unaged device: {} mounted on {} ; dstat device={}".format(fresh_blkdev, fresh_mount, fresh_stat_device), flush=True)
        print("Filesystem: {}".format(fs_type), flush=True)
        print("Mount options: {}".format(mount_opts if mount_opts else "(none)"), flush=True)
        print("Cache clear method for grep phases: {}".format(cache_clear_method), flush=True)
        print("Workspace: {}".format(workspace), flush=True)
        print("Source mode: {}".format("per-thread" if src_repo_prefix else "single"), flush=True)
        if src_repo_prefix:
            print("Source repos: {}{}0 ... {}{}{}".format(src_repo + os.sep, src_repo_prefix, src_repo + os.sep, src_repo_prefix, num_threads - 1), flush=True)
        print("Run tag: {}".format(run_tag), flush=True)
        print("Combined output: {}".format(output_file_path), flush=True)
        print("Phase summary: {}".format(phase_summary_path), flush=True)
        print("Dstat enabled: {}".format(not disable_dstat), flush=True)
        print("Dstat dir: {}".format(dstat_dir), flush=True)
        print("Dstat devices: {}".format(",".join(dstat_devices)), flush=True)
        print("--------------------------------------------------------------------------------", flush=True)

        threads = []
        for tid in range(num_threads):
            t = threading.Thread(target=worker, args=(tid,), name="worker-{:03d}".format(tid))
            t.start()
            threads.append(t)

        for t in threads:
            t.join()

        if first_error is not None:
            print("Benchmark failed.", flush=True)
            sys.exit(1)

        print("Benchmark finished successfully.", flush=True)
        print("Combined output : {}".format(output_file_path), flush=True)
        print("Phase summary   : {}".format(phase_summary_path), flush=True)
        print("Per-thread files: {}".format(thread_output_dir), flush=True)
        print("Dstat CSV dir   : {}".format(dstat_dir), flush=True)

    finally:
        if output_file is not None:
            output_file.close()
        if phase_summary_file is not None:
            phase_summary_file.close()


if __name__ == "__main__":
    main()



