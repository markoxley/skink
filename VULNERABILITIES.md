# Skink Compiler & Toolchain — Security Findings Log

This document records security-relevant findings in the Skink compiler suite
(compiler, runtime C, stdlib, web server, scales package manager, LSP) and
their remediation status. Regression tests for each finding are named; see
`comp_error.md` for the parallel engineering log (#149–#160).

**Date of this audit pass:** 2026-10-26. **Scope:** everything under
`skink/`, `tools/`, and the stdlib.

## Remediation summary

All findings below are FIXED unless marked otherwise. Each fix ships a
regression test per the AGENTS.md policy.

---

## High

### H1 — String element access had no bounds check (OOB read/write)
- **comp_error.md #149.** `s[i]` reads walked past the allocation (info
  leak); `s[i] = c` writes corrupted adjacent heap. The emitter explicitly
  skipped strings ("strlen per index would be quadratic").
- **Fix:** `Skink_str_len` (O(1) for RC strings via the payload-8 size
  field; strlen fallback) bounds reads to the allocation length and writes
  to the string length. ~60 stdlib/tool byte-buffer sites converted to the
  new raw helpers `Skink_mem_write_byte`/`Skink_mem_byte`.
- **Regression:** `std/compiler_safety_test.skink`.

### H2 — Integer div/rem by zero, INT_MIN/-1, out-of-range shifts = LLVM UB
- **comp_error.md #150.** Raw `sdiv`/`srem`/`shl`/`ashr`/`lshr` emitted
  with no guards.
- **Fix:** `emitIntDivRemGuard` + `emitShiftCountGuard` panic with
  Go-style messages.
- **Regression:** `std/compiler_safety_test.skink`.

### H3 — Unbounded checker/emitter recursion on long infix chains
- **comp_error.md #150.** A 3000-term `x+x+x+…` chain overflowed the C
  stack (compiler crash — reproducible, core dump).
- **Fix:** `maxCheckExprDepth`/`maxEmitExprDepth` = 200 (compiled frames
  are large; 512 still crashed).
- **Regression:** `std/compilefail/cf_deep_chain.skink`.

### H4 — scales tar-slip (arbitrary file write via malicious archives)
- **comp_error.md #158.** `tar -xzf` without member validation.
- **Fix:** members listed and validated (`TarMemberUnsafe`) before
  extraction; `--no-same-owner --no-same-permissions`.
- **Regression:** `tools/scales/scales_test.skink` `TestTarMemberUnsafe`.

### H5 — scales `@version` → `git clone --branch` option injection (RCE)
- **comp_error.md #158.** Unvalidated version strings (e.g.
  `--upload-pack=…`) reached git as options.
- **Fix:** `modres.IsSafeVersion` enforced in `ParseSpec` and
  `FetchPackage`; git uses `--branch=<version>` + `--`.
- **Regression:** `scales_test.skink` `TestUnsafeVersionRejected`.

---

## Medium

### M1 — fs.ReadAll returned raw (kind-1) buffers that RC release never frees
- **comp_error.md #153.** Leak in every long-running caller (LSP, scales).
- **Fix:** `Skink_mem_alloc` (RC). **Regression:** `fs_test.skink`
  `TestReadAllRCReclaimed`.

### M2 — Embedded NUL bytes in strings (silent truncation everywhere)
- **comp_error.md #151.** `\0` escape / `Builder.WriteByte(0)` produced
  strings where strlen < allocation.
- **Fix (policy: NULs rejected):** lexer emits T_ILLEGAL for `\0`; raw
  NUL bytes in source are errors; `WriteByte(0)` panics.
- **Regression:** `std/compilefail/cf_nul_string.skink`, `str_test.skink`
  `TestWriteByteNulPanics`.

### M3 — Web server per-request leaks (URLDecode/stripCRLF used raw malloc)
- **comp_error.md #153.** Now RC. **Regression:** web tests.

### M4 — `ResponseWriter.WriteBytes(data, -1)` → memcpy overflow
- **comp_error.md #153.** Now panics. **Regression:** `web/web_test.skink`
  `TestWriteBytesNegativePanics`.

