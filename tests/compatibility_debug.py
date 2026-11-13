"""Bidirectional Windows compatibility using generated state and old/new CLI binaries."""
import argparse,json,pathlib,subprocess,uuid
p=argparse.ArgumentParser();p.add_argument('--old',required=True);p.add_argument('--new',required=True);p.add_argument('--client-test',required=True);p.add_argument('--work',required=True);p.add_argument('--report',required=True);a=p.parse_args()
root=pathlib.Path(a.work)/('compat-'+uuid.uuid4().hex[:8]);root.mkdir(parents=True)
def run(*args):return subprocess.run(list(map(str,args)),capture_output=True,text=True,encoding='utf-8',errors='replace',check=True,timeout=30,creationflags=subprocess.CREATE_NO_WINDOW)
results={}
for host,client,label,port in [(a.old,a.new,'new_client_old_host',47553),(a.new,a.old,'old_client_new_host',47554)]:
    state=root/label/'state';share=root/label/'share';share.mkdir(parents=True);state.mkdir();(share/'generated.txt').write_text('Lantern compatibility fixture',encoding='utf-8')
    init=run(host,'init',state,share,'31').stdout.splitlines();pin=init[0].split()[1]
    secret=root/label/'test.dpapi';run(host,'secret',state,secret)
    server=subprocess.Popen([host,'host',str(state),'127.0.0.1',str(port)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        # Existing clients retry endpoint connection until the owned listener is ready.
        run(client,'client','127.0.0.1',port,pin,secret,'stat','generated.txt')
        run(client,'client','127.0.0.1',port,pin,secret,'caps')
        if label=='new_client_old_host':run(a.client_test,port,pin,secret)
        results[label]={'file_stat':True,'authenticated_capabilities':True,'typed_av_input':label=='new_client_old_host'}
    finally:server.terminate();server.wait(timeout=10)
pathlib.Path(a.report).write_text(json.dumps(results,indent=2),encoding='utf-8');print(json.dumps(results,indent=2))
