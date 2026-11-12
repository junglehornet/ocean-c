#!/usr/bin/env python3
"""Exercise the actual term/tui frontend on a PTY, including live progress."""
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

compiler = Path(sys.argv[1]).resolve()
stdlib = Path(os.environ.get('OCEAN_SRC', compiler.parent.parent / 'ocean-src')).resolve()
SGR = re.compile(rb'\x1b\[[0-9;]*m')

with tempfile.TemporaryDirectory(prefix='ocean-live-test-') as directory:
    root = Path(directory)
    (root / 'suite.oc').write_text('package suite;')
    (root / 'suite_test.oc').write_text('''package suite;
import "testing";
import "time";
import "io";
func TestFirst(t *testing.T) {
    time.Sleep(200000000);
    t.Run("child", func(t *testing.T) { time.Sleep(200000000); });
}
func TestSecond(t *testing.T) {
    t.Log("running second test");
    io.Println("raw test output");
    time.Sleep(1200000000);
    t.Error("visible failure");
}
func TestSkip(t *testing.T) { t.Skip("not available"); }
''')
    env = {**os.environ, 'OCEAN_SRC': str(stdlib), 'TERM': 'xterm-256color',
           'NO_COLOR': '', 'OCEAN_BUILD_CACHE': str(root / 'cache')}

    def session(options=(), action=None, color=False, dumb=False):
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
        settings = dict(env)
        if color:
            settings.pop('NO_COLOR', None)
        if dumb:
            settings['TERM'] = 'dumb'
        process = subprocess.Popen([str(compiler), 'test', str(root), *map(str, options)],
                                   stdin=slave, stdout=slave, stderr=slave, env=settings)
        received = bytearray()
        deadline = time.monotonic() + 30

        def drain(wait=.1):
            if select.select([master], [], [], wait)[0]:
                received.extend(os.read(master, 65536))

        def expect(text):
            while text not in received:
                assert time.monotonic() < deadline, (text, bytes(received)[-5000:])
                assert process.poll() is None, (text, process.returncode, bytes(received)[-5000:])
                drain()

        try:
            if action:
                action(process, master, slave, expect, received)
            while process.poll() is None:
                assert time.monotonic() < deadline, bytes(received)[-5000:]
                drain()
            while select.select([master], [], [], 0)[0]:
                drain(0)
            assert termios.tcgetattr(slave) == original, 'terminal settings were not restored'
            return process.wait(), bytes(received)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)

    report = root / 'report.txt'
    events = root / 'events.jsonl'

    def progress(process, master, slave, expect, received):
        expect(b'\x1b[?25l')
        assert not termios.tcgetattr(slave)[3] & (termios.ECHO | termios.ICANON)
        expect(b'TestFirst/child')
        expect(b'TestSecond')
        expect('✓     TestFirst'.encode())
        # A complete live frame must keep First's child beneath its parent,
        # regardless of which concurrent root function started first.
        deadline = time.monotonic() + 3
        while True:
            frames = SGR.sub(b'', received).split(b'\x1b[J')
            complete = [frame for frame in frames if '✓     TestFirst'.encode() in frame
                        and b'TestFirst/child ' in frame and b'TestSecond ' in frame
                        and '↑/↓ scroll'.encode() in frame]
            if complete:
                frame = complete[-1]
                parent = frame.index(b'TestFirst ')
                child = frame.index(b'TestFirst/child ')
                assert parent < child, frame
                for other in (b'TestSecond ', b'TestSkip '):
                    assert other not in frame or not parent < frame.index(other) < child, frame
                break
            assert time.monotonic() < deadline, bytes(received)[-4000:]
            if select.select([master], [], [], .1)[0]:
                received.extend(os.read(master, 65536))
        # First has finished while Second is still running: this must be a live
        # completion, not output printed after the test executable has exited.
        assert process.poll() is None
        assert re.search('✓ +TestFirst'.encode(), SGR.sub(b'', received))
        data = [json.loads(line) for line in events.read_text().splitlines()]
        assert any(e['Action'] == 'pass' and e.get('Test') == 'TestFirst' for e in data)
        assert not any(e['Action'] == 'fail' and e.get('Test') == 'TestSecond' for e in data)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 12, 65, 0, 0))
        os.write(master, b'\x1b[A\x1b[Bf')
        expect(b'running second test')

    code, screen = session(['--jsonfile', events, '--outfile', report], progress)
    assert code == 1 and b'\x1b[?25h' in screen
    assert '∅'.encode() in screen and b'visible failure' in screen
    assert all(m.group(0) in (b'\x1b[0m', b'\x1b[m') for m in SGR.finditer(screen)), 'NO_COLOR must suppress styling without disabling terminal redraws'
    assert '\x1b' not in report.read_text() and 'Summary' in report.read_text()
    assert [json.loads(line) for line in events.read_text().splitlines()][-1]['Action'] == 'fail'

    def interrupt(process, master, slave, expect, received):
        expect(b'TestSecond')
        os.write(master, b'\x03')

    code, screen = session(['--jsonfile', events], interrupt)
    assert code == 130 and b'\x1b[?25h' in screen, (code, screen[-3000:])
    assert [json.loads(line) for line in events.read_text().splitlines()][-1]['Action'] == 'fail'

    def terminate(process, master, slave, expect, received):
        expect(b'TestSecond')
        process.send_signal(signal.SIGTERM)

    code, screen = session(action=terminate)
    assert code == 128 + signal.SIGTERM and b'\x1b[?25h' in screen

    code, screen = session(['--timeout', '250ms'])
    assert code != 0 and b'timed out' in screen and b'\x1b[?25h' in screen

    code, screen = session(['--notty'])
    assert code == 1 and b'\x1b[?25l' not in screen
    code, screen = session(['--json'])
    assert code == 1 and b'\x1b[?25l' not in screen
    json_events = [json.loads(line) for line in screen.decode().splitlines()]
    assert json_events[-1]['Action'] == 'fail'
    code, screen = session(dumb=True)
    assert code == 1 and b'\x1b[?25l' not in screen
    code, screen = session(color=True)
    assert code == 1 and b'\x1b[32m' in screen and b'\x1b[31m' in screen

    # A frontend crash restores the terminal but must not kill the test suite.
    if Path('/proc/self/task').exists():
        def crash(process, master, slave, expect, received):
            expect(b'TestSecond')
            children = Path(f'/proc/{process.pid}/task/{process.pid}/children').read_text().split()
            renderers = [int(pid) for pid in children if os.readlink(f'/proc/{pid}/exe').endswith('/display')]
            assert len(renderers) == 1, children
            os.kill(renderers[0], signal.SIGKILL)
        code, screen = session(action=crash)
        assert code == 1 and b'\x1b[?25h' in screen
        assert b'visible failure' in screen

    # A busy event stream must not block the compiler from enforcing timeout.
    (root / 'suite_test.oc').write_text('''package suite; import "testing";
func TestManyLogs(t *testing.T) {
    bytes := i8[]{};
    for (i := 0; i < 2000; i++) bytes.append('x');
    for (i := 0; i < 200; i++) t.Log(bytes->string);
    for (;;) {}
}
''')
    started = time.monotonic()
    code, screen = session(['--timeout', '300ms'])
    assert code != 0 and b'timed out' in screen and b'\x1b[?25h' in screen
    assert time.monotonic() - started < 10, 'renderer backpressure stalled timeout'

    # Run the Ocean model tests without a terminal as well.
    result = subprocess.run([str(compiler), 'test', str(stdlib / 'testing/internal/display'), '--notty'],
                            env=env, capture_output=True, timeout=45)
    assert result.returncode == 0, (result.stdout, result.stderr)

print('Ocean live test display checks passed')
