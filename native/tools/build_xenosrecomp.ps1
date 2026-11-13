# build_xenosrecomp.ps1 [-BuildDir DIR] - Windows twin of build_xenosrecomp.sh: builds the patched XenosRecomp (native/third_party/XenosRecomp, see third_party/fetch.ps1).
# Default build dir: native/third_party/build/xenosrecomp (ignored by git). Prints the path of the executable.
# Needs: cmake, ninja, clang (LLVM) and the MSVC environment (scripts/windows/env.ps1 loads both). DXC comes with the sources (thirdparty/dxc-bin: dxcompiler.dll, dxil.dll).
param([string]$BuildDir = '')
$ErrorActionPreference = 'Stop'
$native = Split-Path -Parent $PSScriptRoot
$src = Join-Path $native 'third_party\XenosRecomp'
if (-not $BuildDir) { $BuildDir = Join-Path $native 'third_party\build\xenosrecomp' }
. (Join-Path $native '..\scripts\windows\env.ps1')
$ErrorActionPreference = 'Continue'   # (native tools write progress to stderr)
if (-not (Test-Path (Join-Path $src 'XenosRecomp'))) { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $native 'third_party\fetch.ps1') | Out-Host }
& cmake -S $src -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'cmake configure of XenosRecomp failed' }
& cmake --build $BuildDir | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'cmake build of XenosRecomp failed' }
Write-Output (Join-Path $BuildDir 'XenosRecomp\XenosRecomp.exe')
