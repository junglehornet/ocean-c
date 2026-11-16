"""Metadata refresh and stable header timestamps without release-time subprocesses."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'cmake/BuildInfo.cmake'
CMAKE = shutil.which('cmake')


class BuildInfoTests(unittest.TestCase):
    def test_supplied_metadata_needs_no_git_and_preserves_unchanged_timestamp(self):
        with tempfile.TemporaryDirectory(prefix='ocean build info ') as directory:
            root = Path(directory)
            tools = root / 'tools'
            tools.mkdir()
            # Git discovery is unnecessary with explicit release commit fields.
            fake_git = tools / ('git.cmd' if os.name == 'nt' else 'git')
            marker = root / 'git-was-called'
            if os.name == 'nt':
                fake_git.write_text(f'@echo called > "{marker}"\nexit /b 1\n')
            else:
                fake_git.write_text(f'#!/bin/sh\ntouch "{marker}"\nexit 1\n')
                fake_git.chmod(0o755)
            env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ['PATH'])
            command = [CMAKE, '-DOCEAN_VERSION=nightly-2026.11.15',
                       '-DOCEAN_COMMIT=' + 'a' * 40, '-DOCEAN_COMMIT_DATE=2026-11-15',
                       '-DOCEAN_COMMIT_YEAR=2026', '-DOCEAN_BUILD_DATE=2026-11-16T00:00:00Z',
                       '-DOCEAN_SOURCE_DIR=' + str(root), '-DOCEAN_BINARY_DIR=' + str(root),
                       '-DCMAKE_COMMAND=' + str(root / 'no-nested-cmake'), '-P', str(SCRIPT)]
            subprocess.run(command, env=env, check=True)
            header = root / 'ocean_build_info.h'
            self.assertIn('nightly-2026.11.15', header.read_text())
            self.assertFalse(marker.exists())
            os.utime(header, (1000, 1000))
            stamp = header.stat().st_mtime_ns
            subprocess.run(command, env=env, check=True)
            self.assertEqual(header.stat().st_mtime_ns, stamp)
            command[1] = '-DOCEAN_VERSION=nightly-2026.11.16'
            subprocess.run(command, env=env, check=True)
            self.assertIn('nightly-2026.11.16', header.read_text())
            self.assertNotEqual(header.stat().st_mtime_ns, stamp)
            self.assertFalse((root / 'ocean_build_info.h.tmp').exists())

    def test_missing_metadata_still_discovers_git(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for args in (['init'], ['-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                                    'commit', '--allow-empty', '-m', 'Fixture']):
                subprocess.run(['git', '-C', str(root), *args], check=True, capture_output=True)
            commit = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
            subprocess.run([CMAKE, '-DOCEAN_VERSION=dev', '-DOCEAN_SOURCE_DIR=' + str(root),
                            '-DOCEAN_BINARY_DIR=' + str(root), '-P', str(SCRIPT)], check=True)
            self.assertIn(commit, (root / 'ocean_build_info.h').read_text())


if __name__ == '__main__':
    unittest.main()