### M5 — Duplicate Content-Length headers accepted (smuggling)
- **comp_error.md #155.** Now rejected with 400.

### M6 — `Skink_os_exec_with_output` unbounded capture + inherited fds
- **comp_error.md #153.** Output capped at 64 MiB; fds CLOEXEC; Linux
  `/proc/self/fd` sweep in the child.

### M7 — `Skink_chan_send` silently dropped values on OOM; select timeout overflow
- **comp_error.md #153.** Now panics; timeout math in int64.

### M8 — compress 2 GiB overflow (`Skink_compress_bound` wrap; Inflater INT_MAX outCap)
- **comp_error.md #154.** Rejected. **Regression:** `compress_test.skink`.

### M9 — LSP Content-Length / header-line DoS
- **comp_error.md #159.** Capped at 64 MiB / 8 KiB.

### M10 — LSP temp check-file symlink attack (predictable name + fopen "w")
- **comp_error.md #159.** `Skink_fs_mktemp_in` (O_EXCL 0600).

### M11 — `os.Getwd` returned NULL on failure
- **comp_error.md #153.** Now returns "".

### M12 — `Skink_slice_len` legacy payload-8 under-read on untracked pointers
- **comp_error.md #153.** Now returns 0 for untracked/kind-1.

### M13 — reflect erased-value scan walked past registered non-strings
- **comp_error.md #153.** Bounded to the registered allocation size.

---

## Low

- **L1** str.Substr/Concat int32 overflow near 2 GiB (#152) — fixed.
- **L2** resolver source-file size cap (256 MiB) (#153) — fixed.
- **L3** import path traversal / unconfined imports + ancestor `std/`
  shadowing (#156) — fixed (`isUnsafeImportPath`, project-boundary walk).
- **L4** driver temp `.ll` TOCTOU (#153) — `Skink_fs_mktemp_fopen`.
- **L5** `Skink_mem_realloc` dropped destructors (#153) — fixed.
- **L6** regex/readdir zero-size buffer guards (#153) — fixed.
- **L7** coverage file O_NOFOLLOW (#153) — fixed.
- **L8** fmt width overflow + unchecked mallocs (#157) — fixed.
- **L13** resolver semver parse overflow (#156) — fixed.
- **L9** Windows SOCKET width — `Skink_socket_wrap`/`Skink_accept_wrap`
  validate SOCKET ≤ INT_MAX; the `(int)sock` narrowing in
  `Skink_net_resolve_and_connect` is bounds-checked — fixed.
- **L10** >2 GiB file ops — `Skink_fs_seek64`/`Skink_fs_tell64` use
  `_fseeki64`/`fseeko` — fixed.
- **L11** `Skink_format_float` fixed-64B truncation — snprintf-probed
  buffer + precision clamp — fixed.
- **L12** binary-file NUL — Skink strings reject embedded NULs (M2), and
  file I/O uses explicit-length `fread`/`fwrite`/`Skink_mem_byte` paths —
  covered.

## ASan gate findings (second pass — all fixed)

- **Per-thread panic pool leak** — `panic_jmps`/`panic_msg_storage`
  __thread allocations leaked ~12800 bytes per detached thread exit
  (28× in web live tests, per-goroutine). `Skink_panic_thread_cleanup`
  is now called at the end of every thread trampoline (`spawn`,
  `web_conn`, `web_tls_conn`, `Skink_ruleset_loop`).
- **strlen overreads on raw buffers** — `p[0]`/`buf[i]`/`ch[0]`
  checked-index reads on uninitialized `libc.malloc` storage called
  `strlen` and scanned past the allocation (libc_test, mcp_test 2-byte
  thread overread, reflect_test global overread). Converted to
  `Skink_mem_byte`; `mcp` body buffer is now terminated before
  `str.Substr`.

## Open / documented

- **#160** deferred-closure writes to captured locals do not propagate
  on panic unwinds — re-verified: NOT a bug. The capture-by-reference
  mechanism propagates correctly; the earlier observation was a
  test-scoping artifact (comp_error.md #160 updated).