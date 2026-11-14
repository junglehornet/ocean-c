"""Exercise ELF loader paths and real Mach-O dylib relocation without macOS."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("bundle", Path(__file__).resolve().parents[1] / "scripts/dependency_bundle.py")
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class BundleTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("patchelf") and shutil.which("ld.lld"), "ELF tools required")
    def test_loader_runtime_path_satisfies_glibc_linker_script(self):
        clang = shutil.which("clang")
        interpreter = Path(bundle.output("patchelf", "--print-interpreter", clang))
        with tempfile.TemporaryDirectory(prefix="ocean-loader-") as directory:
            root = Path(directory)
            script = root / "libc.so"
            script.write_text(f"GROUP ( AS_NEEDED ( {interpreter} ) )\n")
            command = ["ld.lld", "--sysroot=" + str(root), "-shared", str(script), "-o", str(root / "probe.so")]
            before = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(before.returncode, 0)
            bundle.copy_linux_interpreter(clang, root)
            self.assertEqual((root / interpreter.relative_to("/")).read_bytes(), interpreter.read_bytes())
            subprocess.run(command, check=True)

    @unittest.skipUnless(shutil.which("clang") and shutil.which("ld64.lld") and shutil.which("llvm-otool") and shutil.which("llvm-install-name-tool"), "LLVM Mach-O tools required")
    def test_real_macho_ids_imports_and_resource_directories(self):
        native_run = subprocess.run
        native_output = subprocess.check_output
        with tempfile.TemporaryDirectory(prefix="ocean-macho-") as directory:
            base = Path(directory)
            root = base / "bundle"
            (root / "bin").mkdir(parents=True)
            (root / "lib/clang/23/include").mkdir(parents=True)
            origin = base / "brew/opt/dependency/lib"
            origin.mkdir(parents=True)
            leaf = origin / "libLeaf.dylib"
            library = root / "lib/libffi.dylib"
            for arch in ("x86_64", "arm64"):
                for name, body in (("leaf", "int leaf(void) { return 7; }"),
                                   ("ffi", "extern int leaf(void); int (*ffi)(void) = leaf;")):
                    source = base / (name + ".c")
                    source.write_text(body)
                    native_run(["clang", "--target=" + arch + "-apple-macosx11.0", "-c", str(source), "-o", str(base / (name + ".o"))], check=True)
                flags = ["ld64.lld", "-dylib", "-arch", arch, "-platform_version", "macos", "11.0", "11.0", "-headerpad_max_install_names"]
                native_run([*flags, "-install_name", "@rpath/libLeaf.dylib", str(base / "leaf.o"), "-o", str(leaf)], check=True)
                native_run([*flags, "-install_name", "/homebrew/lib/libffi.dylib", str(base / "ffi.o"), str(leaf), "-o", str(library)], check=True)
                copied_leaf = root / "lib/libLeaf.dylib"
                if copied_leaf.exists():
                    copied_leaf.unlink()
                signed = []

                def query(tool, *args):
                    self.assertEqual(tool, "otool")
                    self.assertTrue(Path(args[-1]).is_file(), args)
                    return native_output(["llvm-otool", *args], text=True).strip()

                def execute(command, **kwargs):
                    if command[0] == "codesign":
                        signed.append(Path(command[-1]))
                        return subprocess.CompletedProcess(command, 0)
                    if command[0] == "install_name_tool":
                        command = ["llvm-install-name-tool", *command[1:]]
                    return native_run(command, **kwargs)

                with mock.patch.object(bundle, "output", side_effect=query), mock.patch.object(bundle.subprocess, "run", side_effect=execute):
                    bundle.relocate_macos(root, base / "llvm/bin/clang", base / "brew")
                for file in (library, copied_leaf):
                    self.assertIn("@rpath/" + file.name, native_output(["llvm-otool", "-D", str(file)], text=True))
                    self.assertIn("@loader_path", native_output(["llvm-otool", "-l", str(file)], text=True))
                    self.assertIn(file, signed)
                imports = native_output(["llvm-otool", "-L", str(library)], text=True)
                self.assertIn("@rpath/libLeaf.dylib", imports)
                self.assertNotIn("/homebrew/", imports)


if __name__ == "__main__":
    unittest.main()
