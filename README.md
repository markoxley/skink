# skink

The self-hosted Skink compiler, written in Skink itself. This is the canonical
compiler toolset — no Go or other bootstrapping language is required once the
initial binary is installed.

## Quick Install

Install the latest release from GitHub:

```bash
curl -fsSL https://raw.githubusercontent.com/markoxley/skink/main/install.sh | bash
```

This downloads the `skink` compiler, `skink-lsp` language server, `skinkfmt`
formatter, `scales` package manager, `skinkjs` JavaScript transpiler, and the
standard library to `~/.local/skink/`, then updates your shell config
(`.bashrc`, `.zshrc`, etc.) to add them to `PATH` and set `SKINK_HOME`.

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
