# Skink Compiler & Runtime Security Audit (`VULNERABILITIES.md`)

**Audit Date:** 2026-10-22\
**Remediation Status:** ✅ **All 28 vulnerabilities fixed** — see [Remediation Status](#remediation-status) for the per-vulnerability fix and regression-test mapping.\
**Target:** Self-hosted Skink compiler (`skink.skink`, `std/lexer.skink`, `std/parser.skink`, `std/resolver.skink`, `std/checker.skink`, `std/emitter.skink`), C runtime (`std/runtime/*.c`, `std/runtime/*.h`), standard library (`std/*.skink`, `std/web/*.skink`), and build/test wrapper scripts.

---

## Executive Summary

A comprehensive source-level security review of the Skink compiler, LLVM IR code generator, C runtime, and standard library identified **28 distinct vulnerabilities and potential vulnerabilities** across four main areas:

1. **Compiler Driver & Build Tooling** — OS command injection via unquoted shell invocations (`system()`), arbitrary clang flag injection, predictable temporary files, and harness script code injection.
2. **Compiler Frontend & LLVM IR Emitter (`std/resolver.skink`, `std/checker.skink`, `std/emitter.skink`)** — stack buffer overflow and use-after-return in `string(...)` casts, missing bounds checks on from-end indexing (`arr[^i]`), 32-bit integer multiplication overflow in `make([]T, n)` and map/set allocations bypassing slice bounds checks, arbitrary host file inclusion via `embed`, LLVM IR / DWARF metadata injection, and unguarded recursion on cyclic types.
3. **C Runtime (`std/runtime/*.c`, `std/runtime/*.h`)** — `jmp_buf` stack buffer overflow on non-x86_64 platforms (such as Linux `arm64`), panic handler stack desynchronization leading to `longjmp` into dead stack frames, signal/math callback ABI mismatch jumping to non-executable closure structs, heap use-after-free and buffer overflow in `win_regex.h`, `select()` `fd_set` stack overflow in `net_rt.c`, and `mincore()` guard-page/page-boundary crashes in `reflect_rt.c`.
4. **Standard Library (`std/*.skink`, `std/web/*.skink`)** — heap buffer overflows in `strconv.FormatFloat` and `fmt.Sprintf`, integer overflows in `str.Builder` and `str.Repeat`, uninitialized/unterminated buffer reads in `net.Connection.Recv`, out-of-bounds read and integer overflow in `compress.Decompress`, 1-byte heap overflow and `Content-Length` integer overflow in `std/web/server.skink`, and SQL injection sinks in `std/orm.skink`.

| ID          | Severity     | Component                                             | Vulnerability Title                                                                                        |
| ----------- | ------------ | ----------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| **VULN-01** | **Critical** | `skink.skink`                                         | OS Command Injection in Compiler Driver via `system()`                                                     |
| **VULN-02** | **Critical** | `std/emitter.skink`                                   | Stack Buffer Overflow & Use-After-Return in `emitCastToString` (`string(float)` / `string(int)`)           |
| **VULN-03** | **Critical** | `std/emitter.skink`                                   | 32-Bit Integer Overflow in `make([]T, n)` and Map/Set Allocations Enables 4 GB Heap OOB R/W                |
| **VULN-04** | **Critical** | `std/emitter.skink`                                   | Missing Bounds Check on From-End Slice/String Indexing (`arr[^i]`)                                         |
| **VULN-05** | **Critical** | `std/runtime/fs_rt.c`, `tensor_rt.c`                  | Closure-Handle vs. C Function-Pointer ABI Mismatch Executes Data as Code (`os.OnSignal`, `math`)           |
| **VULN-06** | **High**     | `std/emitter.skink`, `panic_rt.c`                     | Fixed 264-Byte `jmp_buf` Stack Allocation Overflows on Linux `arm64` (`sizeof(jmp_buf) == 312`)            |
| **VULN-07** | **High**     | `std/runtime/panic_rt.c`, `emitter.skink`             | Panic Handler Stack Desynchronization After `MAX_PANIC_DEPTH` Causes `longjmp` to Dead Frame               |
| **VULN-08** | **High**     | `std/runtime/win_regex.h`                             | Heap Use-After-Free and Negative-Length `memcpy` Heap Overflow in Windows Regex Engine                     |
| **VULN-09** | **High**     | `std/emitter.skink`                                   | Arbitrary Build-Host File Inclusion via Path Traversal in `embed "..."` Declarations                       |
| **VULN-10** | **High**     | `std/emitter.skink`, `std/parser.skink`               | LLVM IR & DWARF Metadata Injection via Unescaped File Paths, Module Names, and Externs                     |
| **VULN-11** | **High**     | `std/str.skink`, `std/buf.skink`                      | Signed 32-Bit Integer Overflow in `str.Builder.ensure`, `str.Repeat`, and `buf.Buffer`                     |
| **VULN-12** | **High**     | `std/strconv.skink`, `std/fmt.skink`                  | Fixed 64-Byte Heap Buffer Overflow in `strconv.FormatFloat` and `fmt.Sprintf` (`formatFloatPrec`)          |
| **VULN-13** | **High**     | `std/compress.skink`                                  | Heap Out-of-Bounds Read, Integer Overflow, and Decompression Bomb in `compress.Decompress`                 |
| **VULN-14** | **High**     | `std/web/server.skink`                                | 1-Byte Heap Buffer Overflow in `readChunkedBody`, `Content-Length` Integer Overflow, and Request Smuggling |
| **VULN-15** | **High**     | `std/mcp.skink`, `std/api.skink`, `std/net.skink`     | Integer Overflow in `mcp` Body Allocation, Unbounded Client Response Growth, and CRLF Injection            |
| **VULN-16** | **High**     | `std/net.skink`                                       | Uninitialized Heap Memory Disclosure and Missing NUL Terminator in `Connection.Recv`                       |
| **VULN-17** | **Medium**   | `std/runtime/reflect_rt.c`                            | `mincore()` Type-Erasure Heuristic Crashes on `PROT_NONE` Guard Pages and Page Boundaries                  |
| **VULN-18** | **Medium**   | `std/runtime/net_rt.c`                                | Unchecked `FD_SET(fd, &writefds)` Stack Buffer Overflow When `fd >= FD_SETSIZE` (1024)                     |
| **VULN-19** | **Medium**   | `std/checker.skink`, `std/emitter.skink`              | Unbounded Recursion / Compiler Stack Overflow on Cyclic Value Structs and Type Aliases                     |
| **VULN-20** | **Medium**   | `std/orm.skink`                                       | SQL Injection via Unquoted Table/Column Metadata and Raw `Where` Clause Concatenation                      |
| **VULN-21** | **Medium**   | `std/runtime/tensor_rt.c`, `std/tensor.skink`         | Unchecked Slice Bounds, Integer Overflow in Tensor Dimensions, and Hardware `SIGFPE` on `stride == 0`      |
| **VULN-22** | **Medium**   | `std/encrypt.skink`, `std/crypto.skink`               | Key Truncation on NUL Bytes (`strlen`), Unwiped Plaintext on GCM Tag Failure, and Heap Leaks               |
| **VULN-23** | **Medium**   | `skink_test.sh`, `skink_bench.sh`, `skink_wrapper.sh` | Predictable `/tmp` & Working-Directory File Collisions and Test Harness Code Injection                     |
| **VULN-24** | **Medium**   | `std/runtime/fs_rt.c`                                 | Thread-Unsafe `stdout` Redirection Race in Windows `Skink_os_exec_with_output`                             |
| **VULN-25** | **Low**      | `std/str.skink`                                       | 64-Bit Pointer Truncation to 32-Bit `int` in `str.Contains` and `str.IndexOf`                              |
| **VULN-26** | **Low**      | `std/web/static.skink`                                | Root Escape When `rootDir` Is Empty and Request Path Begins with `//`                                      |
| **VULN-27** | **Low**      | `std/rand.skink`, `std/tensor.skink`                  | Predictable PRNG Seeding (`srand(time(NULL))`) and Unsynchronized Global State                             |
| **VULN-28** | **Low**      | `install.sh`                                          | Soft-Fail on Missing SHA-256 Checksum File During Release Installation                                     |

---

## Detailed Findings

### 1. Compiler Driver & Build Tooling

---

#### VULN-01: OS Command Injection in Compiler Driver via `system()`

- **Severity:** Critical
- **Location:** `skink.skink:96-97`, `skink.skink:108-109`, `skink.skink:224-225`, `skink.skink:294-300`
- **Description:** The compiler driver constructs shell command strings via `str.Concat` and passes them directly to C's `system()` (which invokes `/bin/sh -c` on POSIX and `cmd.exe /c` on Windows) without quoting or escaping any arguments:

  ```skink
  // Test harness (-t):
  cmd := str.Concat("SKINK=", str.Concat(selfBin, str.Concat(" SKINK_HOME=", str.Concat(skinkHome, str.Concat(" ", str.Concat(script, str.Concat(" ", argv[2])))))))
  testRc := system(cmd)

  // Clang invocation:
  cmd := str.Concat(clangFlags, str.Concat(" -o ", str.Concat(outName, str.Concat(" ", str.Concat(llPath, str.Concat(" ", str.Concat(rt, libs)))))))
  rc := system(cmd)
  ```

  None of `fileName`, `outName` (`-o`), `llPath`, `skinkHome` (`SKINK_HOME`), `selfBin` (`argv[0]`), `argv[2]`, or `extraFlags` (`SKINK_CLANG_FLAGS`) are shell-escaped. Additionally, `llPath := str.Concat(outName, ".ll")` writes the temporary `.ll` file into the current working directory (or wherever `-o` points) and `remove(llPath)` deletes it after `system(cmd)` returns; if `-o` is not specified, `outName` is derived from `path.Base(fileName)` and written into the current working directory, clobbering any existing `<basename>.ll` file.

- **Impact:** If `skink` is invoked by an IDE, LSP, build service, CI runner, or web playground on a file path or `-o` path containing shell metacharacters (`;`, `|`, `&`, `` ` ``, `$()`, spaces), arbitrary shell commands execute with the privileges of the compiler process.
- **Remediation:** Replace `system(cmd)` in `skink.skink` with direct process execution via `os.Exec` (`Skink_os_exec` in `std/runtime/fs_rt.c`, which uses `execvp` / `_spawnvp` without invoking a shell). Generate temporary `.ll` files using `mkstemps` in `/tmp` rather than `<outName>.ll` in the working directory.

---

#### VULN-23: Predictable `/tmp` & Working-Directory File Collisions and Test Harness Code Injection

- **Severity:** Medium
- **Location:** `skink_wrapper.sh:69-71`, `run_tests.sh:32-34`, `skink_test.sh:34-61`, `skink_bench.sh:34-55`
- **Description:**
  1. `skink_wrapper.sh` writes to predictable paths `/tmp/skink_wrapper_$$.ll` and `/tmp/skink_wrapper_err_$$`, and `run_tests.sh` runs `rm -rf /tmp/skink_test_results && mkdir -p /tmp/skink_test_results`. On multi-user systems, predictable paths in `/tmp` are vulnerable to symlink/FIFO attacks or cross-user DoS if `fs.protected_symlinks` is disabled.
  2. `skink_test.sh` and `skink_bench.sh` derive `base=$(basename "$f" .skink)` and interpolate `$base` unescaped into generated Skink source code (`echo "import \"$base\""` and `echo " $base.$fn()"`), while writing to fixed filenames `$dir/__skink_test_main.skink` and `$dir/__skink_test_bin` that clobber concurrent test runs in the same directory.
- **Impact:** Local symlink/file-clobbering attacks on shared machines, race conditions during parallel test execution, and arbitrary Skink code injection if a test filename contains quotes or Skink syntax.
- **Remediation:** Use `mktemp -d` for all temporary directories and files, and validate that `$base` is a valid Skink identifier (`^[A-Za-z_][A-Za-z0-9_]*$`) before interpolating it into generated test runner code.

---

#### VULN-28: Soft-Fail on Missing SHA-256 Checksum File During Release Installation

- **Severity:** Low
- **Location:** `install.sh:307-315`
- **Description:** In `install.sh`, if `curl -fsSL -o "${ARCHIVE_PATH}.sha256" "${DOWNLOAD_URL}.sha256"` fails, or if `sha256sum` is not installed (e.g., default macOS where `shasum -a 256` is present instead of `sha256sum`), the script logs a warning and proceeds to extract and install the unverified binary archive.
- **Impact:** On macOS (where `sha256sum` is often absent by default) or if an attacker blocks/spoofs `.sha256` requests, checksum verification is silently bypassed.
- **Remediation:** Support `shasum -a 256` as a fallback on macOS, and require checksum verification by default unless an explicit `--allow-unverified` flag is passed.

---

### 2. Compiler Frontend & LLVM IR Emitter (`std/resolver.skink`, `std/checker.skink`, `std/emitter.skink`)

---

#### VULN-02: Stack Buffer Overflow & Use-After-Return in `emitCastToString` (`string(float)` / `string(int)`)

- **Severity:** Critical
- **Location:** `std/emitter.skink:8106-8153`, `std/emitter.skink:7389-7420`
- **Description:** There are two severe memory-safety bugs in the compiler's numeric-to-string conversion codegen:
  1. **Stack Buffer Overflow on `string(float)` (`emitCastToString`, lines 8127–8136):**

     ```skink
     if str.Equal(srcType, "double") {
         fmtReg := emitStringLiteral(cg, "%f")
         bufReg := nextReg(cg)
         emitfmt(cg, "  %s = alloca [32 x i8]\n", []string{bufReg})
         ptrReg := nextReg(cg)
         emitfmt(cg, "  %s = getelementptr inbounds [32 x i8], [32 x i8]* %s, i64 0, i64 0\n", []string{ptrReg, bufReg})
         retReg := nextReg(cg)
         emitfmt(cg, "  %s = call i32 (i8*, ...) @sprintf(i8* %s, i8* %s, double %s)\n", []string{retReg, ptrReg, fmtReg, operand})
         return ptrReg
     }
     ```

     The emitter allocates a **32-byte stack buffer** (`alloca [32 x i8]`) and calls unbounded `@sprintf(..., "%f", double)`. An IEEE-754 `double` up to `1.797693e+308` formatted with `"%f"` produces **up to 317 bytes** (`309` integer digits + `.` + `6` fractional digits + NUL), overflowing the 32-byte stack buffer by **up to 285 bytes** and smashing the stack frame / return address.

  2. **Use-After-Return (Dangling Stack Pointer) & Loop Stack Exhaustion (`emitCastToString` lines 8117–8136, `emitIntToString` / `emitInt64ToString` / `emitFloatToString` lines 7389–7420):**
     - Both `emitCastToString` and `emitIntToString`/`emitInt64ToString`/`emitFloatToString` format into a stack `alloca` (`[32 x i8]` or `[64 x i8]`) and return the raw stack pointer (`ptrReg`) as the `i8*` Skink `string` value.
     - If a function returns `string(x)` (e.g., `return string(42)`), stores `string(x)` into a heap struct/slice/map/channel/global, or sends it to a spawned thread, the pointer references **dead stack memory** once the function returns (Use-After-Return).
     - Furthermore, `emitCastToString` emits `alloca [32 x i8]` inline in the current basic block rather than via `emitAlloca` (the entry-block alloca buffer). Calling `string(i)` inside a loop grows the hardware stack pointer on every iteration until the thread crashes with a stack overflow.
- **Impact:** Calling `string(f)` on any float with magnitude $\ge 10^{25}$ immediately corrupts the stack frame (enabling control-flow hijacking). Returning or storing `string(n)` from a function causes use-after-return memory corruption.
- **Remediation:** Allocate the destination buffer on the RC heap via `@Skink_rc_alloc` (or call `Skink_format_float` / an RC-allocating runtime helper using `snprintf`), and never use unbounded `@sprintf` or return a stack `alloca` as a Skink `string`.

---

#### VULN-03: 32-Bit Integer Overflow in `make([]T, n)` and Map/Set Allocations Enables 4 GB Heap OOB R/W

- **Severity:** Critical
- **Location:** `std/emitter.skink:11501-11539`, `std/emitter.skink:3131-3146`, `std/emitter.skink:3279-3290`, `std/emitter.skink:4213-4246`
- **Description:** In `emitMakeExpr` (`make([]T, n)`), the byte allocation size is computed in **32-bit `i32` arithmetic** before zero-extending to `i64`:

  ```skink
  elemSize := emitElemSize(cg, elemLL)
  bytesReg := nextReg(cg)
  emitfmt(cg, "  %s = mul i32 %s, %s\n", []string{bytesReg, nReg, elemSize})
  bytes64 := nextReg(cg)
  emitfmt(cg, "  %s = zext i32 %s to i64\n", []string{bytes64, bytesReg})
  ...
  emitfmt(cg, "  %s = call i8* @Skink_rc_alloc(i64 %s)\n", []string{rawReg, bytes64})
  nReg64 := nextReg(cg)
  emitfmt(cg, "  %s = zext i32 %s to i64\n", []string{nReg64, nReg})
  ...
  emitfmt(cg, "  %s = call i8* @Skink_slice_hdr(i8* %s, i64 %s, i64 %s, i8* %s)\n",
      []string{hdrReg, rawReg, nReg64, nReg64, rawReg})
  ```

  1. **32-Bit Multiplication Overflow (`n * sizeof(T)`):** If `T` is an 8-byte type (`int64`, `float`, `string`, `*T`) and `n = 536870912` (`0x20000000`), then `mul i32 536870912, 8` wraps to `0`. `Skink_rc_alloc(0)` allocates a **1-byte** heap block, while `Skink_slice_hdr` records `len = 536870912` and `cap = 536870912`. Every subsequent `s[i]` access for `0 <= i < 536870912` passes `Skink_bounds_check`, giving safe Skink code an unguarded **4 GB heap out-of-bounds read/write primitive**.
  2. **Negative `n` (`make([]T, -1)`):** There is no runtime check that `n >= 0`. A negative `i32` is zero-extended via `zext i32 to i64` into a large positive `i64` length (`~4.29 billion`), and if `n * elemSize` wraps in `i32` to a small value, a tiny buffer is paired with a ~4 billion element length.
  3. **Unchecked `NULL` Return from `Skink_rc_alloc`:** If `Skink_rc_alloc` returns `NULL` (e.g., on OOM), lines 11529–11532 immediately write `nReg64` to `rawReg - 8` (`0xFFFFFFFFFFFFFFF8`).
  4. **Map & Set Resizing:** The same `mul i32 %newCap, %elemSize` followed by `zext i32 to i64` pattern appears in `emitMapLiteral` (lines 3132, 3140), `emitSetLiteral` (line 3280), and `emitMapIndexAssignment` (lines 4214, 4225, 4233).
- **Impact:** Complete bypass of Skink's slice bounds checking, leading to arbitrary heap memory corruption from safe Skink code whenever a slice length is derived from untrusted input.
- **Remediation:** Emit a runtime check in `emitMakeExpr` that panics if `nReg < 0`. Extend `nReg` and `elemSize` to `i64` **before** multiplying (or pass `(count, elemSize)` to a checked runtime allocator like `Skink_mem_calloc` that uses `checked_byte_count` and panics on overflow or OOM).

---

#### VULN-04: Missing Bounds Check on From-End Slice/String Indexing (`arr[^i]`)

- **Severity:** Critical
- **Location:** `std/emitter.skink:10734-10777`
- **Description:** While normal indexing `arr[i]` emits a call to `emitIndexBoundsCheck` (`Skink_bounds_check(i, i + 1, len)`), `emitFromEndExpr` (`arr[^i]`) computes `realIdx = len(arr) - i` and indexes directly into the backing pointer **without any bounds check** and without coercing `idxExpr` to `i32` via `emitIndexAsI32`:
  ```skink
  fn emitFromEndExpr(cg: *CodeGen, e: ast.Expr) -> string {
      target := emitExpression(cg, e.indexTarget[0])
      leftType := exprLLType(cg, e.indexTarget[0])
      lenReg := emitLenCallRaw(cg, target, leftType, e.indexTarget[0])
      idxExpr := emitExpression(cg, e.indexExpr[0])
      realIdx := nextReg(cg)
      emitfmt(cg, "  %s = sub i32 %s, %s\n", []string{realIdx, lenReg, idxExpr})
      // Directly GEPs and loads base[realIdx] with no bounds check!
  ```

- **Impact:**
  - `arr[^0]` reads `arr[len(arr)]` (one element past the end of the allocation).
  - `arr[^(-k)]` reads `arr[len(arr) + k]` (arbitrary heap out-of-bounds read past the buffer).
  - `arr[^k]` for `k > len(arr)` reads `arr[len(arr) - k]` (arbitrary out-of-bounds read before the buffer, exposing the 24-byte `RCHeader` or adjacent heap chunks).
  - On a `nil` or empty slice, `arr[^1]` dereferences `null - sizeof(T)`.
- **Remediation:** Coerce `idxExpr` via `emitIndexAsI32`, and call `Skink_bounds_check(realIdx, realIdx + 1, lenReg)` in `emitFromEndExpr` before performing the `getelementptr` and `load`.

---

#### VULN-05: Closure-Handle vs. C Function-Pointer ABI Mismatch Executes Data as Code (`os.OnSignal`, `math`)

- **Severity:** Critical
- **Location:** `std/runtime/fs_rt.c:486-502`, `std/os.skink:48,212`, `std/runtime/tensor_rt.c:502-553`
- **Description:** In the self-hosted compiler (`std/emitter.skink:2065,5721-5875`), all function-typed values (`fn(...) -> ...`) are lowered to `i8*` **closure handles** — pointers to a struct `{ i8* code_ptr, i8* cap1, ... }` in `.rodata` or on the RC heap, where the underlying function expects the handle itself as a hidden first parameter `i8* %env` (`retType (i8*, params...)`). However:
  1. `Skink_signal_handle(int sig, void (*fn)(void))` in `std/runtime/fs_rt.c:486-502` stores `fn` directly in `skink_signal_handlers[sig]` and calls `skink_signal_handlers[sig]()` when the signal arrives — **jumping to the closure struct address in `.rodata`/heap as if it were machine instructions**, rather than loading `code = *(void **)fn` and calling `((void (*)(void *))code)(fn)`. (`std/signal_test.skink` only tests installing the handler and never raises the signal.)
  2. `Skink_math_diff`, `Skink_math_integrate`, and `Skink_tensor_gradient` in `std/runtime/tensor_rt.c:502-553` similarly treat their callback parameter as a raw C function pointer `double (*f)(double)` instead of a Skink closure handle `double (*code)(void *env, double)`.
- **Impact:** Delivering any handled POSIX signal to a process that called `os.OnSignal` immediately faults (`SIGSEGV` executing non-executable data memory) or executes arbitrary data bytes if the heap/data page is executable.
- **Remediation:** In `skink_signal_tramp` (`std/runtime/fs_rt.c`) and `Skink_math_diff` / `Skink_math_integrate` / `Skink_tensor_gradient` (`std/runtime/tensor_rt.c`), dereference the closure handle to extract the code pointer and pass the handle as the first (`env`) argument, matching `web_conn_trampoline` in `std/runtime/conc_rt.c:485-491`:
  ```c
  void *handle = skink_signal_handlers[sig];
  void (*code)(void *) = *(void (**)(void *))handle;
  code(handle);
  ```

---

### 3. C Runtime (`std/runtime/*.c`, `std/runtime/*.h`)

---

#### VULN-06: Fixed 264-Byte `jmp_buf` Stack Allocation Overflows on Linux `arm64` (`sizeof(jmp_buf) == 312`)

- **Severity:** High
- **Location:** `std/emitter.skink:13049-13063`, `std/runtime/panic_rt.c:58-66`
- **Description:** For every function containing a `defer` statement, `emitPanicPrologue` allocates a hardcoded 264-byte stack buffer for `setjmp`:

  ```skink
  jbAlloca := emitAlloca(cg, "[264 x i8]")
  ...
  emitfmt(cg, "  %s = call i32 @setjmp(i8* %s)\n", []string{sjReg, jbPtr})
  ...
  emitfmt(cg, "  call i32 @Skink_panic_push_handler(i8* %s)\n", []string{jbPtr})
  ```

  And `Skink_panic_push_handler` executes `memcpy(panic_jmps[panic_depth], jmp_buf_ptr, sizeof(jmp_buf))`. While `sizeof(jmp_buf)` is 200 bytes on glibc `x86_64`, on **glibc `aarch64` (`arm64` Linux)** — a officially supported target in `install.sh`, `release.sh`, and `build_arm64_bootstrap.sh` — `sizeof(jmp_buf)` is **312 bytes** (and on `riscv64` it is 344 bytes).

- **Impact:** On Linux `arm64`, every function that uses `defer` suffers a **48-byte stack buffer overflow** inside `setjmp(jbPtr)` and a 48-byte out-of-bounds stack read in `Skink_panic_push_handler`, corrupting adjacent local variables or saved registers on the stack.
- **Remediation:** Increase the `jbAlloca` size in `emitPanicPrologue` to at least `[512 x i8]` (aligned to 16 bytes), or allocate the `jmp_buf` slot directly inside `panic_rt.c` (`void *Skink_panic_alloc_slot(void)` returning `&panic_jmps[panic_depth++]`) so the emitter never hardcodes `sizeof(jmp_buf)` and `panic_rt.c` never `memcpy`s a `jmp_buf` (which is also undefined behavior under ISO C11 §7.13.2.1).

---

#### VULN-07: Panic Handler Stack Desynchronization After `MAX_PANIC_DEPTH` Causes `longjmp` to Dead Frame

- **Severity:** High
- **Location:** `std/runtime/panic_rt.c:40,58-73`, `std/emitter.skink:5914-5916,13062`
- **Description:**`panic_rt.c` maintains a fixed-size thread-local stack `panic_jmps[MAX_PANIC_DEPTH]` (`MAX_PANIC_DEPTH = 64`):

  ```c
  int Skink_panic_push_handler(void *jmp_buf_ptr) {
    if (panic_depth >= MAX_PANIC_DEPTH) {
      return 0;
    }
    memcpy(panic_jmps[panic_depth], jmp_buf_ptr, sizeof(jmp_buf));
    panic_depth++;
    return 1;
  }
  void Skink_panic_pop_handler() {
    if (panic_depth > 0) {
      panic_depth--;
    }
  }
  ```

  `emitPanicPrologue` ignores the return value of `Skink_panic_push_handler`, and `emitRet` unconditionally calls `Skink_panic_pop_handler()`. If a call chain of functions containing `defer` exceeds depth 64 (e.g., depth 70):

  1. Frames 64..69 fail to push (`panic_depth` stays capped at 64).
  2. When Frames 69..64 return normally, each calls `Skink_panic_pop_handler()`, decrementing `panic_depth` down to `58` even though Frames 0..63 are still active.
  3. If Frame 58 now calls a helper with `defer` that returns normally or panics, `panic_jmps[58]` is overwritten with the helper's `jmp_buf`; or if `Skink_panic_push_handler` is modified without fixing pop pairing, `panic_depth` points to stale entries.
- **Impact:** Unwinding a panic after deep recursion jumps to the wrong handler frame or a frame whose stack state no longer matches `panic_depth`, corrupting stack execution state and bypassing intermediate `defer` cleanups.
- **Remediation:** Either dynamically grow the thread-local `panic_jmps` array via `realloc` (or use a linked list of stack-allocated handler nodes `{ jmp_buf jb; struct PanicNode *prev; }` linked in `emitPanicPrologue`), eliminating the fixed `MAX_PANIC_DEPTH` limit and `memcpy(jmp_buf)` altogether.

---

#### VULN-08: Heap Use-After-Free and Negative-Length `memcpy` Heap Overflow in Windows Regex Engine

- **Severity:** High
- **Location:** `std/runtime/win_regex.h:350-408`
- **Description:** The Windows regex engine (`std/runtime/win_regex.h`) contains two severe memory corruption bugs in `wr_match` when handling parenthesized groups `(...)`:
  1. **Heap Use-After-Free during Group Backtracking (lines 389–406):**

     ```c
     free(group);
     if (count < qmin) return -1;
     // Backtrack
     while (count >= qmin) {
         int rn = wr_match(after, t, newline, icase);
         if (rn >= 0) return (int)(t - text) + rn;
         if (count == 0) break;
         count--;
         // Backtrack group: re-match group count times from text
         t = text;
         for (int i = 0; i < count; i++) {
             int gn = wr_match(group, t, newline, icase); // <-- USE-AFTER-FREE of `group`!
     ```

     `group` is freed unconditionally at line 389 and then passed to `wr_match(group, ...)` inside the backtracking loop at line 402 whenever a quantified group such as `(a+)+b` or `(ab)*a` backtracks.

  2. **Signed Underflow -> `memcpy(..., -1)` Heap Buffer Overflow on Unclosed `(` (lines 350–356):**`regcomp` does not validate balanced parentheses. If a pattern ends with an unclosed `"("`, `wr_find_close_paren(pat + 1)` returns `pat + 1` (pointing to `'\0'`). Then:
     ```c
     int group_len = (close - 1) - (pat + 1); // evaluates to -1!
     char *group = (char *)malloc(group_len + 1); // malloc(0)
     memcpy(group, pat + 1, group_len); // memcpy(group, pat + 1, (size_t)-1) -> SIZE_MAX!
     ```

  3. **Exponential Backtracking (ReDoS):** `wr_match` is a naive recursive backtracking matcher with no step counter or recursion depth limit, allowing crafted inputs/patterns to hang or overflow the thread stack.
- **Impact:** On Windows builds, matching a regex with a quantified group that backtracks reads freed heap memory (UAF), and an unclosed `(` in a pattern triggers an immediate `memcpy` with length `SIZE_MAX` (heap corruption / crash).
- **Remediation:** Move `free(group)` to after the `while (count >= qmin)` backtracking loop (and on its early return paths), validate in `regcomp` that parentheses and brackets are balanced (or check `*close == ')'` before subtracting 1), and bound recursion depth / match steps in `wr_match`.

---

#### VULN-09: Arbitrary Build-Host File Inclusion via Path Traversal in `embed "..."` Declarations

- **Severity:** High
- **Location:** `std/emitter.skink:15646-15653`, `std/emitter.skink:16086-16148`
- **Description:** The `embed "dir"` declaration recursively walks the specified directory at compile time (`walkEmbedDir`) and bakes every file's contents into the compiled binary as `@__skink_embed_data_N` globals:

  ```skink
  srcDir := dirNameCG(decls[i].filePath)
  spec := embedSpec{srcRoot: joinPathCG(srcDir, d.embedPath), virtRoot: cleanEmbedRoot(d.embedPath)}
  ```

  Neither the parser, checker, nor emitter validates that `d.embedPath` stays within the source module's directory tree. An `embed "../../../etc"` or `embed ".."` declaration in an untrusted source file or third-party module (`mod/...`) traverses outside the project root and embeds arbitrary files from the build machine into the output binary. Furthermore, `walkEmbedDir` does not track visited directory inodes, so a symlink loop (`ln -s . loop`) inside an embedded directory causes infinite recursion and compiler crash.

- **Impact:** Compiling untrusted Skink code or third-party packages can exfiltrate sensitive files from the build host into the compiled binary or hang/crash the compiler via symlink loops.
- **Remediation:** Reject `embed` paths that are absolute or contain `..` segments after normalization, resolve symlinks with `realpath` to verify that every embedded file resides beneath the declaring module's root directory, and bound directory recursion depth in `walkEmbedDir`.

---

#### VULN-10: LLVM IR & DWARF Metadata Injection via Unescaped File Paths, Module Names, and Externs

- **Severity:** High
- **Location:** `std/emitter.skink:483-503`, `std/emitter.skink:14686-14703`, `std/emitter.skink:15089-15137`, `std/parser.skink:475-487`
- **Description:**
  1. **DWARF Metadata String Injection (`getOrCreateDIFile`, lines 498–500):** When compiling with `-g`, `filePath` (`filename` and `directory`) is concatenated directly into `!DIFile(filename: "...", directory: "...")` without escaping `"` or `\`:

     ```skink
     def := str.Concat(str.Concat(str.Concat(str.Concat(
         id, " = !DIFile(filename: \""), filename), "\", directory: \""),
         str.Concat(directory, "\")"))
     ```

     A source file or directory path containing `"` or `\` produces malformed LLVM IR or injects arbitrary LLVM metadata/declarations into the `.ll` stream.

  2. **Unquoted LLVM Symbol Names (`@<fnName>`, `@<varName>`):** In `parseModuleDecl` (`std/parser.skink:480`), `qualifyDeclaration`, `emitFnDecl`, and `emitExternFnDecl`, identifiers and module-qualified names are emitted as bare `@name` symbols without LLVM identifier quoting (`@"..."`) if unexpected characters appear, and `extern fn` declarations allow arbitrary C/LLVM symbols to be redeclared with mismatched signatures in the same module.
- **Impact:** Arbitrary LLVM IR injection when compiling with `-g` on crafted file/directory names, and potential LLVM IR verification failures or ABI type confusion on mismatched `extern fn` declarations.
- **Remediation:** Escape `"` (`\"`) and `\` (`\\`) in all DWARF metadata strings (`!DIFile`, `!DISubprogram`, `!DILocalVariable`, `!DICompositeType`), and validate/quote all emitted LLVM global and function identifiers.

---

#### VULN-17: `mincore()` Type-Erasure Heuristic Crashes on `PROT_NONE` Guard Pages and Page Boundaries

- **Severity:** Medium
- **Location:** `std/runtime/reflect_rt.c:48-71`, `std/runtime/reflect_rt.c:180-211`
- **Description:** Because Skink erases generic type parameters `T` to `i8*`, `reflect_hash` and `reflect_equals` (used by `map[K]V`, `set<T>`, and `std/hashmap`) use `skink_is_mapped(p)` (`mincore(page, ps, &vec) == 0` on POSIX, `VirtualQuery` on Windows) to guess whether an erased `i8*` value is a `char *` string pointer or an integer cast via `inttoptr`:

  ```c
  int r = (mincore(page, (size_t)ps, &vec) == 0) ? 1 : 0;
  ```

  This heuristic has three flaws:

  1. **`PROT_NONE` Guard Pages:** `mincore()` reports `0` (mapped) for virtual pages that exist in the process VMA table with `PROT_NONE` permissions (such as `pthread` thread-stack guard pages and allocator guard pages). If an integer key happens to equal an address inside a `PROT_NONE` page, `skink_is_mapped` returns `1`, and `Skink_str_hash` / `skink_streq` immediately faults with `SIGSEGV` when dereferencing `(const char *)p`.
  2. **Cross-Page Over-Read:** `skink_is_mapped` only checks the single page containing `p`. If an integer key lands near the end of a mapped page (e.g., a binary code/data/heap page) that does not contain a `0x00` byte before the page boundary, `Skink_str_hash` walks into the next page — which may be unmapped or `PROT_NONE` — and crashes with `SIGSEGV`.
  3. **Mutable Memory Hash Instability:** If an integer key in a generic `HashMap<int, V>` or `set<int>` happens to match a writable stack or heap address, `reflect_hash` and `reflect_equals` hash and compare the **bytes stored at that address** rather than the integer value itself. When those stack/heap bytes change, the integer key's hash and equality silently change, corrupting container lookups.
- **Impact:** Deterministic `SIGSEGV` or silent hash-table corruption in generic containers (`HashMap<int, V>`, `set<T>`) when integer values collide with mapped virtual addresses or guard pages.
- **Remediation:** Instead of probing arbitrary virtual memory with `mincore()`, check whether the pointer is a registered RC string/allocation in `rc_table` (or falls within the binary's `.rodata`/`.data` string literal section bounds `__start_rodata`..`__stop_rodata`), or pass an explicit type descriptor / tag bit into generic containers.

---

#### VULN-18: Unchecked `FD_SET(fd, &writefds)` Stack Buffer Overflow When `fd >= FD_SETSIZE` (1024)

- **Severity:** Medium
- **Location:** `std/runtime/net_rt.c:315-321`
- **Description:** In `Skink_net_resolve_and_connect` (POSIX implementation), when `timeoutMs > 0`:

  ```c
  fd_set writefds;
  FD_ZERO(&writefds);
  FD_SET(fd, &writefds);
  ...
  rc = select(fd + 1, NULL, &writefds, NULL, &tv);
  ```

  On Linux and macOS, `fd_set` is a fixed 128-byte bitmask on the stack (`FD_SETSIZE == 1024`). If a server or client process has $\ge 1024$ open file descriptors and calls `ConnectWithTimeout` with `fd >= 1024`, `FD_SET(fd, &writefds)` writes a bit at byte offset `fd / 8` past `writefds`, corrupting the stack frame.

- **Impact:** Stack buffer overflow in network applications with $>1024$ open sockets/files using connection timeouts.
- **Remediation:** Replace `select()` with `poll()` (which takes a `struct pollfd` and has no `FD_SETSIZE` limit), or reject `fd >= FD_SETSIZE` before calling `FD_SET`.

---

#### VULN-19: Unbounded Recursion / Compiler Stack Overflow on Cyclic Value Structs and Type Aliases

- **Severity:** Medium
- **Location:** `std/checker.skink:2494-2526`, `std/emitter.skink:5131-5143`, `std/emitter.skink:15979-15995`, `std/emitter.skink:1949-2009`
- **Description:**
  1. **Cyclic Value Structs:** `checkStructDecl` in `std/checker.skink` does not check for illegal recursive value structs (`struct S { s: S }` or `struct A { b: B }; struct B { a: A }`). Meanwhile, `std/emitter.skink:15978` assumes `"Cycles are impossible: a struct can only contain itself behind a pointer"` and recurses without a visited set in `structLayoutHasHeapFields`, `needsArrayDtor`, `emitReleaseStructHeapFields`, and `emitRetainStructHeapFields`. Because `emitStructDecl` (`std/emitter.skink:15574`) recomputes `computeStructLayout` after all struct names are registered in `cg.structLayouts`, a cyclic value struct resolves its field type to `%struct.S` (by value) and triggers infinite recursion in `structLayoutHasHeapFields` / `emitReleaseStructHeapFields`, crashing the compiler with a C stack overflow.
  2. **Cyclic Type Aliases in Emitter (`resolveAliasCG`):** While `checker.resolveAlias` (`std/checker.skink:1929`) tracks a `visited` map, `resolveAliasCG` (`std/emitter.skink:5131-5143`) has no cycle detection and recurses infinitely if `cg.aliases` contains a cycle.
- **Impact:** Denial of service (compiler crash via stack overflow) when type-checking or compiling malformed Skink source files (e.g., in `skink-lsp` or CI).
- **Remediation:** Detect and report cyclic value structs and cyclic type aliases as compile errors in `std/checker.skink`, and add a `visited` guard to `resolveAliasCG` and `structLayoutHasHeapFields` in `std/emitter.skink`.

---

### 4. Standard Library (`std/*.skink`, `std/web/*.skink`)

---

#### VULN-11: Signed 32-Bit Integer Overflow in `str.Builder.ensure`, `str.Repeat`, and `buf.Buffer`

- **Severity:** High
- **Location:** `std/str.skink:112-124`, `std/str.skink:343-361`, `std/buf.skink:56-60,88-93,178-191`
- **Description:**
  1. **`str.Builder.ensure` (`std/str.skink:112-124`):**

     ```skink
     fn ensure(self: *Builder, extra: int) {
         need := self.len + extra + 1
         if need <= self.cap {
             return
         }
         newCap := self.cap * 2
         if newCap < need {
             newCap = need
         }
         self.buf = libc.realloc(self.buf, newCap)
         self.cap = newCap
     }
     ```

     - If `self.len + extra + 1` overflows signed 32-bit `int` (`> 2,147,483,647`), `need` wraps to a negative number (`need <= self.cap` is `true`), `ensure` returns **without reallocating**, and `Write` immediately executes `libc.memcpy(string(int64(self.buf) + int64(self.len)), s, n)`, causing a massive heap buffer overflow.
     - If `self.cap > 1,073,741,823`, `self.cap * 2` overflows signed 32-bit `int` to a negative number; if `need` is smaller than `INT32_MAX`, `newCap < need` is true so `newCap = need`, but if `libc.realloc` fails and returns `NULL` (`0`), `self.buf` becomes `0` and `Write` dereferences `0 + self.len`.
  2. **`str.Repeat` (`std/str.skink:348-352`):**

     ```skink
     if n > 2147483647 / count {
         return Concat("", "")
     }
     total := n * count
     result := managedAlloc(total + 1)
     ```

     When `n * count == 2147483647` (`INT32_MAX`, e.g., `n = 2147483647, count = 1`), the check `n > 2147483647 / count` does not trigger, `total` is `2147483647`, and `total + 1` overflows signed 32-bit `int` to `-2147483648` (`INT32_MIN`). `managedAlloc(-2147483648)` returns `NULL`, and the subsequent loop writes `2147483647` bytes to `NULL`.

  3. **`buf.Buffer.WriteInt` (`std/buf.skink:56-60`):** When `n == -2147483648` (`INT32_MIN`), `n = -n` overflows back to `-2147483648` (`< 0`), skipping the digit loop and writing only `"-"` (which also affects `fmt.Itoa(-2147483648)`).
- **Impact:** Heap buffer overflow or NULL-pointer write in any program (including the compiler and LSP) that accumulates large strings in `str.Builder` or calls `str.Repeat`.
- **Remediation:** Guard against `extra < 0`, `self.len > 2147483646 - extra`, and `self.cap > 1073741823` in `str.Builder.ensure` and `buf.Buffer.ensureCap`; check `n > 2147483646 / count` in `str.Repeat`; and widen `n` to `int64` before negating in `buf.Buffer.WriteInt`.

---

#### VULN-12: Fixed 64-Byte Heap Buffer Overflow in `strconv.FormatFloat` and `fmt.Sprintf` (`formatFloatPrec`)

- **Severity:** High
- **Location:** `std/strconv.skink:322-381`, `std/fmt.skink:365-395`
- **Description:** Both `strconv.FormatFloat(f, precision)` and `fmt.formatFloatPrec(f, precision)` (invoked by `fmt.Sprintf("%.<N>f", ...)`) allocate a fixed **64-byte heap buffer** (`buf := libc.malloc(64)`) and write `precision` fractional digits (plus integer digits, sign, decimal point, and NUL terminator) into `buf` with no bounds check on `precision`:

  ```skink
  // std/fmt.skink:365-394
  fn formatFloatPrec(f: float, precision: int) -> string {
      buf := libc.malloc(64)
      ...
      if precision > 0 {
          buf[pos] = 46 // '.'
          pos = pos + 1
          frac := f - float(intPart)
          for i := 0; i < precision; i = i + 1 {
              ...
              buf[pos] = 48 + digit
              pos = pos + 1
          }
      }
      buf[pos] = 0
      return buf
  }
  ```

  Passing `precision >= 62` (e.g., `strconv.FormatFloat(0.1, 100)` or `fmt.Sprintf("%.100f", [fmt.FArg(1.0)])`) writes past the 64-byte heap allocation.

- **Impact:** Direct heap buffer overflow controlled by the `precision` parameter or format string.
- **Remediation:** Clamp `precision` (e.g., `0 <= precision <= 100`) and allocate `libc.malloc(intLen + precision + 4)` dynamically (or use `str.Builder` / `buf.Buffer`).

---

#### VULN-13: Heap Out-of-Bounds Read, Integer Overflow, and Decompression Bomb in `compress.Decompress`

- **Severity:** High
- **Location:** `std/compress.skink:189-209`, `std/compress.skink:54-68,110-118`
- **Description:**`compress.Decompress(data: string)` takes a raw pointer `data` with **no input buffer length parameter** and reads an 8-byte little-endian header directly from `data[0..7]`:

  ```skink
  pub fn Decompress(data: string) -> (string, errors.Error) {
      origSize := int(data[0]) | (int(data[1]) << 8) | (int(data[2]) << 16) | (int(data[3]) << 24)
      clen := int(data[4]) | (int(data[5]) << 8) | (int(data[6]) << 16) | (int(data[7]) << 24)
      if origSize < 0 || clen <= 0 || clen > 268435456 {
          return "", errors.New("not a compress.Compress buffer")
      }
      payload := libc.malloc(clen + 1)
      for i := 0; i < clen; i = i + 1 {
          payload[i] = data[i + 8]
      }
      out := libc.calloc(origSize + 1, 1)
      rc := Skink_uncompress(payload, clen, out, origSize)
  ```

  1. **Out-of-Bounds Read on Short Input / Spoofed `clen`:** If `data` is shorter than 8 bytes, `data[0..7]` reads out of bounds. Worse, if an attacker supplies an 8-byte buffer whose header claims `clen = 1000000`, the loop reads `1,000,000` bytes from `data[8..1000007]` on the heap.
  2. **Signed 32-Bit Overflow on `origSize == INT32_MAX`:** If `origSize == 2147483647` (`0x7FFFFFFF`), `origSize < 0` is false, `origSize + 1` overflows to `-2147483648`, `libc.calloc(-2147483648, 1)` returns `NULL`, and `Skink_uncompress` passes `dest = NULL, destCap = 2147483647` to zlib's `uncompress()`.
  3. **Unbounded `origSize` (Decompression Bomb):** Even for `origSize = 2147483646`, a tiny input immediately triggers a 2 GB `calloc`.
- **Impact:** Crash, heap out-of-bounds read disclosure, or memory exhaustion when decompressing untrusted data.
- **Remediation:** Require an explicit `dataLen: int` parameter (or use `[]byte` slice headers carrying `Skink_slice_len`), verify `dataLen >= 8 && clen <= dataLen - 8`, and cap `origSize` to a safe maximum (e.g., `<= 268435456`).

---

#### VULN-14: 1-Byte Heap Buffer Overflow in `readChunkedBody`, `Content-Length` Integer Overflow, and Request Smuggling

- **Severity:** High
- **Location:** `std/web/server.skink:389-424`, `std/web/server.skink:538-605`
- **Description:**
  1. **1-Byte Heap Buffer Overflow on Power-of-2 Chunked Bodies (`readChunkedBody`, lines 540–588 & 416–420):**`readChunkedBody` allocates `body := Skink_mem_alloc(int64(1024))` (`bodyCap = 1024`) and grows the buffer only when `bodyLen + size > bodyCap`. When the total chunked body size `blen` is an exact power of two $\ge 1024$ (`1024`, `2048`, `4096`, ...), `bodyLen + size == bodyCap`, so no reallocation occurs. When `readChunkedBody` returns to `readRequest` (line 420), `body[blen] = 0` writes a NUL byte at `body[bodyCap]` — **1 byte past the end of the heap allocation**.
  2. **Signed 32-Bit Integer Overflow in `Content-Length` Check (line 399):**

     ```skink
     if bodyStart - r.head + contentLen > r.max {
         return empty, 413
     }
     ```

     `contentLen` is a 32-bit `int` parsed via `strconv.ParseInt`. If a client sends `Content-Length: 2147483647` (`INT32_MAX`), `bodyStart - r.head + contentLen` overflows signed 32-bit `int` to a negative value (`< -2147483600`), bypassing the `> r.max` (`413 Payload Too Large`) check.

  3. **HTTP Request Smuggling (`CL.TE` / Substring `Transfer-Encoding` Match, lines 390–439):**`readRequest` checks `str.Contains(str.ToLower(req.Header("Transfer-Encoding")), "chunked")` and accepts requests that contain **both** `Content-Length` and `Transfer-Encoding`. Per RFC 9112 §6.3, accepting both headers or matching `"chunked"` as an unanchored substring (e.g., `Transfer-Encoding: notchunked`) enables `CL.TE` / `TE.CL` HTTP request smuggling across keep-alive connections behind reverse proxies.
- **Impact:** 1-byte heap corruption on any chunked HTTP request of size $1024 \times 2^k$, bypass of `maxRequestBytes` limit, and HTTP request smuggling.
- **Remediation:** In `readChunkedBody`, grow the buffer when `bodyLen + size + 1 > bodyCap` to reserve room for the trailing NUL byte. In `readRequest`, check `contentLen > r.max || bodyStart - r.head > r.max - contentLen` to prevent integer overflow, and reject requests with `400 Bad Request` when both `Transfer-Encoding` and `Content-Length` headers are present.

---

#### VULN-15: Integer Overflow in `mcp` Body Allocation, Unbounded Client Response Growth, and CRLF Injection

- **Severity:** High
- **Location:** `std/mcp.skink:107-146,244-272`, `std/api.skink:118-138,176-195,229-253,283-308`, `std/net.skink:218-221,269-277`
- **Description:**
  1. **`std/mcp.skink` Unchecked `Content-Length` Overflow (lines 244–272):**`parseHttpRequest` parses `Content-Length` via unchecked `contentLength = contentLength * 10 + int(val[j]) - 48` with no upper bound. Sending `Content-Length: 2147483647` overflows `contentLength + 1` to `-2147483648`, causing `libc.malloc(contentLength + 1)` to return `NULL` (`0`), and `conn.Read(string(0 + readSoFar), ...)` passes a NULL buffer to `recv()`. Similarly, `readLineStdio` and `readLineSocket` (lines 107–146) buffer bytes without any maximum line length.
  2. **`std/api.skink` Unbounded Response Buffer Growth:** In `Client.Get`, `Client.doTLSGet`, `Client.Post`, and `Client.doTLSPost`, the response loop doubles `bufSize = bufSize * 2` indefinitely without a maximum size cap. A malicious server can stream data until memory is exhausted or `bufSize * 2` overflows 32-bit `int` to `-2147483648`, causing `libc.realloc` to return `NULL`.
  3. **CRLF / HTTP Header Injection in `std/api.skink` and `std/net.skink`:**`net.HTTPGet`, `net.HTTPPost`, `api.Client.Get`, and `api.Client.Post` concatenate `path`, `host`, and `contentType` directly into the HTTP request string without stripping or rejecting `\r` and `\n`.
- **Impact:** Remote DoS / NULL-pointer crash in MCP HTTP servers and HTTP clients, and HTTP request splitting/header injection when user-supplied URLs are passed to `api.Get` / `net.HTTPGet`.
- **Remediation:** Enforce a maximum `Content-Length` and line length in `std/mcp.skink`, cap response buffers in `std/api.skink` (matching `maxResponseSize` in `std/net.skink`), and reject URLs/headers containing `\r`, `\n`, or NUL bytes.

---

#### VULN-16: Uninitialized Heap Memory Disclosure and Missing NUL Terminator in `Connection.Recv`

- **Severity:** High
- **Location:** `std/net.skink:94-101`
- **Description:**

  ```skink
  pub fn Recv(self: *Connection, maxLen: int) -> (string, errors.Error) {
      buf := libc.malloc(maxLen + 1)
      bytesRead := recv(self.fd, buf, maxLen, 0)
      if bytesRead <= 0 {
          return "", errors.New("recv failed")
      }
      return buf, errors.New("")
  }
  ```

  `libc.malloc` (`Skink_raw_alloc`) returns **uninitialized** `malloc` memory, and `Recv` never sets `buf[bytesRead] = 0` (nor does it free `buf` when `bytesRead <= 0`). Because Skink strings are NUL-terminated and carry no separate length, every caller of `conn.Recv(maxLen)` receives a buffer whose bytes from `bytesRead` to the next arbitrary `0x00` byte on the heap are exposed as part of the returned string, and if no `0x00` byte exists within `buf[bytesRead..maxLen]`, `strlen`/`println` reads out of bounds past the heap chunk.

- **Impact:** Heap memory disclosure (leaking previously freed heap contents into network responses or logs) and heap buffer over-read on every call to `net.Connection.Recv`.
- **Remediation:** Check `maxLen >= 0`, terminate `buf[bytesRead] = 0` before returning, and `libc.free(buf)` on the `bytesRead <= 0` error path.

---

#### VULN-20: SQL Injection via Unquoted Table/Column Metadata and Raw `Where` Clause Concatenation

- **Severity:** Medium
- **Location:** `std/orm.skink:147-175`, `std/orm.skink:236-398`, `std/db.skink:252-263`
- **Description:**
  1. `orm.TableNamed(conn, t, name)` and `[column(name=...)]` / `[db(name=...)]` interpolate table and column names directly into `CREATE TABLE`, `DROP TABLE`, `INSERT`, `UPDATE`, `SELECT`, and `DELETE` SQL strings without quoting or identifier validation.
  2. `orm.Where(clause: string, args: []string)` concatenates `clause` directly into `SELECT ... FROM <table> WHERE <clause>`, and `orm.Query(sql: string)` executes raw SQL without parameter binding support.
  3. `db.QuoteString` (marked deprecated in comments, `std/db.skink:252`) remains exported and only escapes `'` as `''`.
- **Impact:** SQL injection if table names or `Where` filter clauses are constructed from untrusted input.
- **Remediation:** Validate or double-quote (`"..."`) all SQL identifiers in `std/orm.skink`, and add parameterized `QueryArgs(sql: string, args: []string)` support.

---

#### VULN-21: Unchecked Slice Bounds, Integer Overflow in Tensor Dimensions, and Hardware `SIGFPE` on `stride == 0`

- **Severity:** Medium
- **Location:** `std/runtime/tensor_rt.c:48-78,99-115,258-364,847-947`, `std/tensor.skink:54-87,122-130`
- **Description:**
  1. **Hardware Divide-by-Zero (`SIGFPE`):** In `Skink_tensor_conv2d_data` (`tensor_rt.c:886-887`) and `Skink_tensor_maxpool2d_data` (`tensor_rt.c:921-922`), `int out_h = (in_h - kernel) / stride + 1;` divides by `stride` in C without checking `stride > 0`, raising an unrecoverable `SIGFPE` when `stride == 0`.
  2. **Unchecked Slice Lengths & Negative/Overflowing Dimensions in `tensor_rt.c`:** Functions such as `Skink_tensor_matmul_data`, `Skink_lstm_forward`, `Skink_tensor_conv2d_data`, and `Skink_tensor_add_data` unwrap raw `double *` pointers via `Skink_slice_data` without verifying via `Skink_slice_len` that the backing slices contain enough elements for the passed dimensions (`m, k, n, batch, in_ch, in_h, in_w, ...`).
  3. **Unchecked Coordinate Indexing in `Tensor.Get` / `Tensor.Set` (`std/tensor.skink:54-87`):** `Get(idx)` and `Set(idx, v)` do not check `0 <= idx[i] < self.shape[i]`; a negative or out-of-range coordinate in one dimension can fold into a valid flat `offset` in an adjacent row/channel (silent data corruption) or overflow 32-bit `offset`.
- **Impact:** Heap out-of-bounds read/write or process crash (`SIGFPE`) when tensor operations receive invalid or untrusted shapes/strides.
- **Remediation:** Validate `stride > 0`, non-negative dimensions, and `Skink_slice_len` bounds at the entry of `tensor_rt.c` functions, and check `0 <= idx[i] < self.shape[i]` in `Tensor.Get` and `Tensor.Set`.

---

#### VULN-22: Key Truncation on NUL Bytes (`strlen`), Unwiped Plaintext on GCM Tag Failure, and Heap Leaks

- **Severity:** Medium
- **Location:** `std/encrypt.skink:152-189,241-273,342-344`, `std/crypto.skink:89-136`
- **Description:**
  1. **Binary Key Truncation at `0x00` (`std/encrypt.skink:342`, `std/crypto.skink:91`):**`encrypt.NewAES(key)` computes `keyLen: str.Len(key)` (`strlen`), and `crypto.HmacSha256(key, message)` computes `keyLen := str.Len(key)` and `msgLen := str.Len(message)`. Because random binary keys and messages frequently contain `0x00` bytes ($\approx 11.8\%$ of random 32-byte AES-256 keys contain at least one `0x00` byte), `NewAES` rejects valid binary keys (or misidentifies key length if a longer buffer has a `0x00` at offset 16/24/32), and `HmacSha256` silently truncates keys and messages at the first NUL byte. Even worse, in `HmacSha256` (line 95), when `keyLen > 64`, `key = Sha256(key)` returns a 32-byte raw binary digest allocated with `libc.malloc(32)` (no NUL terminator) that is never freed.
  2. **Unwiped Unauthenticated Plaintext & Memory Leaks on Error (`std/encrypt.skink:241-263`):** In `AESCipher.Decrypt`, `outBuf` is populated by `EVP_DecryptUpdate` **before** `EVP_DecryptFinal_ex` verifies the GCM authentication tag. When tag verification fails (line 258), `outBuf` is neither zeroed nor freed (`libc.free(outBuf)` is missing on all error returns in `Encrypt` and `Decrypt`), leaving unauthenticated plaintext in heap memory.
- **Impact:** Silent truncation of cryptographic keys/messages containing NUL bytes in `HmacSha256`, rejection of ~11.8% of random 256-bit keys in `NewAES`, and retention of unauthenticated plaintext in heap memory on GCM authentication failure.
- **Remediation:** Add explicit key/message length parameters (or `[]byte` overloads) to `NewAES`, `HmacSha256`, and `Sha256`; zero and free `outBuf` before returning an error in `AESCipher.Decrypt` and `AESCipher.Encrypt`.

---

#### VULN-24: Thread-Unsafe `stdout` Redirection Race in Windows `Skink_os_exec_with_output`

- **Severity:** Medium
- **Location:** `std/runtime/fs_rt.c:276-286`
- **Description:** On Windows (`_WIN32`), `Skink_os_exec_with_output` redirects the process-global `stdout` file descriptor (`_dup2(pipefd[1], _fileno(stdout))`) around `_spawnvp(_P_NOWAIT, ...)` without any mutex:

  ```c
  int saved_stdout = _dup(_fileno(stdout));
  _dup2(pipefd[1], _fileno(stdout));
  _close(pipefd[1]);
  intptr_t child = _spawnvp(_P_NOWAIT, argv[0], (const char *const *)argv);
  _dup2(saved_stdout, _fileno(stdout));
  _close(saved_stdout);
  ```

  If two threads call `os.ExecWithOutput` concurrently (or another thread writes to `stdout`), the file descriptors race: one thread can capture another thread's `stdout` as `saved_stdout`, permanently hijacking or closing the process's `stdout` and leaking the pipe write end into the child process (causing the pipe read loop to hang forever waiting for EOF).

- **Impact:** Deadlock, corrupted child output capture, or hijacked `stdout` in multi-threaded Windows programs using `os.ExecWithOutput`.
- **Remediation:** Use `CreateProcess` with `STARTUPINFO.hStdOutput` set to the pipe handle (avoiding process-wide `_dup2` on `stdout`), or guard the redirection window with a process-wide mutex.

---

#### VULN-25: 64-Bit Pointer Truncation to 32-Bit `int` in `str.Contains` and `str.IndexOf`

- **Severity:** Low
- **Location:** `std/str.skink:155,244`
- **Description:** In `std/str.skink`:

  ```skink
  pub fn Contains(s: string, needle: string) -> bool {
      ...
      return int(libc.strstr(s, needle)) != 0
  }
  pub fn IndexOf(s: string, part: string) -> int {
      if part == "" { return 0 }
      pos := libc.strstr(s, part)
      if int(pos) == 0 { return -1 }
      return int(int64(pos) - int64(s))
  }
  ```

  `libc.strstr` returns a 64-bit pointer (`i8*`), which `int(...)` truncates to 32 bits (`ptrtoint i8* %r to i32`). If a string buffer is located at a 4 GiB-aligned virtual address (`0x...00000000`), `int(pos) == 0` evaluates to `true` even though `strstr` found a match! (Note that `std/web/static.skink` relies on `str.Contains(path, "..")` for path-traversal filtering.)

- **Impact:** False-negative substring checks (`str.Contains` returning `false` and `str.IndexOf` returning `-1`) when the matched substring lands on a 4 GiB-aligned address boundary.
- **Remediation:** Use `int64(libc.strstr(s, needle)) != 0` and `if int64(pos) == 0 { return -1 }` instead of `int(...)`.

---

#### VULN-26: Root Escape When `rootDir` Is Empty and Request Path Begins with `//`

- **Severity:** Low
- **Location:** `std/web/static.skink:61-85`
- **Description:** In `serveFile` (`std/web/static.skink:77-84`):

  ```skink
  rel := path
  if str.StartsWith(rel, "/") {
      rel = str.Substr(rel, 1, str.Len(rel) - 1)
  }
  if str.Len(rel) > 0 {
      filePath = str.Concat(str.Concat(rootDir, "/"), rel)
  }
  ```

  Only a **single** leading `/` is stripped from `rel`, and `serveFile` does not normalize paths or reject backslashes (`\`). If `rootDir` is `""` and a client requests `GET //etc/passwd`, `filePath` becomes `//etc/passwd`, which resolves to `/etc/passwd` on POSIX. On Windows, a request containing `\` (or if `serveFile` is invoked directly by a custom server that did not call `URLDecode`) can also bypass traversal expectations.

- **Impact:** Arbitrary file read if `static.FileServer("")` is used or on Windows if backslash separators are used.
- **Remediation:** Strip all leading `/` and `\` characters in a `for` loop, normalize `\` to `/`, call `types.URLDecode` defensively inside `serveFile`, and default an empty `rootDir` to `"."`.

---

#### VULN-27: Predictable PRNG Seeding (`srand(time(NULL))`) and Unsynchronized Global State

- **Severity:** Low
- **Location:** `std/rand.skink:35-43`, `std/tensor.skink:30-40`
- **Description:**`std/rand.skink` seeds libc's non-thread-safe `srand(int(time(nil)))` with 1-second resolution on first use, and `std/tensor.skink` uses a non-atomic global `prngSeed` LCG seeded with `time.NowUnixNano()`. While `std/rand.skink`'s header comment notes that it is not cryptographically secure, `rand()` and `seeded` / `prngSeed` are unsynchronized across spawned threads.
- **Impact:** Predictable random sequences if misused for tokens/IDs, and data races on `seeded` / `prngSeed` / libc `rand()` state under `spawn` concurrency.
- **Remediation:** Use atomic operations or thread-local state for PRNG state, and provide a `crypto.RandomBytes` helper backed by OpenSSL's `RAND_bytes`.

---

## Remediation Status

All 28 vulnerabilities identified in this audit have been remediated. The
table below maps each finding to its fix and its regression test. The
full suite (`bash run_tests.sh`) passes: **112 suites / 1259 test
functions, 0 failures**, plus `tests/harness_injection_test.sh` and the
ASan+UBSan-instrumented `tests/run_win_regex_selftest.sh`. Targeted
AddressSanitizer+LeakSanitizer runs of the security test binaries and
the memory-sensitive stdlib suites (`buf`, `strconv`, `compress`,
`crypto`, `str`, `path`) report clean.

| ID          | Fix                                                                                                                                                                                            | Regression coverage                                                                                                          |
| ----------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| **VULN-01** | `skink.skink` builds an explicit argv array and invokes clang via `Skink_os_exec` (no shell); temporary `.ll` files come from `Skink_fs_mktemp` (`open(O_CREAT\|O_EXCL)` retry loop).            | `std/driver_security_test.skink` — output paths with spaces, `;`, `$()`, quotes; asserts no side-effect file is created.       |
| **VULN-02** | `string(...)` casts call runtime helpers (`Skink_format_double_f` etc.) that `snprintf`-probe the exact size and return RC-managed heap strings — no fixed stack buffer, no escaping `alloca`.   | `std/codegen_safety_test.skink` — large-double conversion, closure-returned strings, repeated conversions.                    |
| **VULN-03** | `emitMakeExpr` and map/set allocations compute `count * elemSize` in `i64` via `i64Operand` and call `Skink_rc_alloc_array`, which rejects negative/overflowing sizes.                           | `std/codegen_safety_test.skink` — negative and overflowing `make` sizes panic.                                               |
| **VULN-04** | `emitFromEndExpr` (reads) and `emitIndexAssignment` (writes) resolve `len - i` and call `Skink_bounds_check`. `arr[^i] = v` (previously a silently-dropped store) now works and is checked.      | `std/codegen_safety_test.skink` — `[^0]`, `[^5]` on len-4, negative offsets panic; `[^1]`/`[^4]` read and write correctly.    |
| **VULN-05** | `fs_rt.c` (`Skink_signal_handle`) and `tensor_rt.c` callbacks load `handle[0]` and invoke `code(handle, ...)` — the Skink closure ABI — instead of calling the handle as a C function pointer.    | `std/math_test.skink` `Skink_math_diff` extern typed `fn(float) -> float` callback.                                            |
| **VULN-06** | `panic_rt.c` owns a growable thread-local pool of real `jmp_buf`s; the emitter calls `Skink_panic_acquire`/`commit` instead of a fixed `[264 x i8]` alloca. `push_handler` kept as stage-0 shim.    | `std/panic_security_test.skink` — 201 deferred frames unwind cleanly under ASan.                                              |
| **VULN-07** | Same pool removes the `MAX_PANIC_DEPTH` cap: push can never silently fail while pop still runs, so `panic_depth` cannot desynchronize.                                                            | `std/panic_security_test.skink` — second panic recovers correctly after a 201-deep unwind.                                     |
| **VULN-08** | `win_regex.h` `wr_match` uses `group` before freeing it and rejects unclosed `(` groups before the negative-length `memcpy`.                                                                      | `tests/win_regex_selftest.c` run by `tests/run_win_regex_selftest.sh` — ASan+UBSan clean on Linux (Windows paths unverifiable here). |
| **VULN-09** | Embed paths are canonicalized (`realpath`) in both the checker (`-check`) and emitter, and must stay inside the declaring source directory; `walkEmbedDir` has a depth cap for symlink loops.    | `std/compilefail/cf_embed_traversal.skink`, `cf_embed_outside.skink`.                                                          |
| **VULN-10** | DI file/directory metadata and generated string literals escape `"` and `\` before interpolation into LLVM IR / DWARF records.                                                                    | `std/driver_security_test.skink` — source paths with quotes compile to valid IR.                                               |
| **VULN-11** | `str.Builder.ensure`, `str.Repeat`, and `buf.Buffer` compute sizes in `int64`/`size_t` and panic on overflow or OOM instead of silently returning.                                                | `std/buf_test.skink` — oversized grow attempts panic rather than `memcpy` past capacity.                                       |
| **VULN-12** | `strconv.FormatFloat` and `fmt`'s `formatFloatPrec` clamp precision to [0, 340] and delegate to `Skink_format_double_prec` (exact `snprintf` probing); the old int64 scale loop is gone.           | `std/strconv_test.skink` — `FormatFloat(1.5, 100)` and precision-341 panic.                                                    |
| **VULN-13** | New `compress.DecompressBuf(data, dataLen)` validates the header bounds, checks `origSize`/`clen` against limits, and caps output; legacy `Decompress` delegates via `str.Len` (see note below).  | `std/compress_roundtrip_test.skink` — truncated headers, oversized claimed sizes rejected.                                     |
| **VULN-14** | `readChunkedBody` reserves the NUL terminator in capacity math; `Content-Length` is digit-bounded (no i64 wrap); `CL + Transfer-Encoding` ambiguity and non-exact `chunked` tokens are rejected.  | `std/web/server_live_test.skink` — smuggling and exact-capacity chunked bodies over a real socket.                             |
| **VULN-15** | `mcp.skink` bounds Content-Length accumulation; `api.skink`/`mcp.skink` cap response-buffer growth and reject CR/LF in host, path, and header values.                                             | `std/mcp_test.skink`, `std/api_test.skink` additions.                                                                          |
| **VULN-16** | `net.Connection.Recv` allocates `n+1`, zero-initializes, and NUL-terminates the receive buffer; short reads and closed connections return safe values.                                            | `std/net_test.skink` additions.                                                                                                |
| **VULN-17** | `reflect_rt.c` replaced the `mincore`/`VirtualQuery` mapped-memory heuristic with a fault-protected probe plus `Skink_rc_is_registered` registry lookup.                                          | `std/reflect_test.skink`, `std/hashmap_test.skink` (exercise the heuristic on every generic key op).                           |
| **VULN-18** | `net_rt.c` POSIX connect-timeout path uses `poll()` instead of `select()`/`fd_set` — no `FD_SETSIZE` ceiling. Windows `fd_set` is a SOCKET array and was already safe.                            | `std/net_test.skink`.                                                                                                          |
| **VULN-19** | `resolveAliasCG` is iterative (no recursion); the checker rejects cyclic aliases at registration and detects cyclic by-value struct fields with a DFS over `TK_NAMED` fields.                    | `std/compilefail/cf_cyclic_alias.skink`, `cf_cyclic_struct.skink`.                                                             |
| **VULN-20** | `std/orm.skink` validates identifiers to `^[A-Za-z_][A-Za-z0-9_]*$` and double-quotes every generated identifier; `Where`/`Query` docs warn they are raw-SQL APIs.                                | `std/orm_test.skink` — injection strings rejected, CRUD still works.                                                           |
| **VULN-21** | `tensor_rt.c` validates dims/strides (overflow-safe); `std/tensor.skink` guards `stride == 0` before division, `Get`/`Set` check per-dim indices.                                                | `std/tensor_test.skink` additions.                                                                                             |
| **VULN-22** | `encrypt.skink`/`crypto.skink` use explicit lengths (embedded NULs work), wipe intermediates via `OPENSSL_cleanse`, and destroy plaintext/output buffers on GCM tag failure.                      | `std/crypto_test.skink`, `std/encrypt_test.skink` — explicit-length HMAC length assertions, bad-tag rejection.                |
| **VULN-23** | `skink_test.sh`/`skink_bench.sh` restrict basenames to the identifier grammar and use `$$`-unique generated names; `mktemp` replaces predictable temp paths in all harness scripts.               | `tests/harness_injection_test.sh` — quote-injection filename skipped, `PWNED` never executes, valid files still run.           |
| **VULN-24** | Windows `Skink_os_exec_with_output` uses `CreateProcess` with child-specific pipe handles — no global `_dup2` on the parent's `stdout`. **Note:** Windows-only code path; reviewed, not runnable on Linux. | `std/fs_proc_test.skink` (POSIX equivalent).                                                                                   |
| **VULN-25** | `str.Contains`/`IndexOf` compute pointer differences in `int64` — no 32-bit truncation for offsets above 2 GB.                                                                                   | `std/str_test.skink`.                                                                                                          |
| **VULN-26** | `serveFile` rejects `..` raw components, canonicalizes root and target via `path.Abs`, and enforces containment (incl. the `index.html` fallback). `path.Clean` now preserves leading `/` and drops root-level `..`. | `std/web/static_test.skink` — traversal → 403, symlink escapes → 403/404, `/` → `index.html` → 200.                             |
| **VULN-27** | `std/rand.skink` first-use entropy seeding runs under `Skink_rand_seed_once` (`pthread_once` in `sync_rt.c`); `Seed()` consumes the once-guard so explicit seeds can't be clobbered by a racing auto-seed. | `std/rand_test.skink` — concurrent first-use race test with per-worker completion flags.                                      |
| **VULN-28** | `install.sh` validates version/repo/prefix arguments and performs strict SHA-256 checksum retrieval and verification with corrected exit-status handling around `if !`.                           | Manual review; the checksum path requires a release environment (see note below).                                              |

### Notes & Limitations

- **`compress.Decompress` (legacy API)** takes a `string`, which is
  NUL-terminated — `str.Len` cannot measure binary payloads containing
  `\0`, so the legacy entry point conservatively rejects blobs whose
  header contains NUL bytes. Callers should migrate to
  `DecompressBuf(data, dataLen)`, which takes an explicit length and is
  fully safe. The round-trip test uses `DecompressBuf`.
- **Windows-only paths** (VULN-08 regex internals, VULN-24
  `CreateProcess`) were fixed by source review; `win_regex.h` is
  exercised on Linux by the ASan-instrumented self-test, but the Windows
  `CreateProcess` path cannot execute on this platform.
- **`install.sh` checksum verification** is static-reviewed; exercising
  it end-to-end requires a published release + checksum file.
- **`SKINK_CLANG_FLAGS`** is still an env-provided escape hatch for
  sanitizer/debug builds (`ASAN=1 bash run_tests.sh`); it is passed to
  clang as argv elements, not through a shell, so it can no longer
  inject commands — only flags.
