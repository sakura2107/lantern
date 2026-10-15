"""Physical device validation; credentials cross authorized ADB only in memory. No personal files."""
import argparse, pathlib, subprocess, socket, struct, json, time, uuid, base64, threading

p=argparse.ArgumentParser()
p.add_argument('--adb',required=True);p.add_argument('--serial',required=True);p.add_argument('--host-exe',required=True);p.add_argument('--apk',required=True);p.add_argument('--address',required=True);p.add_argument('--work',required=True);p.add_argument('--report',required=True);p.add_argument('--keep-host',action='store_true')
p.add_argument('--port',type=int,default=47443)
p.add_argument('--desktop-only', action='store_true')
p.add_argument('--video-only', action='store_true')
p.add_argument('--control-only', action='store_true')
p.add_argument('--interaction-only', action='store_true')
p.add_argument('--files-media-only',action='store_true')
p.add_argument('--media-fixture')
p.add_argument('--external-app')
p.add_argument('--reliability-only',action='store_true')
p.add_argument('--device-trust-only',action='store_true')
p.add_argument('--saf-stream-only',action='store_true')
p.add_argument('--tools-only',action='store_true')
p.add_argument('--isolated-host')
p.add_argument('--audio-only', action='store_true')
p.add_argument('--audio-tone')
p.add_argument('--audio-duration',type=int,default=15,choices=range(5,61))
p.add_argument('--input-target')
a=p.parse_args()
if a.external_app and '/' not in a.external_app:p.error('--external-app requires package/activity, not a package alone')
root=pathlib.Path(a.work).resolve()/('phone-'+uuid.uuid4().hex[:8]);root.mkdir(parents=True);share=root/'share';share.mkdir();state=root/'state';state.mkdir()
flags=subprocess.CREATE_NO_WINDOW
def adb(*args,**kw):return subprocess.run([a.adb,'-s',a.serial,*args],capture_output=True,check=True,creationflags=flags,**kw)
def command(*args):return subprocess.run(list(map(str,args)),capture_output=True,text=True,check=True,creationflags=flags)
block=bytes((i*179+i//313)%256 for i in range(1024*1024))
with (share/'large.bin').open('wb') as f:
    for _ in range(32):f.write(block)
(share/'readme.txt').write_text('Lantern physical-device test files only. No personal files shared.',encoding='utf-8')
if a.files_media_only:
    import shutil
    fixture=share/'fixture';(fixture/'nested').mkdir(parents=True)
    (fixture/'nested'/'hit.txt').write_text('generated fixture only',encoding='utf-8')
    (fixture/'A.txt').write_text('A',encoding='utf-8')
    bmp=bytearray(58);bmp[0:2]=b'BM';bmp[2]=58;bmp[10]=54;bmp[14]=40;bmp[18]=1;bmp[22]=1;bmp[26]=1;bmp[28]=24;bmp[34]=4;bmp[56]=255
    (fixture/'image.bmp').write_bytes(bmp)
    shutil.copyfile(a.media_fixture,share/'movie.mp4')
initialized=command(a.host_exe,'init',state,share,'31' if (a.saf_stream_only or a.tools_only or a.audio_only or a.files_media_only or a.reliability_only or a.device_trust_only) else '15').stdout.splitlines()
pin=initialized[0].split()[1];token=initialized[1].split()[-1]
phone_id=None;other_secret=None
if a.device_trust_only:
    issued=command(a.host_exe,'device-add',state,'Android validation device','3').stdout.splitlines();phone_id=issued[0].split()[-1];token=issued[-1].split()[-1]
    other=command(a.host_exe,'device-add',state,'Windows validation device','3').stdout.splitlines();other_id=other[0].split()[-1];other_secret=root/'second-device.dpapi'
    command(a.host_exe,'device-secret',state,other_id,other_secret)
target_process=None
tone_process=None
host_args=[a.host_exe,'host',str(state),'0.0.0.0',str(a.port)]
if a.control_only or a.interaction_only or (a.reliability_only and a.input_target):
    if not a.input_target:raise RuntimeError('Owned input canvas executable is required')
    input_log=root/'input-target.jsonl'
    target_args=[a.input_target,str(input_log)]+(['--interaction'] if a.interaction_only else [])
    target_process=subprocess.Popen(target_args,creationflags=flags)
    host_args += ['--input-guard-pid',str(target_process.pid)]
    deadline=time.monotonic()+5
    while not input_log.exists() or not input_log.read_text(encoding='utf-8').strip():
        if target_process.poll() is not None or time.monotonic()>deadline:raise RuntimeError('Owned input canvas failed to start')
        time.sleep(.05)
if a.tools_only:
    if not a.isolated_host:raise RuntimeError('Isolated clipboard host helper required')
    host_args=[a.isolated_host,'--host-lan',str(root),str(a.port)]
host=subprocess.Popen(host_args,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=flags)
test=None;success=False;watcher=None;watcher_stop=threading.Event();restarted=False;restart_errors=[]
trust_applied=[]
try:
    if a.audio_only:
        if not a.audio_tone:raise RuntimeError('Generated audio tone helper is required')
        tone_report=root/'tone-source.json'
        tone_process=subprocess.Popen([a.audio_tone,str(share/'tone-start.bin'),str(tone_report),str(a.audio_duration)],creationflags=flags)
    adb('install','-r',a.apk)
    if a.device_trust_only:
        adb('shell','run-as','org.lantern.client','rm','-f','files/device-trust.request','files/device-trust.ack')
        def edit_device_trust():
            try:
                while not watcher_stop.wait(.1):
                    requested=subprocess.run([a.adb,'-s',a.serial,'shell','run-as','org.lantern.client','cat','files/device-trust.request'],capture_output=True,creationflags=flags)
                    stage=requested.stdout.strip().decode('ascii','ignore')
                    if stage in ['1','2','3'] and stage not in trust_applied:
                        operation={'1':['device-grant',state,phone_id,'1'],'2':['device-end',state,phone_id],'3':['device-remove',state,phone_id]}[stage]
                        command(a.host_exe,*operation);trust_applied.append(stage)
                        adb('shell','run-as','org.lantern.client','sh','-c',f"'echo {stage} > files/device-trust.ack'")
                        if stage=='3':return
            except Exception as error:restart_errors.append(type(error).__name__)
        watcher=threading.Thread(target=edit_device_trust,daemon=True);watcher.start()
    if a.reliability_only:
        adb('shell','run-as','org.lantern.client','rm','-f','files/reliability-restart.request')
        def restart_owned_host():
            global host,restarted
            try:
                while not watcher_stop.wait(.25):
                    found=subprocess.run([a.adb,'-s',a.serial,'shell','run-as','org.lantern.client','cat','files/reliability-restart.request'],capture_output=True,creationflags=flags)
                    if found.returncode==0:
                        host.terminate();host.wait(timeout=10);time.sleep(2)
                        host=subprocess.Popen(host_args,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=flags);restarted=True;return
            except Exception as error:restart_errors.append(type(error).__name__)
        watcher=threading.Thread(target=restart_owned_host,daemon=True);watcher.start()
    if a.address=='127.0.0.1':adb('reverse',f'tcp:{a.port}',f'tcp:{a.port}')
    adb('forward','tcp:47899','localabstract:lantern-device-validation')
    instrument=[a.adb,'-s',a.serial,'shell','am','instrument']
    if a.saf_stream_only:instrument += ['-e','saf_stream_only','true']
    if a.tools_only:instrument += ['-e','tools_only','true']
    if a.desktop_only:instrument += ['-e','desktop_only','true']
    if a.video_only:instrument += ['-e','video_only','true']
    if a.control_only:instrument += ['-e','control_only','true']
    if a.files_media_only:instrument += ['-e','files_media_only','true']
    if a.reliability_only:instrument += ['-e','reliability_only','true']
    if a.device_trust_only:instrument += ['-e','device_trust_only','true']
    if a.external_app:instrument += ['-e','external_app',a.external_app]
    if a.audio_only:instrument += ['-e','audio_only','true']
    if a.interaction_only:
        fixture=json.loads(input_log.read_text(encoding='utf-8').splitlines()[0])
        encoded=base64.b64encode(json.dumps(fixture).encode()).decode()
        instrument += ['-e','interaction_only','true','-e','input_fixture',encoded]
    instrument += ['-w','org.lantern.client/.DeviceTestRunner']
    test=subprocess.Popen(instrument,stdout=subprocess.PIPE,stderr=subprocess.PIPE,creationflags=flags)
    def string(s):b=s.encode('utf-8');return struct.pack('>I',len(b))+b
    provisioning=struct.pack('>I',1)+string(a.address)+struct.pack('>I',a.port)+string(pin)+string(token)
    deadline=time.monotonic()+25
    while True:
        try:
            with socket.create_connection(('127.0.0.1',47899),timeout=2) as s:
                s.sendall(provisioning)
                if s.recv(1)!=b'\x01':raise ConnectionError('Provisioner not ready')
            break
        except OSError:
            if test.poll() is not None or time.monotonic()>deadline:raise RuntimeError('Physical-device instrumentation did not accept provisioning')
            time.sleep(.2)
    stdout,stderr=test.communicate(timeout=240)
    text=stdout.decode('utf-8','replace')
    reports=[line.split('INSTRUMENTATION_RESULT: report=',1)[1] for line in text.splitlines() if line.startswith('INSTRUMENTATION_RESULT: report=')]
    if not reports:raise RuntimeError('No device result: '+text[-1000:])
    result=json.loads(reports[-1]);success=result.get('success',False);result['test_fixture_directory']=str(share)
    result['owned_host_exit_code_during_test']=host.poll()
    if 'INSTRUMENTATION_RESULT: restore_error=' in text:success=False;result['success']=False;result['user_state_restore_failed']=True
    if a.reliability_only:
        result['test_mode']='background_transfer_recovery';result['owned_host_restarted']=restarted;result['restart_errors']=restart_errors
        success=success and restarted and not restart_errors;result['success']=success
    if a.device_trust_only:
        result['test_mode']='per_device_trust';result['host_commands_applied']=trust_applied
        if success:
            command(a.host_exe,'client',a.address,str(a.port),pin,other_secret,'stat','readme.txt');result['other_device_access_after_revoke_verified']=True
        success=success and trust_applied==['1','2','3'] and not restart_errors;result['success']=success
    result['usb_is_only_debug_transport']=a.address!='127.0.0.1'
    if a.files_media_only:result['test_mode']='files_external_apps'
    if a.control_only:result['test_mode']='fullscreen_control'
    if a.interaction_only:result['test_mode']='drag_double_click_ime'
    if a.saf_stream_only:result['test_mode']='saf_stream_upload'
    if a.tools_only:result['test_mode']='foreground_desktop_tools'
    if a.audio_only:
        result['test_mode']='independent_system_audio'
        if tone_report.exists():result['generated_audio_source']=json.loads(tone_report.read_text(encoding='utf-8'))
        if success:
            success=result.get('generated_audio_source',{}).get('waveOutWrite_accepted',False);result['success']=success
    if a.interaction_only and success:
        captured=[json.loads(line) for line in input_log.read_text(encoding='utf-8').splitlines()]
        bounds=captured[0];moves=[c for c in captured if c.get('kind')=='window_move']
        double=[c for c in captured if c.get('kind')=='double_click']
        texts=[c['value'] for c in captured if c.get('kind')=='test_text']
        held=[c['down'] for c in captured if c.get('kind')=='held']
        moved=moves[0] if moves else {}
        dx=moved.get('left',0)-bounds['window_left'];dy=moved.get('top',0)-bounds['window_top']
        move_ok=bool(moves) and abs(dx-result['expected_window_dx'])<=3 and abs(dy-result['expected_window_dy'])<=3
        text_ok=bool(texts) and result['expected_test_text']==texts[-1] and all('zhongwen' not in value for value in texts)
        release_ok=held.count(True)>=3 and held.count(True)==held.count(False) and held[-1] is False
        result.update({'physical_window_move_verified':move_ok,'actual_window_dx':dx,'actual_window_dy':dy,
                       'physical_windows_double_click_verified':len(double)==1,
                       'physical_unicode_text_verified':text_ok,'actual_generated_test_text':texts[-1] if texts else '',
                       'pointer_release_verified':release_ok,'held_button_transitions':held})
        success=move_ok and len(double)==1 and text_ok and release_ok;result['success']=success
    if a.control_only and success:
        captured=[json.loads(line) for line in input_log.read_text(encoding='utf-8').splitlines()]
        bounds=captured[0];clicks=[c for c in captured if c.get('kind')=='click'];expected=result['expected_click_targets']
        observations=[]
        for actual,wanted in zip(clicks,expected):
            dx=abs(actual['x']-wanted['fraction_x']*bounds['width']);dy=abs(actual['y']-wanted['fraction_y']*bounds['height'])
            observations.append({'label':wanted['label'],'expected_x':wanted['fraction_x']*bounds['width'],'expected_y':wanted['fraction_y']*bounds['height'],'actual_x':actual['x'],'actual_y':actual['y'],'error_x_pixels':dx,'error_y_pixels':dy})
        success=len(clicks)==len(expected) and all(max(c['error_x_pixels'],c['error_y_pixels'])<=2 for c in observations)
        result['physical_windows_clicks']=observations;result['physical_windows_clicks_verified']=success;result['success']=success
        result['canvas_click_count']=len(clicks)
    pathlib.Path(a.report).write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(result,ensure_ascii=True,indent=2),flush=True)
    if success and a.keep_host:
        (root/'host-process.json').write_text(json.dumps({'pid':host.pid,'state':str(state),'share':str(share)}),encoding='utf-8')
        print('Test-only host remains running, PID '+str(host.pid),flush=True)
finally:
    watcher_stop.set()
    if watcher is not None:watcher.join(timeout=15)
    if test is not None and test.poll() is None:test.terminate();test.wait(timeout=10)
    adb('forward','--remove','tcp:47899')
    if not(success and a.keep_host):host.terminate();host.wait(timeout=10)
    if target_process is not None and target_process.poll() is None:target_process.terminate();target_process.wait(timeout=10)
    if tone_process is not None and tone_process.poll() is None:tone_process.terminate();tone_process.wait(timeout=10)
if not success:raise SystemExit(1)
