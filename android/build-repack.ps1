# Debug APK without NDK or Gradle: compiles Java, links the manifest and launcher icons with aapt2, then
# Repack keeps the native libraries and assets of an earlier debug APK (BaseApk) and swaps in the rest.
# Java tools cannot open the non-ASCII project path here, so everything is staged in an ASCII work folder.
param(
 [Parameter(Mandatory=$true)][string]$Tools,   # folder with jdk\, build-tools\, platforms\, adb\
 [Parameter(Mandatory=$true)][string]$BaseApk,
 [Parameter(Mandatory=$true)][string]$Keystore,
 [string]$Apk = "$PSScriptRoot\..\..\Lantern-0.1-Android-debug.apk",
 [string]$Work = "$env:TEMP\lantern-repack",
 [switch]$Install
)
$ErrorActionPreference='Stop'
function Check-Exit([string]$Step) { if ($LASTEXITCODE) { throw "$Step failed: $LASTEXITCODE" } }
$Jdk=(Get-ChildItem "$Tools\jdk" -Directory | Select-Object -First 1).FullName
$BuildTools=(Get-ChildItem "$Tools\build-tools" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$AndroidJar=(Get-ChildItem "$Tools\platforms\*\android.jar" | Sort-Object FullName -Descending | Select-Object -First 1).FullName
$Java="$Jdk\bin\java.exe"
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force "$Work\classes","$Work\dex" | Out-Null
Copy-Item -Recurse "$PSScriptRoot\app\src\main\java" "$Work\main"
Copy-Item -Recurse "$PSScriptRoot\app\src\debug\java" "$Work\debug"
Copy-Item -Recurse "$PSScriptRoot\app\src\main\res" "$Work\res"
Copy-Item -Recurse "$PSScriptRoot\app\src\main\assets" "$Work\assets"
$Libs=@(Get-ChildItem "$PSScriptRoot\libs\*.jar" | ForEach-Object { Copy-Item $_.FullName "$Work\$($_.Name)"; "$Work\$($_.Name)" })
Copy-Item "$PSScriptRoot\tools\Repack.java" "$Work\Repack.java"
Copy-Item $BaseApk "$Work\base.apk"
Get-ChildItem "$Work\main","$Work\debug" -Recurse -Filter *.java | ForEach-Object FullName | Set-Content -Encoding ASCII "$Work\sources.txt"
# Tool notes on stderr are not failures in Windows PowerShell; exit codes are checked instead.
$ErrorActionPreference='Continue'
& "$Jdk\bin\javac.exe" --release 17 -encoding UTF-8 -nowarn -classpath ((@($AndroidJar)+$Libs) -join ';') -d "$Work\classes" "@$Work\sources.txt"
Check-Exit 'Java compile'
& "$Jdk\bin\jar.exe" cf "$Work\classes.jar" -C "$Work\classes" .
& $Java -cp "$BuildTools\lib\d8.jar" com.android.tools.r8.D8 --release --min-api 26 --lib $AndroidJar --output "$Work\dex" "$Work\classes.jar" $Libs
Check-Exit 'Dex conversion'
# Same debug manifest as build-standalone.ps1.
$Manifest=Get-Content "$PSScriptRoot\app\src\main\AndroidManifest.xml" -Raw
$Manifest=$Manifest.Replace('<manifest ', '<manifest package="org.lantern.client" ')
$Manifest=$Manifest.Replace('<application ', '<application android:debuggable="true" ')
$Manifest=$Manifest.Replace('</application>', '<provider android:name=".TestDocuments" android:authorities="org.lantern.client.testdocs" android:exported="true" android:permission="android.permission.MANAGE_DOCUMENTS" android:grantUriPermissions="true" /></application>')
$Manifest=$Manifest.Replace('</manifest>', '<instrumentation android:name="org.lantern.client.DeviceTestRunner" android:targetPackage="org.lantern.client" android:label="Lantern device validation" /></manifest>')
Set-Content "$Work\AndroidManifest.xml" $Manifest -Encoding utf8
& "$BuildTools\aapt2.exe" compile --dir "$Work\res" -o "$Work\res.zip"
Check-Exit 'Launcher icon resources'
& "$BuildTools\aapt2.exe" link -I $AndroidJar --manifest "$Work\AndroidManifest.xml" -R "$Work\res.zip" -A "$Work\assets" --min-sdk-version 26 --target-sdk-version 35 --version-code 1 --version-name 0.1.0 -o "$Work\resources.apk"
Check-Exit 'Android resources'
& $Java "$Work\Repack.java" "$Work\base.apk" "$Work\dex\classes.dex" "$Work\resources.apk" "$Work\unsigned.apk"
Check-Exit 'Repack'
& "$BuildTools\zipalign.exe" -f -P 16 4 "$Work\unsigned.apk" "$Work\aligned.apk"
Check-Exit 'APK alignment'
& $Java -jar "$BuildTools\lib\apksigner.jar" sign --ks $Keystore --ks-pass pass:android --key-pass pass:android --out "$Work\signed.apk" "$Work\aligned.apk"
Check-Exit 'APK signing'
& $Java -jar "$BuildTools\lib\apksigner.jar" verify "$Work\signed.apk"
Check-Exit 'APK verification'
Copy-Item -Force "$Work\signed.apk" $Apk
Write-Output "Debug APK built: $Apk"
if ($Install) {
 & "$Tools\adb\platform-tools\adb.exe" install -r --no-incremental $Apk
 Check-Exit 'adb install'
}
