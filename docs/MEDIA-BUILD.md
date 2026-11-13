# Static media build and relink delivery

The optional Windows media stack is FFmpeg 7.1.2, statically linked to the host.
There is no ffmpeg.exe, subprocess conversion, mobile FFmpeg, x264 or x265.
The checked-in lock is [sources.lock.json](../tools/media/sources.lock.json).
The FFmpeg source archive is pinned by SHA-256; dependencies are pinned by
vcpkg commit and source archive hashes. MSVC 19.44.35229/x64, Windows SDK, LLVM
20.1.8 with NVPTX, GNU make/MSYS2, native mingw pkgconf and NASM 3.01 are build
prerequisites. LLVM is a build tool; the NVIDIA driver is supplied by the system.

## Clean build

Use an x64 Visual Studio 2022 developer PowerShell. Allocate an empty SSD work
directory; the script refuses to reuse a configured FFmpeg directory. A vcpkg
checkout at `4bca8fd8654e5ba76f92661db7bfe954768ad8ef` is read-only input.
All dependency installation, sources, downloads, package cache and logs go
under the selected work directory, not into another project's vcpkg installation.

```powershell
# Tool and work locations are deliberately supplied by the developer.
python tools/media/build_media.py --work $MediaWork --vcpkg $VcpkgRoot `
  --build-deps --msys-bin $MsysBin --pkgconf $NativePkgconf --clang $LlvmClang `
  --extra-path $MsysSupplementalBin --extra-path $NasmBin --jobs 4 *> $MediaLog

cmake -S . -B $LanternBuild `
  -DLANTERN_MEDIA_ROOT="$MediaWork/ffmpeg" `
  -DLANTERN_MEDIA_DEPS="$MediaWork/deps/x64-windows-static"
cmake --build $LanternBuild --config Release --parallel 4
```

`--deps-prefix` can reuse an already pinned static dependency prefix instead
of `--build-deps`. That mode verifies a clean FFmpeg rebuild, not a clean
dependency rebuild. Do not represent it as an independent-machine reproduction.
MSYS2_ARG_CONV_EXCL must not disable argument conversion globally: it corrupts
clang's forced-include paths. Native mingw pkgconf is required for drive-letter
paths; the POSIX variant can split them incorrectly.

FFmpeg disables GPL, nonfree, programs, network, shared libraries and autodetect.
It enables the explicitly listed H264/HEVC NVDEC/NVENC, CUDA scaling, selected
audio decoders/AAC encoder, ASS, MKV/MP4/TS demuxers and fragment-capable MP4/HLS
muxers. No CUDA Toolkit/nvcc is used: CUDA filters compile through LLVM NVPTX.
The script checks CONFIG_GPL=0, CONFIG_NONFREE=0, CONFIG_VERSION3=0 and CUDA_LLVM=1
before compilation. `/MT` is used for compatibility with the host. FFmpeg's
normal release optimization is retained; `--extra-cflags` is recorded verbatim.
Changed configure options require a completely new build directory, because
stale allformats.o/movenc.o archives previously hid an enabled muxer.

Disabling LANTERN_MEDIA_ROOT removes adaptation and its capability advertisement.
It does not simulate successful adaptation. Normal file operations and desktop
streaming still work without it.

## Source and relink package

Build the final binaries first, then run:

```powershell
python tools/media/package_relink.py --repo $Repo --build $LanternBuild `
  --media "$MediaWork/ffmpeg" --deps $DependencyPrefix --dep-build $DependencyBuild `
  --downloads $SourceDownloads --vcpkg $VcpkgRoot --out $RelinkDirectory
# Run the supplied recipe against the actual exported objects, then test the CLI.
& "$RelinkDirectory/relink.ps1"
& "$RelinkDirectory/relinked/lantern.exe" # prints usage when invoked without args
python tools/media/verify_relink.py --exe "$RelinkDirectory/relinked/lantern.exe" `
  --fixture $GeneratedMkv --work $TestWork --report $TestReport
python tools/media/package_relink.py --out $RelinkDirectory --inventory-only
```

The package includes host/CLI entry objects and resources, non-LGPL application
static libraries, all media/dependency static libraries and headers, complete
Lantern source, original corresponding dependency archives, actual patched
dependency source trees, vcpkg patches/recipes, licenses, a portable relink
recipe and one SHA-256 inventory. It excludes pairing state, DPAPI files,
keystores, PDBs and local reports. Use a new package destination each time.

Recipients may replace `media` or `dependencies` with their modified `/MT` x64
libraries and run `relink.ps1 -MediaPrefix ... -DependencyPrefix ...`. The
source, objects and recipe are supplied with the binaries under the LGPL 2.1
section 6(a) route; reverse engineering for debugging modifications is allowed.
Lantern source remains MIT; linked LGPL components retain their own licenses.
See [FFmpeg legal](https://ffmpeg.org/legal.html), the included full
[LGPL 2.1 text](../licenses/media/FFmpeg-LGPL-2.1.txt), and [THIRD_PARTY](../THIRD_PARTY.md).
The public download must ship this corresponding source/relink material together
with the binary. Claude must expose the attribution and notices in the release
UI/about flow; this backend work does not edit those UI files.

The procedure is reproducible from pinned inputs; byte-identical PE files are
not promised because compiler versions, archive paths and PE timestamps matter.
Clean dependency rebuild and an independent clean Windows machine remain separate
verification items when only --deps-prefix was used for the recorded run.
