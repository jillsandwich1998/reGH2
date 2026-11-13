# fetch.ps1 - Windows twin of fetch.sh: clones XenosRecomp at the pinned commit and applies native/third_party/patches/*.patch (idempotent).
# usage: powershell -File native\third_party\fetch.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$url = 'https://github.com/hedge-dev/XenosRecomp.git'
$commit = '990d03b28a27b50277ee5d8d942e1c5f873869d1'
$src = Join-Path $here 'XenosRecomp'

function Invoke-Git { & git.exe @args; if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed" } }
function Test-Git { & git.exe @args *> $null; return $LASTEXITCODE -eq 0 }

# core.autocrlf=false: the patches are LF and must apply byte for byte (a Windows checkout would turn the sources into CRLF).
if (-not (Test-Path (Join-Path $src '.git'))) { Invoke-Git -c core.autocrlf=false clone $url $src }
Invoke-Git -C $src config core.autocrlf false
Invoke-Git -C $src fetch --quiet origin
Invoke-Git -C $src checkout --quiet $commit
Invoke-Git -C $src submodule update --init --recursive --depth 1

# Our patches are applied from LF copies (the working tree of this repository may hold them as CRLF).
$lf = Join-Path ([IO.Path]::GetTempPath()) 'gh2-xr-patches'
Remove-Item $lf -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $lf | Out-Null
foreach ($p in Get-ChildItem (Join-Path $here 'patches') -Filter '*.patch' | Sort-Object Name) {
  $text = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($p.FullName)).Replace("`r`n", "`n")
  $copy = Join-Path $lf $p.Name
  [IO.File]::WriteAllBytes($copy, (New-Object Text.UTF8Encoding($false)).GetBytes($text))
  if (Test-Git -C $src apply --check $copy) {
    Invoke-Git -C $src apply $copy
    Write-Host "applied $($p.Name)"
  } elseif (Test-Git -C $src apply --reverse --check $copy) {
    Write-Host "already applied $($p.Name)"
  } else {
    throw "PATCH DOES NOT APPLY: $($p.Name)"
  }
}
Write-Host ("XenosRecomp at " + (& git.exe -C $src rev-parse --short HEAD) + " + patches")
