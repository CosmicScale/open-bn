#!/usr/bin/env python3
"""Run the tests of build/test/ on a PS2 (the second half of `make test`).

Usage: .venv/bin/python tools/test_ps2.py HOST [--keep]
       (on the host, from the repository root, after tools/test.py; `make venv` installs telnetlib3)

1. build/test/ is served over HTTP on the address this machine uses to reach the console;
2. a telnet session (user and password from PS2_USER and PS2_PASSWORD, default those of the
   Definitive Patch) downloads the programs with wget into /bntmp/test and runs them one by one,
   in the order of tools/test.py, printing their output as it comes;
3. /bntmp/test is removed, whatever happened (--keep leaves it, with the databases of a program
   that failed).

A program passes when it exits 0 and prints Unity's summary with OK. Exit 0 if all pass.
The commands sent are kept short: the shell of the console drops long typed lines.
Run it with bn idle on a menu: the tests share pfsd, the CPU and the memory with it. So that a test
that goes wrong cannot take the console down, each program limits its own memory and time
(TEST_MEMORY_KB and TEST_SECONDS in tests/support/testutil.h); if it still has not ended a minute
past its time the runner interrupts it.
"""
import asyncio
import functools
import http.server
import importlib.util
import os
import re
import socket
import sys
import threading

import telnetlib3

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# tools/test.py by its path: `import test` could find the test package of the standard library
_spec = importlib.util.spec_from_file_location('open_bn_tests', os.path.join(ROOT, 'tools', 'test.py'))
_tests = importlib.util.module_from_spec(_spec)
sys.path.insert(0, os.path.join(ROOT, 'tools'))  # tools/test.py imports build and fetch
_spec.loader.exec_module(_tests)
OUT, TESTS = _tests.OUT, _tests.TESTS

DIR = '/bntmp/test'
LOGIN_TIMEOUT = 20
COMMAND_TIMEOUT = 120


def test_seconds():
    with open(os.path.join(ROOT, 'tests/support/testutil.h')) as f:
        return int(re.search(r'^#define TEST_SECONDS (\d+)', f.read(), re.M).group(1))


TEST_TIMEOUT = test_seconds() + 60  # the programs kill themselves first
SUMMARY = re.compile(r'^(\d+) Tests (\d+) Failures (\d+) Ignored\s*\n(OK|FAIL)', re.M)
DONE = re.compile(rb'__rc\d+=(\d+)__')
TIMEOUTS = (TimeoutError, asyncio.TimeoutError)  # distinct classes before Python 3.11


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve(host):
    """Serves OUT on the address that routes to `host`; returns the server's URL."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((host, 23))
        ip = s.getsockname()[0]
    server = http.server.ThreadingHTTPServer((ip, 0), functools.partial(Quiet, directory=OUT))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return f'http://{ip}:{server.server_address[1]}'


class Session:
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        self.count = 0

    async def expect(self, pattern, timeout):
        return await asyncio.wait_for(self.reader.readuntil_pattern(re.compile(pattern)), timeout)

    async def run(self, cmd, timeout=COMMAND_TIMEOUT, echo=None):
        """Runs `cmd` in the shell; returns (exit code, output). `echo` gets the output as it comes."""
        # a marker of its own, so that what an interrupted command prints later is not taken for this one's
        self.count += 1
        done = re.compile(rb'__rc%d=(\d+)__' % self.count)
        self.writer.write(f'{cmd}; echo "__rc{self.count}=$?__"\r\n'.encode())
        out = b''
        loop = asyncio.get_running_loop()
        end = loop.time() + timeout
        while not done.search(out):
            left = end - loop.time()
            if left <= 0:
                raise TimeoutError(f'{cmd}: no answer in {timeout} s')
            try:
                chunk = await asyncio.wait_for(self.reader.read(4096), left)
            except asyncio.TimeoutError:
                raise TimeoutError(f'{cmd}: no answer in {timeout} s')
            if not chunk:
                raise ConnectionError('telnet session closed')
            out += chunk
            if echo:
                echo(chunk)
        m = done.search(out)
        return int(m.group(1)), out[:m.start()].decode('utf-8', 'replace').replace('\r\n', '\n')


async def login(host):
    reader, writer = await telnetlib3.open_connection(host, 23, encoding=False, term='dumb',
                                                      connect_timeout=LOGIN_TIMEOUT)
    s = Session(reader, writer)
    await s.expect(rb'login: ', LOGIN_TIMEOUT)
    writer.write(os.environ.get('PS2_USER', 'root').encode() + b'\r\n')
    await s.expect(rb'Password: ', LOGIN_TIMEOUT)
    writer.write(os.environ.get('PS2_PASSWORD', 'password').encode() + b'\r\n')
    # no echo of what is typed and no prompt: the output is only what the commands print
    writer.write(b"stty -echo; PS1=''; PS2=''; export TERM=dumb\r\n")
    rc, _ = await s.run('true', LOGIN_TIMEOUT)
    return s


class Echo:
    """Prints a program's output line by line, without the end marker."""

    def __init__(self):
        self.pending = b''

    def __call__(self, chunk):
        self.pending += chunk
        *lines, self.pending = self.pending.split(b'\n')
        for line in lines:
            if not DONE.search(line):
                print('  ' + line.decode('utf-8', 'replace').rstrip('\r'), flush=True)


