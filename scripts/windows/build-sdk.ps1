# Builds the patched ReXGlue SDK (pinned commit + sdk-patches/*.patch) for Windows and installs it into <prefix>\win-amd64.
# usage: powershell -File scripts\windows\build-sdk.ps1 -Prefix C:\gh2-sdk [-Work C:\gh2-rexsdk]
param(
  [Parameter(Mandatory = $true)][string]$Prefix,
  [string]$Work = (Join-Path $env:USERPROFILE 'gh2\rexsdk')   # short path: the SDK's tree goes deep
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')

$here = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$base = (Get-Content (Join-Path $here 'sdk-patches\SDK_BASE_COMMIT') -Raw).Trim()
New-Item -ItemType Directory -Force $Prefix | Out-Null
$Prefix = (Resolve-Path $Prefix).Path
New-Item -ItemType Directory -Force $Work | Out-Null
$src = Join-Path $Work 'src'

function Invoke-Git { & git.exe @args; if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed" } }

# core.autocrlf=false: the patches are LF and must apply byte for byte.
if (-not (Test-Path (Join-Path $src '.git'))) { Invoke-Git -c core.autocrlf=false clone https://github.com/rexglue/rexglue-sdk $src }
Push-Location $src
try {
  Invoke-Git config core.autocrlf false
  $ErrorActionPreference = 'Continue'   # (a failed fetch of one commit is expected on some servers: fall back to a full fetch)
  & git fetch -q origin $base 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Invoke-Git fetch -q origin }
  $ErrorActionPreference = 'Stop'
  Invoke-Git reset -q --hard $base
  Invoke-Git submodule update --init --depth 1

  # Git symlinks arrive on Windows as small text files holding the target path (creating real links needs Developer Mode). The SDK's libmspack and o1heap use them for sources:
  # replace each one by a copy of its target.
  function Resolve-GitSymlinks([string]$repo) {
    foreach ($line in (& git.exe -C $repo ls-files -s)) {
      if ($line -notmatch '^120000 \S+ \d\t(.+)$') { continue }
      $link = Join-Path $repo $Matches[1]
      if (-not (Test-Path -LiteralPath $link -PathType Leaf)) { continue }
      # a link text is one short line: anything else is already a real copy (a second run)
      if ((Get-Item -LiteralPath $link).Length -gt 1024) { continue }
      $target = (Get-Content -LiteralPath $link -Raw).Trim()
      if ($target -eq '' -or $target.IndexOfAny([IO.Path]::GetInvalidPathChars()) -ge 0 -or $target.Contains("`n")) { continue }
      $dest = [IO.Path]::GetFullPath((Join-Path (Split-Path $link) $target))
      if (Test-Path -LiteralPath $dest -PathType Container) { Remove-Item -LiteralPath $link -Force; Copy-Item -LiteralPath $dest -Destination $link -Recurse }
      elseif (Test-Path -LiteralPath $dest -PathType Leaf) { Copy-Item -LiteralPath $dest -Destination $link -Force }
      else { Write-Warning "symlink $($Matches[1]) -> ${target}: target not found" }
    }
  }
  Resolve-GitSymlinks $src
  foreach ($sub in (& git.exe submodule foreach --quiet --recursive 'echo $toplevel/$sm_path')) { Resolve-GitSymlinks $sub.Trim() }

  # Common patches first, then the Windows follow-ups (sdk-patches\win\*.patch) that adapt them (e.g. 0010's fd -> HANDLE).
  $patches = @(Get-ChildItem (Join-Path $here 'sdk-patches') -Filter '*.patch' | Sort-Object Name) +
             @(Get-ChildItem (Join-Path $here 'sdk-patches\win') -Filter '*.patch' -ErrorAction SilentlyContinue | Sort-Object Name)
  # A Windows checkout may hold the patches as CRLF; they are applied from LF copies.
  $lf = Join-Path $Work 'patches-lf'
  Remove-Item $lf -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $lf | Out-Null
  foreach ($p in $patches) {
    Write-Host "applying $($p.Name)"
    $copy = Join-Path $lf $p.Name
    $bytes = [IO.File]::ReadAllBytes($p.FullName)
    $text = [Text.Encoding]::UTF8.GetString($bytes).Replace("`r`n", "`n")
    [IO.File]::WriteAllBytes($copy, (New-Object Text.UTF8Encoding($false)).GetBytes($text))
    Invoke-Git apply --whitespace=nowarn $copy
  }

  $flags = '-march=x86-64-v2'
  & cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
    "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" -DCMAKE_CXX_STANDARD=23 `
    -DREXGLUE_USE_D3D12=ON -DREXGLUE_USE_VULKAN=OFF `
    -DREXGLUE_ENABLE_TRACY=OFF "-DCMAKE_INSTALL_PREFIX=$Prefix\win-amd64"
  if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
  & cmake --build out
  if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }
  & cmake --install out
  if ($LASTEXITCODE -ne 0) { throw 'cmake install failed' }

  # The SDK's Windows build is D3D12-only (its Vulkan backend needs POSIX dlfcn.h): the SDK serves as the application shell and code generator here, the game is drawn by this project's
  # own Vulkan renderer. That renderer wants Vulkan Memory Allocator next to the SDK headers, as in the Linux install: the SDK installs it only with its Vulkan backend, so copy it.
  Copy-Item (Join-Path $src 'thirdparty\vulkan-memory-allocator\include\vk_mem_alloc.h') (Join-Path $Prefix 'win-amd64\include\vk_mem_alloc.h') -Force
} finally { Pop-Location }
Write-Host "SDK installed in $Prefix\win-amd64"
