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

The installer runs entirely as native Ocean code. Each target also uploads
`ocean-deps_<tag>_<arch>-<os>.tar.xz`, a relocatable Clang, linker and library
bundle for installation within the Ocean prefix. Linux includes a C sysroot;
macOS uses Apple's separately installed SDK. Every bundle compiles and executes
a libffi/libcurl probe after relocation before publication.

`scripts/release.py` passes `VERSION`, `COMMIT`, `COMMIT_DATE`, `COMMIT_YEAR`, and
`BUILD_DATE` to the GNUmakefile. It verifies that HEAD is the tagged commit and
that both version flags print those exact values before packaging. Each target
also uploads its build metadata JSON and archive SHA-256 checksums. The installer's
installed `installation.json` takes its version, commit, commit date, and build
date from that verified compiler output, adding the install date and stdlib
commit. Metadata comes from the selected compiler executable, rather than the installer build.

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
