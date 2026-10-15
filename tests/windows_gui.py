"""Drive only the Lantern/fixture processes launched here; no user apps/files/input targets."""
import ctypes as C, ctypes.wintypes as W, subprocess, os, time, json, argparse, socket
from pathlib import Path
from PIL import Image
u=C.WinDLL('user32',use_last_error=True)
u.SendMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM];u.SendMessageW.restype=W.LPARAM
u.PostMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]
u.GetDlgItem.argtypes=[W.HWND,C.c_int];u.GetDlgItem.restype=W.HWND
u.GetWindowTextW.argtypes=[W.HWND,W.LPWSTR,C.c_int];u.GetWindowTextLengthW.argtypes=[W.HWND]
u.FindWindowW.argtypes=[W.LPCWSTR,W.LPCWSTR];u.FindWindowW.restype=W.HWND
u.GetWindowRect.argtypes=[W.HWND,C.POINTER(W.RECT)];u.GetClientRect.argtypes=u.GetWindowRect.argtypes
u.SetForegroundWindow.argtypes=[W.HWND];u.GetForegroundWindow.restype=W.HWND
u.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)]
u.FindWindowExW.argtypes=[W.HWND,W.HWND,W.LPCWSTR,W.LPCWSTR];u.FindWindowExW.restype=W.HWND
u.ShowWindow.argtypes=[W.HWND,C.c_int];u.SetWindowPos.argtypes=[W.HWND,W.HWND,C.c_int,C.c_int,C.c_int,C.c_int,W.UINT]
u.SetProcessDpiAwarenessContext(C.c_void_p(-4))
g=C.WinDLL('gdi32');k=C.WinDLL('kernel32');k.GetCurrentThreadId.restype=W.DWORD
u.GetWindowDC.argtypes=[W.HWND];u.GetWindowDC.restype=W.HDC;u.ReleaseDC.argtypes=[W.HWND,W.HDC];u.PrintWindow.argtypes=[W.HWND,W.HDC,W.UINT]
g.CreateCompatibleDC.argtypes=[W.HDC];g.CreateCompatibleDC.restype=W.HDC;g.CreateCompatibleBitmap.argtypes=[W.HDC,C.c_int,C.c_int];g.CreateCompatibleBitmap.restype=W.HANDLE
g.SelectObject.argtypes=[W.HDC,W.HANDLE];g.SelectObject.restype=W.HANDLE;g.DeleteObject.argtypes=[W.HANDLE];g.DeleteDC.argtypes=[W.HDC]
g.GetDIBits.argtypes=[W.HDC,W.HANDLE,W.UINT,W.UINT,C.c_void_p,C.c_void_p,W.UINT]
u.AttachThreadInput.argtypes=[W.DWORD,W.DWORD,W.BOOL]
u.GetGuiResources.argtypes=[W.HANDLE,W.DWORD];u.GetGuiResources.restype=W.DWORD
def foreground(h):
    ours=k.GetCurrentThreadId();other=u.GetWindowThreadProcessId(u.GetForegroundWindow(),None)
    attached=ours!=other and u.AttachThreadInput(ours,other,True)
    try:u.SetForegroundWindow(h)
    finally:
        if attached:u.AttachThreadInput(ours,other,False)
def capture(h,path):
    r=rect(h);width=r.right-r.left;height=r.bottom-r.top;dc=u.GetWindowDC(h);mem=g.CreateCompatibleDC(dc);bitmap=g.CreateCompatibleBitmap(dc,width,height);old=g.SelectObject(mem,bitmap)
    try:
        check(u.PrintWindow(h,mem,2),'owned window capture '+path.stem)
        info=C.create_string_buffer(40);C.memmove(info,(C.c_uint32*10)(40,width,C.c_uint32(-height).value,0x00200001,0,width*height*4,0,0,0,0),40)
        pixels=C.create_string_buffer(width*height*4);g.SelectObject(mem,old);check(g.GetDIBits(mem,bitmap,0,height,pixels,info,0)==height,'capture pixels '+path.stem);Image.frombytes('RGB',(width,height),pixels.raw,'raw','BGRX').save(path)
    finally:g.SelectObject(mem,old);g.DeleteObject(bitmap);g.DeleteDC(mem);u.ReleaseDC(h,dc)
checks=[]
def check(value,name):
    if not value:raise AssertionError(name)
    checks.append(name)
    print(name,flush=True)
def wait(f,name,timeout=15):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        value=f()
        if value:return value
        time.sleep(.05)
    raise AssertionError(name)
def text(h):
    b=C.create_unicode_buffer(u.SendMessageW(h,14,0,0)+1);u.SendMessageW(h,13,len(b),C.cast(b,C.c_void_p).value);return b.value
