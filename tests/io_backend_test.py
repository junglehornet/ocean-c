"""Exercise IO formatting and its backend requirements at O0 and O2."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = map(lambda x: str(Path(x).resolve()), sys.argv[1:])
io = Path(__file__).with_name('fixtures').joinpath('io.oc').read_text()

def run(*args):
    return subprocess.run(args, text=True, capture_output=True, timeout=20)

with tempfile.TemporaryDirectory(prefix='ocean io ') as directory:
    root = Path(directory)
    source, ir, exe = root / 'main.oc', root / 'main.ll', root / 'main'
    def program(body, output='', status=0, library=False):
        source.write_text((io.replace('package io;', 'package main;') if library else 'package main;') + body)
        result = run(compiler, 'build', str(source), '--emit-llvm', '-o', str(ir))
        assert result.returncode == 0, result.stderr
        for opt in ('-O0', '-O2'):
            result = run(clang, opt, '-Wno-override-module', '-x', 'ir', str(ir), '-o', str(exe), '-lffi')
            assert result.returncode == 0, result.stderr
            result = run(str(exe))
            assert (result.returncode, result.stdout) == (status, output), result

    program(r'''
func main() int {
    Print("a"); Println("b"); Println();
    Println(Sprintf("x{}y{}z", 12, "Q"));
    Println(Sprintf("a{b\\q\\{c\\\\d"));
    Println(Sprint(0, 12345, -987, (-9223372036854775807 - 1)));
    Println(Sprint(7->i32, (-8)->i16, 9->i8, 1.25->f32));
    Println(sprintFloat(1.999, 2), " ", sprintFloat(-0.125, 3));
    Println(sprintFloat(2.0, -1), " ", sprintFloat(1.25, -1), " ", sprintFloat(2.5, 0));
    nested := int[]{1, 3}; Println(Sprint(nested));
    Println(Sprint(mixed any[]{1, "two", true}));
    Println(Sprint(string[]{"a", "b"}));
    Println(Sprint(int[][]{{1, 2}, {3}}));
    c := cStrOf("hello"); if (len(c) != 6 || c[5] != 0) return 1;
    return 0;
}
''', 'ab\n\nx12yQz\na{b\\q{c\\d\n0 12345 -987 -9223372036854775808\n7 -8 9 1.25\n2.00 -0.125\n2.0 1.25 2\n[1 3]\n[1 two [unknown type]]\n[a b]\n[[1 2] [3]]\n', library=True)

    program('''
func classify(x any) int { switch (y := x.(type)) {
case i16: return y->int; case f32: return y->int; default: return 99; } }
extern i32 printf(buffer<i8>[] format, ...);
func relay(args ...any) { extern.printf("%d %.1f\\n\\0".data, args...); }
func main() int {
    a := (-300)->i16; b := a->i32; c := b->i16;
    f := 1.5->f32; f++; f = -f;
    if (sizeof(i16) != 2 || sizeof(f32) != 4 || c->int != -300 || f->float != -2.5) return 1;
    if (classify(7->i16) + classify(3.5->f32) != 10) return 2;
    relay((-3)->i16, 1.5->f32);
    extern.printf("%d %.1f\\n\\0".data, (-3)->i16, 1.5->f32);
    return 0;
}''', '-3 1.5\n-3 1.5\n')

    program('''
func main() int {
    n := 0; calls := 0;
    switch (n++) { default: calls = 99; case 0, 1: calls = 1; fallthrough; case 2: calls += 2; }
    if (n != 1 || calls != 3) return 1;
    switch { case false: return 2; case true: calls++; default: return 3; }
    switch ("yes") { case "no": return 4; case "yes": break; default: return 5; }
    for (i := 0; i < 3; i++) { switch (i) { case 1: continue; default: calls++; } }
    switch (x := 5; x) { case 5: calls++; }
    return calls;
}''', status=7)

    # Explicit any[] values remain homogeneous; ...any packs remain mixed.
    program('''func total(xs any[]) int {
        result := 0; for (i, x := range xs) result += x.(int); return result;
    }
    func mixedArgs(xs ...any) int { xs[0] = true; return len(xs); }
    func main() int { a: any[] = int[]{1, 2}; a[0] = 4; a += 3;
        return total(a) + total(int[]{5, 6}) + mixedArgs(1, "x"); }''', status=22)

    def rejected(body):
        source.write_text('package main; ' + body)
        result = run(compiler, 'build', str(source), '--check')
        assert result.returncode != 0 and 'same type' in result.stderr, result

    rejected('func main() { a := any[]{1, true}; }')
    rejected('func main() { a := any[]{1->i16, 2->i32}; }')
    for body in ('a := any[]{1, 2}; a[0] = true;',
                 'a := any[]{1}; a += true;',
                 'x: any = true; a := any[]{1, x};'):
        source.write_text('package main; func main() { ' + body + ' }')
        result = run(compiler, 'build', str(source), '-o', str(exe))
        assert result.returncode == 0, result.stderr
        result = run(str(exe))
        assert result.returncode != 0, body

    program('''func inspect(x any) int { switch (xs := x.(type)) {
        case any[]: return len(xs); default: return 10; } }
    func main() int { return inspect(int[]{1, 2}) + inspect(bool[]{true}) + inspect(3); }''', status=13)
    program('''func main() int { x: f32 = 1.5; y: i16 = -3; ++x; ++y;
        if (x != 2.5 || y != -2 || zero(f32) != 0.0) return 1;
        a := f32[]{x, -x}; return (a[0] - a[1])->int; }''', status=5)

    program('''func make() any[] { return int[]{2, 3}; }
    func main() int { a := make(); a = int[]{4, 5}; return a[1].(int); }''', status=5)

    source.write_text('package main;\nfunc need(x int) int { return x; }\n'
                      'func main() int { x: any = 3; return need(x); }\n')
    result = run(compiler, 'build', str(source), '--emit-llvm', '-o', str(ir))
    assert result.returncode != 0 and 'value type mismatch' in result.stderr, result
    diagnostics = [line for line in result.stderr.splitlines() if 'backend error:' in line]
    assert len(diagnostics) == len(set(diagnostics)), diagnostics
    assert any(':3:' in line and 'value type mismatch' in line for line in diagnostics), diagnostics
