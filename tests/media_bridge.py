"""Real-process HTTP/TLS byte-range verification. Never prints pairing secrets or temporary URL."""
import sys, subprocess, pathlib, urllib.request, urllib.error, concurrent.futures, time, json, uuid
exe=pathlib.Path(sys.argv[1]).resolve()
work=pathlib.Path(sys.argv[2]).resolve()/('media-'+uuid.uuid4().hex[:8]);work.mkdir(parents=True)
root=work/'share';root.mkdir();state=work/'state';state.mkdir()
data=bytes((i*179+i//313)%256 for i in range(2*1024*1024+73));(root/'source.mkv').write_bytes(data)
flags=subprocess.CREATE_NO_WINDOW if sys.platform=='win32' else 0
def run(*args):return subprocess.run([str(exe),*map(str,args)],capture_output=True,text=True,check=True,creationflags=flags).stdout
pin=run('init',state,root,'1').splitlines()[0].split()[1]
run('secret',state,work/'client.dpapi')
host=subprocess.Popen([str(exe),'host',str(state),'127.0.0.1','47452'],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,creationflags=flags)
bridge=None
try:
    time.sleep(.15)
    bridge=subprocess.Popen([str(exe),'client','127.0.0.1','47452',pin,str(work/'client.dpapi'),'media','source.mkv','47883'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,creationflags=flags)
    line=bridge.stdout.readline()
    if not line.startswith('Original-media URL (local, temporary): '):raise RuntimeError('Media bridge did not start')
    url=line.split(': ',1)[1].strip()
    checks=0
    def get(begin,end):
        request=urllib.request.Request(url,headers={'range':f'bytes={begin}-{end}'})
        with urllib.request.urlopen(request,timeout=10) as r:
            assert r.status==206 and r.read()==data[begin:end+1]
            assert r.headers['Content-Range']==f'bytes {begin}-{end}/{len(data)}'
    with urllib.request.urlopen(urllib.request.Request(url,method='HEAD'),timeout=10) as r:
        assert int(r.headers['Content-Length'])==len(data)
    checks+=1
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda pair:get(*pair),[(0,100),(731,127777),(1000000,1001999),(len(data)-101,len(data)-1)]))
    checks+=4
    with urllib.request.urlopen(urllib.request.Request(url,headers={'Range':'bytes=-73'}),timeout=10) as r:
        assert r.read()==data[-73:]
    checks+=1
    for target,headers,expected in [(url,{'Range':'bytes=1-2junk'},416),(url,{'Range':'bytes=999999999-'},416),(url+'/wrong',{},404)]:
        try:urllib.request.urlopen(urllib.request.Request(target,headers=headers),timeout=10);raise AssertionError('Expected rejection')
        except urllib.error.HTTPError as e:assert e.code==expected
        checks+=1
    print(json.dumps({'kind':'measured','media_bridge_assertions':checks,'actual_original_bytes':len(data),'parallel_consumers':4,'decode_playback_tested':False}))
finally:
    if bridge is not None:bridge.terminate();bridge.wait(timeout=10)
    host.terminate();host.wait(timeout=10)
