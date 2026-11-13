# Builds reGH2-Setup-<version>.exe: stages the program folder (launcher, Python, the prebuilt game host, a pruned llvm-mingw, Ninja, the SDK's code generator, the shader
# tools) and compiles the Inno Setup script. Nothing of the game is in it: the player's copy is turned into the game on their machine (codegen + the game module
# gh2game.dll, compiled with the llvm-mingw it carries: nothing from Microsoft is needed there).
#
#   powershell -File packaging\windows\make-installer.ps1 [-Sdk C:\Users\you\gh2\sdk] [-HostPrefix C:\Users\you\gh2\recomp] [-Out dist] [-StageOnly]
#
# Needs on this machine: the SDK made by scripts\windows\build-sdk.ps1, a full build of these sources in -HostPrefix (`gh2win.py build --full`: the host, gh2.exe, is
# shipped prebuilt as host\ and must match these sources), LLVM and Visual Studio Build Tools (rc.exe, the launcher stub, the C++ runtime DLLs), Inno Setup 6
# (winget install JRSoftware.InnoSetup) and a network for the first run (Python, Ninja and llvm-mingw are downloaded into -Cache and checked against pinned SHA-256).
param(
  [string]$Sdk = (Join-Path $env:USERPROFILE 'gh2\sdk'),
  [string]$HostPrefix = (Join-Path $env:USERPROFILE 'gh2\recomp'),
  [string]$Out = '',
  [string]$Cache = (Join-Path $env:USERPROFILE 'gh2\cache'),
  [string]$Version = '',
  [switch]$StageOnly
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Out) { $Out = Join-Path $repo 'dist' }
New-Item -ItemType Directory -Force $Out, $Cache | Out-Null
$Out = (Resolve-Path $Out).Path
if (-not $Version) { $Version = [regex]::Match((Get-Content (Join-Path $repo 'scripts\gh2install\__init__.py') -Raw), '__version__ = "([^"]+)"').Groups[1].Value }

# ---- pinned downloads ---------------------------------------------------------------------------------------------------------------------------------------
$downloads = @(
  @{ Name = 'cpython-3.12.16+20261026-x86_64-pc-windows-msvc-install_only_stripped.tar.gz'; Sha = '1f2ab53880fc67ea323398c6388e05af28addc99a8e389209f08a51fdff4b149'
     Url = 'https://github.com/astral-sh/python-build-standalone/releases/download/20261026/cpython-3.12.16%2B20261026-x86_64-pc-windows-msvc-install_only_stripped.tar.gz' },
  @{ Name = 'ninja-win-1.12.1.zip'; Sha = 'f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a'
     Url = 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' },
  @{ Name = 'llvm-mingw-20261104-ucrt-x86_64.zip'; Sha = '552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f'
     Url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/llvm-mingw-20261104-ucrt-x86_64.zip' }
)
function Get-Pinned($d) {
  $f = Join-Path $Cache $d.Name
  if (-not (Test-Path $f)) { Write-Host "downloading $($d.Name)"; & curl.exe -sSL --fail -o $f $d.Url; if ($LASTEXITCODE -ne 0) { throw "download failed: $($d.Url)" } }
  $got = (Get-FileHash $f -Algorithm SHA256).Hash.ToLower()
  if ($got -ne $d.Sha) { Remove-Item $f -Force; throw "$($d.Name): SHA-256 is $got, expected $($d.Sha)" }
  return $f
}
function Get-Text($name, $url, $dest) {   # a licence text (documentation, not pinned) copied next to the binaries; a failed download is a warning, the build goes on
  $f = Join-Path $Cache $name
  # (-s without -S: curl prints nothing to stderr, which Windows PowerShell would turn into a terminating error under ErrorActionPreference=Stop)
  if (-not (Test-Path $f)) { & curl.exe -sL --fail -o $f $url; if ($LASTEXITCODE -ne 0) { Remove-Item $f -ErrorAction SilentlyContinue; $global:LASTEXITCODE = 0; Write-Warning "licence not downloaded: $url (add $name to $Cache by hand)"; return } }
  Copy-Item $f $dest
}
function Copy-Tree($from, $to, [string[]]$excludeDirs = @(), [string[]]$excludeFiles = @()) {
  New-Item -ItemType Directory -Force $to | Out-Null
  $rcArgs = @($from, $to, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP')
  if ($excludeDirs) { $rcArgs += '/XD'; $rcArgs += $excludeDirs }
  if ($excludeFiles) { $rcArgs += '/XF'; $rcArgs += $excludeFiles }
  & robocopy @rcArgs | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "robocopy $from -> $to failed ($LASTEXITCODE)" }
  $global:LASTEXITCODE = 0
}

# ---- what is needed on this machine -----------------------------------------------------------------------------------------------------------------------
$sdkPrefix = Join-Path $Sdk 'win-amd64'
if (-not (Test-Path (Join-Path $sdkPrefix 'bin\rexglue.exe'))) { throw "the ReXGlue SDK is not at ${sdkPrefix}: run scripts\windows\build-sdk.ps1 -Prefix $Sdk" }
. (Join-Path $repo 'scripts\windows\env.ps1')
$ErrorActionPreference = 'Stop'
$llvm = Join-Path $env:ProgramFiles 'LLVM'
if (-not (Test-Path $llvm)) { throw "$llvm not found (winget install LLVM.LLVM): it compiles the launcher stub" }
if (-not (Test-Path (Join-Path $HostPrefix 'build\out\gh2.exe'))) { throw "no full build in ${HostPrefix}: py scripts\windows\gh2win.py build --full --sdk $Sdk --prefix $HostPrefix --game <your game>" }
$vs =& (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe') -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$redist = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT') -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $redist) { throw 'the C++ runtime DLLs (VC\Redist) were not found in Visual Studio' }
$xr = Join-Path $repo 'native\third_party\build\xenosrecomp\XenosRecomp'
if (-not (Test-Path (Join-Path $xr 'XenosRecomp.exe'))) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'native\third_party\fetch.ps1'); if ($LASTEXITCODE -ne 0) { throw 'fetch.ps1 failed' }
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'native\tools\build_xenosrecomp.ps1') | Out-Null; if ($LASTEXITCODE -ne 0) { throw 'build_xenosrecomp.ps1 failed' }
}
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc -and -not $StageOnly) { throw 'Inno Setup 6 was not found (winget install JRSoftware.InnoSetup)' }
$py = Get-Pinned $downloads[0]; $ninjaZip = Get-Pinned $downloads[1]; $mingwZip = Get-Pinned $downloads[2]

