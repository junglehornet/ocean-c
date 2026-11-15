#!/usr/bin/env python3
"""Tag matching main snapshots when a nightly is due; builds run via workflow_call."""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import re
import subprocess

UTC = dt.timezone.utc
NIGHTLY = re.compile(r"nightly-\d{4}\.\d{2}\.\d{2}\Z")


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()


def revision(repo, ref):
    result = subprocess.run(["git", "-C", str(repo), "rev-parse", "--verify", ref + "^{commit}"],
                            text=True, capture_output=True)
    return result.stdout.strip() if result.returncode == 0 else None


def latest_nightly(releases):
    candidates = [release for release in releases if not release["draft"]
                  and release.get("published_at")
                  and (NIGHTLY.fullmatch(release["tag_name"]) or release["tag_name"] == "nightly")]
    return max(candidates, key=lambda release: release["published_at"], default=None)


def reason_to_build(now, compiler, stdlib, previous, previous_compiler, previous_stdlib):
    if previous is None:
        return "no published nightly"
    if (compiler, stdlib) != (previous_compiler, previous_stdlib):
        return "main changed"
    published = dt.datetime.fromisoformat(previous["published_at"].replace("Z", "+00:00"))
    if now - published >= dt.timedelta(days=30):
        return "30 days since the last published nightly"
    return None


def ensure_tag(repo, tag, commit, snapshot):
    existing = revision(repo, "refs/tags/" + tag)
    if existing and existing != commit:
        raise ValueError(f"{repo}: {tag} already points to a different commit; refusing to move it")
    if not existing:
        git(repo, "-c", "user.name=github-actions[bot]", "-c",
            "user.email=41898282+github-actions[bot]@users.noreply.github.com",
            "tag", "-a", tag, commit, "-m", json.dumps(snapshot, sort_keys=True))
    git(repo, "push", "origin", f"refs/tags/{tag}:refs/tags/{tag}")


def prepare(compiler, stdlib, now, releases):
    tag = "nightly-" + now.astimezone(UTC).strftime("%Y.%m.%d")
    if any(release["tag_name"] == tag and not release["draft"] for release in releases):
        return None, "today's nightly is already published"
    snapshot = dict(compiler=revision(compiler, "refs/remotes/origin/main"),
                    stdlib=revision(stdlib, "refs/remotes/origin/main"))
    if not all(snapshot.values()):
        raise ValueError("Both repositories must have origin/main")
    # The stdlib tag is pushed first and stores both snapshots. Recover that
    # pair after an interrupted push/build, even if main has since advanced.
    if revision(stdlib, "refs/tags/" + tag):
        saved = json.loads(git(stdlib, "for-each-ref", "--format=%(contents)", "refs/tags/" + tag))
        if set(saved) != {"compiler", "stdlib"} or saved["stdlib"] != revision(stdlib, "refs/tags/" + tag):
            raise ValueError("Existing nightly tag has invalid snapshot metadata")
        snapshot = saved
        reason = "retry today's tagged snapshots"
    else:
        previous = latest_nightly(releases)
        old_tag = "refs/tags/" + previous["tag_name"] if previous else ""
        reason = reason_to_build(now, snapshot["compiler"], snapshot["stdlib"], previous,
                                 revision(compiler, old_tag) if previous else None,
                                 revision(stdlib, old_tag) if previous else None)
        if reason is None:
            return None, "both main branches unchanged; last nightly is less than 30 days old"
    ensure_tag(stdlib, tag, snapshot["stdlib"], snapshot)
    ensure_tag(compiler, tag, snapshot["compiler"], snapshot)
    return tag, reason


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stdlib", type=Path, required=True)
    args = parser.parse_args()
    pages = json.loads(subprocess.check_output(
        ["gh", "api", "--paginate", "--slurp", f"repos/{os.environ['GITHUB_REPOSITORY']}/releases?per_page=100"],
        text=True))
    tag, reason = prepare(Path.cwd(), args.stdlib.resolve(), dt.datetime.now(UTC),
                          [release for page in pages for release in page])
    print(reason, flush=True)
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write(f"build={'true' if tag else 'false'}\ntag={tag or ''}\n")
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
        summary.write(f"Nightly: {tag or 'skipped'} — {reason}\n")


if __name__ == "__main__":
    main()
