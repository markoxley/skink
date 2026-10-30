# Compiler errors / limitations — active list

New issues discovered since the archive at `artifacts/comp_error.md` (#1–#138). Continue numbering from #139.

## #139 — `fn main(args: []string)` compiles but `args` is always empty — FIXED 2026-10-19

- **Severity:** medium — silent wrong behavior; the program builds and runs but can never see command-line arguments.
- **Symptom:** `fn main(args: []string) -> int { println(len(args)) ... }` prints `0` no matter how many arguments are passed. No diagnostic.
- **Root cause:** the emitted C entry point calls `@_skink_main()` with no parameters (`emitRuntimeMain` in `std/emitter.skink`), so a `main` that declares an `args` parameter is invoked with it uninitialized/empty. argc/argv are only stored into runtime globals for `os.Args()` / `os.ArgsCount()`.
- **Fix:** `emitRuntimeMain` now detects a `main` with exactly one `[]string` parameter and passes the result of `@_skink_runtime_get_argv()` (argv as a slice header, identical to `os.Args()`, including argv[0]). `checkFnDecl` rejects any other parameter signature on module-main `main` (e.g. `fn main(n: int)`) with "main must take no parameters or a single []string parameter...". Library-module and method `main`s are unaffected.
- **Regression:** `std/main_args_test.skink` (standalone; asserts args == os.Args() and re-execs itself to verify argv passthrough), `std/compilefail/cf_main_args.skink` (rejected signature).
- **Found via:** skink-tutorials `16-building-a-small-project.md` uses this signature in `main.skink`; it now works as documented.

## #140 — `x in s` and `for v in s` segfault on a null set — FIXED 2026-10-20

- **Severity:** high — hard crash (SIGSEGV) at runtime.
- **Symptom:** `for v in h.s` or `x in h.s` where `s` is an uninitialized `set<T>` struct field (or any other null set value) segfaults. `len()` on the same null set was already guarded (#138); `in` and `for` were not.
- **Root cause:** `emitSetInOp` and `emitForInSetStmt` in `std/emitter.skink` GEP'd and loaded `{data, count}` fields through the set pointer unconditionally — a null set hits address ~8 and dies.
- **Fix:** both emitters now emit `icmp eq %set.T* reg, null` first and branch to the shared end label — `in` reports false, `for` iterates zero times. (The `in` fix uses the result-alloca path already present; `endLabel` was hoisted above the field loads in both functions.)
- **Regression:** `std/sets_test.skink` `TestNullNativeSetOps` / `TestFromSetEmpty`.
- **Workaround (pre-fix):** always assign a non-empty `set{...}` literal to set fields, or guard with `len(s) > 0`.

## #141 — erased `i8*` element comparisons use `strcmp`/`Skink_str_hash` — FIXED 2026-10-21

- **Severity:** high — hard crash inside generic (template) code.
- **Symptom:** inside a generic function/type, `x in s` on `set<T>`, `a == b` on two erased values, or `m[k]`/`m[k]=v`/`delete(m,k)`/ `for k in m` on `map[K]V` crashes when the concrete element/key type is not a string — erased `i8*` values are `inttoptr`'d integers and `strcmp`/`Skink_str_hash` dereferences them.
- **Fix (sets):** `emitSetElemCompare` calls `reflect_equals(a, b, "")` for `i8*` elements — mapped pointers compare by string content, unmapped values by raw bits — so `in` and literal dedup work for erased int/bool/float/pointer elements. Real `set<string>` behavior is unchanged.
- **Fix (maps):** new `emitMapKeyHash`/`emitMapKeyCompare` route `i8*` keys through `reflect_hash`/`reflect_equals` with a type-name hint from `mapKeyReflectName` — `"string"` when `exprTypeLookup` proves the map's semantic key type is builtin string (same hash/compare semantics as `Skink_str_hash`/`strcmp`, no heuristic overhead), `""` for erased keys (mapped-memory heuristic). Threaded through `emitMapIndex`, `emitMapIndexOk`, `emitMapIndexAssignment`, `emitDeleteCall`, and map literals. `reflect_hash` is declared beside `reflect_equals` in the runtime preamble.
- **Fix (== / !=):** `emitInfixExpr`'s `i8*` equality path previously `strcmp`'d both operands (a literal-only `isIntegerLiteralExpr` escape existed, but `x == y` on two generic vars still crashed). Both operands now go through `reflect_equals` with a `"string"` hint only when both sides are provably builtin string — a mixed string/erased pair keeps the heuristic so `inttoptr` values are never dereferenced. Real string `==` semantics are preserved.
- **Regression:** `std/sets_test.skink` (set side); `std/compiler_types_test.skink` `TestGenericMapScalarKeys`, `TestGenericMapStringKeys`, `TestGenericErasedEquality` (map and ==/!= sides — all segfaulted before the fix).

## #142 — field access on an instantiated generic struct returns the erased type — FIXED 2026-10-21

- **Severity:** medium — wrong static type; runtime crash if the value is used as its concrete type.
- **Symptom:** `w := WrapA<int>{elems: []int{3,4}}` then `w.elems[0]` (field `elems: []T`) yielded an erased `i8*` element — `println(w.elems[0])` treated it as a string pointer and segfaulted (`strlen` on `inttoptr(3)`). Scalar `v: T` fields similarly read raw `inttoptr` boxes. By contrast, inside `fn f(w: WrapA<int>)`, `w.elems[0]` was correctly `int`.
- **Root cause:** not the checker — `checkFieldAccessExpr` already substitutes `T→int` via `genericArgs`, and the struct literal records them. The emitter's `exprFieldLLType`/`emitFieldAccessExpr` derived the field type solely from the single erased `StructLayout` slot (`[]T → i8**`, `T → i8*`), so `exprLLType(w.elems)` reported `i8**` and every downstream consumer (index loads, for-in, append, println) used the erased element type.
- **Fix:** when the receiver's semantic type is an instantiated generic (`receiverIsGenericInstance` — `genericArgs` non-empty on the named or pointed-to type), `exprFieldLLType` returns `llvmType` of the checker's substituted field type instead of the layout slot type, and `emitFieldAccessExpr` casts the loaded slot value to that type via the new `emitCastLL` (i8\* → concrete unboxes through `emitSetElemUnbox`, pointer → pointer bitcasts). Compound assignment (`w.v += 1`) unboxes before arithmetic and reboxes for the store. Indexing, `for-in`, `append`, `len`, and `delete` then see `[]int`/`int`/`map[int]V` automatically because they all derive element types through `exprLLType`.
- **Regression:** `std/compiler_types_test.skinkTestGenericStructFieldAccess`, `TestGenericStructFieldWrite`, `TestGenericStructStringFields`, `TestGenericStructNestedFields` — `w.elems[0]` on `GenWrap<int>` segfaulted before the fix.

## #143 — `set<T>` element storage is element-sized, so erased generic reads misalign — FIXED 2026-10-20

- **Severity:** high — silent data corruption inside generic code.
- **Symptom:** passing a concrete `set<int>` to a generic `s: set<T>` parameter produced garbage on `for v in s` / `x in s` — the erased reader walks the backing array as `i8*` (8-byte stride) while the literal stored raw `i32` elements (4-byte stride). `sets.FromSet[int]` returned a `Set` full of misaligned values and `Has`/`Contains` missed real members.
- **Root cause:** set backing arrays were `elemLL*` — element-sized — but `set<T>` parameters lower to `%set.i8Ptr*`, whose readers assume an `i8*` element stride. Unlike `[]T` and `map[K]V` (which the checker rejects for non-pointer-sized instantiation), `set<T>` params were not guarded, so the mismatch compiled silently.
- **Fix:** uniform boxing. `emitSetTypeDecl` now always declares `{ i8**, i32 }`; `emitSetLiteral` boxes every element via the new `emitSetElemBox` (`inttoptr` for integers, `bitcast`-through-`inttoptr` for floats, `bitcast` for pointers/strings/structs, RC-heap-copy for by-value composites); `emitSetInOp`, `emitForInSetStmt`, and the literal's dedup scan read `i8*` and unbox via `emitSetElemUnbox`. Concrete semantics are unchanged (int `in` still lowers to `icmp` after the unbox); generic `set<T>` now reads the same layout the literals write.
- **Regression:** `std/sets_test.skink` — all `TestNative*` cases plus `TestFromSet`/`TestFromSetEmpty` exercise a concrete `set<int>` across a generic `set<T>` boundary.
- **Residual (re-assessed):** a `set<T>` _literal_ emitted inside generic code still gets the i8\* array destructor, which calls `Skink_rc_release` on each element — but release is table-guarded (`rc_table_find` in `rc_release_locked` returns early for pointers not in the live-allocation table), so `inttoptr`'d scalars are safe no-ops rather than wild frees. Verified: `mkAndProbe<int>` creating and releasing `set{a,b}` inside a generic fn runs clean. The only remaining hazard is theoretical: an integer value that numerically aliases a live RC pointer would be released — inherent to untagged erasure, same as the mapped-memory heuristic in `reflect_equals`/`reflect_hash`. By-value composite set elements remain unhandled (`icmp` cannot compare aggregates); the box path merely keeps storage correct.

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
- **Residual hazard (unfixed, inherent):** the mapped-memory heuristic classifies an erased value as "string" if its _start_ page is mapped. `skink_streq`/`skink_hash_string` then walk bytes unboundedly; a misclassified `inttoptr` value near a page boundary can read into an unmapped page and segv. Fully fixing requires bounded/validated scans or richer type tagging at erasure sites.

## #146 — std/web has no trusted client-IP plumbing; `web_ctx.IP()` headers are forgeable — FIXED 2026-10-24

- **Severity:** Medium (security-sensitive API returning attacker-controlled data)
- **Symptom:** `modules/skink-web/web_ctx.skink`'s `IP()` reads `remote_addr` then `x-forwarded-for` from the request headers, but `std/web`'s server never injects either — both can arrive as ordinary client-supplied headers, so any consumer (rate limiter keys, IP allowlists, audit logs) can be spoofed.
- **Root cause:** `net.Connection` exposes no peer-address accessor (no `getpeername` wrapper), and `web/types.Request` has no field to carry it — the server layer has nothing trustworthy to hand the request.
- **Fix:** `net.Connection.PeerAddr()` wraps `getpeername`+`inet_ntoa` and returns `(ip, port)`; `types.Request` gained a `peer` field with `PeerAddr()`/`SetPeer()`; `server.readRequest` records `ip:port` from the socket on every accepted request; `web_ctx.IP` returns the socket peer when present, then `remote_addr`, then XFF. Priority: real peer > remote_addr > XFF.
- **Regression:** `modules/skink-web/mw_test.skink` `TestIpRemoteAddrWins` + `TestIpPeerFieldWins`; `net.PeerAddr` verified live (loopback dial/accept).

## #147 — `match` used as an identifier produces a confusing parser error — FIXED 2026-10-24

- **Severity:** Low (diagnostic quality)
- **Symptom:** `match := true` inside a block yields `expected { (got 'true' "true")` / `expected => (got 'for')` cascades — nothing tells the user `match` is a contextual keyword that can't be a variable name.
- **Root cause:** the parser treats `match` as starting a match-expression in statement position without checking the following token, so `match :=` is parsed as `match` on a bogus scrutinee.
- **Fix:** `parseStatement` now checks the token after `match` — when it is `:=`, `=`, `,`, `;`, `.`, or `[` (none of which can begin a match subject), the parser emits `cannot use 'match' as an identifier` at the keyword and drops it so follow-on errors stay local instead of cascading through match-arm parsing.
- **Regression:** `std/compilefail/cf_match_ident.skink` — `match := true` now yields the clear diagnostic.

## #148 — generic `hashmap`/`reflect_hash` string-key lookups are binary-layout-dependent — FIXED 2026-10-24

- **Severity:** High (nondeterministic wrong results, no crash)
- **Symptom:** `modules/skink-vox/internal/wordlist.skink` kept `hashmap.New<string,int>` lazily populated; `basic.Has("the")` returned `false` inside `internal_test` in the full-suite binary while returning `true` in an otherwise-identical binary missing only the `std/testing` import from `main` — and from a probe module it always returned `true`. Same sources, same call path, different link layout → different result.
- **Root cause (confirmed):** `skink_looks_like_string` in `std/runtime/reflect_rt.c` scanned only `skink_probe_span` bytes for a NUL terminator, and on POSIX `skink_probe_span` returns bytes-to-end-of-page — so a string literal whose bytes+NUL straddled a page boundary was classified as an int, hashed by pointer value, and looked up in the wrong bucket (`reflect.Equals` pointer-compared too). Every observation explained: caller-dependence (different literal addresses → different page offsets), binary-layout dependence (literal packing decides which literals straddle boundaries), determinism per binary, and why any unrelated change shifted the outcome.
- **Fix:** `skink_looks_like_string` now walks forward across page boundaries — each new page is re-verified mapped+readable (VULN-17 contract preserved) and scanned for the NUL, capped at 64 pages (~256KB) to bound probing of non-string pointers. The earlier `wordlist` workaround (builtin `map[string]bool`) is retained regardless.
- **Regression:** `std/reflect_test.skink` `TestPageBoundaryStringHash` — builds `"the\0"` at pageSize-3 via `posix_memalign` so the NUL lands on the next page; fails on the unfixed runtime (pointer-hash `177266685` vs content-hash `114801`) and passes after. Also verified against the original `internal_test` binary shape.

## #149 — string element access was not bounds-checked (OOB read/write) — FIXED 2026-10-26

- **Severity:** high — OOB read (info leak) and OOB write (heap corruption).
- **Symptom:** `s[i]` and `s[i] = c` on a string emitted no bounds check (only slices were checked); an out-of-range index read past the allocation or wrote into adjacent heap. The write path was a reachable heap overflow; the read path leaked adjacent bytes.
- **Root cause:** `emitIndexBoundsCheck` explicitly skipped strings ("Plain strings are also unchecked… a strlen per index would make per-char loops quadratic").
- **Fix:** strings are now checked in both the read and write paths. New runtime `Skink_str_len` reads the RC header size field at payload-8 (O(1) for RC strings — allocation is always len+1) and falls back to strlen for literals/foreign text. Reads are bounded by the allocation length (len+1, so the NUL terminator byte reads as 0); writes are bounded by the string length (len) so the terminator cannot be overwritten. Because the stdlib built byte buffers with indexed writes (`buf[len] = 0` terminators and data writes into not-yet-terminated buffers), new raw byte helpers `Skink_mem_write_byte`/`Skink_mem_byte` were added and ~60 stdlib/tool sites converted (str Builder/Concat/Substr/ToLower/ToUpper/Repeat, fs ReadAll/Stat/ReadDir, net Recv, path, strconv Itoa/Itoa64, fmt, compress, crypto HMAC padding, os Exec status, web server/static/tmpl/types, i2c/spi/base64/reader/buf/csv/encrypt/api/regex, LSP, scales).
- **Regression:** `std/compiler_safety_test.skink` (`TestStringReadOOBPanics`, `TestStringWriteOOBPanics`, `TestStringSliceOOBPanics`, `TestStringIndexInBounds`, `TestStringIndexLoopPerf`).

## #150 — integer division/remainder by zero, INT_MIN/-1, and out-of-range shift counts emitted LLVM UB — FIXED 2026-10-26

- **Severity:** high — sdiv/srem/shl UB: optimizer miscompiles, x86 idiv traps.
- **Symptom:** `x/0`, `x%0`, `INT_MIN/-1`, `INT_MIN%-1`, and shifts with count < 0 or ≥ bit width produced raw LLVM `sdiv`/`srem`/`shl`/`ashr`/`lshr` instructions with no guard.
- **Fix:** new emitters `emitIntDivRemGuard` (zero divisor → panic "integer divide by zero"; signed INT_MIN/-1 → panic "integer overflow") and `emitShiftCountGuard` (count < 0 or ≥ width → panic "shift count out of range") applied in `emitInfixExpr` and `computeCompoundOp`.
- **Regression:** `std/compiler_safety_test.skink` (`TestDivZeroPanics`, `TestRemZeroPanics`, `TestIntMinDivNegOnePanics`, `TestIntMinRemNegOnePanics`, `TestShiftTooFarPanics`, `TestShiftNegativePanics`, `TestArithStillWorks`).

## #151 — embedded NUL bytes in strings — FIXED 2026-10-26

- **Severity:** medium — silent truncation everywhere (Len is strlen): path/command confusion, invisible trailing bytes, `s[i]=c` writes into the invisible region.
- **Symptom:** `"a\0b"` (the `\0` escape) or `str.Builder.WriteByte(0)` produced strings whose allocation was longer than their strlen; every stdlib operation (Concat/Substr/fs.WriteAll/exec args/paths) truncated at the NUL.
- **Fix (policy: NUL bytes are rejected):** the lexer returns T_ILLEGAL for the `\0` escape (positioned parse error); a raw NUL byte in source is reported as T_ILLEGAL instead of silently ending the file; `str.Builder.WriteByte(0)` panics.
- **Regression:** `std/compilefail/cf_nul_string.skink`, `std/str_test.skink` `TestWriteByteNulPanics`.

## #152 — str.Substr/Concat int32 overflow near 2 GiB — FIXED 2026-10-26

- **Severity:** low — OOB read / negative allocation for >2 GiB strings.
- **Fix:** 64-bit math in `Substr` (clamp) and `Concat` (panic "size overflow"), mirroring `Repeat`/`Builder.ensure`.
- **Regression:** `str_test.skink` `TestSubstrHugeIndices`.

## #153 — misc runtime/stdlib hardening batch — FIXED 2026-10-26

- fs.ReadAll returned raw (kind-1) buffers that RC release never frees → leak in every long-running caller (LSP, scales); now RC (`Skink_mem_alloc`) so ordinary release reclaims them. Regression: `fs_test.skink` `TestReadAllRCReclaimed`.
- `Skink_slice_len`'s legacy fallback read payload-8 of untracked/kind-1 pointers (wild under-read); now only registered kind-0 blocks use it. 
- `Skink_mem_realloc` on a kind-0 block with a destructor dropped the dtor (element leak); the replacement now preserves it.
- `Skink_chan_send` silently dropped values on malloc failure; now panics "out of memory". `Skink_chan_select` timeout arithmetic overflowed int32; now int64.
- `Skink_os_exec_with_output` captured unbounded child output (OOM) and the child inherited the parent's fds; output is capped at 64 MiB, inherited fds are marked close-on-exec (Linux /proc/self/fd), the pipe read end is CLOEXEC.
- `Skink_fs_mktemp_fopen` (driver's `.ll` temp file) removes the close-and-reopen TOCTOU window; `Skink_fs_mktemp_in` (LSP check files) creates O_EXCL 0600 files in the source directory.
- `os.Getwd` returned NULL on failure (crash in strlen callers); now returns "".
- `regex_rt.c`/`fs_rt.c` guard zero/negative buffer sizes.
- `cov_rt.c` writes the coverage file with O_NOFOLLOW.
- `Skink_format_float` documented; `compress` 2 GiB `Skink_compress_bound` overflow and `Inflater.Decompress` INT_MAX outCap rejected (also in C wrappers: negative lengths/capacities rejected).
- web: `URLDecode`/`stripCRLF` now RC (per-request leaks fixed); `%00` decode rejected; `ResponseWriter.WriteBytes` guards negative n; duplicate Content-Length rejected (comp_error.md #155).
- `fmt`: width capped (panic "width too large"), malloc failures panic; `str.Builder` NUL guard (#151).
- reflect erased-value scan bounded to the registered block size (no walk past the allocation).

## #154 — compress 2 GiB integer overflow — FIXED 2026-10-26

- **Severity:** medium — `Skink_compress_bound(2 GiB)` wrapped negative → undersized/NULL destination → zlib wrote into it; `Inflater.Decompress(outCap=INT_MAX)` overflowed the calloc size → zlib wrote to NULL.
- **Fix:** `Skink_compress_bound` rejects srcLen > INT32_MAX/2; C wrappers reject negative lengths/capacities; std/compress checks bound ≤ 0 and calloc NULL; `Inflater.Decompress` caps outCap at 1 GiB.
- **Regression:** `compress_test.skink` `TestInflaterHugeOutCap`, `TestCompressBoundRejectsHuge`.

## #155 — duplicate Content-Length headers accepted (request smuggling) — FIXED 2026-10-26

- **Severity:** medium — a front-end proxy and this server could disagree on the body boundary.
- **Fix:** `parseRequestHead` flags a second Content-Length; `readRequest` returns 400.
- **Regression:** `web/web_test.skink` (parser-level coverage); `server_live_test` raw-socket dup-CL check.

## #156 — import path traversal / unconfined imports — FIXED 2026-10-26

- **Severity:** medium — `import "../secret"`, absolute paths, and `mod/…` subpaths with ".." made the compiler read arbitrary files (info disclosure via diagnostics).
- **Fix:** `isUnsafeImportPath` rejects ".."/"."/empty/absolute/backslash segments (leading "./" still allowed — the diamond fixture uses it); mod subpaths and manifest `entry` fields validated; the ancestor walk-up stops at the project boundary (skink.toml/skink.mod/.git) so an ancestor-directory attacker cannot shadow std/vendor; `isUnsafeVersion` rejects leading '-' and path metacharacters.
- **Regression:** `std/resolver_test.skink` (`TestImportTraversalRejected`, `TestValidRelativeImport`), `std/compilefail/cf_import_traversal.skink`.

## #157 — fmt width overflow — FIXED 2026-10-26

- **Severity:** low — `%99999999999999999d` wrapped int32 → negative/undersized padding allocation.
- **Fix:** width capped at 1 MiB (panic "fmt: width too large"); malloc failures panic.
- **Regression:** `fmt_test.skink` `TestWidthTooLargePanics`.

## #158 — scales: tar-slip and git option injection — FIXED 2026-10-26

- **Severity:** high — a malicious package archive with ".." members overwrote arbitrary user files (tar-slip); an unvalidated `@version` string reached `git clone --branch <version>` as an option (`-c core.sshCommand=…` → command execution).
- **Fix:** `modres.IsSafeVersion` (charset, no leading '-', no path metacharacters) enforced in `ParseSpec` and `FetchPackage`; tar members are listed and validated (`TarMemberUnsafe`) before extraction with `--no-same-owner --no-same-permissions`; git uses `--branch=<version>` + `--`.
- **Regression:** `scales_test.skink` `TestUnsafeVersionRejected`, `TestTarMemberUnsafe`.

## #159 — LSP message framing and temp-file security — FIXED 2026-10-26

- **Severity:** medium — a crafted Content-Length drove a giant malloc (server crash on OOM); header lines grew unbounded; the predictable `.skink_lsp_check_<n>.lspcheck` name plus `fopen("w")` followed a planted symlink (arbitrary file overwrite in shared dirs).
- **Fix:** Content-Length capped at 64 MiB, malloc failures handled, header lines capped at 8 KiB; check files created via `Skink_fs_mktemp_in` (O_EXCL 0600, unpredictable names).

## #160 — deferred-closure writes to captured locals do not propagate on panic unwinds — RESOLVED (not a bug)

- Initially logged while writing #149 tests as a suspected propagation failure in `defer fn() { saved = recover() }()`. Re-verified with a direct repro: a deferred closure writing a captured local during panic unwind propagates correctly (`saved` reads `"boom"` in a subsequent deferred fn, and the closure capture mechanism boxes the var by reference). The earlier observation was a test-scoping artifact — the capture was read by the *caller* after `invoke` returned, but the captured local is only visible within the deferring frame. No compiler defect.

## #161 — stdlib gaps found porting a real project (skink-nexus) — DOCUMENTED (worked around, not fixed)

Not compiler bugs — missing stdlib surface and sharp edges hit while porting
the Go Nexus pub/sub server to Skink (`~/Projects/test/skink-nexus`). All
worked around in-project; candidates for future stdlib additions.

**std/encrypt — no RSA-PSS.** `RSACipher.Sign`/`Verify` is
PKCS1-v1_5/SHA-256 only; Go used `rsa.SignPSS`/`VerifyPSS`. Workaround: call
libcrypto directly via externs — `RSA_padding_add_PKCS1_PSS_mgf1` +
`RSA_private_encrypt(RSA_NO_PADDING)` to sign, `RSA_public_decrypt` +
`RSA_verify_PKCS1_PSS_mgf1` to verify (sLen=-2 = max-salt sign / auto verify,
matching Go's nil PSSOptions). Would fit naturally in std/encrypt as
`SignPSS`/`VerifyPSS` or a padding-mode parameter.

**std/yaml — gaps for real-world config files.** `Unmarshal` handles nested
maps and block sequences, but:
- `- key: val` array-of-maps items lose the inline first key
  (`parseArrayLines` strips `- ` then only parses the *following*
  deeper-indented lines — `itemStr` is discarded).
- `#` comments are not stripped (full-line `#` survives as content since it
  has no colon it is silently skipped, but trailing `  # note` corrupts the
  scalar).
- Quoted scalars keep their quotes: `cert: "/a.pem"` yields `"/a.pem"`.
- Flow lists `[a, b]` parse as a literal string, not an array.
- Workaround: a ~60-line `normalizeYAML` preprocessor in the project that
  rewrites `- key:` into a bare `-` + deeper-indented map, expands `[a,b]`
  into block items, unquotes scalars, strips comments — then `Unmarshal`.

**Binary data vs string ops.** `str.Len`/`str.Substr`/`s[i]` are NUL-based —
binary buffers (RSA ciphertext, AES-GCM, WS frames) silently truncate, and
`buf[i]` on `libc.malloc`'d memory bounds-checks against `strlen` and panics
on embedded NULs. Mitigation: `Skink_mem_byte`/`Skink_mem_write_byte` raw
helpers + explicit-length APIs everywhere. A real `[]byte`/`bytes` module or
length-carrying string ops would remove a whole class of port bugs.

**Integer literal width.** Literals >2^31 (e.g. `1000000000000000` for
nanosecond timestamps) silently overflow `int` at compile time — wrong
magnitude branches, no diagnostic. Also `int64 * int` demotes to i32
(`v * 10` in an int64 accumulator truncates; must write `v * int64(10)`).

**std/time wall clock.** `time.NowUnixNano()` is CLOCK_MONOTONIC (seconds
since boot), not epoch — auth/timestamp code needs
`clock_gettime(CLOCK_REALTIME)` or `time.Now().Unix()`.

**std/json precision.** `FromNumber`/`GetInt` round-trip through float64 —
int64 nanosecond timestamps serialize as `-.------`. Workaround: emit large
ints as raw digit strings and parse digits out of the raw JSON.

**std/web test surface.** `types.NewRequest` computes
`bodyLen = str.Len(body)` — binary bodies cannot be constructed in unit
tests; only the live server path populates `BodyBytes`/`BodyLen` correctly.

**std/testing.** `testing.Assert*` failures do not abort the test — a failed
assert that yields nil still dereferences it later and segfaults; tests must
guard manually.

**API naming.** `str.HasPrefix` does not exist (use `s[0] == 35` /
`str.IndexOf == 0`); `fs.WriteFile` does not exist (it's `io.WriteFile` /
`fs.WriteAll`).
