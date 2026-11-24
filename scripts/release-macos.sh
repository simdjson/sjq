#!/bin/sh
# Builds the universal macOS binary on a Mac and attaches it to a GitHub
# release, updating SHA256SUMS. For when the release workflow could not get
# a macOS runner.
#
#   scripts/release-macos.sh v0.1.0
set -eu

tag="$1"
repo="simdjson/sjq"
asset="sjq-macos-universal"
root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

git -C "$root" fetch -q --tags origin
git -C "$root" worktree add -q --detach "$work/src" "$tag"
cmake -S "$work/src" -B "$work/build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 >/dev/null
cmake --build "$work/build" -j 8 >/dev/null
ctest --test-dir "$work/build" --output-on-failure
strip "$work/build/sjq"
lipo -info "$work/build/sjq"

mkdir -p "$work/dist/$asset"
cp "$work/build/sjq" "$work/src/README.md" "$work/src/LICENSE" "$work/src/LICENSE-MIT" "$work/dist/$asset/"
tar -C "$work/dist" -czf "$work/$asset.tar.gz" "$asset"

# The checksums of the release, with this file added (or replaced).
gh release download "$tag" --repo "$repo" --pattern SHA256SUMS --dir "$work"
grep -v " $asset.tar.gz\$" "$work/SHA256SUMS" > "$work/SHA256SUMS.new" || true
(cd "$work" && shasum -a 256 "$asset.tar.gz") >> "$work/SHA256SUMS.new"
mv "$work/SHA256SUMS.new" "$work/SHA256SUMS"
cat "$work/SHA256SUMS"
gh release upload "$tag" --repo "$repo" --clobber "$work/$asset.tar.gz" "$work/SHA256SUMS"
git -C "$root" worktree remove --force "$work/src"

# The release is complete: the package managers can follow it now.
"$root/scripts/update_packages.sh"
