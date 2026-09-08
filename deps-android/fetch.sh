#!/bin/bash
# 下载依赖源码包到 deps-android/src/
# 运行环境：Windows Git Bash（因为 github 只有 Windows 侧代理可达）
#
#   ./fetch.sh                 下载全部批次
#   ./fetch.sh core            下载某个批次
#   ./fetch.sh jansson ffmpeg  下载指定依赖
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=versions.sh
source "$HERE/versions.sh"

PROXY="${OBS_PROXY:-http://127.0.0.1:7892}"
SRC="$HERE/src"
mkdir -p "$SRC"

# 解析参数：批次名 或 依赖名
TARGETS=""
for a in "$@"; do
  if echo " $DEP_BATCHES " | grep -q " $a "; then
    TARGETS="$TARGETS $(batch_deps "$a")"
  else
    TARGETS="$TARGETS $a"
  fi
done
[ -z "${TARGETS// /}" ] && TARGETS=$(dep_names | tr '\n' ' ')

FAIL=""
SKIP=""
for d in $TARGETS; do
  url=$(dep_url "$d")
  if [ -z "$url" ]; then
    echo "[SKIP] $d : 未配置下载地址"
    SKIP="$SKIP $d"; continue
  fi
  ver=$(dep_version "$d")
  fn="$(basename "$url")"
  out="$SRC/${d}-${ver}-${fn}"
  if [ -s "$out" ]; then
    echo "[CACHE] $d ($ver) 已存在"
    continue
  fi
  prox=""
  case "$url" in
    *github.com*) prox="-x $PROXY" ;;
  esac
  echo "[GET ] $d $ver -> $fn"
  # shellcheck disable=SC2086
  if ! curl -fL --retry 3 --retry-delay 2 -C - -sS $prox -o "$out.part" "$url"; then
    echo "[FAIL] $d 下载失败：$url"
    rm -f "$out.part"; FAIL="$FAIL $d"; continue
  fi
  mv "$out.part" "$out"
  printf '       %8d bytes\n' "$(stat -c %s "$out")"
done

echo "=============================="
echo "完成。失败:${FAIL:- 无}  跳过:${SKIP:- 无}"
[ -n "$FAIL" ] && exit 1
exit 0
