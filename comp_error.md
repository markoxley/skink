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

## #140 — `x in s` and `for v in s` segfault on a null set — FIXED 2026-10-20

- **Severity:** high — hard crash (SIGSEGV) at runtime.
- **Symptom:** `for v in h.s` or `x in h.s` where `s` is an uninitialized
  `set<T>` struct field (or any other null set value) segfaults. `len()`
  on the same null set was already guarded (#138); `in` and `for` were not.
- **Root cause:** `emitSetInOp` and `emitForInSetStmt` in
  `std/emitter.skink` GEP'd and loaded `{data, count}` fields through the
  set pointer unconditionally — a null set hits address ~8 and dies.
- **Fix:** both emitters now emit `icmp eq %set.T* reg, null` first and
  branch to the shared end label — `in` reports false, `for` iterates zero
  times. (The `in` fix uses the result-alloca path already present;
  `endLabel` was hoisted above the field loads in both functions.)
- **Regression:** `std/sets_test.skink` `TestNullNativeSetOps` /
  `TestFromSetEmpty`.
- **Workaround (pre-fix):** always assign a non-empty `set{...}` literal to
  set fields, or guard with `len(s) > 0`.

## #141 — erased `i8*` element comparisons use `strcmp`/`Skink_str_hash` — FIXED 2026-10-21

- **Severity:** high — hard crash inside generic (template) code.
- **Symptom:** inside a generic function/type, `x in s` on `set<T>`,
  `a == b` on two erased values, or `m[k]`/`m[k]=v`/`delete(m,k)`/
  `for k in m` on `map[K]V` crashes when the concrete element/key type is
  not a string — erased `i8*` values are `inttoptr`'d integers and
  `strcmp`/`Skink_str_hash` dereferences them.
- **Fix (sets):** `emitSetElemCompare` calls `reflect_equals(a, b, "")`
  for `i8*` elements — mapped pointers compare by string content, unmapped
  values by raw bits — so `in` and literal dedup work for erased
  int/bool/float/pointer elements. Real `set<string>` behavior is
  unchanged.
- **Fix (maps):** new `emitMapKeyHash`/`emitMapKeyCompare` route `i8*`
  keys through `reflect_hash`/`reflect_equals` with a type-name hint from
  `mapKeyReflectName` — `"string"` when `exprTypeLookup` proves the map's
  semantic key type is builtin string (same hash/compare semantics as
  `Skink_str_hash`/`strcmp`, no heuristic overhead), `""` for erased keys
  (mapped-memory heuristic). Threaded through `emitMapIndex`,
  `emitMapIndexOk`, `emitMapIndexAssignment`, `emitDeleteCall`, and map
  literals. `reflect_hash` is declared beside `reflect_equals` in the
  runtime preamble.
- **Fix (== / !=):** `emitInfixExpr`'s `i8*` equality path previously
  `strcmp`'d both operands (a literal-only `isIntegerLiteralExpr` escape
  existed, but `x == y` on two generic vars still crashed). Both operands
  now go through `reflect_equals` with a `"string"` hint only when both
  sides are provably builtin string — a mixed string/erased pair keeps the
  heuristic so `inttoptr` values are never dereferenced. Real string `==`
  semantics are preserved.
- **Regression:** `std/sets_test.skink` (set side);
  `std/compiler_types_test.skink` `TestGenericMapScalarKeys`,
  `TestGenericMapStringKeys`, `TestGenericErasedEquality` (map and ==/!=
  sides — all segfaulted before the fix).

## #142 — field access on an instantiated generic struct returns the erased type — FIXED 2026-10-21

- **Severity:** medium — wrong static type; runtime crash if the value is
  used as its concrete type.
- **Symptom:** `w := WrapA<int>{elems: []int{3,4}}` then `w.elems[0]`
  (field `elems: []T`) yielded an erased `i8*` element —
  `println(w.elems[0])` treated it as a string pointer and segfaulted
  (`strlen` on `inttoptr(3)`). Scalar `v: T` fields similarly read raw
  `inttoptr` boxes. By contrast, inside `fn f(w: WrapA<int>)`,
  `w.elems[0]` was correctly `int`.
- **Root cause:** not the checker — `checkFieldAccessExpr` already
  substitutes `T→int` via `genericArgs`, and the struct literal records
  them. The emitter's `exprFieldLLType`/`emitFieldAccessExpr` derived the
  field type solely from the single erased `StructLayout` slot
  (`[]T → i8**`, `T → i8*`), so `exprLLType(w.elems)` reported `i8**` and
  every downstream consumer (index loads, for-in, append, println) used
  the erased element type.
- **Fix:** when the receiver's semantic type is an instantiated generic
  (`receiverIsGenericInstance` — `genericArgs` non-empty on the named or
  pointed-to type), `exprFieldLLType` returns `llvmType` of the checker's
  substituted field type instead of the layout slot type, and
  `emitFieldAccessExpr` casts the loaded slot value to that type via the
  new `emitCastLL` (i8* → concrete unboxes through `emitSetElemUnbox`,
  pointer → pointer bitcasts). Compound assignment (`w.v += 1`) unboxes
  before arithmetic and reboxes for the store. Indexing, `for-in`,
  `append`, `len`, and `delete` then see `[]int`/`int`/`map[int]V`
  automatically because they all derive element types through
  `exprLLType`.
- **Regression:** `std/compiler_types_test.skink`
  `TestGenericStructFieldAccess`, `TestGenericStructFieldWrite`,
  `TestGenericStructStringFields`, `TestGenericStructNestedFields` —
  `w.elems[0]` on `GenWrap<int>` segfaulted before the fix.

