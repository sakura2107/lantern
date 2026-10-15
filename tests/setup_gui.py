"""Focused UI verification: a new generated identity, folder chooser and bandwidth setting."""
import argparse,json,os,subprocess,time,uuid
from pathlib import Path
import windows_gui as gui
u=gui.u
def main():
 p=argparse.ArgumentParser();p.add_argument('--build',required=True);p.add_argument('--output',required=True);a=p.parse_args();build=Path(a.build).resolve();out=Path(a.output).resolve()/uuid.uuid4().hex[:12];out.mkdir(parents=True);share=out/'generated-share';share.mkdir();state=out/'new-state'
 manager=subprocess.Popen([str(build/'lantern-host.exe')],creationflags=subprocess.CREATE_NO_WINDOW,env=dict(os.environ,LANTERN_HOST_MANAGER_SETTINGS=str(out/'host-manager.ini')))
 def owned(cls):
  h=None
  while True:
   h=u.FindWindowExW(None,h,cls,None)
   if not h:return None
   pid=gui.W.DWORD();u.GetWindowThreadProcessId(h,gui.C.byref(pid))
   if pid.value==manager.pid:return h
 try:
  h=gui.wait(lambda:owned('LanternHostManager'),'own manager');c=lambda n:u.GetDlgItem(h,n);gui.wait(lambda:c(35),'new setup control');gui.settext(c(10),str(state));u.PostMessageW(h,0x111,35,0)
  dialog=gui.wait(lambda:owned('#32770'),'own setup dialog');root_field=u.GetDlgItem(dialog,700)
  if not root_field:raise RuntimeError('Own wizard root editor missing')
  gui.settext(root_field,str(share));u.PostMessageW(dialog,0x111,1,0)
  def confirmation():
   d=owned('#32770')
   if d and gui.text(d)=='创建新主机' and u.GetDlgItem(d,6):return d
  confirm=gui.wait(confirmation,'own new-identity confirmation')
  parts=[]
  @gui.C.WINFUNCTYPE(gui.W.BOOL,gui.W.HWND,gui.W.LPARAM)
  def static_text(child,param):
   cls=gui.C.create_unicode_buffer(128);u.GetClassNameW(child,cls,128)
   if cls.value=='Static':parts.append(gui.text(child))
   return True
  u.EnumChildWindows(confirm,static_text,0);prompt='\n'.join(parts)
  if str(share).casefold() not in prompt.casefold():raise RuntimeError('Refuse confirming any root except this generated share; actual setup prompt: '+prompt.replace(str(Path.cwd()),'<workspace>'))
  u.PostMessageW(confirm,0x111,6,0)
  gui.wait(lambda:(state/'identity.dpapi').exists() and (state/'config.dpapi').exists(),'new identity files');gui.check(u.IsWindowEnabled(c(15)) and not u.IsWindowEnabled(c(16)),'graphical first setup loads state without starting network')
  status=gui.text(c(32));gui.check(str(share) in status and '证书指纹' in status,'chosen generated root and identity appear in UI');gui.check(u.SendMessageW(c(27),14,0,0)==0,'first setup does not expose device secret')
  gui.settext(c(36),'24');u.SendMessageW(h,0x111,15,0);gui.wait(lambda:u.IsWindowEnabled(c(16)),'own host start');gui.check(True,'host accepts explicit aggregate bandwidth ceiling')
  gui.settext(c(36),'16');u.SendMessageW(h,0x111,37,0);gui.check(u.IsWindowEnabled(c(16)),'capacity can change while own host stays running');gui.capture(h,out/'windows-setup.png')
  start=time.monotonic();u.PostMessageW(h,0x10,0,0);manager.wait(timeout=10);gui.check(manager.returncode==0,'first configured host closes cleanly')
  result={'success':True,'kind':'measured','assertions_passed':len(gui.checks),'checks':gui.checks,'close_ms':(time.monotonic()-start)*1000,'generated_fixture':str(out),'credentials_logged':False};(out/'setup-gui.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(result,ensure_ascii=True))
 except Exception:
  if (h:=owned('LanternHostManager')):
   status=u.GetDlgItem(h,31)
   if status:print('Own host status: '+gui.text(status),flush=True)
  raise
 finally:
  if manager.poll() is None:
   if (h:=owned('LanternHostManager')):u.PostMessageW(h,0x10,0,0)
   try:manager.wait(timeout=5)
   except subprocess.TimeoutExpired:manager.terminate();manager.wait(timeout=5)
if __name__=='__main__':main()
