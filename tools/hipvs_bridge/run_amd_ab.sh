#!/usr/bin/env bash
# 在 AMD 开发者平台（Radeon Cloud 的 seekdb-hipvs 模板）上，跑冻结 V5 与本分支的 A/B 对照。
#
# 思路：
#  * 模板预编译的 observer 链的那个 bridge .so 就是冻结基线本身，直接复制出来当对照组，
#    不需要 git，也不需要重编 seekdb。
#  * 候选 bridge 用本仓库的 tools/hipvs_bridge/seekdb_cuvs_bridge.c 编。
#  * seekdb 是把 bridge 按绝对路径链接进去的
#      cmake/Env.cmake:            ob_define(CUVS_BRIDGE_LIB "")
#      src/oblib/lib/CMakeLists.txt: target_link_libraries(oblib_lib PUBLIC ${CUVS_BRIDGE_LIB})
#    所以换掉那个路径上的 .so 就换掉了实现。
#
# 用法：
#   bash get_and_run_amd_ab.sh [WORKDIR]
#
# 可选环境变量：
#   SEEKDB_BIN   observer 路径（默认自动找）
#   BRIDGE_PATH  bridge 动态库路径（默认从 observer 的依赖里读）
#   SRC_DIR      seekdb 源码目录（默认从当前目录往上找）
#   ROWS DIM QUERIES  数据规模与查询条数
#   SMOKE=0      跑全量（默认冒烟）

set -Eeuo pipefail

WORK=${1:-/workspace/amd-ab}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
V5=634888d9b037153fa143d2c913ba4b25503e1d62
ROWS=${ROWS:-250000}
DIM=${DIM:-128}
QUERIES=${QUERIES:-4096}
PORT_V5=${PORT_V5:-2882}
PORT_CAND=${PORT_CAND:-2881}

[[ -x "$ROCM_ROOT/bin/hipcc" ]] || { echo "hipcc 不可用：$ROCM_ROOT/bin/hipcc" >&2; exit 2; }
[[ -d "$HIPVS_ROOT/include" ]] || { echo "hipVS 头文件缺失：$HIPVS_ROOT/include" >&2; exit 2; }
command -v python3 >/dev/null || { echo "需要 python3" >&2; exit 2; }

mkdir -p "$WORK"
cd "$WORK"

# --------------------------------------------------------------- 定位 observer
find_seekdb_bin() {
  if [[ -n "${SEEKDB_BIN:-}" ]]; then echo "$SEEKDB_BIN"; return; fi
  for c in /workspace/seekdb/build_release/src/observer/seekdb \
           /workspace/seekdb-2026/build_release/src/observer/seekdb \
           /seekdb_hipvs_notebook/seekdb/build_release/src/observer/seekdb \
           ./build_release/src/observer/seekdb \
           /usr/local/bin/seekdb /usr/bin/seekdb; do
    [[ -x "$c" ]] && { echo "$c"; return; }
  done
  find / -maxdepth 7 -type f -name seekdb -perm -u+x 2>/dev/null | head -1
}

SEEKDB_BIN=$(find_seekdb_bin)
[[ -n "$SEEKDB_BIN" && -x "$SEEKDB_BIN" ]] || {
  echo "找不到 observer，用 SEEKDB_BIN=... 指定" >&2; exit 3; }
echo "observer: $SEEKDB_BIN"

BRIDGE_PATH=${BRIDGE_PATH:-$(ldd "$SEEKDB_BIN" 2>/dev/null \
  | awk '/seekdb_cuvs_bridge/ {print $3; exit}')}
[[ -n "$BRIDGE_PATH" ]] || {
  echo "observer 没有链接 seekdb_cuvs_bridge，需要一份带 OB_BUILD_CUVS 的 seekdb" >&2; exit 3; }
echo "bridge  : $BRIDGE_PATH"

# --------------------------------------------------------------- 源码与 bridge 源码
SRC_DIR=${SRC_DIR:-}
if [[ -z "$SRC_DIR" ]]; then
  # 先从当前目录往上找，再试几个模板里常见的位置，最后全盘找一次
  d=$(pwd)
  while [[ "$d" != "/" && ! -f "$d/tools/hipvs_bridge/seekdb_cuvs_bridge.c" ]]; do d=$(dirname "$d"); done
  [[ -f "$d/tools/hipvs_bridge/seekdb_cuvs_bridge.c" ]] && SRC_DIR="$d"
