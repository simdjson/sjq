# Installs sjq from its GitHub releases (Windows).
#
#   irm https://raw.githubusercontent.com/simdjson/sjq/main/install.ps1 | iex
#
# Environment:
#   SJQ_VERSION      a release tag such as v0.1.0 (default: the latest release)
#   SJQ_INSTALL_DIR  where to put sjq.exe (default: %LOCALAPPDATA%\Programs\sjq)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$repo = 'simdjson/sjq'
$dir = if ($env:SJQ_INSTALL_DIR) { $env:SJQ_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA 'Programs\sjq' }
$arch = if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') { 'arm64' } else { 'x86_64' }
$asset = "sjq-windows-$arch.zip"
$base = if ($env:SJQ_VERSION) { "https://github.com/$repo/releases/download/$($env:SJQ_VERSION)" } else { "https://github.com/$repo/releases/latest/download" }

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("sjq-" + [System.Guid]::NewGuid())
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
  Write-Host "Downloading $asset from $base"
  Invoke-WebRequest -Uri "$base/$asset" -OutFile (Join-Path $tmp $asset)
  Invoke-WebRequest -Uri "$base/SHA256SUMS" -OutFile (Join-Path $tmp 'SHA256SUMS')
  $line = Get-Content (Join-Path $tmp 'SHA256SUMS') | Where-Object { $_ -match " $([regex]::Escape($asset))$" }
  if (-not $line) { throw "sjq: no checksum for $asset" }
  $expected = ($line -split ' ')[0]
  $actual = (Get-FileHash -Algorithm SHA256 (Join-Path $tmp $asset)).Hash.ToLower()
  if ($expected -ne $actual) { throw "sjq: checksum mismatch for $asset" }

  New-Item -ItemType Directory -Force $dir | Out-Null
  Expand-Archive -Force -Path (Join-Path $tmp $asset) -DestinationPath $tmp
  Copy-Item -Force (Join-Path $tmp 'sjq.exe') (Join-Path $dir 'sjq.exe')
} finally {
  Remove-Item -Recurse -Force $tmp
}

# The user Path is null on a profile that never set one.
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (-not $userPath) { $userPath = '' }
if (-not (($userPath -split ';') -contains $dir)) {
  $newPath = if ($userPath) { $userPath.TrimEnd(';') + ";$dir" } else { $dir }
  [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
  Write-Host "Added $dir to your user PATH (open a new terminal to use it)."
}
& (Join-Path $dir 'sjq.exe') --version
