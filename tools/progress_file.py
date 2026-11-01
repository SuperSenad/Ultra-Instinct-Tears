"""[UIT 0046] The helpers' progress file (one JSON object the launcher polls), written so that it can never fail a task.

The file is replaced whole (write NAME.tmp, rename it over NAME) so the launcher never reads half of it. On Windows
that rename fails with "[WinError 5] Access is denied" while another process has NAME open without FILE_SHARE_DELETE
(an older launcher's poll, a virus scanner, an editor). That broke an update mid-build: progress is only a display, so
the rename is retried briefly and otherwise this one update is skipped (noted once in the log). The last state
(done / failed / rolled_back) is retried longer: the launcher reads it after the helper exits to choose its message.
"""
import json, os, pathlib, time

FINAL = ('done', 'failed', 'rolled_back')


class ProgressFile:
    def __init__(self, path, note=None):
        """note(text) is called once, the first time an update has to be skipped (the helper's log)."""
        self.path = pathlib.Path(path) if path else None
        self.note, self.skipped, self.noted = note, 0, False

    def write(self, state):
        if not self.path:
            return True
        final = state.get('state') in FINAL
        delays = (0.005, 0.01, 0.02, 0.04, 0.08) + ((0.1,) * 20 if final else ())
        tmp = self.path.with_name(self.path.name + '.tmp')
        err = None
        for d in (0.0,) + delays:
            if d:
                time.sleep(d)
            try:
                tmp.write_text(json.dumps(state))
                os.replace(tmp, self.path)
                return True
            except OSError as e:   # PermissionError on Windows (a reader without share-delete), or any other I/O error
                err = e
        self.skipped += 1
        if final:   # last resort for the state the launcher acts on: write in place (works unless it is locked outright)
            try:
                with open(self.path, 'w') as f:
                    f.write(json.dumps(state))
                return True
            except OSError:
                pass
        if not self.noted and self.note:
            self.noted = True
            try:
                self.note(f'note: the progress display could not be updated ({err}); the task goes on, this is only '
                          f'the progress screen')
            except Exception:   # noqa: BLE001 - a note is never worth failing for
                pass
        return False
