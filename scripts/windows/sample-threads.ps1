# Samples where every thread of a running process is (instruction pointer), several times, and resolves the addresses against the executable's PDB with llvm-symbolizer.
# A poor man's profiler for "the game stopped doing anything": the frames of the translated code are named sub_XXXXXXXX after the guest function they came from.
# usage: powershell -File scripts\windows\sample-threads.ps1 -ProcessId <pid> -Exe C:\...\gh2.exe [-Samples 6] [-DelayMs 150]
param(
  [int]$StackBytes = 24576,   # how much of each thread's stack to scan for return addresses into the executable (0 = none)
  [Parameter(Mandatory = $true)][int]$ProcessId,
  [Parameter(Mandatory = $true)][string]$Exe,
  [int]$Samples = 6,
  [int]$DelayMs = 150
)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ThreadProbe {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
  [DllImport("kernel32.dll")] public static extern uint SuspendThread(IntPtr h);
  [DllImport("kernel32.dll")] public static extern uint ResumeThread(IntPtr h);
  [DllImport("kernel32.dll")] public static extern bool GetThreadContext(IntPtr h, IntPtr ctx);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr h, out IntPtr name);
  public static string Describe(uint tid) {
    IntPtr h = OpenThread(0x0800, false, tid);   // QUERY_LIMITED_INFORMATION
    if (h == IntPtr.Zero) return "";
    try { IntPtr n; if (GetThreadDescription(h, out n) == 0 && n != IntPtr.Zero) return Marshal.PtrToStringUni(n); return ""; } finally { CloseHandle(h); }
  }
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, int size, out IntPtr read);
  // CONTEXT (x64) is 1232 bytes, 16-aligned; ContextFlags at +0x30; Rsp at +0x98; Rip at +0xF8. CONTEXT_CONTROL = 0x100001.
  public static bool Sample(uint tid, out ulong rip, out ulong rsp) {
    rip = 0; rsp = 0;
    IntPtr h = OpenThread(0x0002 | 0x0008 | 0x0010, false, tid);   // SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION
    if (h == IntPtr.Zero) return false;
    IntPtr raw = Marshal.AllocHGlobal(1232 + 16);
    try {
      long aligned = (raw.ToInt64() + 15) & ~15L;
      IntPtr ctx = new IntPtr(aligned);
      for (int i = 0; i < 1232; i += 8) Marshal.WriteInt64(ctx, i, 0);
      Marshal.WriteInt32(ctx, 0x30, 0x100001);
      if (SuspendThread(h) == 0xFFFFFFFF) return false;
      bool ok = GetThreadContext(h, ctx);
      if (ok) { rsp = (ulong)Marshal.ReadInt64(ctx, 0x98); rip = (ulong)Marshal.ReadInt64(ctx, 0xF8); }
      ResumeThread(h);
      return ok;
    } finally { Marshal.FreeHGlobal(raw); CloseHandle(h); }
  }
}
'@

$proc = Get-Process -Id $ProcessId
$exeFull = (Resolve-Path $Exe).Path
$module = $proc.Modules | Where-Object { $_.FileName -ieq $exeFull } | Select-Object -First 1
$base = [uint64]$module.BaseAddress.ToInt64()
$size = [uint64]$module.ModuleMemorySize
$ripsByThread = @{}   # tid -> list of rip
$lastRsp = @{}        # tid -> stack pointer of the last sample
for ($i = 0; $i -lt $Samples; $i++) {
  $proc.Refresh()
  foreach ($t in $proc.Threads) {
    [uint64]$rip = 0; [uint64]$rsp = 0
    if ([ThreadProbe]::Sample([uint32]$t.Id, [ref]$rip, [ref]$rsp)) {
      if (-not $ripsByThread.ContainsKey($t.Id)) { $ripsByThread[$t.Id] = New-Object System.Collections.ArrayList }
      [void]$ripsByThread[$t.Id].Add($rip)
      $lastRsp[$t.Id] = $rsp
    }
  }
  Start-Sleep -Milliseconds $DelayMs
}
# resolve: addresses inside the executable through llvm-symbolizer, others by module name
$llvm = Join-Path $env:ProgramFiles 'LLVM\bin\llvm-symbolizer.exe'
$mods = $proc.Modules | ForEach-Object { [pscustomobject]@{ Name = $_.ModuleName; Base = [uint64]$_.BaseAddress.ToInt64(); Size = [uint64]$_.ModuleMemorySize } }
foreach ($tid in ($ripsByThread.Keys | Sort-Object)) {
  "== thread $tid  [" + [ThreadProbe]::Describe([uint32]$tid) + "]"
  $counts = @{}
  foreach ($rip in $ripsByThread[$tid]) {
    $where = $null
    if ($rip -ge $base -and $rip -lt $base + $size) {
      $rva = $rip - $base
      $sym = & $llvm --obj="$exeFull" --relative-address ("0x{0:X}" -f $rva) 2>$null | Select-Object -First 1
      $where = "gh2.exe!$sym"
    } else {
      $m = $mods | Where-Object { $rip -ge $_.Base -and $rip -lt $_.Base + $_.Size } | Select-Object -First 1
      $where = if ($m) { "{0}+0x{1:X}" -f $m.Name, ($rip - $m.Base) } else { "0x{0:X}" -f $rip }
    }
    $counts[$where] = 1 + [int]$counts[$where]
  }
  $counts.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { "   {0,3}x  {1}" -f $_.Value, $_.Key }
  if ($StackBytes -gt 0 -and $lastRsp.ContainsKey($tid)) {
    # read page by page up to the end of what is readable (a single big read fails when it runs past the top of this thread's stack)
    $buf = New-Object byte[] $StackBytes
    $total = 0
    $page = New-Object byte[] 4096
    while ($total + 4096 -le $StackBytes) {
      $got = [IntPtr]::Zero
      $at = [int64]$lastRsp[$tid] + $total
      if (-not [ThreadProbe]::ReadProcessMemory($proc.Handle, [IntPtr]$at, $page, 4096, [ref]$got) -or $got.ToInt64() -ne 4096) { break }
      [Array]::Copy($page, 0, $buf, $total, 4096)
      $total += 4096
    }
    $got = [IntPtr]$total
    if ($total -gt 0) {
      $seen = New-Object System.Collections.ArrayList
      $other = New-Object System.Collections.ArrayList
      for ($o = 0; $o + 8 -le $got.ToInt64(); $o += 8) {
        $v = [BitConverter]::ToUInt64($buf, $o)
        if ($v -ge $base -and $v -lt $base + $size) { [void]$seen.Add($v) }
        else { $m = $mods | Where-Object { $_.Name -match 'rexruntime|rexgpu|SDL3' -and $v -ge $_.Base -and $v -lt $_.Base + $_.Size } | Select-Object -First 1
               if ($m -and $other.Count -lt 8) { [void]$other.Add(("{0}+0x{1:X}" -f $m.Name, ($v - $m.Base))) } }
      }
      $names = foreach ($v in ($seen | Select-Object -First 14)) { (& $llvm --obj="$exeFull" --relative-address ("0x{0:X}" -f ($v - $base)) 2>$null | Select-Object -First 1) }
      if ($other.Count) { "   other modules on the stack: " + ($other -join ', ') }
      if ($names) { "   stack (return addresses into gh2.exe, innermost first):"; $names | ForEach-Object { "      $_" } }
    }
  }
}
