"""Nightly change detection and real paired Git tags, without GitHub writes."""
import datetime as dt
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("nightly", Path(__file__).resolve().parents[1] / "scripts/nightly.py")
nightly = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nightly)
UTC = dt.timezone.utc
NOW = dt.datetime(2026, 11, 15, tzinfo=UTC)


def published(tag, when):
    return dict(tag_name=tag, draft=False, published_at=when.isoformat())


class NightlyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ocean nightly ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.compiler = self.repository("compiler")
        self.stdlib = self.repository("stdlib")

    def repository(self, name):
        remote = self.root / (name + ".git")
        repo = self.root / name
        subprocess.run(["git", "init", "--bare", str(remote)], check=True, capture_output=True)
        subprocess.run(["git", "init", "-b", "main", str(repo)], check=True, capture_output=True)
        nightly.git(repo, "config", "user.name", "Nightly Fixture")
        nightly.git(repo, "config", "user.email", "fixture@example.invalid")
        nightly.git(repo, "remote", "add", "origin", str(remote))
        self.change(repo)
        return repo

    def change(self, repo):
        nightly.git(repo, "commit", "--allow-empty", "-m", "Main update")
        nightly.git(repo, "push", "origin", "main")
        return nightly.revision(repo, "HEAD")

    def prepare(self, now=NOW, releases=()):
        return nightly.prepare(self.compiler, self.stdlib, now, releases)

    def test_first_build_creates_matching_annotated_tags_at_main(self):
        tag, reason = self.prepare()
        self.assertEqual(tag, "nightly-2026.11.15")
        self.assertEqual(reason, "no published nightly")
        for repo in (self.compiler, self.stdlib):
            self.assertEqual(nightly.revision(repo, "refs/tags/" + tag), nightly.revision(repo, "main"))
            remote = nightly.git(repo, "ls-remote", "origin", f"refs/tags/{tag}^{{}}")
            self.assertEqual(remote.split()[0], nightly.revision(repo, "main"))
            metadata = json.loads(nightly.git(repo, "for-each-ref", "--format=%(contents)", "refs/tags/" + tag))
            self.assertEqual(metadata["compiler"], nightly.revision(self.compiler, "main"))
            self.assertEqual(metadata["stdlib"], nightly.revision(self.stdlib, "main"))

    def test_unchanged_sources_skip_without_creating_tomorrows_tags(self):
        tag, _ = self.prepare()
        next_day = NOW + dt.timedelta(days=1)
        result, _ = self.prepare(next_day, [published(tag, NOW)])
        self.assertIsNone(result)
        for repo in (self.compiler, self.stdlib):
            self.assertIsNone(nightly.revision(repo, "refs/tags/nightly-2026.11.16"))

    def test_either_main_change_triggers_a_build(self):
        tag, _ = self.prepare()
        self.change(self.stdlib)
        source_tag, reason = self.prepare(NOW + dt.timedelta(days=1), [published(tag, NOW)])
        self.assertEqual(reason, "main changed")
        self.assertEqual(nightly.revision(self.compiler, "refs/tags/" + source_tag),
                         nightly.revision(self.compiler, "refs/tags/" + tag))
        self.change(self.compiler)
        next_tag, reason = self.prepare(NOW + dt.timedelta(days=2),
                                        [published(source_tag, NOW + dt.timedelta(days=1))])
        self.assertEqual(next_tag, "nightly-2026.11.17")
        self.assertEqual(reason, "main changed")

    def test_thirty_day_boundary_rebuilds_identical_commits(self):
        tag, _ = self.prepare()
        releases = [published(tag, NOW)]
        self.assertIsNone(self.prepare(NOW + dt.timedelta(days=30, seconds=-1), releases)[0])
        new_tag, reason = self.prepare(NOW + dt.timedelta(days=30), releases)
        self.assertIn("30 days", reason)
        for repo in (self.compiler, self.stdlib):
            self.assertEqual(nightly.revision(repo, "refs/tags/" + tag),
                             nightly.revision(repo, "refs/tags/" + new_tag))

    def test_retry_keeps_snapshots_after_main_moves(self):
        ensure = nightly.ensure_tag
        old_commit = nightly.revision(self.compiler, "main")
        def interrupted(repo, *args):
            if repo == self.compiler:
                raise RuntimeError("interrupted between tag pushes")
            return ensure(repo, *args)
        with mock.patch.object(nightly, "ensure_tag", side_effect=interrupted):
            with self.assertRaises(RuntimeError):
                self.prepare()
        self.change(self.compiler)
        tag, reason = self.prepare()
        self.assertIn("retry", reason)
        self.assertEqual(nightly.revision(self.compiler, "refs/tags/" + tag), old_commit)
        self.assertNotEqual(old_commit, nightly.revision(self.compiler, "main"))
        self.assertIsNone(self.prepare(releases=[published(tag, NOW)])[0])

    def test_conflicting_tag_is_never_moved(self):
        snapshot = dict(compiler=nightly.revision(self.compiler, "main"),
                        stdlib=nightly.revision(self.stdlib, "main"))
        nightly.ensure_tag(self.compiler, "nightly-2026.11.15", snapshot["compiler"], snapshot)
        new_commit = self.change(self.compiler)
        with self.assertRaisesRegex(ValueError, "refusing to move"):
            nightly.ensure_tag(self.compiler, "nightly-2026.11.15", new_commit, snapshot)
        self.assertEqual(nightly.revision(self.compiler, "refs/tags/nightly-2026.11.15"), snapshot["compiler"])

    def test_only_published_nightly_releases_count(self):
        old = published("nightly-2026.10.01", NOW - dt.timedelta(days=45))
        stable = published("1.0.0", NOW)
        draft = dict(published("nightly-2026.11.15", NOW), draft=True)
        self.assertEqual(nightly.latest_nightly([old, stable, draft]), old)
        self.assertIsNone(nightly.latest_nightly([stable, draft]))


if __name__ == "__main__":
    unittest.main()
