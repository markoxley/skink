# Skink Compiler Bugs / TODOs

Canonical running list of compiler issues discovered while building Skink
projects (entries #1–#18 migrated from CHRIS). Original binary:
`/usr/local/bin/skink` (Sep 15, v0.2.52 era). Current binary:
`/usr/local/bin/skink` **v0.2.59** (Sep 19 install).

**Status as of repo build (current):** issues #1–#43 all FIXED or
resolved. Latest pass (Oct 6): #35/#36 now produce clean checker
rejections instead of miscompiles; #38–#43 fixed (paren-assign parsing,
declaration-time capture boxing, param shadowing, runtime defer stack,
fn-type diagnostics). #22's StrIndex checker tables cut `-check` on
compiler-importing files ~37s → ~1.5s and self-build ~8–10 min → ~67s.

---

## 1. Map struct type not emitted unless `make()` is codegen'd — ✅ FIXED

~~The emitter only writes a map's runtime header definition when it codegens
a `make(map[K]V)` call. Referenced-but-never-instantiated map types stayed
opaque → `getelementptr` into unsized type.~~

Verified fixed: `len()` on a cross-module map type compiles and returns 0.

---

## 2. `for k, v in` over `map[string]*Struct` emits invalid IR — ✅ FIXED

~~Loop value used mangled name `struct_X*` (missing `%`); field access on the
loop var used the raw pointer.~~

Verified fixed: `for k, v in m` + `v.field` compiles and runs correctly.

---

## 3. Map store `m[k] = v` double-dereferences pointer lvalues — ✅ FIXED

~~`m[k] = nh` / `m[k] = &T{}` stored the first 8 bytes of the struct as the
pointer → garbage on read → segfault.~~

Verified fixed: pointer variables and literals store correctly.

---

## 4. `var x = []T{const1, const2}` — inferred-type module var checked before consts resolve — ✅ FIXED (Sep 18 repo build)

**Severity:** compile failure on valid code — narrow trigger, easy workaround

**Symptom** (still present in v0.2.58):

```
tc.skink 8:25: array element 2 type mismatch: expected A, got B
```

**Root cause — checker pass ordering, not const typing itself.**

In `std/checker.skink` `checkProgram`:

1. **Pre-registration** (~line 1593): each const is declared with a
   placeholder type `namedType(constName)` — `bindings["A"].typ =
   namedType("A")`. This is intentional, for forward references.
2. **First pass** (~line 1747–1757): `ST_VARDECL` *without* a type
   annotation runs `checkExpr(c, decl.varValue[0])` immediately to infer
   the declared type. At this point const bindings still hold the
   placeholder named types, so `checkArrayLit` (~line 3573–3610) compares
   `namedType("A")` vs `namedType("B")` with strict `typeEquals` →
   reports the phantom mismatch.
3. **Const pass** (~line 1776–1790): `checkConstDecl` (~line 1902–1933)
   rewrites `b.typ` to the initializer's real type (`string`) and clears
   `c.typeCache`.
4. **Second pass**: `checkVarDecl` (~line 1887–1898) re-checks the same
   initializer — consts now resolve to `string`, elements unify, no error.

The reported error fires exactly once and is a first-pass artifact; the
type system itself is fine.

**Verified evidence (v0.2.58):**

```skink
const A = "a"
const B = "b"

var bad  = []string{A, B}          // ❌ "expected A, got B"
var good []string = []string{A, B} // ✅ compiles and runs — annotation
                                   //    path skips the pass-1 checkExpr
fn main() {
    local := []string{A, B}         // ✅ works — checked after const pass
    takes(A); takes(B)              // ✅ consts fine in arg/assign positions
}
```

**Fix options (compiler):**

- **Reorder**: run const resolution (`checkConstDecl` pass) *before* the
  first-pass var-initializer inference, so placeholders are already
  rewritten when `checkExpr` runs. `resolveConstNamedType` already
  handles forward refs.
- **Defer/suppress**: the pass-1 `checkExpr` on var initializers exists
  only to infer the declared type — `checkVarDecl` rechecks in pass 2
  anyway. Either declare the var `anyType()` in pass 1 and let pass 2
  fill in the real type, or suppress `report` while the inference check
  runs.
- Relaxing `checkArrayLit`'s `typeEquals` alone won't fix it — at pass-1
  time the placeholder has no underlying type recorded to unify with.

**Workaround (project code):** annotate the var —
`var names []string = []string{CONST1, CONST2}` works today. For CHRIS
`hormoneNames` can become `var hormoneNames []string = []string{DOPAMINE, ...}`
immediately, no compiler fix needed.

---

## 5. `len()` on a nil map segfaults — ✅ FIXED

Verified fixed: returns 0.

---

## 6. Zero-value `sync.Mutex` segfaults on `Lock()` — ✅ FIXED

~~`Mutex{handle: int64}` zero value passed `handle = 0` to
`Skink_mutex_lock` → SIGSEGV.~~

Verified fixed: `Mutex{}.Lock()/Unlock()` works (lazy init presumably).

---

## 7. `spawn` on a method call emits a synchronous call — ✅ FIXED

~~`spawn self.loop()` emitted a plain `call` — spawn silently dropped for
method receivers.~~

Verified fixed: `spawn self.m()` runs concurrently.

---

## 8. `chan<Struct>` delivers corrupted data — ✅ FIXED

~~Struct values through channels arrived as garbage in every field.~~

Verified fixed: `HormoneUpdate`/`HormoneOutput` arrive intact.

---

## 9. `chan<*Struct>` send emits invalid IR — ✅ FIXED

~~`inttoptr i32 %r20 to i8*` on a `ptr` operand → clang error.~~

Verified fixed: compiles and delivers correct pointer.

---

## 10. Multiple `const {}` blocks collide on a phantom "" declaration — ✅ FIXED (v0.2.58)

~~Each `const {}` block registered a declaration with an empty name, so a
second block in the same module failed with
`variable "" already declared in this scope`.~~

Verified fixed: multiple `const {}` blocks in one module compile.

---

## 11. `pub` on a `const {}` block doesn't parse/apply — ✅ FIXED (v0.2.58)

~~`pub const { ... }` hit the phantom `""` collision; `pub` inside a block
failed to parse.~~

Verified fixed: `pub const { ... }` compiles and exports each item.

---

## 12. Module-level var initializer in non-main module emits unqualified const refs — ✅ FIXED (repo build)

**Severity:** compile failure on valid code — surfaced by the #4 fix

**Symptom:**

```
prog.ll: global variable reference must have pointer type
  store i32 @K1, i32* %r21
```

**Root cause:** `_skink_global_init` and the `globalVarTypes`
registration passes emitted each module-level `var` initializer with no
module context, so `var Names = []string{K1, K2}` in module `varslib`
emitted `@K1` instead of resolving `varslib.K1` from `cg.consts`.

**Fix (std/emitter.skink):** `setModuleContextFromName` derives the
owning module from the resolver-qualified var name (`varslib.Names` →
`varslib`; unqualified → `main`), applied at both `globalVarTypes`
registration sites and per-statement in `_skink_global_init`.

---

## 13. `pub const` initializer referencing same-module symbols fails cross-module — ✅ FIXED (repo build)

**Severity:** compile failure on valid code

**Symptom:** same `global variable reference must have pointer type`
LLVM error, e.g. `store i32 @K` for:

```skink
// module slicelib
const K = "k"
pub const Names = []string{K, "z"}

// module main — referencing slicelib.Names
```

**Root cause:** `cg.consts` stores the const's initializer AST and
re-emits it inline at every reference site under the *caller's* module
context, so `slicelib.Names` emitted in `main` couldn't resolve `K`.

**Fix (std/emitter.skink):** `emitConstExpr`/`constExprLLType` wrap
`emitExpression`/`exprLLType` with `setModuleContextFromName` on the
const's qualified key, applied at all `cg.consts` lookup sites
(`emitIdent`, `emitFieldAccessExpr`, `exprIdentLLType`,
`exprFieldLLType`). Also covers private-fn calls (`helper(10)`) and
struct literals (`Point{x:1}`) inside exported const initializers.

---

## 14. Negative int literal in narrow-int struct field emits wrong width — ✅ FIXED (repo build)

**Severity:** compile failure (LLVM/clang error) on valid code

```skink
o := hormones.HormoneOutput{..., Wellbeing: -5}   // Wellbeing: int16
```

```
bin/main.ll:40665:13: error: '%r8' defined with type 'i32' but expected 'i16'
  store i16 %r8, i16* %r9
```

The `-5` unary-minus expr emits `i32` and is stored into the `i16` field
with no truncation. Positive literals emit at field width correctly.

**Fix:** `emitStructLiteral` now runs every field value through
`coerceArgToParamType` (trunc/sext/etc.) against the declared field type.
The same missing-coercion class was also fixed in: `emitVarDecl`
(`var a int16 = -5`), `emitReturnStmt` (single + multi returns),
`emitMapIndexAssignment` (`m[k] = -9` into `map[string]int16`),
`emitInfixExpr` (`i16var == -5`, `i16var + 1`, mixed `i16 + i64` — operand
widths are now unified to the operation type, subsuming the old i64-only
promotion), and `emitPrefixExpr` (`-x`/`~x` on narrow ints emit at the
operand's own width).

**Verified:** struct fields, var decls, returns, map values, comparisons,
arithmetic, unary ops, module-level struct literals — all narrow widths.

---

## 15. `println` renders `int16`/`int8` as unsigned — ✅ FIXED (repo build)

`println(int16(-5))` printed `65531` — printf varargs read a full i32 so
the narrow register's zero-extended bits leaked through.

**Fix:** `emitPrintCall` extends `i8`/`i16` args to `i32` before
formatting — `sext` for signed, `zext` for unsigned. Signedness comes
from the checker's new `exprTypes` map (records each expression's type
keyed by `file line:col`; `checker.exprTypeLookup`), since LLVM types
lose signedness. `uint8(250)`/`uint16(60000)` print correctly.

---

## 18. String interpolation fails for narrow int types — ✅ FIXED (repo build)

**Symptom:** `"v = {v}"` where `v` is `int8`/`uint8`/`int16`/`uint16` fails in
clang:
`error: '%rN' defined with type 'i16' but expected 'ptr'` on
`icmp eq i8* %rN, null`.

**Root cause:** `emitExprAsString` fell through to `emitValAsString`,
which didn't handle `i8`/`i16` and returned the raw register;
`emitStringConcat` then null-checked it as `i8*`.

**Fix:** `emitExprAsStringT` detects `i8`/`i16`, extends to `i32`
(sext/zext via the checker-recorded type) and stringifies with
`emitIntToString`. Interpolation-part exprs are sub-parsed from the
literal text so their `line:col` are fragment-relative and collide;
`checkInterpExpr` therefore records each part's type under the parent
string's position + `#index` (`checker.exprTypeLookupAt`). `[]int16`
slice printing was also broken (i16 regs into `snprintf %d`) — fixed via
the same extension in `emitValAsString`'s fallback.

---

## 17. Sending a struct on a `self.<chan field>` corrupts the value — ✅ FIXED (repo build)

**Symptom:** Inside a method, `self.out <- o` (chan stored in a struct field)
delivers a struct with garbage fields — `Cortisol: 33` arrives as `32611`,
values look like raw pointer bytes. Runtime delivery still works (receiver
unblocks on the right channel), only the sent value is corrupted.

**Root cause:** `chanElemLLTypeOpt` re-derived the channel's element type
at emit time via `checker.resolveFieldType`, which re-runs `checkExpr` on
`self` — but `self` isn't in the checker's scope during emission, so it
returned unknown → element type `""` → the send boxed a `%struct.S*`
pointer into a slot the runtime memcpy'd as a 16-byte `%struct.S` value.
Same for inferred locals (`oc := self.out` — `synType` is only tracked
for `make()`/fn-lit). This also explains why the *value* was corrupted
while delivery worked.

**Fix:** the checker now records every checked expression's type in
`Checker.exprTypes` keyed by `file line:col` (see `checkExpr` wrapper +
`exprTypeLookup`); `chanElemLLTypeOpt` and `chanRecvElemLLType` consult it
first, so `self.out`, copied locals, and select cases all resolve the true
element type. `emitChanSendBox` then coerces the send value (`%struct.S*`
→ load → `%struct.S`) and boxes the value correctly. `resolveFieldType`
sets `suppressExprTypes` so emit-time rechecks can't pollute the map.
`cg.currentFilePath` is now maintained across global-var init paths so the
lookup works there too, and `constFiles` tracks each const's file for
`emitConstExpr`.

**Verified:** `self.out <- o`, `oc := self.out; oc <- o`,
`select { case self.out <- o: }`, value-receiver struct field channels,
`chan<*P>` through fields — all deliver correct values.

---

## 16. `self.ctx.Done()` on a `*context.Context` field returned garbage — ✅ FIXED

**Severity:** high — segfault/heap corruption on any `&local` that
outlives its frame.

**Reproduced & root cause:** `ctx: &ctx` inside `New()` stored the
address of a **stack alloca** into the heap `Net` struct. After `New`
returned the pointer dangled: `self.ctx.Done()` in the spawned loop read
dead stack memory — a garbage `parent` field made `Done()` recurse until
the stack overflowed (SIGSEGV), and on other runs random contents read
as `true`. Minimal repros earlier "passed" only because the dead frame
wasn't overwritten yet.

**Fix (std/emitter.skink):** escape analysis for address-taken locals.
`collectEscapedVars` pre-scans each function body for `&x` (incl. inside
struct literals, call args, `spawn`/`defer` bodies); `boxVarAtScope`
promotes such locals into an RC-managed heap block whose pointer lives
in a `heapSlot` alloca. `varValuePtr` routes all loads/stores and `&x`
through the block, so the address is stable after return. Also fixed:
`*p = v` (store through a deref lvalue) was silently dropped —
`emitAssignStmt` now handles `EX_PREFIX "*"` targets. Regression tests in
`std/context_test.skink` (`TestContextFieldEscape` — the original
spawn+`self.ctx.Done()` shape, run to clean cancellation) and
`std/compiler_fixes_test.skink` (boxed `int` read/mutation, deref-store).

---

## 19. Unsigned integer widening uses `sext`; unsigned ops use signed predicates — ✅ FIXED (repo build)

**Severity:** high — silent wrong values for `uint8`/`uint16`/`uint32`/`uint64`.

**Symptom:** `int(uint8(250))` evaluated to `-6`; implicit widening like
`takeInt(uint8(250))` (param `int`) also produced `-6`. Additionally,
`uint8(200) > 100` evaluated to `false`, and `uint8(200) / 3` /
`string(int16v)` produced wrong results or invalid IR.

**Root cause:** LLVM integer types carry no signedness, and the emitter
used `sext` for *all* integer widening (`emitCast`, `coerceArgToParamType`
call sites), `sdiv`/`srem`/`slt`/`sgt`/`sle`/`sge` for all int division,
remainder, and relational compares, and `lshr` for all `>>`. `emitCastToString`
didn't handle `i8`/`i16` sources at all, so `string(int16v)` returned the
raw narrow register which the caller then retained as `i8*`.