fi
if [[ -z "$SRC_DIR" ]]; then
  for c in /seekdb_hipvs_notebook/seekdb-v5-source \
           /seekdb_hipvs_notebook/seekdb \
           /workspace/seekdb_hipvs_notebook/seekdb-v5-source \
           /workspace/seekdb_hipvs_notebook/seekdb; do
    [[ -f "$c/tools/hipvs_bridge/seekdb_cuvs_bridge.c" ]] && { SRC_DIR="$c"; break; }
  done
fi
if [[ -z "$SRC_DIR" ]]; then
  hit=$(find / -maxdepth 7 -path "*/tools/hipvs_bridge/seekdb_cuvs_bridge.c" 2>/dev/null | head -1)
  [[ -n "$hit" ]] && SRC_DIR="${hit%/tools/hipvs_bridge/seekdb_cuvs_bridge.c}"
fi
if [[ -z "$SRC_DIR" || ! -f "$SRC_DIR/tools/hipvs_bridge/seekdb_cuvs_bridge.c" ]]; then
  echo "找不到 seekdb 源码树，用 SRC_DIR=... 指定" >&2
  find / -maxdepth 7 -path "*/tools/hipvs_bridge/seekdb_cuvs_bridge.c" 2>/dev/null | head -5 >&2
  exit 3
fi
echo "源码树  : $SRC_DIR"

CAND_SRC="$SRC_DIR/tools/hipvs_bridge/seekdb_cuvs_bridge.c"
[[ -f "$CAND_SRC" ]] || { echo "候选 bridge 源码缺失：$CAND_SRC" >&2; exit 3; }

# 兜底：内嵌的 V5 源码（冻结提交的文件）
V5_SRC="$WORK/bridge_v5.c"
if [[ ! -f "$V5_SRC" ]]; then
  base64 -d "$WORK/bridge_v5.c.b64" > "$V5_SRC" 2>/dev/null || true
fi

build_bridge() {   # $1 源码, $2 输出
  "$ROCM_ROOT/bin/hipcc" -shared -fPIC -O2 -D__HIP_PLATFORM_AMD__ \
    -I"$HIPVS_ROOT/include" -I"$ROCM_ROOT/include" "$1" \
    -L"$HIPVS_ROOT/lib" \
    -Wl,-soname,libseekdb_cuvs_bridge.so \
    -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" \
    -lcuvs_c -o "$2"
}

echo
echo "=== 准备两个 bridge ==="
# 对照组：模板里现成的那个（就是冻结基线）
if [[ -f "$BRIDGE_PATH" ]]; then
  cp -f "$BRIDGE_PATH" "$WORK/bridge_v5.so"
  echo "对照组：直接复用模板里的 $BRIDGE_PATH"
else
  echo "对照组：模板里没有现成的 bridge，用内嵌的 V5 源码编"
  build_bridge "$V5_SRC" "$WORK/bridge_v5.so"
fi
# 候选组
build_bridge "$CAND_SRC" "$WORK/bridge_candidate.so"
ls -la "$WORK"/*.so

# 跑完把模板原来的 bridge 放回去
ORIG_SO=""
[[ -f "$BRIDGE_PATH" ]] && ORIG_SO="$WORK/bridge_v5.so"
restore_bridge() {
  if [[ -n "$ORIG_SO" && -f "$ORIG_SO" ]]; then
    cp -f "$ORIG_SO" "$BRIDGE_PATH" 2>/dev/null || true
    echo "已把原来的 bridge 装回 $BRIDGE_PATH"
  fi
}
trap restore_bridge EXIT

install_bridge() { cp -f "$1" "$BRIDGE_PATH"; ldconfig 2>/dev/null || true; }

run_one() {   # $1 标签 $2 .so $3 端口 $4 数据目录
  echo
  echo "=== 测 $1 ==="
  install_bridge "$2"
  python3 "$SRC_DIR/tools/hipvs_bridge/bench_bridge.py" \
    --binary "$SEEKDB_BIN" --port "$3" --base-dir "$4" \
    --rows "$ROWS" --dim "$DIM" --queries "$QUERIES" \
    --label "$1" --json-out "$WORK/$1.json" | tee "$WORK/$1.txt"
}

# 先候选后基线，与评测交换顺序的做法相反，减少先后顺序偏差
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
