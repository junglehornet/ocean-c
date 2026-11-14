"""Release asset naming, contents, checksums, and tag metadata without CI access."""
import argparse
import hashlib
import importlib.util
import lzma
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

spec = importlib.util.spec_from_file_location("release", Path(__file__).resolve().parents[1] / "scripts/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
import dependency_bundle


class ReleaseTests(unittest.TestCase):
    def test_linux_bundle_finds_multiarch_libraries_with_generic_pkgconfig_libdir(self):
        for machine in ("x86_64-linux-gnu", "aarch64-linux-gnu", "riscv64-linux-gnu"):
            with self.subTest(machine=machine), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                libdir = root / "usr/lib"
                multiarch = libdir / machine
                multiarch.mkdir(parents=True)
                for name in ("libffi.so", "libffi.so.8"):
                    (multiarch / name).write_bytes(b"shared-library fixture")
                with mock.patch.object(dependency_bundle, "output", side_effect=[str(libdir), "libffi.so"]):
                    files = dependency_bundle.linux_shared_libraries("libffi", "libffi", "clang", machine)
                self.assertEqual({file.name for file in files}, {"libffi.so", "libffi.so.8"})
                self.assertTrue(all(file.parent == multiarch for file in files))
                for file in files:
                    dependency_bundle.copy(file, root / "bundle/lib" / file.name)
                self.assertEqual((root / "bundle/lib/libffi.so").read_bytes(), b"shared-library fixture")

    def test_linux_bundle_uses_driver_lookup_and_rejects_missing_libraries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = root / "libfixture.so"
            library.write_bytes(b"driver-selected library")
            with mock.patch.object(dependency_bundle, "output", side_effect=[str(root / "missing"), str(library)]):
                self.assertEqual(dependency_bundle.linux_shared_libraries(
                    "fixture", "libfixture", "clang", "fixture-linux-gnu"), [library])
            with mock.patch.object(dependency_bundle, "output", side_effect=[str(root / "missing"), "libmissing.so"]):
                with self.assertRaisesRegex(RuntimeError, "Cannot locate shared fixture libraries"):
                    dependency_bundle.linux_shared_libraries("fixture", "libmissing", "clang", "fixture-linux-gnu")

    @unittest.skipUnless(shutil.which("xz"), "xz required")
    def test_host_finalization_preserves_bundle_and_checksums_final_assets(self):
        with tempfile.TemporaryDirectory(prefix="ocean host packaging ") as directory:
            root = Path(directory)
            binary = root / "executable"
            binary.write_bytes(b"release executable")
            for project in ("ocean-c", "ocean-installer"):
                release.package(binary, root, project, "nightly", "riscv64", "linux")
            source = root / "ocean-deps_nightly_riscv64-linux.tar"
            with tarfile.open(source, "w") as archive:
                archive.add(binary, arcname="bin/clang")
            original = source.read_bytes()
            subprocess.run([sys.executable, release.__file__, "--version", "nightly",
                            "--arch", "riscv64", "--os", "linux", "--output", str(root),
                            "--finalize-dependency-compression"], check=True)
            self.assertFalse(source.exists())
            archive = source.with_suffix(".tar.xz")
            self.assertEqual(lzma.decompress(archive.read_bytes()), original)
            with tarfile.open(archive) as stream:
                self.assertEqual(stream.extractfile("bin/clang").read(), binary.read_bytes())
            lines = (root / "SHA256SUMS_riscv64-linux.txt").read_text().splitlines()
            self.assertEqual(len(lines), 3)
            for line in lines:
                digest, name = line.split("  ")
                self.assertEqual(digest, hashlib.sha256((root / name).read_bytes()).hexdigest())
                self.assertFalse(name.endswith("-linux.tar") and name.startswith("ocean-deps"))

    def test_failed_host_compression_keeps_input_and_removes_partial_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "ocean-deps_nightly_riscv64-linux.tar"
            source.write_bytes(b"verified dependency tar")
            for project in ("ocean-c", "ocean-installer"):
                (root / f"{project}_nightly_riscv64-linux.tar").write_bytes(b"compiler")
            args = argparse.Namespace(output=root, version="nightly", arch="riscv64", os="linux")
            with mock.patch.object(release.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "xz")):
                with self.assertRaises(subprocess.CalledProcessError):
                    release.finalize_dependency_compression(args)
            self.assertTrue(source.is_file())
            self.assertFalse(source.with_suffix(".tar.xz.tmp").exists())
            self.assertFalse((root / "SHA256SUMS_riscv64-linux.txt").exists())

    def test_each_supported_target_has_a_flat_archive(self):
        self.assertEqual(len(release.TARGETS), 7)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "input"
            binary.write_bytes(b"executable fixture")
            for system, arch in release.TARGETS:
                for project in ("ocean-c", "ocean-installer"):
                    archives = release.package(binary, root / "dist", project, "v1.2.3", arch, system)
                    self.assertEqual(len(archives), 2 if system == "windows" else 1)
                    name = project + (".exe" if system == "windows" else "")
                    for path in archives:
                        self.assertEqual(path.stem, f"{project}_v1.2.3_{arch}-{system}")
                        if path.suffix == ".tar":
                            with tarfile.open(path) as archive:
                                self.assertEqual(archive.getnames(), [name])
                                self.assertEqual(archive.extractfile(name).read(), binary.read_bytes())
                                self.assertEqual(archive.getmember(name).mode, 0o755)
                        else:
                            with zipfile.ZipFile(path) as archive:
                                self.assertEqual(archive.namelist(), [name])
                                self.assertEqual(archive.read(name), binary.read_bytes())

    def test_metadata_requires_tagged_head_and_carries_commit_fields(self):
        with mock.patch.object(release, "run", side_effect=["a" * 40, "a" * 40, "2026-11-12", "2026"]):
            data = release.metadata("v1.2.3")
        self.assertEqual(data["VERSION"], "v1.2.3")
        self.assertEqual(data["COMMIT"], "a" * 40)
        self.assertEqual(data["COMMIT_DATE"], "2026-11-12")
        self.assertEqual(data["COMMIT_YEAR"], "2026")
        self.assertTrue(data["BUILD_DATE"].endswith("Z"))
        with mock.patch.object(release, "run", side_effect=["a" * 40, "b" * 40]), self.assertRaises(ValueError):
            release.metadata("v1.2.3")
        for tag in ("../../escape", "tag with spaces", "", "-option"):
            with self.assertRaises(ValueError):
                release.metadata(tag)

    def test_real_tags_survive_remote_deletion_and_mismatches_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            origin = Path(directory) / "origin"
            checkout = Path(directory) / "checkout"
            executable = shutil.which("git")

            def git(*args, cwd=origin):
                return subprocess.check_output([executable, *args], cwd=cwd,
                                               text=True, stderr=subprocess.DEVNULL).strip()

            origin.mkdir()
            git("init")
            git("config", "user.name", "Release Fixture")
            git("config", "user.email", "fixture@example.invalid")
            git("commit", "--allow-empty", "-m", "First")
            git("tag", "lightweight")
            git("tag", "-a", "annotated", "-m", "Annotated release")
            git("clone", str(origin), str(checkout), cwd=Path(directory))
            git("tag", "-d", "lightweight", "annotated")

            def run(*args):
                return subprocess.check_output(args, cwd=checkout, text=True,
                                               stderr=subprocess.DEVNULL).strip()

            with mock.patch.dict(os.environ, RELEASE_GIT=executable), \
                    mock.patch.object(release, "run", side_effect=run) as invoke:
                for tag in ("lightweight", "annotated"):
                    self.assertEqual(release.metadata(tag)["COMMIT"], git("rev-parse", "HEAD"))
                self.assertTrue(all(call.args[0] == executable for call in invoke.call_args_list))
                with self.assertRaisesRegex(ValueError, "not resolvable in the local checkout"):
                    release.metadata("missing")
                git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                    "commit", "--allow-empty", "-m", "Second", cwd=checkout)
                with self.assertRaisesRegex(ValueError, "HEAD does not match"):
                    release.metadata("annotated")


if __name__ == "__main__":
    unittest.main()
