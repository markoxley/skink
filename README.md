# skink

The self-hosted Skink compiler, written in Skink itself. This is the canonical
compiler toolset — no Go or other bootstrapping language is required once the
initial binary is installed.

- **Website:** <https://skink-lang.uk> — docs, StdLib reference, package index
- **Book:** [*Programming in Skink*](https://www.amazon.co.uk/Programming-Skink-Developer-Library-Book-ebook/dp/B0HMX1GWRW/ref=sr_1_1?crid=19T1IMU1A9QCS&dib=eyJ2IjoiMSJ9.KCfcTtEJvU5yXvJ988OkKQ.61Z7KR6fvfPDhMDZMtbestlhCLjtkcWK2mC_BGOxOb8&dib_tag=se&keywords=skink+programming&qid=1793730746&sprefix=skink+programming%2Caps%2C225&sr=8-1) (Amazon UK)

## Quick Install

Install the latest release from GitHub:

```bash
curl -fsSL https://raw.githubusercontent.com/markoxley/skink/main/install.sh | bash
```

This downloads the `skink` compiler, `skink-lsp` language server, `skinkfmt`
formatter, `scales` package manager, `skinkjs` JavaScript transpiler,
`skinkcov` coverage reporter, and the standard library to `~/.local/skink/`,
then updates your shell config (`.bashrc`, `.zshrc`, etc.) to add them to
`PATH` and set `SKINK_HOME`.

The installer detects your OS and CPU architecture automatically and fetches
the matching archive — **x86-64** and **arm64** (aarch64) are both supported
on Linux and macOS, including 64-bit Raspberry Pi OS. (32-bit ARM is not
supported.)

Install a specific version:

```bash
curl -fsSL https://raw.githubusercontent.com/markoxley/skink/main/install.sh | bash -s -- --version 0.2.0
```

Install to a custom prefix:

```bash
curl -fsSL https://raw.githubusercontent.com/markoxley/skink/main/install.sh | bash -s -- --prefix /opt/skink
```

Skip shell config updates:

```bash
curl -fsSL https://raw.githubusercontent.com/markoxley/skink/main/install.sh | bash -s -- --no-shell-rc
```

After installing, restart your shell (or `source ~/.bashrc`) and verify:

```bash
skink -v
```

### Windows

Windows is distributed as ZIP archives rather than a curl installer. Download
`skink-<version>-windows-x86_64.zip` (or `skink-<version>-windows-aarch64.zip`
on ARM64) from the [releases page](https://github.com/markoxley/skink/releases),
extract it, and run the bundled PowerShell installer:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1
```

Or install to a custom directory:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1 -InstallDir "C:\Skink"
```

The installer copies `skink.exe`, `skink-lsp.exe`, `skinkfmt.exe`, `scales.exe`,
`skinkjs.exe`, and the required DLLs to `%LOCALAPPDATA%\Skink\bin`, installs the
standard library and C runtime to `%LOCALAPPDATA%\Skink\std`, sets the
`SKINK_HOME` user environment variable, and adds the `bin` directory to your
user `PATH`. Open a new terminal afterward and verify with `skink -v`.

**Prerequisite:** LLVM/Clang must be installed and on `PATH` — Skink shells out
to `clang` to link every program it builds:

```powershell
winget install LLVM.LLVM
```

(or download an installer from <https://releases.llvm.org/>)

The `skink -t` and `skink -b` test/bench modes go through the bundled
`skink_test.cmd` / `skink_bench.cmd` wrappers, which locate Git Bash — install
[Git for Windows](https://git-scm.com/) if you want to run test suites.

## Building from Source

If you already have a `skink` binary (from the installer or a previous build):

```bash
make
```

This builds the self-hosted compiler using the existing `skink` binary as the
stage-0 compiler. The resulting `./skink` binary is a full replacement.

### Self-Hosting Verification

```bash
make stage2
```

This compiles `skink.skink` with the stage-1 binary, verifies the stage-2
binary can type-check itself, and replaces the stage-1 binary with stage-2.

### Installation from Source

```bash
make install        # builds and installs the toolset to ~/.local/skink
make install-only   # installs pre-built binaries without rebuilding
make uninstall      # removes the toolset and stdlib
```

This installs `skink`, `skink-lsp`, `skinkfmt`, `scales`, `skinkjs`,
`skink_test.sh`, and `skink_bench.sh` to `~/.local/skink/bin/`, plus the
standard library and C runtime to `~/.local/skink/std/` — the same layout
`install.sh` uses. Override the root with `make SKINK_PREFIX=/opt/skink install`.

## Project Layout

```
skink/
├── skink.skink          Compiler driver (main entry point)
├── std/                 Standard library
│   ├── lexer.skink      Lexer
│   ├── parser.skink     Parser
│   ├── resolver.skink   Module/scope resolver
│   ├── checker.skink    Type checker
│   ├── emitter.skink    LLVM IR emitter
│   ├── ast.skink        AST types
│   ├── json.skink       JSON encoding/decoding
│   ├── str.skink        String utilities
│   ├── os.skink         OS/file operations
│   ├── ...              (90+ modules total)
│   └── runtime/         C runtime support files
├── install.sh           Curl install script
├── release.sh           Release packaging script
├── Makefile             Build system
└── version.txt          Version string
```

## License

MIT
