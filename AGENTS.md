# Skink Compiler - Project Notes

## Required Policy: Tests and Documentation for Every Change

- **Every fix or change must ship with new unit tests.** Whenever a bug fix
  or behavior change is applied to the compiler, runtime, or stdlib, add
  regression tests covering it — in the appropriate `std/*_test.skink`
  file or a new test file following the same conventions (`pub fn
  TestXxx()` functions using `std/testing` asserts; standalone programs
  use `module main` + `fn main()`).
- **New tests must fail before the change and pass after it** where
  feasible — verify the test reproduces the bug on the unfixed code
  first.
- **The full suite must pass after every change**: run `bash
  run_tests.sh` (all `*_test` suites plus book examples) before
  considering the work done.
- **All code must be fully documented**: file-purpose header comments on
  new files, doc comments on every new function/type, and comments
  explaining non-obvious implementation decisions. Test functions must
  state which regression they guard.

## Build & Test Commands

- `make` — Build the self-hosted Skink compiler (Stage 1 bootstrap)
- `bash run_tests.sh` — Run all test suites
- `SKINK_HOME="$(pwd)" ./skink -o <output> <input.skink>` — Compile a Skink program
- `SKINK_HOME="$(pwd)" ./skink -emit-ll <input.skink>` — Emit LLVM IR without linking
- `SKINK_HOME="$(pwd)" ./skink -g -o <output> <input.skink>` — Compile with DWARF debug info
- `SKINK_HOME="$(pwd)" ./skink -g -emit-ll <input.skink>` — Emit LLVM IR with debug metadata

## Architecture

- Self-hosted compiler written in Skink, compiled via LLVM IR
- Bootstrap: `bootstrap-home/` contains the previous-stage compiler used to build `skink`
- When modifying `std/*.skink` (compiler source), copy to `bootstrap-home/std/` before rebuilding
- Runtime C files in `std/runtime/` are linked into every program **except `ui_rt.c`**, which is conditionally linked only when the program imports `std/ui` (the driver in `skink.skink` scans the resolved import list for `std/ui*` and adds the shim + `sdl2-config` cflags/libs). Copy runtime changes to `bootstrap-home/runtime/` too. The vendored Nuklear headers live in `vendor/` beside each `runtime/` dir (`std/runtime/vendor/`, `runtime/vendor/`, `bootstrap-home/runtime/vendor/`).
- GUI toolkit: `std/ui` (widgets + `Window` + `Widget` template over the SDL2/Nuklear C shim `std/runtime/ui_rt.c`) and `std/ui/binding` (`Observable`, `Bind*` adapters, `RowModel` sources). Requires system `libsdl2-dev`. Tests run headless via `SDL_VIDEODRIVER=dummy` (see `std/ui/ui_test.skink`). Sample: `samples/ui_demo.skink`. Book: `book/19b_gui.md`.

## Bootstrap Build Performance

The bootstrap compiler uses **linear-scan maps** (O(N) per lookup with `strcmp`) rather than hash tables. When compiling `skink.skink`, the checker processes ~725 declarations from all imported modules. Each identifier resolution does multiple O(N) map lookups on the global scope, resulting in O(M×N) total complexity where M is the total number of identifier resolutions (~50,000+). This makes the self-compilation take **~8-10 minutes of CPU time**. This is not a hang — it is expected slow behavior. Use a timeout of at least 600 seconds when building.

## Generic Type Erasure Limitations

The compiler uses **type erasure** for generics (templates). All generic type parameters are erased to `i8*` (void pointer) at compile time. This has fundamental limitations:

1. **`reflect.TypeOf<T>`** — Type info is generated inline at call sites with concrete types via `emitReflectTypeOf`. Inside generic functions where `T` is a type parameter (e.g. `K` in HashMap), the type name is the parameter name ("K"), not the concrete type. Runtime heuristics in `reflect_hash` and `reflect_equals` detect whether erased `i8*` values are strings or integers by checking if the pointer falls in mapped memory (mincore on POSIX, VirtualQuery on Windows). If mapped, the value is treated as a string and hashed/compared by content; otherwise it is treated as an integer and hashed/compared by raw value. This is safe (no segfaults on unmapped addresses) and consistent (same value → same hash), so `HashMap<string, V>` now works with content-based key equality.
2. **`hashmap_test`** — `==` on erased `i8*` keys uses `strcmp` (string comparison), which crashes on integer keys cast via `inttoptr`. Fixed by using `reflect.Equals` with type-aware comparison: `strcmp` for strings, pointer equality for integers, heuristic detection for generic type parameters.

## Test Results

**105/105 test suites passing, 1170 test functions.**

Book examples: **314/326 pass, 0 failed, 12 intentionally skipped**
(REPL/shell snippets and external-service examples). The book now lives in
the sibling `skink-book` repo (`v1.1/`): extract with `python3 v1.1/extract_samples.py`
and verify with `bash v1.1/test_examples.sh` — the harness derives `SKINK_HOME`
from its own location, so `skink`, `std`, `runtime` must be visible at the
`skink-book/` root (symlinks work and are untracked). The `book/` guard in
`run_tests.sh` is a no-op in this layout.

## Book Examples Harness

