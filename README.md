# skink-pure

The self-hosted Skink compiler, written in Skink itself. This is the canonical
compiler toolset — no Go or other bootstrapping language is required once the
initial binary is installed.

## Quick Install

Install the latest release from GitHub:

```bash
curl -fsSL https://raw.githubusercontent.com/skink-lang/skink/main/skink-pure/install.sh | bash
```

This downloads the `skink` compiler, `skink-lsp` language server, and the
standard library to `~/.local/skink/`, then updates your shell config
(`.bashrc`, `.zshrc`, etc.) to add them to `PATH` and set `SKINK_HOME`.

Install a specific version:

```bash
curl -fsSL https://raw.githubusercontent.com/skink-lang/skink/main/skink-pure/install.sh | bash -s -- --version 0.2.0
```

Install to a custom prefix:

```bash
curl -fsSL https://raw.githubusercontent.com/skink-lang/skink/main/skink-pure/install.sh | bash -s -- --prefix /opt/skink
```

Skip shell config updates:

```bash
curl -fsSL https://raw.githubusercontent.com/skink-lang/skink/main/skink-pure/install.sh | bash -s -- --no-shell-rc
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
make install        # builds and installs to /usr/local
make install-only   # installs pre-built binary without rebuilding
make uninstall      # removes from /usr/local
```

## Creating a Release

```bash
./release.sh                    # uses version from version.txt
./release.sh 0.2.0              # explicit version
./release.sh --cross linux-arm64  # cross-compile target
```

This builds the compiler and LSP, packages them with the standard library into
`dist/skink-<version>-<os>-<arch>.tar.gz`, and generates a SHA256 checksum.

Upload to GitHub:

```bash
gh release create v0.2.0 \
  dist/skink-0.2.0-linux-x86_64.tar.gz \
  dist/skink-0.2.0-linux-x86_64.tar.gz.sha256 \
  --title "Skink v0.2.0" \
  --notes "Release v0.2.0"
```

## Project Layout

```
skink-pure/
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