**Fix:** `emitCast` now zero-extends when the source expression's
checker-recorded type is unsigned (or `i1`); new `coerceExprToParamType`
wraps `coerceArgToParamType` and picks `zext` for unsigned source
expressions — applied at every coercion site with a source expression
(call args, var decls, assignments incl. compound, struct-literal fields,
array elements, map values, index/field assignment, channel sends, returns,
switch cases, range bounds, global-var initializers, enum payloads).
`emitInfixExpr` now selects `udiv`/`urem`, `ult`/`ugt`/`ule`/`uge`, and
`lshr` vs `ashr` by operand signedness (unsigned if either operand's
recorded type is unsigned — C-style promotion rule). `emitCastToString`
widens `i8`/`i16` to `i32` via `extendNarrowInt` before stringifying.

**Verified:** `compiler_narrow_int_test.skink` — `TestUnsignedWidening`
failed with 9 assertions on the pre-fix binary and passes after.

---

## 20. Function literals cannot capture enclosing locals — ✅ FIXED (real closures)

**Severity:** high — silently produces invalid IR; any module containing
such a closure fails to compile *on import alone*, whether or not the
function is ever called.

**Symptom:** Merely `import "std/web/static"` broke compilation:

```
error: global variable reference must have pointer type
  store i32 @rootDir, i32* %r52
```

`static.FileServer(rootDir)` returned `fn(req, w) { ... rootDir ... }`.
Inside the lifted `__skink_fnlit_N` function, `rootDir` is not a local,
so `emitIdent` fell through to the global path and emitted `@rootDir`
which is never defined.

**Root cause:** `emitFnLit` (`std/emitter.skink` ~7115) lifts fn literals
to module-scope `define`s and its own comment says "Closure variable
capture is not supported (the body can only reference globals and its own
params)". The `closureEnv`/`closureEnvs` fields on `CodeGen` are
initialized but never populated — dead scaffolding. The checker does not
reject captures either, so the failure surfaces as invalid LLVM rather
than a compile error.

**Workaround applied:** `std/web/static.skink` rewritten to use a
module-level `var fileServerRoot` + named `fileServerHandler`; the handler
references module-scope state instead of capturing. (Not re-entrant —
last `FileServer` call wins.) New `std/web/static_test.skink` covers the
handler. Along the way, `fs.Open(dir, "r")` succeeds on POSIX directories,
so the index.html fallback never fired; an `fs.IsDir` check was added.

**Partial fix (repo build):** `std/emitter.skink` now emits function
literals that reference only parameters and globals correctly (non-capturing
fn literals compile and run). The `std/web/static.skink` workaround
remains in place.

**Fix implemented (closures):** all `fn`-typed values are now a uniform
`i8*` closure handle `{i8* code, i8* cap0, ...}` — `llvmType(TK_FUNCTION)`
returns `i8*`. `emitFnLit` collects enclosing-local references
(`collectCaptures` walks the literal body, skipping shadowed names),
boxes each capture via `boxVarAtScope` (the same RC block used by #16's
escape boxing — captured mutation writes through shared storage), packs
`{code, boxes...}` into an RC-allocated handle with a generated
`__clos_dtor_*` destructor that releases each box, and lifts the literal
to `__skink_fnlit_N(i8* %env, ...)` which unpacks `env[i+1]` back into
borrowed `heapSlot` bindings. Non-capturing literals use a module-level
constant `{i8* code}` handle; named functions referenced as values get a
`__clos_t_*` thunk (env arg ignored) plus a constant handle. Calls through
locals, fields, globals, module-qualified refs and index/deref exprs all
go through `emitClosureCall` (load code, call `code(env, args...)`);
local fn-typed vars shadow same-named functions at call sites.
Regression tests: `TestFnLitNoCapture`, `TestClosureCaptureRead`,
`TestClosureCaptureMutation`, `TestClosureCaptureAfterReturn` in
`std/compiler_fixes_test.skink`.

---

## 21. Calling a `:=`-inferred function-typed local emits `@h` — ✅ FIXED (repo build)

**Severity:** low — compile failure with workaround.

**Symptom:**

```skink
var h types.Handler
h = static.FileServer(dir)
h(req, w)            // OK
// vs.
h := static.FileServer(dir)
h(req, w)            // error: use of undefined value '@h'
```

**Root cause:** `emitCallExpr` only emitted `emitIndirectCall` when the
local's synType was `TK_FUNCTION` or the llType ended with `)*`. A local
declared with `:=` inferred the named type `types.Handler` (a `type` alias
for `fn(...)`) rather than `TK_FUNCTION`, so the call resolved `h` as a
global function name instead.

**Fix (std/emitter.skink):** `emitCallExpr` and `exprCallLLType` now call
`resolveDirectFnName` first. If the callee is a local whose inferred type
unwinds (through type aliases) to `TK_FUNCTION`, the call is emitted as an
indirect `call %rettype (argtypes)* %local` instead of `@local`. The same
helper now guards direct references to function-typed globals, locals,
and parameter bindings so a non-function name is never used as a direct
function symbol.

**Test:** `TestInferredFunctionLocalCall` in `std/compiler_fixes_test.skink`.

---

## 22. Checker perf starves the LSP: a single `-check` on compiler-importing files takes minutes — ✅ FIXED (compiler-side; skink-lsp workaround retained)

**Severity:** high for tooling — breaks all LSP features while it runs.

**Symptom:** In VS Code/Devin, autocomplete (and hover, go-to-def, etc.)
appear dead on any file that imports the compiler's own modules
(`std/checker`, `std/resolver`, `std/ast`, ...). The `skink-lsp` process
spins at 100% CPU.

**Root cause:** `skink -check` on `import "std/checker"` alone takes ~39 s;
on `skink-lsp/lsp.skink` (imports the whole front-end) it exceeds 90 s —
the same O(M×N) linear-scan-map resolution documented under "Bootstrap
Build Performance" in AGENTS.md (~8–10 min self-compile). `skink-lsp` ran
`resolver.Resolve` + `checker.checkProgram` **synchronously inside
`handleDidOpen`/`handleDidChange`**, so every keystroke queued another
multi-minute check ahead of all completion requests. Not a hang — just
pathological slowness the single-threaded server can't hide.

**Workaround (applied in skink-lsp, Sep 20):** `didOpen`/`didChange` now
publish only the fast lexical pass (`diagnostics.checkFast`); the full
check runs only on `didSave` (`textDocumentSync.save: true` advertised),
and `didChange` skips diagnostics entirely when stdin already has pending
input (FIONREAD). Completion is now sub-second even on `lsp.skink`.