- `book/extract_samples.py` extracts every ```` ```skink ```` fence into
  `book/example_code/`, classified as `compile`, `run`, `fragment`, or `skip`
  based on whether the snippet is a complete program.
- `book/test_examples.sh` wraps each sample in a `main` (if needed) and
  compiles it with the self-hosted `skink` binary; `run` samples are executed.

## Key Fixes Applied

- **Struct field tags (C#-style `[Attr]` annotations)**: `std/parser.skink` + `std/ast.skink` + `std/emitter.skink` + `std/reflect.skink` — `[name]` / `[name(args)]` lines above struct fields are parsed into `ast.Field.tags` (`parseFieldTag` preserves argument text, unlike declaration `parseAttributes` which drops args), carried through `StructLayout.fieldTags` (space-joined), and emitted by `emitReflectTypeOf` into a new fourth `reflect.Field` slot (`tag: string`, `reflect_Field` record size 24→32). Readable at runtime via `reflect.FieldByName(t,"x").tag` or `reflect.FieldTag(t,"x")`. Tags placed above `fn` members inside a struct are a **compile error** (they were previously silently discarded). Regression: `std/reflect_test.skink` `TestFieldTags`.
- **`std/orm` (lightweight struct↔table mapper)**: `std/orm.skink` — Dapper-shaped ORM over `std/db` (SQLite): plain structs + field tags (`[db(pk,auto)]`, `[column(name=…)]`, `[db(ignore)]`). Because generics erase `T`, the API takes `reflect.Type` + `int64(&obj)` explicitly (`orm.TableOf(conn, reflect.TypeOf<User>(u))`). Supports `Create`/`Insert`/`Get`/`Save`/`Delete`/`All`/`Where`/raw `Query`; types: int/int64/bool/string/float. `db.Result` gained `ColumnCount`/`ColumnName`/`Close`. `reflect` gained shared tag helpers (`TagGet`, `TagHas`, `TagArg`) reused by `json.MarshalStruct`/`UnmarshalInto`. Limits: int pks only for `Get`/`Save`/`Delete`, `Where` args bind as text, no NULL concept.
- **Ruleset `action: fn() {...}` empty body + never invoked (comp_error.md #123)**: `std/emitter.skink` — `emitRulesetCode` emitted `trueLabel:`/`endLabel:` via raw `emitln`, leaving `cg.terminated` stuck true after `emitCondBranch`, so `emitBlockStmtRaw` skipped the lifted literal body entirely (invalid IR: function with no basic block). The `ST_EXPR`/`EX_FN_LIT` action was also only evaluated (closure handle discarded), never called. Fixed: labels via `emitLabel`/`emitBranch`, and fn-literal actions invoked via `emitClosureCall`. Regression: `std/rules_test.skink` `TestRulesetFnLitAction`.
- **`int64(-N)` literal miscompile (comp_error.md #121)**: `std/emitter.skink` — `emitCast`'s i64 raw-literal path didn't see through `PrefixExpr(-, IntLit)`, so `int64(-9876543210)` used the i32-truncated `intVal` before negation. Now folds the sign into `strVal` via `intLitToI64String` (decimal and non-decimal forms). Regression: `compiler_fixes_test`.
- **`Fn[T]` bracket-syntax diagnostic (comp_error.md #122)**: `std/checker.skink` — `Probe[string]()` failed with opaque `cannot index`/`undefined variable: string`; now reports `cannot index generic function — generic arguments use <...>`. (Angle-bracket keyword type args like `Probe<string>()` always worked — `string` lexes `T_STRING_TYPE`, `int` lexes `T_IDENT`.) Regression: `std/compilefail` fixture.
- **Rendezvous channel lost wakeup (comp_error.md #119)**: `std/runtime/conc_rt.c` — on a capacity-0 channel, `send_cv` waiters are a mix of post-enqueue senders (waiting for consumption) and pre-send waiters (waiting for a free slot). `Skink_chan_recv` signaled only one waiter per dequeue, so when it landed on the already-satisfied sender the queued sender slept forever — an intermittent ~50% deadlock for the common "two senders + two receives" pattern. `recv` now broadcasts `send_cv` for rendezvous channels (bounded channels keep the single signal).
- **`async`/`await` (thread-per-task futures)**: `std/checker.skink` + `std/emitter.skink` — `async f(args)` type-checks as `future<T>` (a `TK_NAMED` type named `future` with `elem=[T]`) and `await p` unwraps to `T`; the checker records per-expression element types in `awaitElemTypes` keyed by `line:col`. The emitter's `emitAsyncExpr` mirrors `emitSpawnStmt` — it packs/coerces/retains args into a heap struct, creates a future via `Skink_future_make`, and calls `Skink_spawn(thunk, pack, fut)`. The generated `Skink_async_thunk_N` calls the real function signature and boxes the result into an `Skink_rc_alloc`'d slot (uniform boxing for all non-void types); the spawn trampoline's `Skink_future_set` stores the box. `emitAwaitExpr` calls `Skink_future_get` (blocking), loads the typed value out of the box, and frees the box via `Skink_rc_release`. Multi-return calls and non-named-function callees are rejected. `await` on a `void` call returns the raw (null) handle — useful as a join. Futures are small C structs that are never freed (~72 bytes per task — acceptable).
- **Channels are RC-managed (comp_error.md #97)**: `std/runtime/conc_rt.c` — `Skink_chan_make` now allocates via `Skink_rc_alloc_typed` with `Skink_chan_destroy`, which drains queued nodes (releasing each element's queue-owned ref) and destroys the sync primitives when the last reference drops. Channels emit as `i8*` so existing retain/release machinery handles them. Spawn thunks now release each retained heap arg after the call (previously every `spawn` leaked one ref per heap arg).
- **Parser enum-loop hang (comp_error.md #98)**: `std/parser.skink` — `parseEnumDecl`'s variant loop made no progress on non-IDENT tokens (e.g. `fn` after a deleted `}`), hanging the compiler. Unexpected tokens now error + break; bare IDENTs still continue (newline-separated variants). All 24 parser `for !curIs(...)` loops were audited — this was the only no-progress path.
- **Fuzzing hardening**: `std/compiler_fuzzgen_test.skink` adds grammar-aware fuzzing — structured random program generation through `-emit-ll` (exercises the emitter) and line-level mutation of real repo sources. `SKINK_FUZZ_N`/`SKINK_FUZZ_SEED` env vars scale ad-hoc deep runs; failing inputs are preserved under `/tmp/cfg_fail_*`. The compilefail harness wraps the compiler in `timeout 10` so parser hangs fail instead of wedging the suite. `skink.skink` accepts `SKINK_CLANG_FLAGS` for sanitizer builds (`-fsanitize=address`).
- **Template fat-struct boxes are heap-allocated and RC-managed (comp_error.md #95)**: `std/emitter.skink` + `std/runtime/rc_rt.c` — `coerceToTemplate` previously boxed `{dataPtr, methodFns...}` into a stack `alloca`, so a `[]Widget`-style slice returned from a helper function held pointers into a dead frame (segfault on dispatch, e.g. `call 0x1`). The box is now allocated via `Skink_rc_alloc_typed` with `Skink_template_box_dtor` (which releases the boxed data pointer), the data pointer is retained at boxing time, and `isHeapLLType` classifies `%template.*` pointer types as RC-managed so every existing retain/release site (slice/field stores, scope cleanup, array dtors) handles boxes automatically.
- **Template method lookup for underscored module names (comp_error.md #96)**: `std/emitter.skink` — `findConcreteMethodFnName` mapped `mod_Type` → `mod.Type` by replacing `_` with `.`, which broke for module names containing underscores (e.g. `template_escape_test_Alpha`), leaving method slots null. A fallback re-sanitizes each registered `module.Type.Method` key's receiver part and compares it to the LLVM struct name — unambiguous for any underscore placement.
- **Template fat pointer for heap-allocated structs**: `std/emitter.skink` — `coerceToTemplate` now strips all trailing `*` from `structLLName` (using a loop instead of a single `if`), fixing template interface dispatch for heap-allocated structs passed by address-of (e.g. `&tee` where `tee` is a `*TeeReader`). Previously, structs containing template interface fields (like `streams.TeeReader`, `streams.LimitReader`, `streams.MultiWriter`) were heap-allocated, producing a double-pointer `argType` like `%struct.streams_TeeReader**`. The single-strip left a stray `*` in `structLLName` (e.g. `streams_TeeReader*`), causing `findConcreteMethodFnName` to fail and the method function pointer in the fat pointer to remain null, resulting in a segfault on any method call through the template interface.
- **File/line/col in error messages**: `std/checker.skink` + `std/emitter.skink` + `std/resolver.skink` + `skink.skink` — Compilation errors now include the source file path, line, and column in `file:line:col: message` format. The checker's `currentFile` field is now set from `decl.filePath` in both `checkProgram` loops, and `currentLine`/`currentCol` are set at the entry of `checkDeclaration`, `checkStatement`, and `checkExpr`. The `report()` function prepends `file:line:col:` (matching `reportAt()`'s format). The emitter's `errorf` now includes `currentFilePath:currentStmtLine:currentStmtCol:` prefix, with `currentStmtLine`/`currentStmtCol` set per-statement in `emitStatement`. The resolver's `Resolve()` now returns `([]ast.Stmt, []string)` (decls + errors) instead of printing errors internally, and the main driver checks for resolver errors and exits early before running the checker.
- **Struct string interpolation**: `std/emitter.skink` — `emitValAsString` now handles struct-by-value types (`%struct.*`, `%map_*`, `%set.*`) by returning a `"{}"` placeholder string instead of passing the raw struct register to `emitStringConcat`, which caused LLVM IR type errors (`icmp eq i8* %structval, null`).
- **`delete` builtin**: `std/checker.skink` + `std/emitter.skink` — Added `delete(x)` builtin that releases heap-allocated values (strings, slices, maps, pointers) via `Skink_rc_release` and nulls the variable. For struct-by-value types, releases all heap-allocated fields via `emitReleaseStructHeapFields` and zeroes the struct. Useful for explicit memory management before end of scope.
- **ParseFloat validation**: `std/strconv.skink` — `ParseFloat` now validates input strings before calling `strtod`, preventing non-numeric strings from being treated as valid floats.
- **Defer on early return**: `std/emitter.skink` — `emitRet` now emits deferred statements before any `return` instruction, not just the fall-through at end of function.
- **Ruleset code generation**: `std/emitter.skink` — `emitRulesetCode` generates proper LLVM IR for rulesets using the C runtime (`rules_rt.c`), including per-rule eval functions, start/stop/reset methods, and `registerRule` with dynamic wrapper generation.
- **Null template field access**: `std/emitter.skink` — Template fat pointer field access now includes a null check, returning null when the fat pointer is zero-initialized.
- **Ruleset runtime thread**: `std/runtime/rules_rt.c` — `Skink_ruleset_start` now spawns a background thread for the evaluation loop, and `Skink_ruleset_evaluate` skips rules with null eval functions.
- **Function pointer retain/release**: `std/emitter.skink` — `isHeapLLType` now excludes function pointer types (ending with `)*`) from reference counting, preventing crashes when function pointers are incorrectly retained/released as heap objects.
- **Composite type call args**: `std/emitter.skink` — `emitCallExpr` now uses `paramLLType(llvmType(...))` for composite types, ensuring struct arguments are passed correctly by pointer in indirect calls.
- **Slice of maps support**: `std/emitter.skink` — Fixed `parseMapTypeLL` callers to check for `**` suffix, distinguishing slices of maps (`%map_...**`) from actual maps (`%map_...*`). Affected: `emitMakeExpr`, `emitLenCall`, `emitForInStmt`, `emitIndexExpr`, `emitAssignStmt`, `exprIndexLLType`.
- **Module-level var type inference**: `std/checker.skink` — Module-level `var` declarations now infer types from type annotations or initializer values instead of always defaulting to `anyType()`.
- **Concurrent web server**: `std/web/server.skink` — Connection handling now uses `Skink_spawn` for concurrent connections via `Skink_spawn_handle_conn` C helper.
- **HTTPS API client**: `std/api.skink` — Added `Skink_tls_client_connect` in `std/runtime/tls_rt.c` for HTTPS requests, and `Skink_set_socket_timeout_ms` in `std/runtime/net_rt.c` for timeout support.
- **Testing counters and FailNow**: `std/testing.skink` — `testCount`, `passCount`, `failCount` are now tracked, and `FailNow()` calls `Skink_test_failnow` (which calls `abort()`) for early test exit.
- **Logic variable renaming**: `std/logic.skink` — Rule variables are now renamed to fresh names before unification to avoid collisions with query variables. Uses `libc.malloc` (non-RC) for renamed variable names stored as map keys, since RC-managed strings from `str.Concat` can be freed prematurely when used as map keys (maps don't retain keys). Also added proper backtracking in `collectGoals` via `solveBodyThenContinue`, and result deduplication via `filterBindings` + `deduplicateBindings`.
- **ARC slice element destructors**: `std/emitter.skink` + `std/runtime/rc_rt.c` — Slice backing arrays now use `Skink_rc_alloc_typed` with per-element-type destructor functions generated by `getOrCreateArrayDtor`. The destructor walks the array and releases each RC-managed element (pointer or struct-with-heap-fields), preventing memory leaks from slices. `emitAppendCall`, `emitMakeExpr`, and `emitArrayLiteral` all use typed allocation. Array destructor names are sanitized via `sanitizeLLTypeName` to handle `*` in type names (e.g. `i8*` → `i8Ptr`).
- **i64 prefix negation fix**: `std/emitter.skink` — `emitPrefixExpr` now checks `resultType == "i64"` and uses `sub i64` instead of `sub i32` for i64 negation.
- **i64 literal overflow**: `std/parser.skink` + `std/emitter.skink` — Integer literals are now stored in `strVal` (raw text) in addition to `intVal` (i32). `emitCast` uses the raw literal directly when casting to `i64`, avoiding i32 overflow for large constants like `int64(6364136223846793005)`.
- **XML parser infinite loop on depth limit**: `std/xml.skink` — `parseNode` now consumes the `<` character when hitting the depth limit (1000), preventing the caller's content loop from repeatedly calling `parseNode` on the same position.
- **CSV reader EOF detection**: `std/csv.skink` — `Reader.Read()` now returns an `EOF` error when `pos >= len` at the start, preventing infinite loops in callers that read until error.
- **JSON parser depth limit error + parseString guard**: `std/json.skink` — Added `errMsg` field to `Parser`; depth limit now sets it and `Unmarshal` checks it. `parseString` now checks for `"` before consuming, preventing it from eating non-string characters (e.g. `{`) and masking depth limit errors.
- **io.Itoa and io.Print**: `std/io.skink` — Added `io.Itoa(n)` as a wrapper for `strconv.Itoa`, and `io.Print(s)` as an alias for `io.PrintString`.
- **errors.Error() method**: `std/errors.skink` — Added `Error()` method on the `Error` struct (Go-style error interface). The checker was updated to allow function names to coexist with struct type names in the same scope.
- **strconv.Itoa64**: `std/strconv.skink` — Added `Itoa64(n: int64) -> string` for int64-to-string conversion without precision loss.
- **err == nil / err != nil comparison**: `std/emitter.skink` — `emitInfixExpr` now handles nil comparisons against `errors.Error` structs by comparing the `message` field (i8\*) to null, instead of trying to compare a struct value to null.
- **Map key/val array destructors**: `std/emitter.skink` — Map key and value backing arrays now use `Skink_rc_alloc_typed` with per-element destructors (via `emitMapArrayAlloc`), ensuring retained keys/values are properly released when the map grows. Map literals also retain RC-managed keys and values.
- **Map slice-value retain/release (stage2 segfault)**: `std/emitter.skink` — `emitMapIndexAssignment` retained/released map keys and values only when the LLVM type was exactly `i8*` or a `%struct.*` pointer, so heap values like `i8**` (slices, e.g. `map[string][]string`) and `%map_*` were stored unretained. Since statement temps are released at scope end, `m[k] = append(m[k], v)` stored a slice whose backing array was then freed by the temp release — the dangling slot was later reused by unrelated string data (segfault inside `str.Len`/`str.Concat`, e.g. `checkProgram` → `astNamedToType`). All key/value retain+release sites in `emitMapIndexAssignment` (update, rehash, resize-insert, no-resize-insert) and `emitDeleteCall` now use `isHeapLLType`. Additionally, `delete(m, k)`'s rehash copied elements to the new arrays with no retain at all before releasing the old arrays — losing a ref for every remaining entry; retains now match the grow-rehash path.
- **delete()/for-in map valLL mis-derivation**: `std/emitter.skink` — `emitDeleteCall` and `emitForInMapStmt` re-derived the map value type from the mangled map name via `unsanitizeLLTypeName`, which strips only one `Ptr` suffix (`i8PtrPtr` → `i8Ptr*`), emitting invalid LLVM IR for slice-valued maps. Both now use `parsedVal`/`mapValLL` from `parseMapTypeLL` (which uses `desanitizeLLTypeName`, replacing all `Ptr`). The same fix was applied to `set` element extraction in `emitForInStmt`.
- **Nested slice literals**: `std/parser.skink` — `parseNestedCompositeLit` handles `{...}` inside `[]Type{...}` composite literals, allowing shorthand syntax like `[][]int{{1,2},{3,4}}` instead of requiring explicit `[]int{1,2}` for each inner literal.
- **errors.Error() method**: `std/checker.skink` + `std/errors.skink` — The checker now allows a function name to share a scope with a struct type name (TK_NAMED + TK_FUNCTION), enabling the `Error()` method on the `Error` struct. The struct remains accessible via `c.structs` for struct literals and type annotations.
- **err == nil / err != nil for named error structs**: `std/checker.skink` + `std/emitter.skink` — The checker allows nil comparison for any named struct type with a `message` field. The emitter compares the message field's first byte to 0 (empty string) or pointer to null, correctly detecting "no error" states.
- **Compound assignment operators**: `std/emitter.skink` — `emitAssignStmt` now handles compound operators (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`) for simple variables (local and global), struct fields, and array indices. Previously, `emitAssignStmt` ignored `stmt.assignOp` for non-map targets, causing `i += 1` to behave as `i = 1` (infinite loops in for-loops). `emitFieldAssignment` and `emitIndexAssignment` now accept an `op` parameter and load the old value before applying `computeCompoundOp`. `computeCompoundOp` was also extended to handle bitwise operations (`and`, `or`, `xor`, `shl`, `lshr`/`ashr`).
- **Enum constant emission**: `std/emitter.skink` — Simple enum variants (no payloads) are now registered as int constants in `cg.consts` during the pre-pass, so they get inlined as `i32` literals. Previously, enum variants were referenced as `@VariantName` globals but never defined, causing LLVM errors ("global variable reference must have pointer type").
- **Until loop condition**: `std/emitter.skink` — `emitUntilStmt` now correctly branches to the body when the negated condition is true (i.e., when the original condition is false). Previously, the branch targets were swapped, causing `until` loops to never execute their body.
- **Match expression type-aware comparison**: `std/emitter.skink` — `emitMatchExpr` now uses the subject's actual LLVM type for comparison (`i1` for bools, `i64` for int64s) instead of always using `icmp eq i32`. Previously, matching on bool values caused LLVM type errors.
- **Select statement implementation**: `std/emitter.skink` + `std/checker.skink` + `std/runtime/conc_rt.c` — The `select` keyword was parsed and type-checked but produced no code. Added `emitSelectStmt` which builds arrays of channels/send-flags/values, calls `Skink_chan_select`, and dispatches to the ready case body. Supports `case val := <-ch:` (receive-binding), `case <-ch:` (receive), `case ch <- val:` (send), and `default:` (non-blocking fallback). Without `default`, blocks forever; with `default`, polls once. Fixed `Skink_chan_select` runtime to support infinite timeout (-1) and non-blocking mode (0).
- **Parser infinite loop in struct body**: `std/parser.skink` — `parseStructDecl`'s field-parsing loop could enter an infinite loop when encountering unrecognized tokens (e.g., when a struct method's return type was written without `->`, causing `parseFnDecl` to leave tokens unconsumed). The loop now advances past unrecognized tokens when no progress was made (no field name consumed and no colon to follow), preventing the infinite loop and allowing proper error reporting.
- **Consistent error message format**: `std/parser.skink` + `std/resolver.skink` + `std/checker.skink` + `std/emitter.skink` — All compilation errors now follow the format `filename line:col: message`. Previously, errors were inconsistent: parser errors used `filename: line:col:`, checker `report` had no file/line/col at all (and `currentFile` was never set), checker `reportAt` used `filename:line:col:`, and emitter `errorf` had no prefix. Added `currentLine`/`currentCol` fields to the Checker (set from `setModuleFromDecl`, `checkStatement`, and `checkExpr`), `currentStmtCol` to CodeGen (set from `emitStatement`), and set `currentFile` from `decl.filePath` in `setModuleFromDecl`. Also synced `filePath` field in `bootstrap-home/std/ast.skink`.
- **`copy` builtin**: `std/checker.skink` + `std/emitter.skink` — Added `copy(x)` builtin that produces a like-for-like deep copy of a slice or map. For slices, `emitCopyCall` allocates a new backing array with the same element destructor and bulk-copies the element bytes via `llvm.memcpy` (the fast path — a single bulk memory copy), then runs an element retain loop only when the element type is reference-counted (strings, pointers, structs with heap fields). For maps, `emitMapCopy` allocates a new map struct and deep-copies both the key and value backing arrays (same memcpy + retain approach) plus the length field. Null sources are handled (return null). The checker's `checkCopyCall` validates that the argument is a slice (`TK_ARRAY`) or map (`TK_MAP`) and returns the same type.
- **HashMap string-key content equality**: `std/runtime/reflect_rt.c` — `reflect_hash` and `reflect_equals` now use a safe page-mapping heuristic (mincore on POSIX, VirtualQuery on Windows) to detect whether an erased `i8*` value in a generic container is a string pointer or an integer. Previously, generic type parameters erased to `i8*` with type name "K" (not the concrete type), so `HashMap<string, V>` used pointer identity for string keys — two distinct allocations of "hello" would not match. Now, if the erased value points to mapped memory, it is treated as a string and hashed/compared by content; otherwise it is hashed/compared as a raw integer. This is safe (mincore never segfaults on unmapped addresses) and consistent (the same value always produces the same hash), so lookups remain correct even if an integer value happens to land in a mapped page.
- **HashMap Delete method**: `std/hashmap.skink` — Added `Delete(key) -> errors.Error` using backward-shift deletion to maintain probe chain integrity without tombstones. When an entry is removed, subsequent entries that probed past the gap are shifted back to fill it, ensuring all remaining keys remain findable. Also added `Len() -> int` method.
- **errors.None()**: `std/errors.skink` — Added `None()` as the idiomatic way to return a no-error value, replacing the `errors.New("")` convention in HashMap's `Get` and `Delete` methods.
- **Module-var inference deferred past const resolution**: `std/checker.skink` — In `checkProgram`, the first pass previously ran `checkExpr` on module-level `var` initializers while const bindings still held placeholder `namedType(name)` types, so `var x = []string{CONST_A, CONST_B}` reported a spurious `array element N type mismatch: expected A, got B`. Initializer-only vars now get an `anyType()` placeholder in the first pass, and a dedicated var-inference pass (after the const-evaluation and `resolveConstNamedType` passes, before the main check pass) runs `checkExpr` and updates the binding — so every declaration sees the final inferred type regardless of declaration order. Errors from the inference pass are suppressed (swap `c.errors` for a fresh slice) since `checkVarDecl` re-checks the initializer in the main pass and reports genuine errors once.
- **Module context for global var initializers**: `std/emitter.skink` — Once the checker let `var x = []string{K1, K2}` through in non-main modules, the emitter failed with `global variable reference must have pointer type` because it emitted `@K1` (unqualified) — `cg.currentModule`/`cg.checker.currentModule` were never set for module-level var processing, so `cg.consts["mod.K1"]` and `exprLLType` lookups missed. Added `setModuleContextFromName`, which derives the owning module from the resolver-qualified `stmt.varName` (e.g. `varslib.Names` → `varslib`; unqualified → `main`), applied at both `globalVarTypes` registration sites and per-statement inside `_skink_global_init`.
- **Module context for const initializers**: `std/emitter.skink` — `cg.consts` stores each const's initializer AST, re-emitted inline at every reference site under whatever module context is active. A `pub const` whose initializer referenced same-module symbols (consts, private fns, struct types) — e.g. `pub const Names = []string{K, "z"}` — failed cross-module with `global variable reference must have pointer type` (`@K` emitted unqualified) because the caller's `cg.currentModule` was used. Added `emitConstExpr`/`constExprLLType`, which wrap `emitExpression`/`exprLLType` with `setModuleContextFromName` on the const's qualified key, applied at all `cg.consts` lookup sites (`emitIdent`, `emitFieldAccessExpr`, `exprIdentLLType`, `exprFieldLLType`).
- **Checker `exprTypes` — recorded expression types for the emitter**: `std/checker.skink` — `checkExpr` is now a wrapper over `checkExprImpl` that records each expression's `TType` in `c.exprTypes` keyed by `"file line:col"` (all checker passes call `setModuleFromDecl` so `currentFile` is accurate). The emitter reads it via `checker.exprTypeLookup` — essential where emit-time re-derivation fails (field exprs like `self.out`, inferred locals, integer signedness). `resolveFieldType` sets `c.suppressExprTypes` so emit-time rechecks can't pollute the map. Interpolation part exprs are sub-parsed from the literal text and carry colliding fragment-relative positions, so `checkInterpExpr` additionally records each part under `"file line:col#index"` (`checker.exprTypeLookupAt`). `cg.currentFilePath` is maintained across all global-var/decl emission paths, and `cg.constFiles` tracks each const's file so `emitConstExpr`/`constExprLLType` restore file context too.
- **Channel-field send corruption**: `std/emitter.skink` — `chanElemLLTypeOpt` re-derived a channel's element type at emit time via `resolveFieldType`, which re-ran `checkExpr` on `self`/locals with no checker scope → `""` → the send boxed a `%struct.S*` pointer into a slot the runtime copied as a 16-byte `%struct.S` value (delivered garbage). `chanElemLLTypeOpt` and `chanRecvElemLLType` now consult `exprTypes` first, covering `self.out <- v`, `oc := self.out`, select cases, and receives on all three call sites.
- **Missing integer-width coercion (`i32` value → narrow slot)**: `std/emitter.skink` — Unary/binary exprs emit `i32`, but several sites stored them into `i8`/`i16` slots without truncating (`store i16 %r_i32` — invalid LLVM). `coerceArgToParamType` (trunc/sext/inttoptr/ptrtoint/struct ptr→val/int→float) is now applied at: `emitStructLiteral` field stores, `emitVarDecl` initializers, `emitReturnStmt` single + multi returns, `emitMapIndexAssignment` (keeping its `i1`→`zext` special case, since generic coercion would `sext` bools to -1), and `emitInfixExpr`, which now unifies both operand widths to the operation type (`i16var == -5`, `i16var + 1`, `i16 + i64` — subsumes the old i64-only promotion). `emitPrefixExpr` negates/bitwise-nots narrow ints at the operand's own width. Note `append(slice, v)` can't hit this — the checker rejects mismatched append types.
- **Narrow-int printing and interpolation**: `std/emitter.skink` — printf varargs read a full `i32`, so `println(int16(-5))` printed `65531`. `emitPrintCall` extends `i8`/`i16` args via `extendNarrowInt` (`sext`/`zext` chosen by `typeIsUnsignedInt` on the recorded checker type — LLVM types lose signedness). Interpolation (`"v = {v}"`) previously passed raw `i8`/`i16` regs into `emitStringConcat`'s `icmp eq i8*` null check → invalid IR; `emitExprAsStringT` now extends narrow ints to `i32` and stringifies via `emitIntToString`. `emitValAsString`'s `i8`/`i16` fallback (used by slice printing) also extends — signed by default since it has no expression context.
- **Unsigned widening and unsigned int ops**: `std/emitter.skink` — LLVM int types carry no signedness, and several paths silently treated unsigned values as signed: `int(uint8(250))` was `-6` (all widening used `sext`), `uint8(200) > 100` was `false` (all compares used `slt`/`sgt`/etc.), division/remainder were always `sdiv`/`srem`, `>>` was always `lshr` (wrong for signed negatives too), and `string(int16v)` returned the raw `i16` reg which callers retained as `i8*` (invalid IR). `emitCast` now `zext`s unsigned/bool sources; new `coerceExprToParamType` wraps `coerceArgToParamType` and `zext`s when the source expression's recorded checker type is unsigned — applied at all ~25 coercion call sites. `emitInfixExpr` picks `udiv`/`urem`, `ult`/`ugt`/`ule`/`uge`, and `lshr` vs `ashr` via `unsignedOps` (unsigned if either operand's recorded type is unsigned). `emitCastToString` extends `i8`/`i16` via `extendNarrowInt` before stringifying.
- **`std/web/static` closure-capture breakage**: `std/web/static.skink` — `FileServer` returned a function literal capturing the `rootDir` parameter, but Skink fn literals could not capture enclosing locals at the time (`emitFnLit` lifted them to module scope; the captured reference emitted as an undefined `@rootDir` global), so merely *importing* `std/web/static` broke compilation. Rewrote `FileServer` to store the root in a module-level `var fileServerRoot` and return the named `fileServerHandler` (documented non-reentrancy caveat — now moot: real closure capture landed in comp_error.md #20, though the workaround remains harmless). Also fixed the index.html fallback: `fs.Open(dir, "r")` succeeds on POSIX directories, so directories now hit an `fs.IsDir` check that routes them to the index.html path. Covered by new `std/web/static_test.skink` (200/404/403/index-fallback).
- **`std/web/tmpl` `{{end}}`/`{{else}}` slicing**: `std/web/tmpl.skink` — `executeImpl` advanced `i = endPos + 2` past `{{if}}`/`{{range}}` blocks, but `endPos` is the `{{` of the closing `{{end}}` tag, so `end}}` leaked into rendered output. Both sites now skip to the `}}` terminating `{{end}}`. Also fixed: `{{if}}` without `{{else}}` sliced the true branch to `elsePos=-1` (negative length → `""`), and the else branch started at `elsePos + 2` (mid-tag, leaking `else}}`). Covered by `TestTmplIf`/`TestTmplRange` in `std/web/web_test.skink`.
- **Geometric append growth (comp_error.md #30, stage-2 OOM)**: `std/runtime/rc_rt.c` + `std/emitter.skink` — `append` previously allocated exactly `len+1` elements and copied all elements on every call: O(N²) churn that OOM-killed stage-2 self-compiles at ~12 GB RSS. New runtime `Skink_slice_ensure(ptr, needBytes)` doubles the payload capacity (the RC table already tracks payload byte size — no header layout change) via `realloc`, in place when possible, moving the RC entry under the write lock. `emitAppendValues`/`emitSpreadAppend` branch on `Skink_rc_count(arr) == 1` for the in-place path; shared arrays still alloc+copy with per-element retains. **Safety**: in-place growth is only emitted when the source reference is provably dead — the `x = append(x, ...)` self-assign idiom detected in `emitAssignStmt` via `lValueKey` (idents and field paths like `t.vals`) and carried on `CodeGen.appendSelfVar`, consumed by the outermost `emitAppendCall`/`emitSpreadAppend` so nested appends can't claim it. When the grown result aliases the consumed input pointer, `emitRetainIfAliased` adds one compensating retain for the pending release-of-old + temp release. Also fixed `Skink_array_dtor_sized`, which read the payload-8 field as bytes and divided by elemSize — it is the element count. 200k appends: ~60 ms; `make stage2` peaks ~590 MB. Covered by `TestAppendManyElements`, `TestAppendSharedSlice`, `TestAppendSpread`, `TestAppendStringSlice`, `TestAppendNonSelfPreservesSource`, `TestAppendSelfSpread`, `TestAppendVariadicSelfAssign`, `TestAppendFieldSelfAssign`, `TestAppendNestedArg` in `std/compiler_fixes_test.skink`.
- **Escaped-local boxing (comp_error.md #16)**: `std/emitter.skink` — `&x` on a stack-stored local used to hand out the alloca address, which dangled once the frame returned (`self.ctx.Done()` read garbage → infinite recursion → segfault). `collectEscapedVars` pre-scans each function for `&x` (struct-literal fields, call args, `spawn`/`defer` bodies included); `boxVarAtScope` promotes the local into an RC heap block kept in a `heapSlot` alloca, and `varValuePtr` routes every read/write/`&x` through it. Also fixed `*p = v` deref-assign being silently dropped (`emitAssignStmt` gained an `EX_PREFIX "*"` target branch).
- **Closures / fn-literal capture (comp_error.md #20)**: `std/emitter.skink` + `std/checker.skink` — all `fn`-typed values are a uniform `i8*` closure handle `{i8* code, i8* cap0, ...}` (`llvmType(TK_FUNCTION) == "i8*"`). `emitFnLit` collects enclosing-local refs (`collectCaptures` skips shadowed names), boxes them via `boxVarAtScope` (shared storage — capture is by reference), packs handle+boxes into an RC block with a generated `__clos_dtor_*`, and lifts the literal to `__skink_fnlit_N(i8* %env, ...)` that unpacks `env[i+1]` into borrowed `heapSlot` bindings. Non-capturing literals use a constant `{i8* code}` global; named fns referenced as values get `__clos_t_*` thunks. Calls through locals/fields/globals/index exprs use `emitClosureCall` (`code(env, args...)`); fn-typed locals shadow same-named functions. `exprCallLLType` falls back to the checker's recorded type for arbitrary callees (was `i32`, which emitted `inttoptr i32` on `i8*` call results — invalid IR, e.g. `self.middlewares[i](final)` in web/router).
- **Generic-call fixes (comp_error.md #31)**: `std/checker.skink` + `std/emitter.skink` — `inferGenericSubst` returns the arg types it checked and `checkCallArgsWithTypes` reuses them (nested generic calls were O(2^N)); `genericFnDecl` retries `fnDecls` with the module-qualified name (generic calls in non-`main` modules reported "undefined type T"); `coerceGenericCallResult` maps erased `i8*` call results back to the substituted type (`idOf(42)` was printed as a string pointer → segfault).
- **`x.ptrfield.sub` receiver miscompile (comp_error.md #32)**: `std/emitter.skink` — `emitAddressOf`'s EX_FIELD path used `&x.f` as the GEP base even when `f` is pointer-typed — `&x.ptrfield.sub` and pointer-receiver method calls on subfields read the pointer slot as the struct. Now loads the pointer value when the target type ends in `*`. Note the *bootstrap* binary still miscompiles this pattern in compiler sources — `matchExprResultLLType` uses a `ch := cg.checker` local workaround.
- **Lexer O(N²) scan (comp_error.md #22)**: `std/lexer.skink` — `readChar`/`peekChar`/`peekChar2` called `str.Len(input)` (libc `strlen`, O(file size)) per character. Length is cached in the `Lexer` struct; `skink -check skink.skink` went ~24 s → ~8.8 s.
- **Deref-statement parse ambiguity (comp_error.md #33)**: `std/parser.skink` — `*p = v` after an expression statement parsed as `prev * p` (infix `*` continues across newlines). `parseExpression` now breaks the infix loop on `*ident` at a new line, mirroring the `<-` chan send/recv rule.
- **Signed/unsigned check scope (comp_error.md #34)**: `std/checker.skink` — `checkInfixExpr` rejected *any* mixed signed/unsigned integer operands, so `uint16 >> int` was a compile error even though signed shift counts are conventional. The guard now applies only to `==`, `!=`, `<`, `<=`, `>`, `>=`; arithmetic and bitwise/shift ops accept mixed signedness (the emitter already unifies operand widths and picks `lshr`/`udiv`/`urem` via `unsignedOps`).
- **Web server hardening (comp_error.md #45–#66, #68)**: `std/web/{server,types,router,static}.skink` + `std/net.skink` + `std/runtime/{panic_rt,conc_rt,net_rt,tls_rt}.c` — full HTTP/1.1 stack rework: Content-Length framing with a persistent leftover buffer enables keep-alive and pipelining (previously the server blocked in `recv` until the client closed); `Transfer-Encoding: chunked` request decoding; `Expect: 100-continue`; HEAD routed through GET handlers; percent-decoded paths/query params; case-insensitive request headers; response-header CR/LF sanitization; binary-safe bodies (`Request.bodyBuf/bodyLen`, `ResponseWriter.WriteBytes`, explicit-length static file reads); `Request.params` map for route params (was smuggled in headers as `__param_*`); closure-based `FileServer` roots (was one module-global). Panic state moved to `__thread` globals with a per-connection recover → 500. `Skink_send_all`/`Skink_tls_write_all` loop short writes; `SO_REUSEADDR` + backlog 128 in `net.Listen`; per-conn read/write timeouts (default 30 s); `maxRequestBytes` cap (default 1 MiB → 413); maxConns counting semaphore via Mutex+Cond (Skink channels are unbounded — `Skink_chan_send` never blocks); accept-error exponential backoff; `Server.Shutdown()` does `shutdown(fd, SHUT_RDWR)` (close alone does not wake a blocked `accept` on Linux) then drains via WaitGroup. `Skink_web_spawn_conn`/`Skink_web_spawn_tls_conn` trampolines updated for the closure ABI (handler is a `{code}` handle, not a raw fn ptr). Live-socket regressions in `std/web/server_live_test.skink` (19 tests).
- **Non-decimal int literals in i64 casts (comp_error.md #44)**: `std/emitter.skink` — `emitCast` returned `expr.strVal` verbatim for i64-cast literals (needed for values exceeding the parser's i32 `intVal`), so `uint64(0x0f)` emitted `and i64 %r, 0x0f` — LLVM reads leading `0x` as a hex *float* constant. `intLitToI64String` normalizes `0x`/`0b`/`0o` literals (plus `_` separators, also illegal in IR) to decimal.
- **TLS SNI/ALPN/session resumption (comp_error.md #67)**: `std/runtime/tls_rt.c` + `std/api.skink` + `std/web/server.skink` — server ctxs get `SSL_CTX_set_session_id_context`, an ALPN select callback (`http/1.1`), and a `tlsext_servername` callback that rebinds the SSL to a per-hostname child `SSL_CTX` registered via `Skink_tls_ctx_add_cert` / `Server.AddTLSCertificate`. Clients share one lazily-created `SSL_CTX` (sessions are ctx-scoped — the old per-connect ctx made resumption impossible), send real SNI via `SSL_set_tlsext_host_name` (the old `SSL_set1_host` only configured *verification*), offer ALPN, and cache `SSL_SESSION`s keyed by `host:port` — harvested at `Skink_tls_close` because TLS 1.3 tickets arrive post-handshake. `api.Client.insecureSkipVerify` disables cert verification for tests. Live regressions in `std/web/server_tls_live_test.skink` (openssl-generated certs).
- **SIGPIPE kills the process on closed-peer writes (comp_error.md #70)**: `std/runtime/net_rt.c` — POSIX constructor now does `signal(SIGPIPE, SIG_IGN)`. OpenSSL's socket BIO uses plain `write()` (no MSG_NOSIGNAL possible), so a TLS client closing mid-handshake killed the whole server; plain sends had the same exposure outside `Skink_send_all`.
- **for-in over nil slice crashes (comp_error.md #69)**: `std/emitter.skink` — `emitForInStmt`'s array path loaded the length header at `ptr-8` unconditionally; a nil slice (e.g. never-initialised field of a pointer-to-struct) segfaulted. Now nil-guards via `icmp eq` → branch → `phi i64 [0, %zero], [%loaded, %load]`.

## New Standard Library Modules

- **`std/nn.skink`** — Single-hidden-layer neural network with sigmoid activation, feedforward, and backpropagation training.
- **`std/nlp.skink`** — Basic NLP utilities: tokenization, sentence segmentation, rule-based POS tagging, word count.
- **`std/logic.skink`** — Simple Prolog-style logic engine with unification, facts, rules, and backtracking query evaluation.
- **`std/log.skink`** — Structured logging with configurable levels (Debug/Info/Warn/Error/Fatal) and file writer support.
- **`std/benchmark.skink`** — Enhanced benchmarking with `Elapsed()`, `PerOp()`, and improved `Report()`.

## Sample Programs

All samples are in the `samples/` directory and compile with `SKINK_HOME="$(pwd)" ./skink -o <output> samples/<name>.skink`:

- **`web_server.skink`** — HTTP web server with routing using `std/web`.
- **`api_client.skink`** — HTTP API client using `std/api` (GET, POST JSON).
- **`json_parsing.skink`** — JSON parsing and marshalling using `std/json`.
- **`neural_network.skink`** — Neural network training on the AND function using `std/nn`.
- **`nlp_processing.skink`** — NLP tokenization, POS tagging, and sentence segmentation using `std/nlp`.
- **`benchmark.skink`** — Benchmarking string concatenation and Fibonacci using `std/benchmark`.
- **`logic.skink`** — Family tree queries with facts, rules, and backtracking using `std/logic`.
- **`logging.skink`** — Structured logging at different levels using `std/log`.
- **`csv_processing.skink`** — CSV reading, writing, and data manipulation using `std/csv`.
- **`xml_parsing.skink`** — XML parsing, tree traversal, and serialization using `std/xml`.
- **`yaml_config.skink`** — YAML config parsing, value extraction, and serialization using `std/yaml`.
- **`concurrency.skink`** — Goroutines, channels, WaitGroups, and Mutexes using `std/sync` and `std/waitgroup`.
- **`timer_ticker.skink`** — Timer and Ticker for time-based operations using `std/time`.
- **`file_io.skink`** — File reading, writing, appending, and directory operations using `std/fs` and `std/path`.
- **`streaming.skink`** — Byte-stream I/O between disk files and in-memory buffers using Reader/Writer template interfaces, `streams.Pipe`, `io.Copy`, and stream combinators (TeeReader, LimitReader, MultiWriter).
- **`regex_matching.skink`** — Regex compilation, matching, finding, and replacement using `std/regex`.
- **`hashmap_usage.skink`** — HashMap creation, insertion, lookup, and key existence checks using `std/hashmap`.
- **`sort_examples.skink`** — Sorting integers, floats, strings, and reversal using `std/sort`.
- **`base64_encoding.skink`** — Base64 encoding and decoding using `std/base64`.
- **`random_numbers.skink`** — Random number generation, dice simulation, and distribution checks using `std/rand`.
- **`context_timeout.skink`** — Context cancellation, timeouts, deadlines, and value propagation using `std/context`.
- **`reflect_demo.skink`** — Runtime type reflection and struct field inspection using `std/reflect`.
- **`math_functions.skink`** — Math constants, trigonometric, exponential, and power functions using `std/math`.
- **`path_operations.skink`** — File path joining, splitting, cleaning, and inspection using `std/path`.
- **`graph_compute.skink`** — Computational graph (autograd) for neural networks using `std/graph` and `std/tensor`.
- **`select_channels.skink`** — Select statement for channel operations (receive, send, default, multi-channel) using built-in `select` keyword.

## Delete Builtin

- **`delete(x)` builtin**: `std/checker.skink` + `std/emitter.skink` — Added `delete` builtin function that explicitly releases maps, arrays/slices, strings, and struct-by-value objects from memory. Registered in the checker's global scope with signature `func(anyType()) -> void`. The emitter handles it in `emitCallExpr`:
  - For heap-allocated types (pointers, strings, slices, maps): calls `Skink_rc_release` to decrement the reference count, then nulls out the variable to prevent double-free at scope exit.
  - For struct-by-value types: spills to a temporary alloca and calls `emitReleaseStructHeapFields` to release all heap-allocated fields, then zeros out the variable to prevent double-release at scope exit.
  - If the argument is a simple variable (`EX_IDENT`), the variable is zeroed/nullified after release so the scope-exit cleanup (`emitDeallocationsForScope`) skips it.

## DWARF Debug Info Emission

- **`-g` flag**: `skink.skink` — Added `-g` command-line flag that enables DWARF debug info emission. When enabled, passes `-g` to clang and sets `debugInfo=true` in `newCodeGen`.
- **Source file tracking**: `std/ast.skink` + `std/resolver.skink` — Added `filePath` field to `Stmt` struct, set by `loadFile` in the resolver so every declaration knows its source file.
- **Debug metadata fields**: `std/emitter.skink` — Added `dbgEnabled`, `dbgMetaCounter`, `dbgFiles`, `dbgFilePaths`, `dbgCurrentFile`, `dbgCurrentSubprogram`, `dbgCurrentLoc`, `dbgDefaultLoc`, `dbgCurrentLine`, `dbgCurrentCol`, `dbgCompileUnit`, `dbgTypes`, `dbgStructTypes`, `dbgMetadata` fields to `CodeGen`.
- **Metadata helpers**: `std/emitter.skink` — `nextDbgMeta`, `setDbgMeta`, `getOrCreateDIFile`, `dbgType`, `dbgStructType`, `emitDbgLocation`, `clearDbgLocation`, `getOrCreateDefaultLoc` functions for generating LLVM debug metadata nodes.
- **Auto-!dbg on instructions**: `std/emitter.skink` — `emitln` automatically appends `, !dbg !N` to instruction lines (2-space indent) when debug info is active. Uses a cached default line-0 location as fallback so all instructions in a debug function have a `!dbg` attachment (required by LLVM verifier).
- **DISubprogram emission**: `std/emitter.skink` — `emitFnDecl` creates `!DISubprogram` for each function with file, line, subroutine type, and compile unit references. Emits `#dbg_declare` for parameters.
- **DILocation per statement**: `std/emitter.skink` — `emitStatement` calls `emitDbgLocation` with the statement's line/col. `emitLabel` clears the location.
- **DILocalVariable for locals**: `std/emitter.skink` — `declareVar` emits `!DILocalVariable` and `#dbg_declare` for local variables.
- **DICompileUnit + module flags**: `std/emitter.skink` — `emitDebugMetadata` emits `!DICompileUnit`, `!llvm.dbg.cu`, `!llvm.module.flags` (Debug Info Version + Dwarf Version), and all collected metadata at the end of `emitProgram`.
- **Bootstrap lexer workaround**: The bootstrap lexer doesn't handle `!\"` inside string literals correctly. Module flag strings with `!` before escaped quotes are built using `str.Builder` + `WriteByte(34)` to construct the quote character separately.

