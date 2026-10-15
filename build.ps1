param([string]$BuildDir = "$PSScriptRoot\..\..\work\lantern-build", [switch]$Package, [string]$JniTestJdk = '')
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ desktop tools required.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$LanternConfigureArgs = @('-S', $PSScriptRoot, '-B', $BuildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64')
if ($JniTestJdk) { $LanternConfigureArgs += "-DLANTERN_JNI_TEST_JDK=$JniTestJdk" }
& $cmake @LanternConfigureArgs
if ($LASTEXITCODE) { throw 'Configure failed.' }
& $cmake --build $BuildDir --config Release --parallel 4
if ($LASTEXITCODE) { throw 'Build failed.' }
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir $BuildDir -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed.' }
if ($Package) {
 & $cmake --install $BuildDir --config Release --prefix "$PSScriptRoot\package"
 if ($LASTEXITCODE) { throw 'Install failed.' }
 Copy-Item "$PSScriptRoot\README.md", "$PSScriptRoot\LICENSE", "$PSScriptRoot\THIRD_PARTY.md" "$PSScriptRoot\package"
 foreach ($ReportName in @('benchmark.json','ui-smoke.json','device-lan.json','device-lan-before-pacing.json','desktop-before-pacing.json','desktop-profiles.json','desktop-fullscreen.json','desktop-h264.json','hardware-probe.json','device-lan-after-pacing.json','fullscreen-control.json','desktop-h264-control-regression.json','device-lan-control-regression.json','fullscreen-control-interaction-regression.json','desktop-h264-interaction-regression.json','device-lan-interaction-regression.json','desktop-interaction.json','device-lan-final.json','system-audio.json','audio-only-ui.json','files-media.json','files-media-4k.json','files-media-mkv.json','files-media-final.json','system-audio-opus.json','system-audio-opus-final.json','ui-files-external.json','ui-fullscreen-control.json','ui-h264-regression.json','ui-audio-regression.json','ui-interaction-regression.json','background-recovery.json','files-background-regression.json','audio-background-regression.json','device-trust-native.json','device-trust-android.json','background-trust-regression.json','audio-trust-regression.json','windows-client.json','windows-gui.json','windows-features.json','host-admin.json','host-admin-gui.json','android-host-admin-trust.json','android-host-admin-files.json','android-desktop-tools.json','android-saf-streaming.json','android-av-clock.json','windows-av-clock.json','setup-gui.json','tree-recovery.json','continuation-validation.json')) {
  $ReportPath = Join-Path $PSScriptRoot $ReportName
  if (Test-Path -LiteralPath $ReportPath) { Copy-Item -LiteralPath $ReportPath -Destination "$PSScriptRoot\package" }
 }
 Compress-Archive -Path "$PSScriptRoot\package\*" -DestinationPath "$PSScriptRoot\..\Lantern-0.1-Windows-x64.zip" -Force
}
