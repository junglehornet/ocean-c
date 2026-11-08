"""Exercise type-list operators and generic interface signatures through native LLVM."""
import os
import pathlib
import subprocess
import sys
import tempfile

compiler, clang = (str(pathlib.Path(arg).resolve()) for arg in sys.argv[1:])
source_text = '''package main;
interface Number { int | float }
interface Integer { ~int }
interface Text { string }
interface IntArrays { int[] }
interface IntPointer { *int }
func concatenate(x,y Text) Text { return x+y; }
func combine(x,y IntArrays) IntArrays { return x+y; }
func samePointer(x,y IntPointer) bool { return x==y; }
type ID int;
type Alias = int;
func add<T int | float>(x, y T) T { return x + y; }
func forward<T Number>(x, y T) T { return add<T>(x, y); }
func numeric(x, y Number) Number { return (x + y) * y - x / y; }
func direct(x, y int | float) int | float { return x + y; }
func compare(x, y Number) bool { return x > y; }
func change(x Number) Number { x += 1; x++; return -x; }
func integer<T Integer>(x, y T) T { return x ^ y; }
func exact(value any) int {
    switch (value.(type)) { case Number: return 1; default: return 0; }
}
func backed(value any) int {
    switch (value.(type)) { case Integer: return 1; default: return 0; }
}
interface Arithmetic {
    func add(int) int;
    func subtract(other int) int;
    func multiply<T int | float>(T) float;
}
struct Value { n int; }
func (v Value) add(other int) int { return v.n + other; }
func (v Value) subtract(other int) int { return v.n - other; }
func (v Value) multiply<U float | int>(other U) float {
    return (v.n -> float) * (other -> float);
}
func isArithmetic(value any) bool {
    switch (value.(type)) { case Arithmetic: return true; default: return false; }
}
func apply<T Arithmetic>(value T, small i8) float { return value.multiply<int>(small); }
func inline<T interface { int | float }>(x T) T { return x * x; }
func main() int {
    if (add<int>(2, 3) != 5 || add<float>(1.5, 2.5) != 4.0) return 1;
    if (forward<int>(4, 6) != 10 || inline<float>(2.5) != 6.25) return 2;
    if (numeric(2, 3).(int) != 15 || direct(2, 3.5).(float) != 5.5) return 3;
    small: i8 = 3;
    if (direct(small, small).(int) != 6 || apply<Value>(Value{2}, small) != 6.0) return 4;
    if (!compare(4.0, 3) || change(3).(int) != -5 || change(2.5).(float) != -4.5) return 5;
    x := 7 -> ID; y := 3 -> ID;
    if ((integer<ID>(x, y) -> int) != 4) return 6;
    alias: Alias = 2; narrow: i32 = 2;
    if (exact(alias) != 1 || exact(narrow) != 0 || exact(x) != 0) return 7;
    if (backed(x) != 1 || backed(narrow) != 0) return 8;
    if (concatenate("Ocean", "!").(string) != "Ocean!") return 9;
    joined := combine(int[]{1,2}, int[]{3}).(int[]);
    if (len(joined)!=3 || joined[2]!=3) return 10;
    if (!samePointer(&alias,&alias)) return 11;
    if (!isArithmetic(Value{2})) return 12;
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="ocean type lists ") as directory:
    root = pathlib.Path(directory)
    (root / "main.oc").write_text(source_text)
    executable, ir = root / "program", root / "program.ll"
    env = dict(os.environ, OCEAN_BUILD_CACHE="off")

    def run(*args):
        result = subprocess.run(args, capture_output=True, text=True, timeout=30, env=env)
        assert result.returncode == 0, result.stdout + result.stderr + f"\nexit: {result.returncode}"

    run(compiler, "check", str(root))
    run(compiler, "build", str(root), "-o", str(executable))
    run(str(executable))
    run(compiler, "build", str(root), "--emit-llvm", "-o", str(ir))
    run(clang, "-O2", "-x", "ir", str(ir), "-o", str(executable), "-lffi")
    run(str(executable))

    # Imported lists must retain package identity through generic specialization.
    (root / "ocean.mod").write_text("module example.test/type-lists\n\nocean 0.1\n")
    numbers = root / "numbers"
    numbers.mkdir()
    (numbers / "numbers.oc").write_text("""package numbers;
interface Number { int | float }
interface Arithmetic { func Multiply<T Number>(T) float; }
func Add<T Number>(x,y T) T { return x+y; }
func Sum(x,y Number) Number { return x+y; }
""")
    (root / "main.oc").write_text("""package main;
import "numbers";
struct Value { n int; }
func (v Value) Multiply<U int | float>(x U) float { return (v.n -> float) * (x -> float); }
func apply<T numbers.Arithmetic>(v T) float { return v.Multiply<int>(3); }
func main() int {
    if (numbers.Add<int>(2,3)!=5 || numbers.Sum(2,3.5).(float)!=5.5) return 1;
    if (apply<Value>(Value{2})!=6.0) return 2;
    return 0;
}
""")
    run(compiler, "check", str(root))
    run(compiler, "build", str(root), "-o", str(executable))
    run(str(executable))
print("type-list operators and generic interface methods passed")
