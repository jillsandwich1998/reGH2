# Dot-source this file: it puts the Windows build toolchain on PATH for the current PowerShell session.
#   . scripts\windows\env.ps1
# Needs: Visual Studio 2022 Build Tools (MSVC + Windows SDK), LLVM >= 20 (clang, lld-link), CMake, Git, Vulkan SDK (glslc, spirv-val).
$ErrorActionPreference = 'Stop'

function Find-VsInstall {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio 2022 Build Tools (C++ workload).' }
  $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $p) { throw 'No Visual Studio install with the MSVC x64 tools was found.' }
  return $p
}

# MSVC environment (INCLUDE/LIB/PATH for the Windows SDK and the MSVC runtime that clang targets).
if (-not $env:VSCMD_VER) {
  $vs = Find-VsInstall
  $bat = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
  # vcvars looks for vswhere on PATH; without it, it prints a harmless error that a strict PowerShell would turn into a failure.
  $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
  if ($env:PATH -notlike "*$installer*") { $env:PATH = "$env:PATH;$installer" }
  $dump = cmd /c "`"$bat`" >nul 2>&1 && set"
  foreach ($line in $dump) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
  }
}

# LLVM (winget: LLVM.LLVM installs to Program Files\LLVM).
$llvm = Join-Path $env:ProgramFiles 'LLVM\bin'
if ((Test-Path $llvm) -and ($env:PATH -notlike "*$llvm*")) { $env:PATH = "$llvm;$env:PATH" }

# Vulkan SDK (winget: KhronosGroup.VulkanSDK sets VULKAN_SDK system-wide; a fresh shell sees it).
if (-not $env:VULKAN_SDK) {
  $cand = Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
  if ($cand) { $env:VULKAN_SDK = $cand.FullName }
}
if ($env:VULKAN_SDK) {
  $vb = Join-Path $env:VULKAN_SDK 'Bin'
  if ($env:PATH -notlike "*$vb*") { $env:PATH = "$vb;$env:PATH" }
}

foreach ($tool in 'clang', 'clang++', 'lld-link', 'cmake', 'ninja', 'git') {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Write-Warning "$tool not found on PATH" }
}
