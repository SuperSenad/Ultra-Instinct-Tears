# Releasing Ultra Instinct Tears

The launcher's **About > Install update** asks GitHub for the newest release of
`SuperSenad/Ultra-Instinct-Tears` (the "latest" release; pre-releases only when
**Advanced > Updates and reports > Include pre-releases** is on), compares its tag
with the installed version, shows the release notes as "What's new" and, on
Install, downloads the release's source zip, checks it and installs it exactly
like a zip chosen by hand (backup, rebuild, rollback on failure).

So a release needs three things, all checked by the updater:

| What | Rule |
|---|---|
| Tag | `vX.Y.Z`, or `vX.Y.Z-rc1` etc. for a pre-release (marked *pre-release* on GitHub). Other tags are ignored. |
| Asset | `UltraInstinctTears-src.zip`, made by `tools/make_source_zip.py` on the tagged commit. Its `VERSION.txt` must name the tag's version (the updater refuses a mismatch). |
| Checksum | `UltraInstinctTears-src.zip.sha256` next to it (written by the same script). Optional, but without it only the size is checked. |

## Steps (v1.0.0 as the example)

1. Everything committed, tests green:
   ```
   python3 tools/tests/test_update.py
   python3 tools/tests/test_update_github.py
   python3 tools/tests/test_update_curl.py
   python3 tools/test_launcher.py build/launcher/ultrainstincttears-launcher
   ```
2. Tag the commit (annotated):
   ```
   git tag -a v1.0.0 -m "Ultra Instinct Tears 1.0.0"
   ```
3. Make the package on the tag (the version comes from the tag):
   ```
   python3 tools/make_source_zip.py --out dist
   ```
   It writes `dist/UltraInstinctTears-src.zip` and `dist/UltraInstinctTears-src.zip.sha256`
   and prints both. Check: `cd dist && sha256sum -c UltraInstinctTears-src.zip.sha256`
   and `python3 tools/update.py scan --file dist/UltraInstinctTears-src.zip`
   (it must say `1.0.0`).
4. Push the tag (`git push origin v1.0.0`) and create the release on GitHub from it:
   - title `Ultra Instinct Tears 1.0.0`;
   - the notes are what the launcher shows as "What's new": a short list first
     (the dialog shows about the first 16 lines; the rest is on the release page);
   - attach **both** files from `dist/`;
   - tick *Set as a pre-release* for `-rc` tags, *Set as the latest release* otherwise.
5. Check from a game folder with an older version: About > Install update should
   say `Update available: v1.0.0`.

Do not replace the zip of a published release: launchers that already saw the
old checksum will refuse the new file. Make a new tag (v1.0.1) instead.

## While the repository is private

The public API answers 404, and Install update says "no published release found
... the repository may still be private" and carries on with local zips. To test
against the private repository, give the launcher a fine-grained token with
read-only *Contents* access to this one repository, either as the environment
variable `UIT_GITHUB_TOKEN` or as one line in `updates\github-token.txt` in the
game folder. It is never written to the cfg (problem reports include the cfg) or
to any log. Delete it when you are done testing.

## Testing without GitHub

`UIT_UPDATE_API=http://127.0.0.1:PORT` points the updater at another API
(`tools/tests/mock_github.py` is a small stand-in used by the tests), and
**Advanced > Updates and reports > GitHub repository for updates** (`owner/name`)
checks a fork.

## HTTPS on the player's PC

The launcher runs the updater with the package's `deps\python\python.exe`
(`TOTK_LAUNCHER_PYTHON` overrides it, else `py -3`). That is a full python.org
copy from the builder package: it has `_ssl.pyd` with `libssl-3.dll` /
`libcrypto-3.dll` and trusts the Windows certificate store. A fresh Windows may
not have fetched GitHub's root yet (it downloads roots when first needed, which
Python does not trigger), and an overriding Python may have no `ssl` at all; in
both cases the updater switches to Windows' own `System32\curl.exe` (Windows 10
1803 and later), which checks the certificate with Windows itself and fetches a
missing root, and the download log says so. Verification is never turned off;
the token goes to curl on stdin (`--config -`), never on its command line, and
never across a redirect. Tests: `UIT_UPDATE_HTTP=curl|python` forces one way,
`UIT_UPDATE_CURL` names the curl to use (`tools/tests/test_update_curl.py`).
