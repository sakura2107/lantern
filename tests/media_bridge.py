"""Real-process HTTP/TLS byte-range and loopback Host validation. Never prints pairing secrets or temporary URL."""
import sys, subprocess, pathlib, urllib.request, urllib.error, urllib.parse, concurrent.futures, time, json, uuid, socket, tempfile, shutil

def parse_args(argv):
    if len(argv) >= 3 and argv[1] == '--cli':
        exe = pathlib.Path(argv[2]).resolve()
        if len(argv) >= 4:
            return exe, pathlib.Path(argv[3]).resolve(), False
        return exe, pathlib.Path(tempfile.gettempdir()).resolve(), True
    if len(argv) < 2:
        raise SystemExit('Usage: media_bridge.py [--cli] <lantern.exe> [work_dir]')
    exe = pathlib.Path(argv[1]).resolve()
    if len(argv) >= 3:
        return exe, pathlib.Path(argv[2]).resolve(), False
    return exe, pathlib.Path(tempfile.gettempdir()).resolve(), True

exe, work_base, cleanup_work = parse_args(sys.argv)
work = work_base / ('media-' + uuid.uuid4().hex[:8])
work.mkdir(parents=True)
root = work / 'share'; root.mkdir(); state = work / 'state'; state.mkdir()
data = bytes((i * 179 + i // 313) % 256 for i in range(2 * 1024 * 1024 + 73))
(root / 'source.mkv').write_bytes(data)
flags = subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0
def run(*args): return subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, check=True, creationflags=flags).stdout
pin = run('init', state, root, '1').splitlines()[0].split()[1]
run('secret', state, work / 'client.dpapi')
host = subprocess.Popen([str(exe), 'host', str(state), '127.0.0.1', '47452'], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, creationflags=flags)
bridge = None

def raw_http(port, payload, half_close=True):
    chunks = []
    with socket.create_connection(('127.0.0.1', port), timeout=10) as s:
        s.settimeout(10)
        try:
            s.sendall(payload)
            if half_close:
                s.shutdown(socket.SHUT_WR)
        except (BrokenPipeError, ConnectionResetError):
            pass
        while True:
            try:
                chunk = s.recv(65536)
            except ConnectionResetError:
                break
            if not chunk:
                break
            chunks.append(chunk)
    raw = b''.join(chunks)
    if b'\r\n\r\n' not in raw:
        return None, {}, b''
    head, body = raw.split(b'\r\n\r\n', 1)
    lines = head.decode('latin-1').split('\r\n')
    status = int(lines[0].split(' ', 2)[1]) if len(lines[0].split(' ', 2)) >= 2 else None
    headers = {}
    for line in lines[1:]:
        if ':' in line:
            k, v = line.split(':', 1)
            headers[k.strip().lower()] = v.strip()
    return status, headers, body

try:
    time.sleep(.15)
    bridge = subprocess.Popen([str(exe), 'client', '127.0.0.1', '47452', pin, str(work / 'client.dpapi'), 'media', 'source.mkv', '47883'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, creationflags=flags)
    line = bridge.stdout.readline()
    if not line.startswith('Original-media URL (local, temporary): '): raise RuntimeError('Media bridge did not start')
    url = line.split(': ', 1)[1].strip()
    parsed = urllib.parse.urlsplit(url)
    assert parsed.scheme == 'http' and parsed.hostname == '127.0.0.1' and parsed.port is not None
    port = parsed.port
    authority = f'127.0.0.1:{port}'
    path = parsed.path
    wrong_port = port - 1 if port == 65535 else port + 1
    checks = 0
    def get(begin, end):
        request = urllib.request.Request(url, headers={'range': f'bytes={begin}-{end}'})
        with urllib.request.urlopen(request, timeout=10) as r:
            assert r.status == 206 and r.read() == data[begin:end + 1]
            assert r.headers['Content-Range'] == f'bytes {begin}-{end}/{len(data)}'
    with urllib.request.urlopen(urllib.request.Request(url, method='HEAD'), timeout=10) as r:
        assert int(r.headers['Content-Length']) == len(data)
    checks += 1
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda pair: get(*pair), [(0, 100), (731, 127777), (1000000, 1001999), (len(data) - 101, len(data) - 1)]))
    checks += 4
    with urllib.request.urlopen(urllib.request.Request(url, headers={'Range': 'bytes=-73'}), timeout=10) as r:
        assert r.read() == data[-73:]
    checks += 1
    for target, headers, expected in [(url, {'Range': 'bytes=1-2junk'}, 416), (url, {'Range': 'bytes=999999999-'}, 416), (url + '/wrong', {}, 404)]:
        try: urllib.request.urlopen(urllib.request.Request(target, headers=headers), timeout=10); raise AssertionError('Expected rejection')
        except urllib.error.HTTPError as e: assert e.code == expected
        checks += 1

    # WP-SEC-07: Frozen Host acceptance table checks over raw loopback TCP (never logs bearer URL).
    status, headers, body = raw_http(port, f'GET {path} HTTP/1.1\r\nHost: {authority}\r\n\r\n'.encode('ascii'))
    assert status == 200 and body == data, 'valid GET with exact Host failed'
    checks += 1

    status, headers, body = raw_http(port, f'GET {path} HTTP/1.1\r\nhOsT: \t {authority} \t\r\nRange: bytes=0-63\r\n\r\n'.encode('ascii'))
    assert status == 206 and body == data[:64] and headers.get('content-range') == f'bytes 0-63/{len(data)}', 'case-insensitive Host with SP/HTAB failed'
    checks += 1

    status, headers, body = raw_http(port, f'HEAD {path} HTTP/1.1\r\nHOST:\t{authority} \r\n\r\n'.encode('ascii'))
    assert status == 200 and body == b'' and headers.get('content-length') == str(len(data)), 'valid HEAD with trimmed Host failed'
    checks += 1

    prefix = f'GET {path} HTTP/1.1\r\nHost: {authority}\r\nRange: bytes=0-31\r\nX-Pad: '.encode('ascii')
    suffix = b'\r\n\r\n'
    exact_8192 = prefix + b'a' * (8192 - len(prefix) - len(suffix)) + suffix
    assert len(exact_8192) == 8192
    status, headers, body = raw_http(port, exact_8192)
    assert status == 206 and body == data[:32], 'exact 8192-byte header rejected'
    checks += 1

    over_8192 = prefix + b'a' * (8193 - len(prefix) - len(suffix)) + suffix
    assert len(over_8192) == 8193
    rejected_cases = [
        ('missing_host', f'GET {path} HTTP/1.1\r\n\r\n'.encode('ascii')),
        ('duplicate_host', f'GET {path} HTTP/1.1\r\nHost: {authority}\r\nHOST: {authority}\r\n\r\n'.encode('ascii')),
        ('bare_ip', f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n'.encode('ascii')),
        ('localhost_alias', f'GET {path} HTTP/1.1\r\nHost: localhost:{port}\r\n\r\n'.encode('ascii')),
        ('wrong_port', f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{wrong_port}\r\n\r\n'.encode('ascii')),
        ('truncated_header', f'GET {path} HTTP/1.1\r\nHost: {authority}\r\n'.encode('ascii')),
        ('oversized_header', over_8192),
    ]
    for label, req in rejected_cases:
        status, _, body = raw_http(port, req, half_close=True)
        assert status not in (200, 206) and body == b'', f'host validation failed to reject {label}'
        checks += 1

    print(json.dumps({'kind': 'measured', 'media_bridge_assertions': checks, 'actual_original_bytes': len(data), 'parallel_consumers': 4, 'host_validation_cases': len(rejected_cases) + 4, 'decode_playback_tested': False}))
finally:
    if bridge is not None: bridge.terminate(); bridge.wait(timeout=10)
    host.terminate(); host.wait(timeout=10)
    if cleanup_work: shutil.rmtree(work, ignore_errors=True)
