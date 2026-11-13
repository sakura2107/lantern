"""Build the pinned LGPL media stack in an isolated directory, using MSVC /MT.

Run from an x64 VS developer prompt. Tool paths are arguments, never machine defaults.
Fresh FFmpeg build directories are mandatory: configuration changes can leave stale archives.
"""
import argparse, hashlib, json, os, pathlib, shlex, subprocess, tarfile, urllib.request

HERE = pathlib.Path(__file__).resolve().parent

def sha(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=pathlib.Path, required=True)
    p.add_argument('--vcpkg', type=pathlib.Path, required=True)
    p.add_argument('--deps-prefix', type=pathlib.Path)
    p.add_argument('--build-deps', action='store_true')
    p.add_argument('--msys-bin', type=pathlib.Path, required=True)
    p.add_argument('--pkgconf', type=pathlib.Path, required=True, help='Native mingw pkgconf.exe')
    p.add_argument('--clang', type=pathlib.Path, required=True, help='LLVM 20.1.8 with NVPTX')
    p.add_argument('--extra-path', action='append', default=[])
    p.add_argument('--jobs', type=int, default=4, choices=range(1,9))
    a = p.parse_args()
    lock = json.loads((HERE/'sources.lock.json').read_text(encoding='utf-8'))
    root = a.work.resolve(); root.mkdir(parents=True, exist_ok=True)
    build = root/'ffmpeg-build'
    if build.exists():
        raise SystemExit('Use a new --work directory; never reuse configured FFmpeg objects.')
    env = dict(os.environ)
    env['PATH'] = os.pathsep.join([str(a.msys_bin.resolve()), *a.extra_path, env['PATH']])
    env.pop('MSYS2_ARG_CONV_EXCL', None)
    env['VCPKG_MAX_CONCURRENCY'] = str(a.jobs)
    downloads = root/'downloads'; downloads.mkdir(exist_ok=True)
    env['VCPKG_DOWNLOADS'] = str(downloads)
    cache = root/'binary-cache'; cache.mkdir(exist_ok=True)
    env['VCPKG_DEFAULT_BINARY_CACHE'] = str(cache)
    def run(args, **kwargs):
        subprocess.run(list(map(str,args)), check=True, env=env, **kwargs)
    commit = subprocess.check_output(['git','-C',str(a.vcpkg),'rev-parse','HEAD'],text=True).strip()
    if commit != lock['vcpkg_commit']:
        raise SystemExit('vcpkg checkout must match sources.lock.json; do not mutate shared checkouts.')
    version = subprocess.check_output([str(a.clang),'--version'],text=True)
    if '20.1.8' not in version: raise SystemExit('Expected LLVM 20.1.8 with NVPTX')
    if a.build_deps:
        run([a.vcpkg/'vcpkg.exe','install','libass:x64-windows-static','ffnvcodec:x64-windows-static',
             '--x-install-root='+str(root/'deps'),'--x-buildtrees-root='+str(root/'dep-build'),
             '--x-packages-root='+str(root/'dep-packages'),'--downloads-root='+str(downloads)])
    deps = (a.deps_prefix or root/'deps'/'x64-windows-static').resolve()
    if not (deps/'lib'/'ass.lib').is_file(): raise SystemExit('Missing static dependency prefix')
    archive = downloads/lock['ffmpeg']['archive']
    if not archive.exists():
        urllib.request.urlretrieve(lock['ffmpeg']['url'],archive)
    if sha(archive) != lock['ffmpeg']['sha256']: raise SystemExit('FFmpeg archive checksum mismatch')
    with tarfile.open(archive) as package:
        package.extractall(root, filter='data')
    source = root/'ffmpeg-7.1.2'; build.mkdir()
    prefix = root/'ffmpeg'
    env['PKG_CONFIG_PATH'] = env['PKG_CONFIG_LIBDIR'] = (deps/'lib'/'pkgconfig').as_posix()
    args = [str(source/'configure'), '--prefix='+prefix.as_posix(),'--toolchain=msvc',
        '--disable-debug','--disable-doc','--disable-programs','--disable-network','--disable-autodetect',
        '--disable-gpl','--disable-nonfree','--enable-static','--disable-shared','--disable-everything',
        '--enable-avcodec','--enable-avformat','--enable-avfilter','--enable-swresample','--enable-swscale',
        '--enable-demuxer=matroska,mov,mpegts',
        '--enable-decoder=h264,hevc,h264_cuvid,hevc_cuvid,aac,ac3,eac3,flac,mp3,opus,vorbis,pcm_s16le,ass,ssa,subrip',
        '--enable-encoder=h264_nvenc,hevc_nvenc,aac','--enable-parser=h264,hevc,aac,ac3,flac,mpegaudio,opus,vorbis',
        '--enable-muxer=hls,mpegts,mov,mp4,adts','--enable-protocol=file',
        '--enable-filter=buffer,buffersink,scale_cuda,hwdownload,hwupload_cuda,format,subtitles,ass,aresample,aformat,abuffer,abuffersink',
        '--enable-nvenc','--enable-nvdec','--enable-cuvid','--enable-ffnvcodec','--enable-cuda','--enable-cuda-llvm',
        '--nvcc='+a.clang.resolve().as_posix(),'--enable-libass','--enable-zlib',
        '--pkg-config='+a.pkgconf.resolve().as_posix(),'--pkg-config-flags=--static',
        '--nvccflags=--cuda-gpu-arch=sm_75 -O2',
        '--extra-cflags=-MT -O1 -I'+(deps/'include').as_posix(),
        '--extra-ldflags=-LIBPATH:'+(deps/'lib').as_posix()]
    # bash needs a POSIX source path, while native compiler/prefix arguments retain drive letters.
    configure = subprocess.check_output([str(a.msys_bin/'cygpath.exe'),'-u',str(source/'configure')],text=True).strip()
    args[0] = configure
    (root/'configure-arguments.json').write_text(json.dumps(args,indent=2),encoding='utf-8')
    run([a.msys_bin/'bash.exe','--noprofile','--norc','-c',' '.join(shlex.quote(x) for x in args)],cwd=build)
    config = (build/'config.h').read_text()
    for setting in ['CONFIG_GPL 0','CONFIG_NONFREE 0','CONFIG_VERSION3 0','CONFIG_CUDA_LLVM 1']:
        if '#define '+setting not in config: raise SystemExit('Unexpected media license/config: '+setting)
    run([a.msys_bin/'bash.exe','--noprofile','--norc','-c',f'make -j{a.jobs} && make install'],cwd=build)
    print('Pinned static LGPL media installed:',prefix)

if __name__ == '__main__': main()
