# Skink Production Readiness Remediation Plan

This document lists the concrete steps required to address the production-blocking issues found in the Skink compiler and standard library. Items are grouped by priority: **P0** (must fix before any production use), **P1** (fix before handling untrusted input or running as a service), and **P2** (fix before general production release).

---

## P0 — Must Fix Before Production Use

These issues cause crashes, silent miscompilation, or fundamental language features to fail.

### 1. Implement slice expressions in the emitter
**Where:** `std/emitter.skink` lines 4632-4637 (also mirror to `bootstrap-home/std/emitter.skink`)

**Problem:** `arr[low:high]`, `arr[:high]`, and `arr[low:]` are parsed but the emitter is a stub that returns the original array unchanged.

**Steps:**
1. In `emitSliceExpr`, evaluate `low` and `high` expressions (defaulting to `0` and `len(target)` when omitted).
2. Generate LLVM IR to allocate a new slice header/value with the adjusted data pointer and length.
3. Update the runtime if needed so slices carry `(data, len, cap)` consistently.
4. Add tests covering `arr[1:3]`, `arr[:2]`, `arr[2:]`, and out-of-bounds cases.

### 2. Implement or remove the `**` power operator
**Where:** `std/emitter.skink` lines 2931-3125; `std/parser.skink` precedence table ~line 240

**Problem:** `**` is parsed but `emitInfixExpr` has no case for it, so it falls through and returns `0`.

**Steps:**
1. Add a case in `emitInfixExpr` for `op == "**"`.
2. For integer operands, emit a loop or call to a runtime helper. For floating-point operands, emit `llvm.pow.*`.
3. Fix parser associativity: `**` should be right-associative (`2**3**2 == 2**(3**2)`).
4. Add tests for both integer and float exponentiation, including right-associativity.

### 3. Add null checks after every runtime allocation
**Where:**
- `std/runtime/conc_rt.c` (channel/future/spawn)
- `std/runtime/rules_rt.c` (ruleset)
- `std/runtime/sync_rt.c` (mutex/rwlock/condvar)
- `std/runtime/tensor_rt.c` (tensors)
- `std/runtime/rc_rt.c` `rc_table_grow`
- `std/runtime/panic_rt.c` `Skink_format_float`

**Problem:** `malloc`/`calloc`/`realloc` results are dereferenced without null checks, causing segfaults under memory pressure.

**Steps:**
1. Audit every `malloc`/`calloc`/`realloc` in `std/runtime/*.c`.
2. Return an error value (e.g., `NULL`, `-1`, or a sentinel) when allocation fails.
3. Update Skink-facing extern declarations so failures can be surfaced as errors where possible.
4. Add unit tests that simulate OOM via small allocation limits where feasible.

### 4. Fix RC string lifetime for map keys
**Where:** `std/str.skink` lines 115-123; `std/emitter.skink` map code; `std/runtime/rc_rt.c`

**Problem:** `str.Concat` returns RC-managed strings, but maps do not retain their keys, so keys can be freed while still in the map.

**Steps:**
1. **Preferred fix:** Make the map implementation retain keys (and values) when inserting and release them on removal/free. This requires the emitter to emit `Skink_rc_retain` for keys during map insertion.
2. **Alternative fix:** Provide a dedicated `map[string]T` runtime path that copies keys into non-RC raw memory managed by the map.
3. Remove the manual `mallocString`/`concatMalloc` workarounds in `std/logic.skink` once the language handles this correctly.
4. Add stress tests that use `str.Concat`-produced strings as map keys.

### 5. Replace unsound generic reflection heuristics
**Where:** `std/runtime/reflect_rt.c` lines 43-122; `std/emitter.skink` `emitReflectTypeOf`; `std/hashmap.skink`

**Problem:** Generic type parameters are erased to `i8*`. Runtime functions guess whether a value is a string or integer by inspecting pointer ranges and memory content, which can segfault or produce wrong hash/equality results.

**Steps:**
1. **Preferred fix:** Implement monomorphization for generic functions. Each concrete instantiation gets its own LLVM function with real types.
2. **Alternative fix:** Pass a runtime type tag/handle for each type parameter at every generic call site, and use it in `reflect_hash`/`reflect_equals`.
3. Update `HashMap<K,V>` so that hash/equality use the real type of `K`.
4. Add tests for `HashMap<int,int>`, `HashMap<string,int>`, and `HashMap<*T,int>`.

### 6. Fix the RC retain lock to use a read lock
**Where:** `std/runtime/rc_rt.c` lines 294-306

**Problem:** `Skink_rc_retain` uses `pthread_rwlock_wrlock` even though it only reads the table and increments an atomic counter, causing unnecessary serialization.

