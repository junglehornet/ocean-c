"""Check semantic type names and precise source/caret locations through the CLI."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

compiler = str(Path(sys.argv[1]).resolve())
checks = 0
with tempfile.TemporaryDirectory(prefix="ocean semantic diagnostics ") as directory:
    source = Path(directory) / 'types % "source".oc'
    output = Path(directory) / "output"
    display_source = os.path.relpath(source)

    def diagnostic_case(text, message, marker, focus):
        global checks
        text = "package main;\n" + text
        source.write_bytes(text.encode())
        number, line = next((n, line) for n, line in enumerate(text.splitlines(), 1) if marker in line)
        column = line.index(focus) + 1
        for options in (["--check"], ["-o", str(output)]):
            output.write_text("previous output")
            result = subprocess.run([compiler, "build", *options, str(source)], text=True,
                                    capture_output=True, timeout=10)
            report = result.stderr
            assert result.returncode == 1, result
            assert f"[{display_source}:{number}:{column}]: semantic error: {message}\n" in report, report
            assert "<source>" not in report and ":0:0" not in report, report
            snippet = f" {number} | {line}"
            assert snippet in report, report
            caret = report.split(snippet + "\n", 1)[1].splitlines()[0]
            prefix = f" {number} | "
            assert caret == prefix.replace(str(number), " " * len(str(number))) + "".join(
                "\t" if ch == "\t" else " " for ch in line[:column - 1]) + "^", report
            assert output.read_text() == "previous output"
            checks += 1

    def mismatch(text, expected, actual, marker, focus):
        diagnostic_case(text, f"type mismatch: expected {expected}, got {actual}", marker, focus)

    def count_case(text, kind, expected, actual, marker, focus, minimum=False):
        noun = "args" if kind == "argument" else "return values"
        diagnostic_case(text, f"{kind} count mismatch: Expected "
                        f"{'at least ' if minimum else ''}{expected} {noun}, but got {actual}.",
                        marker, focus)

    count_case("func take(x int) {}\nfunc main() {\n    take();\n}",
               "argument", 1, 0, "take();", "take")
    count_case("func take() {}\nfunc main() {\n    take(1, 2);\n}",
               "argument", 0, 2, "take(1", "take")
    count_case("func take(n int, xs ...int) {}\nfunc main() {\n    take();\n}",
               "argument", 1, 0, "take();", "take", True)
    count_case("struct S {}\nfunc (s S) take(x int) {}\nfunc main() {\n    s := S{};\n    s.take();\n}",
               "argument", 1, 0, "s.take();", "take")
    count_case("func take<T>(x T) {}\nfunc main() {\n    take<int>();\n}",
               "argument", 1, 0, "take<int>();", "take")
    count_case("func take(x int) {}\nfunc main() {\n    callback := take;\n    callback();\n}",
               "argument", 1, 0, "callback();", "callback")
    count_case("func pair() (int, int) { return 1, 2; }\nfunc take(x int) {}\nfunc main() {\n    take(pair());\n}",
               "argument", 1, 2, "take(pair());", "take")
    count_case("func bad() int {\n    return;\n}\nfunc main() {}",
               "return", 1, 0, "return;", "return")
    count_case("func bad() {\n    return 1;\n}\nfunc main() {}",
               "return", 0, 1, "return 1;", "return")
    count_case("func bad() (int, bool) {\n    return 1;\n}\nfunc main() {}",
               "return", 2, 1, "return 1;", "return")
    count_case("func pair() (int, int) { return 1, 2; }\nfunc bad() int {\n    return pair();\n}\nfunc main() {}",
               "return", 1, 2, "return pair();", "return")
    mismatch("func take(x string) {}\nfunc main() {\n    take(42);\n}",
             "string", "int", "take(42)", "42")
    mismatch("func bad() string {\n    return 42;\n}\nfunc main() {}",
             "string", "int", "return 42", "42")
    mismatch("func pair() (int, int) { return 1, 2; }\nfunc bad() (string, int) {\n    return pair();\n}\nfunc main() {}",
             "string", "int", "return pair", "(")
    (Path(directory) / "helper").mkdir()
    (Path(directory) / "helper" / "helper.oc").write_text("package helper;\nfunc Take(value int) {}\n")
    (Path(directory) / "ocean.mod").write_text("module diagnostics\nocean 0.1\n")
    count_case('import "helper";\nfunc main() {\n    helper.Take();\n}',
               "argument", 1, 0, "helper.Take();", ".")

    mismatch("struct Node<T any> { value T; next *Node; }\n"
             "func (n *Node<T>) length() int {\n"
             "\tif (n.value == null && n.next == null) return 0;\n"
             "\treturn 1;\n}\n", "T", "null", "n.value ==", "==")
    mismatch("struct Stack<T> { underlying T[]; top int; }\n"
             "func newStack<T>() Stack<T> {\n"
             "\ts: Stack<T> = {0->any, 0};\n\treturn s;\n}\n"
             "func main() {}", "any[]", "any", "s: Stack<T>", "->any")
    mismatch("func main() {\n\tx := 1;\n\tx = false;\n}", "int", "bool", "x =", "=")
    mismatch("func main() {\n\tx: int = false;\n}", "int", "bool", "x:", "x")
    mismatch("func main() {\n\tx := 1 + true;\n}", "int", "bool", "x :=", "+")
    mismatch("func main() {\n\tx := !42;\n}", "bool", "int", "x :=", "!")
    mismatch("func main() {\n\tif (1) {}\n}", "bool", "int", "if (", "1")
    mismatch("func main() {\n\tfor (1) {}\n}", "bool", "int", "for (", "1")
    mismatch("func main() {\n\tx := true ? 1 : false;\n}", "int", "bool", "x :=", "?")
    mismatch('func bad() int {\n\treturn "bad";\n}\nfunc main() {}', "int", "string", "return", '"bad"')
    mismatch("func takes(x bool) {}\nfunc main() {\n\ttakes(\n\t\t42\n\t);\n}", "bool", "int", "42", "42")
    mismatch("func main() {\n\ta := {1, 2};\n\tx := a[1.5];\n}", "int", "float", "a[", "1.5")
    mismatch("struct Point { value bool; }\nfunc main() {\n\tp := Point{value: 7};\n}", "bool", "int", "p :=", "7")
    mismatch("func main() {\n\tx := 1;\n\tp: *bool = &x;\n}", "*bool", "*int", "p:", "p")
    mismatch("func main() {\n\tb := {true};\n\ta: int[] = b;\n}", "int[]", "bool[1]", "a:", "a")
    mismatch("func main() {\n\ta: buffer<int>[2];\n\tb: buffer<int>[3];\n\ta = b;\n}", "buffer<int>[2]", "buffer<int>[3]", "a =", "=")
    mismatch("struct Box<T any> { value T; }\nfunc main() {\n\tb := Box<bool>{true};\n\ta: Box<int> = b;\n}", "__pkg0_Box<int>", "__pkg0_Box<bool>", "a:", "a")
    mismatch("func f(x int) int { return x; }\nfunc g(x bool) bool { return x; }\n"
             "func main() {\n\tcallback := f;\n\tcallback = g;\n}", "func(int) int", "func(bool) bool", "callback =", "=")
    mismatch("func main() {\n\ta := 1; b := false;\n\ta, b = 1, 2;\n}", "bool", "int", "a, b =", "b")
    mismatch("func main() {\n\tx: i32 = false;\n}", "i32", "bool", "x:", "x")
    mismatch("func main() {\n\tx: i32 = 1;\n\tflag: bool = x;\n}", "bool", "i32", "flag:", "flag")
    mismatch("func main() {\n\tx: f32 = true;\n}", "f32", "bool", "x:", "x")
    mismatch("func main() {\n\tx: *i32;\n\ty: *bool = x;\n}", "*bool", "*i32", "y:", "y")
    mismatch("func main() {\n\ta := {1};\n\ti := true;\n\tx := a[i];\n}", "int", "bool", "a[i]", "i]")
    mismatch("func main() {\r\n\tvalue := 1;\r\n\tvalue = false;\r\n}\r\n", "int", "bool", "value =", "=")

print(f"{checks} semantic diagnostic checks passed")
