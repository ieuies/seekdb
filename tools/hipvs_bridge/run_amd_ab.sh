#!/usr/bin/env bash
# 在 AMD 开发者平台（ROCm + seekdb 实验环境）上，跑冻结 V5 与本分支的 A/B 对照。
#
# 题面里的编译命令是固定的，这里完全照抄，只多了一个 "v5" 变体：
# V5 那一版用 git show 从冻结 commit 里取出 bridge 源码，其余仓库内容相同。
#
# 用法：
#   bash tools/hipvs_bridge/run_amd_ab.sh [WORKDIR]
#
# 产出：
#   $WORK/seekdb-v5 / $WORK/seekdb-candidate   两个可执行文件
#   $WORK/amd_ab.md                             两行 markdown 对照表

set -Eeuo pipefail

WORK=${1:-/workspace/amd-ab}
REPO_URL=${REPO_URL:-https://github.com/ieuies/seekdb.git}
BRANCH=${BRANCH:-amd-hipvs-v5}
V5=634888d9b037153fa143d2c913ba4b25503e1d62
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROWS=${ROWS:-250000}
DIM=${DIM:-128}
QUERIES=${QUERIES:-4096}
PORT_CAND=${PORT_CAND:-2881}
PORT_V5=${PORT_V5:-2882}
JOBS=${JOBS:-$(nproc)}

[[ -x "$ROCM_ROOT/bin/hipcc" ]] || { echo "hipcc 不可用：$ROCM_ROOT/bin/hipcc" >&2; exit 2; }
[[ -d "$HIPVS_ROOT/include" ]] || { echo "hipVS 头文件缺失：$HIPVS_ROOT/include" >&2; exit 2; }

rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

echo "=== 检出候选分支 $BRANCH ==="
git clone --branch "$BRANCH" --single-branch "$REPO_URL" cand
cd cand
if [[ ! -d deps/3rd ]]; then
  echo "=== 初始化第三方依赖 ==="
  ./build.sh init
fi

echo
echo "=== 取冻结 V5 的 bridge 源码 ==="
git fetch --depth 1 origin "$V5"
git show "$V5:tools/hipvs_bridge/seekdb_cuvs_bridge.c" > "$WORK/bridge_v5.c"
cp tools/hipvs_bridge/seekdb_cuvs_bridge.c "$WORK/bridge_candidate.c"

echo
echo "=== 编译两个 bridge ==="
cp "$WORK/bridge_v5.c" tools/hipvs_bridge/seekdb_cuvs_bridge.c
HIPVS_ROOT="$HIPVS_ROOT" ROCM_ROOT="$ROCM_ROOT" \
  tools/hipvs_bridge/build.sh "$WORK/libseekdb_cuvs_bridge_v5.so"
cp "$WORK/bridge_candidate.c" tools/hipvs_bridge/seekdb_cuvs_bridge.c
HIPVS_ROOT="$HIPVS_ROOT" ROCM_ROOT="$ROCM_ROOT" \
  tools/hipvs_bridge/build.sh "$WORK/libseekdb_cuvs_bridge_candidate.so"

build_seekdb() {
  # $1 = 构建目录, $2 = bridge 动态库
  echo
  echo "=== 编译 seekdb（$1） ==="
  cmake -S . -B "$1" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DOB_ENABLE_UNITY=ON \
    -DOB_USE_LLD=ON \
    -DOB_BUILD_CUVS=ON \
    -DOB_BUILD_CUVS_TRACE=OFF \
    -DCUVS_BRIDGE_LIB="$2"
  cmake --build "$1" --target seekdb --parallel "$JOBS"
}

build_seekdb "$WORK/build_v5" "$WORK/libseekdb_cuvs_bridge_v5.so"
build_seekdb "$WORK/build_cand" "$WORK/libseekdb_cuvs_bridge_candidate.so"

cp "$WORK/build_v5/src/observer/seekdb" "$WORK/seekdb-v5"
cp "$WORK/build_cand/src/observer/seekdb" "$WORK/seekdb-candidate"

echo
echo "=== 测候选 ==="
python3 tools/hipvs_bridge/bench_bridge.py \
  --binary "$WORK/seekdb-candidate" --port "$PORT_CAND" \
  --base-dir "$WORK/db_cand" --rows "$ROWS" --dim "$DIM" --queries "$QUERIES" \
  --label "candidate" --json-out "$WORK/candidate.json" | tee "$WORK/candidate.txt"

echo
echo "=== 测冻结 V5 ==="
python3 tools/hipvs_bridge/bench_bridge.py \
  --binary "$WORK/seekdb-v5" --port "$PORT_V5" \
  --base-dir "$WORK/db_v5" --rows "$ROWS" --dim "$DIM" --queries "$QUERIES" \
  --label "V5 (frozen)" --json-out "$WORK/v5.json" | tee "$WORK/v5.txt"

echo
echo "=== 汇总 ==="
python3 - "$WORK" <<'PY'
import json, os, sys
work = sys.argv[1]
cand = json.load(open(os.path.join(work, "candidate.json")))
v5 = json.load(open(os.path.join(work, "v5.json")))
build_ratio = v5["build_sec"] / cand["build_sec"]
q10 = cand["qps"].get(10, 0) / v5["qps"].get(10, 0)
q100 = cand["qps"].get(100, 0) / v5["qps"].get(100, 0)
q_geo = (q10 * q100) ** 0.5
import math
build_score = 14 * max(0.0, min(1.0, math.log(max(build_ratio, 1e-9)) / math.log(3.0)))
query_score = 2 * max(0.0, min(1.0, math.log(max(q_geo, 1e-9)) / math.log(1.5)))
print("| metric | V5 | candidate | ratio |")
print("| --- | ---: | ---: | ---: |")
print("| build+first query (s) | %.3f | %.3f | **%.3fx** |" % (v5["build_sec"], cand["build_sec"], build_ratio))
print("| QPS@10 | %.1f | %.1f | %.3fx |" % (v5["qps"].get(10, 0), cand["qps"].get(10, 0), q10))
print("| QPS@100 | %.1f | %.1f | %.3fx |" % (v5["qps"].get(100, 0), cand["qps"].get(100, 0), q100))
print("| recall@10 | %.4f | %.4f | |" % (v5["recall"].get(10, 0), cand["recall"].get(10, 0)))
print("| recall@100 | %.4f | %.4f | |" % (v5["recall"].get(100, 0), cand["recall"].get(100, 0)))
print("| peak VRAM (MiB) | %d | %d | %.3fx |" % (v5["peak_vram_mib"], cand["peak_vram_mib"],
      cand["peak_vram_mib"] / max(1, v5["peak_vram_mib"])))
print("| peak RSS (KiB) | %d | %d | |" % (v5["peak_rss_kib"], cand["peak_rss_kib"]))
print()
print("composite build speedup : %.3fx -> build score %.3f / 14" % (build_ratio, build_score))
print("composite query speedup : %.3fx -> query score %.3f / 2" % (q_geo, query_score))
print("estimated total         : %.3f / 20 (gates still apply)" % (4 + build_score + query_score))
PY
