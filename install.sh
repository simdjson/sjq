#!/bin/sh
# Installs sjq from its GitHub releases (Linux and macOS).
#
#   curl -fsSL https://raw.githubusercontent.com/simdjson/sjq/main/install.sh | sh
#
# Environment:
#   SJQ_VERSION      a release tag such as v0.1.0 (default: the latest release)
#   SJQ_INSTALL_DIR  where to put sjq (default: $HOME/.local/bin)
#   SJQ_MODIFY_PATH  1: add the directory to PATH in the shell's startup file
set -eu

repo="simdjson/sjq"
dir="${SJQ_INSTALL_DIR:-$HOME/.local/bin}"

case "$(uname -s)" in
  Linux)
    case "$(uname -m)" in
      x86_64 | amd64) asset="sjq-linux-x86_64" ;;
      aarch64 | arm64) asset="sjq-linux-aarch64" ;;
      *) echo "sjq: no prebuilt binary for Linux on $(uname -m); build from source" >&2; exit 1 ;;
    esac ;;
  Darwin) asset="sjq-macos-universal" ;;
  *) echo "sjq: no prebuilt binary for $(uname -s); build from source or use install.ps1 on Windows" >&2; exit 1 ;;
esac

if [ -n "${SJQ_VERSION:-}" ]; then
  base="https://github.com/$repo/releases/download/$SJQ_VERSION"
else
  base="https://github.com/$repo/releases/latest/download"
fi

if command -v curl >/dev/null 2>&1; then
  fetch() { curl -fsSL -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
  fetch() { wget -q -O "$2" "$1"; }
else
  echo "sjq: curl or wget is required" >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "Downloading $asset.tar.gz from $base"
fetch "$base/$asset.tar.gz" "$tmp/$asset.tar.gz"
fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS"

expected="$(grep " $asset.tar.gz\$" "$tmp/SHA256SUMS" | cut -d ' ' -f 1)"
if command -v sha256sum >/dev/null 2>&1; then
  actual="$(sha256sum "$tmp/$asset.tar.gz" | cut -d ' ' -f 1)"
else
  actual="$(shasum -a 256 "$tmp/$asset.tar.gz" | cut -d ' ' -f 1)"
fi
if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
  echo "sjq: checksum mismatch for $asset.tar.gz" >&2
  exit 1
fi

tar -xzf "$tmp/$asset.tar.gz" -C "$tmp"
mkdir -p "$dir"
cp "$tmp/$asset/sjq" "$dir/sjq"
chmod +x "$dir/sjq"
if [ "$(uname -s)" = "Darwin" ]; then
  # Downloaded with curl, the binary has no quarantine attribute; remove it
  # in case another tool added one.
  xattr -d com.apple.quarantine "$dir/sjq" 2>/dev/null || true
fi

echo "Installed $("$dir/sjq" --version) to $dir/sjq"
case ":$PATH:" in
  *":$dir:"*) exit 0 ;;
esac

# The directory is not in PATH: the line to add, for the user's shell.
case "$dir" in
  "$HOME"/*) shown_dir="\$HOME/${dir#"$HOME"/}" ;;
  *) shown_dir="$dir" ;;
esac
case "${SHELL:-}" in
  */zsh) rc="$HOME/.zshrc"; line="export PATH=\"$shown_dir:\$PATH\"" ;;
  */bash)
    # macOS Terminal opens login shells, which read ~/.bash_profile.
    if [ "$(uname -s)" = Darwin ]; then rc="$HOME/.bash_profile"; else rc="$HOME/.bashrc"; fi
    line="export PATH=\"$shown_dir:\$PATH\"" ;;
  */fish) rc="$HOME/.config/fish/config.fish"; line="fish_add_path $shown_dir" ;;
  *) rc="$HOME/.profile"; line="export PATH=\"$shown_dir:\$PATH\"" ;;
esac
case "$rc" in
  "$HOME"/*) shown_rc="~/${rc#"$HOME"/}" ;;
  *) shown_rc="$rc" ;;
esac
if [ "${SJQ_MODIFY_PATH:-}" = 1 ]; then
  mkdir -p "$(dirname "$rc")"
  printf '\n%s\n' "$line" >> "$rc"
  echo "Added to $shown_rc: $line"
  echo "Open a new terminal, or run: $line"
else
  echo "$dir is not in your PATH. To add it, run:"
  echo
  echo "  echo '$line' >> $shown_rc"
  echo
  echo "then open a new terminal (or set SJQ_MODIFY_PATH=1 when installing to have it done)."
fi
