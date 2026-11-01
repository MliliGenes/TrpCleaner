#!/usr/bin/env python3
"""Optional PTY regression tests. Uses C++ temporary fixtures, never real HOME."""
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import termios
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent


def interact(actions, size=(24, 96), expected_code=0):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', *size, 0, 0))
    proc = subprocess.Popen([str(ROOT / 'tests/safety'), '--tui'], stdin=slave,
                            stdout=slave, stderr=slave, env=dict(os.environ, TERM='xterm'))
    os.close(slave)
    output = b''
    offset = 0

    def receive():
        nonlocal output
        if select.select([master], [], [], 0.05)[0]:
            try:
                chunk = os.read(master, 65536)
            except OSError:
                return False
            output += chunk
            return bool(chunk)
        return True

    try:
        for needle, keys in actions:
            deadline = time.monotonic() + 5
            while needle not in output[offset:]:
                if time.monotonic() >= deadline or not receive():
                    raise AssertionError(f'Missing {needle!r}: {output!r}')
            offset = len(output)
            if isinstance(keys, int):
                proc.send_signal(keys)
            else:
                os.write(master, keys)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if not receive():
                break
        code = proc.wait(timeout=2)
        if code != expected_code:
            raise AssertionError((code, output))
        if b'\x1b[?1049l' not in output:
            raise AssertionError('Terminal was not restored')
        return output
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)


class MenuTests(unittest.TestCase):
    def test_quit_without_cleaning(self):
        out = interact([(b'Enter previews', b'q')])
        self.assertIn(b'Fixture cache remaining: 1', out)

    def test_enter_on_bulk_row_then_cancel(self):
        out = interact([(b'Enter previews', b'\r'),
                        (b'y delete these cache files', b'n'),
                        (b'Cleanup cancelled', b'q')])
        self.assertIn(b'Fixture cache remaining: 1', out)

    def test_clean_all_one_action_and_confirmation(self):
        out = interact([(b'Enter previews', b'a'),
                        (b'y delete these cache files', b'y'),
                        (b'Cleaned ', b'q')])
        self.assertIn(b'Fixture cache remaining: 0', out)
        self.assertIn(b'Protected fixture preserved: 1', out)

    def test_clean_highlighted_category(self):
        out = interact([(b'Enter previews', b'3jjc'),
                        (b'y delete these cache files', b'y'),
                        (b'Cleaned ', b'q')])
        self.assertIn(b'Fixture cache remaining: 0', out)

    def test_details_do_not_clean(self):
        out = interact([(b'Enter previews', b'3jj\r'),
                        (b'Details only.', b'\r'),
                        (b'Returned from details', b'q')])
        self.assertIn(b'Fixture cache remaining: 1', out)

    def test_all_action_includes_other_tabs(self):
        out = interact([(b'Enter previews', b'4a'),
                        (b'y delete these cache files', b'y'),
                        (b'Cleaned ', b'a'),
                        (b'No cache files to clean', b'q')])
        self.assertIn(b'Fixture cache remaining: 0', out)

    def test_arrow_navigation_and_rescan(self):
        out = interact([(b'Enter previews', b'\x1b[C\x1b[Br'),
                        (b'Scan complete.', b'q')])
        self.assertIn(b'Fixture cache remaining: 1', out)

    def test_small_terminal_blocks_actions(self):
        out = interact([(b'Resize terminal', b'ayq')], size=(10, 40))
        self.assertIn(b'Fixture cache remaining: 1', out)

    def test_ctrl_c_restores_terminal(self):
        interact([(b'Enter previews', signal.SIGINT)], expected_code=130)


if __name__ == '__main__':
    unittest.main(verbosity=2)