# ---- the stage --------------------------------------------------------------------------------------------------------------------------------------------------
$stage = Join-Path $Out 'stage\reGH2'
if ((Split-Path $stage -Leaf) -ne 'reGH2' -or -not $stage.StartsWith($Out)) { throw "refusing to clear $stage" }
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Write-Host "staging in $stage"

# the repository's own folders (the package has the layout of the repository: gh2install.pipeline.repo_root() is the package root)
Copy-Tree (Join-Path $repo 'scripts') (Join-Path $stage 'scripts') @('__pycache__', 'tests', 'linux') @('*.pyc', 'gh2-recomp', 'build-sdk.sh')
Copy-Tree (Join-Path $repo 'launcher') (Join-Path $stage 'launcher') @('__pycache__') @('*.pyc')
Copy-Tree (Join-Path $repo 'app') (Join-Path $stage 'app') @('__pycache__')
Copy-Tree (Join-Path $repo 'native') (Join-Path $stage 'native') @('__pycache__', 'golden', 'XenosRecomp', 'build') @('*.pyc')
Copy-Tree (Join-Path $repo 'sdk-patches') (Join-Path $stage 'sdk-patches')
foreach ($f in 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md', 'CHANGELOG.md') { Copy-Item (Join-Path $repo $f) $stage }
Copy-Item (Join-Path $PSScriptRoot 'reGH2.ico') (Join-Path $stage 'launcher\reGH2.ico')
Copy-Item (Join-Path $repo 'packaging\icon.png') (Join-Path $stage 'launcher\icon.png')

# Python with Tk (the launcher and the shader pipeline run on it)
Write-Host 'python'
& tar -xf $py -C $stage; if ($LASTEXITCODE -ne 0) { throw 'tar failed on the Python archive' }
if (-not (Test-Path (Join-Path $stage 'python\pythonw.exe'))) { throw 'the Python archive has no python\pythonw.exe' }

# the patched ReXGlue SDK, only what the player's build uses: the code generator (bin\rexglue.exe and the DLLs it loads), simde (header-only SIMD emulation the generated
# code includes) and the licences. Its headers, import libraries and CMake files only build the host, which comes prebuilt.
Write-Host 'sdk'
$sdkOut = Join-Path $stage 'sdk\win-amd64'
Copy-Tree (Join-Path $sdkPrefix 'bin') (Join-Path $sdkOut 'bin')
Copy-Tree (Join-Path $sdkPrefix 'include\simde') (Join-Path $sdkOut 'include\simde')
if (Test-Path (Join-Path $sdkPrefix 'licenses')) { Copy-Tree (Join-Path $sdkPrefix 'licenses') (Join-Path $sdkOut 'licenses') }

# the game host, prebuilt from these sources (gh2.exe, its PDB, the SDK's runtime DLLs, the C++ runtime DLLs, the override list and host.json): `gh2win.py make-host`
Write-Host 'host'
& (Join-Path $stage 'python\python.exe') -I (Join-Path $repo 'scripts\windows\gh2win.py') make-host --sdk $Sdk --prefix $HostPrefix --out (Join-Path $stage 'host')
if ($LASTEXITCODE -ne 0) { throw 'make-host failed (is the full build in -HostPrefix up to date with these sources?)' }

# llvm-mingw, pruned to what compiles the game module for x86_64: the clang driver and its target wrappers, lld, the DLLs they load, the x86_64 configuration files,
# the headers (mingw-w64, libc++), the x86_64 libraries (mingw-w64 import libraries, libc++, libunwind) and compiler-rt's builtins. Everything is redistributable.
Write-Host 'llvm-mingw'
$mingwTmp = Join-Path $Out 'llvm-mingw-unpack'
if (Test-Path $mingwTmp) { Remove-Item $mingwTmp -Recurse -Force }
New-Item -ItemType Directory -Force $mingwTmp | Out-Null
& tar -xf $mingwZip -C $mingwTmp; if ($LASTEXITCODE -ne 0) { throw 'tar failed on the llvm-mingw archive' }
$mingwSrc = (Get-ChildItem $mingwTmp -Directory | Select-Object -First 1).FullName
$mingwOut = Join-Path $stage 'llvm-mingw'
New-Item -ItemType Directory -Force (Join-Path $mingwOut 'bin') | Out-Null
foreach ($f in 'clang.exe', 'clang++.exe', 'clang-target-wrapper.exe', 'ld.lld.exe', 'libLLVM-*.dll', 'libclang-cpp.dll', 'libc++.dll', 'libunwind.dll',
               'x86_64-w64-windows-gnu.cfg', 'x86_64-pc-windows-gnu.cfg', 'mingw32-common.cfg', 'clang-[0-9]*.exe') {
  $hits = @(Get-ChildItem (Join-Path $mingwSrc 'bin') -File | Where-Object { $_.Name -like $f })   # (-like: -Filter knows no [0-9])
  if (-not $hits) { throw "llvm-mingw: bin\$f not found" }
  $hits | ForEach-Object { Copy-Item $_.FullName (Join-Path $mingwOut 'bin') }
}
Copy-Tree (Join-Path $mingwSrc 'include') (Join-Path $mingwOut 'include')
Copy-Tree (Join-Path $mingwSrc 'x86_64-w64-mingw32\lib') (Join-Path $mingwOut 'x86_64-w64-mingw32\lib')
$res = (Get-ChildItem (Join-Path $mingwSrc 'lib\clang') -Directory | Sort-Object Name | Select-Object -Last 1).Name
Copy-Tree (Join-Path $mingwSrc "lib\clang\$res\include") (Join-Path $mingwOut "lib\clang\$res\include")
New-Item -ItemType Directory -Force (Join-Path $mingwOut "lib\clang\$res\lib\windows") | Out-Null
Copy-Item (Join-Path $mingwSrc "lib\clang\$res\lib\windows\libclang_rt.builtins-x86_64.a") (Join-Path $mingwOut "lib\clang\$res\lib\windows")
Copy-Item (Join-Path $mingwSrc 'LICENSE.TXT') (Join-Path $mingwOut 'LICENSE.TXT')
Remove-Item $mingwTmp -Recurse -Force

# Ninja
Write-Host 'ninja'
Expand-Archive $ninjaZip (Join-Path $stage 'ninja') -Force

# the shader tools the game runs at its first start (native/tools + the patched XenosRecomp and DXC), laid out like the Linux bundle (packaging/build-shader-tools.sh)
Write-Host 'shader tools'
$st = Join-Path $stage 'shader-tools\native'
New-Item -ItemType Directory -Force (Join-Path $st 'tools'), (Join-Path $st 'third_party\patches'), (Join-Path $st 'third_party\XenosRecomp\XenosRecomp'), (Join-Path $st 'third_party\build\xenosrecomp\XenosRecomp') | Out-Null
foreach ($f in 'shader_pipeline.py', 'shader_cache_build.py', 'ark_extract.py') { Copy-Item (Join-Path $repo "native\tools\$f") (Join-Path $st 'tools') }
Copy-Item (Join-Path $repo 'native\third_party\patches\*.patch') (Join-Path $st 'third_party\patches')
Copy-Item (Join-Path $repo 'native\third_party\XenosRecomp\XenosRecomp\shader_common.h') (Join-Path $st 'third_party\XenosRecomp\XenosRecomp')
$xrOut = Join-Path $st 'third_party\build\xenosrecomp\XenosRecomp'
foreach ($f in 'XenosRecomp.exe', 'dxcompiler.dll', 'dxil.dll') { Copy-Item (Join-Path $xr $f) $xrOut }
Get-ChildItem $redist.FullName -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $xrOut }   # the C++ runtime, app-local (the player may have no Visual C++ Redistributable)
Get-ChildItem (Join-Path $repo 'native\third_party\XenosRecomp') -Filter 'LICENSE*' -ErrorAction SilentlyContinue | ForEach-Object { Copy-Item $_.FullName (Join-Path $st 'third_party\XenosRecomp') }

