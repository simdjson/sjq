#!/bin/sh
# After a release: updates the Scoop manifest (bucket/sjq.json, here) and
# the Homebrew formula (the simdjson/homebrew-tap repository), from the
# release's SHA256SUMS, and pushes both.
#
#   scripts/update_packages.sh v0.3.0 [path/to/homebrew-tap]
#
# The winget manifests (simdjson.sjq) are written to packaging/winget for a
# pull request to microsoft/winget-pkgs.
set -eu

tag="$1"
root="$(cd "$(dirname "$0")/.." && pwd)"
tap="${2:-$root/../homebrew-tap}"
out="$root/packaging"

python3 "$root/scripts/package_manifests.py" "$tag" "$out"

cp "$out/scoop/sjq.json" "$root/bucket/sjq.json"
git -C "$root" add bucket/sjq.json
git -C "$root" commit -q -m "Scoop manifest for $tag" && git -C "$root" push -q origin HEAD

git -C "$tap" pull -q --ff-only
cp "$out/homebrew/sjq.rb" "$tap/Formula/sjq.rb"
git -C "$tap" add Formula/sjq.rb
git -C "$tap" commit -q -m "sjq ${tag#v}" && git -C "$tap" push -q origin HEAD

echo "Updated the Scoop bucket and the Homebrew tap for $tag."
echo "winget manifests: $out/winget"
