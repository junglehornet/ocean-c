#!/usr/bin/env python3
"""End-to-end language testing, package isolation, unwinding, and reports."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
stdlib = Path(os.environ.get('OCEAN_SRC', compiler.parent.parent / 'ocean-src'))
env = {**os.environ, 'OCEAN_SRC': str(stdlib), 'OCEAN_BUILD_CACHE': 'off', 'NO_COLOR': ''}


def run(*args, ok=True, cwd=None):
    result = subprocess.run([str(compiler), *map(str, args)], cwd=cwd, env=env,
                            text=True, capture_output=True, timeout=30)
    assert (result.returncode == 0) == ok, (args, result.returncode, result.stdout, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix='ocean-test-cli-') as temporary:
    root = Path(temporary)
    (root / 'ocean.mod').write_text('module example.test/calc\n\nocean 0.1\n')
    (root / 'calc.oc').write_text('''package calc;
func private() int { return 42; }
func Answer() int { return private(); }
''')
    internal = root / 'calc_test.oc'
    internal.write_text('''package calc;
import "testing";
func TestInternal(t *testing.T) {
    if (private() != 42) t.Fatal("private access failed");
    if (t.Name() != "TestInternal") t.Error("name");
    value := 0;
    if (!t.Run("child", func(t *testing.T) {
        if (t.Name() != "TestInternal/child") t.Fatal("child name");
        t.Cleanup(func() { value = 7; });
        t.Cleanup(func() { if (value != 0) t.Error("cleanup order"); });
        t.Logf("answer {}", private());
    })) t.Error("child failed");
    if (value != 7) t.Error("cleanup not awaited");
}
func lowerCase(t *testing.T) { t.Fatal("must not run"); }
''')
    external = root / 'api_test.oc'
    external.write_text('''package calc_test;
import "example.test/calc";
import "testing";
func TestPublic(t *testing.T) { if (calc.Answer() != 42) t.Error("public API"); }
''')
    passed = run('test', root, '-v')
    assert 'TestInternal/child' in passed.stdout and 'TestPublic' in passed.stdout
    assert '─── Summary ' in passed.stdout and '✓' in passed.stdout and '∑' in passed.stdout
    assert '=== RUN' not in passed.stdout and '\x1b' not in passed.stdout
    if Path('/dev/full').exists():
        run('test', root, '--jsonfile', '/dev/full', ok=False)
        run('test', root, '--outfile', '/dev/full', ok=False)
    default = run('test', root)
    assert 'TestPublic' not in default.stdout  # tang hides passing details by default

    # Ordinary builds exclude tests even when a test file selects the package.
    broken = root / 'broken_test.oc'
    broken.write_text('this is deliberately not valid Ocean')
    run('build', broken, '-o', root / 'calc.o')
    run('check', root, ok=False)
    run('test', root, ok=False)
    broken.unlink()

    # External tests never inherit the package's private symbols.
    external.write_text(external.read_text().replace('calc.Answer()', 'calc.private()'))
    private_error = run('test', root, ok=False)
    assert 'not exported' in private_error.stderr
    external.write_text(external.read_text().replace('calc.private()', 'calc.Answer()'))

    failing = root / 'failure_test.oc'
    failing.write_text('''package calc;
import "testing";
import "io";
func TestFatal(t *testing.T) {
    value := 0;
    success := t.Run("fatal", func(t *testing.T) {
        defer func() { value = 1; }();
        t.Cleanup(func() { value += 2; });
        t.Fatalf("fatal {}", 42);
        io.Println("UNREACHABLE");
    });
    if (success || value != 3) t.Fatal("bad fatal unwind");
    t.Error("parent continued");
    t.Errorf("formatted {}", 9);
}
func TestPanic(t *testing.T) { panic "caught panic"; }
func TestSkip(t *testing.T) { t.Skip("skip reason"); }
func TestLater(t *testing.T) { t.Log("later test ran"); io.Println("raw stdout"); }
func TestCleanup(t *testing.T) {
    t.Cleanup(func() { t.Log("last cleanup"); });
    t.Cleanup(func() { t.Fatal("fatal cleanup"); });
    t.Cleanup(func() { t.Log("first cleanup"); });
}
func TestEscaping(t *testing.T) { t.Log("quote: \\" slash: \\\\ tab: \\t newline: \\n"); }
''')
    report = root / 'results.txt'
    events_file = root / 'results.jsonl'
    result = run('test', root, '--json', '--outfile', report, '--jsonfile', events_file, ok=False)
    events = [json.loads(line) for line in result.stdout.splitlines()]
    assert events == [json.loads(line) for line in events_file.read_text().splitlines()]
    assert events[0]['Action'] == 'start' and events[-1]['Action'] == 'fail'
    assert all(event['Package'] == str(root) for event in events)
    assert any(event.get('Test') == 'TestFatal/fatal' and event['Action'] == 'fail' for event in events)
    assert any(event.get('Test') == 'TestLater' and event['Action'] == 'pass' for event in events)
    messages = ''.join(event.get('Output', '') for event in events)
    assert 'UNREACHABLE' not in messages and 'bad fatal unwind' not in messages
    assert 'parent continued' in messages and 'formatted 9' in messages and 'caught panic' in messages
    assert 'raw stdout' in messages and 'later test ran' in messages
    assert 'skip reason' in messages and 'quote: " slash: \\ tab: \t newline: \n' in messages
    cleanup = [event.get('Output', '') for event in events if event.get('Test') == 'TestCleanup' and event['Action'] == 'output']
    assert cleanup == ['first cleanup\n', 'fatal cleanup\n', 'last cleanup\n'], cleanup
    assert any(event['Elapsed'] > 0 for event in events if event['Action'] == 'pass')
    saved = report.read_text()
    assert '─── Messages ' in saved and '─── Summary ' in saved and 'FAIL' in saved
    assert 'TestFatal/fatal' in saved and '\x1b' not in saved
    run('test', root, '--outfile', internal, ok=False)
    assert internal.read_text().startswith('package calc;')
    run('test', root, '--outfile', report, '--jsonfile', report, ok=False)
    failing.unlink()

    # Reject incorrect discovered signatures at compilation time.
    for signature in ('func TestBad() {}', 'func TestBad(t int) {}',
                      'func TestBad(t *testing.T) int { return 0; }'):
        failing.write_text('package calc; import "testing"; ' + signature)
        run('test', root, ok=False)
    failing.write_text('package wrong_test; import "testing"; func TestBad(t *testing.T) {}')
    assert 'calc_test' in run('test', root, ok=False).stderr
    failing.unlink()

    # Package main retains its ordinary main only for normal builds.
    app = root / 'app'; app.mkdir()
    (app / 'main.oc').write_text('package main; func main() int { return 19; }')
    (app / 'main_test.oc').write_text('package main; import "testing"; func TestMain(t *testing.T) {}')
    run('test', app)
    assert run('run', app, ok=False).returncode == 19

    # External tests import any module-root package by the module name.
    (app / 'ocean.mod').write_text('module calculator\n')
    for declared in ('main', 'arithmetic'):
        (app / 'main.oc').write_text('package ' + declared + '; '
                                   'func Answer() int { return 42; } func main() {}')
        (app / 'main_test.oc').write_text('package ' + declared + '_test; '
            'import "testing"; import "calculator"; '
            'func TestRoot(t *testing.T) { if (calculator.Answer() != 42) t.Fatal("answer"); }')
        run('check', app)
        assert 'TestRoot' in run('test', app, '-v').stdout

    # A production package may itself have a name ending in _test.
    suffix = root / 'suffix'; suffix.mkdir()
    (suffix / 'base.oc').write_text('package suffix_test; func private() int { return 7; }')
    (suffix / 'a_test.oc').write_text('package suffix_test; import "testing"; func TestSuffix(t *testing.T) { if (private() != 7) t.Fatal("private"); }')
    (suffix / 'api_test.oc').write_text('package suffix_test_test; import "testing"; func TestSuffix(t *testing.T) {}')
    suffix_events = [json.loads(line) for line in run('test', suffix, '--json').stdout.splitlines()]
    suffix_names = [event['Test'] for event in suffix_events if event['Action'] == 'run']
    assert len(suffix_names) == len(set(suffix_names)) == 2, suffix_names

    # Empty suites succeed and do not import test files from dependencies.
    empty = root / 'empty'; empty.mkdir()
    (empty / 'empty.oc').write_text('package empty; import "example.test/calc"; func Value() int { return calc.Answer(); }')
    internal.write_text('invalid source excluded from dependency builds')
    run('test', empty)

    # Hanging tests are terminated and produce a failing package event.
    (empty / 'wait_test.oc').write_text('package empty; import "testing"; func TestWait(t *testing.T) { for (;;) {} }')
    timeout = run('test', empty, '--timeout', '100ms', '--json', ok=False)
    timeout_events = [json.loads(line) for line in timeout.stdout.splitlines()]
    assert any('timed out' in event.get('Output', '') for event in timeout_events)
    assert timeout_events[-1]['Action'] == 'fail'

print('Ocean test integration checks passed')
