param([string]$Bin, [string]$Out)
# Loopback end-to-end check of the redesigned Windows UIs with generated test data only.
# Usage: powershell -ExecutionPolicy Bypass -File tests\windows_ui_smoke.ps1 -Bin <build>\Release -Out <scratch dir>
# Creates a throwaway host identity + device credential with lantern.exe, starts it on 127.0.0.1, then drives
# lantern-client (pair, list, thumbnail, desktop stream, fullscreen collapse, close) and lantern-host
# (load, start, masked pairing, scope chips, close) by control id, saving screenshots into -Out.
# Credentials never leave the process or the -Out folder; nothing is printed.
# Complements tests/windows_gui.py and tests/host_admin_gui.py, which need Python and their own fixtures.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class U {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder l);
  // WM_GETTEXT: GetWindowText across processes returns an edit's original caption, not its contents.
  public static string Text(IntPtr h){ var s=new StringBuilder(4096); SendMessage(h,0x0D,(IntPtr)4096,s); return s.ToString(); }
}
"@
[U]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$checks = New-Object System.Collections.Generic.List[string]
function Check([bool]$ok, [string]$name) { if (-not $ok) { throw "FAILED: $name" }; $checks.Add($name); Write-Host "ok  $name" }
function Wait([scriptblock]$cond, [string]$name, [int]$seconds = 15) {
  $end = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $end) { if (& $cond) { Check $true $name; return }; Start-Sleep -Milliseconds 200 }
  throw "TIMEOUT: $name"
}
function Shot($h, [string]$name) {
  Start-Sleep -Milliseconds 600
  $r = New-Object U+RECT; [U]::GetWindowRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
  $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [U]::PrintWindow($h, $dc, 2) | Out-Null
  $g.ReleaseHdc($dc); $g.Dispose(); $bmp.Save((Join-Path $Out "$name.png")); $bmp.Dispose()
}
function Cmd($h, [int]$id) { [U]::SendMessage($h, 0x111, [IntPtr]$id, [IntPtr]::Zero) | Out-Null }

New-Item -ItemType Directory -Force $Out | Out-Null
$state = Join-Path $Out 'host-state'; $share = Join-Path $Out 'share'; $client = Join-Path $Out 'client-state'
foreach ($d in $state, $share, $client) { if (Test-Path $d) { Remove-Item -Recurse -Force $d }; New-Item -ItemType Directory $d | Out-Null }
New-Item -ItemType Directory (Join-Path $share '照片') | Out-Null
Set-Content -Encoding UTF8 (Join-Path $share '说明.txt') 'Lantern UI loopback fixture'
$img = New-Object System.Drawing.Bitmap 320, 200; $gg = [System.Drawing.Graphics]::FromImage($img)
$gg.Clear([System.Drawing.Color]::FromArgb(38, 108, 175)); $gg.FillEllipse([System.Drawing.Brushes]::Orange, 110, 50, 100, 100); $gg.Dispose()
$img.Save((Join-Path $share '照片\a-preview.png')); $img.Dispose()
$lantern = Join-Path $Bin 'lantern.exe'
& $lantern init $state $share 31 | Out-Null
$added = (& $lantern device-add $state 'UI loopback' 31) -join "`n"
$secret = [regex]::Match($added, 'PAIRING SECRET[^\n]*?([0-9a-f]{64})').Groups[1].Value
$pin = [regex]::Match($added, 'PIN ([0-9a-f]{64})').Groups[1].Value
Check ($secret.Length -eq 64 -and $pin.Length -eq 64) 'generated host identity and device credential'
$port = 47461
$hostProc = Start-Process $lantern -ArgumentList @('host', $state, '127.0.0.1', "$port") -PassThru -WindowStyle Hidden
Start-Sleep 2

