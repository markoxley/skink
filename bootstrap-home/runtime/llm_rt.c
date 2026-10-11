// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/llm_rt.c
// Purpose: Stubs for the LLM runtime.
// Author:  Mark Oxley
// Created: 2026
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Stubs for the LLM runtime.  These let the std/llm_test suite fail
// gracefully when no real llama.cpp backend is available.

#include <stdint.h>

// last_error holds the most recent LLM backend error message.
static const char *last_error = "llm backend not available";

// Skink_llm_load_model is a stub that always returns 0 (no model loaded).
int64_t Skink_llm_load_model(const char *path, int n_ctx) {
  (void)path;
  (void)n_ctx;
  return 0;
}

// Skink_llm_generate is a stub that always returns an empty string.
char *Skink_llm_generate(int64_t handle, const char *prompt, int max_tokens) {
  (void)handle;
  (void)prompt;
  (void)max_tokens;
  return (char *)"";
}

// Skink_llm_embeddings is a stub that always returns NULL with zero length.
float *Skink_llm_embeddings(int64_t handle, const char *text, int *outLen) {
  (void)handle;
  (void)text;
  *outLen = 0;
  return 0;
}

// Skink_llm_free is a stub that does nothing (no model to free).
void Skink_llm_free(int64_t handle) { (void)handle; }

// Skink_llm_last_error returns the most recent LLM backend error message.
char *Skink_llm_last_error(void) { return (char *)last_error; }