**Steps:**
1. Replace `pthread_rwlock_wrlock(&rc_lock)` with `pthread_rwlock_rdlock(&rc_lock)` in `Skink_rc_retain`.
2. Verify that table entries are not mutated under this path.
3. Run the concurrent reference-counting tests under load.

---

## P1 — Fix Before Handling Untrusted Input or Running as a Service

### 7. Enable TLS certificate verification and hostname checking
**Where:** `std/runtime/tls_rt.c` lines 115-137; callers in `std/api.skink`

**Problem:** `Skink_tls_client_connect` sets verify paths but never enables `SSL_VERIFY_PEER` or hostname verification.

**Steps:**
1. In `Skink_tls_client_connect`:
   - Call `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL)` after `SSL_CTX_new`.
   - Pass the target hostname into the API and call `SSL_set1_host(ssl, hostname)` (or verify against the peer certificate after connect).
2. Update `std/api.skink` `Client` to store the hostname and pass it through.
3. Add tests connecting to a server with a valid cert, a self-signed cert (should fail), and a cert for the wrong hostname (should fail).

### 8. Switch AES-CBC to authenticated encryption
**Where:** `std/encrypt.skink` lines 22-140

**Problem:** AES-CBC without a MAC is vulnerable to padding oracle and bit-flipping attacks.

**Steps:**
1. Replace AES-CBC with AES-GCM.
2. The ciphertext format should be: `nonce (12 bytes) || tag (16 bytes) || ciphertext`.
3. Update `Decrypt` to verify the tag before returning plaintext.
4. Add negative tests with tampered ciphertext.

### 9. Replace RSA PKCS#1 v1.5 padding with OAEP
**Where:** `std/encrypt.skink` lines 48, 240, 268

**Problem:** PKCS#1 v1.5 is vulnerable to Bleichenbacher-style attacks.

**Steps:**
1. Change `RSA_PKCS1_PADDING` to `RSA_PKCS1_OAEP_PADDING`.
2. Ensure the public/private key functions use the same padding.
3. Add round-trip encryption/decryption tests.

### 10. Remove or deprecate broken hash algorithms
**Where:** `std/crypto.skink` lines 21-42

**Problem:** MD5 and SHA1 are exposed as public APIs despite being cryptographically broken.

**Steps:**
1. Remove `Md5`, `Md5Hex`, `Sha1`, and `Sha1Hex`, or move them to an internal/legacy module with warnings.
2. Keep only SHA256/SHA512 and their HMAC variants.
3. Update documentation to warn users against using MD5/SHA1 for security.

### 11. Fix random number generation modulo bias
**Where:** `std/rand.skink` lines 39-56

**Problem:** `Intn` uses raw `% n`, which biases results when `n` does not divide `2^32`.

**Steps:**
1. Use rejection sampling: keep drawing 32-bit values until `v < (2^32 - (2^32 % n))`, then return `v % n`.
2. Fix `Int()` so it does not call `Intn(INT_MAX)` with bias.
3. Add statistical tests for uniform distribution.

### 12. Add platform-independent secure random source
**Where:** `std/rand.skink` lines 26-37

**Problem:** `rand.Bytes` reads from `/dev/urandom`, which does not exist on Windows.

**Steps:**
1. Add a C runtime helper that uses OpenSSL `RAND_bytes` on all platforms, or platform-specific APIs (e.g., `BCryptGenRandom` on Windows).
2. Fall back gracefully if the entropy source is unavailable.
3. Add tests that verify non-determinism and error handling.

### 13. Parameterize database queries
**Where:** `std/db.skink` lines 79-104; `std/runtime/db_rt.c`

**Problem:** `Connection.Exec` passes raw SQL to `sqlite3_exec`; there is no parameter-binding API.

**Steps:**
1. Expose `sqlite3_prepare_v2`, `sqlite3_bind_*`, `sqlite3_step`, and `sqlite3_finalize` in `std/runtime/db_rt.c`.
2. Add a high-level API such as `QueryArgs(sql, ...args)` or prepared-statement objects.
3. Deprecate or clearly document `Exec` as unsafe for user input.
4. Remove `QuoteString` or mark it unsafe.
5. Add SQL injection tests.

### 14. Fix command injection in `os` and the compiler driver
**Where:** `std/os.skink` lines 37-115; `skink.skink` lines 39-51

**Problem:** `os.System`, `os.Run`, `os.RunWithOutput`, and `skink -t` pass user strings directly to `system()`/`popen()`.

