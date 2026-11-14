#!/usr/bin/env python3
"""Build and verify one native release target, then create flat archives."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import tarfile
import zipfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dependency_bundle import build_bundle

TARGETS = {("linux", "x86_64"), ("linux", "aarch64"), ("linux", "riscv64"),
           ("macos", "x86_64"), ("macos", "aarch64"),
           ("windows", "x86_64"), ("windows", "aarch64")}


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def git(*args):
    return run(os.environ.get("RELEASE_GIT", "git"), *args)


def metadata(version):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", version):
        raise ValueError("Release tags must be filename-safe version strings.")
    commit = git("rev-parse", "HEAD")
    # Resolve annotated and lightweight tags without caret revision syntax.
    # A remote deletion cannot remove a tag already fetched into this checkout.
    try:
        tag_commit = git("log", "-1", "--format=%H", f"refs/tags/{version}", "--")
    except subprocess.CalledProcessError as error:
        raise ValueError(f"Release tag {version!r} is not resolvable in the local checkout; "
                         "verify the fetched tag and RELEASE_GIT executable.") from error
    if commit != tag_commit:
        raise ValueError("Checkout HEAD does not match the release tag.")
    return dict(VERSION=version, COMMIT=commit,
                COMMIT_DATE=git("show", "-s", "--format=%cs", commit),
                COMMIT_YEAR=git("show", "-s", "--format=%cd", "--date=format:%Y", commit),
                BUILD_DATE=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))


def package(binary, output, project, version, arch, system):
    """Exactly one root-level executable in every archive, including Windows ZIPs."""
    output.mkdir(parents=True, exist_ok=True)
    suffix = ".exe" if system == "windows" else ""
    name = project + suffix
    stem = f"{project}_{version}_{arch}-{system}"
    paths = [output / (stem + ".tar")]
    with tarfile.open(paths[0], "w", format=tarfile.PAX_FORMAT) as stream:
        info = stream.gettarinfo(binary, arcname=name)
        info.mode = 0o755
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        with binary.open("rb") as file:
            stream.addfile(info, file)
    if system == "windows":
        paths.append(output / (stem + ".zip"))
        with zipfile.ZipFile(paths[-1], "w", zipfile.ZIP_DEFLATED) as stream:
            stream.write(binary, arcname=name)
    # Treat archive layout as a release invariant, not a convention.
    with tarfile.open(paths[0]) as stream:
        assert stream.getnames() == [name]
    if system == "windows":
        with zipfile.ZipFile(paths[-1]) as stream:
            assert stream.namelist() == [name]
    return paths


def write_checksums(output, arch, system, paths):
    lines = []
    for path in paths:
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        lines.append(f"{digest}  {path.name}\n")
    checksum = output / f"SHA256SUMS_{arch}-{system}.txt"
    checksum.write_text("".join(lines), encoding="utf-8")


def finalize_dependency_compression(args):
    """Compress an already verified dependency tar on the native runner CPU."""
    source = args.output / f"ocean-deps_{args.version}_{args.arch}-{args.os}.tar"
    archive = source.with_suffix(".tar.xz")
    temporary = archive.with_suffix(".xz.tmp")
    paths = []
    for project in ("ocean-c", "ocean-installer"):
        for extension in (("tar", "zip") if args.os == "windows" else ("tar",)):
            path = args.output / f"{project}_{args.version}_{args.arch}-{args.os}.{extension}"
            if not path.is_file():
                raise FileNotFoundError(path)
            paths.append(path)
    try:
        print(f"Compressing verified dependency bundle: {source}", flush=True)
        with temporary.open("wb") as stream:
            subprocess.run(["xz", "--threads=2", "--stdout", str(source)],
                           stdout=stream, check=True)
        temporary.replace(archive)
        write_checksums(args.output, args.arch, args.os, [*paths, archive])
        source.unlink()
    finally:
        temporary.unlink(missing_ok=True)


def build(args):
    actual_os = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}[platform.system()]
    actual_arch = {"AMD64": "x86_64", "arm64": "aarch64", "ARM64": "aarch64"}.get(platform.machine(), platform.machine())
    if (args.os, args.arch) not in TARGETS or (actual_os, actual_arch) != (args.os, args.arch):
        raise ValueError(f"Release must be built natively for {args.arch}-{args.os}; host is {actual_arch}-{actual_os}.")
    info = metadata(args.version)
    environment = dict(os.environ, **info, OCEAN_SRC=str(args.stdlib.resolve()))
    cmake_args = ["-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF", *args.cmake_arg]
    environment["CMAKE_ARGS"] = shlex.join(cmake_args)
    # Passing these as make arguments keeps the public Make interface exercised.
    subprocess.run(["make", f"BUILD_DIR={args.build_dir}",
                    *(f"{key}={value}" for key, value in info.items()), "ocean"],
                   env=environment, check=True)
    extension = ".exe" if args.os == "windows" else ""
    compiler = args.build_dir.resolve() / ("ocean-c" + extension)
    expected = (f"Ocean compiler (ocean-c) {args.version}\n"
                f"Commit {info['COMMIT']} ({info['COMMIT_DATE']}), built {info['BUILD_DATE']}\n"
                f"Copyright Connor Egan {info['COMMIT_YEAR']}")
    assert run(str(compiler), "--version") == expected
    assert run(str(compiler), "-v") == expected
    installer = args.build_dir.resolve() / ("ocean-installer" + extension)
    flags = ["--static"] if args.os in ("linux", "windows") else []
    installer_environment = dict(environment)
    if args.os == "linux" and os.environ.get("RELEASE_STATIC_PKG_CONFIG_PATH"):
        installer_environment["PKG_CONFIG_PATH"] = os.environ["RELEASE_STATIC_PKG_CONFIG_PATH"] + (
            os.pathsep + environment["PKG_CONFIG_PATH"] if environment.get("PKG_CONFIG_PATH") else "")
    subprocess.run([str(compiler), "build", *flags, "installer", "-o", str(installer)], env=installer_environment, check=True)
    reported_platform = run(str(installer), "--platform")
    assert reported_platform == f"{args.arch}-{args.os}", f"Unexpected installer platform: {reported_platform!r}"
    subprocess.run([str(installer), "--help"], check=True)
    subprocess.run([str(compiler), "test", "installer", "--notty"], env=environment, check=True)
    subprocess.run([str(compiler), "test", str(args.stdlib.resolve() / "os/exec"), "--notty"], env=environment, check=True)
    if args.os == "linux":
        subprocess.run([os.environ.get("RELEASE_PYTHON", "python3"), "installer/test_native_installer.py", str(installer)], env=environment, check=True)
    subprocess.run([os.environ.get("RELEASE_PYTHON", "python3"), "tests/release_smoke_test.py", str(compiler)],
                   env=environment, check=True)
    paths = package(compiler, args.output, "ocean-c", args.version, args.arch, args.os)
    paths += package(installer, args.output, "ocean-installer", args.version, args.arch, args.os)
    paths.append(build_bundle(args.output, args.version, args.arch, args.os,
                              compress=not args.defer_dependency_compression))
    manifest = args.output / f"build_{args.version}_{args.arch}-{args.os}.json"
    manifest.write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    if not args.defer_dependency_compression:
        write_checksums(args.output, args.arch, args.os, paths)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--os", required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--stdlib", type=Path, default=Path("../ocean-src"))
    parser.add_argument("--build-dir", type=Path, default=Path("build-release"))
    parser.add_argument("--output", type=Path, default=Path("dist"))
    parser.add_argument("--cmake-arg", action="append", default=[])
    compression = parser.add_mutually_exclusive_group()
    compression.add_argument("--defer-dependency-compression", action="store_true",
                             help="write a verified dependency tar for host-side compression")
    compression.add_argument("--finalize-dependency-compression", action="store_true",
                             help="compress the verified dependency tar and write final checksums")
    args = parser.parse_args()
    if args.finalize_dependency_compression:
        finalize_dependency_compression(args)
    else:
        build(args)
