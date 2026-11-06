"""Extern-free IO: decimal rounding, scalar operands, and foreign ABI assembly."""
from pathlib import Path
import ctypes
import math
import os
import platform
import random
import re
import struct
import subprocess
import sys
import tempfile

compiler, probe, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
fixtures = Path(__file__).with_name('fixtures')
io = (fixtures / 'io.oc').read_text()
assert not re.search(r'\bextern\b', io)


def run(args, env=None):
    return subprocess.run(args, text=True, capture_output=True, timeout=60, env=env)


with tempfile.TemporaryDirectory(prefix='ocean stdlib asm ') as directory:
    root = Path(directory)
    standard = root / 'standard'
    (standard / 'reflect').mkdir(parents=True)
    (standard / 'reflect' / 'reflect.oc').write_text((fixtures / 'reflect.oc').read_text())
    env = dict(os.environ, OCEAN_SRC=str(standard))
    source, ir, executable = (root / name for name in ('main.oc', 'main.ll', 'app'))
    targets = {
        'x86_64/linux': 'x86_64-linux-gnu',
        'aarch64/linux': 'aarch64-linux-gnu',
        'riscv64/linux': 'riscv64-linux-gnu',
        'x86_64/macos': 'x86_64-apple-macosx11.0',
        'aarch64/macos': 'arm64-apple-macosx11.0',
        'x86_64/windows': 'x86_64-windows-msvc',
        'aarch64/windows': 'aarch64-windows-msvc',
    }
    blocks = dict(re.findall(r'asm (\w+/\w+) \{\n(.*?)\n        \}', io, re.S))
    assert blocks.keys() == targets.keys()
    for selector, body in blocks.items():
        source.write_text('package probe; func sample() { asm ' + selector + ' {\n' + body + '\n} }')
        result = run([probe, str(source)])
        assert result.returncode == 0, result.stdout + result.stderr
        ir.write_text(result.stdout)
        for opt in ('-O0', '-O2'):
            result = run([clang, '--target=' + targets[selector], '-Wno-override-module', opt,
                          '-c', '-x', 'ir', str(ir), '-o', str(root / 'foreign.o')])
            assert result.returncode == 0, (selector, opt, result.stderr)

    # Compare exact binary64-to-decimal conversion with the native C formatter.
    libc = ctypes.CDLL(None)
    bits = [0, 1, 0x8000000000000000, 0x7ff0000000000000, 0xfff0000000000000,
            0x7ff8000000000001, 0x0010000000000000, 0x7fefffffffffffff]
    bits += [struct.unpack('=Q', struct.pack('=d', x))[0]
             for x in (2.5, 3.5, 1.999, -0.125, 0.1, 0.0001, 0.00001, 1e16, 1e17)]
    rng = random.Random(42)
    bits += [rng.getrandbits(64) for _ in range(30)]
    program = io.replace('package io;', 'package main;')
    program += '\nfunc floatOf(bits uint) float { raw: *void = &bits; return *(raw -> (*float)); }\nfunc main() {\n'
    expected = []
    for value in bits:
        x = struct.unpack('=d', struct.pack('=Q', value))[0]
        for places in (-1, 0, 2, 17, 100):
            buffer = ctypes.create_string_buffer(2048)
            if places == -1:
                libc.snprintf(buffer, 2048, b'%.17g', ctypes.c_double(x))
            else:
                libc.snprintf(buffer, 2048, b'%.*f', ctypes.c_int(places), ctypes.c_double(x))
            text = buffer.value.decode()
            # libc's spelling/case for nonfinite values differs across systems.
            if math.isnan(x): text = 'nan'
            if math.isinf(x): text = '-inf' if x < 0 else 'inf'
            if places == -1 and math.isfinite(x) and all(c not in text for c in '.eE'):
                text += '.0'
            expected.append(text)
            program += f'Println(sprintFloat(floatOf({value}), {places}));\n'
    source.write_text(program + '}\n')
    result = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)], env)
    assert result.returncode == 0, result.stdout + result.stderr
    assert '@snprintf' not in ir.read_text()
    for opt in ('-O0', '-O2'):
        result = run([clang, '-Wno-override-module', opt, '-x', 'ir', str(ir), '-o', str(executable), '-lffi'])
        assert result.returncode == 0, result.stderr
        result = run([str(executable)])
        assert result.returncode == 0, result.stderr
        actual = result.stdout.splitlines()
        assert len(actual) == len(expected), (len(actual), len(expected))
        for index, (got, want) in enumerate(zip(actual, expected)):
            assert got == want, (opt, index, got, want)

    if platform.system() == 'Linux' and platform.machine() in ('x86_64', 'AMD64'):
        # Run the Windows x64 sequence against ms_abi shims on the native host.
        # This catches shadow-space, alignment, pointer and returned-count errors.
        source.write_text('package probe; func sample() { asm x86_64/windows {\n' +
                          blocks['x86_64/windows'] + '\n} }')
        result = run([probe, str(source)])
        assert result.returncode == 0, result.stdout + result.stderr
        ir.write_text(result.stdout)
        shim = root / 'windows.c'
        shim.write_text(r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
extern int64_t sample(void *, int64_t);
__attribute__((ms_abi,noinline)) void *GetStdHandle(uint32_t id) {
    if (id != (uint32_t)-11) abort();
    return (void *)42;
}
__attribute__((ms_abi,noinline)) int WriteFile(void *handle, const void *p, uint32_t n,
                                             uint32_t *written, void *overlapped) {
    if (handle != (void *)42 || overlapped || n != 3 || memcmp(p,"asm",3)) abort();
    *written = 2; // Partial write must be returned to the Ocean loop.
    return 1;
}
int main(void) { return sample("asm",3) == 2 ? 0 : 1; }
''')
        for opt in ('-O0', '-O2'):
            result = run([clang, '-Wno-override-module', opt, str(ir), str(shim), '-o', str(executable)])
            assert result.returncode == 0, result.stderr
            result = run([str(executable)])
            assert result.returncode == 0, result.stderr

        # Exercise the complete Ocean write loop with deliberately short writes.
        windows_io = re.sub(r'asm (\w+/\w+)',
                            lambda m: 'asm x86_64/linux' if m[1] == 'x86_64/windows'
                            else 'asm riscv64/windows', io)
        shim.write_text(r'''
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
__attribute__((ms_abi,noinline)) void *GetStdHandle(uint32_t id) {
    if (id != (uint32_t)-11) abort();
    return (void *)42;
}
__attribute__((ms_abi,noinline)) int WriteFile(void *handle, const char *p, uint32_t n,
                                             uint32_t *written, void *overlapped) {
    if (handle != (void *)42 || overlapped || !n) abort();
    if (p[0] == '!') { *written=0; return 0; }
    *written = n > 2 ? 2 : n;
    return write(1,p,*written) == *written;
}
''')
        for text, success in (('abcdef', True), ('!', False)):
            source.write_text(windows_io.replace('package io;', 'package main;') +
                              f'\nfunc main() {{ Print("{text}"); }}')
            result = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)], env)
            assert result.returncode == 0, result.stdout + result.stderr
            for opt in ('-O0', '-O2'):
                result = run([clang, '-Wno-override-module', opt, str(ir), str(shim),
                              '-o', str(executable), '-lffi'])
                assert result.returncode == 0, result.stderr
                result = run([str(executable)])
                if success:
                    assert result.returncode == 0 and result.stdout == text, result
                else:
                    assert result.returncode != 0 and 'stdout write failed' in result.stderr, result

print('standard library assembly and floating formatting tests passed')
