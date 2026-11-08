"""Directory packages, modules, native build/run, and cross-file diagnostics."""
from pathlib import Path
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

compiler = str(Path(sys.argv[1]).resolve())


def run(args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, timeout=30, **kwargs)


with tempfile.TemporaryDirectory(prefix="ocean packages ") as temporary:
    base = Path(temporary)
    root, standard, scratch = base / "project", base / "standard", base / "scratch"
    for p in (root, standard, scratch): p.mkdir()
    env = dict(os.environ, OCEAN_SRC=str(standard), OCEAN_MOD_CACHE=str(base / "cache"), TMPDIR=str(scratch))
    checks = 0

    def write(path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def invoke(*args, status=0, contains=None, **kwargs):
        global checks
        result = run([compiler, *map(str, args)], env=env, cwd=root, **kwargs)
        assert result.returncode == status, (args, result)
        if contains is not None: assert contains in result.stdout + result.stderr, result
        assert not list(scratch.iterdir()), list(scratch.iterdir())
        checks += 1
        return result

    invoke('mod', 'init', contains='created ocean.mod')
    manifest = root / 'ocean.mod'
    assert manifest.read_text() == 'module project\n\nocean 0.1\n'
    invoke('mod', 'init', 'other', status=1, contains='already exists')
    assert manifest.read_text() == 'module project\n\nocean 0.1\n'
    manifest.unlink()
    invoke('mod', 'init', 'github.com/example/project')
    assert manifest.read_text() == 'module github.com/example/project\n\nocean 0.1\n'
    manifest.unlink()
    for name in ('', '../bad', 'bad name', 'bad\nname', '/absolute', 'bad//name'):
        invoke('mod', 'init', name, status=1, contains='invalid module name')
        assert not manifest.exists()
    invoke('mod', 'init', 'one', 'two', status=1, contains='Usage:')
    invoke('mod', 'init', '--help', contains='Usage: ocean mod init')
    assert not manifest.exists()

    write(root / "ocean.mod", 'module example.com/project\nocean 0.1\n')
    write(root / "main.oc", 'package main; import "example.com/project/math"; '
          'func main() int { return helper() + math.Answer(); }')
    write(root / "helper.oc", 'package main; func helper() int { return 3; }')
    write(root / "math/value.oc", 'package math; func Answer() int { return other(); } '
          'func main() { panic "imported main must not run"; }')
    write(root / "math/other.oc", 'package math; func other() int { return 4; }')
    invoke("run", ".", status=7)
    invoke("run", root / "main.oc", status=7)
    invoke("build", ".", "-o", base / "program")
    assert run([str(base / "program")]).returncode == 7
    invoke("build", "--check", ".")
    invoke("build", "--emit-llvm", ".", "-o", base / "program.ll")
    invoke("build", "math", "-o", base / "math.o")
    assert (base / "math.o").read_bytes().startswith(b'\x7fELF')
    invoke("run", "math", status=1, contains="not main")
    invoke("build")
    assert (root / "project").exists()

    # Resolving an ambiguous array index frees rewritten type tokens. Package
    # cleanup must not free those tokens again, on success or semantic failure.
    ownership = base / "index-ownership"
    write(ownership / "main.oc", 'package main; func main() int { '
          'items := int[2]; i := 0; items[i] = 7; return items[i]; }')
    invoke("run", ownership, status=7)
    write(ownership / "bad.oc", 'package main; '
          'extern i32 printf(buffer<i8>, ...); '
          'func forward(values ...int) { extern.printf(values...); }')
    invoke("build", ownership, status=1,
           contains="expanded argument requires an Ocean variadic function")

    # Imports can use module-relative paths, package declarations as qualifiers,
    # explicit aliases, or dot imports. Names stay isolated across packages.
    write(root / "main.oc", 'package main; import "math" as m; import . "tools"; '
          'func main() int { return m.Answer() + Answer(); }')
    write(root / "tools/tools.oc", 'package tools; func Answer() int { return 5; }')
    invoke("run", ".", status=9)
    write(root / "main.oc", 'package main; import "math"; func main() int { '
          'math := 7; return math; }')
    invoke("run", ".", status=7)

    # Qualified generic types, methods, and cross-file helpers.
    write(root / "math/value.oc", 'package math; struct Box<T> { value T; } '
          'func Make<T>(v T) Box<T> { return Box<T>{v}; } '
          'func (b Box<T>) Get() T { return b.value; }')
    write(root / "main.oc", 'package main; import "math"; func main() int { '
          'b: math.Box<int> = math.Make<int>(7); return b.Get(); }')
    invoke("run", ".", status=7)

    write(root / "main.oc", 'package main; import "math"; '
          'func main() int { b := zero(math.Box<int>); '
          'return sizeof(math.Box<int>) + sizeof(*math.Box<int>) + b.value; }')
    invoke("run", ".", status=16)

    # Receivers must belong to this package, including through alias chains.
    receivers = base / "receivers"
    write(receivers / "ocean.mod", 'module example.com/receivers\nocean 0.1\n')
    write(receivers / "lib/types.oc", 'package lib; struct Box<T> { value T; } '
          'type Count int; type Alias = Count;')
    for owner in ('Box<T>', '*Box<T>', 'Box<T>*', 'Count', 'Alias', 'Local'):
        write(receivers / "main.oc", 'package main; import . "lib"; '
              'type Local = Alias; func (value ' + owner + ') Extra() {} func main() {}')
        invoke('build', '--check', receivers, status=1,
               contains='cannot define a method on a type from another package')
    write(receivers / "main.oc", 'package main; import "lib"; '
          'type Local = lib.Count; func (value Local) Extra() {} func main() {}')
    invoke('build', '--check', receivers, status=1,
           contains='cannot define a method on a type from another package')
    # A distinct local definition and receivers in sibling files remain legal.
    write(receivers / "main.oc", 'package main; import "lib"; type Local lib.Count; '
          'func main() int { value := 7->Local; return value.Extra(); }')
    write(receivers / "methods.oc", 'package main; func (value Local) Extra() int { return value->int; }')
    invoke('run', receivers, status=7)

    # Global scope and package initialization are shared across files.
    write(root / "math/value.oc", 'package math; Counter: int = 4; '
          'func Answer() int { return Counter; } func Bump() { Counter++; }')
    write(root / "helper.oc", 'package main; total: int = mathAlias(); '
          'func helper() int { return total; }')
    write(root / "main.oc", 'package main; import "math"; '
          'func mathAlias() int { return math.Answer(); } '
          'func main() int { math.Bump(); return helper() + math.Counter; }')
    invoke("run", ".", status=9)
    write(root / "helper.oc", 'package main; items: int[] = {1, 2}; '
          'func helper() { items = {7, 8, 9}; }')
    write(root / "main.oc", 'package main; func main() int { helper(); return items[0] + len(items); }')
    invoke("run", ".", status=10)

    write(root / "helper.oc", 'package main; items: int[] = {}; '
          'func helper() { for (i := 0; i < 100; i++) { a := int[100000]; a.data = {7}; items = a; } }')
    write(root / "main.oc", 'package main; func main() int { helper(); return items[0]; }')
    invoke("run", ".", status=7)

    write(root / "helper.oc", 'package main; nested: int[][] = {{1, 2}, {3}}; '
          'items: int[] = make(); '
          'func make() int[] { return int[]{4, 5}; } '
          'func helper() { nested[0] = {7, 8, 9}; items = make(); }')
    write(root / "main.oc", 'package main; func main() int { helper(); return nested[0][0] + items[0]; }')
    invoke("run", ".", status=11)

    write(root / "helper.oc", 'package main; first := second; second := third; third := 7; '
          '_: int = initialize(); counter: int; func initialize() int { counter++; return 0; }')
    write(root / "main.oc", 'package main; func main() int { return first + second + third + counter; }')
    invoke("run", ".", status=8)

    write(root / "helper.oc", 'package main; raw: buffer<int>[] = {3, 4}; '
          'nested: buffer<int[]>[] = {{5, 6}}; '
          'func helper() { raw[0]++; nested[0][0]++; }')
    write(root / "main.oc", 'package main; func main() int { helper(); return raw[0] + nested[0][0]; }')
    invoke("run", ".", status=10)

    write(root / "helper.oc", 'package main; struct S { text string; } '
          'p: *S = &S{text: "safe"}; text: i8[] = "hello\\0"; '
          'func helper() { a := int[10000]; a.data = {8}; }')
    write(root / "main.oc", 'package main; func main() int { helper(); '
          'if (p.text != "safe" || text[0] != 104 || text[5] != 0) return 1; return 0; }')
    invoke("run", ".")

    # Each file resolves its own imports; equivalent extern declarations link once.
    write(root / "helper.oc", 'package main; import "math"; '
          'extern *void malloc(int n); extern void free(*void p); '
          'func helper() int { p := extern.malloc(8); extern.free(p); return math.Answer(); }')
    write(root / "main.oc", 'package main; extern *void malloc(int n); extern void free(*void p); '
          'func main() int { p := extern.malloc(8); extern.free(p); return helper(); }')
    invoke("run", ".", status=4)

    write(root / "math/value.oc", 'package math; extern *void malloc(int n); extern void free(*void p); '
          'func Answer() int { p := extern.malloc(8); extern.free(p); return 4; }')
    invoke("run", ".", status=4)

    # Extern declarations are private to the declaring file and namespace.
    scope = base / 'extern-scope'
    write(scope / 'ocean.mod', 'module example.com/extern-scope\n')
    write(scope / 'helper.oc', 'package main; extern i32 abs(i32 value); '
          'func helper() int { return extern.abs(-7); }')
    write(scope / 'main.oc', 'package main; func main() int { return extern.abs(-3); }')
    invoke('build', scope, status=1, contains='not declared in this file')
    write(scope / 'main.oc', 'package main; extern i32 abs(i32 value); '
          'func main() int { return extern.abs(-3) + helper(); }')
    invoke('run', scope, status=10)
    write(scope / 'main.oc', 'package main; extern i32 abs(i32 value); '
          'func abs(value int) int { return value + 1; } '
          'func main() int { return abs(3) + extern.abs(-3) + helper(); }')
    invoke('run', scope, status=14)
    write(scope / 'main.oc', 'package main; extern i32 abs(i32 value); '
          'func main() int { abs := 9; return abs + extern.abs(-3); }')
    invoke('run', scope, status=12)
    write(scope / 'main.oc', 'package main; extern i32 abs(i32 value); '
          'func main() int { return abs(-3); }')
    invoke('build', scope, status=1, contains='extern.name')
    write(scope / 'library/library.oc', 'package library; extern i32 abs(i32 value); '
          'func Absolute(value int) int { return extern.abs(value -> i32); }')
    write(scope / 'main.oc', 'package main; import "library"; '
          'func main() int { return library.abs(-3); }')
    invoke('build', scope, status=1, contains='has no declaration abs')
    write(scope / 'main.oc', 'package main; import "library"; '
          'func main() int { return library.Absolute(-3); }')
    invoke('run', scope, status=3)

    # Standard library search path (override confines the fixture to this tempdir).
    write(standard / "greeting/greet.oc", 'package greeting; func Value() int { return 6; }')
    write(root / "main.oc", 'package main; import "greeting"; func main() int { return greeting.Value(); }')
    invoke("run", ".", status=6)

    # Installed layouts resolve relative to the actual executable, independent
    # of the working directory, PATH invocation, or a symlink to the compiler.
    installed = base / "installation"
    binary = installed / "bin/ocean"
    binary.parent.mkdir(parents=True)
    shutil.copy2(compiler, binary)
    write(installed / "src/greeting/greet.oc",
          'package greeting; func Value() int { return 8; }')
    default_env = dict(env)
    default_env.pop("OCEAN_SRC", None)
    link = base / "links/ocean"
    link.parent.mkdir()
    link.symlink_to(binary)
    default_env["PATH"] = str(binary.parent) + os.pathsep + os.environ.get("PATH", "")
    for command in (str(binary), str(link), "ocean"):
        result = run([command, "run", "."], env=default_env, cwd=root)
        assert result.returncode == 8, result
        assert not list(scratch.iterdir()), list(scratch.iterdir())
        checks += 1
    result = run([str(binary), "run", "."], env=env, cwd=root)
    assert result.returncode == 6, result
    checks += 1

    # Replaced external module, nested package, and dependency module's imports.
    external = base / "external"
    write(external / "ocean.mod", 'module github.com/example/dependency\n')
    write(external / "sub/deeper/value.oc", 'package deeper; import "github.com/example/dependency/util"; '
          'func Answer() int { return util.Value(); }')
    write(external / "util/value.oc", 'package util; func Value() int { return 11; }')
    write(root / "ocean.mod", 'module example.com/project\nrequire (\n'
          'github.com/example/dependency v1.2.3\n)\nreplace (\n'
          'github.com/example/dependency => ../external\n)\n')
    write(root / "main.oc", 'package main; import "github.com/example/dependency/sub/deeper"; '
          'func main() int { return deeper.Answer(); }')
    invoke("run", ".", status=11)

    # Exercise the real Git download path without accessing the network.
    def git(*args):
        result = run(['git', *map(str, args)])
        assert result.returncode == 0, result
    git('init', '-q', external)
    git('-C', external, 'add', '.')
    git('-C', external, '-c', 'user.name=Ocean Test', '-c', 'user.email=test@example.com', 'commit', '-qm', 'module fixture')
    git('-C', external, 'tag', 'v1.2.3')
    configuration = base / 'gitconfig'
    configuration.write_text(f'[url "{external.as_uri()}"]\n\tinsteadOf = https://github.com/example/dependency\n')
    env['GIT_CONFIG_GLOBAL'] = str(configuration)
    env['GIT_CONFIG_NOSYSTEM'] = '1'
    write(root / 'ocean.mod', 'module example.com/project\nrequire github.com/example/dependency v1.2.3\n')
    invoke('run', '.', status=11, contains='downloading')
    assert (base / 'cache/github.com/example/dependency@v1.2.3/ocean.mod').exists()
    result = invoke('run', '.', status=11)
    assert 'downloading' not in result.stderr

    # Runtime reports use the correct file and embedded caller snippet.
    write(root / 'helper.oc', 'package main;\nfunc helper() int {\n return panic "caller error";\n}\n')
    write(root / 'main.oc', 'package main;\nfunc main() int {\n return helper();\n}\n')
    result = invoke('run', '.', status=1, contains="main.oc:3:")
    assert '3 |  return helper();' in result.stderr and 'at helper (' in result.stderr, result
    write(root / 'helper.oc', 'package main;\nfunc helper() int {\n a := int[0];\n return a[1];\n}\n')
    invoke('run', '.', status=1, contains="helper.oc:4:")
    write(root / 'helper.oc', 'package main;\nfunc helper() int {\n x: bool = 42;\n return 0;\n}\n')
    result = invoke('build', '.', status=1, contains="helper.oc:3:")
    assert '3 |  x: bool = 42;' in result.stderr, result

    # Diagnostics use compilation-cwd-relative paths, even for absolute inputs.
    assert str(root) not in result.stderr, result
    sub = root / 'sub'
    write(sub / 'main.oc', 'package main; func main() int { return panic "sub failure"; }')
    result = invoke('run', sub, status=1, contains='sub/main.oc:1:')
    assert str(sub) not in result.stderr, result
    external_error = base / 'outside'
    write(external_error / 'main.oc', 'package main; func main() { panic "outside"; }')
    result = invoke('run', external_error, status=1, contains='../outside/main.oc:1:')
    assert str(external_error) not in result.stderr, result
    write(sub / 'main.oc', 'package main; func main() { x := ; }')
    result = invoke('build', sub, status=1, contains='sub/main.oc:1:')
    assert str(sub) not in result.stdout + result.stderr, result

    # Label names are function/file local and never cross into another file.
    write(root / 'helper.oc', 'package main; func helper() int { done: return 7; }')
    write(root / 'main.oc', 'package main; func main() int { done: return helper(); }')
    invoke('run', '.', status=7)
    write(root / 'main.oc', 'package main; func main() int { goto foreign; }')
    write(root / 'helper.oc', 'package main; func helper() int { foreign: return 7; }')
    invoke('build', '.', status=1, contains='undefined label')

    write(root / 'main.oc', 'package "main"; func main() {}')
    invoke('build', '.', status=1, contains='Expected unquoted package name')

    # Duplicate globals, mixed packages, missing members, cycles, and absent files.
    write(root / 'main.oc', 'package main; func main() int { return 0; }')
    write(root / 'helper.oc', 'package main; func main() {}')
    invoke('build', '.', status=1, contains='duplicate package declaration')
    write(root / 'helper.oc', 'package other; func helper() {}')
    invoke('build', '.', status=1, contains='conflicts with package')
    write(root / 'helper.oc', 'func helper() {}')
    invoke('build', '.', status=1, contains='expected package declaration')
    write(root / 'helper.oc', 'package main; func helper() {}')
    write(root / 'main.oc', 'package main; import "math"; func main() { math.missing(); }')
    invoke('build', '.', status=1, contains='has no declaration missing')
    write(root / 'math/value.oc', 'package math; import "tools"; func Answer() { tools.Answer(); }')
    write(root / 'tools/tools.oc', 'package tools; import "math"; func Answer() { math.Answer(); }')
    invoke('build', '.', status=1, contains='import cycle')
    invoke('build', base / 'missing.oc', status=1, contains='does not exist')
    write(root / 'main.oc', 'package main; func main() {}')
    invoke('build', '.', '-o', root / 'helper.oc', status=1, contains='overwrite')
    invoke('run', '-o', base / 'bad', '.', status=1, contains='incompatible')
    invoke('run', '.', '-o', base / 'bad', status=1, contains='incompatible')
    assert not (base / 'bad').exists()

    # stdin/stdout and signal status are preserved; hidden executables are removed.
    write(root / 'main.oc', 'package main; extern i32 getchar(); '
          'func main() int { return extern.getchar(); }')
    invoke('run', '.', status=65, input='A')
    # A runtime trap must be visible even when the program prints nothing.
    write(root / 'main.oc', 'package main; struct Stack<T> { underlying T[]; top int; } '
          'func (s *Stack<T>) grow() { s.underlying = T[1]; } '
          'func main() { s: Stack<int> = {}; s.grow(); p: *int = null; *p = 1; }')
    result = invoke('run', '.', status=128 + signal.SIGILL,
                    contains=f'program terminated by signal {signal.SIGILL}')
    assert result.stdout == '', result
    assert signal.strsignal(signal.SIGILL) in result.stderr, result
    # Diagnostics from the child continue to pass through stderr.
    write(root / 'main.oc', 'package main; func main() { panic "visible panic"; }')
    invoke('run', '.', status=1, contains='Panic: visible panic')
    write(root / 'main.oc', 'package main; func main() { for (;;) {} }')
    child = subprocess.Popen([compiler, 'run', '.'], cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and not list(scratch.glob('.ocean-run-*/program')):
        assert child.poll() is None
        time.sleep(0.01)
    child.send_signal(signal.SIGTERM)
    child.communicate(timeout=10)
    assert child.returncode == 128 + signal.SIGTERM, child.returncode
    assert not list(scratch.iterdir()), list(scratch.iterdir())

    # Exported names start with an ASCII capital; private names remain shared
    # across files in their own package and do not pollute dot imports.
    visibility = base / 'exports'
    write(visibility / 'ocean.mod', 'module example.com/exports\n')
    write(visibility / 'lib/types.oc', 'package lib; struct Public<T> { value T; } '
          'struct hidden { value int; } privateGlobal: int = 3; Global: int = 4; '
          'func secret() int { return privateGlobal; } '
          'func (p Public<T>) secret() T { return p.value; }')
    write(visibility / 'lib/api.oc', 'package lib; func shared() int { return 2; } '
          'func Value() int { return secret() + shared(); } '
          'func Make<T>(value T) Public<T> { return Public<T>{value}; } '
          'func (p Public<T>) Get() T { return p.secret(); } '
          'func Hidden() hidden { return hidden{6}; }')
    write(visibility / 'main.oc', 'package main; import "lib"; '
          'func main() int { p := lib.Make<int>(5); lib.Global++; '
          'return p.Get() + lib.Value() + lib.Global; }')
    invoke('run', visibility, status=15)
    write(visibility / 'main.oc', 'package main; import . "lib"; '
          'func secret() int { return 2; } '
          'func main() int { p := Make<int>(4); return p.Get() + Value() + secret() + Global; }')
    invoke('run', visibility, status=15)
    # A local binding can shadow an import without losing access to its fields.
    write(visibility / 'main.oc', 'package main; import "lib"; '
          'func main() int { lib := lib.Make<int>(7); return lib.Get(); }')
    invoke('run', visibility, status=7)

    for declaration in (
            'func main() int { return l.secret(); }',
            'func main() int { return l.privateGlobal; }',
            'func main() { l.privateGlobal = 1; }',
            'func main() { value := l.hidden{1}; }',
            'func main() { value: l.hidden; }',
            'type Alias = l.hidden; func main() {}',
            'func f<T l.hidden>(value T) {} func main() {}',
            'func main() { value := zero(l.hidden); }',
            'func main() { value := sizeof(l.hidden); }',
            'func main() { value := l.Make<l.hidden>(l.Hidden()); }',
            'func main() { value := l.Hidden(); switch (value.(type)) { case l.hidden: break; } }'):
        write(visibility / 'main.oc', 'package main;\nimport "lib" as l;\n' + declaration)
        result = invoke('build', '--check', visibility, status=1, contains='not exported')
        assert 'main.oc:3:' in result.stderr and ' 3 | ' + declaration in result.stderr, result

    write(visibility / 'main.oc', 'package main; import "lib"; '
          'func main() int { p := lib.Make<int>(5); return p.secret(); }')
    invoke('build', '--check', visibility, status=1, contains='method is not exported')
    for reference in ('secret()', 'privateGlobal', 'hidden{1}'):
        write(visibility / 'main.oc', 'package main; import . "lib"; '
              'func main() { value := ' + reference + '; }')
        invoke('build', '--check', visibility, status=1,
               contains='literal prefix' if reference.startswith('hidden') else 'unresolved')

    write(visibility / 'other/other.oc', 'package other; '
          'func secret() int { return 9; } func Extra() int { return 2; }')
    write(visibility / 'main.oc', 'package main; import . "lib"; import . "other"; '
          'func main() int { return Value() + Extra(); }')
    invoke('run', visibility, status=7)
    # Two private helpers are invisible, rather than an ambiguous dot import.
    write(visibility / 'main.oc', 'package main; import . "lib"; import . "other"; '
          'func main() int { return secret(); }')
    result = invoke('build', '--check', visibility, status=1, contains='unresolved')
    assert 'ambiguous dot import' not in result.stderr, result
    write(visibility / 'other/other.oc', 'package other; func Value() int { return 9; }')
    write(visibility / 'main.oc', 'package main; import . "lib"; import . "other"; '
          'func main() int { return Value(); }')
    invoke('build', '--check', visibility, status=1, contains='ambiguous dot import')

    # Type aliases, interfaces, and enum constants follow the same name rule.
    write(visibility / 'lib/constraints.oc', 'package lib; '
          'type Alias = int; type privateAlias = int; '
          'interface Constraint { ~int; } interface privateConstraint { ~int; } '
          'enum Choice { First, second }')
    write(visibility / 'main.oc', 'package main; import "lib"; '
          'func accept<T lib.Constraint>(value T) {} '
          'func main() { value: lib.Alias = 1; choice := lib.Choice.First; accept<int>(value); }')
    # Enum variants are namespaced under the exported enum type.
    invoke('build', '--check', visibility)
    for type_name in ('privateAlias', 'privateConstraint'):
        write(visibility / 'main.oc', 'package main; import "lib"; '
              'func use(value lib.' + type_name + ') {} func main() {}')
        invoke('build', '--check', visibility, status=1, contains='not exported')
    write(visibility / 'main.oc', 'package main; import "lib"; func main() { value := lib.Choice.second; }')
    invoke('build', '--check', visibility, status=1, contains='not exported')

print(f'{checks} package/module build and run checks passed, including Git cache and cleanup')
