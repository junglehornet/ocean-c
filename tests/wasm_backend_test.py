"""WebAssembly target selection, assembly, linking, and cache isolation."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

compiler = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="ocean-wasm-") as directory:
    root = Path(directory)
    source = root / "main.oc"
    source.write_text("package main; func main() int { asm wasm { nop } return 42; }")
    env = dict(os.environ, OCOS="js", OCARCH="wasm", OCEAN_BUILD_CACHE=str(root / "cache"))

    def run(*args, ok=True, environment=env):
        result = subprocess.run([compiler, *args, str(source)], env=environment,
                                text=True, capture_output=True, timeout=30)
        assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
        return result.stdout + result.stderr

    ir = root / "main.ll"
    run("build", "--emit-llvm", "-o", str(ir))
    assert 'target triple = "wasm32-unknown-unknown"' in ir.read_text()
    assert 'asm sideeffect' in ir.read_text()
    output = root / "main.wasm"
    for _ in range(2):
        run("build", "-o", str(output))
        assert output.read_bytes()[:4] == b"\0asm"
    run("check")
    run("build", "-o", str(output), environment=dict(env, OCEAN_BUILD_CACHE="off"))
    if shutil.which("node"):
        subprocess.run(["node", "-e", """
const fs = require('fs');
const module = new WebAssembly.Module(fs.readFileSync(process.argv[1]));
const instance = new WebAssembly.Instance(module, {env: {free() {}}});
if (instance.exports.main() !== 42) throw new Error('wrong Wasm result');
""", str(output)], check=True, capture_output=True, timeout=30)
    run("run", ok=False)
    for os_name, arch in (("js", ""), ("", "wasm"), ("linux", "wasm"), ("js", "x86_64")):
        assert "requires OCOS=js and OCARCH=wasm" in run(
            "build", "--emit-llvm", ok=False,
            environment=dict(env, OCOS=os_name, OCARCH=arch))
    for selector in ("wasm/js", "x86_64/js", "js"):
        source.write_text(f"package main; func main() {{ asm {selector} {{ nop }} }}")
        run("build", "--emit-llvm", ok=False)
    source.write_text("package main; func main() { asm wasm { not_an_instruction } }")
    run("check", ok=False)
    native = dict(env, OCOS="", OCARCH="")
    run("check", environment=native)
    run("build", "-o", str(root / "native"), environment=native)
    assert (root / "native").read_bytes()[:4] != b"\0asm"
    source.write_text("package sample; func Answer() int { return 42; }")
    run("check")
    run("build", "-o", str(root / "sample.o"))
    assert (root / "sample.o").read_bytes()[:4] == b"\0asm"
    source.write_text("package main; func main() { x := 1; asm wasm { local.get ${x} } }")
    assert "do not yet support scalar operands" in run("build", "--emit-llvm", ok=False)