**Steps:**
1. Replace `system()`/`popen()` with `execve`/`fork`/`pipe` in the C runtime, exposing an API that takes an argv array.
2. In Skink, add `os.RunArgs(args []string)` that does not invoke a shell.
3. For the compiler driver, validate that `argv[2]` matches `^[A-Za-z0-9_/]+$` before invoking the test script, or use `execve`.
4. Add tests with malicious command strings to ensure they are not executed.

### 15. Fix race conditions in the ruleset runtime
**Where:** `std/runtime/rules_rt.c` lines 36-113

**Problem:** `Skink_ruleset_evaluate` reads the rules array outside the mutex while `Skink_ruleset_add_rule` can realloc it. The detached thread cannot be joined before freeing the ruleset.

**Steps:**
1. Hold the ruleset mutex for the entire evaluation loop.
2. Add a `stopped` condition variable so `Skink_ruleset_stop` can wait for the loop thread to exit.
3. Add a `Skink_ruleset_destroy` function that destroys the mutex after the thread has exited.
4. Add concurrent add/evaluate/stop tests.

### 16. Make the HTTPS server concurrent
**Where:** `std/web/server.skink` lines 85-91

**Problem:** `ListenAndServeTLS` handles connections in the accept loop, while `ListenAndServe` spawns threads.

**Steps:**
1. Change `ListenAndServeTLS` to call `Skink_web_spawn_conn` with a TLS-aware handler entry point.
2. Move TLS context creation outside the per-connection path where possible.
3. Add concurrent load tests for both HTTP and HTTPS.

### 17. Replace process-wide `abort()` with recoverable panic handling
**Where:** `std/runtime/panic_rt.c` lines 5-12, 25-32; `std/testing.skink` lines 62-66

**Problem:** `Skink_panic` and `Skink_test_failnow` call `abort()`, killing the whole process.

**Steps:**
1. Use `pthread_exit` or `longjmp` to a per-thread panic recovery point instead of `abort()`.
2. In the web server, ensure panics in a connection handler only terminate that connection thread.
3. For tests, allow `FailNow()` to stop the current test without killing the runner.
4. Add tests that verify panic recovery.

---

## P2 — Fix Before General Production Release

### 18. Add DoS limits to parsers
**Where:** `std/json.skink`, `std/xml.skink`, `std/yaml.skink`, `std/csv.skink`, `std/runtime/regex_rt.c`

**Problem:** Deep nesting, huge fields, and pathological regex patterns can exhaust stack, CPU, or memory.

**Steps:**
1. Add a configurable maximum nesting depth to JSON/XML/YAML parsers (e.g., 1000).
2. Add maximum field size, row count, and total document size limits to CSV/JSON/XML/YAML.
3. Add a regex execution timeout or backtracking limit in `regex_rt.c`.
4. Return explicit errors instead of crashing.
5. Add fuzz/DoS tests.

### 19. Fix JSON literal and number parsing
**Where:** `std/json.skink` lines 459-507

**Problem:** `parseTrue`/`parseFalse`/`parseNull` do not validate characters, and `parseNumber` drops the fractional part.

**Steps:**
1. In each literal parser, verify the exact expected bytes before consuming them.
2. Use `strconv.ParseFloat` for JSON numbers, preserving decimals and exponents.
3. Add tests for malformed literals and non-integer numbers.

### 20. Fix CSV character handling
**Where:** `std/csv.skink` lines 70-99

**Problem:** The parser builds one-character strings by taking the address of loop variables and casting to `string`.

**Steps:**
1. Accumulate fields as byte slices/string builders instead of per-character `str.Concat`.
2. Use `str.Substr(self.data, self.pos, 1)` for a single character, not `string(int64(&c))`.
3. Add CSV tests with quotes, escapes, and multi-byte characters.

### 21. Fix buffer overflow in HTTP helpers
**Where:** `std/net.skink` lines 168-222; `std/api.skink` lines 102-115, 151-164, 208-218, 253-266

**Problem:** `HTTPGet`/`HTTPPost` use fixed 8192-byte buffers without bounds checking; `api.Client` grows buffers without a maximum size.

**Steps:**
1. In `net.skink`, grow the buffer like `api.skink` does, but cap growth at a configurable maximum (default e.g., 10 MB).
2. In `api.skink`, add the same maximum-size cap and return an error when exceeded.
3. Add tests for responses larger than the cap and for exact buffer-boundary sizes.

### 22. Fix integer overflow in string/buffer helpers
**Where:** `std/str.skink` lines 260-264; `std/buf.skink` lines 157-167; `std/strconv.skink` lines 54-65, 161-174

**Problem:** `str.Repeat`, `buf.ensureCap`, and `strconv` can overflow integer calculations.

