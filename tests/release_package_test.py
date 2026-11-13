"""Release asset naming, contents, checksums, and tag metadata without CI access."""
import importlib.util
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
