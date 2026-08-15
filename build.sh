#!/usr/bin/env bash

set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
requested_config="${1:-Release}"

case "${requested_config,,}" in
  release)
    build_type="Release"
    ;;
  debug)
    build_type="Debug"
    ;;
  *)
    echo "Usage: $0 [Release|Debug]" >&2
    exit 2
    ;;
esac

for tool in cmake ninja clang-cl llvm-lib llvm-rc lld-link; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: required build tool '$tool' was not found" >&2
    exit 1
  fi
done

xwin_cache="$root_dir/.xwin-cache"
if [[ ! -d "$xwin_cache/sdk" || ! -d "$xwin_cache/crt" ]]; then
  if ! command -v xwin >/dev/null 2>&1; then
    echo "error: xwin is required to create $xwin_cache" >&2
    echo "Enter the project's Nix shell or install xwin, then run this script again." >&2
    exit 1
  fi

  echo "Preparing the Windows SDK and CRT..."
  xwin --accept-license splat --include-debug-libs --output "$xwin_cache"
fi

build_dir="$root_dir/build-${build_type,,}"
toolchain_file="$root_dir/toolchain-windows.cmake"

cmake \
  -S "$root_dir" \
  -B "$build_dir" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain_file"

cmake --build "$build_dir"

artifact="$root_dir/build/x64/$build_type/steam_api64.dll"
if [[ ! -f "$artifact" ]]; then
  echo "error: build completed but $artifact was not created" >&2
  exit 1
fi

echo
echo "$build_type build complete: $artifact"
