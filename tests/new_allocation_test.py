"""new(T) allocates independent heap storage initialized to zero(T)."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=20)


with tempfile.TemporaryDirectory(prefix="ocean new allocation ") as directory:
    root = Path(directory)
    source, ir, executable = (root / name for name in ("main.oc", "module.ll", "native"))
    checks = 0

    def program(text):
        global checks
        source.write_text("package main; " + text)
        emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        module = ir.read_text()
        assert 'call ptr @"\\01calloc"' in module, module
        for optimization in ("-O0", "-O2"):
            built = run([clang, optimization, "-Wno-override-module", "-x", "ir", str(ir), "-o", str(executable)])
            assert built.returncode == 0, built.stdout + built.stderr
            result = run([str(executable)])
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result
            checks += 1

    program('''func main() int {
        a := new(int); b := new(int);
        if (*a != 0 || *b != 0 || a == b) return 1;
        *a = 42; if (*a != 42 || *b != 0) return 2;
        x8 := new(i8); x16 := new(i16); x32 := new(i32); x64 := new(i64);
        f := new(float); f32 := new(f32); f64 := new(f64); flag := new(bool);
        if (*x8 != 0 || *x16 != 0 || *x32 != 0 || *x64 != 0 ||
            *f != 0.0 || *f32 != 0.0 || *f64 != 0.0 || *flag) return 3;
        *x8 = 7; *x16 = 8; *x32 = 9; *x64 = 10;
        *f = 1.5; *f32 = 2.5; *f64 = 3.5; *flag = true;
        if (*x8 != 7 || *x16 != 8 || *x32 != 9 || *x64 != 10 ||
            *f != 1.5 || *f32 != 2.5 || *f64 != 3.5 || !*flag) return 4;
        pp := new(*int); if (*pp != zero(*int)) return 5;
        *pp = a; **pp = 12; if (*a != 12) return 6;
        free(pp); free(a); free(b); free(x8); free(x16); free(x32); free(x64);
        free(f); free(f32); free(f64); free(flag); return 0;
    }''')

    program('''func create<T>() *T { return new(T); }
    func text() *string { p := new(string); *p = "heap"; return p; }
    func replace(p *string) { *p = "a much longer string"; }
    func clobber() { data: buffer<int>[1000]; data[0] = 1; }
    func main() int {
        p := new(string); q := new(string);
        if (len(*p) != 0 || *p != "" || p == q) return 1;
        *p = "hello"; replace(p); clobber();
        if (*p != "a much longer string" || len(*q) != 0) return 2;
        if ((*p)[0] != 'a') return 3;
        *p = ""; if (len(*p) != 0) return 4;
        r := text(); clobber(); if (*r != "heap") return 5;
        scalar := create<int>(); stringPointer := create<string>();
        if (*scalar != 0 || len(*stringPointer) != 0) return 6;
        *scalar = 9; *stringPointer = "generic";
        if (*scalar != 9 || *stringPointer != "generic") return 7;
        free(p); free(q); free(r); free(scalar); free(stringPointer); return 0;
    }''')

    program('''struct Inner { value int; name string; }
    struct Record { flag bool; inner Inner; values int[3]; items int[]; pointer *int; }
    struct Empty {}
    type Number int;
    type Alias = Record;
    func main() int {
        p := new(Record);
        if (p.flag || p.inner.value != 0 || len(p.inner.name) != 0 ||
            p.values[0] != 0 || p.values[2] != 0 || len(p.items) != 0 ||
            p.pointer != zero(*int)) return 1;
        p.flag = true; p.inner.value = 7; p.inner.name = "nested";
        p.values[2] = 8; p.items.append(9, 10);
        if (!p.flag || p.inner.value != 7 || p.inner.name != "nested" ||
            p.values[2] != 8 || p.items[1] != 10) return 2;
        fixed := new(int[3]); raw := new(buffer<int>[3]);
        if ((*fixed)[0] != 0 || (*fixed)[2] != 0 || (*raw)[2] != 0) return 3;
        (*fixed)[1] = 11; (*raw)[2] = 12;
        if ((*fixed)[1] != 11 || (*raw)[2] != 12) return 4;
        e := new(Empty); f := new(Empty); z := new(int[0]);
        if (e == f || len(*z) != 0) return 5;
        number := new(Number); alias := new(Alias);
        if (*number != zero(Number) || alias.inner.value != 0) return 6;
        free(p.items); free(p); free(fixed); free(raw); free(e); free(f); free(z);
        free(number); free(alias); return 0;
    }''')

    program('''extern i32 memcmp(buffer<i8> a, buffer<i8> b, int count);
    struct Padded { flag bool; number int; small i32; }
    func main() int {
        p := new(Padded); size := sizeof(Padded); expected: buffer<i8>[size];
        if (extern.memcmp(p->buffer<i8>, expected, sizeof(Padded)) != 0) return 1;
        free(p); return 0;
    }''')

    source.write_text('package main; func main() { p := new(void); }')
    rejected = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
    assert rejected.returncode != 0 and 'storage type' in rejected.stderr, rejected
    checks += 1

print(f"{checks} new allocation checks passed")