## Known Limitations

## Newly Implemented Language Features

The following language features were implemented to align the compiler with the book documentation:

- **`for i, v in collection`** — Two-variable for-in iteration. For arrays/slices, the first variable is the index and the second is the element. For maps, the first is the key and the second is the value. Parser stores both in `forIterVar` and `forIterVar2`.
- **`delete(map, key)`** — Removes a key from a map by scanning the keys array, shifting remaining elements down, and decrementing the length.
- **If-initializers** — `if x := expr; cond { ... }` declares a variable scoped to the entire if statement (both then and else branches). Parser detects the semicolon after the init statement.
- **Map comma-ok** — `v, ok := m[key]` returns both the value and a boolean indicating whether the key exists. Implemented via `emitMapIndexOk`.
- **Channel receive comma-ok** — `v, ok := <-ch` returns both the value and a boolean indicating whether the channel had data (true) or was closed and empty (false).
- **Named returns** — `fn f() -> (result: int, ok: bool) { ...; return }` declares named return variables, initializes them to zero values, and bare `return` uses their current values. The parser uses colon syntax (`name: type`) consistent with parameter syntax.
- **Parenthesized const blocks** — `const ( ... )` syntax now supported in addition to `const { ... }`.
- **Spread append** — `append(slice, anotherSlice...)` iterates over the source slice and appends each element to the destination. The parser recognizes the `...` suffix on call arguments and creates `EX_SPREAD` expressions.
- **JSON Get* helpers** — `json.GetString`, `json.GetNumber`, `json.GetInt`, `json.GetBool`, `json.GetObject`, `json.GetArray` convenience functions.
- **`net.Dial`** — Alias for `net.Connect`, matching the book's API.