# licences of what is carried
Write-Host 'licences'
$lic = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force $lic | Out-Null
Copy-Item (Join-Path $mingwOut 'LICENSE.TXT') (Join-Path $lic 'LLVM-LICENSE.TXT')   # (clang, lld, libc++, libunwind, compiler-rt: Apache-2.0 with LLVM exceptions)
Get-Text 'llvm-mingw-LICENSE.txt' 'https://raw.githubusercontent.com/mstorsjo/llvm-mingw/20261104/LICENSE.txt' (Join-Path $lic 'llvm-mingw-LICENSE.txt')
Get-Text 'mingw-w64-COPYING.txt' 'https://raw.githubusercontent.com/mingw-w64/mingw-w64/master/COPYING' (Join-Path $lic 'mingw-w64-COPYING.txt')
Get-Text 'mingw-w64-COPYING.MinGW-w64.txt' 'https://raw.githubusercontent.com/mingw-w64/mingw-w64/master/COPYING.MinGW-w64/COPYING.MinGW-w64.txt' (Join-Path $lic 'mingw-w64-COPYING.MinGW-w64.txt')
Get-Text 'mingw-w64-COPYING.MinGW-w64-runtime.txt' 'https://raw.githubusercontent.com/mingw-w64/mingw-w64/master/COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt' (Join-Path $lic 'mingw-w64-COPYING.MinGW-w64-runtime.txt')
Get-Text 'Ninja-COPYING.txt' 'https://raw.githubusercontent.com/ninja-build/ninja/v1.12.1/COPYING' (Join-Path $lic 'Ninja-COPYING.txt')
Copy-Item (Join-Path $stage 'python\LICENSE.txt') (Join-Path $lic 'Python-LICENSE.txt')
if (Test-Path (Join-Path $sdkPrefix 'licenses')) { Copy-Tree (Join-Path $sdkPrefix 'licenses') (Join-Path $lic 'rexglue-sdk') }
Set-Content (Join-Path $lic 'README.txt') "Licences of the components this program carries. Also see THIRD_PARTY_NOTICES.md.`r`nllvm-mingw (clang, lld, libc++, libunwind, compiler-rt: LLVM-LICENSE.TXT; the mingw-w64 headers and libraries: mingw-w64-*COPYING.txt) compiles the game module on this computer.`r`nMicrosoft's Visual C++ runtime DLLs (msvcp140*.dll, vcruntime140*.dll) are redistributed app-local under the Visual Studio redistribution terms.`r`nThe DirectX Shader Compiler (dxcompiler.dll, dxil.dll) is used under its own licence (LLVM Release License)."

