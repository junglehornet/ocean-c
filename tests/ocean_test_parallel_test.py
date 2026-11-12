#!/usr/bin/env python3
"""Bounded shared workers, OS thread reuse, and unconstrained async subtests."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
stdlib = Path(os.environ.get('OCEAN_SRC', compiler.parent.parent / 'ocean-src')).resolve()
env = {**os.environ, 'OCEAN_SRC': str(stdlib), 'OCEAN_BUILD_CACHE': 'off', 'NO_COLOR': ''}

with tempfile.TemporaryDirectory(prefix='ocean-parallel-test-') as directory:
    root = Path(directory)
    (root / 'base.oc').write_text('package pool;')
    tests = root / 'pool_test.oc'

    def run(*options, failed=False):
        result = subprocess.run([str(compiler), 'test', str(root), '--json', '--timeout', '8s', *options],
                                env=env, capture_output=True, text=True, timeout=20)
        assert (result.returncode != 0) == failed, (options, result.returncode, result.stdout, result.stderr)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        assert events[-1]['Action'] == ('fail' if failed else 'pass'), events[-1:]
        return events

    def pool_source(expected, uneven=False):
        body = f'''package pool;
import "testing"; import "sync"; import "threads"; import "time";
gate: sync.Mutex;
active: int; peak: int; started: int; completed: int;
ids: map[int]bool;
func job(t *testing.T) {{
    gate.Lock();
    active++; started++;
    if (active > peak) peak = active;
    id := threads.CurrentID();
    ids[id] = true;
    gate.Unlock();
    for (;;) {{
        gate.Lock(); ready := started >= {expected}; gate.Unlock();
        if (ready) break;
        time.Sleep(time.Millisecond);
    }}
'''
        if uneven:
            body += '''    if (t.Name() == "Test00") {
        for (;;) {
            gate.Lock(); ready := completed == 31; gate.Unlock();
            if (ready) break;
            time.Sleep(time.Millisecond);
        }
    }
'''
        body += '''    time.Sleep(5 * time.Millisecond);
    gate.Lock();
    active--; completed++;
    last := completed == 32; high := peak; unique := len(ids); done := completed;
    gate.Unlock();
    if (last) t.Logf("pool peak={} ids={} completed={}", high, unique, done);
}
'''
        return body + ''.join(f'func Test{i:02d}(t *testing.T) {{ job(t); }}\n' for i in range(32))

    for options, expected in [((), 16), (('-t', '3'), 3), (('--single-threaded',), 1), (('-t', '1'), 1)]:
        tests.write_text(pool_source(expected))
        events = run(*options)
        starts = [e['Test'] for e in events if e['Action'] == 'run']
        passes = [e['Test'] for e in events if e['Action'] == 'pass' and 'Test' in e]
        assert len(starts) == len(set(starts)) == len(passes) == 32
        messages = ''.join(e.get('Output', '') for e in events)
        assert f'pool peak={expected} ids={expected} completed=32' in messages, messages

    # One worker stays in Test00 while another drains all 31 remaining jobs.
    # Static partitions cannot satisfy this barrier; a shared queue can.
    tests.write_text(pool_source(2, uneven=True))
    run('-t', '2')

    # Failing/skipping workers retire, but the pool must continue draining work.
    tests.write_text('''package pool; import "testing"; import "threads";
func identify(t *testing.T) { t.Logf("id={}", threads.CurrentID()); }
func TestFatal(t *testing.T) { identify(t); defer func() { t.Log("fatal defer"); }(); t.Fatal("fatal"); t.Log("UNREACHABLE"); }
func TestSkip(t *testing.T) { identify(t); t.Skip("skip"); }
func TestPanic(t *testing.T) { identify(t); panic "caught panic"; }
func TestCleanup(t *testing.T) {
    identify(t);
    t.Cleanup(func() { t.Log("last cleanup"); });
    t.Cleanup(func() { t.Fatal("cleanup fatal"); });
}
func TestLater(t *testing.T) { identify(t); t.Log("all later work ran"); }
''')
    events = run('--single-threaded', failed=True)
    assert len([e for e in events if e['Action'] == 'run']) == 5
    messages = ''.join(e.get('Output', '') for e in events)
    assert all(s in messages for s in ('fatal defer', 'caught panic', 'last cleanup', 'all later work ran'))
    assert 'UNREACHABLE' not in messages
    ids = {e['Test']: int(re.search(r'id=(\d+)', e['Output']).group(1))
           for e in events if e['Action'] == 'output' and e.get('Output', '').startswith('id=')}
    assert ids['TestPanic'] == ids['TestCleanup'], ids  # ordinary recovered panic keeps its worker
    assert ids['TestFatal'] != ids['TestSkip'] != ids['TestPanic']
    assert ids['TestCleanup'] != ids['TestLater']

    tests.write_text('''package pool;
import "testing"; import "sync"; import "time";
func TestAsync(t *testing.T) {
    gate: sync.Mutex;
    started := 0; finished := 0; cleanups := 0; release := false;
    parent := t;
    t.Cleanup(func() { if (cleanups != 24) t.Error("lost concurrent cleanup registration"); });
    for (i := 0; i < 24; i++) {
        t.Parallel("child", func(t *testing.T) {
            gate.Lock(); started++; gate.Unlock();
            parent.Cleanup(func() { cleanups++; });
            for (;;) {
                gate.Lock(); ready := release; gate.Unlock();
                if (ready) break;
                time.Sleep(time.Millisecond);
            }
            t.Cleanup(func() { gate.Lock(); finished++; gate.Unlock(); });
        });
    }
    // This barrier requires >16 additional native threads, even with -t 1.
    for (;;) {
        gate.Lock(); ready := started == 24; gate.Unlock();
        if (ready) break;
        time.Sleep(time.Millisecond);
    }
    t.Log("24 async children started without blocking the caller");
    gate.Lock(); release = true; gate.Unlock();
    t.Cleanup(func() { if (finished != 24) t.Error("parent cleanup ran before child cleanup"); });
}
func TestAsyncFail(t *testing.T) {
    t.Parallel("fatal", func(t *testing.T) { t.Fatal("async fatal"); });
    t.Parallel("panic", func(t *testing.T) { panic "async panic"; });
    t.Parallel("skip", func(t *testing.T) { t.Skip("async skip"); });
    t.Cleanup(func() { t.Log("parent cleanup after async failure"); });
}
func TestRunWaits(t *testing.T) {
    complete := false;
    success := t.Run("sync", func(t *testing.T) {
        t.Parallel("nested", func(t *testing.T) {
            time.Sleep(10 * time.Millisecond); complete = true; t.Error("nested failure");
        });
    });
    if (success || !complete) t.Fatal("Run returned before nested async completion");
    t.Log("Run waited");
}
func TestParentFatal(t *testing.T) {
    t.Parallel("late", func(t *testing.T) { time.Sleep(20 * time.Millisecond); t.Log("late child done"); });
    t.Cleanup(func() { t.Log("fatal parent cleanup"); });
    t.Fatal("parent fatal");
}
func TestFinal(t *testing.T) { t.Log("final test ran"); }
''')
    for options in [('--single-threaded',), ()]:
        events = run(*options, failed=True)
        messages = ''.join(e.get('Output', '') for e in events)
        assert '24 async children started without blocking the caller' in messages
        assert 'parent cleanup ran before child cleanup' not in messages
        assert 'lost concurrent cleanup registration' not in messages
        assert 'Run returned before nested async completion' not in messages
        assert 'Run waited' in messages and 'final test ran' in messages
        async_names = [e['Test'] for e in events if e['Action'] == 'run' and e['Test'].startswith('TestAsync/child')]
        assert len(async_names) == len(set(async_names)) == 24
        for parent in ('TestAsync', 'TestAsyncFail', 'TestRunWaits', 'TestParentFatal'):
            parent_end = next(i for i, e in enumerate(events) if e.get('Test') == parent and e['Action'] in ('pass', 'fail'))
            child_ends = [i for i, e in enumerate(events) if e.get('Test', '').startswith(parent + '/') and e['Action'] in ('pass', 'fail', 'skip')]
            assert child_ends and max(child_ends) < parent_end, parent
        assert next(e['Action'] for e in events if e.get('Test') == 'TestAsync' and e['Action'] in ('pass', 'fail')) == 'pass'
        for parent in ('TestAsyncFail', 'TestRunWaits', 'TestParentFatal'):
            assert any(e.get('Test') == parent and e['Action'] == 'fail' for e in events)

    for options in [('-t',), ('-t', '0'), ('-t', '-1'), ('-t', '2x'), ('-t', '2147483648')]:
        result = subprocess.run([str(compiler), 'test', *options, str(root)], env=env, capture_output=True, text=True)
        assert result.returncode != 0 and 'thread count' in result.stderr, (options, result.stderr)

print('Ocean parallel test checks passed')
