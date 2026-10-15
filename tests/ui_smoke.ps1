param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference='Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class LanternWindowTest {
 public delegate bool EnumCallback(IntPtr h,IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCallback callback,IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint process);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 public static IntPtr Find(uint wanted) {IntPtr result=IntPtr.Zero;EnumWindows((h,p)=>{uint found;GetWindowThreadProcessId(h,out found);if(found==wanted){result=h;return false;}return true;},IntPtr.Zero);return result;}
}
'@
$timer=[Diagnostics.Stopwatch]::StartNew()
$process=Start-Process -FilePath (Resolve-Path $Executable).Path -WindowStyle Hidden -PassThru
try {
 $handle=[IntPtr]::Zero
 while (!$process.HasExited -and $timer.Elapsed.TotalSeconds -lt 10) {
  $handle=[LanternWindowTest]::Find($process.Id)
  if ($handle -ne [IntPtr]::Zero) { break }
  Start-Sleep -Milliseconds 10
 }
 if ($handle -eq [IntPtr]::Zero) { throw 'Client did not create a native window' }
 $created=$timer.Elapsed.TotalMilliseconds
 Start-Sleep -Milliseconds 300
 $process.Refresh()
 $memory=$process.WorkingSet64
 $cpuBefore=$process.TotalProcessorTime.TotalSeconds
 Start-Sleep -Seconds 3
 $process.Refresh()
 $cpuDelta=$process.TotalProcessorTime.TotalSeconds-$cpuBefore
 [LanternWindowTest]::PostMessage($handle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
 if (!$process.WaitForExit(20000)) { throw 'Client did not close cleanly' }
 if ($process.ExitCode -ne 0) { throw 'Client exited with error' }
 [ordered]@{kind='measured';native_window_create_warm_ms=[Math]::Round($created,3);idle_working_set_bytes=$memory;idle_cpu_seconds_during_3s=[Math]::Round($cpuDelta,6);clean_exit=$true;hidden_window=$true;cold_start_tested=$false;connected_rendering_tested=$false} | ConvertTo-Json
} finally { if (!$process.HasExited) { $process.Kill();$process.WaitForExit() };$process.Dispose() }