$env:LANTERN_CLIENT_STATE = $client
$app = Start-Process (Join-Path $Bin 'lantern-client.exe') -PassThru
try {
  $w = [IntPtr]::Zero; Wait { $script:w = [U]::FindWindow('LanternClient', [NullString]::Value); $script:w -ne [IntPtr]::Zero } 'client window'
  [U]::SetForegroundWindow($w) | Out-Null
  $ctl = { param($id) [U]::GetDlgItem($w, $id) }
  Check ([U]::IsWindowVisible((& $ctl 1)) -and -not [U]::IsWindowVisible((& $ctl 5))) 'device page shown alone'
  Check ([U]::SendMessage((& $ctl 3), 0xD2, [IntPtr]::Zero, [IntPtr]::Zero) -ne [IntPtr]::Zero) 'pairing secret masked'
  [U]::SendMessage((& $ctl 1), 0x0C, [IntPtr]::Zero, "127.0.0.1:$port") | Out-Null
  [U]::SendMessage((& $ctl 2), 0x0C, [IntPtr]::Zero, $pin) | Out-Null
  [U]::SendMessage((& $ctl 3), 0x0C, [IntPtr]::Zero, $secret) | Out-Null
  Cmd $w 10
  Wait { [U]::Text((& $ctl 6)) -like '*已验证*' } 'pairing verified over TLS'
  Shot $w 'client-device'
  Cmd $w 103
  Check ([U]::IsWindowVisible((& $ctl 5)) -and -not [U]::IsWindowVisible((& $ctl 1))) 'files page independent of desktop'
  Cmd $w 11
  Wait { [U]::SendMessage((& $ctl 5), 0x1004, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -eq 2 } 'root listing without video'
  [U]::SendMessage((& $ctl 4), 0x0C, [IntPtr]::Zero, '照片') | Out-Null; Cmd $w 11
  Wait { [U]::SendMessage((& $ctl 5), 0x1004, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -eq 1 } 'subfolder listing'
  [U]::SendMessage((& $ctl 5), 0x100, [IntPtr]0x24, [IntPtr]::Zero) | Out-Null
  Wait { [U]::SendMessage((& $ctl 66), 0x173, [IntPtr]::Zero, [IntPtr]::Zero) -ne [IntPtr]::Zero } 'remote image thumbnail shown'
  Shot $w 'client-files'
  Cmd $w 104
  Check ([U]::IsWindowVisible((& $ctl 68)) -and [U]::IsWindowVisible((& $ctl 69))) 'task policy controls visible'
  Shot $w 'client-transfers'
  Cmd $w 102; Cmd $w 16
  Wait { [U]::Text((& $ctl 6)) -match 'GPU解码|JPEG' } 'desktop stream connected' 25
  Check ([U]::IsWindowVisible((& $ctl 20)) -and [U]::IsWindowVisible((& $ctl 63))) 'control and clipboard tools visible'
  Start-Sleep 2
  Shot $w 'client-desktop'
  Cmd $w 24
  Check ([U]::IsWindowVisible((& $ctl 20)) -and -not [U]::IsWindowVisible((& $ctl 101))) 'fullscreen keeps tools, hides navigation'
  Shot $w 'client-fullscreen'
  Cmd $w 117
  Check (-not [U]::IsWindowVisible((& $ctl 20)) -and [U]::IsWindowVisible((& $ctl 118))) 'fullscreen toolbar collapses to handle'
  Cmd $w 118
  Check ([U]::IsWindowVisible((& $ctl 20))) 'fullscreen toolbar restored'
  Cmd $w 31
  Check ([U]::IsWindowVisible((& $ctl 5))) 'fullscreen returns to files'
  Cmd $w 102; Cmd $w 16
  $sw = [Diagnostics.Stopwatch]::StartNew()
  [U]::SendMessage($w, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Check ($app.WaitForExit(15000)) 'client closes cleanly'
  Write-Host ("client close {0} ms, exit {1}" -f $sw.ElapsedMilliseconds, $app.ExitCode)
} finally { if (-not $app.HasExited) { $app | Stop-Process -Force } }
$hostProc | Stop-Process -Force
Start-Sleep 1

$env:LANTERN_HOST_MANAGER_SETTINGS = Join-Path $Out 'host-manager.ini'  # never touch the user's remembered settings
$mgr = Start-Process (Join-Path $Bin 'lantern-host.exe') -PassThru
try {
  $m = [IntPtr]::Zero; Wait { $script:m = [U]::FindWindow('LanternHostManager', [NullString]::Value); $script:m -ne [IntPtr]::Zero } 'host manager window'
  $c = { param($id) [U]::GetDlgItem($m, $id) }
  [U]::SendMessage((& $c 10), 0x0C, [IntPtr]::Zero, $state) | Out-Null
  Cmd $m 12
  Wait { [U]::SendMessage((& $c 20), 0x1004, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -eq 1 } 'device list loaded'
  Check ([U]::IsWindowEnabled((& $c 15)) -and -not [U]::IsWindowEnabled((& $c 16))) 'loading does not start host'
  Check ([U]::Text((& $c 32)) -like '*证书指纹*' -and [U]::SendMessage((& $c 32), 0xBA, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -eq 2) 'root and fingerprint lines'
  Shot $m 'host-settings'
  [U]::SendMessage((& $c 14), 0x0C, [IntPtr]::Zero, '47462') | Out-Null
  Cmd $m 15
  Wait { [U]::IsWindowEnabled((& $c 16)) } 'host started from manager'
  Check (-not [U]::IsWindowEnabled((& $c 10))) 'running host locks state selection'
  Cmd $m 50
  [U]::SendMessage((& $c 20), 0x100, [IntPtr]0x24, [IntPtr]::Zero) | Out-Null
  Cmd $m 26
  Check ([U]::SendMessage((& $c 27), 0x0E, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -eq 64 -and [U]::SendMessage((& $c 27), 0xD2, [IntPtr]::Zero, [IntPtr]::Zero) -ne [IntPtr]::Zero) 'pairing lookup stays masked'
  [U]::SendMessage((& $c 202), 0xF1, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Check ([U]::SendMessage((& $c 202), 0xF0, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -eq 0) 'scope chips honour BM_SETCHECK/BM_GETCHECK'
  [U]::SendMessage((& $c 202), 0xF1, [IntPtr]1, [IntPtr]::Zero) | Out-Null
  Shot $m 'host-devices'
  Cmd $m 51
  Shot $m 'host-sessions'
  $sw = [Diagnostics.Stopwatch]::StartNew()
  [U]::SendMessage($m, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Check ($mgr.WaitForExit(15000)) 'manager stops running host and closes'
  Write-Host ("manager close {0} ms" -f $sw.ElapsedMilliseconds)
} finally { if (-not $mgr.HasExited) { $mgr | Stop-Process -Force } }
Write-Host "PASSED $($checks.Count) checks"
