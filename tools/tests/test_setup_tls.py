"""[UIT 0140] Box test for tools/setup.py's downloads (the About > PGO button: "certificate has expired" from Python on
repo.msys2.org): certificates are always verified, Windows' curl.exe is the fallback only after a certificate error,
its command never weakens a check, and the pinned SHA-256 is still checked after it."""
import pathlib, ssl, sys, tempfile, urllib.error, hashlib
HERE = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(HERE / 'tools'))
import setup  # noqa: E402

ok = True
def check(c, what):
    global ok
    print(('ok   ' if c else 'FAIL ') + what); ok &= bool(c)

ctx = setup.tls_context()
check(ctx.verify_mode == ssl.CERT_REQUIRED and ctx.check_hostname, 'the TLS context verifies the chain and the host name')
cmd = setup.curl_command(pathlib.Path('C:/Windows/System32/curl.exe'), 'https://repo.msys2.org/x', pathlib.Path('o'))
bad = {'-k', '--insecure', '--ssl-no-revoke', '--ssl-revoke-best-effort', '--proxy-insecure', '--cacert', '--capath'}
check(not bad & set(cmd), 'curl.exe: no option that weakens or replaces the certificate checks')
check('--fail' in cmd and cmd[cmd.index('--proto') + 1] == '=https' and cmd[cmd.index('--proto-redir') + 1] == '=https',
      'curl.exe: https only (redirects too), fails on HTTP errors')
expired = ssl.SSLCertVerificationError(1, '[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: certificate has expired (_ssl.c:1032)')
check(setup.is_cert_error(urllib.error.URLError(expired)), 'the PGO failure ("certificate has expired" in a URLError) is a certificate error')
check(not setup.is_cert_error(urllib.error.URLError(TimeoutError('timed out'))), 'a timeout is not (no curl fallback for it)')
check(not setup.is_cert_error(OSError('disk full')), 'other errors are not')

# the fallback in download(): only after a certificate error, then the pinned SHA-256 decides
data = b'pinned package bytes'
calls = []
def fake_urlopen(request, timeout):
    raise urllib.error.URLError(expired)
class FakeRun:
    def __init__(self, rc): self.returncode = rc
def fake_run(cmd, **kw):
    calls.append(cmd); pathlib.Path(cmd[cmd.index('--output') + 1]).write_bytes(payload[0]); return FakeRun(0)
payload = [data]
setup.urlopen, setup.windows_curl, setup.subprocess.run, setup.say = fake_urlopen, (lambda: pathlib.Path('curl.exe')), fake_run, (lambda *a: None)
with tempfile.TemporaryDirectory() as d:
    setup.ROOT = pathlib.Path(d)
    digest = hashlib.sha256(data).hexdigest()
    p = setup.fetch_pinned('pkg.tar.zst', len(data), digest, 'https://repo.msys2.org/pkg.tar.zst', 'the PGO tools')
    check(p.read_bytes() == data and len(calls) == 1, 'certificate error -> curl.exe -> SHA-256 matches -> used')
    payload[0] = b'pinned package byteZ'   # same size, other bytes
    (pathlib.Path(d) / 'dev-local/downloads/pkg.tar.zst').unlink()
    try:
        setup.fetch_pinned('pkg.tar.zst', len(data), digest, 'https://repo.msys2.org/pkg.tar.zst', 'the PGO tools'); got = None
    except setup.Fail as e:
        got = str(e)
    check(got and 'expected SHA-256' in got and not (pathlib.Path(d) / 'dev-local/downloads/pkg.tar.zst').exists(),
          'a curl.exe download with other bytes is refused (SHA-256) and not kept')
    setup.windows_curl = lambda: None
    try:
        setup.download('https://repo.msys2.org/x', pathlib.Path(d) / 'x', 'the PGO tools'); got = None
    except setup.Fail as e:
        got = str(e)
    check(got and 'certificate has expired' in got, 'without curl.exe: the certificate error is reported, nothing downloaded')
print('setup tls: all ok' if ok else 'setup tls: FAILED')
sys.exit(0 if ok else 1)
