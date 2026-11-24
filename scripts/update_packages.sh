#!/bin/sh
# Updates the package managers to the latest release, once it is complete
# (all five binaries): runs the workflows that do it, which otherwise run
# every six hours.
#   - Packages (this repository): bucket/sjq.json, the Scoop manifest, and
#     the winget manifests as an artifact.
#   - update (simdjson/homebrew-tap): the Homebrew formula.
# scripts/release-macos.sh calls it once it has attached the macOS binary.
set -eu

gh workflow run packages.yml --repo simdjson/sjq
gh workflow run update.yml --repo simdjson/homebrew-tap
echo "Started the Packages (simdjson/sjq) and update (simdjson/homebrew-tap) workflows."
