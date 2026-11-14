"""Installer target detection must retain both ARM64 stores on Darwin too."""
from pathlib import Path
import os
import platform
import re
import subprocess
import sys
import tempfile

compiler, probe, clang, objdump = (str(Path(arg).resolve()) for arg in sys.argv[1:])
source = (Path(__file__).resolve().parents[1] / "installer/platform.oc").read_text()
blocks = re.findall(r"asm (aarch64(?:/\w+)?)\s*\{([^}]*\$\{\w+\}[^}]*)\}", source)
assert {selector for selector, _ in blocks} == {
    "aarch64", "aarch64/linux", "aarch64/macos", "aarch64/windows"}
targets = {"linux": "aarch64-linux-gnu", "macos": "arm64-apple-macosx11.0",
           "windows": "aarch64-w64-windows-gnu"}

with tempfile.TemporaryDirectory(prefix="ocean installer target ") as directory:
    root = Path(directory)
    sample, ir, binary = (root / name for name in ("sample.oc", "sample.ll", "sample.o"))
    for selector, body in blocks:
        body = re.sub(r"\$\{\w+\}", "${result}", body)
        sample.write_text("package probe; func sample() { asm " + selector + " {" + body + "} }")
        ir.write_text(subprocess.check_output([probe, str(sample)], text=True))
        systems = [selector.split("/")[1]] if "/" in selector else targets
        for system in systems:
            for optimization in ("-O0", "-O2"):
                subprocess.run([clang, "--target=" + targets[system], "-Wno-override-module",
                                optimization, "-c", "-x", "ir", str(ir), "-o", str(binary)], check=True)
                disassembly = subprocess.check_output([objdump, "-d", str(binary)], text=True)
                assert re.search(r"\bstr\s+x0,\s*\[", disassembly), (
                    selector, system, optimization, "target store was discarded", disassembly)

    # Also exercise the complete target() function on this host, with and
    # without package caching, rather than relying only on successful assembly.
    system = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}[platform.system()]
    architecture = {"AMD64": "x86_64", "arm64": "aarch64", "ARM64": "aarch64"}.get(
        platform.machine(), platform.machine())
    sample.write_text(source + '\nfunc main() int { system, architecture := target(); '
                      f'if (system != "{system}" || architecture != "{architecture}") return 1; '
                      'return 0; }\n')
    executable = root / ("target.exe" if os.name == "nt" else "target")
    for cache in (str(root / "cache"), "off"):
        subprocess.run([compiler, "build", str(sample), "-o", str(executable)],
                       env=dict(os.environ, OCEAN_BUILD_CACHE=cache), check=True)
        subprocess.run([str(executable)], check=True)

print("Installer platform assembly and native detection checks passed.")
