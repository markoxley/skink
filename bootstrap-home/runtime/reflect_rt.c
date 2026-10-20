// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/reflect_rt.c
// Purpose: C runtime support for reflect rt.
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

#include <stdint.h>

// ---------------------------------------------------------------------------
// Safe string detection for erased generic values.
//
// Inside a generic container (e.g. HashMap<K,V>), type parameters are erased
// to i8* and reflect.TypeOf<K> returns "K", not the concrete type.  This
// means reflect_hash/reflect_equals cannot tell whether an erased i8* value
// is a string pointer or an integer cast via inttoptr.
//
// We use a page-mapping check (mincore on POSIX, VirtualQuery on Windows) to
// safely determine whether the value points to readable memory.  Integer
// values (especially i32 inttoptr) typically fall in unmapped low addresses,
// while string pointers (heap or .rodata) are always in mapped pages.
//
// This is consistent: the same integer value always produces the same hash
// (it either maps to the same readable page or doesn't), so lookups remain
// correct even when an integer value happens to land in a mapped page.
// ---------------------------------------------------------------------------

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>

// skink_is_mapped checks if a pointer points to readable memory using mincore (POSIX).
static int skink_is_mapped(const void *p) {
  static long ps = 0;
  if (ps == 0)
    ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0)
    return 0;
  uintptr_t page_mask = ~((uintptr_t)ps - 1);
  void *page = (void *)((uintptr_t)p & page_mask);
  // 1-entry cache to avoid redundant syscalls for same-page accesses.
  // Kept in thread-local storage: spawn routines call reflect_hash /
  // reflect_equals concurrently, and a shared static pair would race
  // (a thread could observe cached_page from another thread's update
  // while reading its own stale cached_result).
  static __thread void *cached_page = NULL;
  static __thread int cached_result = 0;
  if (page == cached_page)
    return cached_result;
  unsigned char vec;
  int r = (mincore(page, (size_t)ps, &vec) == 0) ? 1 : 0;
  cached_page = page;
  cached_result = r;
  return r;
}
#elif defined(_WIN32)
#include <windows.h>

// skink_is_mapped checks if a pointer points to committed memory using VirtualQuery (Windows).
static int skink_is_mapped(const void *p) {
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
    return 0;
  return (mbi.State == MEM_COMMIT) ? 1 : 0;
}
#else
// skink_is_mapped is a fallback that always returns 0 (treats all values as integers).
static int skink_is_mapped(const void *p) {
  (void)p;
  return 0; /* fallback: treat all values as integers */
}
#endif

// Returns 1 if typeName is exactly "string".
static int skink_is_string_type(const char *typeName) {
  return typeName && typeName[0] == 's' && typeName[1] == 't' &&
         typeName[2] == 'r' && typeName[3] == 'i' && typeName[4] == 'n' &&
         typeName[5] == 'g' && typeName[6] == '\0';
}

// Hash a NUL-terminated C string (djb2-like, matches the old algorithm).
// Made public so the emitter can call it for hash-based map lookups.
int Skink_str_hash(const char *s) {
  if (!s)
    return 0;
  int hash = 0;
  for (int i = 0; s[i]; i++)
    hash = hash * 31 + s[i];
  return hash;
}

static int skink_hash_string(const char *s) {
  return Skink_str_hash(s);
}

// Null-safe string equality: returns 1 if equal, 0 if not.
static int skink_streq(const char *sa, const char *sb) {
  while (*sa && *sb) {
    if (*sa != *sb)
      return 0;
    sa++;
    sb++;
  }
  return *sa == *sb;
}

// Heuristic: does the erased i8* value look like a readable string pointer?
static int skink_looks_like_string(const void *p) {
  if (!p)
    return 0;
  return skink_is_mapped(p);
}

// reflect_get_bool reads a boolean value at the given offset from a struct address.
int reflect_get_bool(uintptr_t addr, int offset) {
  return *((uint8_t *)(addr + offset)) != 0;
}

// reflect_set_bool writes a boolean value at the given offset in a struct address.
void reflect_set_bool(uintptr_t addr, int offset, int val) {
  *((uint8_t *)(addr + offset)) = val ? 1 : 0;
}

// reflect_get_int reads an int value at the given offset from a struct address.
int reflect_get_int(uintptr_t addr, int offset) {
  return *(int *)(addr + offset);
}

// reflect_set_int writes an int value at the given offset in a struct address.
void reflect_set_int(uintptr_t addr, int offset, int val) {
  *(int *)(addr + offset) = val;
}

// reflect_get_int64 reads an int64 value at the given offset from a struct address.
int64_t reflect_get_int64(uintptr_t addr, int offset) {
  return *(int64_t *)(addr + offset);
}

// reflect_set_int64 writes an int64 value at the given offset in a struct address.
void reflect_set_int64(uintptr_t addr, int offset, int64_t val) {
  *(int64_t *)(addr + offset) = val;
}

// reflect_get_string reads a string pointer at the given offset from a struct address.
const char *reflect_get_string(uintptr_t addr, int offset) {
  return *(const char **)(addr + offset);
}

// reflect_set_string writes a string pointer at the given offset in a struct address.
void reflect_set_string(uintptr_t addr, int offset, const char *val) {
  *(const char **)(addr + offset) = val;
}

// reflect_get_float reads a double value at the given offset from a struct address.
double reflect_get_float(uintptr_t addr, int offset) {
  return *(double *)(addr + offset);
}

// reflect_set_float writes a double value at the given offset in a struct address.
void reflect_set_float(uintptr_t addr, int offset, double val) {
  *(double *)(addr + offset) = val;
}

// reflect_hash computes a hash for a value using type-aware heuristics for erased generics.
int reflect_hash(void *addrPtr, const char *typeName) {
  // Explicit string type: hash string content directly.
  if (skink_is_string_type(typeName))
    return skink_hash_string((const char *)addrPtr);
  // Generic type parameter: use heuristic to detect string pointers.
  // If the erased i8* points to mapped memory, treat it as a string and
  // hash its content; otherwise hash the raw integer value.
  if (skink_looks_like_string(addrPtr))
    return skink_hash_string((const char *)addrPtr);
  // Integer key: hash the raw pointer/integer value.
  int64_t addr = (int64_t)(uintptr_t)addrPtr;
  unsigned int raw = (unsigned int)(addr ^ (addr >> 32));
  return (int)(raw & 0x7fffffff);
}

// reflect_equals compares two values for equality using type-aware heuristics for erased generics.
int reflect_equals(void *a, void *b, const char *typeName) {
  if (a == b)
    return 1;
  if (!a || !b)
    return 0;
  // Explicit string type: compare content via strcmp.
  if (skink_is_string_type(typeName))
    return skink_streq((const char *)a, (const char *)b);
  // Generic type parameter: use heuristic. If both values point to mapped
  // memory, compare as strings (content equality). Otherwise they are
  // different integers (a != b already checked) and thus not equal.
  if (skink_looks_like_string(a) && skink_looks_like_string(b))
    return skink_streq((const char *)a, (const char *)b);
  return 0;
}
