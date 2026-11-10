"""Reachable lowering, reflection reachability, and native section GC."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
fixture = Path(__file__).with_name('fixtures').joinpath('reflect.oc').read_text()

with tempfile.TemporaryDirectory(prefix='ocean reachability ') as directory:
    root = Path(directory)
    standard = root / 'standard'
    (standard / 'reflect').mkdir(parents=True)
    (standard / 'reflect' / 'reflect.oc').write_text(fixture)
    project = root / 'project'
    project.mkdir()
    source, ir, native = project / 'main.oc', root / 'main.ll', root / 'program'
    env = dict(os.environ, OCEAN_SRC=str(standard), OCEAN_BUILD_CACHE=str(root / 'cache'))

    def run(args, **kwargs):
        result = subprocess.run(list(map(str, args)), capture_output=True, text=True,
                                timeout=30, env=env, **kwargs)
        assert result.returncode == 0, (args, result.stdout, result.stderr)
        return result

    def emit(body, package='main'):
        source.write_text('package ' + package + '; ' + body)
        run([compiler, 'build', '--emit-llvm', source, '-o', ir])
        return ir.read_text()

    def execute():
        run([compiler, 'build', source, '-o', native])
        return run([native])

    # Global initializers, recursive direct calls, deferred calls, and function
    # adapters are roots/edges. A dead strongly connected component is not.
    text = emit('''extern int missingNative();
        func deadA() int { return deadB() + extern.missingNative(); }
        func deadB() int { return deadA(); }
        func initial() int { return 3; }
        value := initial();
        func recursive(n int) int { if (n == 0) return value; return recursive(n - 1); }
        func cleanup() {}
        func callback() int { return recursive(2); }
        func main() int { defer cleanup(); f: func() int = callback; return f() - 3; }
    ''')
    def has_function(text, name):
        return re.search(r'@"ocean\.(?:__pkg\d+_)?' + re.escape(name) + r'"', text) is not None

    for name in ('initial', 'recursive', 'cleanup', 'callback', 'main'):
        assert has_function(text, name), name
    assert not has_function(text, 'deadA') and not has_function(text, 'deadB')
    execute()

    # Library entry points remain available to downstream native callers.
    text = emit('func Exported() int { return helper(); } func helper() int { return 7; }',
                package='library')
    assert has_function(text, 'Exported') and has_function(text, 'helper')

    text = emit('''import "reflect"; struct Public { value int; }
        func Inspect(value any) array<reflect.Field> { return reflect.Fields(value); }
    ''', package='library')
    assert '__ocean.reflect.fields' in text
    fields_body = text.split('define internal ptr @__ocean.reflect.fields', 1)[1].split('\n}', 1)[0]
    assert 'icmp eq ptr' in fields_body  # Preserve layouts for unknown callers.

    scalar = '''import "reflect"; struct Unused { x int; }
        func dead() { reflect.Fields(Unused{1}); }
        func main() int {
            if (reflect.IsStruct(3)) return 1;
            return len(reflect.Fields("hello"));
        }'''
    normal = emit(scalar)
    assert '__ocean.reflect.fields' in normal
    assert not has_function(normal, 'dead')
    execute()
    # The removed option must not enable a second lowering/omission mode.
    for command in ('build', 'check', 'run'):
        rejected = subprocess.run([compiler, command, '--omit-reflection', str(source)],
                                  capture_output=True, text=True, timeout=30, env=env)
        assert rejected.returncode != 0 and 'unknown option' in rejected.stderr
    run([compiler, 'check', source])
    run([compiler, 'run', source])

    # Scalar reflection remains available alongside private Result payload boxing.
    text = emit('''import "reflect";
        func answer() Result<int> { return Result<int>.value(7); }
        func main() int { if (reflect.IsStruct("text")) return 1; return answer().unwrap() - 7; }
    ''')
    assert '__ocean.reflect.is_struct' in text
    execute()

    # Nested fields and arrays retain reflection when structs may be inspected.
    text = emit('''import "reflect";
        struct Inner { n int; } struct Outer { child Inner; }
        func main() int {
            values: any = Outer[]{Outer{Inner{7}}};
            rows := values.(mixed any[]);
            outer := reflect.Fields(rows[0]);
            inner := reflect.Fields(outer[0].Value);
            return inner[0].Value.(int) - 7;
        }''')
    assert '__ocean.reflect.fields' in text
    execute()

    # Exercise real printing/formatting code, including Result's private boxes.
    (standard / 'io').mkdir()
    (standard / 'io' / 'io.oc').write_text(
        Path(__file__).with_name('fixtures').joinpath('io.oc').read_text())
    text = emit('''import "io";
        func main() { io.Println("hello", 7); }
    ''')
    assert '__ocean.reflect.is_struct' in text
    assert execute().stdout == 'hello7\n'
    text = emit('''import "io";
        struct Inner { n int; } struct Outer { child Inner; }
        func main() { io.Println(Outer{Inner{7}}); }
    ''')
    assert '__ocean.reflect.fields' in text
    assert execute().stdout == '{child: {n: 7}}\n'

    # Function/data sections let the linker remove unreferenced generated code.
    # Inspect actual tool arguments while still invoking the real native tools.
    tools = root / 'tools'
    tools.mkdir()
    log = root / 'args'
    wrapper = tools / 'clang'
    wrapper.write_text('#!' + sys.executable + '\nimport os, sys\n'
                       'with open(' + repr(str(log)) + ', "a") as f: f.write(repr(sys.argv[1:]) + "\\n")\n'
                       'os.execv(' + repr(clang) + ', [' + repr(clang) + ', *sys.argv[1:]])\n')
    wrapper.chmod(0o755)
    env.update(PATH=str(tools) + os.pathsep + env.get('PATH', ''), OCEAN_BUILD_CACHE='off')
    emit('func main() {}')
    execute()
    arguments = log.read_text()
    assert '-ffunction-sections' in arguments and '-fdata-sections' in arguments
    assert ('-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections') in arguments

print('Reachability, reflection, and section garbage collection checks passed')
