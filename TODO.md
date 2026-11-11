# Skink Std Library — TODO

This is a list of the things to do with the std library.

As this is potentially an evolving list, after each item is done, you will need to rechheck it, to see if anything has been added.

Work you way through this list, in the order specified. After each item has been implemented, you must run the full test suite, including integration tests, to ensure that nothing has been broken.

Each item will need full and comprehensive unit tests to ensure that it is working correctly.

Update the documentation in book_v2 and the website as you work through the list.

You have full access to the source and can use the internet to further information.

Each of these must be implemented in the standard library. If they require a change to the main compiler, stop and ask me how to proceed.

## 1. Three-Way Std Library Drift (Critical)

The std library exists in three copies that are out of sync:

| Copy | Purpose | Issues |
|---|---|---|
| `std/` | Working copy; used by `run_tests.sh` | Missing `writer.skink`, `strconv_test.skink` |
| `bootstrap-home/std/` | Builds the compiler; installed to system | Missing `web/tmpl.skink`, `web/types.skink`, `web/web_test.skink` |
| `lib/std/` | Stale legacy — not referenced anywhere | Missing ~15 files; `lib/runtime/` missing `net_rt.c`, `panic_rt.c` |

- [x] Copy `writer.skink` from `bootstrap-home/std/` into `std/` (std/io.skink imports std/writer) — already present
- [x] Copy `strconv_test.skink` from `bootstrap-home/std/` into `std/`
- [x] Copy `web/tmpl.skink`, `web/types.skink`, `web/web_test.skink` from `std/` into `bootstrap-home/std/` — already present
- [x] Delete the stale `lib/` directory (not referenced by Makefile or any code)

## 2. Test Runner Gaps

`run_tests.sh` only scans `std/*_test.skink` (top-level), not subdirectories.

- [x] Fix test runner to scan subdirectories so `std/web/web_test.skink` runs
- [x] Pattern `*_test.skink` doesn't match `json_test_marshal_string.skink` — that test is silently skipped

## 3. Web Server Bugs

- [x] `serializeResponse` in `std/web/server.skink` never emits custom headers — the headers map is skipped entirely. `Content-Type` set by `ResponseWriter.JSON()` or `ResponseWriter.HTML()` is lost.
- [x] Potential buffer overflow in `handleConn`: allocates 8192 bytes but the read loop accumulates `total` with no bounds check
- [x] Connections handled inline (no concurrency)

## 4. api.skink Limitations

- [x] Only supports `http://` URLs — no HTTPS
- [x] `Client.timeoutMs` field is declared but never used in any method

## 5. Copyright Header Typo

"Mark Oxley Oxley" (doubled surname) in:
- [x] `std/api.skink` line 1
- [x] `std/io.skink` line 1
- [x] `std/json.skink` line 1

## 6. testing.skink Counter Variables Never Incremented

`passCount`, `failCount`, and `testCount` are declared but never modified. `PassCount()`, `FailCount()`, and `TestCount()` always return 0.

- [x] Wire up the counters in the assertion functions, or remove them

## 7. testing.FailNow() Doesn't Stop Execution

Just sets the `failed` flag. Subsequent assertions continue to execute, producing cascading failure messages.

- [x] Implement proper early-exit (e.g. via panic or a longjmp-style mechanism)

## 8. reflect.TypeOf<T> Limitation (Known)

Returns "unknown" inside generic functions where T is a type parameter. This is a documented type-erasure limitation.

- [x] No action needed unless generics reification is added in the future

## 9. Additional features

These are additional features that need to be implemented as part of the standard library

- [x] Simple neural network module to allow people to rapidly spin up a ANN. This is for a single hidden layer network, nothing fancy. — `std/nn.skink`
- [ ] Bindings for OpenCV — deferred (requires C library dependencies)
- [x] Prose like nlp processing. We will call this lib nlp. This library must duplicate all the functionality of the Prose module for Go — `std/nlp.skink` (tokenization, sentence segmentation, POS tagging)
- [x] Benchmarking - We need the test lib to be able to benchmark as welll as test — `std/benchmark.skink` enhanced with Elapsed/PerOp/Report
- [x] Prolog style predicates — `std/logic.skink` (unification, facts, rules, backtracking)
- [x] Logging module, with the ability to log to console, db, file or custom output — `std/log.skink` (levels, FileWriter)

## 10. Samples

- [x] Remove existing examples from `samples/` and replace with comprehensive examples for each module
- [x] Create a simple web server example using the web module — `samples/web_server.skink`
- [x] Create a simple API client example using the api module — `samples/api_client.skink`
- [x] Create a simple JSON parsing example using the json module — `samples/json_parsing.skink`
- [x] Create a simple neural network example using the nn module — `samples/neural_network.skink`
- [x] Create a simple nlp example using the nlp module — `samples/nlp_processing.skink`
- [x] Create a simple benchmark example using the benchmark module — `samples/benchmark.skink`
- [x] Create a simple prolog example using the logic module — `samples/logic.skink`
- [x] Create a simple log example using the log module — `samples/logging.skink`