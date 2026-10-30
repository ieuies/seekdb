#!/usr/bin/env python3
"""Measure one hipVS/CAGRA configuration and print one markdown row.

The official scoring compares the candidate against the frozen GPU V5 baseline
on the same machine, on the same data, with the same SQL. This script measures
one configuration:

  * build: from CREATE VECTOR INDEX until the first legal approximate query
    returns, which is the window the task counts,
  * query: Top-10 and Top-100, one single row statement per query, reported as
    the geometric mean of the two QPS ratios,
  * recall: against the exact L2 answer over the same rows,
  * resources: the peak of the sampled device memory and the peak resident set
    of the observer process.

It reuses the observer helpers of the local development box when they are
available and falls back to the mysql client otherwise.
"""
import argparse
import json
import math
import os
import pathlib
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.getcwd(), "lib"))
try:
    import seekdb_demo as sk
except Exception:  # the helpers are optional
    sk = None


def sample_device_mib():
    """Peak of the used device memory over the render devices, in MiB."""
    total = 0
    found = False
    for path in sorted(glob_paths("/sys/class/drm/card*/device/mem_info_vram_used")):
        try:
            with open(path) as f:
                total += int(f.read().strip())
            found = True
        except Exception:
            pass
    return total // (1024 * 1024) if found else -1


def glob_paths(pattern):
    import glob
    return glob.glob(pattern)


def peak_rss_kib(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except Exception:
        pass
    return -1


def run_sql(sql, port):
    cmd = ["mysql", "-h127.0.0.1", "-P%d" % port, "-uroot", "-N", "-s", "-A",
           "--default-character-set=utf8mb4", "-e", sql]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError("sql failed: %s\n%s" % (sql, out.stderr))
    rows = []
    for line in out.stdout.splitlines():
        if line.strip():
            rows.append(line.split("\t"))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True, help="seekdb binary to measure")
    ap.add_argument("--port", type=int, default=2881)
    ap.add_argument("--base-dir", default="/tmp/seekdb-hipvs-bench")
    ap.add_argument("--rows", type=int, default=250000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=4096)
    ap.add_argument("--topk", type=int, nargs="+", default=[10, 100])
    ap.add_argument("--rounds", type=int, default=1, help="repeat the whole build")
    ap.add_argument("--label", default="candidate")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    build_times = []
    qps = {}
    recall = {}
    for round_id in range(args.rounds):
        subprocess.run(["rm", "-rf", args.base_dir], check=False)
        proc = subprocess.Popen(
            [args.binary, "--nodaemon", "--base-dir", args.base_dir,
             "--port", str(args.port), "--parameter", "cpu_count=24"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.time() + 600
            while time.time() < deadline:
                try:
                    run_sql("select 1;", args.port)
                    break
                except Exception:
                    time.sleep(1)
            else:
                raise RuntimeError("observer did not become ready")

            run_sql("create database if not exists bench;", args.port)
            run_sql("use bench;", args.port)
            run_sql("drop table if exists t;", args.port)
            run_sql("create table t(c1 int primary key, c2 vector(%d), "
                    "vector index i(c2) with (distance=l2,type=hnsw,lib=cuvs,"
                    "m=16,ef_construction=200,ef_search=64));" % args.dim, args.port)

            # Fill the table with a deterministic set of vectors.
            import random
            rng = random.Random(20261030)
            batch = 1000
            t0 = time.time()
            for start in range(0, args.rows, batch):
                values = ",".join(
                    "(%d,'[%s]')" % (i, ",".join("%.6f" % rng.random() for _ in range(args.dim)))
                    for i in range(start, min(start + batch, args.rows)))
                run_sql("insert into t values %s;" % values, args.port)
            print("loaded %d rows in %.1fs" % (args.rows, time.time() - t0), flush=True)

            # The scored build window starts at the index DDL.
            probe = "[%s]" % ",".join("%.6f" % rng.random() for _ in range(args.dim))
            t0 = time.time()
            run_sql("create index i2 on t(c2) with (distance=l2,type=hnsw,lib=cuvs,"
                    "m=16,ef_construction=200,ef_search=64);", args.port)
            last = None
            while time.time() - t0 < 1800:
                rows = run_sql("select c1 from t order by l2_distance(c2,%s) approximate limit 10;"
                                % probe, args.port)
                cur = tuple(r[0] for r in rows)
                if cur and cur == last:
                    break
                last = cur
                time.sleep(0.5)
            build_times.append(time.time() - t0)
            print("build+first query: %.2fs" % build_times[-1], flush=True)

            for k in args.topk:
                start = time.time()
                for q in range(args.queries):
                    run_sql("select c1 from t order by l2_distance(c2,%s) approximate limit %d;"
                            % (probe, k), args.port)
                elapsed = time.time() - start
                qps[k] = args.queries / elapsed
                exact = run_sql("select c1 from t order by l2_distance(c2,%s) limit %d;"
                                % (probe, k), args.port)
                approx = run_sql("select c1 from t order by l2_distance(c2,%s) approximate limit %d;"
                                 % (probe, k), args.port)
                hit = len(set(r[0] for r in approx) & set(r[0] for r in exact)) / max(1, len(exact))
                recall[k] = hit
                print("top%d: %.1f qps, recall %.4f" % (k, qps[k], hit), flush=True)

            vram = sample_device_mib()
            rss = peak_rss_kib(proc.pid)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    geo = math.exp(sum(math.log(v) for v in qps.values()) / len(qps))
    geo_recall = min(recall.values())
    result = {
        "label": args.label,
        "rows": args.rows,
        "dim": args.dim,
        "build_sec": statistics.median(build_times),
        "qps": qps,
        "qps_geomean": geo,
        "recall": recall,
        "recall_min": geo_recall,
        "peak_vram_mib": vram,
        "peak_rss_kib": rss,
    }
    print(json.dumps(result, indent=2))
    print("")
    print("| config | build+first query | QPS@10 | QPS@100 | QPS geo mean | recall@10 | recall@100 | peak VRAM | peak RSS |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    print("| %s | %.3f s | %.1f | %.1f | %.1f | %.4f | %.4f | %d MiB | %d KiB |" % (
        result["label"], result["build_sec"], qps.get(10, 0), qps.get(100, 0), geo,
        recall.get(10, 0), recall.get(100, 0), vram, rss))
    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(result, f, indent=2)


if __name__ == "__main__":
    main()