### Book-Audit Fixes (megaplan remediation)

Compiler/emitter fixes made while aligning the book with the implementation:

- **Anonymous function definitions at module scope**: `emitFnLit` writes the
  generated `define` into `cg.anonFns` (emitted at module scope) instead of
  inside the enclosing function body, and emits deferred statements + scope
  cleanup before the final return. `cg.currentModule` is preserved inside
  function literals so calls resolve module-qualified names (e.g.
  `panic_recover_test.recSave`).
- **Indirect calls through function variables**: `emitCallExpr` resolves
  local variables holding function pointers (`TK_FUNCTION` synType or
  `R (args)*` llType) and emits `emitIndirectCall` with composite-ABI
  parameter types. `exprCallLLType` returns the variable's declared return
  type for such calls.
- **Match arms**: final arms that are variant patterns now go through tag
  comparison + `bindVariantPayloads` instead of being treated as unconditional
  fallbacks. Bare variant patterns (`RegEmpty => ...`) count toward
  exhaustiveness in the checker. Match-expression result type is recorded by
  the checker (`matchResultTypes` keyed `line:col`) so the emitter doesn't
  rely on the first arm's last expression type (pattern-bound names aren't in
  scope then). Trailing `if/else` statements in arm bodies emit in value
  position via `emitIfValueStmt`.
