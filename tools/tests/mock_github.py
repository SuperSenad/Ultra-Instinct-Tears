"""[UIT 0280] A stand-in for api.github.com for the updater's box tests (tools/update.py reads UIT_UPDATE_API).

    srv = MockGitHub(); srv.start()          # srv.api = 'http://127.0.0.1:PORT'
    srv.add_release('v1.0.0', zip_bytes, body='...', sha='sidecar'|'digest'|'body'|None|'wrong')
    srv.mode = 'ok' | '404' | 'ratelimit' | '401' | 'garbage' | 'stall'
Downloads: browser_download_url = /dl/<tag>/<name> (refuses an Authorization header: GitHub's storage host would
ignore it, and it must never be sent there); url = /assets/<id> (the API: redirects to /dl/... like GitHub).
srv.auth_seen lists (path, header) for every request that carried Authorization.
"""
import hashlib, http.server, json, threading, time


class MockGitHub:
    REPO = 'SuperSenad/Ultra-Instinct-Tears'

    def __init__(self, repo=REPO, tls=None):
        # [UIT 0284] tls=(certfile, keyfile): serve https (a certificate Python does not trust: the curl.exe fallback)
        self.repo, self.releases, self.assets, self.mode = repo, [], {}, 'ok'
        self.auth_seen, self.requests, self.truncate = [], [], False
        srv = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                srv.handle(self)

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
        self.httpd.daemon_threads = True
        scheme = 'http'
        if tls:
            import ssl
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(*tls)
            self.httpd.socket = ctx.wrap_socket(self.httpd.socket, server_side=True)
            scheme = 'https'
        self.api = f'{scheme}://127.0.0.1:{self.httpd.server_address[1]}'

    def start(self):
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        return self

    def stop(self):
        self.httpd.shutdown()

    def add_release(self, tag, data, body='', sha='sidecar', prerelease=False, draft=False, name='UltraInstinctTears-src.zip',
                    extra_assets=()):
        h = hashlib.sha256(data).hexdigest()
        assets = []
        def asset(n, d, **kw):
            aid = len(self.assets) + 1
            self.assets[aid] = (tag, n, d)
            a = {'id': aid, 'name': n, 'size': len(d), 'url': f'{self.api}/repos/{self.repo}/releases/assets/{aid}',
                 'browser_download_url': f'{self.api}/dl/{tag}/{n}', 'content_type': 'application/zip'}
            a.update(kw); assets.append(a)
        asset(name, data, **({'digest': 'sha256:' + h} if sha == 'digest' else {}))
        if sha == 'sidecar':
            asset(name + '.sha256', f'{h}  {name}\n'.encode())
        if sha == 'wrong':
            asset(name + '.sha256', f'{"0" * 64}  {name}\n'.encode())
        if sha == 'body':
            body += f'\n\nSHA-256 ({name}): {h}\n'
        for n, d in extra_assets:
            asset(n, d)
        self.releases.insert(0, {'tag_name': tag, 'name': tag, 'body': body, 'draft': draft, 'prerelease': prerelease,
                                 'published_at': '2026-11-01T12:00:00Z', 'assets': assets,
                                 'html_url': f'https://github.com/{self.repo}/releases/tag/{tag}'})
        return h

    def send(self, w, code, body=b'', ctype='application/json', headers=()):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        w.send_response(code)
        w.send_header('Content-Type', ctype)
        for k, v in headers:
            w.send_header(k, v)
        w.send_header('Content-Length', str(len(body)))
        w.end_headers()
        if self.truncate and ctype == 'application/octet-stream':
            body = body[:len(body) // 2]
        try:
            w.wfile.write(body)
        except OSError:
            pass

    def handle(self, w):
        p = w.path.split('?')[0]
        self.requests.append(w.path)
        if w.headers.get('Authorization'):
            self.auth_seen.append((p, w.headers['Authorization']))
        if p.startswith('/dl/'):
            if w.headers.get('Authorization'):
                return self.send(w, 400, {'message': 'Authorization sent to the storage host'})
            _, _, tag, n = p.split('/', 3)
            for t, name, d in self.assets.values():
                if t == tag and name == n:
                    return self.send(w, 200, d, 'application/octet-stream')
            return self.send(w, 404, {'message': 'Not Found'})
        m = self.mode
        if m == 'stall':
            time.sleep(60); return
        if m == '404':
            return self.send(w, 404, {'message': 'Not Found'})
        if m == '401':
            return self.send(w, 401, {'message': 'Bad credentials'})
        if m == 'ratelimit':
            return self.send(w, 403, {'message': 'API rate limit exceeded'},
                             headers=(('X-RateLimit-Remaining', '0'), ('X-RateLimit-Reset', str(int(time.time()) + 1800))))
        if m == 'garbage':
            return self.send(w, 200, b'<html>login to the hotel wifi</html>', 'text/html')
        base = f'/repos/{self.repo}/releases'
        live = [r for r in self.releases if not r['draft']]
        if p == base + '/latest':
            rel = next((r for r in live if not r['prerelease']), None)
            return self.send(w, 200, rel) if rel else self.send(w, 404, {'message': 'Not Found'})
        if p == base:
            return self.send(w, 200, self.releases)
        if p.startswith(base + '/tags/'):
            rel = next((r for r in live if r['tag_name'] == p[len(base) + 6:]), None)
            return self.send(w, 200, rel) if rel else self.send(w, 404, {'message': 'Not Found'})
        if p.startswith(base + '/assets/'):
            aid = int(p.rsplit('/', 1)[1])
            if aid not in self.assets:
                return self.send(w, 404, {'message': 'Not Found'})
            tag, n, _ = self.assets[aid]
            w.send_response(302); w.send_header('Location', f'{self.api}/dl/{tag}/{n}'); w.send_header('Content-Length', '0'); w.end_headers()
            return
        return self.send(w, 404, {'message': 'Not Found'})
