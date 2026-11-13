# Ocean installer

Build the native installer with the matching compiler and stdlib:

```sh
OCEAN_SRC=/path/to/ocean-src build/ocean build installer -o ocean-installer
```

The executable contains the installer logic in Ocean. No Python runtime or helper
is required. Git is needed to inspect tags and clone the matching stdlib; tar is
needed when selecting a dependency bundle. HTTPS uses verified libcurl transport.

The installer detects its compiled OS and architecture, lists release tags merged
into the compiler's main branch with matching stdlib tags, and offers latest
(stable preferred) or an explicit version. Compiler downloads use
`ocean-c_<tag>_<arch>-<os>.tar`. Temporary downloads and clones are deleted after
success, cancellation, or failure. Compiler release metadata is checked against
the selected tag and recorded in `installation.json`, including version, compiler
commit/date, build date, installation date, and stdlib commit.

Choose a system directory (`/usr/local/lib/ocean` on Unix), a user directory
(`~/.local/lib/ocean`), or an absolute custom directory. Windows defaults are
Program Files/Ocean and LocalAppData/Ocean. The installer checks permissions and
requests sudo or Windows elevation when required. Existing installations display
their metadata (or unknown version) and require explicit removal confirmation.
Installation and shell PATH changes use a backup and rollback on failure.

Dependency options are skip, system package manager, or a matching release
bundle installed within the chosen Ocean directory. The bundle provides Clang,
linker, headers and libraries including libffi and libcurl, and supports a
user-local installation without administrator access. Linux bundles include a C
sysroot. macOS needs Apple's Command Line Tools SDK; setup can launch Apple's SDK
installer and asks you to rerun when it finishes. System package installation
uses apt, dnf, pacman, zypper, apk, Homebrew, or MSYS2/winget as appropriate and
requires explicit consent. Package-manager changes cannot be rolled back.

PATH configuration supports Bash, Zsh, Fish, csh and POSIX shells on Unix, and
the Windows user or machine environment. Open a new shell after installation.

Release archives contain only `ocean-installer` (`ocean-installer.exe` on Windows).
Dependency bundles are separate `ocean-deps_<tag>_<arch>-<os>.tar.xz` assets.
All seven native platforms are supported; Wasm is excluded.

Validation:

```sh
OCEAN_SRC=/path/to/ocean-src build/ocean test installer --notty
python3 installer/test_native_installer.py ./ocean-installer
```

Python is used only by the offline integration test driver, which creates local
repositories and a loopback HTTP fixture. It never installs system packages.
