param(
 [string]$MediaPrefix = "$PSScriptRoot\media",
 [string]$DependencyPrefix = "$PSScriptRoot\dependencies",
 [string]$OutputDir = "$PSScriptRoot\relinked"
)
$ErrorActionPreference='Stop'
# Run from an x64 Visual Studio developer prompt. Replace either library prefix to test modifications.
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$project=Join-Path $PSScriptRoot 'objects'
$libraries=@('lantern_win.lib','lantern_core.lib','qrcodegen.lib','opus.lib') | ForEach-Object {Join-Path $project $_}
foreach($name in 'avformat','avfilter','avcodec','swresample','swscale','avutil'){
 $path=Join-Path $MediaPrefix "lib\lib$name.a"
 if(!(Test-Path -LiteralPath $path)){$path=Join-Path $MediaPrefix "lib\$name.lib"}
 $libraries+=$path
}
foreach($name in 'ass','fribidi','harfbuzz','freetype','bz2','libpng16','zs','brotlidec','brotlicommon'){$libraries+=Join-Path $DependencyPrefix "lib\$name.lib"}
$system=@('shlwapi','usp10','dwrite','ws2_32','secur32','crypt32','bcrypt','ncrypt','ole32','oleaut32','advapi32','windowscodecs','gdi32','user32','shell32','d3d11','dxgi','iphlpapi','winhttp','xmllite','kernel32','winspool','uuid','comdlg32','comctl32') | ForEach-Object {"$_.lib"}
foreach($kind in 'host','cli'){
 $name=if($kind -eq 'host'){'lantern-host'}else{'lantern'}
 $entry=if($kind -eq 'host'){'host_gui.obj'}else{'main.obj'}
 $subsystem=if($kind -eq 'host'){'WINDOWS'}else{'CONSOLE'}
 $options=@('/NOLOGO','/INCREMENTAL:NO','/MACHINE:X64','/DYNAMICBASE','/NXCOMPAT','/MANIFEST:EMBED',"/SUBSYSTEM:$subsystem",('/OUT:'+ (Join-Path $OutputDir "$name.exe")),(Join-Path $project $entry),(Join-Path $project 'app_icon.res'))
 if($kind -eq 'host'){$options+=(Join-Path $project 'host_wizard.res');$options+=('/MANIFESTINPUT:'+(Join-Path $PSScriptRoot 'source\src\client.manifest'))}
 foreach($path in @($libraries)+(Join-Path $project $entry)){if(!(Test-Path -LiteralPath $path)){throw "Required relink input missing: $path"}}
 & link.exe @options @libraries @system
 if($LASTEXITCODE){throw "Relink $name failed: $LASTEXITCODE"}
}
Write-Output "Relinked host and CLI: $OutputDir. Use only generated test files when validating modifications."
