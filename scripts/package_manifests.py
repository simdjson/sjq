#!/usr/bin/env python3
"""Writes the package manager files of a release, from its SHA256SUMS.

Usage: scripts/package_manifests.py v0.2.0 [OUT_DIR]

OUT_DIR (default: packaging/) gets:
  homebrew/sjq.rb                    a formula for a tap (simdjson/homebrew-tap)
  scoop/sjq.json                     a Scoop manifest (bucket)
  winget/simdjson.sjq.*.yaml         the three winget manifests, for a pull
                                     request to microsoft/winget-pkgs
"""
import json
import os
import sys
import urllib.request

REPO = "simdjson/sjq"
DESCRIPTION = "A jq-compatible command-line JSON processor built on simdjson"
HOMEPAGE = "https://simdjson.github.io/sjq/"
# Homebrew wants a capital letter first and no article.
HOMEBREW_DESCRIPTION = "JSON processor compatible with jq, built on simdjson"


def fetch(url):
    """Opens a URL; GitHub API calls carry GH_TOKEN or GITHUB_TOKEN when one
    is set (anonymous calls from CI runners hit the rate limit)."""
    req = urllib.request.Request(url)
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token and url.startswith("https://api.github.com/"):
        req.add_header("Authorization", f"Bearer {token}")
    return urllib.request.urlopen(req)


def checksums(tag):
    url = f"https://github.com/{REPO}/releases/download/{tag}/SHA256SUMS"
    with fetch(url) as r:
        lines = r.read().decode().splitlines()
    sums = {}
    for line in lines:
        digest, name = line.split()
        sums[name.lstrip("*")] = digest
    return sums


def release_date(tag):
    url = f"https://api.github.com/repos/{REPO}/releases/tags/{tag}"
    with fetch(url) as r:
        return json.load(r)["published_at"][:10]


def main():
    tag = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "packaging")
    version = tag.lstrip("v")
    sums = checksums(tag)
    base = f"https://github.com/{REPO}/releases/download/{tag}"

    # A release can lack the macOS binary for a while (scripts/release-macos.sh
    # attaches it). No package manager is updated before the release is
    # complete: until then, nothing is written.
    needed = ["sjq-macos-universal.tar.gz", "sjq-linux-x86_64.tar.gz", "sjq-linux-aarch64.tar.gz",
              "sjq-windows-x86_64.zip", "sjq-windows-arm64.zip"]
    missing = [n for n in needed if n not in sums]
    if missing:
        print(f"{tag} is not complete (missing: {', '.join(missing)}): no manifest written")
        return
    write_homebrew(out, base, sums)
    write_scoop(out, base, sums, version)
    write_winget(out, base, sums, version, tag)
    print(f"wrote the manifests of {tag} in {os.path.abspath(out)}")


def write_homebrew(out, base, sums):
    os.makedirs(os.path.join(out, "homebrew"), exist_ok=True)
    with open(os.path.join(out, "homebrew", "sjq.rb"), "w") as f:
        f.write(f'''class Sjq < Formula
  desc "{HOMEBREW_DESCRIPTION}"
  homepage "{HOMEPAGE}"
  license any_of: ["Apache-2.0", "MIT"]

  on_macos do
    # One universal binary for Apple silicon and Intel.
    on_arm do
      url "{base}/sjq-macos-universal.tar.gz"
      sha256 "{sums["sjq-macos-universal.tar.gz"]}"
    end
    on_intel do
      url "{base}/sjq-macos-universal.tar.gz"
      sha256 "{sums["sjq-macos-universal.tar.gz"]}"
    end
  end

  on_linux do
    on_arm do
      url "{base}/sjq-linux-aarch64.tar.gz"
      sha256 "{sums["sjq-linux-aarch64.tar.gz"]}"
    end
    on_intel do
      url "{base}/sjq-linux-x86_64.tar.gz"
      sha256 "{sums["sjq-linux-x86_64.tar.gz"]}"
    end
  end

  def install
    bin.install "sjq"
  end

  test do
    assert_equal "2\\n", pipe_output("#{{bin}}/sjq .a", '{{"a": 2}}')
  end
end
''')



def write_scoop(out, base, sums, version):
    os.makedirs(os.path.join(out, "scoop"), exist_ok=True)
    with open(os.path.join(out, "scoop", "sjq.json"), "w") as f:
        f.write(f'''{{
    "version": "{version}",
    "description": "{DESCRIPTION}",
    "homepage": "{HOMEPAGE}",
    "license": "Apache-2.0|MIT",
    "architecture": {{
        "64bit": {{
            "url": "{base}/sjq-windows-x86_64.zip",
            "hash": "{sums["sjq-windows-x86_64.zip"]}"
        }},
        "arm64": {{
            "url": "{base}/sjq-windows-arm64.zip",
            "hash": "{sums["sjq-windows-arm64.zip"]}"
        }}
    }},
    "bin": "sjq.exe",
    "checkver": "github",
    "autoupdate": {{
        "architecture": {{
            "64bit": {{ "url": "https://github.com/{REPO}/releases/download/v$version/sjq-windows-x86_64.zip" }},
            "arm64": {{ "url": "https://github.com/{REPO}/releases/download/v$version/sjq-windows-arm64.zip" }}
        }},
        "hash": {{ "url": "https://github.com/{REPO}/releases/download/v$version/SHA256SUMS" }}
    }}
}}
''')



def write_winget(out, base, sums, version, tag):
    wid = "simdjson.sjq"
    os.makedirs(os.path.join(out, "winget"), exist_ok=True)
    header = f'''PackageIdentifier: {wid}
PackageVersion: {version}
'''
    def schema(kind):
        return f"# yaml-language-server: $schema=https://aka.ms/winget-manifest.{kind}1.28.0.schema.json\n\n"

    with open(os.path.join(out, "winget", f"{wid}.yaml"), "w") as f:
        f.write(schema("version.") + header + '''DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.28.0
''')
    with open(os.path.join(out, "winget", f"{wid}.installer.yaml"), "w") as f:
        f.write(schema("installer.") + header + f'''InstallerType: zip
NestedInstallerType: portable
NestedInstallerFiles:
- RelativeFilePath: sjq.exe
  PortableCommandAlias: sjq
Commands:
- sjq
ReleaseDate: {release_date(tag)}
Installers:
- Architecture: x64
  InstallerUrl: {base}/sjq-windows-x86_64.zip
  InstallerSha256: {sums["sjq-windows-x86_64.zip"].upper()}
- Architecture: arm64
  InstallerUrl: {base}/sjq-windows-arm64.zip
  InstallerSha256: {sums["sjq-windows-arm64.zip"].upper()}
ManifestType: installer
ManifestVersion: 1.28.0
''')
    with open(os.path.join(out, "winget", f"{wid}.locale.en-US.yaml"), "w") as f:
        f.write(schema("defaultLocale.") + header + f'''PackageLocale: en-US
Publisher: simdjson
PublisherUrl: https://github.com/simdjson
PackageName: sjq
PackageUrl: https://github.com/{REPO}
License: Apache-2.0 OR MIT
LicenseUrl: https://github.com/{REPO}/blob/main/LICENSE
ShortDescription: {DESCRIPTION}
Description: sjq reads the jq language and runs jq filters ten to a hundred times faster on large inputs, with simdjson.
Moniker: sjq
Tags:
- json
- jq
- cli
- simdjson
ReleaseNotesUrl: https://github.com/{REPO}/releases/tag/{tag}
ManifestType: defaultLocale
ManifestVersion: 1.28.0
''')


if __name__ == "__main__":
    main()
