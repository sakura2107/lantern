"""Assemble actual sources, patches, licenses, static libraries and MSVC relink objects.

The destination must be outside the repository. This deliberately includes no pairing state,
APK signing key, log, credentials, or compiler PDB.
"""
import argparse, hashlib, json, pathlib, shutil, subprocess, tarfile

def inventory(out):
    manifest={}
    for source in sorted(out.rglob('*')):
        if source.is_file() and source!=out/'MANIFEST.json':
            with source.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
            manifest[source.relative_to(out).as_posix()]={'bytes':source.stat().st_size,'sha256':digest}
    (out/'MANIFEST.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    print('Final SHA-256 inventory written:',out/'MANIFEST.json')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=pathlib.Path,required=True)
    p.add_argument('--inventory-only',action='store_true',help='Finalize the inventory after relink verification')
    names=['repo','build','media','deps','dep-build','downloads','vcpkg']
    for name in names:p.add_argument('--'+name,type=pathlib.Path)
    a=p.parse_args();out=a.out.resolve()
    if a.inventory_only:
        if not out.is_dir():p.error('Package directory does not exist')
        inventory(out);return
    if any(getattr(a,name.replace('-','_')) is None for name in names):p.error('All source/build locations are required')
    repo=a.repo.resolve()
    if out==repo or repo in out.parents:raise SystemExit('Package outside the source repository')
    if out.exists():raise SystemExit('Destination exists; choose a new package directory')
    out.mkdir(parents=True)
    def copy(source,target):
        target=out/target;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
    listing=subprocess.check_output(['git','-C',str(repo),'ls-files','--cached','--others','--exclude-standard','-z'])
    excluded={'.dpapi','.keystore','.pdb','.log','.apk','.idsig','.part'}
    for raw in listing.split(b'\0'):
        if not raw:continue
        name=raw.decode('utf-8');source=repo/name
        if source.is_file() and source.suffix.lower() not in excluded:copy(source,'source/'+name)
    for name in ['lantern_win','lantern_core','qrcodegen']:
        copy(a.build/'Release'/(name+'.lib'),'objects/'+name+'.lib')
    copy(a.build/'third_party/opus-1.5.2/Release/opus.lib','objects/opus.lib')
    for directory,name in [('lantern-host','host_gui.obj'),('lantern-host','host_wizard.res'),('lantern-host','app_icon.res'),('lantern','main.obj')]:
        copy(a.build/(directory+'.dir')/'Release'/name,'objects/'+name)
    for name in ['lantern-host.exe','lantern.exe']:copy(a.build/'Release'/name,'bin/'+name)
    for prefix,target in [(a.media,'media'),(a.deps,'dependencies')]:
        for name in ['include','lib','share']:
            source=prefix/name
            if source.exists():shutil.copytree(source,out/target/name)
    lock=json.loads((repo/'tools/media/sources.lock.json').read_text(encoding='utf-8'))
    names=[lock['ffmpeg']['archive']]+[entry['archive'] for entry in lock['dependency_sources']]
    # Corresponding source archives are supplied together with the binaries, not just remote links.
    for name in names:copy(a.downloads/name,'sources/archives/'+name)
    ports=['libass','fribidi','harfbuzz','freetype','libpng','zlib','bzip2','brotli','ffnvcodec']
    with tarfile.open(out/'sources/patched-dependencies.tar.gz','w:gz') as tar:
        for name in ports:
            roots=list((a.dep_build/name/'src').glob('*'))
            if not roots:raise SystemExit('Missing corresponding patched source: '+name)
            for source in roots:tar.add(source,arcname=name+'/'+source.name,filter=lambda info:None if '/.git/' in info.name else info)
    for name in ports:
        shutil.copytree(a.vcpkg/'ports'/name,out/'sources/vcpkg-ports'/name)
    shutil.copytree(repo/'licenses',out/'licenses')
    copy(repo/'THIRD_PARTY.md','THIRD_PARTY.md');copy(repo/'LICENSE','LICENSE')
    copy(repo/'tools/media/relink.ps1','relink.ps1')
    copy(repo/'tools/media/sources.lock.json','sources.lock.json')
    readme='''Lantern static media source and relink delivery

Run .\\relink.ps1 from an x64 Visual Studio 2022 developer PowerShell prompt.
No application key or user state is needed. Both bin/lantern-host.exe and bin/lantern.exe
can be rebuilt using supplied MIT project objects and the static libraries in media/lib
and dependencies/lib. Pass -MediaPrefix or -DependencyPrefix to use your modified libraries.
Use MSVC /MT-compatible x64 libraries. System Windows SDK import libraries and the MSVC
toolchain are prerequisites and are not redistributed here.

source/ contains the complete Lantern working source used for this build. LGPL sources,
original dependency archives, actual patched vcpkg trees, port patches, notices, and pinned
build recipes are included. See source/docs/MEDIA-BUILD.md for the clean rebuild procedure.
Libraries may be modified and relinked. Reverse engineering for debugging those modifications
is permitted; this package imposes no additional restriction on that right.

The generated PE files are not promised byte-identical (toolchain/PE timestamps differ).
Do not publish a binary separately from this corresponding source and relink delivery.
The UI owner must expose THIRD_PARTY and the included LGPL notices before public release.
'''
    (out/'README.txt').write_text(readme,encoding='utf-8')
    print('Source and relink directory:',out)
    print('Verify relinking, then run --out <directory> --inventory-only before archiving.')

if __name__=='__main__':main()
