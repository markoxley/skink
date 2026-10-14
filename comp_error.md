# Skink Compiler Bugs / TODOs

Canonical running list of NEW compiler issues discovered while building or
using Skink. The resolved history (#1–#120) lives in
`artifacts/comp_error.md` — do not reopen it; if an old issue resurfaces,
file a fresh entry here and reference the archived number. Numbering
continues from **#121**.

Entry format: numbered heading, severity, symptom/error message, root cause
(if diagnosed), expected behavior, and any workaround. Mark entries
`✅ FIXED` in place when resolved.

## #121 — `int64(-N)` literal miscompiles for N > i32::MAX — ✅ FIXED

- **Severity**: medium — silent wrong value, no diagnostic
- **Symptom**: `int64(-9876543210)` yields `-1286608618` (the i32-truncated
  magnitude negated). `int64(-5000000000)` yields `-705032704`. Positive
  out-of-range literals (`int64(9876543210)`) are correct.
- **Root cause**: the i64-literal-overflow fix made `emitCast` substitute the
  literal's raw `strVal` when casting `int64(<IntLit>)`. But `-N` parses as
  `PrefixExpr(-, IntLit)`, not an `IntLit`, so `int64(-N)` never sees a
  literal — the i32-truncated `intVal` is used, then negated at runtime.
- **Expected**: `int64(-9876543210)` == `-9876543210`; either fold the
  prefix minus into the literal's `strVal` at parse time, or teach
  `emitCast` to look through `PrefixExpr(-, IntLit)`.
- **Workaround**: negate outside the cast — `-int64(9876543210)` or
  `int64(0) - int64(9876543210)` both produce the correct value.
- **Discovered by**: `std/strconv_test.skink` `TestParseInt64` (orm work).

## #122 — `Fn[T]` bracket syntax gives opaque "cannot index" — ✅ FIXED

- **Severity**: low — misleading diagnostic, no wrong code
- **Symptom**: `Probe[string]()` (square brackets instead of `<>`) fails
  with `undefined variable: string` + `cannot index type fn() -> string`
  — accurate (it *is* an index of a function) but unhelpful: the user
  meant generic args. Angle-bracket `Probe<string>()` always worked;
  the original "bug" was a square-bracket typo.
- **Fix**: `std/checker.skink` `checkIndexExpr` — when the index target
  is a generic function (`genericFnDecl` + `fnGenerics`), report
  `cannot index generic function "Probe" — generic arguments use <...>,
  e.g. Probe<T>(...)`.
- **Regression test**: `std/compilefail` fixture `fixture_generic_index`
  asserting the hint text appears.
- **Discovered by**: orm generics probe (square-bracket typo that
  produced the confusing error).

## #123 — ruleset `action: fn() { ... }` emits empty body, never runs — ✅ FIXED

- **Severity**: high — invalid LLVM IR (empty function body fails `llc`),
  and semantically the action would never execute
- **Symptom**: a ruleset rule written as `action: fn() { ... }` (literal
  form, as used in skink-advanced/07-ruleset-intro) compiles to
  `define void @__skink_fnlit_N(i8* %env) {\n}` — a function with no
  basic block — so `clang`/`llc` rejects the module with "function body
  requires at least one basic block". The `action: { ... }` block form
  worked fine.
- **Root cause**: two bugs in `emitRulesetCode` (`std/emitter.skink`):
  1. After `emitCondBranch` the `trueLabel:`/`endLabel:` were emitted
     via raw `emitln` instead of `emitLabel`, leaving `cg.terminated`
     stuck `true`; `emitBlockStmtRaw` inside `emitFnLit` then skipped the
     literal's body, producing the empty define.
  2. The parser stores `action: fn() {...}` as an `ST_EXPR` statement
     holding the `EX_FN_LIT`; `emitStatement` merely *evaluated* the
     literal, yielding a closure handle that was discarded — the action
     body never ran.
- **Fix**: emit labels via `emitLabel`/`emitBranch` (which manage
  `terminated` correctly), and detect `ST_EXPR`/`EX_FN_LIT` actions —
  emit the literal then `emitClosureCall` the handle.
- **Regression test**: `std/rules_test.skink` `TestRulesetFnLitAction`
  — fails to compile on the old emitter (invalid IR) and asserts the
  action actually fires at runtime.
- **Discovered by**: Phase-5 battle sweep — building
  `skink-advanced/07-ruleset-intro` and `08-distributed-system` end to
  end (prior validation only `-check`ed those projects).
