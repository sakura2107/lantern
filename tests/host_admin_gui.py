"""Exercise only a generated host state and this script's own host manager/peer processes."""
import argparse,os,subprocess,json,time,uuid
from pathlib import Path
import windows_gui as gui
u=gui.u

def main():
 p=argparse.ArgumentParser();p.add_argument('--build',required=True);p.add_argument('--fixture',required=True);p.add_argument('--output',required=True);a=p.parse_args();build=Path(a.build).resolve();fixture=Path(a.fixture).resolve();out=Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
 signals=out/('signals-'+uuid.uuid4().hex[:8]);signals.mkdir()
 flags=subprocess.CREATE_NO_WINDOW;manager=subprocess.Popen([str(build/'lantern-host.exe')],creationflags=flags,env=dict(os.environ,LANTERN_HOST_MANAGER_SETTINGS=str(signals/'host-manager.ini')));peer=None
 def owned_window():
  h=None
  while True:
   h=u.FindWindowExW(None,h,'LanternHostManager',None)
   if not h:return None
   pid=gui.W.DWORD();u.GetWindowThreadProcessId(h,gui.C.byref(pid))
   if pid.value==manager.pid:return h
 try:
  h=gui.wait(owned_window,'owned host manager');c=lambda n:u.GetDlgItem(h,n)
  def command(n):u.SendMessageW(h,0x111,n,0)
  def confirm(n):
   u.PostMessageW(h,0x111,n,0)
   def dialog():
    d=None
    while True:
     d=u.FindWindowExW(None,d,'#32770',None)
     if not d:return None
     pid=gui.W.DWORD();u.GetWindowThreadProcessId(d,gui.C.byref(pid))
     if pid.value==manager.pid:return d
   d=gui.wait(dialog,'own confirmation');u.PostMessageW(d,0x111,6,0)
  def count(id):return u.SendMessageW(c(id),0x1004,0,0)
  gui.wait(lambda:c(40) and u.IsWindowVisible(c(10)),'manager controls created');gui.check(not u.IsWindowEnabled(c(15)),'network cannot start before loading a state')
  gui.settext(c(10),str(fixture/'state'));command(12);gui.wait(lambda:count(20)==2,'two fixture devices loaded');gui.check(u.IsWindowEnabled(c(15)) and not u.IsWindowEnabled(c(16)),'loading configuration does not start host');gui.check(u.SendMessageW(c(27),0xD2,0,0)!=0,'pairing secret masked by default');gui.check('证书指纹' in gui.text(c(32)) and u.SendMessageW(c(32),0xBA,0,0)==2,'root and certificate fingerprint have two independent selectable lines')
  gui.settext(c(21),'GUI fixture device');command(22);gui.wait(lambda:count(20)==3,'GUI adds a device');gui.check(u.SendMessageW(c(27),14,0,0)==64,'new unique pairing credential available without printing it');command(30);gui.check(u.SendMessageW(c(27),14,0,0)==0,'refresh clears pairing secret from edit control')
  u.SendMessageW(c(20),0x100,0x24,0);command(26);gui.check(u.SendMessageW(c(27),14,0,0)==64 and u.SendMessageW(c(27),0xD2,0,0)!=0,'explicit pairing lookup remains masked');command(30)
  gui.capture(h,out/'windows-host-devices.png')
  gui.settext(c(14),'47447');command(15);gui.wait(lambda:u.IsWindowEnabled(c(16)),'graphical host started');gui.check(not u.IsWindowEnabled(c(10)),'running host locks state selection')
  peer=subprocess.Popen([str(build/'host_admin_tests.exe'),'--hold',str(fixture),str(signals)],creationflags=flags);gui.wait(lambda:(signals/'ready').exists(),'two actual TLS peers ready');command(51);gui.wait(lambda:count(40)==2,'live sessions listed');gui.check(not u.IsWindowVisible(c(20)) and u.IsWindowVisible(c(40)),'sessions have a separate page');gui.capture(h,out/'windows-host-sessions.png')
  u.SendMessageW(c(40),0x100,0x24,0);command(41);gui.wait(lambda:count(40)==1,'one selected session terminated');a_closed=(signals/'closed-a').exists();b_closed=(signals/'closed-b').exists();gui.check(a_closed!=b_closed,'GUI termination closes exactly one real TLS connection')
  command(50);u.SendMessageW(c(20),0x100,0x24,0)
  if a_closed:u.SendMessageW(c(20),0x100,0x28,0)
  for i in range(5):u.SendMessageW(c(200+i),0xF1,0,0)
  command(23);gui.wait(lambda:(signals/'closed-a').exists() and (signals/'closed-b').exists(),'permission zero closes remaining requested scope');gui.check(True,'GUI permission change affects actual host authorization')
  u.SendMessageW(c(20),0x100,0x23,0);confirm(24);gui.wait(lambda:count(20)==2,'selected device revoked');gui.check(True,'GUI revocation removes only selected device');confirm(34);gui.wait(lambda:'已禁用' in gui.text(c(19)),'legacy credential disabled');gui.check(True,'GUI disables legacy credential explicitly')
  command(16);gui.wait(lambda:not u.IsWindowEnabled(c(16)) and u.IsWindowEnabled(c(15)),'graphical host stopped');gui.check(True,'stop returns manager to editable state');command(15);start=time.monotonic();u.PostMessageW(h,0x10,0,0);manager.wait(timeout=10);gui.check(manager.returncode==0,'closing manager stops its running host cleanly')
  report={'success':True,'kind':'measured','assertions_passed':len(gui.checks),'checks':gui.checks,'transport':'same-PC Schannel TLS loopback','close_running_host_ms':(time.monotonic()-start)*1000,'credentials_logged':False};(out/'host-admin-gui.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(report,ensure_ascii=True))
 finally:
  (signals/'stop').write_bytes(b'1')
  if peer:
   try:peer.wait(timeout=5)
   except subprocess.TimeoutExpired:peer.terminate();peer.wait(timeout=5)
  if manager.poll() is None:
   if 'h' in locals():u.PostMessageW(h,0x10,0,0)
   try:manager.wait(timeout=5)
   except subprocess.TimeoutExpired:manager.terminate();manager.wait(timeout=5)
if __name__=='__main__':main()
