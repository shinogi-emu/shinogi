#!/usr/bin/env python3
"""Native API-boundary regression plus a strict Windows SDK cross build."""
import json
import os
from pathlib import Path
import selectors
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(tempfile.mkdtemp(prefix='shinogi-install-tap-'))
ENV = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
# LeakSanitizer cannot run under the sandbox's ptrace. ASan/UBSan and explicit
# SetupAPI list/key/device ownership accounting remain enabled.
subprocess.run([
    'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
    '-fsanitize=address,undefined', '-Itests/win-settings',
    'tests/win-settings/test_install_tap.c', '-o', str(BUILD / 'test-install-tap')
], cwd=ROOT, check=True)
result = subprocess.run([str(BUILD / 'test-install-tap')], cwd=ROOT, env=ENV,
                        check=True, text=True, capture_output=True)
(BUILD / 'native.log').write_text(result.stdout + result.stderr)
records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
assert records and any(item['recoveryRequired'] for item in records)
for item in records:
    if item['progress']:
        assert item['deviceInstanceId'].startswith('ROOT\\AtariTAP\\')
    elif not item['success'] and not item['recoveryRequired']:
        assert item['deviceInstanceId'] is None
    if item['success'] and item['guid'] is None:
        assert item['rebootRequired']

# The fake first mutating API blocks reading stdin. It cannot continue until
# this process has actually received the flushed initial identity over a pipe.
process = subprocess.Popen([str(BUILD / 'test-install-tap'), '--progress-probe'],
                           env=ENV, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
selector = selectors.DefaultSelector()
try:
    selector.register(process.stdout, selectors.EVENT_READ)
    assert selector.select(timeout=5), 'Initial recovery identity was not flushed'
    first = json.loads(process.stdout.readline())
    assert first['progress'] and first['deviceInstanceId'].startswith('ROOT\\AtariTAP\\')
    tail, errors = process.communicate('x', timeout=5)
    assert process.returncode == 0, errors
    final = json.loads(tail.strip().splitlines()[-1])
    assert final['success'] and not final['progress']
finally:
    selector.close()
    if process.poll() is None:
        process.kill()
        process.communicate()

subprocess.run([str(BUILD / 'test-install-tap'), '--broken-progress'], env=ENV,
               check=True, capture_output=True)

for compiler in ('x86_64-w64-mingw32-gcc', 'i686-w64-mingw32-gcc'):
    subprocess.run([
        compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-municode',
        'tools/win/install-tap.c', '-o', str(BUILD / (compiler + '.exe')),
        '-lsetupapi', '-lnewdev', '-ladvapi32', '-lole32'
    ], cwd=ROOT, check=True)
print('PASS targeted installation, every stage failure, own-node rollback, reboot/GUID, JSON, flushed progress; x64/x86 strict SDK builds')
print('Outputs:', BUILD)
