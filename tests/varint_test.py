"""Exact signed integer widths, conversions, containers and named type identity."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='ocean-varint-') as directory:
    root = Path(directory)
    source, ir, executable = root / 'main.oc', root / 'main.ll', root / 'program'

    def check(body, message=None):
        source.write_text('package main;\n' + body)
        result = subprocess.run([compiler, 'build', '--check', str(source)], capture_output=True, text=True)
        assert (result.returncode == 0) == (message is None), result.stderr
        if message:
            assert message in result.stderr, result.stderr

    def program(body, status=0):
        check(body)
        result = subprocess.run([compiler, 'build', str(source), '--emit-llvm', '-o', str(ir)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        for opt in ('-O0', '-O2'):
            result = subprocess.run([clang, opt, '-Wno-override-module', str(ir), '-o', str(executable), '-lffi'], capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            result = subprocess.run([str(executable)])
            assert result.returncode == status, (body, result.returncode)

    program('''type nibble varint(4); type Alias = nibble;
func identity(x varint(6)) varint(6) { return x; }
func main() int {
    n := 7->nibble; n++; n += 1->Alias;
    x: varint(6) = -12; a: varint(4)[2] = {2, -3};
    p := &x; *p = -11;
    b: any = x; y := b.(varint(6));
    if (sizeof(varint(4)) != 1 || sizeof(nibble) != 1) return 1;
    if (n->int != -7 || identity(y)->int != -11 || a[1]->int != -3 || x != -11 || a[0] != 2) return 2;
    z := zero(varint(9));
    literal := varint(4)[2]{1, 2}; dynamic := varint(4)[]{3, 4};
    if (literal[0] != 1 || dynamic[1] != 4) return 3;
    return z->int;
}''')
    body = []
    for width in range(1, 65):
        minimum = -(1 << (width - 1))
        maximum = (1 << (width - 1)) - 1
        body.append(f'a{width}: varint({width}) = {minimum}; b{width}: varint({width}) = {maximum};')
        body.append(f'if (a{width}->int != {minimum} || b{width}->int != {maximum}) return 1;')
        body.append(f'b{width}++; if (b{width}->int != {minimum}) return 2;')
    program('func main() int {\n' + '\n'.join(body) + '\nreturn 0; }')
    for width in (0, 65, -1):
        check(f'func main() {{ x: varint({width}); }}', 'varint width')
    check('func main() { x: varint(4) = 8; }', 'type mismatch')
    check('func main() { x: varint(6) = 3; y: varint(4) = x; }', 'type mismatch')
    check('type nibble varint(4); func main() { x: nibble = 3; }', 'type mismatch')
    check('func main() { x: varint; }', 'varint requires')