- **`_` blank identifier**: `_ = expr` is legal (value discarded);
  `a, _ = f()` tuple targets skip checking/storage.
- **Integer→float coercion**: `coerceArgToParamType` emits `sitofp` for int
  args to `double` params; math builtins (`sin`, `cos`, `tan`, `sqrt`, `pow`)
  coerce args the same way.
- **Generic erasure coercions**: `inttoptr` for int args → erased `i8*`
  params, `ptrtoint` for erased `i8*` values → int params (fixes
  `hashmap.Get` returning values passed to `io.Itoa`).
- **Static vs instance methods**: `checkFieldAccessExpr` distinguishes type
  bindings from variables so `Type.Method()` doesn't get an implicit receiver.
- **Generic struct literals**: `checkStructLit` preserves generic args so
  `Stack<int>{}` methods see substituted `T`.
- **Generic-arg inference for unqualified calls**: `inferGenericSubst` maps
  `T`→arg type from call args when the callee declares generic params.
- **String relational comparison**: `<`, `>`, `<=`, `>=` on `i8*` operands
  use `strcmp` sign comparison (previously pointer `icmp`, which was wrong).
- **`delete(m, key)`** key type checks `mapType.keyType` (was `elem`).
- **`delete(x)`** (1-arg) releases heap values and nulls the variable.
- **`os.Exec`/`os.ExecWithOutput` argv marshalling**: stores the *pointer* to
  each C string (`&args[i]`), not the string contents.
