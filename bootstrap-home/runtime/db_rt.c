// Copyright 2026 Mark Oxley. All rights reserved.
//
// Project: Skink — A self-hosted, statically-typed programming language
//          targeting LLVM IR, designed for systems programming with
//          modern ergonomics and a Go-inspired syntax.
//
// File:    std/runtime/db_rt.c
// Purpose: C runtime support for db rt.
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

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>

/* Skink_res_track / Skink_res_untrack (rc_rt.c) register native-resource
 * blocks so heap scanners treat them as reachable and the process-exit
 * sweep destroys them.  sqlite3 handles surface to Skink as int64 fields in
 * by-value Connection/Statement structs, so no single owner exists to close
 * them deterministically (comp_error.md #103). */
extern void Skink_res_track(void *ptr, void (*dtor)(void *));
extern void Skink_res_untrack(void *ptr);

/* skink_db_dtor closes a still-tracked connection during the exit sweep.
 * sqlite3_close_v2 tolerates unfinalised statements (the connection becomes
 * a zombie freed once they are), so it is safe regardless of sweep order
 * relative to tracked statements. */
static void skink_db_dtor(void *p) { sqlite3_close_v2((sqlite3 *)p); }

/* skink_stmt_dtor finalises a still-tracked statement during the exit
 * sweep. */
static void skink_stmt_dtor(void *p) { sqlite3_finalize((sqlite3_stmt *)p); }

// Open a SQLite database.  Returns an opaque handle (cast sqlite3*) or 0 on
// error.
int64_t Skink_db_open(const char *filename) {
  sqlite3 *db;
  if (sqlite3_open(filename, &db) != SQLITE_OK) {
    if (db) {
      sqlite3_close(db);
    }
    return 0;
  }
  Skink_res_track(db, skink_db_dtor);
  return (int64_t)db;
}

// Close a database connection.  Returns 0 on success, -1 on error.
// Untracks the handle first so the exit sweep cannot double-close it.
int Skink_db_close(int64_t handle) {
  if (!handle) {
    return -1;
  }
  Skink_res_untrack((void *)(intptr_t)handle);
  int rc = sqlite3_close((sqlite3 *)handle);
  return rc == SQLITE_OK ? 0 : -1;
}

// Return the last error message for a connection.
const char *Skink_db_errmsg(int64_t handle) {
  if (!handle) {
    return "null database handle";
  }
  return sqlite3_errmsg((sqlite3 *)handle);
}

// Execute a non-query SQL statement.  Returns number of rows affected,
// or -1 on error.
int64_t Skink_db_exec(int64_t handle, const char *sql) {
  if (!handle) {
    return -1;
  }
  char *err = NULL;
  int rc = sqlite3_exec((sqlite3 *)handle, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) {
    if (err) {
      sqlite3_free(err);
    }
    return -1;
  }
  return sqlite3_changes((sqlite3 *)handle);
}

// Prepare a SQL statement.  Returns an opaque statement handle or 0 on error.
// The statement is tracked so the exit sweep finalises it if the caller
// never calls Skink_db_finalize.
int64_t Skink_db_prepare(int64_t handle, const char *sql) {
  if (!handle) {
    return 0;
  }
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2((sqlite3 *)handle, sql, -1, &stmt, NULL) !=
      SQLITE_OK) {
    return 0;
  }
  Skink_res_track(stmt, skink_stmt_dtor);
  return (int64_t)stmt;
}

// Step a prepared statement.  Returns SQLITE_ROW (100) while rows remain,
// SQLITE_DONE (101) when finished, or an error code otherwise.
int Skink_db_step(int64_t stmt) {
  if (!stmt) {
    return SQLITE_DONE;
  }
  return sqlite3_step((sqlite3_stmt *)stmt);
}

// Return the number of columns in the result set of a prepared statement.
int Skink_db_column_count(int64_t stmt) {
  if (!stmt) {
    return 0;
  }
  return sqlite3_column_count((sqlite3_stmt *)stmt);
}

// Return the text value of a column in the current row.
const char *Skink_db_column_text(int64_t stmt, int col) {
  if (!stmt) {
    return "";
  }
  const unsigned char *txt = sqlite3_column_text((sqlite3_stmt *)stmt, col);
  if (!txt) {
    return "";
  }
  return (const char *)txt;
}

// Return the name of a column in the result set.
const char *Skink_db_column_name(int64_t stmt, int col) {
  if (!stmt) {
    return "";
  }
  return sqlite3_column_name((sqlite3_stmt *)stmt, col);
}

// Finalize a prepared statement, releasing its resources.  Untracks the
// statement first so the exit sweep cannot finalise it twice.
void Skink_db_finalize(int64_t stmt) {
  if (stmt) {
    Skink_res_untrack((void *)(intptr_t)stmt);
    sqlite3_finalize((sqlite3_stmt *)stmt);
  }
}

// Bind a text value to a prepared statement parameter (1-indexed).
// Returns 0 on success, -1 on error.
int Skink_db_bind_text(int64_t stmt, int idx, const char *value) {
  if (!stmt || !value) {
    return -1;
  }
  int rc =
      sqlite3_bind_text((sqlite3_stmt *)stmt, idx, value, -1, SQLITE_TRANSIENT);
  return rc == SQLITE_OK ? 0 : -1;
}

// Bind an integer value to a prepared statement parameter (1-indexed).
// Returns 0 on success, -1 on error.
int Skink_db_bind_int(int64_t stmt, int idx, int value) {
  if (!stmt) {
    return -1;
  }
  int rc = sqlite3_bind_int((sqlite3_stmt *)stmt, idx, value);
  return rc == SQLITE_OK ? 0 : -1;
}

// Bind a 64-bit integer value to a prepared statement parameter (1-indexed).
// Returns 0 on success, -1 on error.
int Skink_db_bind_int64(int64_t stmt, int idx, int64_t value) {
  if (!stmt) {
    return -1;
  }
  int rc = sqlite3_bind_int64((sqlite3_stmt *)stmt, idx, value);
  return rc == SQLITE_OK ? 0 : -1;
}

// Bind a double value to a prepared statement parameter (1-indexed).
// Returns 0 on success, -1 on error.
int Skink_db_bind_double(int64_t stmt, int idx, double value) {
  if (!stmt) {
    return -1;
  }
  int rc = sqlite3_bind_double((sqlite3_stmt *)stmt, idx, value);
  return rc == SQLITE_OK ? 0 : -1;
}

// Bind a NULL value to a prepared statement parameter (1-indexed).
// Returns 0 on success, -1 on error.
int Skink_db_bind_null(int64_t stmt, int idx) {
  if (!stmt) {
    return -1;
  }
  int rc = sqlite3_bind_null((sqlite3_stmt *)stmt, idx);
  return rc == SQLITE_OK ? 0 : -1;
}

// Reset a prepared statement so it can be re-executed with new bindings.
// Returns 0 on success, -1 on error.
int Skink_db_reset(int64_t stmt) {
  if (!stmt) {
    return -1;
  }
  int rc = sqlite3_reset((sqlite3_stmt *)stmt);
  return rc == SQLITE_OK ? 0 : -1;
}
