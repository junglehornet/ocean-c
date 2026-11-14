# Tagged releases

Push a new version tag on the compiler repository to run
`.github/workflows/release.yml`. Create the **same literal tag** on
`junglehornet/ocean-src` first. The workflow checks out that stdlib tag to build
and test the installer; it never silently substitutes the library's main branch.
Use filename-safe tags such as `1.2.3`, `v1.2.3`, or `1.2.3-rc.1`.

The seven native target combinations are:

| OCARCH | OCOS |
| --- | --- |
| x86_64 | linux |
| aarch64 | linux |
| riscv64 | linux |
| x86_64 | macos |
| aarch64 | macos |
| x86_64 | windows |
| aarch64 | windows |

Linux RISC-V runs its build and tests under QEMU. The other targets use native
GitHub-hosted runners. Wasm is excluded. Windows builds use MSYS2's native
CLANG64/CLANGARM64 toolchains; the resulting executables use the Windows CRT,
not an MSYS/Cygwin runtime.

Each target uploads two flat tar archives:

```text
ocean-c_<tag>_<ocarch>-<ocos>.tar
ocean-installer_<tag>_<ocarch>-<ocos>.tar
```

Windows also uploads both archives as `.zip`. Compiler archives contain only
`ocean-c` (`ocean-c.exe` on Windows). Installer archives contain only
`ocean-installer` (`ocean-installer.exe` on Windows). There is no enclosing
folder, helper script, or DLL in either archive. Linux and Windows installer
builds use `--static`; macOS uses the system runtime libraries.

Linux release jobs first run `.github/scripts/build-static-curl.sh` to build a
checksum-pinned static libcurl with OpenSSL and threaded DNS. SSH, GSSAPI, and
other optional dependencies are disabled: Ubuntu does not ship every static
library required by its general-purpose libcurl.a. The helper verifies a fully
static TLS/asynchronous-DNS probe before building Ocean. Only the installer link
uses this private `libcurl.pc`, selected by `RELEASE_STATIC_PKG_CONFIG_PATH`;
normal program tests and dependency bundles still use the system libcurl.

The installer runs entirely as native Ocean code. Each target also uploads
`ocean-deps_<tag>_<arch>-<os>.tar.xz`, a relocatable Clang, linker and library
bundle for installation within the Ocean prefix. Linux includes a C sysroot;
macOS uses Apple's separately installed SDK. Every bundle compiles and executes
a libffi/libcurl probe after relocation before publication.
Linux library collection uses Clang's search result and multiarch directories
when pkg-config reports a generic libdir such as `/usr/lib`. It includes both
developer link names and versioned shared-library files, and fails explicitly
if libffi or libcurl cannot be located. The RISC-V container installs curl and
CA certificates before downloading the pinned installer dependency source.
The ELF interpreter is copied into the sysroot at its absolute runtime path,
including paths such as `/lib64/ld-linux-x86-64.so.2` that glibc linker scripts
reference outside the multiarch directory. macOS jobs install Homebrew's separate
`lld` formula and bundle both `ld.lld` and `ld64.lld`. Mach-O relocation processes
files only, rewrites dylib imports and install IDs, and signs the modified files.
Ubuntu 26.04's RISC-V container explicitly installs `libjitterentropy3-dev` to
satisfy the `libjitterentropy.a` dependency advertised by static OpenSSL.

`scripts/release.py` passes `VERSION`, `COMMIT`, `COMMIT_DATE`, `COMMIT_YEAR`, and
`BUILD_DATE` to the GNUmakefile. It verifies that HEAD is the tagged commit and
that both version flags print those exact values before packaging. Each target
also uploads its build metadata JSON and archive SHA-256 checksums. The installer's
installed `installation.json` takes its version, commit, commit date, and build
date from that verified compiler output, adding the install date and stdlib
commit. Metadata comes from the selected compiler executable, rather than the installer build.
Windows preserves the Git executable used by checkout in `RELEASE_GIT`, so
metadata verification does not switch Git installations after MSYS2 setup.
Tag verification is local: deleting a remote tag after checkout does not remove
the fetched local tag. A missing or mismatched local tag still fails the build.

Every build runs native smoke tests for importing the stdlib, checking, cached
compilation, executable paths containing spaces, execution, 64-bit integer
literals, a unit test, and LSP diagnostics/hover with editor overlays. Windows
uses captured unit-test reports; the live termios frontend is Unix-only.

Publishing waits for **all seven targets**. The publish job checks that every
expected archive exists, creates a draft GitHub release, uploads the artifacts,
and then publishes it. A failing build or smoke test prevents publication. A
rerun replaces that tag's uploaded assets. Only the publish job has
`contents: write`; no release is created by local verification.

To exercise one build locally, check out the desired compiler tag and matching
stdlib tag, install its native dependencies, then run:

```sh
# Linux standalone installers need a complete static libcurl dependency closure:
bash .github/scripts/build-static-curl.sh /tmp/ocean-static-curl
export RELEASE_STATIC_PKG_CONFIG_PATH=/tmp/ocean-static-curl/lib/pkgconfig
python3 scripts/release.py --version 1.2.3 --os linux --arch x86_64 \
  --stdlib /path/to/tagged/ocean-src
```

The script refuses to label one architecture's executable as another target.
A normal development compiler build can supply metadata explicitly too:

```sh
make VERSION=1.2.3 COMMIT=<full-hash> COMMIT_DATE=2026-11-13 \
  COMMIT_YEAR=2026 BUILD_DATE=2026-11-13T12:00:00Z
```

CMake equivalents are `OCEAN_VERSION`, `OCEAN_COMMIT`, `OCEAN_COMMIT_DATE`,
`OCEAN_COMMIT_YEAR`, and `OCEAN_BUILD_DATE`. Empty commit/date overrides use Git;
an empty build-date override uses the build clock (or `SOURCE_DATE_EPOCH`).
