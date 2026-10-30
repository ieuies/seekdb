#!/usr/bin/env bash
# 在 AMD 开发者平台（Radeon Cloud 的 seekdb-hipvs 模板）上，跑冻结 V5 与本分支的 A/B 对照。
#
# 关键点：seekdb 是把 hipVS bridge 以绝对路径链接进去的
#   cmake/Env.cmake: ob_define(CUVS_BRIDGE_LIB "")
#   src/oblib/lib/CMakeLists.txt: target_link_libraries(oblib_lib PUBLIC ${CUVS_BRIDGE_LIB})
# 所以换掉那个路径上的 .so 就换掉了实现，**不需要重编 seekdb**。
# 模板里预编译的 observer 本身���是链在冻结 V5 的 bridge 上的，正好当对照组。
#
# 用法：
#   bash tools/hipvs_bridge/run_amd_ab.sh [WORKDIR]
#
# 可选环境变量：
#   SEEKDB_BIN   预编译的 observer 路径（默认自动找）
#   SRC_DIR      seekdb 源码目录（只在需要重编时用）
#   BRIDGE_PATH  bridge 动态库被链接的路径（默认从 observer 的依赖里读）
#   ROWS DIM QUERIES  数据规模与查询条数

set -Eeuo pipefail

WORK=${1:-/workspace/amd-ab}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
V5=634888d9b037153fa143d2c913ba4b25503e1d62
REPO_URL=${REPO_URL:-https://github.com/ieuies/seekdb-2026.git}
BRANCH=${BRANCH:-amd-hipvs-v5}
ROWS=${ROWS:-250000}
DIM=${DIM:-128}
QUERIES=${QUERIES:-4096}
PORT_V5=${PORT_V5:-2882}
PORT_CAND=${PORT_CAND:-2881}

[[ -x "$ROCM_ROOT/bin/hipcc" ]] || { echo "hipcc 不可用：$ROCM_ROOT/bin/hipcc" >&2; exit 2; }
[[ -d "$HIPVS_ROOT/include" ]] || { echo "hipVS 头文件缺失：$HIPVS_ROOT/include" >&2; exit 2; }
command -v python3 >/dev/null || { echo "需要 python3" >&2; exit 2; }

mkdir -p "$WORK"

# ---------------------------------------------------------------- 定位产物
find_seekdb_bin() {
  if [[ -n "${SEEKDB_BIN:-}" ]]; then echo "$SEEKDB_BIN"; return; fi
  for c in /workspace/seekdb/build_release/src/observer/seekdb \
           /workspace/seekdb-2026/build_release/src/observer/seekdb \
           ./build_release/src/observer/seekdb \
           /usr/local/bin/seekdb /usr/bin/seekdb; do
    [[ -x "$c" ]] && { echo "$c"; return; }
  done
  find /workspace -maxdepth 5 -type f -name seekdb -perm -u+x 2>/dev/null | head -1
}

SEEKDB_BIN=$(find_seekdb_bin)
[[ -n "$SEEKDB_BIN" && -x "$SEEKDB_BIN" ]] || {
  echo "找不到 seekdb 可执行文件，请用 SEEKDB_BIN 指定" >&2; exit 3; }
echo "observer: $SEEKDB_BIN"

# bridge 是按绝对路径链接的，从依赖里读出来
BRIDGE_PATH=${BRIDGE_PATH:-$(ldd "$SEEKDB_BIN" 2>/dev/null \
  | awk '/seekdb_cuvs_bridge/ {print $3; exit}')}
[[ -n "$BRIDGE_PATH" ]] || {
  echo "observer 没有链接 seekdb_cuvs_bridge，需要重新编译一份带 OB_BUILD_CUVS 的 seekdb" >&2; exit 3; }
echo "bridge  : $BRIDGE_PATH"

# ------------------------------------------------------- 取源码（只需要 V5 的 bridge 源码）
SRC_DIR=${SRC_DIR:-$(pwd)}
if [[ ! -f "$SRC_DIR/tools/hipvs_bridge/seekdb_cuvs_bridge.c" ]]; then
  echo "=== 拉取仓库（$REPO_URL @ $BRANCH） ==="
  SRC_DIR="$WORK/src"
  rm -rf "$SRC_DIR"
  git clone --branch "$BRANCH" --single-branch "$REPO_URL" "$SRC_DIR"
fi
cd "$SRC_DIR"

if ! git cat-file -e "$V5^{commit}" 2>/dev/null; then
  echo "=== 取冻结 V5 的 bridge 源码 ==="
  git fetch --depth 1 origin "$V5" || git fetch origin "$V5"
fi

build_bridge() {
  # $1 = 源码文件, $2 = 输出 .so
  "$ROCM_ROOT/bin/hipcc" -shared -fPIC -O2 -D__HIP_PLATFORM_AMD__ \
    -I"$HIPVS_ROOT/include" -I"$ROCM_ROOT/include" \
    "$1" \
    -L"$HIPVS_ROOT/lib" \
    -Wl,-soname,libseekdb_cuvs_bridge.so \
    -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" \
    -lcuvs_c -o "$2"
}

echo
echo "=== 编译两个 bridge ==="
git show "$V5:tools/hipvs_bridge/seekdb_cuvs_bridge.c" > "$WORK/bridge_v5.c"
cp tools/hipvs_bridge/seekdb_cuvs_bridge.c "$WORK/bridge_candidate.c"
build_bridge "$WORK/bridge_v5.c" "$WORK/bridge_v5.so"
build_bridge "$WORK/bridge_candidate.c" "$WORK/bridge_candidate.so"
ls -la "$WORK"/*.so

install_bridge() {
  # $1 = .so
  cp "$1" "$BRIDGE_PATH"
  ldconfig 2>/dev/null || true
}

run_one() {
  # $1 = 标签, $2 = .so, $3 = 端口, $4 = 数据目录
  echo
  echo "=== 测 $1（bridge = $2） ==="
  install_bridge "$2"
  python3 tools/hipvs_bridge/bench_bridge.py \
    --binary "$SEEKDB_BIN" --port "$3" --base-dir "$4" \
    --rows "$ROWS" --dim "$DIM" --queries "$QUERIES" \
    --label "$1" --json-out "$WORK/$1.json" | tee "$WORK/$1.txt"
}

# 先跑候选再跑基线，与评测的顺序相反，减少先后顺序带来的偏差
run_one "candidate" "$WORK/bridge_candidate.so" "$PORT_CAND" "$WORK/db_cand"
run_one "V5" "$WORK/bridge_v5.so" "$PORT_V5" "$WORK/db_v5"

echo
echo "=== 汇总 ==="
python3 - "$WORK" <<'PY'
import json, math, os, sys
work = sys.argv[1]
cand = json.load(open(os.path.join(work, "candidate.json")))
v5 = json.load(open(os.path.join(work, "V5.json")))
build_ratio = v5["build_sec"] / cand["build_sec"]
q10 = cand["qps"].get(10, 0) / max(1e-9, v5["qps"].get(10, 0))
q100 = cand["qps"].get(100, 0) / max(1e-9, v5["qps"].get(100, 0))
q_geo = math.sqrt(q10 * q100)
build_score = 14 * max(0.0, min(1.0, math.log(max(build_ratio, 1e-9)) / math.log(3.0)))
query_score = 2 * max(0.0, min(1.0, math.log(max(q_geo, 1e-9)) / math.log(1.5)))
print("| metric | V5 (frozen) | candidate | ratio |")
print("| --- | ---: | ---: | ---: |")
print("| build+first query (s) | %.3f | %.3f | **%.3fx** |"
      % (v5["build_sec"], cand["build_sec"], build_ratio))
print("| QPS@10 | %.1f | %.1f | %.3fx |" % (v5["qps"].get(10, 0), cand["qps"].get(10, 0), q10))
print("| QPS@100 | %.1f | %.1f | %.3fx |" % (v5["qps"].get(100, 0), cand["qps"].get(100, 0), q100))
print("| recall@10 | %.4f | %.4f | |" % (v5["recall"].get(10, 0), cand["recall"].get(10, 0)))
print("| recall@100 | %.4f | %.4f | |" % (v5["recall"].get(100, 0), cand["recall"].get(100, 0)))
print("| peak VRAM (MiB) | %d | %d | %.3fx |"
      % (v5["peak_vram_mib"], cand["peak_vram_mib"],
         cand["peak_vram_mib"] / max(1, v5["peak_vram_mib"])))
print("| peak RSS (KiB) | %d | %d | |" % (v5["peak_rss_kib"], cand["peak_rss_kib"]))
print()
print("composite build speedup : %.3fx -> build score %.3f / 14" % (build_ratio, build_score))
print("composite query speedup : %.3fx -> query score %.3f / 2" % (q_geo, query_score))
print("estimated total         : %.3f / 20 (gates still apply)" % (4 + build_score + query_score))
print()
print("recall 门槛：SIFT250K >= 0.95；Hidden250K >= 0.90 / 0.85；且比同轮基线下降不超过 0.005")
PY