# the launcher program: reGH2.exe, a small stub that starts the Tk launcher with the package's Python
Write-Host 'reGH2.exe'
$tmp = Join-Path $Out 'stub'
New-Item -ItemType Directory -Force $tmp | Out-Null
& rc /nologo /fo (Join-Path $tmp 'reGH2.res') (Join-Path $PSScriptRoot 'reGH2.rc'); if ($LASTEXITCODE -ne 0) { throw 'rc failed' }
& clang++ -std=c++17 -O2 -fms-runtime-lib=static -municode (Join-Path $PSScriptRoot 'launcher_stub.cpp') (Join-Path $tmp 'reGH2.res') -o (Join-Path $stage 'reGH2.exe') '-Wl,/subsystem:windows' '-Wl,/entry:wWinMainCRTStartup' -luser32
if ($LASTEXITCODE -ne 0) { throw 'compiling reGH2.exe failed' }
Remove-Item $tmp -Recurse -Force

$bytes = (Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("staged {0:N0} MB in {1} files" -f ($bytes / 1MB), (Get-ChildItem $stage -Recurse -File).Count)
if ($StageOnly) { Write-Host "stage: $stage"; return }

# ---- Setup.exe ----------------------------------------------------------------------------------------------------------------------------------------------
Write-Host 'compiling Setup.exe (LZMA2 on the whole folder: a few minutes)'
& $iscc "/DStageDir=$stage" "/DAppVersion=$Version" "/DOutputDir=$Out" /Q (Join-Path $PSScriptRoot 'reGH2.iss')
if ($LASTEXITCODE -ne 0) { throw 'ISCC failed' }
$setup = Join-Path $Out "reGH2-Setup-$Version.exe"
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
Set-Content (Join-Path $Out 'SHA256SUMS.txt') "$hash  reGH2-Setup-$Version.exe" -Encoding ascii
Write-Host ("{0}  ({1:N0} MB)`r`nsha256 {2}" -f $setup, ((Get-Item $setup).Length / 1MB), $hash)
