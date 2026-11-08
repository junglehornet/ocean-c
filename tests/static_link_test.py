"""Static flags reach both link paths, reuse objects, and work with run."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

compiler, clang = (str(Path(arg).resolve()) for arg in sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='ocean-static-') as temporary:
    root = Path(temporary)
    tools = root / 'tools'
    tools.mkdir()
    log = root / 'calls.jsonl'
    wrapper = tools / 'clang'
    # Strip -static after recording: test option propagation on hosts without
    # static system libraries as well as those supporting static executables.
    wrapper.write_text('#!' + sys.executable + '\nimport os, sys, json\n'
                       'with open(os.environ["STATIC_LOG"], "a") as f: f.write(json.dumps(sys.argv[1:]) + "\\n")\n'
                       'if "-static" in sys.argv and os.environ.get("STATIC_FAIL"): sys.exit(23)\n'
                       'os.execv(' + repr(clang) + ', [' + repr(clang) + ', *[a for a in sys.argv[1:] if a != "-static"]])\n')
    wrapper.chmod(0o755)
    env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ.get('PATH', ''),
               STATIC_LOG=str(log), OCEAN_BUILD_CACHE=str(root / 'cache'), OCEAN_SRC=str(root))
    source = root / 'main.oc'
    source.write_text('package main; func main() int { return 7; }')
    output = root / 'program'

    def invoke(*args, status=0, environment=None):
        result = subprocess.run([compiler, *map(str, args)], env=environment or env,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == status, result

    def calls():
        return [json.loads(line) for line in log.read_text().splitlines()]

    invoke('build', source, '-o', output)
    before = len(calls())
    for flag in ('--static', '-s'):
        invoke('build', flag, source, '-o', output)
        assert len(calls()) == before + 1 and '-static' in calls()[-1]
        assert subprocess.run([str(output)]).returncode == 7
        before += 1
        invoke('run', source, flag, status=7)
        assert len(calls()) == before + 1 and '-static' in calls()[-1]
        before += 1
        for args in [('check', flag, source), ('build', '--check', flag, source),
                     ('build', '--emit-llvm', flag, source)]:
            invoke(*args, status=1)
        assert len(calls()) == before
    invoke('build', '-s', source, '-o', output, environment=dict(env, OCEAN_BUILD_CACHE='off'))
    assert '-static' in calls()[-1] and '-x' in calls()[-1]
    invoke('run', '-s', source, status=7, environment=dict(env, OCEAN_BUILD_CACHE='off'))
    saved = output.read_bytes()
    invoke('build', '-s', source, '-o', output, status=1, environment=dict(env, STATIC_FAIL='1'))
    assert output.read_bytes() == saved
    source.write_text('package library; func Value() int { return 1; }')
    invoke('build', '-s', source, status=1)
print('static link checks passed')