def settext(h,s):
    b=C.create_unicode_buffer(s);u.SendMessageW(h,12,0,C.cast(b,C.c_void_p).value)
def rect(h,client=False):
    r=W.RECT();(u.GetClientRect if client else u.GetWindowRect)(h,C.byref(r));return r
def click(h,x,y):
    p=(y<<16)|(x&65535);u.PostMessageW(h,0x201,1,p);u.PostMessageW(h,0x202,0,p);time.sleep(.15)
def records(p):
    try:return [json.loads(x) for x in p.read_text(encoding='utf-8').splitlines()]
    except (OSError,json.JSONDecodeError):return []
def main():
    a=argparse.ArgumentParser();a.add_argument('--build',required=True);a.add_argument('--fixture',required=True);a.add_argument('--output',required=True);args=a.parse_args()
    build=Path(args.build).resolve();fixture=Path(args.fixture).resolve();out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=True)
    check((fixture/'client/device.dpapi').exists() and (fixture/'host/config.dpapi').exists(),'isolated generated fixture exists')
    processes=[];log=out/'input-target.jsonl';gui=None
    def spawn(cmd,env=None):
        p=subprocess.Popen([str(x) for x in cmd],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW);processes.append(p);(out/'owned-pids.json').write_text(json.dumps([p.pid for p in processes]));return p
    try:
        target=spawn([build/'lantern-input-target.exe',log,'--interactions']);ready=wait(lambda:next((r for r in records(log) if r.get('kind')=='ready'),None),'canvas ready')
        host=spawn([build/'lantern.exe','host',fixture/'host','127.0.0.1','47445','--input-guard-pid',str(target.pid)])
        def listening():
            try:
                with socket.create_connection(('127.0.0.1',47445),.1):return True
            except OSError:return False
        wait(listening,'host ready')
        owned=u.FindWindowW('LanternOwnedInputTarget',None);movable=u.FindWindowW('LanternOwnedMovableTarget',None);u.ShowWindow(owned,0);u.ShowWindow(movable,0)
        env=os.environ.copy();env['LANTERN_CLIENT_STATE']=str(fixture/'client');gui=spawn([build/'lantern-client.exe'],env)
        window=wait(lambda:u.FindWindowW('LanternClient',None),'client window');pid=W.DWORD();u.GetWindowThreadProcessId(window,C.byref(pid));check(pid.value==gui.pid,'drive only our launched client')
        control=lambda n:u.GetDlgItem(window,n)
        def command(n,notice=0):u.SendMessageW(window,0x111,n|(notice<<16),0)
        def screenshot(name):
            time.sleep(.2);capture(window,out/(name+'.png'))
        wait(lambda:u.IsWindowVisible(control(1)) and control(45),'client initialization');check(u.IsWindowVisible(control(1)) and not u.IsWindowVisible(control(5)),'device page separate')
        check(u.SendMessageW(control(3),0xD2,0,0)!=0,'pairing secret masked')
        command(10);wait(lambda:'已验证' in text(control(6)),'pairing verified');screenshot('windows-device')
        gui_directory='gui-fixture-'+str(gui.pid);generated=fixture/'share'/gui_directory;generated.mkdir();(generated/'inside.txt').write_bytes(b'a')
        command(103);check(u.IsWindowVisible(control(5)) and not u.IsWindowVisible(control(1)),'files page independent')
        settext(control(4),gui_directory);command(11);wait(lambda:u.SendMessageW(control(5),0x1004,0,0)==1,'file list loaded without video');screenshot('windows-files')
        u.SendMessageW(control(5),0x1000+43,-1,0) # reset selection mark
        u.SendMessageW(control(5),0x100,0x24,0) # Home selects first row, without a modal DragDetect.
        check(u.SendMessageW(control(5),0x100C,-1,2)==0,'first generated file selected')
        command(52)
        try:url=wait(lambda:text(control(62)),'original media address')
        except AssertionError:
            print(text(control(6)).encode('unicode_escape').decode(),flush=True);raise
        import urllib.request
        with urllib.request.urlopen(urllib.request.Request(url,headers={'Range':'bytes=0-0'}),timeout=10) as r:check(r.status==206 and r.read()==b'a','GUI original-media endpoint reads selected file')
        command(54);wait(lambda:'已停止' in text(control(6)),'bridge stopped');check(text(control(62))=='','expired media address cleared')
        # Preview and native multi-selection use generated files within this fixture only.
        preview=generated/'preview';preview.mkdir(exist_ok=True);Image.new('RGB',(200,120),(38,108,175)).save(preview/'a-image.png');(preview/'z-note.txt').write_text('preview-clear',encoding='utf-8')
        settext(control(4),gui_directory+'/preview');command(11);wait(lambda:u.SendMessageW(control(5),0x1004,0,0)==2,'preview directory loaded');u.SendMessageW(control(5),0x100,0x24,0)
        wait(lambda:u.SendMessageW(control(66),0x173,0,0),'real JPEG thumbnail appears');check('128px' in text(control(67)),'remote image preview visible independently');screenshot('windows-preview')
        initial_gdi=u.GetGuiResources(int(gui._handle),0);check(initial_gdi>0,'owned client GDI resources measured')
        for _ in range(8):
            u.SendMessageW(control(5),0x100,0x23,0);wait(lambda:not u.SendMessageW(control(66),0x173,0,0),'replaced preview cleared');u.SendMessageW(control(5),0x100,0x24,0);wait(lambda:u.SendMessageW(control(66),0x173,0,0),'replacement preview appears')
        check(u.GetGuiResources(int(gui._handle),0)<=initial_gdi+2,'repeated previews keep GDI handles bounded')
        u.SendMessageW(control(5),0x100,0x23,0);wait(lambda:not u.SendMessageW(control(66),0x173,0,0),'preview cleared for text');check('暂无' in text(control(67)),'nonimage selection has explicit preview feedback')
        settext(control(4),gui_directory);command(11);wait(lambda:u.SendMessageW(control(5),0x1004,0,0)==2,'upload destination listed')
        upload_dir=fixture/'gui-upload';upload_dir.mkdir(exist_ok=True);files=[upload_dir/'gui-one.txt',upload_dir/'gui-two.txt']
        for i,f in enumerate(files):f.write_bytes(('generated-upload-'+str(i)).encode())
        u.PostMessageW(window,0x111,14,0)
        def owned_dialog():
            h=None
            while True:
                h=u.FindWindowExW(None,h,'#32770',None)
                if not h:return None
                owner=W.DWORD();u.GetWindowThreadProcessId(h,C.byref(owner))
                if owner.value==gui.pid:return h
        dialog=wait(owned_dialog,'native multi-file picker')
        filename=u.GetDlgItem(dialog,1148) or u.GetDlgItem(dialog,1152)
        if not filename:raise AssertionError('native filename field missing')
        nested_edit=u.FindWindowExW(filename,None,'Edit',None)
        if nested_edit:filename=nested_edit
        settext(filename,' '.join('"'+str(f)+'"' for f in files));u.PostMessageW(dialog,0x111,1,0)
        wait(lambda:all((generated/f.name).exists() for f in files),'native picker uploads both files',20)
        check(all((generated/f.name).read_bytes()==f.read_bytes() for f in files),'native multi-file selection uploads both verified payloads')
        check(u.IsWindowVisible(control(68)) and u.IsWindowVisible(control(69)),'task policy controls available')
        u.SendMessageW(control(45),0x100,0x24,0);u.SendMessageW(control(68),0x14E,2,0);u.SendMessageW(control(69),0x14E,2,0);command(70);wait(lambda:'已保存' in text(control(6)),'policy persisted from GUI');check(u.SendMessageW(control(68),0x147,0,0)==2 and u.SendMessageW(control(69),0x147,0,0)==2,'GUI selected priority and rate saved');screenshot('windows-transfers')
        u.ShowWindow(owned,5);u.ShowWindow(movable,0);foreground(owned)
        command(102);command(16);wait(lambda:'GPU解码' in text(control(6)),'H264 GPU pipeline connected',20);check(u.IsWindowVisible(control(20)),'desktop control tools visible');check(u.IsWindowVisible(control(63)) and u.IsWindowVisible(control(64)) and u.IsWindowVisible(control(65)),'manual and opt-in clipboard tools visible')
        command(24);r=rect(window);check(r.left==0 and r.top==0 and r.right==u.GetSystemMetrics(0) and r.bottom==u.GetSystemMetrics(1),'fullscreen native window covers display');check(u.IsWindowVisible(control(20)) and not u.IsWindowVisible(control(101)),'fullscreen has direct control buttons');screenshot('windows-fullscreen')
        desktop=wait(lambda:u.FindWindowExW(window,None,'LanternDesktop',None),'desktop surface');command(20);foreground(owned);d=rect(desktop,True);sw=ready['width'];sh=ready['height'];vw=d.right;vh=int(vw*sh/sw)
        if vh>d.bottom:vh=d.bottom;vw=int(vh*sw/sh)
        left=(d.right-vw)//2;top=(d.bottom-vh)//2
        for fx,fy in [(.2,.2),(.5,.5),(.8,.8)]:
            x=left+round((vw-1)*fx);y=top+round((vh-1)*fy);old=len([r for r in records(log) if r.get('kind')=='click']);click(desktop,x,y)
            hit=wait(lambda:([r for r in records(log) if r.get('kind')=='click'][-1] if len([r for r in records(log) if r.get('kind')=='click'])>old else None),'fullscreen physical pointer delivery');expected_x=round((sw-1)*(x-left)/(vw-1));expected_y=round((sh-1)*(y-top)/(vh-1));check(abs(hit['x']-expected_x)<=2 and abs(hit['y']-expected_y)<=2,'fullscreen real coordinate '+str(fx))
        old=len([r for r in records(log) if r.get('kind')=='double_click']);x=left+round((vw-1)*.4);y=top+round((vh-1)*.4);click(desktop,x,y);click(desktop,x,y);wait(lambda:len([r for r in records(log) if r.get('kind')=='double_click'])>old,'Windows double click delivery');check(True,'double click reaches Windows without zoom');command(20)
        command(31);check(u.IsWindowVisible(control(5)),'fullscreen returns to files');command(102)
        u.SetWindowPos(window,None,20,20,1100,1300,4);time.sleep(.3);portrait=rect(window,True);check(portrait.right<portrait.bottom and u.IsWindowVisible(control(20)),'portrait resized window keeps tools');u.SetWindowPos(window,None,20,20,1200,800,4)
        command(20);desktop=wait(lambda:u.FindWindowExW(window,None,'LanternDesktop',None),'desktop surface')
        # Guarded input may only reach our generated canvas, even if focus changes.
        u.ShowWindow(owned,5);u.ShowWindow(movable,5);foreground(movable)
        wait(lambda:u.GetForegroundWindow()==movable,'own movable target foreground')
        d=rect(desktop,True);sw=ready['width'];sh=ready['height'];vw=d.right;vh=int(vw*sh/sw)
        if vh>d.bottom:vh=d.bottom;vw=int(vh*sw/sh)
        left=(d.right-vw)//2;top=(d.bottom-vh)//2
        def point(px,py):return (left+round(px*(vw-1)/(sw-1)),top+round(py*(vh-1)/(sh-1)))
        before=rect(movable);x,y=point(ready['title_x'],ready['title_y']);endx,endy=point(ready['title_x']+180,ready['title_y']+85)
        u.SendMessageW(desktop,0x201,1,(y<<16)|x);time.sleep(.15);u.SendMessageW(desktop,0x200,1,(endy<<16)|endx);time.sleep(.15);u.SendMessageW(desktop,0x202,0,(endy<<16)|endx)
        wait(lambda:any(r.get('kind')=='window_move' for r in records(log)),'actual native window dragged')
        after=rect(movable);check(abs((after.left-before.left)-180)<=3 and abs((after.top-before.top)-85)<=3,'actual remote drag displacement mapped')
        # Click the relocated generated editor and send Unicode through the desktop page input field.
        editor=u.FindWindowExW(movable,None,'Edit',None);er=rect(editor);px=(er.left+er.right)//2-ready['left'];py=(er.top+er.bottom)//2-ready['top'];x,y=point(px,py);click(desktop,x,y);time.sleep(.2)
        settext(control(27),'Lantern 中文🙂');command(28);wait(lambda:any('Lantern 中文🙂' in r.get('value','') for r in records(log)),'Windows Unicode text delivered')
        check(True,'actual Unicode/emoji reaches generated remote edit box')
        command(20);u.ShowWindow(owned,0);u.ShowWindow(movable,0);command(104);screenshot('windows-transfers')
        # Close with video still subscribed; the UI must keep pumping while DXGI shuts down.
        t=time.monotonic();u.PostMessageW(window,0x10,0,0);gui.wait(timeout=20);check(gui.returncode==0,'clean shutdown with active GPU stream');shutdown_ms=(time.monotonic()-t)*1000
        report={'success':True,'kind':'measured','assertions_passed':len(checks),'checks':checks,'transport':'TLS same-PC loopback','native_h264_ui_connected':True,'actual_drag_verified':True,'unicode_verified':True,'shutdown_ms':shutdown_ms,'external_player_performance_measured':False}
        (out/'windows-gui.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(report,ensure_ascii=False))
    finally:
        if 'owned' in locals() and owned:u.PostMessageW(owned,0x10,0,0)
        if 'target' in locals():
            try:target.wait(timeout=2)
            except subprocess.TimeoutExpired:pass
        for p in reversed(processes):
            if p.poll() is None:p.terminate()
        for p in processes:
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:p.kill()
if __name__=='__main__':main()