## #143 — `set<T>` element storage is element-sized, so erased generic reads misalign — FIXED 2026-10-20

- **Severity:** high — silent data corruption inside generic code.
- **Symptom:** passing a concrete `set<int>` to a generic `s: set<T>`
  parameter produced garbage on `for v in s` / `x in s` — the erased
  reader walks the backing array as `i8*` (8-byte stride) while the
  literal stored raw `i32` elements (4-byte stride). `sets.FromSet[int]`
  returned a `Set` full of misaligned values and `Has`/`Contains` missed
  real members.
- **Root cause:** set backing arrays were `elemLL*` — element-sized —
  but `set<T>` parameters lower to `%set.i8Ptr*`, whose readers assume an
  `i8*` element stride. Unlike `[]T` and `map[K]V` (which the checker
  rejects for non-pointer-sized instantiation), `set<T>` params were not
  guarded, so the mismatch compiled silently.
- **Fix:** uniform boxing. `emitSetTypeDecl` now always declares
  `{ i8**, i32 }`; `emitSetLiteral` boxes every element via the new
  `emitSetElemBox` (`inttoptr` for integers, `bitcast`-through-`inttoptr`
  for floats, `bitcast` for pointers/strings/structs, RC-heap-copy for
  by-value composites); `emitSetInOp`, `emitForInSetStmt`, and the
  literal's dedup scan read `i8*` and unbox via `emitSetElemUnbox`.
  Concrete semantics are unchanged (int `in` still lowers to `icmp` after
  the unbox); generic `set<T>` now reads the same layout the literals
  write.
- **Regression:** `std/sets_test.skink` — all `TestNative*` cases plus
  `TestFromSet`/`TestFromSetEmpty` exercise a concrete `set<int>` across
  a generic `set<T>` boundary.
- **Residual (re-assessed):** a `set<T>` *literal* emitted inside generic
  code still gets the i8* array destructor, which calls
  `Skink_rc_release` on each element — but release is table-guarded
  (`rc_table_find` in `rc_release_locked` returns early for pointers not
  in the live-allocation table), so `inttoptr`'d scalars are safe no-ops
  rather than wild frees. Verified: `mkAndProbe<int>` creating and
  releasing `set{a,b}` inside a generic fn runs clean. The only remaining
  hazard is theoretical: an integer value that numerically aliases a live
  RC pointer would be released — inherent to untagged erasure, same as
  the mapped-memory heuristic in `reflect_equals`/`reflect_hash`.
  By-value composite set elements remain unhandled (`icmp` cannot compare
  aggregates); the box path merely keeps storage correct.

## #144 — libc `size_t`/`ssize_t` externs declared as `int` — FIXED 2026-10-21

- **Severity:** High (latent memory corruption; flaky crash dependent on register allocation)
- **Symptom:** `std/web/server_live_test` segfaulted intermittently inside `__memmove_avx_unaligned_erms` called from `server.Server.readRequest`, with the copy length register holding e.g. `0x10000002f` — a valid 32-bit length (47) in the low dword and unrelated register residue (`1`) in the high dword.
- **Root cause:** `std/libc.skink` declared `memcpy`/`memset`/`strncpy` with `n: int` (i32) where C expects `size_t` (i64). LLVM passes the i32 argument in `edx` and is free to leave garbage in the upper 32 bits of `rdx`; the libc callee reads the full 64-bit `size_t` and copies gigabytes. Whether a given build leaves dirty upper bits depends on register-allocation choices, which is why identical sources produced both crashing and passing binaries.
- **Fix:** audited every extern taking C `size_t`/`ssize_t`/`off_t`/`socklen_t`-style parameters (and `size_t`/`ssize_t` returns) across `std/libc.skink`, `std/io.skink`, `std/os.skink`, `std/path.skink`, `std/crypto.skink`, `std/net.skink`, `std/i2c.skink`, and `std/mcp.skink`; all now use `int64`. Call-site arguments auto-coerce (`int` args are `sext`'d to `i64` at extern calls), so no call sites needed changes.
- **Diagnostic note:** the crash reproduced only via `skink -t`, which honors the ambient `SKINK_HOME` env var — it was building test binaries against a stale `~/.local/skink/std` install that still carried the old `int` declarations. Repo builds (`SKINK_HOME=<repo>`) always emitted the fixed `i64` calls.
- **Residual hazard:** any third-party/new extern declaration with `size_t`-family parameters declared `int` reintroduces the same silent corruption; there is no checker rule yet that warns on width-mismatched libc prototypes.

## #145 — `skink_is_mapped` page cache races under spawn routines — FIXED 2026-10-21

- **Severity:** Medium (wrong comparison results; theoretical misclassification crash)
- **Symptom:** none observed directly — found by inspection while diagnosing #144.
- **Root cause:** `std/runtime/reflect_rt.c` kept a process-wide 1-entry `{cached_page, cached_result}` cache. Concurrent spawn routines could observe `page == cached_page` written by another thread while reading their own stale `cached_result`, returning the wrong mapped/unmapped verdict for erased `i8*` values in `reflect_hash`/`reflect_equals`.
- **Fix:** cache moved to `__thread` storage — per-thread memoization, no locking.
- **Residual hazard (unfixed, inherent):** the mapped-memory heuristic classifies an erased value as "string" if its *start* page is mapped. `skink_streq`/`skink_hash_string` then walk bytes unboundedly; a misclassified `inttoptr` value near a page boundary can read into an unmapped page and segv. Fully fixing requires bounded/validated scans or richer type tagging at erasure sites.
