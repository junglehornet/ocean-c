"""Compile source -> LLVM IR -> native executable and check observable behavior."""
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile

# Length-aware Ocean helpers, using explicit raw buffers at the C boundary.
PRINT = "extern int write(i32 fd, buffer<i8>[] data, int count); func puts(s string) i32 { extern.write(1, s.data, len(s)); newline: buffer<i8>[1] = {10}; extern.write(1, newline, 1); return 0->i32; } "
COMPARE = "extern i32 memcmp(buffer<i8>[] a, buffer<i8>[] b, int count); func strcmp(a string, b string) i32 { return extern.memcmp(a.data, b.data, len(a)); } "

compiler, clang = map(os.path.abspath, sys.argv[1:])

def run(args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, timeout=15, **kwargs)

with tempfile.TemporaryDirectory(prefix="ocean backend ") as directory:
    root = Path(directory)
    source = root / "source file.oc"
    exe = root / "native executable"
    ir = root / "module.ll"

    def program(text, status=0, stdout="", optimize=False):
        source.write_text("package main; " + text)
        built = run([compiler, "build", str(source), "-o", str(exe)])
        assert built.returncode == 0, built.stdout + built.stderr
        result = run([str(exe)])
        assert (result.returncode, result.stdout) == (status, stdout), result
        emitted = run([compiler, "build", "--emit-llvm", "-o", str(ir), str(source)])
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        assert "define i32 @main()" in ir.read_text()
        verified = run([clang, "-Wno-override-module", "-O2" if optimize else "-O0", "-x", "ir", str(ir), "-o", str(exe), "-lffi"])
        assert verified.returncode == 0, verified.stderr
        result = run([str(exe)])
        assert (result.returncode, result.stdout) == (status, stdout), result

    program("func main() int { return sizeof(*void); }", 8, optimize=True)
    program("extern *void malloc(int bytes); extern void free(*void memory); "
            "func main() int { p := extern.malloc(sizeof(int)) -> (*int); *p = 42; "
            "result := *p; extern.free(p); return result; }", 42, optimize=True)
    program("struct Holder { value *void; } func erase(p *int) *void { return p; } "
            "func pair(p *int) (*void, *void) { return p, p; } "
            "func main() int { x := 7; p: *void = &x; p = &x; h := Holder{value: &x}; "
            "a, b: *void = pair(&x); a, b = &x, &x; "
            "q := true ? &x : p; r := false ? p : &x; "
            "if (p != &x || &x != p || h.value != p || erase(&x) != p || "
            "a != b || q != r) return 1; return *(p -> (*int)); }", 7, optimize=True)
    program("func consume(p *void) int { return *(p -> (*int)); } "
            "func main() int { a := int[]{8}; p: *void = a.data; return consume(a.data); }", 8, optimize=True)
    # C void pointers and explicit casts retain pointer identity and the native ABI.
    program("extern *void malloc(int bytes); extern void free(*void memory); "
            "struct Node<T> { value T; next *Node<T>; } "
            "func main() int { raw := extern.malloc(sizeof(Node<int>)); "
            "if (raw == zero(*void)) panic \"allocation failed\"; "
            "node := raw -> (*Node<int>); *node = Node<int>{value: 42, next: null}; "
            "result := node.value; extern.free(node -> (*void)); return result; }", 42, optimize=True)
    program("func main() int { x := 7; p := (&x) -> (*void); bits := p -> int; "
            "q := bits -> (*int); *q = 9; return x; }", 9, optimize=True)
    program("extern *void malloc(int bytes); extern void free(*void memory); "
            "func main() int { raw := extern.malloc(16); b := raw -> (buffer<int>[]); "
            "b[0] = 3; b[1] = 5; result := b[0] + b[1]; extern.free(b -> (*void)); return result; }", 8, optimize=True)
    program("func main() int { a := (true -> int) -> float; b := 3.5 -> int; "
            "if (!(a -> bool) || (0 -> bool) || !(3 -> bool) || (0.0 -> bool)) return 1; "
            "return (a -> int) + b + ((-2) -> i8 -> int); }", 2, optimize=True)
    program("extern *void malloc(int bytes); extern void free(*void memory); "
            "func recur(n int) int[] { if (n == 0) return int[]{7}; return recur(n - 1); } "
            "func main() int { raw := extern.malloc(8); p := raw -> (*int); *p = 2; "
            "a := recur(3); result := a[0] + *p; extern.free(raw); return result; }", 9, optimize=True)

    # Strings compare full byte contents, including embedded NULs and length.
    program('func main() int { a := "hello"; b := "hel" + "lo"; '
            'if (!(a == b) || a != b || "a" == "ab" || !("ab" != "a") || '
            '"abc" == "abd" || !("abc" != "abd")) return 1; return 0; }', optimize=True)
    program('func main() int { a := zero(string); b := ""; '
            'if (!(a == b) || a != b || a == "x" || !("x" != a)) return 1; '
            'if (!("a\\0b" == "a\\0b") || "a\\0b" == "a\\0c" || '
            '"a\\0b" == "a") return 2; return 0; }', optimize=True)
    program('func same<T>(a T, b T) bool { return a == b; } '
            'func main() int { a := "abc"; b := a; b[1] = 120; '
            'bytes: i8[] = "abc"; if (a == b || !(a != b) || !(bytes == a) || '
            '!same<string>(a, "abc")) return 1; return 0; }', optimize=True)
    program(COMPARE + 'func make(n *int) string { (*n)++; return "same"; } '
            'func main() int { n := 0; if (!(make(&n) == make(&n)) || n != 2) return 1; '
            'if (make(&n) != make(&n) || n != 4) return 2; return 0; }', optimize=True)

    program("extern i32 abs(i32 value); func abs(value int) int { return value + 1; } "
            "func main() int { return abs(3) + extern.abs(-7); }", 11, optimize=True)
    program("extern i32 abs(i32 value); func main() int { abs := 4; return abs + extern.abs(-3); }", 7, optimize=True)

    program("extern i32 abs(i32 value); func abs(value int) int { return 99; } "
            "func main() int { b: buffer<int>[extern.abs(-3)]; b[extern.abs(-2)] = 7; "
            "return b[2]; }", 7, optimize=True)
    assert ir.read_text().count('call i32 @"abs"') == 2

    # zero(Type) matches declared defaults, including concrete generic substitutions.
    program("func main() int { return zero(int); }", 0, optimize=True)
    program("func main() int { if (!zero(bool) && zero(float) == 0.0 && zero(i32) == 0 && zero(i8) == 0) return 7; return 1; }", 7, optimize=True)
    program("func main() int { p: *int; q := zero(*int); if (p == q && q != null) return 7; return 1; }", 7, optimize=True)
    program("func main() int { s := zero(string); xs := zero(int[]); if (s.size == 0 && len(xs) == 0) return 7; return 1; }", 7, optimize=True)
    program("func main() int { b := zero(buffer<int>[3]); b[1] = 7; return b[0] + b[1] + b[2]; }", 7, optimize=True)
    program("struct Box<T> { value T; } func main() int { b := zero(Box<Box<int>>); return b.value.value; }", 0, optimize=True)
    program("struct Node<T> { value T; next *Node<T>; } func main() int { n := zero(Node<int>); if (n.value == 0 && n.next == zero(*Node<int>) && n.next != null) return 7; return 1; }", 7, optimize=True)
    program("struct S(int count) { text string; } func main() int { s := zero(S); if (s.count == 0 && s.text.size == 0) return 7; return 1; }", 7, optimize=True)
    program("func main() int { a := zero(buffer<string>[2]); b: buffer<string>[2]; if (a[0].size + a[1].size + b[0].size + b[1].size == 0) return 7; return 1; }", 7, optimize=True)
    program("struct S { text string; items int[]; } func main() int { a := zero(buffer<S>[2]); if (a[1].text.size == 0 && a[0].items.size == 0) return 7; return 1; }", 7, optimize=True)
    program("func reset<T any>(value *T) { *value = zero(T); } func main() int { x := 42; flag := true; reset<int>(&x); reset[bool](&flag); if (x == 0 && !flag) return 7; return 1; }", 7, optimize=True)
    program("struct Box<T> { value T; } func reset<T>(p *T) { *p = zero(T); } func main() int { b := Box<int>{99}; reset<Box<int>>(&b); return b.value; }", 0, optimize=True)
    program("struct Box<T> { value T; } func (b *Box<T>) reset() { b.value = zero(T); } func main() int { b := Box<int>{99}; b.reset(); return b.value; }", 0, optimize=True)
    program("struct Factory {} func (f Factory) empty<T>() T { return zero(T); } func main() int { f := Factory{}; s := f.empty<string>(); if (s.size == 0) return 7; return 1; }", 7, optimize=True)
    program("func zeroOf<T>() T { return zero(T); } func main() int { s := zeroOf<string>(); a := zeroOf<int[]>(); if (s.size + a.size == 0) return 7; return 1; }", 7, optimize=True)
    program("func countDown<T>(n int) T { if (n == 0) return zero(T); return countDown<T>(n - 1); } func main() int { return countDown<int>(10); }", 0, optimize=True)
    program("struct Box<T> { value T; } func main() int { return zero(Box<int>).value + zero(buffer<int>[3])[1]; }", 0, optimize=True)
    program("func main() int { return sizeof(zero(buffer<int>[3])); }", 24, optimize=True)
    program("func main() int { sum := 0; for (i := 0; i < 100000; i++) { z := zero(string); sum += z.size; } return sum; }", 0, optimize=True)

    program("struct Box<T> { value T; } func (b Box<T>) get() T { return b.value; } func main() int { b := Box<int>{7}; return b.get(); }", 7, optimize=True)
    # Null is pointer-only and distinct from zero-address default pointers.
    program("func main() int { zero: *int; p: *int = null; if (zero != null && p == null && p != zero && null == p && null == null && !(null != null)) { return 7; } return 1; }", 7, optimize=True)
    program("struct S { value int; next *S; } func none() *S { return null; } func main() int { s: S; p := none(); if (s.value == 0 && s.next != null && p == null) { return sizeof(s); } return 1; }", 16, optimize=True)
    program("struct Node<T> { value T; next *Node<T>; } func (n *Node<T>) append(other *Node<T>) { for (n.next != null) { n = n.next; } n.next = other; } func main() int { a := Node<int>{3, null}; b := Node<int>{4, null}; a.append(&b); return a.next.value; }", 4, optimize=True)
    program("func take(p *int) bool { return p == null; } func main() int { x := 4; p: *int = null; q := &x; p, q = q, p; if (q == null && *p == 4 && take(null)) { return 7; } return 1; }", 7, optimize=True)
    program("func main() int { x := 7; p := true ? null : &x; q := false ? null : &x; r: *int = true ? null : null; if (p == null && r == null) { return *q; } return 1; }", 7, optimize=True)
    program("func main() int { p := null->(*int); if (p == null) { return 7; } return 1; }", 7, optimize=True)
    program("struct S {} func (s S) empty<U>() *U { return null; } func main() int { s := S{}; p := s.empty<int>(); if (p == null) { return 7; } return 1; }", 7, optimize=True)
    program("func main() int { values: buffer<*int>[2] = {null}; if (values[0] == null && values[1] != null) { return 7; } return 1; }", 7, optimize=True)
    program("func main() int { x := 7; xs := {&x, null}; if (xs[1] == null) { return *xs[0]; } return 1; }", 7, optimize=True)
    # Explicit null must never be dereferenced as if it were a zeroed object.
    for invalid_access in [
        "func main() int { p: *int = null; return *p; }",
        "struct S { value int; } func main() int { p: *S = null; return p.value; }",
        "struct S { value int; } func (s S) get() int { return s.value; } func main() int { p: *S = null; return p.get(); }",
    ]:
        source.write_text("package main; " + invalid_access)
        built = run([compiler, "build", str(source), "-o", str(exe)])
        assert built.returncode == 0, built.stdout + built.stderr
        assert run([str(exe)]).returncode != 0
    # Method-specific parameters specialize independently of receiver bindings.
    program("struct Box<T> { value T; } func (b Box<T>) pair<U>(other U) (T, U) { return b.value, other; } func main() int { b := Box<int>{3}; x, y := b.pair<int>(4); return x + y; }", 7, optimize=True)
    program("struct Box<T> { value T; } func (b Box<T>) take<U any>(other U) U { return other; } func main() int { b := Box<int>{1}; x := b.take<int>(7); flag := b.take<bool>(true); y := b.take<int>(3); if (flag) { return x + y; } return 0; }", 10, optimize=True)
    assert sum(line.startswith("define ") and "ocean.method." in line for line in ir.read_text().splitlines()) == 2
    program("struct Box<T> { value T; } func (b Box<T>) pair<U>(other U) (T, U) { return b.value, other; } func main() int { a := Box<int>{3}; b := Box<i8>{2}; x, y := a.pair<int>(4); z, w := b.pair<int>(5); return x + y + z + w; }", 14, optimize=True)
    assert sum(line.startswith("define ") and "ocean.method." in line for line in ir.read_text().splitlines()) == 2
    program("struct Factory {} func (f Factory) make<U>(value U) Box<U> { return Box<U>{value}; } struct Box<T> { value T; } func main() int { f := Factory{}; b := f.make<int>(9); return b.value; }", 9, optimize=True)
    program("struct Factory {} func (f *Factory) take<U>(value U) U { return value; } func main() int { f := Factory{}; p := &f; return f.take<int>(3) + p.take<int>(4); }", 7, optimize=True)
    program("struct Box<T> { value T; } func (b Box<T>) take<U>(value U, count int) U { if (count == 0) { return value; } return b.take<U>(value, count - 1); } func main() int { b := Box<int>{1}; return b.take<int>(8, 3); }", 8, optimize=True)
    program("struct Box<T> { value T; } func (b Box<T>) inner<V>(value V) V { return value; } func (b Box<T>) outer<U>(value U) U { return b.inner<U>(value); } func main() int { b := Box<bool>{true}; return b.outer<int>(6); }", 6, optimize=True)
    program("struct Box<T> { value T; } func (b Box<T>) last<U>(values ...U) U { return values[len(values)-1]; } func main() int { b := Box<bool>{true}; values := int[]{2, 4, 8}; return b.last<int>(values...); }", 8, optimize=True)
    program("struct Factory {} func (f Factory) take<U>(value U) U { return value; } struct Box<T> { value T; } func main() int { f := Factory{}; b := f.take<Box<int>>(Box<int>{7}); return b.value; }", 7, optimize=True)
    program("struct Factory {} func (f Factory) pair<A, B>(a A, b B) (A, B) { return a, b; } func main() int { f := Factory{}; a, b := f.pair<int, i8>(3, 4); return a + b; }", 7, optimize=True)
    program("struct Factory {} func (f Factory) take[U](value U) U { return value; } func main() int { f := Factory{}; return f.take[int](5); }", 5, optimize=True)
    # Receiver methods specialize by concrete struct arguments and preserve value/pointer semantics.
    program("struct Node<T any> { value T; } func (n *Node<T>) set(value T) { n.value = value; } func (n Node<T>) get() T { return n.value; } func main() int { n := Node<int>{3}; n.set(7); return n.get(); }", 7, optimize=True)
    program("struct Box<T any> { value T; } func (b Box<E>) get() E { return b.value; } func main() int { a := Box<int>{8}; b := Box<bool>{true}; c := Box<i8>{3}; if (b.get()) { return a.get() + c.get(); } return 0; }", 11, optimize=True)
    assert sum(line.startswith("define ") and "ocean.method." in line for line in ir.read_text().splitlines()) == 3
    program("struct Box<T any> { value T; } func main() int { b := Box<int>{4}; p := &b; return b.get() + p.get() + b.get(); } func (b Box<T>) get() T { return b.value; }", 12, optimize=True)
    assert sum(line.startswith("define ") and "ocean.method." in line for line in ir.read_text().splitlines()) == 1
    program("struct Box<T any> { value T; } func (b Box<T>) replace(value T) T { b.value = value; return b.value; } func main() int { b := Box<int>{3}; p := &b; x := p.replace(8); return x + b.value; }", 11, optimize=True)
    program("struct Counter { value int; } func (c *Counter) inc() { c.value++; } func main() int { c := Counter{1}; p := &c; p.inc(); c.inc(); return c.value; }", 3, optimize=True)
    program("struct Node<T any> { value T; next *Node<T>; } func (n *Node<T>) sum(count int) T { if (count == 1) { return n.value; } return n.next.sum(count - 1); } func main() int { b := Node<int>{4}; a := Node<int>{3, &b}; return a.sum(2); }", 4, optimize=True)
    program("struct A<T any> { value T; } struct B<T any> { value T; } func (a A<T>) get() T { return a.value; } func (b B<T>) get() T { return b.value; } func get() int { return 1; } func main() int { a := A<int>{2}; b := B<int>{3}; return a.get() + b.get() + get(); }", 6, optimize=True)
    program("struct Pair<T any> { first T; second T; } func (p Pair<T>) values() (T, T) { return p.first, p.second; } func main() int { p := Pair<int>{2,3}; a, b := p.values(); return a + b; }", 5, optimize=True)
    program("struct Box<T any> { value T; } func (b *Box<T>) last(values ...T) T { b.value = values[len(values)-1]; return b.value; } func main() int { b := Box<int>{1}; return b.last(2, 3, 4); }", 4, optimize=True)
    program("struct Box<T any> { value T; } func (b Box<T>) get() T { return b.value; } func make(counter *int) Box<int> { (*counter)++; return Box<int>{4}; } func main() int { count := 0; result := make(&count).get(); return result + count; }", 5, optimize=True)
    program("struct Box<T any> { value T; } func (b *Box<T>) set(value T) { b.value = value; } func (b *Box<T>) copy(other *Box<T>) { b.set(other.value); } func main() int { a := Box<int>{1}; b := Box<int>{9}; a.copy(&b); return a.value; }", 9, optimize=True)
    program("struct Box<T any> { value T; } func (b *Box<T>) set(value T) { b.value = value; } func (b Box<T>) get() T { return b.value; } func main() int { b := Box<int>{1}; p := &b; q := &p; q.set(9); return q.get(); }", 9, optimize=True)
    program("struct Item { value int; } struct Box<T any> { value T; } func (b *Box<T>) set(value T) { b.value = value; } func (b Box<T>) get() T { return b.value; } func main() int { b := Box<Item>{{1}}; b.set(Item{7}); item := b.get(); return item.value; }", 7, optimize=True)
    program("func main() {}")
    program("func main() int { return 42; }", 42)
    program(PRINT + 'func main() { puts("Hello, LLVM!"); }', stdout="Hello, LLVM!\n")
    program("func factorial(n int) int { if (n < 2) { return 1; } else { return n * factorial(n - 1); } } func main() int { return factorial(5); }", 120)
    program("func main() int { total := 0; for (i := 0; i < 10; i++) { if (i == 3) { continue; } if (i == 6) { break; } total += i; } return total; }", 12)
    program("func main() int { x := 10; { x := 99; x++; } return x; }", 10)
    program("func main() int { x := 0; a := false && (++x > 0); b := true || (++x > 0); return x; }", 0)
    program("func main() int { x := 0; y := true ? ++x : 90; z := false ? 90 : ++x; return x + y + z; }", 5)
    program("func main() int { x := 0; for (defer x < 3) { x++; } return x; }", 3)
    program("func main() int { total := 0; for (i := 0; i < 3; i++) { for (j := 0; j < 5; j++) { if (j == 2) { break; } total++; } } return total; }", 6)
    program("func main() int { x := 7; x *= 3; x -= 1; x /= 2; x %= 6; x |= 8; x &= 15; x ^= 1; x <<= 1; x >>= 1; return x; }", 13)
    program("func main() int { x := 3.5; y := -x + 6.5; return (y * 2.0)->int; }", 6)
    program("func main() int { x: int; y: bool; if (!y) { x = 8; } return x; }", 8)
    program("func answer() (int result) { result = 23; return; } func main() int { return answer(); }", 23)
    program("func narrow(x i32) i32 { return x + 1; } func main() int { return narrow(40); }", 41)
    program(COMPARE + 'func main() int { return strcmp("a", "b"); }', 255)
    program(PRINT + 'func main() { puts("quote: \\\" slash: \\\\ tab:\\t"); }', stdout='quote: " slash: \\ tab:\t\n')

    program("func main() int { myBuffer: buffer<int>[4] = {1, 2, 3, 4}; myBuffer[3] += 6; myBuffer[0]++; return myBuffer[3] + myBuffer[0]; }", 12)
    program("func main() int { b: buffer<int>[4] = {7}; c := b; c[0] = 9; return b[0] + c[3]; }", 7)
    program("func main() int { b: buffer<int>[2]; b = {8, 9}; return b[0] + b[1]; }", 17)
    program("func main() int { b: buffer<buffer<int>[2]>[2] = {{1, 2}, {3, 4}}; b[1][0]++; return b[1][0]; }", 4)
    program("func main() int { b: buffer<i32>[3] = {1, 2, 3}; i := 1; b[i] *= 5; return b[i]; }", 10)
    program("func main() int { b: buffer<float>[2] = {1.5, 2.5}; return (b[0] + b[1])->int; }", 4)
    program(PRINT + 'func main() { b: buffer<string>[2] = {"hello", "buffer"}; puts(b[1]); }', stdout="buffer\n")
    program("func main() int { b: buffer<bool>[2] = {true, false}; if (b[0] && !b[1]) { return 3; } return 0; }", 3)
    program("func make() buffer<int>[2] { return buffer<int>[2]{4, 5}; } func sum(b buffer<int>[2]) int { return b[0] + b[1]; } func main() int { return sum(make()); }", 9)
    program("func main() int { b: buffer<int>[3]; total := 0; for (i := 0; i < 3; i++) { b[i] = i + 1; total += b[i]; } return total; }", 6)

    program("func sum(b buffer<int>[2]) int { b[0]++; return b[0] + b[1]; } func main() int { b: buffer<int>[2] = {2, 3}; return sum(b) + b[0] + sum(buffer<int>[2]{4, 5}); }", 18)
    program("func make() buffer<int>[2] { return buffer<int>[2]{6, 7}; } func main() int { return make()[1]; }", 7)

    program("extern void srand(i32 seed); extern i32 abs(i32); func main() int { extern.srand(1); return extern.abs(-5); }", 5)

    program("func sum(values ...int) int { total := 0; for (i := 0; i < len(values); i++) { total += values[i]; } return total; } func main() int { return sum() + sum(2, 3, 4); }", 9)
    program("func add(base int, values ...int) int { for (i := 0; i < len(values); i++) { base += values[i]; } return base; } func main() int { return add(5) + add(1, 2, 3); }", 11)
    program("func sum(values ...int) int { total := 0; for (i := 0; i < len(values); i++) { total += values[i]; } return total; } func forward(values ...int) int { return sum(values...); } func main() int { b: buffer<int>[3] = {4, 5, 6}; return forward(b[0], b[1], b[2]) + forward(); }", 15)
    program("func total(values ...float) float { sum := 0.0; for (i := 0; i < len(values); i++) { sum += values[i]; } return sum; } func main() int { return total(1.5, 2.5)->int; }", 4)
    program(PRINT + 'func log(lines ...string) { for (i := 0; i < len(lines); i++) { puts(lines[i]); } } func main() { log("hello", "world"); log(); }', stdout="hello\nworld\n")
    program("func first(values ...buffer<int>[2]) int { values[0][1]++; return values[0][1]; } func main() int { b: buffer<int>[2] = {2, 3}; return first(b) + b[1]; }", 7)
    program("func f(n int, xs ...int) int { if (n == 0) { return len(xs); } return f(n - 1, xs...); } func main() int { return f(4, 10, 20, 30); }", 3)
    program("func pair() (int, bool) { return 7, true; } func main() int { number, ok := pair(); if (ok) { return number; } return 0; }", 7)
    program("func pair() (int, int) { return 3, 8; } func main() int { a, b: int = pair(); a, b = b, a; return a * 10 + b; }", 83)
    program("func pair() (int, int) { return 2, 9; } func main() int { _, b := pair(); a := 0; a, _ = pair(); return a + b; }", 11)
    program("func named() (int number, bool ok) { number = 6; ok = true; return; } func relay() (int, bool) { return named(); } func main() int { n, ok := relay(); if (ok) { return n; } return 0; }", 6)
    program("func pair() (int, int) { return 2, 5; } func add(a int, b int) int { return a + b; } func sum(xs ...int) int { n := 0; for (i := 0; i < len(xs); i++) { n += xs[i]; } return n; } func main() int { return add(pair()) + sum(pair()); }", 14)
    program("func pair(x int) (int, int) { if (x == 0) { return 1, 2; } return pair(x - 1); } func main() int { a, b := pair(3); return a + b; }", 3)
    program("func pair() (buffer<int>[2], float, bool, i32) { return buffer<int>[2]{3, 4}, 2.5, true, 8; } func main() int { b, f, ok, n := pair(); if (ok) { return b[1] + f->int + n; } return 0; }", 14)
    # Runtime C varargs: homogeneous arrays, heterogeneous forwarding, register
    # overflow, default promotions, empty tails, prefixes, and return values.
    program('extern i32 printf(buffer<i8> format, ...); '
            'func println(format string, values ...any) i32 { '
            'return extern.printf((format + "\\n\\0").data, values...); } '
            'func relay(format string, values ...any) i32 { return println(format, values...); } '
            'func main() int { n := relay("%d %.1f %s %d %d", -7, 2.5, "ok\\0".data, true, -3->i8); '
            'if (n != 15) return 1; relay("empty"); values := int[]{11, 12}; relay("%ld %ld %ld", 10, values...); '
            'relay("%d %d %d %d %d %d %d %d %d", 1,2,3,4,5,6,7,8,9); '
            'relay("%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f", 1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0); '
            'return 0; }', stdout="-7 2.5 ok 1 -3\nempty\n10 11 12\n1 2 3 4 5 6 7 8 9\n1.0 2.0 3.0 4.0 5.0 6.0 7.0 8.0 9.0\n", optimize=True)
    program('extern i32 printf(buffer<i8> format, ...); '
            'func numbers(n int) int[] { a := int[n]; for (i := 0; i < n; i++) a[i] = i + 2; return a; } '
            'func main() { extern.printf("%ld %ld %ld\\n\\0".data, 1, numbers(2)...); '
            'empty := int[0]; extern.printf("empty\\n\\0".data, empty...); '
            'small: i8[] = {-8, 7}; extern.printf("%d %d\\n\\0".data, small...); '
            'flags: bool[] = {true, false}; extern.printf("%d %d\\n\\0".data, flags...); }',
            stdout="1 2 3\nempty\n-8 7\n1 0\n", optimize=True)
    program('extern i32 printf(buffer<i8> format, ...); '
            'func first(values ...any) { extern.printf("%d %.1f\\n\\0".data, values[0], values[1]); } '
            'func main() { first(7, 2.5); }', stdout="7 2.5\n", optimize=True)
    program("extern i32 printf(buffer<i8>[] format, ...); func main() { extern.printf(\"%s %d %.1f %d\\n\\0\".data, \"ok\\0\".data, 4->i32, 2.5, true); }", stdout="ok 4 2.5 1\n")
    program("func tick(b buffer<int>[1]) (int, int) { return b[0] + 1, b[0] + 2; } func main() int { b: buffer<int>[2]; i := 0; i, b[i] = tick(buffer<int>[1]{5}); return i + b[0]; }", 13)

    program(PRINT + 'func pair() (int, int) { puts("once"); return 4, 5; } func main() int { a, b := pair(); return a + b; }', 9, stdout="once\n")
    program("func main() int { a := 1; b := 2; a, b = b, ++a; return a * 10 + b; }", 22)
    program("func f(xs ...int) int { list := xs; list[0]++; return len(list) + xs[0]; } func main() int { return f(5, 6); }", 7)
    program("func pair() (int, int) { return 3, 4; } func main() int { b: buffer<int>[2]; i := 0; b[i], b[1] = pair(); return b[0] + b[1]; }", 7)

    program("func fill(b buffer<int>[], n int) { for (i := 0; i < n; i++) { b[i] = i + 1; } } func main() int { n := 4; b: buffer<int>[n]; fill(b, n); return b[0] + b[3]; }", 5)
    program("func fill(b buffer<int>[], n int) { b[n - 1] = 9; } func main() int { b: buffer<int>[3] = {1, 2, 3}; fill(b, 3); return b[2]; }", 9)
    program("func read(n int, b buffer<int>[n]) int { return b[n - 1]; } func main() int { n := 3; b: buffer<int>[n]; b[2] = 7; return read(n, b); }", 7)
    program("func main() int { n := 2; b: buffer<int>[n++]; n = 1; b[1] = 8; alias: buffer<int>[] = b; alias[1]++; return b[1] + n; }", 10)
    program("func size(n int) int { return n + 1; } func main() int { n := 3; b: buffer<int>[size(n)]; b[3] = 4; return b[3]; }", 4)
    program("func main() int { n := 2; b: buffer<int>[n * 2 + 1]; b[4] = 7; return b[4]; }", 7)
    program("func main() int { b: buffer<int>[] = {4, 5}; c: buffer<int> = b; c[0]++; return b[0] + c[1]; }", 10)
    program("func main() int { b: buffer<int>[2] = {3, 4}; view: buffer<int>[] = b; view[1] = 8; return b[1]; }", 8)
    program("func main() int { n := 2; b: buffer<buffer<int>[2]>[n]; b[1][0] = 9; return b[1][0]; }", 9)
    program(PRINT + 'func main() { n := 2; b: buffer<string>[n]; b[0] = "runtime"; puts(b[0]); }', stdout="runtime\n")
    program("extern i32 memcmp(buffer<i32>[] a, buffer<i32>[] b, int size); func main() int { a: buffer<i32>[2] = {3, 4}; b: buffer<i32>[] = {3, 4}; return extern.memcmp(a, b, 8); }", 0)
    program("extern buffer<int>[] malloc(int bytes); extern void free(buffer<int>[] data); func main() int { b: buffer<int>[] = extern.malloc(32); b[3] = 6; result := b[3]; extern.free(b); return result; }", 6)
    # Each iteration allocates a large VLA; continue/break must restore the scope's stack.
    program("func main() int { n := 100000; result := 0; for (i := 0; i < 200; i++) { b: buffer<int>[n]; b[0] = i; if (i < 100) { continue; } result = b[0]; if (i == 101) { break; } } return result; }", 101)
    program("func main() int { n := 200000; for (i := 0; i < 100; i++) { if (i % 2 == 0) { b: buffer<int>[n]; b[n - 1] = i; } else { b: buffer<int>[n]; b[0] = i; } } return 3; }", 3)

    program('extern i32 sscanf(buffer<i8>[] text, buffer<i8>[] format, ...); func main() int { b: buffer<i32>[] = {0}; extern.sscanf("7\\0".data, "%d\\0".data, b); return b[0]; }', 7)

    source.write_text("package main; func last(b buffer<int>[], n int) int { return b[n - 1]; } func main() int { n := 3; { b: buffer<int>[n]; b[2] = 4; value := last(b, n); } return 0; }")
    emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
    assert emitted.returncode == 0, emitted.stdout + emitted.stderr
    runtime = ir.read_text()
    assert 'define i64 @"ocean.__pkg0_last"(ptr %arg0, i64 %arg1)' in runtime
    assert "alloca i64, i64 %" in runtime
    assert "getelementptr i64, ptr" in runtime and "getelementptr inbounds" not in runtime
    assert "call void @llvm.stackrestore.p0" in runtime

    program("func main() int { arr: int[] = int[5]; arr[4] = 9; return len(arr) + arr[4]; }", 14)
    program("func main() int { a: int[] = int[5]; return a.size; }", 40)
    program("func main() int { a: i32[] = i32[3]; a.data[2] = 7; return a.size + len(a) + a[2]; }", 22)
    program("func main() int { a: bool[] = bool[5]; a[4] = true; if (a[4]) { return a.size + len(a); } return 0; }", 10)
    program("func main() int { n := 3; a: int[] = int[n++]; n = 10; return len(a) + a.size; }", 27)
    program("func count(n int) int { return n + 1; } func main() int { a := float[count(2)]; a[1] = 2.5; return len(a) + a.size; }", 27)
    program("func main() int { a: int[]; b := int[0]; c := int[-0]; return len(a) + len(b) + len(c); }", 0)
    program("func fill(a *int[]) { for (i := 0; i < len(*a); i++) { (*a).data[i] = i + 2; } } func main() int { a := int[3]; fill(&a); copy := a; copy[2] = 8; return a[0] + a[2]; }", 6)
    program("func main() int { a: int[][] = int[][2]; a[0] = int[3]; a[1] = int[4]; return len(a) + len(a[0]) + len(a[1]); }", 9)
    program("func main() int { a: buffer<int>[2][] = buffer<int>[2][3]; a[1][0] = 7; return a.size + len(a) + a[1][0]; }", 58)
    program(PRINT + 'func main() { a := string[2]; a[0] = "array"; puts(a.data[0]); }', stdout="array\n")
    program("extern i32 memcmp(int[] a, buffer<int>[] expected, int bytes); func main() int { a := int[2]; a[0] = 11; a[1] = 22; expected: buffer<int>[3] = {16, 11, 22}; return extern.memcmp(a, expected, 24); }", 0)
    program("func main() int { n := 100000; result := 0; for (i := 0; i < 100; i++) { a := int[n]; a[0] = i; if (i < 98) { continue; } result = a[0]; break; } return result; }", 98)
    program("func main() int { n := 3; a := int[n++]; return n; }", 4)
    program("func main() int { n := 3; size := sizeof(++n); return n + size + sizeof(i32); }", 15)
    program(PRINT + 'func main() int { return sizeof(puts("not executed")); }', 4)
    program("func main() int { a := int[2]; return sizeof(a) + sizeof(a.data) + sizeof(a.size); }", 24)
    program("func id(a int[]) int[] { return a; } func main() int { a := int[3]; return len(id(a)); }", 3)
    program("func stats(a int[]) (int, int) { return len(a), a.size; } func main() int { a := int[3]; n, bytes := stats(a); return n + bytes; }", 27)
    program("func main() int { n := 0; a := bool[4]; size := len((n++, a)); return n + size; }", 5)
    program("func len(n int) int { return n + 1; } func main() int { return len(4); }", 5)

    program("func stats(a int[]) (int[], int) { return a, len(a); } func main() int { a := int[3]; copy, n := stats(a); return len(copy) + n; }", 6)
    program("func total(arrays ...int[]) int { n := 0; for (i := 0; i < len(arrays); i++) { n += len(arrays[i]); } return n; } func main() int { return total(int[2], int[3]); }", 5)
    program("func named() (int[] a, int n) { n = len(a); return; } func main() int { _, n := named(); return n; }", 0)

    source.write_text("package main; func main() int { n := 5; a: int[] = int[n]; a.data[0] = 4; return len(a); }")
    emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
    assert emitted.returncode == 0, emitted.stdout + emitted.stderr
    layout = ir.read_text()
    assert "getelementptr { i64, [0 x i64] }" in layout
    assert "alloca i8, i64" in layout and "sdiv i64" in layout
    assert "{ i64, ptr }" not in layout  # Payload is inline, rather than a separate pointer field.

    program("func main() int { a := int[2]; a[0] = 3; a[1] = 4; i := 0; a[i++] += 2; return a[0] + a[1] + i; }", 10)
    program("func get(a int[], calls buffer<int>[]) int[] { calls[0]++; return a; } func main() int { a := int[2]; a[1] = 7; calls: buffer<int>[1]; return get(a, calls)[1] + calls[0]; }", 8)
    program("func main() int { a := int[1]; b := int[2]; a[0] = 4; b[0] = 9; return a[(a = b, 0)]; }", 4)

    program("func main() int { a := int[4]; a.data = {2, 3}; return a[0] + a[1] + a[2] + a[3]; }", 5)
    program("func main() int { a := int[3]; a.data = {1, 2, 3}; a.data = {a[1], a[0]}; return a[0] * 10 + a[1] + a[2]; }", 21)
    program("func main() int { a := int[3]; a.data = {1, 2, 3}; a.data = {}; return a[0] + a[1] + a[2] + len(a); }", 3)
    program("func main() int { a := i32[2]; b: buffer<i32>[3] = {5, 6, 7}; a.data = b; b[0] = 9; return a[0] + a[1] + a.size; }", 19)
    program("func fill(a *int[], source buffer<int>[]) { (*a).data = source; } func main() int { a := int[3]; b: buffer<int>[3] = {4, 5, 6}; fill(&a, b); return a[0] + a[2] + len(a); }", 13)
    program("func main() int { a := int[2]; a.data = {3, 4}; a.data = a.data; return a[0] + a[1]; }", 7)
    program(COMPARE + 'func main() int { a := string[3]; a.data = {"hi"}; return strcmp(a[1], "") + strcmp(a[2], ""); }', 0)
    program("func main() int { a := float[3]; a.data = {2.5}; return (a[0] + a[1] + a[2])->int; }", 2)
    program("func main() int { a := bool[3]; a.data = {true}; if (a[0] && !a[1] && !a[2]) { return 4; } return 0; }", 4)
    program("func main() int { a: int[][] = int[][3]; a.data = {{1, 2}, {3}}; return len(a) + len(a[0]) + len(a[1]) + len(a[2]) + a[0][1]; }", 8)
    program(COMPARE + 'func main() int { a := buffer<string>[2][2]; a.data = {{"hi"}}; return strcmp(a[0][1], "") + strcmp(a[1][0], ""); }', 0)
    program("func source(b buffer<int>[], calls buffer<int>[]) buffer<int>[] { calls[0]++; return b; } func target(a int[], calls buffer<int>[]) int[] { calls[0]++; return a; } func main() int { a: int[] = {0, 0}; b: buffer<int>[2] = {5, 6}; calls: buffer<int>[1]; target(a, calls).data = source(b, calls); return calls[0] + a[0] + a[1]; }", 2)
    program("func main() int { n := 100000; result := 0; for (i := 0; i < 100; i++) { a := int[n]; a.data = {i}; if (i < 98) { continue; } result = a[0] + a[n - 1]; break; } return result; }", 98)
    program("func main() int { a := int[0]; a.data = {}; b: buffer<int>[] = {}; a.data = b; return len(a); }", 0)

    program("func sum(xs ...int) int { total := 0; for (i := 0; i < len(xs); i++) { total += xs[i]; } return total; } func main() int { a := int[3]; a.data = {2, 3, 4}; return sum(a...); }", 9)
    program("func sum(base int, xs ...int) int { for (i := 0; i < len(xs); i++) { base += xs[i]; } return base; } func main() int { a := int[2]; a.data = {3, 4}; return sum(5, 1, 2, a...); }", 15)
    program("func change(xs ...int) { xs[0] = 9; } func main() int { a := int[2]; a.data = {1, 2}; change(a...); return a[0]; }", 1)
    program("func change(xs ...int) { xs[1] = 9; } func main() int { a := int[2]; a.data = {1, 2}; change(8, a...); return a[0]; }", 1)
    program("func fill(xs ...int) { xs.data = {7}; } func main() int { a := int[3]; a.data = {1, 2, 3}; fill(a...); return a[0] + a[1] + a[2] + len(a); }", 9)
    program("func inspect(a int[]) int { return len(a) + a.size; } func f(xs ...int) int { copy: int[] = xs; return inspect(copy); } func main() int { return f(1, 2, 3); }", 27)
    program("extern i32 memcmp(int[] a, buffer<int>[] expected, int bytes); func f(xs ...int) i32 { expected: buffer<int>[3] = {16, 2, 3}; return extern.memcmp(xs, expected, 24); } func main() int { return f(2, 3); }", 0)
    program("func collect(xs ...int) int[] { return xs; } func main() int { a := collect(2, 3); return len(a) + a[1]; }", 5)
    program("func sum(xs ...int) int { n := 0; for (i := 0; i < len(xs); i++) { n += xs[i]; } return n; } func forward(xs ...int) int { return sum(xs...); } func main() int { a := int[2]; a.data = {4, 5}; return forward(a...); }", 9)
    program("func count(xs ...int) int { return len(xs); } func main() int { a := int[0]; return count(a...) + count() + count(1, a...); }", 1)
    program("func total(xs ...float) float { n := 0.0; for (i := 0; i < len(xs); i++) { n += xs[i]; } return n; } func main() int { a := float[2]; a.data = {1.5, 2.5}; return total(2.0, a...)->int; }", 6)
    program(PRINT + 'func log(xs ...string) { for (i := 0; i < len(xs); i++) { puts(xs[i]); } } func main() { a := string[2]; a.data = {"one", "two"}; log("zero", a...); }', stdout="zero\none\ntwo\n")
    program("func count(xs ...int[]) int { n := 0; for (i := 0; i < len(xs); i++) { n += len(xs[i]); } return n; } func main() int { a := int[][2]; a.data = {{1}, {2, 3}}; return count(a...); }", 3)
    program("func get(a int[], calls buffer<int>[]) int[] { calls[0]++; return a; } func count(xs ...int) int { return len(xs); } func main() int { a := int[2]; calls: buffer<int>[1]; return count(7, get(a, calls)...) + calls[0]; }", 4)
    program("func count(xs ...int) int { return len(xs); } func main() int { total := 0; for (i := 0; i < 150; i++) { total += count(1, 2, 3); } return total % 100; }", 50)

    source.write_text("package main; func inspect(a int[]) int { return len(a); } func f(xs ...int) int { return inspect(xs) + xs.size; } func main() int { return f(1, 2); }")
    emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
    assert emitted.returncode == 0, emitted.stdout + emitted.stderr
    variadic_ir = ir.read_text()
    assert 'define i64 @"ocean.__pkg0_f"(ptr %arg0)' in variadic_ir
    assert 'define i64 @"ocean.__pkg0_inspect"(ptr %arg0)' in variadic_ir
    assert "getelementptr { i64, [0 x i64] }" in variadic_ir
    # Runtime helpers may use any boxes; typed variadic functions must not.
    typed_functions = re.findall(r'define i64 @"ocean.__pkg0_(?:f|inspect)"[^\n]*\{(.*?)^}',
                                 variadic_ir, re.MULTILINE | re.DOTALL)
    assert len(typed_functions) == 2
    assert all("{ ptr, i64 }" not in body for body in typed_functions)

    program("func count(xs ...int) int { return len(xs); } func main() int { a := int[100000]; a.data = {}; total := 0; for (i := 0; i < 100; i++) { total += count(1, a...); } return total % 100; }", 0)
    program("func replace(xs ...int) int { xs = int[1]; xs[0] = 7; return len(xs); } func main() int { a := int[3]; return replace(a...) + len(a); }", 4)

    program("func main() int { a: int[] = {1, 2, 3}; return len(a) + a[0] + a[2]; }", 7)
    program("func main() int { a := {1, 2}; alias := a; a = {3, 4, 5}; return len(a) + len(alias) + a[2] + alias[0]; }", 11)
    program("func main() int { a: int[] = {}; b := int[]{}; return len(a) + len(b); }", 0)
    program("func main() int { b: buffer<int>[4] = {1, 2}; b = {9}; return b[0] + b[1] + b[2] + b[3]; }", 9)
    program("func main() int { b: buffer<int>[] = {1, 2}; old := b; b = {3, 4}; return old[0] + b[0]; }", 4)
    program("func main() int { a: int[][] = {{1, 2}, {3}}; return len(a) + len(a[0]) + a[1][0]; }", 7)
    program("func main() int { a := {{1, 2}, {3, 4}}; return len(a[0]) + a[1][1]; }", 6)
    program("func main() int { a, b: int[] = {1, 2}, {3}; a, b = {4}, {5, 6}; return len(a) + len(b) + a[0] + b[1]; }", 13)
    program("func sum(a int[]) int { n := 0; for (i := 0; i < len(a); i++) { n += a[i]; } return n; } func main() int { return sum(int[]{1, 2, 3}); }", 6)
    program("func sum(b buffer<int>[]) int { return b[0] + b[1]; } func main() int { return sum(buffer<int>{4, 5}); }", 9)
    program("func sum(b buffer<int>[3]) int { return b[0] + b[1] + b[2]; } func main() int { return sum(buffer<int>[3]{4, 5}); }", 9)
    program("func size(a int[][]) int { return len(a) + len(a[0]) + len(a[1]); } func main() int { return size(int[][]{{1, 2}, {3}}); }", 5)
    program(COMPARE + 'func main() int { b: buffer<string>[3] = {"hi"}; return strcmp(b[1], "") + strcmp(b[2], ""); }', 0)
    program("func main() int { a := {1->i32, 2->i32}; return a.size + len(a); }", 10)
    program("func item(c buffer<int>[]) int { c[0]++; return c[0]; } func main() int { c: buffer<int>[1]; a := {item(c), item(c)}; return a[0] * 10 + a[1] + c[0]; }", 14)
    program("func pass(a int[]) int[] { return a; } func main() int { a := pass(int[]{7, 8}); return len(a) + a[1]; }", 10)
    program("func first(xs ...int[]) int { return xs[0][1]; } func main() int { return first(int[]{4, 5}); }", 5)
    program("func count(xs ...int) int { return len(xs); } func main() int { return count(int[]{1, 2, 3}...); }", 3)
    program("func main() int { n := 100000; result := 0; for (i := 0; i < 100; i++) { a: int[] = {i, 2, 3}; if (i < 98) { continue; } result = a[0]; break; } return result; }", 98)
    program("func main() int { a := int[3]; a.data = int[]{1}; return a[0] + a[1] + a[2]; }", 1)
    program("func main() int { a := int[3]; a.data = buffer<int>{4, 5}; return a[0] + a[1] + a[2]; }", 9)
    program("func main() int { return len(int[]{1, 2, 3}) + sizeof(int[]{7}); }", 11)
    program("func main() int { b: buffer<int>[2] = {1, 2}; views: buffer<buffer<int>[]>[1] = {b}; views[0][0] = 7; return b[0]; }", 7)

    # Inspect unchecked accesses without executing out-of-range memory operations.
    source.write_text("package main; func main() int { b: buffer<int>[2]; b[-1] = 9; return b[99]; }")
    emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
    assert emitted.returncode == 0, emitted.stdout + emitted.stderr
    # Inspect the source function, excluding unrelated runtime safety checks.
    unchecked = re.search(r'define i64 @"ocean.main"\(\) \{(.*?)^}',
                          ir.read_text(), re.MULTILINE | re.DOTALL).group(1)
    assert "getelementptr [2 x i64]" in unchecked
    assert "inbounds" not in unchecked and "icmp" not in unchecked and "@llvm.trap" not in unchecked


    program('func main() int { s: string; return len(s) + s.size; }', optimize=True)
    program('func main() int { s: string = "abc"; a: i8[] = s; a[0] = 65; return s[0]; }', 97, optimize=True)
    program('func main() int { a: i8[] = {65, 66}; s: string = a; return len(s) + sizeof(s[0]); }', 3, optimize=True)
    program('func main() int { s := "a\\0b" + "\\0c"; return len(s) + s[2] + s[3]; }', 103, optimize=True)
    program('func main() int { s := "ab"; t := s + "cd"; t[0] = 90; if (s[0] == 97 && len(t) == 4 && t[3] == 100) { return 7; } return 1; }', 7, optimize=True)
    program('func main() int { s := "" + ""; s += "ab"; s += "cd"; return len(s) + s[3]; }', 104, optimize=True)
    program(PRINT + 'func main() { s := "hello" + " " + "world"; puts(s); }', stdout="hello world\n", optimize=True)
    program(PRINT + 'func main() { puts("a\\0b" + "c"); }', stdout="a\0bc\n", optimize=True)
    program('func sum(xs ...i8) int { total := 0; for (i := 0; i < len(xs); i++) { total += xs[i]; } return total; } func main() int { return sum("ab"...); }', 195, optimize=True)
    program('func main() int { s := "a"; s.data = {255}; return s[0]->int + 2; }', 1, optimize=True)
    program('func main() int { s := "abcd"; s.data = {65}; return s[0] + s[1] + s[3]; }', 65, optimize=True)
    program('func main() int { a := "ab"; b := "cd"; for (i := 0; i < 100000; i++) { c := a + b; if (len(c) != 4) { return 1; } } return 0; }', optimize=True)

    program(PRINT + 'func main() { s := "é" + `é`; puts(s); }', stdout="éé\n", optimize=True)
    program('func main() int { s := "é"; return len(s) + s[0]->int + 64; }', 5, optimize=True)
    source.write_text("package main; " + 'func main() int { s := "abc"; return len(s); }')
    emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
    assert emitted.returncode == 0, emitted.stdout + emitted.stderr
    literal_ir = ir.read_text()
    assert 'constant [3 x i8] c"\\61\\62\\63"' in literal_ir
    assert 'constant [4 x i8] c"\\61\\62\\63\\00"' not in literal_ir

    # Native struct values and concrete generic layouts, including optimized IR.
    program("struct P { x int; y int; } func main() int { p := P{2, 3}; q := p; q.x = 7; return p.x + q.x + q.y; }", 12, optimize=True)
    program("struct P { x int; y int; } func make(x int) P { return P{x, 3}; } func sum(p P) int { p.x++; return p.x + p.y; } func main() int { p := make(4); return sum(p) + p.x + make(8).y; }", 15, optimize=True)
    program("struct P { x int; y int; } func main() int { p: P = {y: 5}; p.x += 2; p.y++; return p.x + p.y; }", 8, optimize=True)
    program("struct Inner { x int; } struct Outer { a Inner; data buffer<int>[2]; } func fill(b buffer<int>[]) { b[1] = 9; } func main() int { o: Outer = {{3}, {4,5}}; o.a.x++; fill(o.data); o.data[0]++; return o.a.x + o.data[0] + o.data[1]; }", 18, optimize=True)
    program("struct P { x int; } func update(p *P) { p.x++; (*p).x += 2; } func main() int { p := P{3}; ptr := &p; update(ptr); (*ptr).x++; return ptr.x; }", 7, optimize=True)
    program("struct P { x int; } func main() int { p := P{3}; x := &p.x; *x = 9; return p.x; }", 9, optimize=True)
    program("struct Node<T any> { value T; next *Node<T>; } func main() int { b := Node<int>{value: 7}; a := Node<int>{3, &b}; a.next.value++; return a.value + b.value; }", 11, optimize=True)
    program("struct A { next *B; value int; } struct B { next *A; value int; } func main() int { b := B{value: 4}; a := A{&b, 3}; b.next = &a; return a.next.value + b.next.value; }", 7, optimize=True)
    program("struct Box<T any>(int tag) { data T; } func main() int { a := Box<int>{3, 4}; b := Box<i8>{5, 6}; return a.tag + a.data + b.tag + b.data + sizeof(a) + sizeof(b); }", 50, optimize=True)
    program("struct P { x int; } struct Box<T any>(T parameter, int tag) { data T; } func main() int { b: Box<P> = {{2}, 3, {4}}; b.data.x++; return b.parameter.x + b.tag + b.data.x; }", 10, optimize=True)
    program("struct Pair[T any, U any](T first) { second U; } func main() int { p := Pair[int, bool]{4, true}; if (p.second) { return p.first; } return 1; }", 4, optimize=True)
    program("struct Box<T any> { data T; } func main() int { b := Box<Box<int>>{{7}}; b.data.data++; return b.data.data; }", 8, optimize=True)
    program("struct Box<T any> { data buffer<T>[2]; items T[]; } func main() int { b: Box<int> = {{1, 2}, {3, 4}}; b.data[1]++; b.items.data = {5}; return b.data[1] + b.items[0] + b.items[1]; }", 8, optimize=True)
    program("struct S(int first, second) { value int; } func make() S { return S{2, 3, 4}; } func main() int { s := make(); s = {5, 6, 7}; return s.first + s.second + s.value; }", 18, optimize=True)
    program("struct S { name string; items int[]; } func main() int { s: S; t := S{}; return len(s.name) + len(s.items) + len(t.name) + len(t.items); }", 0, optimize=True)
    program("struct S { name string; } func main() int { a := S[2]; a.data = {{\"hi\"}}; return len(a[0].name) + len(a[1].name); }", 2, optimize=True)
    program("struct P { x int; y int; } func main() int { a := P[2]; a.data = {{2,3}, {4,5}}; a[1].x++; b: buffer<P>[2] = {{6,7}}; b[0].y++; return a[1].x + b[0].y + b[1].x; }", 13, optimize=True)
    program("struct P { x int; } func first(xs ...P) int { xs[0].x++; return xs[0].x; } func main() int { p := P{4}; return first(p) + p.x; }", 9, optimize=True)
    program("struct P { x int; } func pair() (P, P) { return P{2}, P{3}; } func main() int { a,b := pair(); a.x,b.x = b.x,a.x; return a.x * 10 + b.x; }", 32, optimize=True)
    program("struct Empty<T any> {} func main() int { e := Empty<int>{}; return sizeof(e); }", 0, optimize=True)
    program("struct S { x int; y int; } func make(n buffer<int>[]) S { n[0]++; return S{n[0], 2}; } func main() int { n: buffer<int>[] = {0}; return make(n).x + n[0]; }", 2, optimize=True)
    program("struct Empty<T any> { data int; } func main() int { a := Empty<int>{4}; b := Empty<bool>{5}; return sizeof(a) + sizeof(b) + a.data + b.data; }", 25, optimize=True)

    source.write_text("package main; struct Box<T any> { value T; } func main() int { a := Box<int>{1}; b := Box<bool>{true}; c := Box<int>{2}; return a.value + c.value; }")
    emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
    assert emitted.returncode == 0, emitted.stdout + emitted.stderr
    layouts = [line for line in ir.read_text().splitlines() if line.startswith("%ocean.struct.") and " = type " in line]
    assert len(layouts) == 2, layouts
    assert sorted(line.split(" = type ")[1] for line in layouts) == ["{ i1 }", "{ i64 }"], layouts

    program("func main() int { struct Local<T any> { data T; size int; } x := Local<int>{3, 4}; x.data++; x.size++; return x.data + x.size; }", 9, optimize=True)
    program("struct S { size int; data int; } func main() int { s := S{3, 4}; s.data = 7; s.size = 2; return s.data + s.size; }", 9, optimize=True)

    program("struct Node { value int; next *Node; } func main() int { b := Node{value: 2}; a := Node{1, &b}; if (a.next == &b && &a != &b) { return a.next.value; } return 0; }", 2, optimize=True)
    program("struct Box<T any> { value T; } func main() int { a := Box<int>[]{{3}, {4}}; a[1].value++; return a.size + len(a) + a[1].value; }", 23, optimize=True)
    # Concrete generic record types and addressed literals inside specializations.
    program("struct Optional<T> { value T; ok bool; } "
            "func (o Optional<T>) unwrap() T { if (!o.ok) panic \"empty optional\"; return o.value; } "
            "struct Node<T> { value T; next *Node<T>; } "
            "func (n *Node<T>) read() T { node: *Node<T> = n; previous: *Node<T> = null; "
            "if (previous == null) return node.value; panic \"unreachable\"; } "
            "func local<T>(value T) T { node: *Node<T> = &Node<T>{value: value, next: null}; "
            "return node.read(); } "
            "func main() int { first := &Node<int>{value: 3, next: null}; "
            "second := &Node<i8>{value: 4, next: null}; "
            "answer := Optional<int>{value: first.read(), ok: true}; "
            "return answer.unwrap() + second.read() + local<int>(5); }", 12, optimize=True)
    # Verify concrete layouts against host C structs, through pointer parameters.
    source.write_text("package main; struct Check<T any> { tag i8; value T; active bool; } extern i32 inspect(*Check<int> a, *Check<i8> b, int sa, int sb); func main() int { a := Check<int>{1, 7, true}; b := Check<i8>{-3, 9, true}; result := extern.inspect(&a, &b, sizeof(a), sizeof(b)); if (a.value != 21) { return 2; } return result; }")
    emitted = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
    assert emitted.returncode == 0, emitted.stdout + emitted.stderr
    helper = root / "struct layout.c"
    helper.write_text("""#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
struct Wide { int8_t tag; int64_t value; bool active; };
struct Narrow { int8_t tag; int8_t value; bool active; };
int inspect(struct Wide *a, struct Narrow *b, int64_t sa, int64_t sb) {
    int ok = sa == sizeof(*a) && sb == sizeof(*b) && offsetof(struct Wide, value) == 8
        && a->tag == 1 && a->value == 7 && a->active
        && b->tag == -3 && b->value == 9 && b->active;
    a->value = 21;
    return ok ? 0 : 1;
}
""")
    linked = run([clang, "-Wno-override-module", "-O2", str(ir), str(helper), "-o", str(exe)])
    assert linked.returncode == 0, linked.stderr
    assert run([str(exe)]).returncode == 0

    def rejected(text, diagnostic):
        source.write_text("package main; " + text)
        exe.write_text("existing output")
        result = run([compiler, "build", str(source), "-o", str(exe)])
        assert result.returncode != 0, text
        assert diagnostic in result.stdout + result.stderr, result
        assert exe.read_text() == "existing output", "failure replaced previous output"
        ir.write_text("existing IR")
        result = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)])
        assert result.returncode != 0, text
        assert ir.read_text() == "existing IR"

    rejected("extern i32 abs(i32 value); func main() int { return abs(-3); }", "extern.name")
    rejected("func main() int { return extern.abs(-3); }", "not declared in this file")
    rejected("func main() { extern i32 abs(i32 value); }", "file scope")
    rejected("extern i32 abs(i32 value); func main() { extern.(); }", "Expected extern function name")
    rejected("func main() { raw: *void; p: *int = raw; }", "type mismatch")
    rejected("func main() { p: **int; raw: **void = p; }", "type mismatch")
    rejected("func main() { a: buffer<*int>[1]; b: buffer<*void>[1] = a; }", "type mismatch")
    rejected("func main() { p: *void; x := *p; }", "before dereferencing")
    rejected("func main() { x := 1.0 -> (*void); }", "invalid conversion")
    rejected("func main() { a := int[]{1}; p := a -> (*void); }", "invalid conversion")
    rejected("func main() { x := zero(void); }", "void has no value representation")
    rejected("func main() int { return sizeof(void); }", "void has no storage size")
    rejected("struct P(int tag) { x int; } func main() { p := P{1,2}; p.tag = 3; }", "immutable")
    rejected("struct P<T any>(T parameter) { value T; } func main() { p := P<int>{1,2}; p.parameter++; }", "immutable")
    rejected("struct P { x int; } struct Box<T any>(T parameter) {} func main() { b := Box<P>{{1}}; b.parameter.x = 3; }", "immutable")
    rejected("struct S { value S; } func main() { s: S; }", "recursive struct")
    rejected("struct A { value B; } struct B { value A; } func main() { a: A; }", "recursive struct")
    rejected("struct S { data buffer<S>[2]; } func main() { s: S; }", "recursive struct")
    rejected("struct S(int n) { data buffer<int>[n]; } func main() { s: S = {2, buffer<int>{1,2}}; }", "runtime-sized inline struct fields")
    rejected("struct S { x int; } extern S make(void); func main() {}", "ABI lowering")
    rejected("struct S { x int; } extern void use(S value); func main() {}", "ABI lowering")
    rejected("struct Grow<T any> { next *Grow<T[]>; } func main() { g: Grow<int>; }", "specialization nesting")
    rejected("struct S { value S; } func main() {}", "recursive struct")
    rejected("func main() { x := zero(); }", "zero requires exactly one type argument")
    rejected("func main() { x := zero(int, bool); }", "closing parenthesis after zero type")
    rejected("func main() { x := zero(Missing); }", "unresolved type")
    rejected("func main() { x := 1; z := zero(x); }", "expected a type")
    rejected("func main() { x := zero(any); }", "zero requires a storage type")
    rejected("func main() { n := 3; x := zero(buffer<int>[n]); }", "zero requires a fixed-size buffer")
    rejected("func main() { zero(int) = 1; }", "writable value")
    rejected("func main() { x: bool = zero(int); }", "type mismatch")
    rejected("func f() {}", "requires func main")
    program("func main() int {}", 0)
    program("func main() int { if (true) { return 1; } }", 1)
    rejected("func main() { break; }", "Branch statement")
    rejected("func main() { continue; }", "Branch statement")
    program("struct Point {} func main() {}")
    rejected("func main() { x: int = \"bad\"; }", "type mismatch")
    rejected("struct Box<T any> { value T; } func (b *Box<T>) set(value T) { b.value = value; } func main() { b := Box<int>{1}; b.set(true); }", "type mismatch")
    rejected("struct Box<T any> {} func (b Box<T>) get() int { return 1; } func main() { b := Box<int>{}; b.get(1); }", "argument count mismatch")
    rejected("struct Box<T any> {} func (b *Box<T>) set() {} func make() Box<int> { return Box<int>{}; } func main() { make().set(); }", "addressable")
    rejected("struct Box<T any> {} func (b Box<T>) get<U any>(value U) U { return value; } func main() { b := Box<int>{}; b.get(1); }", "explicit method type arguments")
    rejected("struct S {} func (s S) take<U>(value U) U { return value; } func main() { s := S{}; s.take<int, bool>(1); }", "type argument count mismatch")
    rejected("struct S {} func (s S) take<U>(value U) U { return value; } func main() { s := S{}; s.take<int>(true); }", "type mismatch")
    rejected("struct S {} func (s *S) take<U>(value U) U { return value; } func make() S { return S{}; } func main() { make().take<int>(1); }", "addressable")
    rejected("struct S {} func (s S) take<U>(value U) U { return value; } func main() { s := S{}; s.take<Missing>(1); }", "unresolved type")
    rejected("func main() { x: int = null; }", "type mismatch")
    rejected("struct S {} func main() { x: S = null; }", "type mismatch")
    rejected("func main() { x := null; }", "explicit pointer type")
    rejected("func main() { x: *int = 0; }", "type mismatch")
    rejected("func id[T any](x T) T { return x; } func main() { id(1); }", "explicit type arguments")
    rejected("func pair() (int, bool) { return 1, true; } func main() { a := pair(); }", "initializer count mismatch")
    rejected("func pair() (int, bool) { return 1, true; } func main() { a, b, c := pair(); }", "initializer count mismatch")
    rejected("func pair() (int, bool) { return 1, true; } func main() { a, b: int = pair(); }", "type mismatch")
    rejected("func f() (int, bool) { return 1, 2; } func main() {}", "type mismatch")
    rejected("func f() (int, bool) { return 1; } func main() {}", "return count mismatch")
    rejected("func f(xs ...int) {} func main() { f(\"bad\"); }", "type mismatch")
    rejected("func f(n int, xs ...int) {} func main() { f(); }", "argument count mismatch")
    rejected("func f(xs ...int) {} func main() { f(2...); }", "expanded argument must be an array")
    rejected("func f(xs ...int) {} func main() { b: buffer<string>[1]; f(b...); }", "buffer expansion is not supported")
    rejected("func f(x int) {} func main() { b: buffer<int>[1]; f(b...); }", "expanded argument requires")
    rejected("func pair() (int, int) { return 1, 2; } func main() { a := 0; b := 0; a, b = 1; }", "assignment count mismatch")
    rejected("func f(xs ...int) { xs.size = 1; } func main() {}", "immutable")
    rejected("func f(xs ...int) { xs.size++; } func main() {}", "immutable")
    rejected("func pair() (int, int) { return 1, 2; } func main() { a, b := pair(), 3; }", "only expression")
    rejected("func f() (int, bool) { return 1, true; } func main() { a := 0; b := 0; a, b = f(); }", "type mismatch")
    rejected("func f(xs ...int) {} func main() { n := 2; b: buffer<int>[n]; f(b...); }", "buffer expansion is not supported")
    rejected("func f(xs ...int) {} func main() { b: buffer<int>[] = {1, 2}; f(b...); }", "buffer expansion is not supported")
    rejected("func main() { b: buffer<int>[]; }", "unsized buffer requires an initializer")
    rejected("func main() { n := 2; b: buffer<int>[n] = {1, 2}; }", "runtime-sized buffers cannot have initializers")
    rejected("func main() { b: buffer<int>[missing]; }", "unresolved name")
    rejected("func main() { n := true; b: buffer<int>[n]; }", "type mismatch")
    rejected("func main() { n := 2; b: buffer<int>[n]; x := b.length; }", "buffer has no members")
    rejected("func main() { b: buffer<int>[] = {1, 2}; c: buffer<int>[2] = b; }", "type mismatch")
    rejected("func main() { n := 2; b: buffer<buffer<int>[n]>[2]; }", "fixed storage size")
    rejected("func main() { a: int[] = float[3]; }", "type mismatch")
    rejected("func main() { a := int[true]; }", "type mismatch")
    rejected("func main() { a := int[-2]; }", "must not be negative")
    rejected("func main() { a := int[missing]; }", "unresolved name")
    rejected("func main() { len(); }", "exactly one array argument")
    rejected("func main() { a := int[2]; len(a, a); }", "exactly one array argument")
    rejected("func main() { len(3); }", "len requires an array")
    rejected("func main() { b: buffer<int>[3]; len(b); }", "len requires an array")
    rejected("func main() { a := int[2]; x := a.length; }", "only has size and data")
    rejected("func main() { a := int[2]; a[0] = true; }", "type mismatch")
    rejected("func main() { a := int[3]; a.size = 16; }", "immutable")
    rejected("func main() { a := int[3]; a.size++; }", "immutable")
    rejected("func main() { a := int[3]; a.size += 8; }", "immutable")
    rejected("func main() { a := int[3]; a.size, a[0] = 8, 2; }", "immutable")
    rejected("func main() { a := int[3]; p := &a.size; }", "immutable")
    rejected("func main() { a := int[2]; a.data = {\"bad\"}; }", "type mismatch")
    rejected("func main() { a := int[2]; a.data = {0: 1}; }", "positional")
    rejected("func main() { a := int[2]; a.data = int[2]; }", "requires a buffer or composite literal")
    rejected("func main() { a := int[2]; b: buffer<int>[2]; a.data += b; }", "copy assignment")
    rejected("func f(xs ...int) {} func main() { a := float[2]; f(a...); }", "type mismatch")
    rejected("func f(n int, xs ...int) {} func main() { a := int[2]; f(a...); }", "expanded argument requires")
    rejected("func f(xs ...int) {} func main() { a := int[2]; f(a.data...); }", "buffer expansion is not supported")
    rejected("func f(xs ...int) int { return xs.length; } func main() {}", "only has size and data")
    rejected("extern i32 printf(string format, ...); func main() { a := int[2]; extern.printf(\"%d\", a.data...); }", "buffer expansion is not supported")
    rejected("func pair(a int[]) (int, int[]) { return 1, a; } func f(n int, xs ...int) {} func main() { a := int[2]; f(pair(a)...); }", "not multiple return values")
    rejected("func use(a int[]) {} func main() { use({1, 2, 3}); }", "standalone literal requires an explicit type")
    rejected("func use(b buffer<int>[]) {} func main() { use({1, 2}); }", "standalone literal requires an explicit type")
    rejected("func use(xs ...int[]) {} func main() { use({1, 2}); }", "standalone literal requires an explicit type")
    rejected("func f() int[] { return {1, 2}; } func main() {}", "standalone literal requires an explicit type")
    rejected("func main() int { return len({1, 2}); }", "standalone literal requires an explicit type")
    rejected("func main() { x := {}; }", "cannot infer an empty literal")
    rejected("func main() { x: int = {1, 2}; }", "literal requires an array, buffer, or struct")
    rejected("func main() { x := int{1, 2}; }", "literal requires an array, buffer, or struct")
    rejected("func main() { a := int[]{1, \"bad\"}; }", "type mismatch")
    rejected("func main() { b := buffer<int>[1]{1, 2}; }", "too many elements")
    rejected("func main() { a := int[]{0: 1}; }", "positional")
    rejected("func main() { a := int[1]; b := a{1, 2}; }", "literal prefix must name")
    source.write_text("package main; extern int missing(void); func main() int { return extern.missing(); }")
    exe.write_text("existing output")
    result = run([compiler, "build", str(source), "-o", str(exe)])
    assert result.returncode != 0 and "clang failed" in result.stderr, result
    assert exe.read_text() == "existing output"

    source.write_text("package main; func main() {}")
    original = source.read_text()
    result = run([compiler, "build", str(source), "-o", str(source)])
    assert result.returncode != 0 and source.read_text() == original
    result = run([compiler, "build", str(source), str(source)])
    assert result.returncode != 0 and "one package location" in result.stderr
    result = run([compiler, "build", "-o"])
    assert result.returncode != 0 and "requires an output path" in result.stderr
    result = run([compiler, "build", "--unknown"])
    assert result.returncode != 0 and "unknown option" in result.stderr
    result = run([compiler, "build", str(source), "-o", str(exe)], env={**os.environ, "PATH": ""})
    assert result.returncode != 0 and "could not execute clang" in result.stderr
    assert exe.read_text() == "existing output"
    result = run([compiler, "build", "--emit-llvm", str(source), "-o", str(ir)], env={**os.environ, "PATH": ""})
    assert result.returncode == 0, result
    result = run([compiler, "build", str(source)], cwd=root)
    assert result.returncode == 0 and (root / root.name).is_file()
    assert run([str(root / root.name)]).returncode == 0
    assert not list(root.glob("*.??????")), "temporary output was not cleaned up"
print("backend end-to-end tests passed")
