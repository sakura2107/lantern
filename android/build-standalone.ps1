param(
 [Parameter(Mandatory=$true)][string]$Jdk,
 [Parameter(Mandatory=$true)][string]$Platform,
 [Parameter(Mandatory=$true)][string]$BuildTools,
 [Parameter(Mandatory=$true)][string]$Ndk,
 [string]$ScratchDir = "$PSScriptRoot\..\..\..\work\android-build",
 [string]$Apk = "$PSScriptRoot\..\..\Lantern-0.1-Android-debug.apk"
)
$ErrorActionPreference='Stop'
function Check-Exit([string]$Step) { if ($LASTEXITCODE) { throw "$Step failed: $LASTEXITCODE" } }
New-Item -ItemType Directory -Force $ScratchDir,"$ScratchDir\classes","$ScratchDir\dex","$ScratchDir\lib\arm64-v8a","$ScratchDir\lib\x86_64" | Out-Null
# Use a fresh Java output directory so removed classes never survive incremental builds.
$LanternJavaClasses=Join-Path $ScratchDir ('classes-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $LanternJavaClasses | Out-Null
$Java=Join-Path $Jdk 'bin\java.exe'
$Jar=Join-Path $Jdk 'bin\jar.exe'
$AndroidJar=Join-Path $Platform 'android.jar'
$Sources=Get-ChildItem "$PSScriptRoot\app\src\main\java","$PSScriptRoot\app\src\debug\java" -Filter '*.java' -Recurse | ForEach-Object FullName
& (Join-Path $Jdk 'bin\javac.exe') --release 17 -encoding UTF-8 -classpath $AndroidJar -d $LanternJavaClasses $Sources
Check-Exit 'Java compile'
$Classes=Get-ChildItem $LanternJavaClasses -Filter '*.class' -Recurse | ForEach-Object FullName
& $Java -cp "$BuildTools\lib\d8.jar" com.android.tools.r8.D8 --release --min-api 26 --lib $AndroidJar --output "$ScratchDir\dex" $Classes
Check-Exit 'Dex conversion'
$Clang=Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
$Core=(Resolve-Path "$PSScriptRoot\..").Path
foreach ($Target in @(@{Abi='arm64-v8a';Triple='aarch64-linux-android26'},@{Abi='x86_64';Triple='x86_64-linux-android26'})) {
 & $Clang "--target=$($Target.Triple)" -shared -fPIC -O2 -std=c++20 -static-libstdc++ '-Wl,-z,max-page-size=16384' "-I$Core\include" "$Core\src\core.cpp" "$PSScriptRoot\app\src\main\cpp\bridge.cpp" -o "$ScratchDir\lib\$($Target.Abi)\liblantern_jni.so"
 Check-Exit 'Native compile'
}
$Manifest=Get-Content "$PSScriptRoot\app\src\main\AndroidManifest.xml" -Raw
$Manifest=$Manifest.Replace('<manifest ', '<manifest package="org.lantern.client" ')
$Manifest=$Manifest.Replace('<application ', '<application android:debuggable="true" ')
$Manifest=$Manifest.Replace('</application>', '<provider android:name=".TestDocuments" android:authorities="org.lantern.client.testdocs" android:exported="true" android:permission="android.permission.MANAGE_DOCUMENTS" android:grantUriPermissions="true" /></application>')
$Manifest=$Manifest.Replace('</manifest>', '<instrumentation android:name="org.lantern.client.DeviceTestRunner" android:targetPackage="org.lantern.client" android:label="Lantern device validation" /></manifest>')
Set-Content "$ScratchDir\AndroidManifest.xml" $Manifest -Encoding utf8
& "$BuildTools\aapt2.exe" compile --dir "$PSScriptRoot\app\src\main\res" -o "$ScratchDir\res.zip"
Check-Exit 'Launcher icon resources'
& "$BuildTools\aapt2.exe" link -I $AndroidJar --manifest "$ScratchDir\AndroidManifest.xml" -R "$ScratchDir\res.zip" --min-sdk-version 26 --target-sdk-version 35 --version-code 1 --version-name 0.1.0 -o "$ScratchDir\unsigned.apk"
Check-Exit 'Android resources'
& $Jar uf "$ScratchDir\unsigned.apk" -C "$ScratchDir\dex" classes.dex -C $ScratchDir lib
Check-Exit 'APK assembly'
if (Test-Path "$PSScriptRoot\app\src\main\assets") {
 & $Jar uf "$ScratchDir\unsigned.apk" -C "$PSScriptRoot\app\src\main" assets
 Check-Exit 'License assets'
}
& "$BuildTools\zipalign.exe" -f -P 16 4 "$ScratchDir\unsigned.apk" "$ScratchDir\aligned.apk"
Check-Exit 'APK alignment'
if (!(Test-Path "$ScratchDir\debug.keystore")) {
 & "$Jdk\bin\keytool.exe" -genkeypair -keystore "$ScratchDir\debug.keystore" -storepass android -keypass android -alias androiddebugkey -dname 'CN=Android Debug,O=Lantern,C=US' -keyalg RSA -keysize 2048 -validity 3650
 Check-Exit 'Debug signing key'
}
& $Java -jar "$BuildTools\lib\apksigner.jar" sign --ks "$ScratchDir\debug.keystore" --ks-pass pass:android --key-pass pass:android --out $Apk "$ScratchDir\aligned.apk"
Check-Exit 'APK signing'
& $Java -jar "$BuildTools\lib\apksigner.jar" verify --verbose $Apk
Check-Exit 'APK verification'
& "$BuildTools\zipalign.exe" -c -P 16 4 $Apk
Check-Exit 'APK alignment verification'
Write-Output "Debug APK built: $Apk. Installation and runtime behavior require a device."