- **`std/path`**: added missing `std/str` import.

### Robustness audit round (comp_error.md #71–#93)

- **Channel semantics** (`std/runtime/conc_rt.c`): `send` on a closed channel
  panics "send on closed channel"; `close` of a closed channel panics
  "close of closed channel"; `make(chan, N)` with `N > 0` is now a bounded
  queue — `send` blocks on `send_cv` until a slot frees (cap 0 stays
  unbounded).
- **Div/mod by zero & shifts** (`emitBinaryOp`): `x/0`, `x%0` panic
  "integer divide by zero" (recoverable) instead of SIGFPE; shifts clamp the
  count for IR safety and select 0 when count >= bit width (Go semantics).
- **Element-index bounds check** (#78): `a[i]` reads/writes on slices call
  `Skink_bounds_check(i, i+1, len)`; raw `T*` pointer indexing is exempt
  (no length header); non-i32 indices are coerced for both the check and
  the GEP (#84).
- **Nil-map paths**: `emitMapIndex`/`emitMapIndexOk` route nil maps to
  zero-value results; `emitMapIndexAssignment` panics "assignment to entry
  in nil map" (Go semantics). `delete`/`for k in m` were already nil-safe.
- **Channel `for-in`** (#83): `emitForInChanStmt` implements
  receive-until-close iteration (was emitted as byte-slice iteration,
  producing garbage).
- **Float `!=`** (#85): `fcmp une` (unordered-or-not-equal) replaces
  `fcmp one`; `one` is ordered-and-not-equal, so NaN comparisons were
  wrong. Same fix on the double→bool conversion path.
- **Checker rejections**: `break` outside loop/switch/select and
  `continue` outside loops (`breakDepth`/`loopDepth`); duplicate struct
  fields; generic-call explicit-arg arity; `for i in <int>` (use `a..b`);
  `&m[k]` address-of map element; bare `x.m` method values; `==`/`!=`
  between struct values (pointer-compare silently returned false).
- **Global-var method calls** (#86/#87): `checkCallArgs` and
  `emitMethodReceiver`/`emitAddressOf` now resolve module-level vars via
  the qualified `module.name` key — fixes "wrong number of arguments"
  on `g.Method()` in non-main modules and mutation of a stack copy.
- **Zero-value sync structs** (#82): `RWMutex`, `Cond`, `WaitGroup`
  lazy-init handles via `Skink_*_ensure` (like `Mutex`); `Cond.Wait` with
  no associated mutex panics.
- **Parser depth guard** (#91): `Parser.exprDepth` caps expression
  recursion at 256 (crash was ~350); recovery skips to the closing
  delimiter. `maxParseErrors = 40` caps the diagnostic flood.
- **SIGPIPE** (#70): `net_rt.c` POSIX constructor ignores SIGPIPE —
  OpenSSL socket BIOs can't use `MSG_NOSIGNAL`, so peer disconnects
  mid-handshake no longer kill the server process.
- **TLS** (#67): real client SNI (`SSL_set_tlsext_host_name`), shared
  client `SSL_CTX` + `host:port` session cache (TLS resumption), ALPN
  `http/1.1` both sides, server-side SNI cert switching via
  `SSL_CTX_set_tlsext_servername_callback` + `Server.AddTLSCertificate`.
- **Timer/Ticker send-on-closed** (#93): `std/time.skink` — `Stop`/`Reset`
  signal a buffered `stop` channel; only the goroutine closes `C` on exit.
- **Tensor flat indexing** (#92): `Tensor.GetFlat`/`SetFlat` added;
  `graph.skink` Adam.Step and ReLU-backward use flat offsets instead of
  len-1 coordinate indices (was an OOB read on rank-2 tensors).
  `Get`/`Set` now panic on `len(idx) != rank`.
- **Nil-slice `for-in`** (#69): `emitForInStmt` nil-guards the slice
  pointer before reading the length header.

### Regression Tests

All new features are covered by `std/compiler_features_test.skink` (20 tests).

### Struct String Interpolation Bug

- **Affected versions**: `/usr/local/bin/skink` (built ~Sep 1, 2026)
- **Issue**: `println("{struct_value}")` or `println("{slice_of_structs}")` generates invalid LLVM IR. The `emitValAsString` function in `std/emitter.skink` doesn't handle struct-by-value types (`%struct.*`), returning the struct register as-is. `emitStringConcat` then tries to null-check this struct register as `i8*`, causing a clang type mismatch error.
- **Error message**: `error: '%rN' defined with type '%struct.X = type { ... }' but expected 'ptr'`
- **Source fix applied**: Added `emitStructToString` function and `%struct.` case in `emitValAsString` (in `std/emitter.skink`). The fix stores the struct to an alloca, uses GEP to extract each field, converts each field to a string via `emitValAsString`, and concatenates them into `{field: value, ...}` format.
- **Workaround**: Avoid string-interpolating structs directly. Instead, write a helper function that manually converts struct fields to strings using `strconv.Itoa` / `strconv.Itoa64` / `str.Concat`.

### Bootstrap Compiler File Size Limitation

- **Affected binaries**: Both `/usr/local/bin/skink` and `skink_new` have file size limitations when compiling imported modules.
- **`skink_new`** (Aug 25, 2026): Can handle std modules up to ~1000 lines (e.g., `std/lexer` at 983 lines works). Hangs on larger modules (`std/parser` at 2673+ lines, `std/checker` at 3661+ lines, `std/emitter` at 6000+ lines).
- **`/usr/local/bin/skink`** (Sep 1, 2026): Hangs on all large std modules including `std/parser`, `std/checker`, `std/emitter`.
- **Impact**: `make` (self-compilation) is broken. The bootstrap compiler cannot compile `skink.skink` because it imports `std/emitter` (8831+ lines), which exceeds the bootstrap compiler's handling capacity.
- **Root cause**: Likely a performance/memory issue in the bootstrap compiler's parser or type checker that causes exponential behavior or infinite loops on large input files.
- **Resolver issue**: The resolver's `resolveImport` function walks up the directory tree and finds `./std/` (current version) before falling back to `SKINK_HOME/std/` (bootstrap version). This means even if bootstrap-home has compatible files, the compiler may use the repo's current std files instead. The overlay check (`dirName(searchRoot)`) turns `./bootstrap-home` into `.`, causing it to find `./std/` first.
