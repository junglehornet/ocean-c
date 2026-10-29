"""Immutable heap strings and bounded allocation counts during reassignment."""
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])


def run(args):
    return subprocess.run(args, text=True, capture_output=True, timeout=30)


with tempfile.TemporaryDirectory(prefix="ocean string ownership ") as directory:
    root = Path(directory)
    source, ir, native, tracker = (root / name for name in ("main.oc", "module.ll", "native", "tracker.c"))
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
    abort(); // A double free or an attempt to free stack/static storage.
}
int64_t liveAllocations(void) { return live; }
void writeFirst(char *data) { data[0] = 'X'; }
''')
    checks = 0

    def program(body):
        global checks
        source.write_text('package main; extern int liveAllocations(); ' + body)
        emitted = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)])
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        module = ir.read_text()
        assert '@__ocean.dynamic_array.track' not in module and '@__ocean.dynamic_array.clean' not in module, module
        assert '@__ocean.dynamic_array.' not in module, module
        for optimization in ('-O0', '-O2'):
            built = run([clang, optimization, '-Wno-override-module', str(ir), str(tracker),
                         '-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=free', '-lffi', '-o', str(native)])
            assert built.returncode == 0, built.stdout + built.stderr
            result = run([str(native)])
            assert (result.returncode, result.stdout, result.stderr) == (0, '', ''), (body, result)
            checks += 1

    def rejected(body, message):
        global checks
        source.write_text('package main; ' + body)
        result = run([compiler, 'build', '--emit-llvm', str(source), '-o', str(ir)])
        assert result.returncode != 0 and message in result.stderr, result
        checks += 1

    program('''func main() int {
        s := "hello"; values: int[] = {1, 2};
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            s = "replacement"; s = s; s += "!";
            values = {i, 2, 3}; values = values; values += int[]{4};
            if (s != "replacement!" || s.capacity != len(s) || len(values) != 4) return 1;
            current := extern.liveAllocations();
            if (current != baseline) return 2;
        }
        s = ""; if (s.capacity != 0 || len(s) != 0) return 3;
        free(s); free(values);
        return extern.liveAllocations();
    }''')
    program('''func replace(p *string) { *p = "long replacement"; }
    func generate(s string) string { s += "!"; return s; }
    func main() int {
        p := new(string); *p = "first"; alias := p;
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            replace(alias); *p = *p; *p = generate(*p); *p += "!";
            if (*p != "long replacement!!" || (*p).capacity != len(*p)) return 1;
            current := extern.liveAllocations(); if (current != baseline) return 2;
        }
        *p = ""; *p = "last";
        free(p); return extern.liveAllocations();
    }''')
    program('''func main() int {
        nested: string[][] = {{"one", "two"}, {"three"}};
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            nested = {{"four", "five"}, {"six"}};
            nested[0][0] = "replacement";
            current := extern.liveAllocations(); if (current != baseline) return 1;
        }
        free(nested); return extern.liveAllocations();
    }''')
    program('''func main() int {
        s := "abc"; bytes: i8[] = s; bytes[0] = 'x'; bytes.append(1);
        if (s != "abc" || len(s) != 3 || s.capacity != 3 || bytes[0] != 'x') return 1;
        t: string = bytes; bytes[1] = 'y';
        if (t[1] != 'b' || t.capacity != len(t)) return 2;
        free(s); free(bytes); free(t); return extern.liveAllocations();
    }''')
    program('''struct Box { text string; values int[]; }
    func main() int {
        p := new(Box); *p = Box{"one", {1, 2}};
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            *p = Box{"two", {3, 4}}; p.text = "longer"; p.values = {5, 6, 7};
            current := extern.liveAllocations(); if (current != baseline) return 1;
        }
        free(p); return extern.liveAllocations();
    }''')
    program('''func join(values ...string) string {
        result := ""; for (i, value := range values) result += value; return result;
    }
    func main() int {
        s := "start"; a: string[] = {"one", "two"};
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            s = join("prefix", a...);
            if (s != "prefixonetwo") return 1;
            current := extern.liveAllocations(); if (current != baseline) return 2;
        }
        free(s); free(a); return extern.liveAllocations();
    }''')
    program('''func main() int {
        s := ""; baseline := extern.liveAllocations(); i := 0;
        for (; len(s + "") < 100; s = s + "x") {
            s = i % 2 == 0 ? s : s;
            ch := "abc"[1]; if (ch != 98) return 1;
            current := extern.liveAllocations(); if (current != baseline) return 2;
            i++;
        }
        if (len(s) != 100 || s.capacity != 100) return 3;
        free(s); return extern.liveAllocations();
    }''')
    program('''extern void writeFirst(buffer<i8> data);
    func main() int {
        s := "abc"; baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            extern.writeFirst(s.data);
            if (s != "abc") return 1;
            current := extern.liveAllocations(); if (current != baseline) return 2;
        }
        free(s); return extern.liveAllocations();
    }''')
    program('''func replace(s string, a int[]) {
        other := "a local value"; nested: int[][] = {{1, 2}, {3}};
        s = "new text"; a = {8, 9}; a.append(10);
    }
    func main() int {
        s := "original"; a: int[] = {1, 2};
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            replace(s, a);
            if (s != "original" || len(a) != 2 || a[0] != 1) return 1;
            current := extern.liveAllocations(); if (current != baseline) return 2;
        }
        free(s); free(a); return extern.liveAllocations();
    }''')
    program('''struct Result { name string; values int[][]; }
    func build(n int) Result {
        discarded := "discarded"; extra: int[] = {7, 8};
        result := Result{"returned", {{n, 2}, {3}}}; return result;
    }
    func early(n int) string {
        a := "outer"; b: int[] = {1, 2};
        if (n == 0) { c := "branch"; return a; }
        return "literal";
    }
    func main() int {
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            value := build(i);
            if (value.name != "returned" || value.values[0][0] != i) return 1;
            if (early(i % 2) != (i % 2 == 0 ? "outer" : "literal")) return 2;
        }
        return extern.liveAllocations() - baseline;
    }''')
    program('''struct Owner { local int[]; pointer *int[]; }
    func replace(s string, a int[]) { s = "copy"; a = {8, 9}; }
    saved: *int[];
    func allocate() { p := new(int[]); *p = {4, 5}; saved = p; }
    func use(p *int[], s *string) {
        replace(*s, *p);
        pointerAlias := p; stringAlias := s;
        copied := *p; copied = {9}; text := *s; text = "local";
        owner := Owner{{7, 8}, p};
    }
    func main() int {
        allocate(); s := new(string); *s = "heap";
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            use(saved, s);
            if ((*saved)[0] != 4 || *s != "heap") return 1;
            current := extern.liveAllocations(); if (current != baseline) return 2;
        }
        free(saved); free(s); return extern.liveAllocations();
    }''')
    program('''func recur(a int[], n int) int[] {
        local := "recursion";
        if (n == 0) return a;
        a.append(n); return recur(a, n - 1);
    }
    func main() int {
        a: int[] = {1}; baseline := extern.liveAllocations();
        for (i := 0; i < 20; i++) {
            result := recur(a, 8);
            if (len(a) != 1 || len(result) != 9 || result[0] != 1) return 1;
        }
        current := extern.liveAllocations(); if (current != baseline) return 2;
        free(a); return extern.liveAllocations();
    }''')
    program('''func main() int {
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            text := "loop"; values: int[] = {1, 2};
            if (i % 2 == 0) continue;
            nested: string[] = {"one", "two"};
            if (i > 90) break;
        }
        return extern.liveAllocations() - baseline;
    }''')
    program('''func factory() *int[] {
        scratch := "temporary"; p := new(int[]); *p = {1, 2, 3}; return p;
    }
    func inspect(p *int[]) { alias := p; copy := *p; copy[0] = 9; }
    func main() int {
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            p := factory(); inspect(p);
            if ((*p)[0] != 1 || len(*p) != 3) return 1;
            free(p);
            current := extern.liveAllocations(); if (current != baseline) return 2;
        }
        return extern.liveAllocations() - baseline;
    }''')
    program('''func empty() any[] {}
    func main() int { a := empty(); if (len(a) != 0) return 1; return 0; }''')
    program('''func consume(values any[]) { values = any[]{"copy", "only"}; }
    func assign(p *any[]) {
        local := *p; consume(local); *p = any[]{"new", "values"};
    }
    func exercise() int {
        p := new(any[]); *p = any[]{"old", "values"};
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            assign(p);
            if ((*p)[0].(string) != "new") return 1;
            current := extern.liveAllocations(); if (current != baseline) return 2;
        }
        free(p); return 0;
    }
    func main() int {
        if (exercise() != 0) return 1;
        return extern.liveAllocations();
    }''')
    program('''struct Texts(count int) { data string[count]; }
    func create(count int) Texts {
        text := Texts{count}; text.data[0] = "first"; return text;
    }
    func main() int {
        baseline := extern.liveAllocations();
        for (i := 0; i < 100; i++) {
            value := create(3);
            if (value.data[0] != "first" || len(value.data[1]) != 0) return 1;
        }
        return extern.liveAllocations() - baseline;
    }''')
    program('''struct Entry { name string; values int[]; }
    func exercise() int {
        words: string[] = {"first", "middle", "last"};
        middle := words.remove(1); last := words.pop();
        if (middle != "middle" || last != "last" || words[0] != "first") return 1;
        words.pop(); words.append(middle); if (words.pop() != "middle") return 2;
        nested: string[][] = {{"one"}, {"two", "three"}};
        selected := nested.pop(); nested[0][0] = "changed";
        if (selected[0] != "two" || selected[1] != "three") return 3;
        records: Entry[] = {Entry{"a", {1}}, Entry{"b", {2, 3}}};
        record := records.remove(0); records.pop();
        if (record.name != "a" || record.values[0] != 1) return 4;
        for (i := 0; i < 100; i++) {
            words.append("temporary"); words.pop();
            nested.append(string[]{"temporary"}); nested.remove(0);
        }
        p := new(string[]); *p = {"heap", "tail"};
        text := (*p).remove(0); free(p);
        if (text != "heap") return 5;
        return 0;
    }
    func main() int {
        if (exercise() != 0) return 1;
        return extern.liveAllocations();
    }''')
    program('''func choose(p *string[]) int { *p = {"replacement"}; return 1; }
    func take(values string[]) string { return values.pop(); }
    func main() int {
        baseline := extern.liveAllocations();
        {
            values: string[] = {"first", "second", "third"};
            removed := values.remove(choose(&values));
            if (removed != "second" || len(values) != 2 || values[1] != "third") return 1;
            returned := take(values);
            if (returned != "third" || len(values) != 2) return 2;
        }
        return extern.liveAllocations() - baseline;
    }''')
    for statement in ("s[0] = 'x';", "s[0]++;", "s[0] += 1;", "s.append(1);", "s.prepend(1);", "s.add(0, 1);", "s.data = {1};"):
        rejected('func main() { s := "abc"; ' + statement + ' }', 'immutable')
    rejected('func main() { s := "abc"; p := &s[0]; }', 'immutable')
    rejected('func main() { s := "abc"; s.capacity = 8; }', 'immutable')
    rejected('func main() { s := "abc"; s.length = 8; }', 'immutable')

print(f'{checks} immutable string/ownership checks passed')
