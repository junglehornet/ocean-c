"""Release asset naming, contents, checksums, and tag metadata without CI access."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

spec = importlib.util.spec_from_file_location("release", Path(__file__).resolve().parents[1] / "scripts/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
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
