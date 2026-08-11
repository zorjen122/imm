#!/usr/bin/env bash
set -euo pipefail

# 作用：
#   检查当前 Git 变更中的 C/C++ 源码是否符合仓库根目录 .clang-format。
#   默认只检查已修改、已暂存和未跟踪的源码文件，不会直接改写文件。
#   若 clang-format 支持 --dry-run --Werror，则优先使用该模式；否则回退到 diff 检查。
# 常用方式：
#   ./scripts/check-format.sh
# 检查全部已纳管源码：
#   ./scripts/check-format.sh --all

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
  echo "Missing clang-format. Install it first, or set CLANG_FORMAT=/path/to/clang-format." >&2
  exit 127
fi

is_source_file() {
  case "$1" in
    *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx) return 0 ;;
    *) return 1 ;;
  esac
}

is_ignored_path() {
  case "$1" in
    build/*|cmake-build-*/*|.demos/*) return 0 ;;
    *) return 1 ;;
  esac
}

collect_changed_files() {
  {
    git -C "$ROOT_DIR" diff --name-only --diff-filter=ACMR
    git -C "$ROOT_DIR" diff --cached --name-only --diff-filter=ACMR
    git -C "$ROOT_DIR" ls-files --others --exclude-standard
  } | sort -u
}

collect_all_files() {
  git -C "$ROOT_DIR" ls-files
}

supports_dry_run() {
  "$CLANG_FORMAT" --help 2>/dev/null | grep -q -- '--dry-run'
}

mode="${1:-}"
if [[ "$mode" == "--all" ]]; then
  mapfile -t candidates < <(collect_all_files)
elif [[ -z "$mode" ]]; then
  mapfile -t candidates < <(collect_changed_files)
else
  echo "Usage: $0 [--all]" >&2
  exit 2
fi

files=()
for file in "${candidates[@]}"; do
  if is_source_file "$file" && ! is_ignored_path "$file" && [[ -f "$ROOT_DIR/$file" ]]; then
    files+=("$file")
  fi
done

if [[ "${#files[@]}" -eq 0 ]]; then
  echo "No changed C/C++ files to check."
  exit 0
fi

failed=0
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

if supports_dry_run; then
  for file in "${files[@]}"; do
    if ! "$CLANG_FORMAT" --style=file --dry-run --Werror "$ROOT_DIR/$file"; then
      failed=1
    fi
  done
else
  for file in "${files[@]}"; do
    formatted="$tmp_dir/$(basename "$file").formatted"
    "$CLANG_FORMAT" --style=file "$ROOT_DIR/$file" > "$formatted"
    if ! diff -u "$ROOT_DIR/$file" "$formatted"; then
      failed=1
    fi
  done
fi

if [[ "$failed" -ne 0 ]]; then
  echo "Formatting check failed. Run clang-format on the files above." >&2
  exit 1
fi

echo "Formatting check passed for ${#files[@]} file(s)."
