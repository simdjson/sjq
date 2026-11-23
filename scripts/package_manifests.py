#!/usr/bin/env python3
"""Writes the package manager files of a release, from its SHA256SUMS.

Usage: scripts/package_manifests.py v0.2.0 [OUT_DIR]

OUT_DIR (default: packaging/) gets:
  homebrew/sjq.rb                    a formula for a tap (simdjson/homebrew-tap)
  scoop/sjq.json                     a Scoop manifest (bucket)
  winget/simdjson.sjq.*.yaml         the three winget manifests, for a pull
                                     request to microsoft/winget-pkgs
"""
import os
import sys
import urllib.request

REPO = "simdjson/sjq"
DESCRIPTION = "A jq-compatible command-line JSON processor built on simdjson"
HOMEPAGE = "https://simdjson.github.io/sjq/"


def checksums(tag):
    url = f"https://github.com/{REPO}/releases/download/{tag}/SHA256SUMS"
    with urllib.request.urlopen(url) as r:
        lines = r.read().decode().splitlines()
    sums = {}
    for line in lines:
        digest, name = line.split()
        sums[name.lstrip("*")] = digest
    return sums


def main():
    tag = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "packaging")
    version = tag.lstrip("v")
    sums = checksums(tag)
    base = f"https://github.com/{REPO}/releases/download/{tag}"

    os.makedirs(os.path.join(out, "homebrew"), exist_ok=True)
    with open(os.path.join(out, "homebrew", "sjq.rb"), "w") as f:
        f.write(f'''class Sjq < Formula
  desc "{DESCRIPTION}"
  homepage "{HOMEPAGE}"
  version "{version}"
  license any_of: ["Apache-2.0", "MIT"]

  on_macos do
    url "{base}/sjq-macos-universal.tar.gz"
    sha256 "{sums["sjq-macos-universal.tar.gz"]}"
  end

  on_linux do
    on_intel do
      url "{base}/sjq-linux-x86_64.tar.gz"
      sha256 "{sums["sjq-linux-x86_64.tar.gz"]}"
    end
    on_arm do
      url "{base}/sjq-linux-aarch64.tar.gz"
      sha256 "{sums["sjq-linux-aarch64.tar.gz"]}"
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

    wid = "simdjson.sjq"
    os.makedirs(os.path.join(out, "winget"), exist_ok=True)
    header = f'''PackageIdentifier: {wid}
PackageVersion: {version}
'''
    with open(os.path.join(out, "winget", f"{wid}.yaml"), "w") as f:
        f.write(header + '''DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.9.0
''')
    with open(os.path.join(out, "winget", f"{wid}.installer.yaml"), "w") as f:
        f.write(header + f'''InstallerType: zip
NestedInstallerType: portable
NestedInstallerFiles:
- RelativeFilePath: sjq.exe
  PortableCommandAlias: sjq
Installers:
- Architecture: x64
  InstallerUrl: {base}/sjq-windows-x86_64.zip
  InstallerSha256: {sums["sjq-windows-x86_64.zip"].upper()}
- Architecture: arm64
  InstallerUrl: {base}/sjq-windows-arm64.zip
  InstallerSha256: {sums["sjq-windows-arm64.zip"].upper()}
ManifestType: installer
ManifestVersion: 1.9.0
''')
    with open(os.path.join(out, "winget", f"{wid}.locale.en-US.yaml"), "w") as f:
        f.write(header + f'''PackageLocale: en-US
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
ManifestVersion: 1.9.0
''')
    print(f"wrote the manifests of {tag} in {os.path.abspath(out)}")


if __name__ == "__main__":
    main()