**Fix (Oct 3-4):** the dominant cost was not the checker at all —
`std/lexer.skink` called `str.Len(input)` (a `strlen`, O(file size)) on
every character from `readChar`/`peekChar`/`peekChar2`, making lexing
O(N²). The length is now cached in the `Lexer` struct. Additionally,
`checkCallArgsWithTypes` reuses argument types already computed by
`inferGenericSubst` (#31), removing exponential re-checking of nested
generic calls. Checker's symbol tables were already hashed
(`bindingTable`/`ttypeTable`/`declTable` over `stridx.StrIndex`).

**Measured (stage-2 binary, Oct 4):**
- `skink -check std/checker.skink` (imports the whole front-end):
  **~39 s → 1.3 s** (~30×)
- `skink -check std/emitter.skink` (largest module): **5.0 s**
- `skink -check skink.skink` (entire compiler): **~90 s-class → 5.6 s**
- typical stdlib/test files: **0.2–0.4 s**

The pathological latency that starved the LSP is gone — checks are now
sub-second on ordinary files and single-digit seconds on the compiler
itself. skink-lsp retains its `didSave`-only full-check policy as a
latency-smoothing measure, not a correctness requirement; a fully
async check would still need a thread-safe runtime send path.

---

## 23. Reslicing a nil slice `s[:0]` segfaults — ✅ FIXED (repo build)

**Symptom:** `s[:0]` on a nil slice crashed with SIGSEGV at runtime — e.g.
an element of `make([][]T, n)` that was never appended to.
`copy(nilSlice)` worked fine (returns len 0).

**Repro:**

```skink
ss := make([][]int, 3)
ss[0] = ss[0][:0]   // ✅ now returns empty slice
```

**Fix:** `emitSliceExpr` in `std/emitter.skink` now null-checks the source
slice pointer before loading its length from the RC header at offset -8;
for a nil source the length is treated as 0, so zero-bounds reslices
produce an empty slice instead of dereferencing null.

**Test:** `TestNilSliceReslice` in `std/compiler_fixes_test.skink`.

---

## 24. Mixed `int * float` arithmetic escapes the checker — emits invalid LLVM — ✅ FIXED (repo build)

**Severity:** medium — compile fails at clang stage with a cryptic IR error
instead of a clean type error at the Skink level.

**Symptom:**

```
bin/main.ll:43106:26: error: '%r190' defined with type 'double' but expected 'i32'
  %r191 = mul i32 %r176, %r190
```

**Repro:**

```skink
import "std/math"
n := 5 * math.Min(2, 3)   // int * float — now evaluates as float
```

**Fix:**
- `std/checker.skink`: `checkArith`/`checkPower` now allow mixed integer/float
  operands and infer the result as `float` (so `n` above is a `float`, and
  assigning the result to an `int` target produces a normal type error).
- `std/emitter.skink`: `emitInfixExpr` now treats the whole expression as
  `double` if either operand is `double`, and coerces the integer operand to
  `double` before emitting `fadd`/`fsub`/`fmul`/`fdiv`/`frem` or `fcmp`.
  This prevents the previous `mul i32 %a, double %b` malformed IR.

**Expected:** assigning a mixed int/float result to an `int` variable still
requires an explicit `int(...)` cast; the arithmetic itself is promoted to
`float`.

---

## Notes / non-bugs encountered

- `time.Duration` is a struct, not a conversion fn — use
  `time.FromMilliseconds(int64(x))` etc. The `time.Second`/`Millisecond`
  consts are `int64` nanosecond counts, not `Duration`s.
- Mutex type is `sync.Mutex` (capital M); `sync.mutex` has no methods.
  `sync.NewMutex()` returns `*Mutex`.
- `select` case binding requires `case v := <- ch:` — `case v <- ch:` doesn't bind.
- `map` literals aren't valid in expression position; use `make(map[K]V)`.
- `nil` is the null literal (not `null`).
- Bitwise `|`/`&` bind *looser* than `==` (C-style precedence, not Go):
  `a | b == 0` parses as `a | (b == 0)` → "bitwise operator | requires
  integer operands". Write `(a | b) == 0`. Always a compile error, never
  silent — but surprising if you're used to Go precedence.
- `spawn`ed goroutines keep the process alive after `main` returns — an
  infinite `loop()` means the program never exits without a stop path.
- The installed compiler is `/usr/local/bin/skink` (v0.2.58); the Makefile
  uses plain `skink`. If testing the repo build at
  `~/Documents/GitHub/skink/skink` directly, it needs `SKINK_HOME=<repo>`
  to find its stdlib.

---

## 25. Diamond imports not deduplicated — same file merged twice — ✅ FIXED (repo build)

**Severity:** medium — blocks common test/dependency layouts.

**Symptom:** file A imports `./snn`, which imports `./hormones`; when A also
imports `./hormones` directly, every symbol in `hormones` used to error with
`variable "hormones.X" already declared in this scope` and `duplicate
import alias` for hormones' own imports.

**Fix (std/resolver.skink):** `normalizePath` canonicalises relative paths
(`./x`, `././x`, `a/../x`) and `loadFile` tracks a `states` map keyed by the
canonical path. When a diamond import reaches the same physical file via a
different textual path, the second load sees `state == 2` and is skipped,
so each module is loaded exactly once.

**Test:** `diamond/diamond_test` in the standard test suite exercises a
three-module diamond (`A -> B -> C` plus `A -> C`).

**Found in:** CHRIS — `lib/snn_test.skink` importing `./snn` + `./hormones`.

---

## 26. make([]uint8, n) and related byte-slice operations — ✅ FIXED (repo build)

**Severity:** high — silent data corruption; bounds checks pass vacuously.

**Symptom:** `s := make([]uint8, 4)` → `len(s) == 0`. `s[2] = 9` wrote OK
(capacity exists), `s[:4]` reslice panicked "slice bounds out of range",
`append` never appeared to grow it. The root cause was that `[]uint8` and
`[]int8` share the LLVM type `i8*` with `string`, so `len()`, slicing,
spread `append`, and `for-in` all used string (`strlen`) semantics on the
zeroed byte payload instead of the slice length stored in the RC header.

**Fix:**
- `std/emitter.skink`: added `exprIsByteSlice` helpers using the checker's
  recorded semantic type to disambiguate `i8*` values. `len()`, slice
  expressions, spread `append`, and `for-in` now use array-length logic
  for `[]uint8`/`[]int8` and keep `strlen` for actual strings.
- `emitAppendValues` now derives the slice element type from the array type
  and coerces the appended value to it, so appending an integer literal to
  `[]uint8` stores a single byte.
- `std/checker.skink`: `checkAppendCall` now allows integer literals to match
  any integer slice element type (e.g. `append(make([]uint8, 2), 9)`).
- `std/checker.skink`: `checkArrayLit` now honors an explicit element type
  (`[]uint8{1,2,3,4}` stays `[]uint8` instead of becoming `[]int`) and
  accepts integer literals as untyped constants that fit the target type.

**Tests:** `TestByteSliceMakeLen`, `TestByteSliceIndexAndReslice`,
`TestByteSliceAppend`, `TestByteSliceSpreadAppend`, `TestByteSliceForIn`,
`TestByteSliceLiteralType` in `std/compiler_fixes_test.skink`.

---

## 27. Mixed signed/unsigned comparison promotes to unsigned — ✅ FIXED (repo build)

**Severity:** high — silently inverts ordering for negative values.

**Symptom:** `int32(-26) > uint16(10000)` evaluated `true` — the signed
operand was converted to unsigned before the compare. `> int32(cap)`
correctly gave `false`.

**Fix (std/checker.skink):** `checkInfixExpr` now rejects ordering/equality
comparisons where one operand is a signed integer and the other is an
unsigned integer, *unless* one side is a non-negative integer literal whose
value fits the unsigned type (e.g. `uint == 0` or `uint > 100`). The error is
`cannot compare signed and unsigned integer operands without explicit
cast`. This prevents the silent unsigned semantics that previously inverted
ordering for negative values while still allowing the common literal-compare
idioms used by the standard library.

**Workaround / migration:** explicitly cast both sides to the same signed
width, e.g. `int32(-26) > int32(uint16(10000))`.

**Test:** `TestMixedSignCompareCast` in `std/compiler_fixes_test.skink`
verifies the explicit-cast workaround still runs correctly.

**Found in:** CHRIS — `consolidate`: `newWeight(int32) > maxWeight(uint16)`
sent negative weights to the ceiling (10000) instead of the MIN_WEIGHT floor.

---

## 28. int16 literal overflow silently wraps (60000 → -5536) — ✅ FIXED (repo build)

**Severity:** medium — silent numeric corruption.

**Symptom:** passing constant `60000` to an `int16` parameter compiled and
wrapped to -5536 instead of erroring.

**Fix (std/checker.skink):** `checkIntLitFits` now range-checks integer
literal expressions against their target narrow integer type (int8/int16/
int32/int/uint8/uint16/uint32/uint, but not int64/uint64 because any
literal the parser accepts already fits in 64 bits). It handles positive
EX_INT literals, prefix-negated literals, and single const aliases stored in
`c.constExprs`. Overflowing literals produce
`constant overflows <type>`.

**Tests:** `TestVarDeclNarrowInit`, `TestStructLiteralNarrowFields`,
`TestModuleVarStructLiteral` in `std/compiler_narrow_int_test.skink` still
pass; compile-time overflow is verified by the regression entries in
`std/compiler_fixes_test.skink`.

---

## 29. `expr < literal` after a selector parses `<` as generic type args — ✅ FIXED (repo build)

**Severity:** low — confusing parse error.

**Symptom:** `out.Wellbeing < 0.0` failed with
`expected type (got 'STRING' "...")` at the following argument — the `<`
was consumed as the start of type arguments on the selector. `>` after a
selector parsed fine.

**Fix (std/parser.skink):** `parseSelector` now records the selector start
position and, when it sees `<`, only accepts it as generic type arguments
if the following tokens actually parse as a type list *and* the next token
is `>` (or `>>`). If not, it rewinds to the selector start and lets the
outer infix expression parse `<` as the less-than operator. This keeps
generic selectors like `m.get<int>(x)` working while allowing ordinary
comparisons.

**Test:** `TestSelectorLessThan` in `std/compiler_fixes_test.skink`.

---

## 30. `append` reallocates at exactly len+1 with a full copy — O(N²) per append loop — ✅ RESOLVED

**Severity:** high — any loop that appends many elements to a slice is
quadratic in both copies and allocator churn; on large inputs it exhausts
memory (stage-2 self-compile was OOM-killed at ~12 GB RSS).

**Root cause:** slice headers store only `len` (i64 at ptr-8) — there is
no capacity field. `emitAppendValues` (std/emitter.skink) therefore emits
`Skink_rc_alloc(len+1)` + an element-by-element copy loop for EVERY
append. Because each new block is slightly larger than the just-freed
one, the allocator cannot reuse the hole, so RSS grows with total churn
(≈ Σ i·elemsize → O(N²)).

**Fix applied (std/runtime/rc_rt.c + std/emitter.skink):** the runtime's
RC table already tracks each block's payload byte size, so that serves
as capacity — no header layout change needed. New
`Skink_slice_ensure(ptr, needBytes)` doubles the payload when capacity is
short (`realloc`, in-place when possible; the RC table entry is moved
atomically under the write lock). `emitAppendValues`/`emitSpreadAppend`
now branch on `Skink_rc_count(arr) == 1` and grow uniquely-owned arrays
in place; shared arrays still take the alloc+copy path with per-element
retains. Append is amortized O(1); 200k appends complete in ~60 ms and
`make stage2` peaks at ~590 MB (normal emit IR construction) instead of
OOM at ~12 GB.

**Safety:** in-place growth is emitted only when the source reference is
provably dead — the `x = append(x, ...)` self-assign idiom (ident or
field path, matched via `lValueKey` through `CodeGen.appendSelfVar`), or
an intermediate result of the same variadic append. `t = append(s, v)`
and `s = append(s, s...)` still copy. When the grow result aliases the
consumed input pointer, `emitRetainIfAliased` adds one retain to
compensate the pending release-of-old + temp release.

**Also fixed (same change):** `Skink_array_dtor_sized` treated the
payload-8 field as bytes and divided by elemSize, running element
destructors on only `len/elemSize` elements; it now reads the field as
the element count the emitter actually stores.

**Earlier mitigation (std/stridx.skink + std/checker.skink):**
`StrIndex.resize` allocates the new entries array with a single
`make([]StrIdxEntry, newCap)`; all eight checker table `Set` methods grow
`vals` geometrically via `make` + copy (doubling) instead of `append`.

**Tests:** `TestAppendManyElements`, `TestAppendSharedSlice`,
`TestAppendSpread`, `TestAppendStringSlice`,
`TestAppendNonSelfPreservesSource`, `TestAppendSelfSpread`,
`TestAppendVariadicSelfAssign`, `TestAppendFieldSelfAssign`,
`TestAppendNestedArg` in `std/compiler_fixes_test.skink`;
`TestResizeManyKeys` in `std/stridx_test.skink`.

**Found in:** stage-2 `make stage2` self-compile OOM (kernel killed
`skink` at ~10-12 GB RSS); diagnosed with gdb on `Skink_rc_alloc` —
~80 k allocations ≥128 KB dominated by `lexer.Tokenize`,
`StrIndex.resize`, and `ttypeTable.Set`.

---

## 31. Nested generic calls are re-checked exponentially — ✅ FIXED

**Severity:** medium — exponential compile time on nested generic calls.

**Symptom:** `id(id(id(...)))` with a generic `fn id<T>(x:T)->T` at depth
N takes ~2^N check time (depth 24 did not finish in 60 s; depth
12→14→16 measured 0.19 s → 0.75 s → 2.9 s). Non-generic nesting is
linear.

**Root cause (suspected):** `inferGenericSubst` calls `checkExpr` on each
call argument to infer type parameters, then `checkCallArgs` checks the
same arguments again — the argument subtree is checked twice per generic
call level, compounding exponentially for nested generic calls.

**Expected:** each expression should be checked once — e.g. pass already
computed arg types into `inferGenericSubst`, or have `checkCallArgs`
reuse them.

**Workaround:** avoid deeply nested generic calls. No generic functions
exist in the compiler's own closure, so this is not the stage-2 blocker
(that was #30).

**Fix (std/checker.skink + std/emitter.skink):** `inferGenericSubst` now
returns the arg types it already checked and `checkCallArgsWithTypes`
reuses them instead of re-checking each argument subtree — nested calls
are linear again (`TestNestedGenericCalls` in
`std/compiler_fixes_test.skink`). Two more bugs fixed along the way:
`genericFnDecl` retries `fnDecls` with the module-qualified name
(`sometest.idOf`) since the resolver rewrites call names — previously
generic calls in non-`main` modules failed with "undefined type T"; and
the emitter now coerces erased `i8*` generic call results back to the
checker's substituted type (`coerceGenericCallResult` wired into direct,
module-qualified and method call sites — `idOf(42)` used to be treated
as a string pointer and crash in `printf`).

**Found in:** ad-hoc experiment `id(id(...))` depth 12-24 while
diagnosing #30.

## 32. `x.ptrfield.sub` receiver addresses skip the pointer load — ✅ FIXED

**Severity:** high (latent) — any method call or `&` on a subfield of a
**pointer-typed** struct field computed the wrong address: `&x.f.g` with
`f: *T` produced `&x.f + off(g)` — i.e. it treated the pointer *slot* as
the struct base — instead of `x.f + off(g)`. The result pointed one
indirection off into unrelated memory.

**Symptom:** `matchExprResultLLType` in `std/emitter.skink` does
`cg.checker.matchResultTypes.Get(key)` where `cg.checker` is a
`*checker.Checker` field. The emitted code computed the receiver as
`cg + offsetof(checker) + offsetof(matchResultTypes)` — reading CodeGen's
own fields as a `ttypeTable`. For months this was harmlessly wrong: the
"table" read garbage and always missed, silently falling back to the
arm-body path. After the CodeGen struct changed (closure work), the slot
held `{len=1, cap=8, ptr}` — looked like a populated `StrIndex` — and
`slotOf` dereferenced `0x800000001` → SIGSEGV while compiling
`tagged_union_test`, `compiler_regress_test`, and the enum book example.

**Root cause:** `emitAddressOf`'s EX_FIELD case always used
`emitAddressOf(receiverExpr)` as the GEP base — correct for value fields
(`&x.f` is the struct base) but wrong when `x.f` is itself a pointer
(`&x.f` is `**T`). The same wrong base was used for
pointer-receiver method calls on subfields and for explicit `&x.f.g`.

**Fix (std/emitter.skink):** when the field-target expression's LLVM type
already ends in `*`, the loaded pointer value is the struct base —
`emitExpression` instead of `emitAddressOf`. Also worked around at the
one known call site (`matchResultTypes` via a `ch := cg.checker` local)
because the *bootstrap* binary still miscompiles that exact expression —
the fix only takes effect in binaries the new compiler emits.

**Found in:** Oct 3 — gdb on `skink -o … std/tagged_union_test.skink`
crash (`StrIndex.slotOf` on a bogus table pointer).

## 33. `*p = v` fails to parse after an expression statement — ✅ FIXED

**Severity:** medium — a valid deref-assign statement was rejected
depending on what preceded it.

**Symptom:** `*p = 42` as a statement produced
`unexpected token in expression (got '=' "=")` whenever the previous
statement ended in an expression that could take an infix `*`
(e.g. `p := &x\n*p = 42` parsed as `p := (&x * p)`, orphaning the `= 42`).

**Root cause:** `parseExpression`'s Pratt loop continued infix parsing
across the newline: `*` has PREC_PRODUCT, so `&x\n*p` became
`(&x) * p`. A statement starting with `*` is ambiguous — but a binary
`*` can never *start* an expression, so `*ident` on a new line is always
a deref statement.

**Fix (std/parser.skink):** in `parseExpression`, break the infix loop
when `curTok` is `*` on a different line than `left` and followed by an
identifier — the same newline-disambiguation already used for `<-`
chan send/recv. Trade-off: a genuine `x *` `*p` operand split across
lines now breaks — acceptable and rare.

---

## 34. Mixed signed/unsigned check in checkInfixExpr fires on bitshift and non-comparison operators — ✅ FIXED

**Severity:** medium — falsely rejects valid bitshift expressions like `uint16 >> int`.

**Symptom:** `value >> shift` where `value` is `uint16` and `shift` is `int`
fails with `cannot compare signed and unsigned integer operands without explicit cast`.

**Root cause:** in `std/checker.skink:3328`, the check for mixed signed/unsigned
operands is performed unconditionally for all `ast.Expr` infix expressions in
`checkInfixExpr`, rather than restricting it to relational/comparison operators
(`==`, `!=`, `<`, `<=`, `>`, `>=`). For shift operators (`>>`, `<<`), it is
completely standard for the shift count to be signed `int`.

**Fix (std/checker.skink):** the signedness guard is gated on
`op == "==" | "!=" | "<" | "<=" | ">" | ">="` — ordering/equality only.
Arithmetic (`+ - * / % **`) and bitwise/shift (`& | ^ << >>`) now accept
mixed signedness; the emitter already unifies operand widths and selects
`lshr`/`udiv`/etc. via `unsignedOps` (unsigned if either operand is
unsigned — C-style usual arithmetic conversions).

**Covered by:** `TestShiftSignedCount`, `TestMixedSignArithmetic` in
`std/compiler_fixes_test.skink`.

**Found in:** CHRIS — `lib/buckets.skink:65`.

## 35. `[N]T` fixed-size arrays silently miscompile — ✅ FIXED (checker rejection)

**Severity:** high — accepted syntax produces a guaranteed null-pointer
crash instead of either working storage or a compile error.

**Symptom:** `var a [3]int; a[0] = 10` compiles cleanly and segfaults at
runtime. Same crash on the pre-change bootstrap binary — pre-existing.

**Root cause:** the parser/checker accept `[N]T` but the emitter
treats the declaration like a slice: `var a [3]int` emits
`alloca i32*` initialized to null — a pointer slot with no backing
store — and `a[i]` then GEPs through the null pointer.

**Expected:** either real `[N x T]` LLVM array storage, or a clean
compile-time rejection ("fixed-size arrays not supported; use []T").
The book already documents `[N]T` as "not yet supported"
(`book/appendix_quick_reference.md`), so a checker rejection is the
honest behavior.

**Workaround:** use `[]T` slices (`a := []int{0,0,0}` or
`make([]int, n)`).

**Found in:** Oct 4 — `var a [3]int` probe while auditing book
accuracy.

**Fix (Oct 6):** the parser now rejects `[N]T` at parse time with
"fixed-size arrays [N]T are not supported; use a []T slice" instead of
silently dropping the size and emitting a null backing pointer.

## 36. Generic `[]T` params miscompile element access for non-pointer T — ✅ FIXED (checker rejection)

**Severity:** high (silent data corruption) — generic functions taking a
`[]T` slice produce garbage elements when instantiated with a
non-pointer-sized element type; no error is emitted.

**Symptom:**

```skink
fn Map<T, U>(items: []T, f: fn(T) -> U) -> []U {
    out := make([]U, len(items))
    for i := 0; i < len(items); i = i + 1 {
        out[i] = f(items[i])
    }
    return out
}
strs := Map<int, string>([]int{1,2,3}, io.Itoa)  // prints 1, 3, 32785
```

`Map<string, string>` works correctly — only non-pointer T breaks.
Identical garbage on the pre-change bootstrap binary (pre-existing).

**Root cause:** type erasure lowers `[]T` to `i8**` — an array of
8-byte `i8*` elements — and the generic body indexes it with an 8-byte
stride. But the caller passes its concrete `i32*` backing array
(`[]int` stores 4-byte elements), so `items[i]` reads 8 bytes starting
at each 4-byte boundary. Erasure fundamentally cannot index `[]T`
without knowing T's stride.

**Expected:** correct element access, via either call-site reboxing
(`[]int` → new `[]i8*` array of `inttoptr`'d elements when passed to a
`[]T` param, and the inverse `ptrtoint` unbox for `[]U` results bound to
non-pointer slices) or monomorphization of generic functions. The
`map[K]V` parameter class likely shares the same defect. Note the
emitter currently cannot see generic declarations — a `pub fn`
accessor over `checker.fnDecls`/`genericFnDecl` is needed to detect
`[]T` params at call sites.

**Workaround:** write concrete wrappers (the stdlib's `sort` does this:
`sort.Ints`/`sort.Strings` rather than `sort.Sort<int>`), or keep
element types pointer-sized (`[]string`, `[]*T`) where stride matches.
Book exercise `Map<T,U>` adjusted to `[]string` input.

**Found in:** Oct 4 — `book/10_generics_and_templates.md` exercise 4
(`Map<T,U>` over `[]int` → `[]string`) silently produced garbage while
auditing book accuracy.

**Fix (Oct 6):** the checker now rejects erased composite generics whose
element slot resolves to a non-pointer-sized type —
`checkGenericErasureSafety` walks each generic fn's param/return types
after substitution (`applyGenericSubst`, `inferGenericSubst`) and
reports e.g. "generic parameter T instantiated with non-pointer-sized
type int — erased composite generics ([]T, map[K]V) require
pointer-sized element types" for `[]T`, `map[K]V`, and nested
composites. Pointer-sized instantiations (`[]string`, `[]*T`) and
bare-scalar generics (`Id<int>`) are unaffected; coverage in
`compiler_fixes_test.skink` (`TestGenericSlicePointerElem`,
`TestGenericScalarStillWorks`). Call-site reboxing/monomorphization
remains future work.

## 37. `mod.fn` used as a value argument coerced to i32 — invalid IR — ✅ FIXED

**Severity:** medium — passing `io.Itoa`, `str.ToUpper`, etc. as a
fn-typed call argument emitted invalid LLVM (`inttoptr i32 %ptr` on an
`i8*` closure handle), failing clang verification.

**Symptom:** `applyStr(str.ToUpper, "hi")` (or passing `io.Itoa` to a
generic `Map`) produced
`error: '%r12' defined with type 'ptr' but expected 'i32'`.

**Root cause:** `exprFieldLLType` handled struct fields, template
methods, module vars, and consts — then fell through to `return "i32"`
for everything else, including module-qualified *functions* used as
values. The emit path correctly produced an `i8*` closure handle
(`emitClosureValue`), but call-arg coercion believed the arg was `i32`
and wrapped it in `inttoptr`.

**Fix (std/emitter.skink):** before the `i32` default, consult the
checker's recorded semantic type (`exprSkinkType`) — `mod.fn` resolves
to `TK_FUNCTION` → `i8*`, matching what emit produces.

**Covered by:** `TestModuleFnAsCallArg` in
`std/compiler_fixes_test.skink`.

**Found in:** Oct 4 — `Map<int,string>(ints, io.Itoa)` clang failure
while verifying the book generics exercise.

## 38. `(*p) = v` — parenthesized deref-assign fails to parse — ✅ FIXED

**Severity:** low — a reasonable-looking assign form is rejected; trivial
workaround exists.

**Symptom:**

```skink
x := 1
p := &x
(*p) = 66   // => "unexpected token in expression (got '=' "=")"
```

**Root cause:** a statement beginning with `(` is parsed as an
expression statement; the assign-target lookahead never sees through the
parens, so the `=` is left over. The sibling forms `*p = v`, `*(p) = v`,
`*(*pp) = v`, and `**pp = v` all parse correctly — only the leading-`(`
shape is rejected (same class as the #33 `*`-statement disambiguation).

**Expected:** parse `(<expr>) = v` as an assignment whose target is the
parenthesized lvalue, or at minimum a clearer "not an assignable target"
error.

**Workaround:** write `*p = 66` or `*(p) = 66`.

**Found in:** Oct 5 — edge-case probe while extending
`compiler_fixes_test.skink` coverage of the #33 fix.

**Fix (Oct 6):** the real mechanism was newline-gluing — a `(`-leading
statement was consumed as a call on the previous line's expression, so
`(*p)` never became a statement at all and the `=` was orphaned (the
same class as #33's `*` disambiguation). The Pratt loop now refuses a
postfix `(` call across a newline, and `emitAssignStmt`/`checkAssignStmt`
unwrap `EX_PAREN` assign targets. Covered by `TestParenAssignDeref`,
`TestParenAssignIdent`, `TestParenAssignCompound`,
`TestParenAssignNewlineStmt` in `compiler_fixes_test.skink`.

## 39. Captures of outer/loop variables in `for i :=` loop bodies miscompile — ✅ FIXED

**Severity:** high — silent data corruption or an infinite loop.

**Symptom:**

```skink
// a) captured outer local reads 0 after iteration 0
n := 7
for i := 0; i < 3; i = i + 1 {
    f := fn() -> int { return n }
    io.PrintLine(io.Itoa(f()))   // prints 7, 0, 0 — expected 7, 7, 7
}

// b) capturing the loop variable never terminates
for i := 0; i < 3; i = i + 1 {
    f := fn() -> int { return i }
    io.PrintLine(io.Itoa(f()))   // prints 0 forever — i never advances
}
```

**Root cause (hypothesis):** the captured variable is boxed once at its
declaration and the closure env aliases that box. The end-of-iteration
scope cleanup appears to release the env/captured box — dropping the
shared box's refcount to 0 while the variable is still live — so later
iterations read freed (zeroed) storage. For the loop counter this also
breaks `i < 3`/`i = i + 1`, yielding the infinite loop. Notably,
capturing a variable declared *inside* the loop body
(`j := i * 10; fn() { return j }`) or a `for x in` element variable
works correctly — only captures of variables whose scope encloses the
loop are affected.

**Expected:** each iteration's closure aliases the live captured box for
the variable's whole scope; per-iteration env teardown must not free
shared captures.

**Workaround:** copy the value to a loop-body local and capture that
(`j := i; f := fn() -> int { return j }`), or use `for x in` iteration.

**Found in:** Oct 5 — edge-case probes `hang_a`/`hang_d`/`hang_e`
(repros in `/tmp/hang_*.skink` shapes above).

**Fix (Oct 6):** the actual root cause was `boxVarAtScope` emitting the
capture box's alloc + value-copy *inline at the literal's evaluation
site*. Inside a loop, iteration N re-boxed an alloca that iteration N−1
had already moved-from/zeroed — so captured `n` read 0 and captured `i`
never advanced. Captured variables are now boxed at **declaration**
time like `&x` escapes: `collectEscapedVars` became `EscapeScan`
(`escaped` + `captured` name maps), `CodeGen.capturedVars` tracks them,
and `declareVar`/`promoteEscapedVar` box a captured variable as soon as
it is declared. Verified: outer capture prints 7,7,7; loop capture
prints 0,1,2. Regression coverage in `compiler_fixes_test.skink`
(#39 section).

**Follow-up (Oct 6):** declaration-time boxing also boxes captured
pointer-typed variables, which broke `&p` on a `p: *T`: `emitAddressOf`
returned the capture box (`**T` bits) where the object pointer was
expected — `spawn worker(&wg)` in the book concurrency recipes passed a
`**WaitGroup` bit-pattern to a `*WaitGroup` parameter and the worker
segfaulted in `waitgroup.Done` (`self.muHandle` read as null). Fixed by
making `emitAddressOf` load the pointer value stored inside the box for
`%struct.*`-typed vars, matching the unboxed path's object-pointer
semantics (non-struct pointer types keep true address-of: the box is
their `**T`). Covered by `TestCapturedPointerAddressOf` and
`TestSpawnCapturedPointerArg` in `compiler_fixes_test.skink`.

## 40. Closure parameter fails to shadow a same-named captured outer variable — ✅ FIXED

**Severity:** medium — silent wrong result; no error emitted.

**Symptom:**

```skink
n := 100
show := fn(n: int) -> int { return n }
show(5)   // => 100 — expected 5; the parameter is ignored
```

**Root cause (hypothesis):** the literal's body resolves `n` through the
env/capture table before consulting its own parameter list, so a capture
with the same name wins over the parameter. Correct scoping requires
params (innermost scope) to shadow captures.

**Expected:** `show(5)` returns `5`; the outer `n` remains `100`.

**Workaround:** rename the literal's parameter so it differs from any
captured name.

**Found in:** Oct 5 — edge-case probe while extending closure tests.

**Fix (Oct 6):** literal parameter names are now excluded from
`collectLitCaptureNames` before env packing, so a same-named parameter
keeps its own scope-0 binding instead of being overwritten by the
capture view. Covered by `TestClosureParamShadowsCapture`.

## 41. `defer fn(){…}()` capturing a block-scoped variable emits invalid IR — ✅ FIXED

**Severity:** medium — valid-looking code fails at the clang stage.

**Symptom:**

```skink
if true {
    j := 5
    defer fn() { io.PrintLine(io.Itoa(j)) }()
}
// => .ll: error: global variable reference must have pointer type
//    %r1 = call i8* @io.Itoa(i32 @j)
```

`j` is emitted as the *global* symbol `@j` instead of an env capture.
The same failure occurs for captures of loop-body locals. A deferred
capturing literal whose captured variable is declared at *function*
scope works fine (`deferCapHelper` in `compiler_fixes_test.skink`).

**Root cause (hypothesis):** deferred literals are emitted in the
function-epilogue defer block; the env-packing path for captures whose
declarations live in a nested block is not wired there, so the lifted
literal's captured reference degrades to a global lookup.

**Expected:** deferred literals capture block-scoped variables
correctly, or the checker rejects the form with a clear error.

**Workaround:** hoist the captured variable to function scope, or defer
a named function instead of a capturing literal.

**Found in:** Oct 5 — edge-case probes `e_deferloop`/`e_deferblock`.

**Fix (Oct 6):** deferred calls are now compiled to zero-argument thunk
literals at the defer statement (so env packing binds block-scoped
capture boxes while the block is live) and pushed onto a runtime defer
stack — see #42's fix note for the mechanism. Verified: `j := 5` inside
`if { }` captured by `defer fn(){…}()` prints `5`.

## 42. `defer` inside a loop body does not accumulate; args evaluated at run time — ✅ FIXED

**Severity:** medium — surprising semantics; deferred calls are silently
dropped.

**Symptom:**

```skink
for i := 0; i < 2; i = i + 1 {
    defer io.PrintLine("a" + io.Itoa(i))
    defer io.PrintLine("b" + io.Itoa(i))
}
io.PrintLine("exit")
// prints: exit, b2, a2  — only TWO deferred calls (expected four), and
// i is read as its post-loop value 2 at run time.
```

Each `defer` *statement site* keeps at most one pending registration —
the next iteration overwrites it — and the deferred call's arguments are
evaluated when the defer runs (function exit), not when it is
registered. Go-style semantics would queue four calls and evaluate args
at registration (printing `b1 a1 b0 a0`).

**Expected:** either accumulate one registration per iteration, or
document that `defer` in a loop body is last-write-wins per site.

**Workaround:** hoist the defer out of the loop, or register explicitly
in a slice of thunks.

**Found in:** Oct 5 — edge-case probes `e_defersimple`/`e_defer2`.

**Fix (Oct 6):** `defer` now registers at run time. Each executed defer
compiles its call into a zero-argument thunk literal — built at the
defer statement so captures bind the live scope — hoists the call's
arguments (and any function-typed/local/literal callee) into hidden
temporaries evaluated at the defer point, and pushes a
`%__defer_node{next, code, env}` onto a per-function defer stack. Every
exit path (`emitRet`, literal fall-through, error-propagation return,
panic unwind via `emitPanicEpilogue`) drains the stack LIFO. The stack
head lives in an `Skink_rc_alloc`'d heap slot because locals mutated
between setjmp and longjmp are indeterminate under `-O2` mem2reg.
Verified: the repro prints `exit, b1, a1, b0, a0`; panic unwinding runs
each frame's defers before re-raising. Coverage:
`TestDeferLoopAccumulates`, `TestDeferLoopOrder`,
`TestDeferBlockCapture` (#41) in `compiler_fixes_test.skink`.

## 43. Fn-type mismatch errors print `<function>` for both sides — ✅ FIXED

**Severity:** low — diagnostics only.

**Symptom:**

```skink
struct Holder { f: fn(int) -> int }
h := Holder{f: io.Itoa}   // field "f" type mismatch:
                          // expected <function>, got <function>
```

Both sides print as `<function>`, so the actual signature difference is
invisible in the error.

**Expected:** print fn signatures (`fn(int) -> int` vs
`fn(int) -> string`) in mismatch messages.

**Found in:** Oct 5 — probe4 while extending mod.fn-as-value tests.

**Fix (Oct 6):** `typeString` now renders TK_FUNCTION via
`functionTypeString` — `fn(int) -> int`, `fn() -> (A, B)` for
multi-return — so the mismatch reads e.g. "expected fn(int) -> int, got
fn(int) -> string".

---

## 44. Hex integer literals (e.g. 0x0f) emitted verbatim into LLVM IR cause Clang syntax error — ✅ FIXED (v0.2.60)

**Severity:** medium — prevents using hex literals in integer bitwise operations.

**Symptom:** `x & uint64(0x0f)` causes Clang error:
`error: floating point constant invalid for type %r26 = and i64 %r25, 0x0f`.

**Root cause:** In LLVM IR, integer constants must be decimal integers (`15`).
LLVM IR treats literals starting with `0x` in certain contexts as IEEE-754 hex floating
point constants, so emitting `0x0f` verbatim into an integer `and` instruction
is rejected by Clang/LLVM.

**Expected:** The compiler emitter should format all integer literals as decimal numbers
in emitted LLVM IR instructions.

**Workaround:** Use decimal literals (e.g. `15` instead of `0x0f`).

**Found in:** CHRIS — `lib/enc.skink`.

---

## 45. HTTP server reads request until connection close — keep-alive clients hang — ✅ FIXED

**Severity:** high — server is unusable with real HTTP/1.1 clients.

**Symptom:** `handleConn` loops `conn.Read` until `recv` returns `<= 0`
(std/web/server.skink ~L124–136). It never stops at the request
boundary (`\r\n\r\n` + `Content-Length`), so a keep-alive client that
sends a complete request and waits for the response deadlocks: the
server blocks in `recv` while the client blocks waiting. Response is
only sent when the client times out or closes (~60–120s). Tests pass
because test clients close the socket after sending.

**Root cause:** read loop has no request-boundary detection and no
socket timeout is ever set.

**Expected:** stop reading when headers are complete and
`Content-Length` bytes of body have arrived; support keep-alive.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 46. Panic/recover state is process-global, not thread-local — ✅ FIXED

**Severity:** high — a panic in a spawned thread corrupts other threads
or kills the whole process.

**Symptom:** `panic_jmps`, `panic_depth`, `panic_pending`,
`panic_msg_storage` in `std/runtime/panic_rt.c` are plain statics. A
panic in a web connection thread either `_exit(1)`s the server or
`longjmp`s into a `jmp_buf` installed by a *different* thread (UB).

**Expected:** `__thread`-local panic state + a per-connection
`recover()` wrapper that converts handler panics into a 500 response.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 47. `conn.Send` / `Skink_tls_write` issue a single `send()` — short writes truncate responses — ✅ FIXED

**Severity:** high — responses larger than the socket buffer (~64KB+)
are silently truncated.

**Symptom:** `net.Connection.Send`/`Write` call `send()` once and return
its count; callers ignore the count. A partial write drops the tail of
the response. Same for `Skink_tls_write`.

**Expected:** loop `send`/`SSL_write` until all bytes are written.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 48. No socket timeouts in the server path — slowloris holds threads — ✅ FIXED

**Severity:** medium — `Skink_set_socket_timeout` exists in
`net_rt.c` but is never called on accepted connections; a client that
trickles bytes pins a handler thread indefinitely.

**Expected:** `Server.readTimeoutMs`/`writeTimeoutMs` (default ~30s)
applied after accept.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 49. Unbounded request buffer — memory exhaustion per connection — ✅ FIXED

**Severity:** medium — the read loop `realloc`s without a cap
(server.skink ~L125–131); one connection can consume arbitrary memory.

**Expected:** `Server.maxRequestBytes` (default ~1MB); overflow → `413`
+ close.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 50. `net.Listen` never sets SO_REUSEADDR — restart fails during TIME_WAIT — ✅ FIXED

**Severity:** low-medium — `bind failed` for ~60s after restarting a
server with recent connections.

**Expected:** set `SO_REUSEADDR` on the listen socket (via a C runtime
helper for portability).

**Found in:** Oct 7 — production-readiness audit of std/web.

## 51. Request body stored in NUL-terminated string — binary bodies truncated — ✅ FIXED

**Severity:** high — POST bodies containing `\x00` (multipart uploads,
binary payloads) are truncated at the first NUL because bodies are
stored/parsed via strlen-based `str.*` functions.

**Expected:** store body as raw bytes + explicit `bodyLen`; expose
`Request.BodyBytes()`; keep `Body()` for text.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 52. Static file serving NUL-truncates binary files + O(n²) concat — ✅ FIXED

**Severity:** medium-high — `static.skink` reads files via
`str.Concat` per 4KB chunk: binary files truncate at the first NUL and
large files cost O(n²). The response send path also truncates via
`str.Len`.

**Expected:** read the file into a single sized buffer (`fs.Stat`) and
write via explicit-length `WriteBytes`.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 53. No percent-decoding of path/query parameters — ✅ FIXED

**Severity:** medium — `QueryParam` returns raw `a%20b`; `+` is not
decoded to space; routes and static paths never see decoded URLs, so
`/my%20file` never matches.

**Expected:** percent-decode path before routing and decode query
values (`%XX` hex + `+`→space); keep the raw path accessible.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 54. Request header lookup is case-sensitive — ✅ FIXED

**Severity:** medium — `headers["Content-Length"]` misses
`content-length`; HTTP field names are case-insensitive.

**Expected:** normalize keys (lowercase) at parse and lookup.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 55. Response header values written verbatim — CRLF injection — ✅ FIXED

**Severity:** medium — `w.Header(k, v)` emits `v` unsanitized; a value
containing `\r\n` injects extra headers/body (response splitting).

**Expected:** strip or reject `\r`/`\n` in header keys and values.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 56. `Expect: 100-continue` unanswered — POST deadlock — ✅ FIXED

**Severity:** medium — clients (e.g. curl with a large body) send
headers + `Expect: 100-continue`, then wait for `100 Continue` before
sending the body. The server waits for the body → deadlock.

**Expected:** emit `HTTP/1.1 100 Continue\r\n\r\n` once when the
header is present and a body is expected.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 57. HEAD requests return a body — ✅ FIXED

**Severity:** low — responses to HEAD include the body, violating
RFC 7230.

**Expected:** run the handler, serialize headers, drop the body bytes.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 58. `statusText` missing common status codes — ✅ FIXED

**Severity:** low — 206, 307, 308, 400-series (408/411/413/414/431),
501, 505 and others serialize as `Unknown`.

**Expected:** cover the standard HTTP/1.1 reason phrases.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 59. Route path params stored in request headers — ✅ FIXED

**Severity:** low — `/:param` matches are injected into `req.headers`
as `__param_<name>` (router.skink ~L109): they collide with real
headers and leak into a user-visible map.

**Expected:** dedicated `Request.params` map; `URLParam` reads it.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 60. `fileServerRoot` is a module global — last FileServer wins — ✅ FIXED

**Severity:** low — mounting two file servers on one router serves the
second root for both handlers; not reentrant (static.skink ~L52–58).

**Expected:** `FileServer` returns a closure capturing `rootDir` —
closures now support captures.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 61. Unbounded detached-thread-per-connection — ✅ FIXED

**Severity:** medium — every accept spawns a detached pthread with no
limit or pool; combined with #45/#48 this is a trivial thread/memory
exhaustion DoS.

**Expected:** `Server.maxConns` (default ~512) enforced via a
buffered-chan semaphore; also raise the listen backlog.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 62. Accept-error `continue` busy-loops at 100% CPU — ✅ FIXED

**Severity:** medium-low — persistent accept failures (e.g. EMFILE)
spin the accept loop.

**Expected:** backoff with a small exponential sleep; exit cleanly on
shutdown.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 63. No graceful shutdown — accept loop never exits, conns untracked — ✅ FIXED

**Severity:** medium — `ListenAndServe` runs `for {}` forever; detached
handler threads are untracked, so the process can only be killed.

**Expected:** `Server.Shutdown()` → flag + close listener → accept
loop exits; WaitGroup tracks active conns for drain.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 64. No HTTP/1.1 keep-alive — every connection serves one request — ✅ FIXED

**Severity:** medium — responses always carry `Connection: close`;
browsers reopen a TCP connection per request. User requirement: real
persistent connections.

**Expected:** parse the `Connection` header, loop requests on the
connection (requires #45's boundary-aware reader and leftover-buffer
handling for pipelined/batched requests).

**Found in:** Oct 7 — production-readiness audit of std/web.

## 65. `Transfer-Encoding: chunked` request bodies undecoded — ✅ FIXED

**Severity:** medium-low — chunked POST bodies are treated as raw
Content-Length-less input; the chunk frames land in `req.body`.

**Expected:** decode hex-size/CRLF frames until the 0-chunk when
`Transfer-Encoding: chunked` is present (case-insensitive).

**Found in:** Oct 7 — production-readiness audit of std/web.

## 66. `ResponseWriter.Write` O(n²) body concatenation — ✅ FIXED

**Severity:** low — `self.body = self.body + data` copies the whole
body per Write; handlers doing many writes degrade quadratically.

**Expected:** accumulate via `str.Builder` or a byte buffer.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 67. TLS lacks SNI/ALPN/session resumption — ✅ FIXED

**Severity:** low — one `SSL_CTX` per server; no virtual hosts, no
ALPN (h2), no session resumption. Adequate for simple HTTPS.

**Found in:** Oct 7 — production-readiness audit of std/web.

## 68. `net.Listen` backlog hardcoded to 10 — ✅ FIXED

**Severity:** low — pending-connection queue of 10 drops bursts under
load.

**Expected:** larger default (e.g. 128) or a parameter.

**Found in:** Oct 7 — production-readiness audit of std/web.

---

## 69. `for x in ptr.field` over a nil slice field of a pointer-to-struct segfaults — ✅ FIXED

**Severity:** medium-high — a common, plausible pattern crashes at
runtime instead of iterating zero times.

**Symptom:** `b := &Box{}; for x in b.items {}` where `items` is a nil
slice segfaults in the loop setup. The same nil slice iterated via a
plain local (`for x in s`) works correctly.

**Repro:**

```skink
struct Pair { a: string b: string }
struct Box { items: []Pair }
b := &Box{}
for x in b.items {}   // SIGSEGV
```

**Root cause:** `emitForInStmt` emits `load len` at `ptr-8`
unconditionally — `mov -0x8(%rax)` with the slice register `rax == 0`
— in some for-in paths (observed with a slice copied out of a
pointer-to-struct field, and with `[]struct` element type). The
`len()` builtin nil-guards correctly; the for-in path does not.

**Workaround:** guard with `len()` before iterating:
`if len(x.items) > 0 { for e in x.items {} }`.

**Found in:** Oct 7 — SNI cert registration loop in
`std/web/server.skink` `ListenAndServeTLS` crashed on Servers without
extra certs (`sniCerts` nil).

---

## 70. Socket writes raise SIGPIPE — any mid-write peer disconnect kills the process — ✅ FIXED

**Severity:** critical — a client closing its socket while the server is
writing terminates the entire process, not just the connection thread.

**Symptom:** TLS `SSL_accept`/`SSL_write`/`SSL_shutdown` (OpenSSL socket
BIO → plain `write()`) and any `send()` lacking `MSG_NOSIGNAL` raise
SIGPIPE on a closed peer. Default disposition kills the process — the
web server dies serving a client that disconnected early.

**Repro:** TLS client connects, completes the TLS 1.3 client view of the
handshake, then closes immediately; the server thread is still inside
`SSL_accept` writing post-handshake flights → SIGPIPE → process death.

**Root cause:** no `signal(SIGPIPE, SIG_IGN)` anywhere in the runtime;
only `Skink_send_all` used `MSG_NOSIGNAL`. OpenSSL's socket BIO has no
way to pass send flags.

**Expected:** ignore SIGPIPE at runtime init (constructor in
`net_rt.c`, POSIX branch) so writes return EPIPE errors instead of
killing the process.

**Found in:** Oct 7 — SNI live test: client closed after handshake while
server was still in `SSL_accept`'s write path.

---

## 71. `send` on a closed channel silently drops the value — 🔧 FIXED

**Severity:** medium — diverges from Go semantics and hides bugs.

**Symptom:** `close(ch); ch <- v` returns normally; the value is
discarded. Go panics with `send on closed channel`.

**Root cause:** `Skink_chan_send` in `std/runtime/conc_rt.c`
early-returns when `ch->closed` instead of panicking.

**Expected:** `Skink_panic("send on closed channel")` (mutex must be
unlocked before panicking — `Skink_panic` longjmps).

**Found in:** Oct 8 — robustness audit, `conc_rt.c` send path.


**Fixed:** Fixed in `Skink_chan_send` (`std/runtime/conc_rt.c`) — panics "send on closed channel". Tests: `chan_close_test`, `compiler_concurrency_test`.

---

## 72. `close` on an already-closed channel is a silent no-op — 🔧 FIXED

**Severity:** low — divergence from Go semantics.

**Symptom:** `close(ch); close(ch)` succeeds silently. Go panics with
`close of closed channel`.

**Root cause:** `Skink_chan_close` unconditionally sets `closed=1`.

**Expected:** panic on second close.

**Found in:** Oct 8 — robustness audit.


**Fixed:** Fixed in `Skink_chan_close` — panics "close of closed channel". Tests: `chan_close_test`.

---

## 73. `make(chan<T>, N)` capacity is accepted but never enforced — 🔧 FIXED

**Severity:** medium — the API advertises bounded channels; sends never
block, so capacity limits silently don't apply.

**Symptom:** `ch := make(chan<int>, 1)` then three sends without a
receiver — all succeed immediately. A Go program would block on the
second send.

**Root cause:** `Chan.capacity` is stored by `Skink_chan_make` but
`Skink_chan_send` never waits on it; `ch->send_cv` is initialised but
never waited on.

**Expected:** when `cap > 0`, send blocks while `count >= cap`; recv and
close wake blocked senders. `cap == 0` deliberately remains an
unbounded queue (rendezvous semantics would break existing code).
`select` send-readiness must check `count < cap`.

**Found in:** Oct 8 — robustness audit. Also: `make(chan, -1)` is
accepted; clamp to 0.


**Fixed:** Fixed for cap > 0: `send` blocks on `send_cv` until a slot frees; cap 0 keeps unbounded/rendezvous behavior. Tests: `compiler_concurrency_test`.

---

## 74. Integer divide/remainder by zero → SIGFPE kills the process — 🔧 FIXED

**Severity:** high — an unrecoverable hardware trap for a common,
user-triggerable error.

**Symptom:** `x / 0` or `x % 0` raises SIGFPE; the process dies with no
panic message and no chance to `recover`.

**Root cause:** the emitter emits raw `sdiv/udiv/srem/urem` with no
zero-divisor check (`emitBinaryOp` in `std/emitter.skink`).

**Expected:** emit a zero check before div/rem →
`Skink_panic("integer divide by zero")`, recoverable via `recover()`.

**Found in:** Oct 8 — robustness audit.


**Fixed:** Fixed in `emitBinaryOp` — `Skink_divzero_check` panics "integer divide by zero" (recoverable). Tests: `compiler_semantics_test`, `compiler_arith_test`.

---

## 75. Shift counts ≥ bit width or negative → LLVM poison (UB) — 🔧 FIXED

**Severity:** high — silent undefined behaviour; result is whatever the
backend happens to produce (x86 masks the count, other targets may
differ).

**Symptom:** `x << 33` on `i32` or `x << -1` produces an arbitrary value
instead of a defined result.

**Root cause:** raw `shl/lshr/ashr` emission with no count check.

**Expected:** Go semantics — negative count panics; count ≥ width
yields 0 (via `icmp` + `select`).

**Found in:** Oct 8 — robustness audit.


**Fixed:** Fixed in `emitBinaryOp` — count clamped for IR safety; result selected to 0 when count >= width (Go semantics). Tests: `compiler_semantics_test`.

---

## 76. `for i in <bare int>` emits invalid LLVM IR — 🔧 FIXED

**Severity:** medium — accepted by the checker, then clang rejects the
module with a cryptic `null must be a pointer type` error.

**Symptom:** `for i in 3 { }` fails at the clang stage:
`%r2 = icmp eq i32 3, null`.

**Root cause:** the checker's for-in path has an `isInteger` branch that
declares the loop var `int`, but the emitter treats the iterable as a
pointer-typed collection and nil-compares it.

**Expected:** the supported range syntax is `for i in a..b`. Reject a
bare-integer iterable in the checker with a clear error pointing at
`a..b`.

**Found in:** Oct 8 — robustness probe.


**Fixed:** Checker now rejects `for i in <int>` with a diagnostic; use `a..b` ranges. Compile-fail snippet: `cf_for_int`.

---

## 77. `&m[k]` (address of map element) emits invalid LLVM IR — 🔧 FIXED

**Severity:** medium — instead of a clean compile error the module
reaches clang and fails with a type error.

**Symptom:** `p := &m["a"]` →
`error: '%r134' defined with type 'ptr' but expected 'i32'` at clang.

**Root cause:** `emitGEP`-style address-of on an index expression treats
the map pointer as an array pointer and GEPs with the key as an integer
index.

**Expected:** checker rejects `&` applied to a map index expression:
"cannot take the address of a map element" (map slots move on rehash).
`&a[i]` on slices remains legal — it GEPs into the backing array.

**Found in:** Oct 8 — robustness probe.


**Fixed:** Checker rejects `&m[k]` ("cannot take address of map element"). Compile-fail snippet: `cf_addr_map_elem`.

---

## 78. Element indexing `a[i]` / `s[i]` has no bounds check — silent UB — 🔧 FIXED

**Severity:** high — out-of-range reads/writes silently corrupt or read
adjacent heap instead of panicking.

**Symptom:** `a := []int{1,2}; x := a[5]` reads out of bounds (returns
whatever is there); `s := "abc"; s[10]` returns 0. `a[lo:hi]` reslicing
*is* checked via `Skink_bounds_check` — element indexing is not.

**Root cause:** `emitIndexExpr` emits a bare GEP+load/store; only the
reslice paths emit `Skink_bounds_check`.

**Expected:** bounds-check element indexing on slices and strings →
`Skink_panic`, recoverable. Pointer arithmetic stays unchecked.

**Found in:** Oct 8 — robustness audit/probe.


**Fixed:** `emitIndexBoundsCheck` emits `Skink_bounds_check(i, i+1, len)` on slice element reads/writes; raw `T*` indexing exempt. Tests: `compiler_semantics_test`; exposed and led to fix #92.

---

## 79. Method value `f := x.m` resolves to the unbound method — confusing — 🔧 FIXED

**Severity:** low-medium — the expression compiles but `f` is the raw
method function expecting an explicit `self` first argument, so `f()`
fails with "wrong number of arguments".

**Root cause:** `c.Get` type-checks as the declared method's function
type (with receiver param), not a bound closure.

**Expected:** either implement bound-method closures, or checker-reject
method value expressions with a clear message ("method values are not
supported; write `fn() { return c.Get() }`").

**Found in:** Oct 8 — robustness probe.


**Fixed:** Checker rejects bare `x.m` method values (unbound fn requiring `self`). Compile-fail snippet.

---

## 80. Writing to a nil map segfaults — 🔧 FIXED

**Severity:** high — `var m map[string]int; m["a"] = 1` crashes instead
of producing a usable map or a clean panic.

**Symptom:** SIGSEGV inside the map-assignment path (null map pointer
dereferenced by the emitted bucket logic).

**Root cause:** `emitMapIndexAssignment` assumes the map pointer is
non-null; `var m map[K]V` leaves it null. (`make` is required, but the
compiler doesn't enforce or diagnose it.)

**Expected:** Go-compatible panic — "assignment to entry in nil map" —
recoverable via `recover()`. (Auto-initialising on first write was
considered; panic is chosen for Go parity and explicitness.)

**Found in:** Oct 8 — robustness probe.


**Fixed:** `emitMapIndexAssignment` nil-guards and panics "assignment to entry in nil map"; `emitMapIndex`/`emitMapIndexOk` return zero values on nil maps. Tests: `compiler_semantics_test`.

---

## 81. `==`/`!=` on struct values compiles but compares addresses — always wrong — 🔧 FIXED

**Severity:** medium-high — silently incorrect results for a natural
operation.

**Symptom:** `a := P{1,2}; b := P{1,2}; a == b` → `false` even though all
fields are equal.

**Root cause:** `checkEquality` allows `==` between equal-typed operands
including `TK_NAMED` structs; the emitter compares the struct's
address/register identity rather than field contents.

**Expected:** checker rejects `==`/`!=` on struct-by-value operands
("structs are not comparable; compare fields or pointers"), OR emit a
field-wise comparison for field-comparable structs. Chosen: reject
(deep equality is a larger feature; pointer equality `&a == &b` stays
legal and meaningful).

**Found in:** Oct 8 — robustness probe.


**Fixed:** Checker rejects `==`/`!=` between struct values. Compile-fail snippet.

---

## 82. Zero-valued `waitgroup.WaitGroup` / `sync.RWMutex` / `sync.Cond` segfault — 🔧 FIXED

**Severity:** medium-high — `var wg waitgroup.WaitGroup` (the Go-idiomatic
declaration) crashes on first use instead of working.

**Symptom:** `var wg waitgroup.WaitGroup; wg.Add(1)` →
`Skink_mutex_lock(mu=0x0)` → SIGSEGV in `pthread_mutex_lock`.

**Root cause:** `WaitGroup.muHandle`/`condHandle` are `int64` handles
initialised to 0; only `waitgroup.New()` allocates them. `sync.Mutex`
solved the same problem with `Skink_mutex_ensure(&self.handle)` lazy
allocation, but `RWMutex`, `Cond`, and `WaitGroup` don't use the
pattern.

**Expected:** lazy-initialise handles on first use via
`Skink_mutex_ensure`-style atomic ensure (`Skink_rwlock_ensure`,
`Skink_cond_ensure` needed in `sync_rt.c`), matching `sync.Mutex` and
Go's usable-zero-value semantics.

**Found in:** Oct 8 — robustness probe (spawn/panic test harness).


**Fixed:** Lazy-init via `Skink_*_ensure` in `sync.RWMutex`/`Cond` and `waitgroup.WaitGroup`; `Cond.Wait` without associated mutex panics. Tests: `sync_test`, `waitgroup_test`, `compiler_concurrency_test`.

---

## 83. `for v in ch` over a channel compiles but iterates garbage — 🔧 FIXED

**Severity:** medium — accepted by the checker (which declares the loop
var as the element type) but the emitter has no channel path, so the
chan `i8*` handle is treated as a byte slice/string and the loop reads
a garbage "length" and garbage elements.

**Repro:** `ch <- 10; ch <- 20; close(ch); for v in ch { sum += v }`
prints 4 instead of 30.

**Expected:** Go semantics — receive elements until the channel is
closed and drained (`Skink_chan_recv` returns NULL).

**Found in:** Oct 8 — robustness test writing.


**Fixed:** `emitForInChanStmt` implements receive-until-close channel iteration. Tests: `compiler_concurrency_test`.

---

## 84. Slice/string indexing with an `int64` index emits invalid LLVM IR — 🔧 FIXED

**Severity:** medium — `a[i]` where `i: int64` reaches clang with
`'%r42' defined with type 'i64' but expected 'i32'` — the GEP index is
never coerced to i32.

**Expected:** narrow the index to i32 for the GEP (consistent with the
i32 element-address model); the new bounds check also catches negative
i64 indices after truncation.

**Found in:** Oct 8 — robustness probe (`v := a[int64(9)]`).

## 85. Float `!=` compiles NaN comparison incorrectly — 🔧 FIXED

- **Severity**: wrong result (silent)
- **Symptom**: `nan != nan` evaluates `false`; `bool(NaN)` cast also false
- **Root cause**: `emitInfixExpr` emitted `fcmp one` ("ordered and not equal") for float `!=`. NaN operands make the comparison *unordered*, so `one` is false — the correct predicate is `fcmp une` ("unordered or not equal"). Same bug in the `double → i1` coercion used for truthiness.
- **Fix**: `fcmp one` → `fcmp une` in both sites (`std/emitter.skink`). `fcmp oeq` for `==`/switch correctly stays ordered.
- **Test**: `TestNaNNotEqual` in `compiler_semantics_test.skink`.

## 86. Method calls on module-level vars rejected in non-main modules — 🔧 FIXED

- **Severity**: spurious compile error
- **Symptom**: `g.Done()` on a package-level `var g waitgroup.WaitGroup` reports "wrong number of arguments: expected 1, got 0" — but identical code in `module main` or on locals compiles fine.
- **Root cause**: `checkMethodCall`'s `isStaticCall` scan looks up `bindings.Get(ident)` to distinguish `v.Method()` (variable receiver → skip self) from `Type.Method()` (static → count self). Global vars are registered under the resolver-qualified name `module.var`, so the unqualified lookup missed at global scope → `hasVar=false` → treated as a static call → `self` param counted in the arity check.
- **Fix**: at global scope in non-main modules, also probe `currentModule + "." + ident` before concluding no variable binding exists (`std/checker.skink`).
- **Test**: `compiler_concurrency_test.skink` module-level WaitGroup/Mutex vars exercised by `TestMutexContention`, `TestWaitGroupZeroValue`, etc.

## 87. Method calls on global struct vars mutate a stack copy — 🔧 FIXED

- **Severity**: silent data loss / wrong results
- **Symptom**: `g.Add(1)` on a package-level `var g waitgroup.WaitGroup` compiles but the counter never changes; `g.Wait()` returns immediately. Works correctly on locals and in `module main` incidentally (unqualified bindings).
- **Root cause**: `emitMethodReceiver`'s pointer-receiver path calls `lookupVar`, which covers only locals; on a miss it falls back to evaluating the receiver and storing it in a temp alloca — `self` points at a stack copy, so all mutations are lost. `emitAddressOf` had the same gap for `&globalVar` in non-main modules.
- **Fix**: when `lookupVar` misses, check `globalVarTypes` (both `name` and `currentModule.name`) and pass `@global` directly; same qualified probe added to `emitAddressOf` (`std/emitter.skink`).
- **Test**: `TestWaitGroupZeroValue`/`TestMutexZeroValue` in `compiler_concurrency_test.skink`.

## 88. `break`/`continue` outside a loop accepted by the checker — 🔧 FIXED

- **Severity**: invalid program compiles (later codegen emits a branch to a nonexistent label)
- **Symptom**: `fn main() { break }` passes `-check` cleanly
- **Root cause**: the checker never tracked loop nesting — `ST_BREAK`/`ST_CONTINUE` were no-ops.
- **Fix**: `loopDepth` counter on Checker, incremented around for/while/for-in bodies; break/continue outside depth 0 reports an error (`std/checker.skink`).
- **Test**: `std/compilefail/cf_break_outside.skink`.

## 89. Duplicate struct fields silently accepted — 🔧 FIXED

- **Severity**: silent miscompilation — the second field overwrites/shadows the first
- **Symptom**: `struct D { x: int x: int }` compiles and runs
- **Root cause**: struct declaration checking never validated field-name uniqueness.
- **Fix**: duplicate field names now report an error during struct checking (`std/checker.skink`).
- **Test**: `std/compilefail/cf_dup_field.skink`.

## 90. Generic call with wrong number of type args accepted — 🔧 FIXED

- **Severity**: invalid program compiles
- **Symptom**: `id<int, int>(5)` against `fn id<T>` compiles — extra type args silently ignored.
- **Root cause**: `checkCallArgs`/generic-substitution never validated `len(genericArgs)` against `len(fnGenerics)`.
- **Fix**: arity check on explicit generic arguments reports a mismatch (`std/checker.skink`).
- **Test**: `std/compilefail/cf_generic_arity.skink`.

## 91. Deeply nested expressions crash the compiler (stack overflow) — 🔧 FIXED

- **Severity**: compiler crash — segfault on hostile/deep input
- **Symptom**: `x := (((...1...)))` or `f(f(f(...)))` nested ~350+ deep → SIGSEGV during `-check`.
- **Root cause**: `parseExpression ↔ parsePrefix` mutual recursion consumes C stack unbounded; ~350 levels exhausted the main stack.
- **Fix**: `exprDepth` counter on `Parser`; `parseExpression` bails with a clean "expression nesting too deep (max 256)" error past the cap (`std/parser.skink`). Statement nesting (if/for blocks) was already handled.
- **Test**: `TestFuzzDeepNesting` in `compiler_fuzz_test.skink` + `std/compilefail/cf_deep_nesting.skink`.

## 92. Tensor coordinate index OOB read in graph Adam/ReLU paths — 🔧 FIXED

- **Severity**: silent data corruption → panic (exposed by #78 element-bounds check)
- **Symptom**: `graph_test.TestTrainStep` panicked "slice bounds out of range" once element indexing gained bounds checks.
- **Root cause**: `Adam.Step` and `backwardNode`'s OP_RELU branch passed `shape1(e)` — a len-1 "flat" index — to `Tensor.Get`/`Set` on rank-2 param tensors; `Get` reads `idx[i]` up to `rank-1`, so `idx[1]` read out of bounds and produced a garbage flat offset (silent corruption pre-#78).
- **Fix**: added `Tensor.GetFlat`/`SetFlat` (row-major flat offsets) in `std/tensor.skink`; `std/graph.skink` Adam.Step and ReLU-backward loops now use them. `Get`/`Set` now panic with "index rank does not match tensor rank" when `len(idx) != rank`.
- **Test**: `graph_test` suite green.

## 93. Timer/Ticker Stop and Reset send on closed channel — 🔧 FIXED

- **Severity**: panic (exposed by #71 send-on-closed panic)
- **Symptom**: `time_test` panicked "send on closed channel" in `TestTickerStop`/`TestTimerReset`.
- **Root cause**: `Stop`/`Reset` closed `self.C` directly while the spawned `timerFire`/`tickerLoop` goroutine could still send — safe only when sends on closed channels were silently dropped.
- **Fix**: `std/time.skink` — `Timer`/`Ticker` gained a buffered `stop` channel; `Stop`/`Reset` send a non-blocking stop signal and only the goroutine closes `C` on exit, so no send can hit a closed channel. `timerFire` always closes `C` on exit; `tickerLoop` selects stop-vs-tick each cycle.
- **Test**: `time_test` suite green.

## 94. len() on a channel accessed via field expression reads a slice header — 🔧 FIXED

- **Severity**: silent data corruption / wrong result
- **Symptom**: `len(h.ch)` where `ch` is a `chan` field returned garbage (e.g. 1) instead of the buffered count; `len(ch)` on a bare local was correct.
- **Root cause**: `exprIsChan` in `std/emitter.skink` resolved field types via `checker.resolveFieldType`, which cannot see local vars/`self` receivers at emit time, so `h.ch` wasn't recognised as a channel and `emitLenCallRaw` read it as a slice (header lookup on the chan pointer).
- **Fix**: `exprIsChan` now prefers `checker.exprTypeLookup` (the checker's per-expression recorded type), falling back to the previous ident/field paths.
- **Test**: `TestLenChanField` in `std/len_chan_test.skink`; exposed by `ui_test`'s `ChanModel.drain` hang (recv blocked on bogus len).

## 95. Template fat-struct pointers escape into slices/returns — dangling vtable pointers — 🔧 FIXED

- **Severity**: use-after-free / segfault (soundness)
- **Symptom**: a `[]Widget`-style slice of template values built inside a function and returned (or otherwise used past the function's frame) holds garbage — calling a method through element N>0 jumps to a bad address (observed `call 0x1`).
- **Root cause**: `coerceToTemplate` (`std/emitter.skink`) boxes `{dataPtr, methodFns...}` into a **stack alloca** in the calling frame and the slice stores that pointer verbatim; `isHeapLLType` explicitly exempts `%template.*` from retain/release. When the boxing frame returns, the elements point at dead stack. Repro:
  `fn mk() -> []ui.Widget { var p []ui.Widget; p = append(p, widgetOf(ui.NewLabel("a"))); p = append(p, widgetOf(ui.NewLabel("b"))); return p }` — `page[1].Height()` segfaults.
- **Expected**: boxed fat-structs escape correctly — heap-allocate the box (`Skink_rc_alloc`) and treat `%template.*` as an RC type so container stores retain and scope cleanup releases.
- **Fix**: `coerceToTemplate` now heap-allocates the box via `Skink_rc_alloc_typed` with `Skink_template_box_dtor` (releases the boxed data pointer), and retains the data pointer when boxing. `isHeapLLType` now classifies `%template.*` **pointer** types as RC-managed, so all existing retain/release sites (slice stores, field stores, scope cleanup, array dtors) handle boxes automatically. `Skink_template_box_dtor` added to `std/runtime/rc_rt.c`.
- **Test**: `std/template_escape_test.skink` — 5 tests covering slice/field/nested-slice/repeat-dispatch escapes. Verified on the original `[]ui.Widget` repro (3/3 elements dispatch).

## 96. Template method table null for modules containing underscores — 🔧 FIXED

- **Severity**: segfault on template method dispatch (latent — any `template` use in a `*_test`-style module)
- **Symptom**: `coerceToTemplate` emitted a box whose method slots stayed zeroinitializer; calling the method jumped to address 0.
- **Root cause**: `findConcreteMethodFnName` maps the LLVM struct name `mod_Type` back to a function `mod.Type.Method` by replacing `_` with `.`. For a module named `template_escape_test` the substitution produced `template.escape.test.Alpha.Greet`, which never matches the emitted `template_escape_test.Alpha.Greet` — so the slot stayed null.
- **Fix**: fallback scan — for each `fnRetTypes` key ending in `.Method`, re-sanitize its receiver part (`module.Type` → `module_Type`) and compare to the LLVM struct name; unambiguous for any underscore placement.
- **Test**: `std/template_escape_test.skink` (module name contains underscores — the previous lookup always failed for it).

## 97. Spawn thunk leaks retained argument references; lazy sync handles unfreed — 🔧 FIXED (spawn) / KNOWN-MINOR (sync)

- **Severity**: memory leak (bounded)
- **Symptom (found via ASAN)**: (a) every `spawn f(arg...)` retained heap args into the arg pack but the generated `Skink_spawn_thunk_N` released only the pack — each string/slice/chan arg leaked one reference per spawn; (b) lazily-allocated `Skink_mutex_ensure`/`Skink_rwlock_ensure` handles (~96 bytes per zero-value WaitGroup/RWMutex) are never freed unless the user calls `Free()`; (c) `Skink_chan_make`/`Skink_chan_send` queue nodes leaked — channel objects and their buffered contents were never reclaimed (ASAN: 21.7KB/2017 objects in compiler_concurrency_test).
- **Fix**: (c) `Skink_chan_make` now allocates via `Skink_rc_alloc_typed` with `Skink_chan_destroy` — channels are i8* RC values, so existing retain/release reclaims them and drains queued nodes (releasing each element's queue-owned ref); (a) spawn thunks now `Skink_rc_release` each retained heap arg after the call (pass-by-ref struct receivers still leak their fields — noted in emitter comment).
- **Remaining (known-minor)**: lazy mutex/rwlock/cond handles are opaque `int64` fields — outside RC reach; bounded per-object, `Free()` API exists for users who care. A single ~24-byte `rc_alloc_typed` leak was observed under a large-workload repo_stats run — non-scaling, unlocated.

## 98. Parser hangs on unterminated enum body (no-progress variant loop) — 🔧 FIXED

- **Severity**: compiler hang (DoS on malformed input; found by corpus-mutation fuzzing)
- **Symptom**: `enum Shape { Circle(float)\nfn f()...` (missing `}`) spins forever — `parseEnumDecl`'s variant loop only advances on IDENT/LPAREN/COMMA tokens, so a keyword like `fn` makes no progress.
- **Fix**: after each variant, an unexpected non-`,`/non-`}`/non-EOF token now reports "unexpected token in enum body" and breaks the loop. Audited all 24 `for !curIs(...)` parser loops — the remaining ones either always advance or break; this was the only no-progress path.
- **Test**: `compilefail/cf_enum_unterminated.skink` (compilefail harness now wraps the compiler in `timeout 10` so hangs fail rather than wedge the suite); deep corpus-mutation fuzzing (3,222 exhaustive compiles + seed sweeps) in `compiler_fuzzgen_test`.

## 99. Array element destructors never ran — wrong RC header offset; delete() left stale slots — 🔧 FIXED

- **Severity**: memory leak (every slice/map/set backing array with heap-reachable elements leaked all element references on free); latent double-release once fixed
- **Symptom (found via ASAN on a real workload — CHRIS SNN)**: ~40KB leaked across `[]Neuron.synapses`, `Buckets.GetNext` reslices, `SNN.active`, etc. `make([][]Bucket, 8)` + `b[0] = b[0][:0]` leaked the empty reslice deterministically.
- **Root cause**: `getOrCreateArrayDtor` (`std/emitter.skink`) emitted a destructor that read the element "total size" at payload **-16** — the RC header's `magic` field (`0xDEADBEEFCAFEBABE`, negative as i64) — and divided by element size. `count` was always ≤ 0, so the release loop never ran: **every array destructor was a silent no-op**. A second, shallower bug: `needsArrayDtor`/`getOrCreateArrayDtor` looked up `structLayouts` with `struct.X` keys (stripping only `%`), never matching layout keys (`X`/`mod.X`/`mod_X`) — fixed via `structLLKey`.
- **Fix**: the dtor now reads the scan count at payload **-8** — the header `size` slot every slice alloc path already repurposes for element count (read by `len()`). Map/set backing arrays are not slices and never wrote it, so `emitMapArrayAlloc`/`emitSetLiteral` now store their slot **capacity** there. Since dtors scan raw slots, `emitDeleteCall` now stores `null` into released key/value slots — otherwise the just-freed old arrays' dtors would double-release stale pointers (UAF when the deleted value is still aliased elsewhere).
- **Test**: `std/compiler_collections_test.skink` — `TestArrayDtorReleasesNestedSlices`, `TestArrayDtorReleasesStructFields` (live-object delta via `Skink_rc_live_objects`), `TestMapDeleteKeepsSharedAlive` (aliased value survives delete+rehash). ASAN repro in /tmp/leakprobe verified clean.

## 100. Pointer-typed call arguments marked borrowed — caller's release suppressed — 🔧 FIXED

- **Severity**: memory leak (any `*T`/`i8**`/`%map_*` variable passed as a call argument leaked the referenced object plus its whole subtree)
- **Symptom**: `n := &Net{...}; ...; work(n)` as the last use of `n` never released `n` — `n`'s scope-exit release was elided, so the box and everything under it orphaned.
- **Root cause**: `emitCallExpr` (`std/emitter.skink`) marked identifier arguments `borrowed` whenever the *parameter* type ended in `*`. Borrowed is only correct when the callee receives a pointer into the caller's storage — i.e., a by-value composite argument (`%struct.X`, passed as `&var`). A pointer-typed variable passes its pointer value by copy; suppressing the release leaked it.
- **Fix**: the borrowed rule now checks the argument's own LLVM type — only by-value composite args (no trailing `*`) are marked borrowed.
- **Test**: `TestPointerArgKeepsOwnership` in `std/compiler_collections_test.skink` (live-object delta via `Skink_rc_live_objects`).

## 101. Zero-size RC payloads invisible to LeakSanitizer — false leak reports at exit — 🔧 FIXED

- **Severity**: false-positive leak reports (masking real leaks; every live `make([]T, 0)` / empty string/slice/struct box at exit was reported)
- **Symptom (found via ASAN on CHRIS SNN)**: `TestRunStop` reported exactly 10×24-byte "leaks" (`buckets.New` inner `[]Bucket`s, output-neuron `synapses`, `SNN` empty-slice fields) — all of them `Skink_rc_alloc(0)` blocks. Instrumented tracing showed complete `ALLOC → RELEASE status=2 → FREE` lifecycles: the blocks were freed, yet still reported.
- **Root cause**: a zero-size payload allocates exactly `RC_PREFIX_SIZE` (24) bytes, so the payload pointer equals the chunk's **end** address. LeakSanitizer (like most heap scanners) only treats strictly-interior pointers as reachability roots — a one-past-the-end pointer does not root the chunk. Every reference to an empty block — registry key in `rc_table`, fields, stack slots — was therefore invisible, and the blocks were reported regardless of lifecycle state. A related smaller window: `rc_table_remove` ran before `free()`, leaving in-flight frees on detached workers allocated-but-unregistered during a stop-the-world scan.
- **Fix**: `checked_rc_allocation_size` (`std/runtime/rc_rt.c`) now allocates at least one payload byte, so every payload pointer is strictly interior and roots its chunk. Additionally, an `rc_pending` list roots each dying block from registry removal until `free()` returns (pushed under the same critical section, popped after `free`), closing the remove→free window in `Skink_rc_release`, `Skink_mem_free`, `Skink_mem_realloc`, and `Skink_slice_ensure`.
- **Test**: `TestZeroSizeAllocLifecycle` (`std/compiler_collections_test.skink`), `TestSpawnTeardownZeroSize` (`std/compiler_concurrency_test.skink`); CHRIS `snn_test` + full program verified ASAN-clean.

## 102. Struct Drop methods ran + auto field release — double-release of manually-freed fields — 🔧 FIXED

- **Severity**: use-after-free (double-release of RC-managed fields inside destructor)
- **Symptom**: `regex_test.TestFindAllRegex` returned garbage `Text` values — dangling pointers into memory reused by later allocations.
- **Root cause**: `regex.Match` declared `pub fn Drop` doing `libc.free(self.Text)` — written under the old regime where a `Drop` trampoline *replaced* automatic field release (and where array dtors were inert no-ops, so it rarely ran). Once struct destructors became "Drop + auto-release every RC field" (#99 follow-up), `Match` teardown released `Text` twice per element: once inside `Drop`, once via the field-release path — freeing a string still referenced by the live `[]Match` element.
- **Fix**: `std/regex.skink` — removed `Match.Drop` entirely; `Text` is an RC string auto-released by the field destructor. `samples/regex_matching.skink` updated (explicit `m.Drop()` calls removed). Semantic rule: `Drop` is for resources outside RC (file handles, C pointers); RC-managed fields are always auto-released — a `Drop` that needs to release a field early must `delete()` it (nulls the field so the auto-release no-ops), never bare-release it.
- **Test**: `regex_test` suite green.

## 103. Map/set backing arrays wrote slot count only for dtor element types — copy() memcpy overshoot — 🔧 FIXED

- **Severity**: heap-buffer-overflow (any `copy(map[string]int)`-style call on a map/set whose element arrays have no destructor read past the allocation)
- **Symptom (ASAN, `copy_test.TestCopyMap`)**: `memcpy` read 128 bytes past a `map[string]int` values array.
- **Root cause**: `emitSliceCopyArr` reads payload **-8** as the element count and memcpys `count × elemSize`. `emitMapArrayAlloc` only stored the slot count there in the element-has-destructor branch; non-dtor arrays (e.g. `i32`/`i64` values) kept the RC header's *byte* size in that slot, so copy multiplied the byte size by the element size a second time.
- **Fix**: `emitMapArrayAlloc` and `emitSetLiteral` now store the slot capacity/count at payload -8 unconditionally. Safe because the runtime registry (`RCEntry.size`) separately tracks byte capacity — `Skink_mem_realloc` reads the registry, not the header slot.
- **Test**: `copy_test` ASAN-clean (was the reproducer).

## 104. `[]uint8`/`[]int8` string interpolation treated as C string — strlen overread — 🔧 FIXED

- **Severity**: heap-buffer-overflow (`"${bytes}"` on a byte slice passed the raw non-terminated buffer to `strlen`)
- **Symptom (ASAN, `compiler_narrow_int_test.TestInterpIntSlice`)**: `strlen` read 3 bytes past a 2-element byte slice.
- **Root cause**: `[]uint8` and `string` share the LLVM type `i8*`; `emitExprAsStringT` returned every `i8*` unchanged as a null-terminated string.
- **Fix**: `emitExprAsStringT` now uses the checker's semantic type — `typeIsByteSlice` routes byte slices through `emitArrayToString` instead of the C-string path.
- **Test**: `compiler_narrow_int_test` ASAN-clean.

## 105. Futures never freed — leaked 104B per async call — 🔧 FIXED

- **Severity**: memory leak (one `Skink_future` per `async` call: ~104 bytes + pthread_mutex_t/cond_t internals)
- **Symptom (ASAN, `async_test`, book `16_concurrency` examples)**: `Skink_future_make` calloc'd struct live at exit for every `async`.
- **Root cause**: futures were raw `calloc` allocations with no free path; `Skink_future_get` returned the result but left the future.
- **Fix**: `std/runtime/conc_rt.c` — futures now use `Skink_rc_alloc_typed` with a destructor that destroys the mutex/cond (guarded by init-state flags so partially-constructed futures tear down safely). `Skink_spawn` retains the future for the detached worker; the trampoline releases it after `Skink_future_set`, so the future lives exactly as long as either side needs it — no premature destroy if the caller drops it while the worker still runs.
- **Test**: `async_test` ASAN-clean; book `16_concurrency_02/03` ASAN-clean.

## 106. Native resource handles (pthread primitives, regex_t, sqlite3) never reclaimed — 🔧 FIXED

- **Severity**: memory/resource leak (every lazily-allocated `Mutex`/`RWMutex`/`Cond`, every compiled `Regex`, every unclosed sqlite connection/statement leaked 40–56B or more)
- **Symptom (ASAN)**: `Skink_mutex_alloc` (40B), `Skink_rwlock_alloc` (56B), `Skink_cond_alloc` (48B) leaks in `sync_test`/`compiler_*`/web-live tests; `regex_t` leaks in `regex_test`/`fuzz_test`; ~48KB sqlite3 in `db_test`.
- **Root cause**: these structs hold opaque `int64` handles — invisible to RC and to LeakSanitizer reachability. A `Drop` method can't fix them: `sync.NewCond`/`regex.Compile` return the struct **by value**, so a field-freeing Drop would fire on the return temp and UAF the copy (the documented reason the emitter doesn't auto-run `Free`/`Close`).
- **Fix**: `std/runtime/rc_rt.c` — new **kind-2 "tracked resource"** registry entries (`Skink_res_track`/`Skink_res_untrack`) keep the native block reachable (killing the LSan report while live) and an `atexit` sweep (`rc_sweep_resources`) runs each registered destructor at exit. Explicit `Free`/`Close` paths untrack first, so the sweep can't double-destroy. The sweep delegates teardown entirely to the dtor — `sqlite3_close_v2`/`sqlite3_finalize` free their own blocks, so the sync/regex dtors `free()` theirs explicitly. `Skink_mem_free`/`Skink_mem_realloc` ignore kind-2 entries.
- **Test**: `sync_test`, `regex_test`, `db_test`, `compiler_concurrency_test`, `compiler_fixes_test`, `web/server_live_test`, `web/server_tls_live_test` all ASAN-clean.

## 107. C runtime returned raw malloc'd buffers as Skink strings — leaked on every call — 🔧 FIXED

- **Severity**: memory leak (per-call; `os.ExecWithOutput` leaked two 4KB buffers per invocation — the dominant leak in `fs_proc_test`, `compilefail_test`, `compiler_fuzz*_test`)
- **Symptom (ASAN)**: leaked `malloc` buffers attributed to `Skink_os_exec_with_output`, `Skink_tls_peer_cert_cn`, `Skink_tls_get_alpn`, `Skink_tls_get_servername`, `Skink_ui_edit_text`, `Skink_format_float`.
- **Root cause**: these functions return `char*` that Skink treats as an RC-managed `string` and eventually passes to `Skink_rc_release` — but the buffers were raw `malloc`/`strdup`, invisible to the RC registry and never freed.
- **Fix**: all of them now allocate via `Skink_rc_alloc` so normal Skink ownership reclaims the buffer. (`tls_rt.c`, `fs_rt.c` POSIX + Windows paths, `ui_rt.c`, `panic_rt.c`.)
- **Test**: `fs_proc_test`, `compilefail_test`, `compiler_fuzz_test`, `compiler_fuzzgen_test`, `web/server_tls_live_test` ASAN-clean.

## 108. `copy(map)` raced concurrent map growth — stale/freed backing-array memcpy — 🔧 FIXED

- **Severity**: heap-use-after-free / heap-buffer-overflow (flaky; `copy(m)` on a map another thread is growing could memcpy from a freed or reused backing array)
- **Symptom (ASAN, CHRIS `hormones_test` / standalone probe)**: `memcpy` read 1024 bytes from a 152-byte keys array during `Hormones.GetEffective`'s `copy(h.effectives)` while a worker inserted into the map.
- **Root cause**: `emitMapCopy` loaded `keys/vals/states` pointers and passed them straight to `emitSliceCopyNullable`, which reads payload `-8` (slot count) and memcpys — with no pin. A concurrent `m[k]=v` grow replaced the arrays and released the old ones; after the chunk was reused, `-8` held a fresh RC *byte* size (e.g. 128) which was misread as an element count → 128×8-byte memcpy. (Reproducer: main thread `copy()`s in a loop while a worker inserts; failed ~1/3 of runs before the fix.)
- **Fix**: two layers in `std/emitter.skink` + `std/runtime/rc_rt.c`:
  1. `emitMapCopy` now pins each backing array: `Skink_rc_retain` the loaded pointer, reload the field, and retry if the pointer changed underfoot (`Skink_rc_retain` is safe on a stale pointer — registry lookup rejects dead entries without dereferencing). Copy runs while pinned; pin released after.
  2. New bounded **retire quarantine** in `rc_rt.c` (`rc_retire`, `RC_RETIRE_CAP` FIFO ring): dead RC blocks are dtor'd immediately but `free()` is deferred, so a racing foreign reader sees stale-but-valid memory instead of freed/reused memory. Applied to `Skink_rc_release`, `Skink_mem_free`, `Skink_mem_realloc`, `Skink_slice_ensure` free paths.
- **Note**: this is a *hardening* fix — a program that mutates a map while another thread copies it still has a data race (CHRIS's `effectives` is such a case: `update_mutex` guards only `update_flag`). The runtime now degrades it to a bounded stale read instead of OOB memcpy / UAF.
- **Test**: `TestMapCopyConcurrent` in `std/compiler_concurrency_test.skink` (worker inserts + main copies under ASAN); race probe 12/12 clean; CHRIS `hormones_test` ASAN-clean with repo compiler.

## 109. Bare `return` (named returns) rejected when a `defer` is in scope — 🔍 OPEN (low)

- **Severity**: minor usability inconsistency (compile error, workaround trivial)
- **Symptom**: `fn f() -> (r: int) { defer x(); r = 1; return }` → `missing return value (expected int)`. The same bare `return` compiles fine without the `defer`.
- **Root cause**: the checker's return-arity validation for bare `return` doesn't run through the deferred-return path.
- **Expected**: bare `return` should behave identically whether or not defers are registered.
- **Workaround**: write `return r` explicitly. Also note deferred closures mutating named returns do not affect the returned value (snapshot-before-defer semantics — matches the emitter's ordering; documented in the book ch.5).

## 110. `[]T` parameters in generic functions read wrong stride — 🔍 OPEN (high)

- **Severity**: wrong results / out-of-bounds reads (silent data corruption when a concrete `[]int`, `[]int8`, etc. is passed to a `[]T` parameter)
- **Symptom**: inside `fn f<T>(items: []T)`, `items[i]` reads elements with an 8-byte `i8*` stride. A `[]T` created *inside* generic code is boxed and works; a `[]T` passed in from concrete code keeps its natural element size → garbage reads for `T` narrower than 8 bytes.
- **Root cause**: type erasure lowers `[]T` to `i8**`/8-byte-element arrays unconditionally; the parameter boundary performs no re-boxing of the caller's concrete backing array.
- **Expected**: either re-box the argument array at the call boundary, or emit generic bodies that index with the caller's element stride.
- **Workaround**: pointer-sized elements (`[]string`, `[]*T`) work; for narrow types use concrete non-generic helpers (`sort.Ints`) or copy into a generic-owned slice first.

## 111. `s[^i]` from-end indexing skips bounds checks — 🔍 OPEN (medium)

- **Severity**: silent out-of-bounds read (returns 0 / garbage instead of panicking)
- **Symptom**: `arr[^0]` on `[]int{10,20,30}` returns 0 — it computes `len-0` = 3, an out-of-bounds index, but no bounds check fires. `arr[3]` panics correctly.
- **Root cause**: `^` desugars to `len(s) - i` and the emitted index path apparently bypasses the bounds-check branch used by plain indexing (or the check compares against capacity, not len).
- **Expected**: `s[^0]` should panic like `s[len(s)]` does; `s[^i]` should panic when `i > len` or `i <= 0`... actually `^0` is just `len` — should panic on `i < 1` too unless `^0` is defined as past-the-end.
- **Workaround**: none needed once fixed; avoid `^0`.

## 112. Generic struct literal `Pair<K,V>{...}` rejects concrete instantiation — ✅ FIXED

- **Severity**: missing feature (compile error, workaround easy)
- **Symptom**: `p := Pair<string, int>{key: "age", value: 30}` → `field "key" type mismatch: expected K, got string` / `expected V, got int`. The checker validates literal fields against the *unsubstituted* type parameters.
- **Root cause**: struct-literal checking doesn't substitute the instantiation's concrete type arguments into the declared field types. (Separately, explicit call-site type args `NewPair[string,int](...)` don't parse at all — inference is the only path.)
- **Fix**: `checkStructLit` now builds a substitution map from `decl.structGenerics` to `e.genericArgs` and applies `substituteType` to each declared field type before comparing (`std/checker.skink`). `substituteType` gained TK_CHAN/TK_SET arms so `chan<T>`/`set<T>` fields substitute too. Field *access* on instantiated generics (`s.items`, `q.ch`) now substitutes as well — `checkFieldAccessExpr` and `resolveFieldType` build the same param map from the receiver's `genericArgs`, which also covers #115's `p.value` on `NewPair<K,V>` returns.
- **Regression tests**: `TestGenericStructLitSubst`, `TestGenericStructLitChan` in `std/compiler_fixes_test.skink`. Verified against all 25 `skink-samples` generic struct-literal files (`generic_stack_*`, `generic_concurrent_queue_*`).

## 113. Module-level `var m map[K]V` emits undefined LLVM type + nil-map global — ✅ FIXED

- **Severity**: compile error (clang: `use of undefined type named 'map_i8Ptr__i32'`) and, once past that, runtime `panic: assignment to entry in nil map`
- **Symptom**: a global `var m map[string]int` emits `@m = global %map_i8Ptr__i32* null` but the named struct type was never declared, so clang rejected the IR. Even when the type decl was emitted, the global stayed `null` forever — the global-init function only ran statements with explicit initializers — so the first `m[k] = v` panicked. The same `var` inside a function worked fine.
- **Root cause**: map types lower to a named `%map_*` struct pointer; the type declaration was only emitted on paths that allocate maps (`make(map)`, literals), and `globalInits` only collected vars with `varValue`, so module-level maps were never allocated.
- **Fix**: `std/emitter.skink` — the module-scope `ST_VAR` loop now emits the `%map_`/`%set.` named type decl for unallocated globals, `globalInits` also collects `%map_`-typed vars without initializers, and `_skink_global_init` synthesizes `make(map[K]V)` for them. Sets are excluded deliberately: `set{}` emits `null`, so null is already the canonical empty set (`in`/`len` handle it) and `emitMakeExpr` has no set arm.
- **Expected**: declaring `var m map[string]int` at global scope now yields a usable empty map.
- **Regression test**: `TestModuleLevelMapDefaultInit` in `std/compiler_fixes_test.skink` — verified `panic: assignment to entry in nil map` on the pre-fix stage-0 binary.

## 114. Slice views cannot carry `s[0:k]` or same-offset different-length views — 🔍 OPEN (low, documented)

- **Severity**: semantic edge case (safe; degrades to copy, no corruption)
- **Symptom**: `s[0:k]` / `s[:k]` with `k < len(s)` produces a copy, not a shared view — `sub[0] = x` does NOT reach the source. Likewise, if `s[i:j]` and `s[i:k]` (j != k) are both live, the second-created one silently copies.
- **Root cause**: a Skink slice is a thin element pointer; its length lives in the base allocation's header slot at payload-8. A view starting at offset 0 aliases that slot and therefore cannot carry a different length. The view registry (kind-3 RCEntry) is keyed on the interior pointer, so two simultaneously-live views of the same address can only report one length — `Skink_slice_view` returns NULL on a length collision and the emitter falls back to the copy path.
- **Expected**: Go-style fat slices (ptr+len) would fix both cases, at the cost of an ABI change touching every slice-typed site.
- **Workaround**: use `copy()` when you need a detached prefix; document the prefix-copy rule (book ch.6 updated).

## 115. Explicit generic call args `f<T,...>` parse but leave result fields unsubstituted — ✅ FIXED

- **Severity**: wrong types surface downstream (compile error like `expected int, got V` on field access — confusing but safe)
- **Symptom**: `Identity<int>(42)` works end-to-end, but `NewPair<string, int>("age", 30)` compiles while `p.value` still type-checks as the parameter `V`, so `io.Itoa(p.value)` fails. Note square brackets do NOT parse — `NewPair[string, int](...)` is a syntax error; the explicit form is `f<...>`.
- **Root cause**: call-site type args feed substitution of the function's signature, but the returned `Pair<K,V>` is not re-substituted per instantiation for field access (same gap as #112's literal path).
- **Fix**: resolved by the #112 field-access substitution — `checkFieldAccessExpr`/`resolveFieldType` now substitute `structGenerics` → receiver `genericArgs`, so `p.value` on an explicit `NewPair<string,int>(...)` result types as `int`. Verified end-to-end: explicit and inferred forms both compile and run correctly.

## 116. `fn main` in a non-main module emits a duplicate `@_skink_main` — ✅ FIXED

- **Severity**: compile error blocking `skink -t` on any library module that keeps its own `fn main` (all skink-samples files).
- **Symptom**: `invalid redefinition of function '_skink_main'` when a test binary imports a module containing `fn main` — e.g. `skink -t generics/generic_stack_0001_test.skink`.
- **Root cause**: `emitFnDecl` renamed every function whose last dotted component was `main` to `_skink_main`, regardless of the decl's module.
- **Fix**: `std/emitter.skink` — the rename now requires the owning module to be `main` (qualified `main.main`, or bare `main` with `d.moduleName` empty/`main`). Library `fn main`s emit under their qualified name like any other function.
- **Regression test**: `std/fixture_libmain/` fixture — `libmain.skink` carries `fn main`; `libmain_test.skink` compiles+runs against it.

## 117. `err == nil`/`!= nil` on a zero `errors.Error` derefs a null message pointer — ✅ FIXED

- **Severity**: runtime segfault (SIGSEGV in generated code)
- **Symptom**: `_, err := f(); if err != nil {...}` where `f` returns `errors.None()` (or a `nil`-coerced zero `Error` after the #112-era coercion fix) crashed. `errors.None()` returns `Error{message: ""}`; a raw `zeroinitializer` error has `message == null`.
- **Root cause**: `emitInfixExpr`'s error-vs-nil path computed `icmp eq i8* %msg, null` and then unconditionally `load i8, i8* %msg` to test the empty-string case — the `or` combining both checks is not short-circuiting, so a null message pointer was dereferenced before the comparison completed.
- **Fix**: `std/emitter.skink` — the byte load now goes through `select i1 %msgIsNull, i8* %zeroByteAlloca, i8* %msg`, where the alloca is initialized to 0, so a null message yields "empty" without a deref.
- **Regression test**: `TestNilErrorStructCmp` in `std/compiler_fixes_test.skink`.

## 118. String literal `"nil"` parsed as the `nil` literal — ✅ FIXED

- **Severity**: runtime segfault (string arg emitted as `i8* null`, e.g. `io.Print("nil")` → `strlen(NULL)`)
- **Symptom**: a string literal whose contents were exactly `nil` compiled to a null pointer instead of a string global — visible as `call void @io.Print(i8* null)` with no `@str.N` emitted for the literal.
- **Root cause**: `parsePrefix` ran `isNilLit(tok.literal)` (literal-text match, "nil") before the `T_STRING` arm, so a string token whose contents were `nil` produced an `EX_NIL` node.
- **Fix**: `std/parser.skink` — both `isNilLit` call sites in `parsePrefix` now require the token kind to be `T_NIL` or `T_IDENT` before matching on literal text.
- **Regression test**: `TestStringLitNilNotNilLiteral` in `std/compiler_fixes_test.skink`.

## 119. Rendezvous channel lost wakeup: mixed `send_cv` waiters → ~50% deadlock — ✅ FIXED

- **Severity**: runtime deadlock (intermittent, ~50% per run)
- **Symptom**: two spawned senders on an unbuffered channel plus two receives intermittently hung forever — e.g. the tutorial 15 WaitGroup example in unbuffered form. Both sends logically complete but the second sender never woke.
- **Root cause**: `std/runtime/conc_rt.c` — `Skink_chan_recv` dequeued one element and called `pthread_cond_signal(&ch->send_cv)`, waking a single waiter. On a capacity-0 channel, `send_cv` waiters are a *mix*: post-enqueue senders (waiting for their element to be consumed — satisfied by this dequeue) and pre-send waiters (waiting for `count == 0` so they can enqueue). If the lone signal landed on the already-satisfied sender, the queued sender was never re-signaled and slept forever, so the next `<-ch` blocked indefinitely. Which waiter woke was timing-dependent — hence the coin-flip.
- **Fix**: `Skink_chan_recv` now calls `pthread_cond_broadcast(&ch->send_cv)` when `ch->capacity == 0` (broadcast lets the satisfied sender return AND the queued sender enqueue); bounded channels keep the single signal since all their waiters share the same condition. Synced to `runtime/` and `bootstrap-home/runtime/`.
- **Reproducer**: 20-run loop of the two-sender unbuffered program — 11 hangs pre-fix, 60/60 clean post-fix.
- **Regression test**: `TestRendezvousChanTwoSenders` in `std/compiler_concurrency_test.skink` — forces the sender1-parked / sender2-queued ordering with staggered spawns across 20 iterations.
