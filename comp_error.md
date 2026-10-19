# Compiler errors / limitations — active list

New issues discovered since the archive at `artifacts/comp_error.md`
(#1–#138). Continue numbering from #139.

## #139 — `fn main(args: []string)` compiles but `args` is always empty — FIXED 2026-10-19

- **Severity:** medium — silent wrong behavior; the program builds and runs
  but can never see command-line arguments.
- **Symptom:** `fn main(args: []string) -> int { println(len(args)) ... }`
  prints `0` no matter how many arguments are passed. No diagnostic.
- **Root cause:** the emitted C entry point calls `@_skink_main()` with no
  parameters (`emitRuntimeMain` in `std/emitter.skink`), so a `main` that
  declares an `args` parameter is invoked with it uninitialized/empty.
  argc/argv are only stored into runtime globals for `os.Args()` /
  `os.ArgsCount()`.
- **Fix:** `emitRuntimeMain` now detects a `main` with exactly one
  `[]string` parameter and passes the result of
  `@_skink_runtime_get_argv()` (argv as a slice header, identical to
  `os.Args()`, including argv[0]). `checkFnDecl` rejects any other
  parameter signature on module-main `main` (e.g. `fn main(n: int)`) with
  "main must take no parameters or a single []string parameter...".
  Library-module and method `main`s are unaffected.
- **Regression:** `std/main_args_test.skink` (standalone; asserts
  args == os.Args() and re-execs itself to verify argv passthrough),
  `std/compilefail/cf_main_args.skink` (rejected signature).
- **Found via:** skink-tutorials `16-building-a-small-project.md` uses this
  signature in `main.skink`; it now works as documented.
