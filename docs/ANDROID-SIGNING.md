# Android signing and validation boundaries

The standalone build is a debug validation APK. It compiles all Java sources,
both native ABIs, packages debug-only instrumentation, and verifies the APK and
16 KiB alignment. Its generated manifest permits cleartext loopback tests and
adds TestDocuments; the production AndroidManifest.xml is never rewritten.

For an existing installed app, use its original keystore and verify the signing
certificate before `adb install -r`. Never replace it with a newly generated key
or clear app data to make installation succeed. Device tests save/restore device
and secret preferences plus the transfer journal; independent comparison is
still required. Do not publish that debug APK as the release product.

Example release signing procedure for the UI owner's production build:

```powershell
# Build the release variant without debug instrumentation/resources first.
./gradlew :app:assembleRelease
zipalign -f -P 16 4 app-release-unsigned.apk app-release-aligned.apk
# Store passwords outside the repository, process arguments and logs.
apksigner sign --ks $ReleaseKeystore --ks-key-alias $ReleaseAlias `
  --ks-pass env:LANTERN_STORE_PASSWORD --key-pass env:LANTERN_KEY_PASSWORD `
  --out Lantern-release.apk app-release-aligned.apk
apksigner verify --verbose --print-certs Lantern-release.apk
zipalign -c -P 16 4 Lantern-release.apk
```

Use the established release key. A newly provisioned release identity does not
upgrade a previously installed differently signed app. The release keystore,
passwords, DPAPI state and device backups are never part of source or relink
delivery. No release signing credential is created by this backend work.
