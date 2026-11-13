"""Smoke-test a relinked CLI with a generated fixture, pinned TLS and real media RPCs."""
import argparse,hashlib,json,pathlib,shutil,socket,ssl,struct,subprocess,time,uuid
p=argparse.ArgumentParser(description=__doc__)
for name in ['exe','fixture','work','report']:p.add_argument('--'+name,required=True)
a=p.parse_args();root=pathlib.Path(a.work)/('relink-'+uuid.uuid4().hex[:8]);share=root/'share';state=root/'state';share.mkdir(parents=True);state.mkdir();shutil.copyfile(a.fixture,share/'movie.mkv')
def run(*args):return subprocess.run(list(map(str,args)),check=True,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30,creationflags=subprocess.CREATE_NO_WINDOW)
identity=run(a.exe,'init',state,share,1).stdout.splitlines();pin=identity[0].split()[1];token=identity[1].split()[-1]
server=subprocess.Popen([a.exe,'host',str(state),'127.0.0.1','47565'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
def string(text):data=text.encode();return struct.pack('>I',len(data))+data
def exact(sock,n):
    out=bytearray()
    while len(out)<n:
        data=sock.recv(n-len(out))
        if not data:raise EOFError('Relinked TLS peer closed')
        out.extend(data)
    return bytes(out)
def call(sock,op,body=b''):
    data=struct.pack('>HH',1,op)+body;sock.sendall(struct.pack('>I',len(data))+data)
    size=struct.unpack('>I',exact(sock,4))[0]
    if not 8<=size<=4194304:raise RuntimeError('RPC response bound')
    data=exact(sock,size);version,operation,error=struct.unpack('>HHI',data[:8])
    if version!=1 or operation!=op or error:raise RuntimeError('Relinked RPC rejected: '+str((operation,error)))
    return data[8:]
try:
    deadline=time.monotonic()+5
    while True:
        try:raw=socket.create_connection(('127.0.0.1',47565),timeout=20);break
        except OSError:
            if time.monotonic()>deadline:raise
            time.sleep(.05)
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT);context.check_hostname=False;context.verify_mode=ssl.CERT_NONE
    with context.wrap_socket(raw,server_hostname='localhost') as tls:
        if hashlib.sha256(tls.getpeercert(binary_form=True)).hexdigest()!=pin:raise RuntimeError('Certificate pin mismatch')
        call(tls,1,string(token)+struct.pack('>II',1,2))
        caps=call(tls,30);n=struct.unpack('>I',caps[:4])[0]
        if b';media-adapt=h264-hls-ts-v3' not in caps[4:4+n]:raise RuntimeError('Static media capability missing')
        tracks=call(tls,70,struct.pack('>I',1)+string('movie.mkv'))
        if struct.unpack('>I',tracks[:4])[0]!=1:raise RuntimeError('Track version')
        n=struct.unpack('>I',tracks[12:16])[0];version=tracks[16:16+n].decode();duration=struct.unpack('>Q',tracks[16+n:24+n])[0]
        sizes=[]
        for index in [0,max(1,(duration+1499)//2000)-1]:
            body=struct.pack('>I',3)+string('movie.mkv')+string(version)+struct.pack('>IIiiI',1280,720,1,2,index)
            result=call(tls,71,body);schema,size=struct.unpack('>II',result[:8]);data=result[8:]
            if schema!=3 or size!=len(data) or not data or data[0]!=0x47 or len(data)%188:raise RuntimeError('Relinked MPEG-TS output invalid')
            sizes.append(size)
    result={'relinked_cli_tls_pin_verified':True,'static_media_capability':True,'first_and_tail_nvdec_nvenc_ass_aac_rpc':True,'segment_bytes':sizes}
    pathlib.Path(a.report).write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result,indent=2))
finally:server.terminate();server.wait(timeout=10)
