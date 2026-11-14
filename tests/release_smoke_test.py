"""Native release gate: spawning, paths with spaces, imports, cache, and LSP workers."""
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import shlex
import sys
import tempfile

compiler = str(Path(sys.argv[1]).resolve())
runner = shlex.split(os.environ.get("OCEAN_TEST_RUNNER", ""))


def execute(*args, **kwargs):
    result = subprocess.run([*runner, compiler, *args], text=True, capture_output=True, timeout=60, **kwargs)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix="ocean release ") as directory:
    root = Path(directory)
    source = root / "main.oc"
    source.write_text('package main; import "io"; func main() { io.Println("release smoke"); }', encoding="utf-8")
    environment = dict(os.environ, OCEAN_BUILD_CACHE=str(root / "cache"))
    execute("check", str(root), env=environment)
    suffix = ".exe" if os.name == "nt" else ""
    binary = root / ("test output" + suffix)
    for _ in range(2):
        execute("build", str(root), "-o", str(binary), env=environment)
        assert subprocess.check_output([*runner, str(binary)], text=True).strip() == "release smoke"
    assert execute("run", str(root), env=environment).stdout.strip() == "release smoke"
    source.write_text('package main; func main() { panic "release panic"; }', encoding="utf-8")
    failure = subprocess.run([*runner, compiler, "run", str(root)], env=environment,
                             text=True, capture_output=True, timeout=60)
    assert failure.returncode == 1 and "Panic: release panic" in failure.stderr, failure
    source.write_text("package main; func main() {}", encoding="utf-8")
    test = root / "main_test.oc"
    test.write_text('package main; import "testing"; func TestRelease(t *testing.T) { if (9223372036854775807 < 0) t.Fatal("integer width"); } func TestRecovery(t *testing.T) { defer func() { if (recover() == null) t.Fatal("missing panic"); }(); panic "native recovery"; }', encoding="utf-8")
    execute("test", "--notty", str(root), env=environment)
    other = root / "other"
    other.mkdir()
    (other / "sample.oc").write_text("package sample;", encoding="utf-8")
    (other / "sample_test.oc").write_text('package sample; import "testing"; func TestOther(t *testing.T) { t.Parallel(); }', encoding="utf-8")
    execute("test", "--notty", "-t", "2", str(root), str(other), env=environment)
    test.unlink()
    # Cached support includes both native runtimes. COFF packages must use
    # archive composition, not an unsupported relocatable (-r) link. On Mach-O,
    # hidden constants must remain external through runtime/library composition.
    runtime = root / "runtime"
    runtime.mkdir()
    (runtime / "main.oc").write_text('package main; import "sync"; import "os/exec"; import "io"; '
        'func main() int { m: sync.Mutex; m.Lock(); m.Unlock(); io.Println("runtime constant").unwrap(); '
        'if (!exec.LookPath("ocean-missing-release-fixture-9861").hasError()) return 1; return 0; }')
    runtime_binary = root / ("runtime output" + suffix)
    for _ in range(2):
        execute("build", str(runtime), "-o", str(runtime_binary), env=environment)
        result = subprocess.run([*runner, str(runtime_binary)], check=True,
                                capture_output=True, text=True, timeout=30)
        assert result.stdout == "runtime constant\n", result
    library = root / "runtime.o"
    (runtime / "main.oc").write_text('package nativefixture; import "sync"; import "os/exec"; import "io"; '
        'func Probe() { m: sync.Mutex; m.Lock(); m.Unlock(); io.Println("library constant"); exec.LookPath("ocean-missing-release-fixture-9861"); }')
    execute("build", str(runtime), "-o", str(library), env=environment)
    if os.name == "nt":
        assert library.read_bytes().startswith(b"!<arch>\n")

    if sys.platform == "darwin":
        symbols = subprocess.check_output(["nm", "-m", str(library)], text=True)
        for name in (".constant.", "Probe"):
            assert any(name in line and "private external" in line
                       for line in symbols.splitlines()), symbols

    # LSP receives overlays through the native Windows re-exec worker too.
    server = subprocess.Popen([*runner, compiler, "check", "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    def send(method, params=None, number=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        if number is not None:
            message["id"] = number
        data = json.dumps(message).encode()
        server.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)
        server.stdin.flush()
    def receive():
        def read():
            header = b""
            while not header.endswith(b"\r\n\r\n"):
                byte = server.stdout.read(1)
                assert byte, "LSP closed the stream"
                header += byte
            return json.loads(server.stdout.read(int(header.split(b":", 1)[1].strip())))
        return pool.submit(read).result(timeout=30)
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)
    try:
        send("initialize", {}, 1)
        assert receive()["id"] == 1
        uri = source.as_uri()
        send("textDocument/didOpen", {"textDocument": {"uri": uri, "version": 1,
             "text": "package main; func main() { value: int = false; }"}})
        diagnostics = receive()
        assert diagnostics["params"]["diagnostics"], diagnostics
        send("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2},
             "contentChanges": [{"text": "package main; func main() { value := 42; }"}]})
        assert not receive()["params"]["diagnostics"]
        send("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 28}}, 2)
        hover = receive()
        assert hover["id"] == 2 and "int" in json.dumps(hover.get("result")), hover
        send("shutdown", None, 3)
        assert receive()["id"] == 3
        send("exit")
        assert server.wait(timeout=10) == 0
    finally:
        if server.poll() is None:
            server.kill()
        server.wait()
        pool.shutdown(wait=True)
print("Native release smoke tests passed.")