**Steps:**
1. In `str.Repeat`, check `n * count` for overflow before allocating.
2. In `buf.ensureCap`, check `self.len + extra + 1` for overflow.
3. In `strconv.Itoa`, handle `INT_MIN` specially (e.g., use a precomputed string or widen to `int64`).
4. In `strconv.ParseInt`, detect overflow and return `(0, false)`.

### 23. Implement DNS resolution in network client
**Where:** `std/net.skink` lines 74-99

**Problem:** `Connect` uses `inet_addr`, which only accepts dotted-quad IP strings.

**Steps:**
1. Add a runtime helper wrapping `getaddrinfo`.
2. Use it in `net.Connect` so hostnames resolve correctly.
3. Add tests for both hostname and IP address connections.

### 24. Add connection timeouts to sockets
**Where:** `std/net.skink` lines 74-99; `std/runtime/net_rt.c`

**Problem:** `Connect` has no timeout, allowing indefinite hangs.

**Steps:**
1. Set `SO_SNDTIMEO`/`SO_RCVTIMEO` before `connect`, or use non-blocking sockets with `select`/`poll`.
2. Surface timeout errors through the `errors.Error` return value.
3. Add tests against a non-routable IP or a deliberately slow server.

### 25. Strengthen type checker soundness
**Where:** `std/checker.skink` lines 1031-1049, 1515-1528; `std/emitter.skink` lines 1342-1357

**Problem:** Generic parameters are treated as mutually assignable, module vars can silently become `any`, and map/slice-of-map detection is string-pattern-based.

**Steps:**
1. Only allow assignment between identical generic parameter names or through an explicit interface bound.
2. Make module-level `var` declarations without a type annotation and without an initializer a compile-time error instead of defaulting to `any`.
3. Replace the `**` string suffix check with a proper type-tag check from the type checker.
4. Add negative type-checker tests for each case.

### 26. Improve parser error recovery
**Where:** `std/parser.skink` lines 311-313, 1853-1856

**Problem:** On unexpected tokens, the parser advances one token and returns an empty node, leading to cascading errors.

**Steps:**
1. Implement synchronization: skip tokens until a known boundary (`;`, `}`, top-level keyword, etc.).
2. Distinguish between resynchronizable and fatal parse errors.
3. Add tests with multiple independent errors to verify that each is reported once.

### 27. Implement a cycle collector or document the limitation
**Where:** `std/runtime/rc_rt.c`

**Problem:** Reference counting alone leaks cyclic structures.

**Steps:**
1. Either implement a cycle collector (e.g., trial deletion or mark-and-sweep backup), or
2. Document that cyclic references must be broken manually and provide examples.

### 28. Make HashMap scalable
**Where:** `std/hashmap.skink`

**Problem:** HashMap uses linear probing in a flat array with no separate capacity; every insert reallocates and copies.

**Steps:**
1. Add `capacity` and `count` fields.
2. Use a load factor (e.g., 0.7) and resize only when exceeded.
3. Consider robin-hood hashing or separate chaining.
4. Add performance benchmarks with large maps.

### 29. Make quicksort robust
**Where:** `std/sort.skink` lines 36-51

**Problem:** Always choosing the last element as pivot degrades to `O(n²)` on sorted input.

**Steps:**
1. Use randomized pivot selection or median-of-three.
2. Add an iteration/depth limit to detect worst-case behavior.
3. Add tests with sorted, reverse-sorted, and adversarial inputs.

### 30. Keep `std/` and `bootstrap-home/std/` in sync
**Where:** `Makefile`, `materialize_bootstrap_home.sh`

**Problem:** The compiler is bootstrapped from `bootstrap-home/std/`, which can diverge from `std/`.

**Steps:**
1. Add a CI check that verifies `std/` and `bootstrap-home/std/` are identical (except generated artifacts).
2. Or, change the build to copy `std/` into `bootstrap-home/std/` before stage-0 compilation.
3. Run `make stage2` in CI to verify self-hosting after every change.

---

## Verification Checklist

Before considering production use:

- [x] All P0 items are implemented and tested.
- [x] TLS/HTTPS client validates certificates and hostnames.
- [x] Crypto uses authenticated encryption and modern padding.
- [x] Database API supports parameterized queries.
- [x] Runtime allocations are checked for failure.
- [x] Slice expressions and `**` operator produce correct results.
- [x] Generic containers work correctly with `int`, `string`, and pointer keys.
- [x] Map keys are retained correctly.
- [x] Multi-threaded web server handles concurrent HTTP and HTTPS load.
- [x] Panics in a thread do not kill the whole process.
- [x] Parsers reject/timeout on maliciously large or deeply nested input.
- [ ] Fuzz testing passes for JSON, XML, YAML, CSV, regex, and network input.
- [x] `make stage2` succeeds and the resulting compiler passes the full test suite.
