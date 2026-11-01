"""Standard-library reflection, value snapshots, and privileged-source isolation."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
fixture = Path(__file__).with_name('fixtures').joinpath('reflect.oc').read_text()
checks = 0


def run(args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, timeout=60, **kwargs)


with tempfile.TemporaryDirectory(prefix='ocean reflection ') as directory:
    root = Path(directory)
    standard, project = root / 'standard', root / 'project'
    reflect = standard / 'reflect'
    reflect.mkdir(parents=True); project.mkdir()
    (reflect / 'reflect.oc').write_text(fixture)
    env = dict(os.environ, OCEAN_SRC=str(standard))
    source, ir, native = project / 'main.oc', root / 'main.ll', root / 'main'
    tracker = root / 'tracker.c'
    tracker.write_text(r'''
#include <stdint.h>
#include <stdlib.h>
static void *allocations[65536];
static int64_t live;
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void __real_free(void *);
static void *remember(void *p) {
    if (!p) return p;
    for (size_t i = 0; i < 65536; ++i) if (!allocations[i]) {
        allocations[i] = p; ++live; return p;
    }
    abort();
}
void *__wrap_malloc(size_t n) { return remember(__real_malloc(n)); }
void *__wrap_calloc(size_t n, size_t s) { return remember(__real_calloc(n, s)); }
void __wrap_free(void *p) {
    if (!p) return;
    for (size_t i = 0; i < 65536; ++i) if (allocations[i] == p) {
        allocations[i] = NULL; --live; __real_free(p); return;
    }
    abort();
}
int64_t liveAllocations(void) { return live; }
''')

    def program(body, imports='import "reflect";', memory=False, expected=''):
        global checks
        source.write_text('package main; ' + imports + (' extern int liveAllocations(); ' if memory else '') + body)
        emitted = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)], env=env)
        assert emitted.returncode == 0, emitted.stderr
        for opt in ('-O0', '-O2'):
            args = [clang, opt, '-Wno-override-module', str(ir), '-lffi', '-o', str(native)]
            if memory:
                args += [str(tracker), '-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=free']
            built = run(args)
            assert built.returncode == 0, built.stderr
            result = run([str(native)])
            assert (result.returncode, result.stdout, result.stderr) == (0, expected, ''), (body, result)
            checks += 1
        return ir.read_text()

    def rejected(body, message='restricted to the standard library reflect package', location=None):
        global checks
        source.write_text(body)
        result = run([compiler, 'build', '--check', str(location or source)], env=env)
        assert result.returncode != 0 and message in result.stderr, result
        checks += 1

    program('''struct Person { name string; age int; active bool; }
    struct Empty {}
    struct Tiny { small i8; }
    struct Box<T> { value T; }
    type Named Person;
    func main() int {
        person := Person{"Ada", 36, true}; fields := reflect.Fields(person);
        if (!reflect.IsStruct(person) || reflect.IsStruct(4) || len(fields) != 3) return 1;
        if (fields[0].Name != "name" || fields[0].Value.(string) != "Ada") return 2;
        if (fields[1].Name != "age" || fields[1].Value.(int) != 36 || !fields[2].Value.(bool)) return 3;
        person.name = "changed"; if (fields[0].Value.(string) != "Ada") return 4;
        named := person->Named;
        if (!reflect.IsStruct(named) || len(reflect.Fields(named)) != 3) return 5;
        empty := Empty{};
        if (!reflect.IsStruct(empty) || len(reflect.Fields(empty)) != 0) return 6;
        if (reflect.Fields(Tiny{7})[0].Value.(i8) != 7) return 7;
        if (reflect.Fields(Box<string>{"generic"})[0].Value.(string) != "generic") return 8;
        return 0;
    }''')

    program('''struct Child { value int; }
    struct Value { title string; numbers int[]; child Child; boxed any; fixed int[2]; pointer *int; }
    struct Sized(count int) { data string[count]; }
    func snapshot() array<reflect.Field> {
        source := Value{"original", {1, 2}, Child{3}, "boxed", {4, 5}, null};
        return reflect.Fields(source);
    }
    func exercise() int {
        p := new(int); *p = 7;
        for (i := 0; i < 30; i++) {
            fields := snapshot();
            if (len(fields) != 6 || fields[0].Value.(string) != "original") return 1;
            numbers := fields[1].Value.(int[]); numbers[0] = 9;
            unchanged := fields[1].Value.(int[]); if (unchanged[0] != 1) return 2;
            child := reflect.Fields(fields[2].Value); if (child[0].Value.(int) != 3) return 3;
            if (fields[3].Value.(string) != "boxed") return 4;
            fixed := fields[4].Value.(int[2]); if (fixed[1] != 5) return 5;
            other := fields; fields[0].Value = "changed";
            if (other[0].Value.(string) != "original") return 6;
            sized := Sized{2, {"one", "two"}}; parameters := reflect.Fields(sized);
            if (parameters[0].Name != "count" || parameters[0].Value.(int) != 2) return 7;
            data := parameters[1].Value.(array<string>); if (data[1] != "two") return 8;
            source := Value{"pointer", {}, Child{0}, 1, {}, p}; copies := reflect.Fields(source);
            if (copies[5].Value.(*int) != p) return 9;
            if (reflect.IsStruct(p) || len(reflect.Fields(p)) != 0) return 10;
            reflect.Fields(source);
        }
        if (*p != 7) return 11; free(p); return 0;
    }
    func main() int { if (exercise() != 0) return 99; return extern.liveAllocations(); }''', memory=True)

    program('''func main() int {
        a: int[2] = {1, 2}; b: int[] = {1, 2};
        if (reflect.IsStruct("string") || reflect.IsStruct(a) || reflect.IsStruct(b)) return 1;
        if (len(reflect.Fields(false)) != 0 || len(reflect.Fields("string")) != 0 || len(reflect.Fields(a)) != 0) return 2;
        return 0;
    }''')

    # Private fields remain visible through the public snapshot API.
    (project / 'ocean.mod').write_text('module example.com/project\nocean 0.1\n')
    models = project / 'models'; models.mkdir()
    (models / 'models.oc').write_text('package models; struct Record { hidden string; } func New() Record { return Record{"private"}; }')
    program('''func main() int {
        fields := r.Fields(models.New());
        if (fields[0].Name != "hidden" || fields[0].Value.(string) != "private") return 1;
        return 0;
    }''', imports='import "reflect" as r; import "example.com/project/models";')

    for name in ('compilerReflectFields', 'compilerReflectIsStruct'):
        rejected('package main; func main() { ' + name + '(1); }')
    rejected(fixture)  # A package name alone does not grant privilege.
    spoof = project / 'reflect'; spoof.mkdir(); (spoof / 'reflect.oc').write_text(fixture)
    rejected('package main; import "reflect"; func main() { reflect.Fields(1); }')
    shutil.rmtree(spoof)
    (reflect / 'reflect.oc').write_text(fixture.replace('package reflect;', 'package imitation;'))
    rejected('package main; import "reflect"; func main() {}')
    (reflect / 'reflect.oc').write_text(fixture + '\nfunc Other(value any) bool { return compilerReflectIsStruct(value); }')
    rejected('package main; import "reflect"; func main() {}')
    (reflect / 'reflect.oc').write_text(fixture)
    external = root / 'outside.oc'; external.write_text(fixture)
    (reflect / 'reflect.oc').unlink(); (reflect / 'reflect.oc').symlink_to(external)
    rejected('package main; import "reflect"; func main() {}')
    (reflect / 'reflect.oc').unlink(); reflect.rmdir()
    outside = root / 'outside'; outside.mkdir(); (outside / 'reflect.oc').write_text(fixture)
    reflect.symlink_to(outside, target_is_directory=True)
    rejected('package main; import "reflect"; func main() {}')
    reflect.unlink(); reflect.mkdir(); (reflect / 'reflect.oc').write_text(fixture)

    other = standard / 'io'; other.mkdir()
    (other / 'io.oc').write_text('package io; func Probe(value any) bool { return compilerReflectIsStruct(value); }')
    rejected('package main; import "io"; func main() {}')
    shutil.rmtree(other)

    # Installed layout: executable-relative <prefix>/src works without OCEAN_SRC.
    installed = root / 'installed'; (installed / 'bin').mkdir(parents=True)
    shutil.copy2(compiler, installed / 'bin/ocean')
    shutil.copytree(standard, installed / 'src')
    source.write_text('package main; import "reflect"; struct S { value int; } func main() int { return reflect.Fields(S{3})[0].Value.(int) - 3; }')
    installed_env = dict(os.environ); installed_env.pop('OCEAN_SRC', None)
    built = run([str(installed / 'bin/ocean'), 'build', str(source), '-o', str(native)], env=installed_env)
    assert built.returncode == 0, built.stderr
    assert run([str(native)]).returncode == 0
    checks += 1

print(f'{checks} standard reflection checks passed')
