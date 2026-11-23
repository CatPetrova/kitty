# License: GPLv3

import os
import select
import shutil
import subprocess
import sys
import time
import unittest
from contextlib import ExitStack, contextmanager
from pathlib import Path
from tempfile import TemporaryDirectory

from kitty.constants import kitten_exe, kitty_exe

from .base import BaseTest


@contextmanager
def running_process(argv, **kwargs):
    with subprocess.Popen(argv, **kwargs) as process:
        try:
            yield process
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def dbus_python():
    for python in dict.fromkeys((shutil.which('python3'), '/usr/bin/python3')):
        if python and os.access(python, os.X_OK):
            probe = subprocess.run(
                [python, '-W', 'ignore::ImportWarning', '-c', 'import dbus.service, dbus.mainloop.glib; from gi.repository import GLib'],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=10,
            )
            if probe.returncode == 0:
                return python
    return None


@unittest.skipUnless(sys.platform.startswith('linux'), 'Requires Linux X11')
class TestFcitx(BaseTest):
    def test_fcitx_commit_and_split_focus(self):
        xvfb, xdotool, dbus = map(shutil.which, ('Xvfb', 'xdotool', 'dbus-daemon'))
        python = dbus_python() if xvfb and xdotool and dbus else None
        if not (xvfb and xdotool and dbus and python):
            self.skipTest('Requires Xvfb, xdotool, dbus-daemon and Python with dbus and GLib bindings')

        with ExitStack() as stack:
            tmp = Path(stack.enter_context(TemporaryDirectory(prefix='kitty-fcitx-')))
            log_path = tmp / 'processes.log'
            log = stack.enter_context(log_path.open('wb'))

            def start(argv, env=None):
                return stack.enter_context(running_process(argv, env=env, stdout=subprocess.PIPE, stderr=log, text=True))

            def ready(process, description):
                output = process.stdout
                if output is not None and select.select([output], [], [], 10)[0]:
                    if line := output.readline().strip():
                        return line
                self.fail(f'{description} did not start.\n{log_path.read_text(errors="replace")}')

            display = ready(start([xvfb, '-displayfd', '1', '-screen', '0', '1024x768x24', '-nolisten', 'tcp']), 'Xvfb')
            address = ready(start([dbus, '--session', '--nofork', '--print-address=1']), 'D-Bus session')
            env = dict(os.environ, DISPLAY=f':{display}', DBUS_SESSION_BUS_ADDRESS=address, XMODIFIERS='@im=fcitx', LIBGL_ALWAYS_SOFTWARE='1')
            env.pop('GLFW_IM_MODULE', None)
            journal = tmp / 'focus'
            mock = Path(__file__).with_name('fcitx_mock.py')
            self.ae(ready(start([python, '-W', 'ignore::ImportWarning', str(mock), str(journal)], env), 'Fcitx portal'), 'ready')

            def pty_capture_file(path):
                program = (
                    'import os, sys, tty\n'
                    'tty.setraw(0)\n'
                    'with open(sys.argv[1], "wb", buffering=0) as capture:\n'
                    '    while data := os.read(0, 4096):\n'
                    '        capture.write(data)\n'
                )
                return [python, '-c', program, str(path)]

            first, second = tmp / 'first', tmp / 'second'
            title = 'kitty-fcitx-test'
            control = f'unix:{tmp / "kitty.sock"}'
            kitty = stack.enter_context(
                running_process(
                    [
                        kitty_exe(),
                        '--config',
                        'NONE',
                        '-o',
                        'linux_display_server=x11',
                        '-o',
                        'enabled_layouts=splits',
                        '-o',
                        'allow_remote_control=socket-only',
                        '--listen-on',
                        control,
                        '--title',
                        title,
                        '--debug-input',
                        *pty_capture_file(first),
                    ],
                    env=env,
                    stdout=log,
                    stderr=log,
                )
            )

            def wait_for(condition, description):
                deadline = time.monotonic() + 10
                while kitty.poll() is None and time.monotonic() < deadline:
                    if result := condition():
                        return result
                    time.sleep(0.02)
                self.fail(f'Failed waiting for {description}.\n{log_path.read_text(errors="replace")}')

            def find_window():
                result = subprocess.run([xdotool, 'search', '--onlyvisible', '--name', f'^{title}$'], env=env, capture_output=True, text=True, timeout=5)
                return result.stdout.strip()

            def keys(*args):
                subprocess.run([xdotool, 'key', '--delay', '0', *args], env=env, check=True, timeout=5)

            def focused_context():
                if journal.exists():
                    for line in reversed(journal.read_text().splitlines()):
                        if line.startswith('focus '):
                            return line.split()[1]
                return ''

            def preedit_count():
                return log_path.read_text(errors='replace').count("updated pre-edit text: '你好'")

            window_id = wait_for(find_window, 'X11 window')
            wait_for(first.exists, 'first terminal child')
            subprocess.run([xdotool, 'windowfocus', '--sync', window_id], env=env, check=True, timeout=5)
            first_context = wait_for(focused_context, 'first input context')

            keys('n')
            wait_for(preedit_count, 'formatted preedit')
            keys('space', 'Return')
            expected = '你好\r'.encode()
            wait_for(lambda: first.stat().st_size >= len(expected), 'Chinese commit before Return')
            self.ae(first.read_bytes(), expected)

            count = preedit_count()
            keys('n')
            wait_for(lambda: preedit_count() > count, 'unfinished preedit before switching panes')
            subprocess.run(
                [kitten_exe(), '@', '--to', control, 'launch', '--type=window', '--location=vsplit', *pty_capture_file(second)],
                env=env,
                check=True,
                capture_output=True,
                timeout=10,
            )
            wait_for(second.exists, 'second terminal child')
            wait_for(lambda: (path := focused_context()) and path != first_context, 'new input context after switching panes')
            self.assertIn(f'reset {first_context}', journal.read_text().splitlines())

            keys('n', 'space', 'Return')
            wait_for(lambda: second.stat().st_size >= len(expected), 'Chinese commit in the new pane')
            self.ae(second.read_bytes(), expected)
            self.ae(first.read_bytes(), expected)
