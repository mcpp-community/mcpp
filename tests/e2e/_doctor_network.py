"""Actual CLI + provider resource checks, with a local HTTP oracle."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import socketserver
import tempfile
import threading
import time

binary = Path(os.environ['MCPP']).resolve()
provider = Path(os.environ.get('MCPP_NETWORK_PROVIDER') or os.environ.get('MCPP_VENDORED_XLINGS')
                or os.environ['XLINGS_BIN']).resolve()

class ResourceServer(http.server.BaseHTTPRequestHandler):
    requests = 0
    def do_GET(self):
        type(self).requests += 1
        body = b'{"indexes":{}}'
        code = 403 if self.path == '/denied' else 404 if self.path.startswith('/cn/') else 200
        self.send_response(code)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            if self.path == '/stall':
                time.sleep(2)
            if self.path == '/trickle':
                for byte in body:
                    self.wfile.write(bytes([byte]))
                    self.wfile.flush()
                    time.sleep(0.1)
            else:
                self.wfile.write(body)
        except OSError:
            pass  # the bounded probe deliberately closes a stalled response
    def log_message(self, *_):
        pass

class RejectTLS(socketserver.BaseRequestHandler):
    client_hellos = 0
    def handle(self):
        self.request.settimeout(2)
        try:
            hello = self.request.recv(1024)
            if hello and hello[0] == 0x16:
                type(self).client_hellos += 1
            self.request.sendall(bytes.fromhex('15030300020228'))  # fatal handshake_failure
        except OSError:
            pass

class TLSServer(socketserver.ThreadingTCPServer):
    daemon_threads = True

tls_server = TLSServer(('127.0.0.1', 0), RejectTLS)
threading.Thread(target=tls_server.serve_forever, daemon=True).start()
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), ResourceServer)
threading.Thread(target=server.serve_forever, daemon=True).start()
try:
    with tempfile.TemporaryDirectory(prefix='mcpp-doctor-network-') as temporary:
        root = Path(temporary)
        home = root / 'home'
        registry = home / 'registry'
        registry.mkdir(parents=True)
        (home / 'config.toml').write_text('[xlings]\nbinary = ' + json.dumps(provider.as_posix()) + '\n', encoding='utf-8')
        base = 'http://127.0.0.1:' + str(server.server_port)
        (registry / '.xlings.json').write_text(json.dumps({'mirror': 'auto', 'index_repos': [{
            'name': 'corp', 'url': base + '/corp.git', 'artifact': {
                'GLOBAL': base + '/global', 'CN': base + '/cn'}}]}), encoding='utf-8')
        env = os.environ | {'MCPP_HOME': str(home), 'XLINGS_HOME': str(registry),
                            'XLINGS_INDEX_BASE_URL': base + '/official', 'MCPP_OFFLINE': '0'}
        handshake = subprocess.run([str(provider), 'interface', '--version'], env=env, cwd=root,
                                   capture_output=True, text=True, encoding='utf-8', timeout=5, check=True)
        assert {'network_diagnostics', 'download_socket_budgets', 'read_only_queries'} <= set(json.loads(handshake.stdout)['features']), handshake.stdout
        def snapshot():
            return {str(p.relative_to(home)): p.read_bytes() if p.is_file() else None
                    for p in home.rglob('*')}
        before = snapshot()
        local_env = env | {name: '' for name in ('HTTPS_PROXY', 'https_proxy', 'ALL_PROXY', 'all_proxy')}
        local_env |= {'NO_PROXY': '*', 'no_proxy': '*'}
        for url, expected, http_status in (
                (base + '/denied', ('http',), 403),
                (base + '/stall', ('timeout',), 200),
                (base + '/trickle', ('timeout',), 200),
                ('https://127.0.0.1:' + str(tls_server.server_address[1]) + '/resource', ('tls', 'connect'), 0)):
            started = time.monotonic()
            response = subprocess.run([str(provider), 'interface', 'probe_url', '--args',
                json.dumps({'url': url, 'socket_timeout_ms': 500})], env=local_env, cwd=root,
                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=3)
            assert response.returncode == 0, (response.stdout, response.stderr)
            data = next(json.loads(line)['payload'] for line in response.stdout.splitlines()
                        if json.loads(line).get('dataKind') == 'network_probe')
            assert data['status'] in expected and not data['reachable'], data
            assert data['http_status'] == http_status, data
            assert time.monotonic() - started < 2, data
            assert snapshot() == before, 'a resource probe changed the home'
        assert RejectTLS.client_hellos == 1, 'the TLS probe never reached the handshake oracle'
        before_doctor_requests = ResourceServer.requests
        started = time.monotonic()
        online = subprocess.run([str(binary), 'self', 'doctor'], env=env, cwd=root, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=8)
        assert time.monotonic() - started < 7, online.stderr
        assert online.returncode == 1, (online.returncode, online.stdout, online.stderr)
        assert 'corp GLOBAL' in online.stderr and ': reachable' in online.stderr, online.stderr
        assert 'corp CN' in online.stderr and ': http' in online.stderr, online.stderr
        assert 'Checking build policy' in online.stderr, online.stderr
        assert snapshot() == before, 'online doctor changed the home'
        requests = ResourceServer.requests
        assert requests - before_doctor_requests >= 3, 'doctor never reached the actual resource server'
        env['MCPP_OFFLINE'] = '1'
        offline = subprocess.run([str(binary), 'self', 'doctor'], env=env, cwd=root, capture_output=True,
                                 text=True, encoding='utf-8', errors='replace', timeout=8)
        assert offline.returncode == 0, (offline.returncode, offline.stdout, offline.stderr)
        assert 'Skipped (offline mode)' in offline.stderr, offline.stderr
        assert ResourceServer.requests == requests, 'offline doctor made a resource request'
        assert snapshot() == before, 'offline doctor changed the home'
        assert not (registry / 'subos').exists(), 'doctor bootstrapped the provider'
finally:
    server.shutdown()
    server.server_close()
    tls_server.shutdown()
    tls_server.server_close()
print('PASS: bounded HTTP/TLS faults, unchanged diagnostic home, and no requests offline')