async def run_test(s, n):
    """Runs program `n`; returns (exit code, output). On a timeout it is interrupted first."""
    try:
        return await s.run(f'./{n} 2>&1', TEST_TIMEOUT, echo=Echo())
    except TimeoutError:
        s.writer.write(b'\x03')  # Ctrl-C: the program must not outlive the session
        await s.run('true', COMMAND_TIMEOUT)
        raise


async def run_tests(s, url, names):
    """Copies and runs the programs; returns the names of those that failed."""
    rc, out = await s.run(f'rm -rf {DIR} && mkdir -p {DIR} && cd {DIR}')
    for n in names:
        if not rc:
            rc, out = await s.run(f'wget -q -O {n} {url}/{n} && chmod 755 {n}')
    if rc:
        sys.exit(f'error: copying the tests to the console failed ({rc}): {out.strip()}')
    failed = []
    ignored = 0
    for n in names:
        print(f'{n}:', flush=True)
        rc, out = await run_test(s, n)
        m = SUMMARY.search(out)
        if m:
            ignored += int(m.group(3))
        if rc or not m or m.group(4) != 'OK':
            failed.append(n)
            why = f'exit {rc}' if m else f'exit {rc}, no summary (crashed?)'
            print(f'  -> FAIL ({why})', flush=True)
    return failed, ignored


async def main_async(host, keep):
    url = serve(host)
    s = await login(host)
    names = list(TESTS)
    try:
        failed, ignored = await run_tests(s, url, names)
    finally:
        if not keep:
            try:
                await s.run(f'cd / && rm -rf {DIR}')
            except (OSError,) + TIMEOUTS:
                print(f'warning: {DIR} could not be removed from the console', file=sys.stderr)
        s.writer.write(b'exit\r\n')
        s.writer.close()
    print(f'test       {len(names) - len(failed)}/{len(names)} programs passed' +
          (f'; failed: {", ".join(failed)}' if failed else '') +
          (f' ({ignored} tests ignored)' if ignored else ''))
    if keep:
        print(f'test       left in {DIR} on the console')
    return 1 if failed else 0


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        sys.exit(__doc__.split('\n\n')[1])
    os.chdir(ROOT)
    missing = [n for n in TESTS if not os.path.exists(f'{OUT}/{n}')]
    if missing:
        sys.exit(f'error: {", ".join(missing)} missing in {OUT}/: run python3 tools/test.py (make test)')
    try:
        return asyncio.run(main_async(args[0], '--keep' in sys.argv))
    except (OSError, asyncio.IncompleteReadError) + TIMEOUTS as e:
        sys.exit(f'error: {args[0]}: {e}')


if __name__ == '__main__':
    sys.exit(main())
