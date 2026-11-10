"""Function literals, structural callable types, shared captures, and indirect calls."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
PREFIX = '''package main;
extern i32 putchar(i32 c);
func emit(c int) { extern.putchar(c->i32); }
'''


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=20)


with tempfile.TemporaryDirectory(prefix="ocean closure ") as directory:
    root = Path(directory)
    source, ir, executable = root / "main.oc", root / "main.ll", root / "native"
    checks = 0

    def case(body, status=0, output="", panic=None):
        global checks
        source.write_text(PREFIX + body)
        emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
        assert emitted.returncode == 0, emitted.stderr
        for optimization in ("native packages", "-O0", "-O2"):
            if optimization == "native packages":
                built = run([compiler, "build", str(source), "-o", str(executable)])
            else:
                built = run([clang, "-Wno-override-module", optimization, "-x", "ir", str(ir), "-o", str(executable), "-lffi"])
            assert built.returncode == 0, built.stderr
            result = run([str(executable)])
            assert (result.returncode, result.stdout) == (status, output), result
            assert (result.stderr.startswith("Panic: " + panic + "\n") if panic is not None else result.stderr == ""), result
            checks += 1

    case('func main() { f := func() { emit(65); }; f(); }', output="A")
    case('func main() int { return func(x int) int { return x+1; }(6); }', 7)
    case('func apply(f func(int) int, n int) int { return f(n); } func main() int { return apply(func(x int) int { return x*2; }, 4); }', 8)
    case('func twice(x int) int { return 2*x; } func apply(f func(int) int) int { return f(4); } func main() int { return apply(twice); }', 8)
    case('func main() int { x := 2; f := func() int { x++; return x; }; x=5; return f()+x; }', 12)
    case('func makeCounter(start int) func() int { return func() int { start++; return start; }; } func main() int { f := makeCounter(3); return f()+f(); }', 9)
    case('func makeCounter(start int) func() int { return func() int { start++; return start; }; } func main() int { a:=makeCounter(2); b:=makeCounter(5); return a()+b()+a(); }', 13)
    case('func makePair() (func() int, func(int)) { x:=1; return func() int { return x; }, func(n int) { x=n; }; } func main() int { get,set:=makePair(); set(7); return get(); }', 7)
    case('func outer(n int) func() func() int { return func() func() int { return func() int { n++; return n; }; }; } func main() int { make:=outer(4); f:=make(); return f(); }', 5)
    case('func main() int { x:=1; f:=func() int { x:=3; g:=func() int { x++; return x; }; return g(); }; return f()+x; }', 5)
    case('func main() int { f: func(int) int; f=func(n int) int { if (n<2) return 1; return n*f(n-1); }; return f(5); }', 120)
    case('func main() int { x:=1; defer func() { x=7; }(); f:=func() int { return x; }; x=3; return f(); }', 3)
    case('func f() { defer func() { p:=recover(); if (p!=null && p.(string)=="hi") emit(65); }(); panic "hi"; } func main() { f(); emit(66); }', output="AB")
    case('func leaf() { return panic "hi"; } func f() { defer func() { recover(); emit(65); }(); leaf(); } func main() { f(); }', output="A")
    case('func f() { defer func() { emit(65); }(); return panic "hi"; } func main() { defer func() { recover(); emit(66); }(); f(); }', output="B")
    case('func main() { f:=func() { emit(65); }; defer f(); f=func() { emit(66); }; f(); }', output="BA")
    case('func makeText() func() string { s:="hello"; return func() string { s=s+"!"; return s; }; } func main() int { f:=makeText(); if (f()=="hello!" && f()=="hello!!") return 7; return 1; }', 7)
    case('func makeArray() func() int { a: int[] = {1}; return func() int { a.append(2); return len(a); }; } func main() int { f:=makeArray(); return f()+f(); }', 5)
    case('func map(data array<int>, closure func(int) int) int { total:=0; for (i:=0;i<len(data);i++) total+=closure(data[i]); return total; } func main() int { offset:=2; return map(int[2]{1,2},func(n int) int { return n+offset; }); }', 7)
    case('struct S { f func(int) int; } func main() int { s:=S{func(n int) int { return n+1; }}; return s.f(6); }', 7)
    case('type Operation = func(int) int; func apply(f Operation) int { return f(6); } func main() int { return apply(func(n int) int { return n+1; }); }', 7)
    case('func main() int { f:=func(a int) (int, bool) { return a,true; }; n,ok:=f(7); if (ok) return n; return 1; }', 7)
    case('func apply(f func(...int) int) int { a: int[] = {2,3}; return f(1,a...); } func main() int { return apply(func(xs ...int) int { n:=0; for (i:=0;i<len(xs);i++) n+=xs[i]; return n; }); }', 6)
    case('type F = func() int; func main() int { xs: F[2] = {func() int {return 2;},func() int {return 3;}}; return xs[0]()+xs[1](); }', 5)
    case('func main() int { x:=2; f: func() int; { y:=3; f=func() int { return x+y; }; } x=4; return f(); }', 7)
    case('func main() int { f:=true ? func() int { return 7; } : func() int { return 2; }; return f(); }', 7)

    case('func make<T>(v T) func() T { return func() T { return v; }; } func main() int { f:=make<int>(7); return f(); }', 7)
    case('func identity<T>(v T) T { return v; } func apply(f func(int) int) int { return f(7); } func main() int { return apply(identity<int>); }', 7)
    case('type F = func() int; func main() int { fs: F[3]; for (i:=0;i<3;i++) { value:=i; fs[i]=func() int { return value; }; } return fs[0]()+10*fs[1]()+100*fs[2](); }', 210)
    case('type F = func() int; func main() int { fs: F[3]; data:=int[3]{1,2,3}; for (i,value:=range data) { fs[i]=func() int { return value; }; } return fs[0]()+10*fs[1]()+100*fs[2](); }', 65)
    case('func main() int { n:=3; boxed: any = func() int { n++; return n; }; f:=boxed.(func() int); return f(); }', 4)
    case('func main() { defer func(n int) { emit(n); }(65); emit(66); }', output="BA")

    case('func make() func() int { type Local = int; x: Local = 7; return func() int { y: Local = x; return y; }; } func main() int { f:=make(); return f(); }', 7)
    case('struct Box<T> { value T; } func (b Box<T>) reader() func() T { return func() T { return b.value; }; } func main() int { b:=Box<int>{7}; f:=b.reader(); return f(); }', 7)
    case('func make(n int) func() int { xs: int[n]; xs[0]=7; return func() int { return xs[0]; }; } func main() int { f:=make(2); return f(); }', 7)

    case('func main() { f: func(); defer func() { if(recover()!=null) emit(65); }(); f(); }', output="A")
    case('func main() { f: func(); defer func() { if(recover()!=null) emit(65); }(); defer f(); emit(66); }', output="BA")
    case('func main() { n:=65; defer func() { emit(n); }(); n=66; }', output="B")
    case('type F func(int) int; func main() int { f: F = (func(n int) int { return n+1; })->F; return f(6); }', 7)

    case('func make<T>(v T) func() T { type Local = T; return func() T { x: Local = v; return x; }; } func main() int { a:=make<int>(7); b:=make<string>("ok"); if(b()=="ok") return a(); return 1; }', 7)
    case('func make() func() int { type Local int; x: Local=7->Local; return func() int { y: Local=x; return y->int; }; } func main() int { f:=make(); return f(); }', 7)

    case('func main() int { n:=2; xs: buffer<int>[n]; xs[0]=5; f:=func() int { xs[0]++; return xs[0]+len(xs); }; return f(); }', 8)

    case('func main() { func() { emit(65); }(); func(n int) { emit(n); }(66); }', output="AB")
    case('f: func() int = func() int { return 7; }; func main() int { return f(); }', 7)

    for body, diagnostic in [
        ('func main() { f: func(int) int = func(s string) int { return 0; }; }', 'type mismatch'),
        ('func main() { f:=func(x int) int { return x; }; f("bad"); }', 'type mismatch'),
        ('func main() { f:=func(x int) int { return x; }; f(); }', 'argument count'),
        ('func main() { f:=func() int { return "bad"; }; }', 'type mismatch'),
        ('func main() { f:=func() { return missing; }; }', 'unresolved name'),
        ('func main() { f:=func() { break; }; }', 'break'),
    ]:
        source.write_text(PREFIX + body)
        result = run([compiler, 'check', str(source)])
        assert result.returncode != 0 and diagnostic in result.stderr.lower(), result

    # Values and callable signatures can cross package boundaries.
    (root / "ocean.mod").write_text("module example.com/closures\n")
    library = root / "lib"
    library.mkdir()
    (library / "lib.oc").write_text("package lib; func Make(n int) func() int { return func() int { n++; return n; }; } func Run(f func() int) int { return f(); }")
    source.write_text('package main; import "example.com/closures/lib"; func main() int { f:=lib.Make(6); return lib.Run(f); }')
    built = run([compiler, "build", str(source), "-o", str(executable)])
    assert built.returncode == 0, built.stderr
    result = run([str(executable)])
    assert result.returncode == 7 and result.stderr == "", result

print(f"{checks} closure runtime checks passed at -O0 and -O2; invalid signatures rejected")
