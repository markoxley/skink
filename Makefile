# Makefile for skink-pure (Self-Hosted Skink Compiler)
# Usage:
#   make              - build the self-hosted skink compiler binary
#   make stage2       - build stage 2 compiler using stage 1 to verify self-hosting
#   make install      - install the pure compiler and standard library to system
#   make install-only - install pre-built compiler and standard library
#   make uninstall    - remove compiler and standard library from system
#   make clean        - remove build artifacts
#   make help         - show this help message

# Installation directories (override with make PREFIX=/custom/path)
PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
LIBDIR      ?= $(PREFIX)/lib/skink

# Compiler and tool settings
SKINK_BIN   := skink
STAGE0      ?= $(shell command -v skink 2>/dev/null || echo "./skink")
CLANG       ?= clang

# Version file handling
VERSION_FILE := version.txt
VERSION_SKINK := version.skink

# Read the current version for display and seeding
VERSION     ?= $(shell cat $(VERSION_FILE) 2>/dev/null || echo "0.1.7")

# Helper: use sudo if not running as root
SUDO        := $(shell if [ `id -u` -ne 0 ]; then echo sudo; fi)

# Source directories
BOOTSTRAP_HOME ?= ./bootstrap-home
STD_DIR        ?= $(BOOTSTRAP_HOME)/std
RUNTIME_DIR    ?= $(BOOTSTRAP_HOME)/runtime

# Standard library and runtime files to install
STD_FILES     := $(shell find $(STD_DIR) -maxdepth 2 -name '*.skink' ! -path '$(STD_DIR)/runtime/*')
RUNTIME_FILES := $(wildcard $(RUNTIME_DIR)/*.c)
RUNTIME_HEADERS := $(wildcard $(RUNTIME_DIR)/*.h)
RUNTIME_LL    := $(wildcard $(RUNTIME_DIR)/*.ll)
# Vendored headers (e.g. runtime/vendor/nuklear*.h) are included by
# ui_rt.c as "vendor/..." — they must install under runtime/vendor/.
RUNTIME_VENDOR := $(wildcard $(RUNTIME_DIR)/vendor/*.h)

# Default goal must be set before the first concrete target
.DEFAULT_GOAL := all

# Create an initial version.txt if it does not exist
$(VERSION_FILE):
	@echo "0.1.7" > $@

# Generate the Skink version module from version.txt
$(VERSION_SKINK): $(VERSION_FILE)
	@echo '// Copyright 2026 Mark Oxley. All rights reserved.' > $@
	@echo '//' >> $@
	@echo '// Project: Skink — A self-hosted, statically-typed programming language' >> $@
	@echo '//          targeting LLVM IR, designed for systems programming with' >> $@
	@echo '//          modern ergonomics and a Go-inspired syntax.' >> $@
	@echo '//' >> $@
	@echo '// File:    version.skink' >> $@
	@echo '// Purpose: Generated module that exports the current Skink version string.' >> $@
	@echo '// Author:  Mark Oxley' >> $@
	@echo '// Created: 2026' >> $@
	@echo '//' >> $@
	@echo '// Licensed under the Apache License, Version 2.0 (the "License");' >> $@
	@echo '// you may not use this file except in compliance with the License.' >> $@
	@echo '// You may obtain a copy of the License at' >> $@
	@echo '//' >> $@
	@echo '//     http://www.apache.org/licenses/LICENSE-2.0' >> $@
	@echo '//' >> $@
	@echo '// Unless required by applicable law or agreed to in writing, software' >> $@
	@echo '// distributed under the License is distributed on an "AS IS" BASIS,' >> $@
	@echo '// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.' >> $@
	@echo '// See the License for the specific language governing permissions and' >> $@
	@echo '// limitations under the License.' >> $@
	@echo '' >> $@
	@echo 'module version' >> $@
	@echo '' >> $@
	@echo 'pub const Version = "$(VERSION)"' >> $@

# Bump the build component of the version
bump-version: $(VERSION_FILE)
	@awk -F. '{print $$1"."$$2"."($$3+1)}' $(VERSION_FILE) > $(VERSION_FILE).tmp && mv $(VERSION_FILE).tmp $(VERSION_FILE)

.PHONY: all build stage1 stage2 install install-only install-deps uninstall clean help release dist check-bootstrap-sync sync-bootstrap

# Default target: build stage 1 self-hosted compiler
all: build

# Install system dependencies (compiler toolchain and stdlib C libraries)
install-deps:
	@echo "Detecting operating system and package manager..."
	@OS=$$(uname -s); \
	if [ "$$OS" = "Linux" ]; then \
		if command -v dnf >/dev/null 2>&1; then \
			echo "Detected: dnf (Fedora/RHEL)"; \
			$(SUDO) dnf install -y clang make openssl-devel zlib-devel sqlite-devel; \
		elif command -v apt-get >/dev/null 2>&1; then \
			echo "Detected: apt (Debian/Ubuntu)"; \
			$(SUDO) apt-get update && $(SUDO) apt-get install -y clang make libssl-dev zlib1g-dev libsqlite3-dev; \
		elif command -v pacman >/dev/null 2>&1; then \
			echo "Detected: pacman (Arch Linux)"; \
			$(SUDO) pacman -S --needed --noconfirm clang make openssl zlib sqlite; \
		elif command -v zypper >/dev/null 2>&1; then \
			echo "Detected: zypper (openSUSE)"; \
			$(SUDO) zypper install -y clang make libopenssl-devel zlib-devel sqlite3-devel; \
		fi; \
	elif [ "$$OS" = "Darwin" ]; then \
		if command -v brew >/dev/null 2>&1; then \
			echo "Detected: Homebrew (macOS)"; \
			brew install openssl zlib sqlite; \
		fi; \
	fi

build: stage1

# Stage 1: Build the pure Skink compiler driver using stage 0
stage1: $(SKINK_BIN)

$(SKINK_BIN): skink.skink $(STD_FILES) $(RUNTIME_FILES) $(VERSION_SKINK)
	@echo "Building Stage 1 self-hosted skink compiler..."
	SKINK_HOME=$(BOOTSTRAP_HOME) $(STAGE0) -o $(SKINK_BIN) skink.skink
	@echo "Built: $(SKINK_BIN)"

# Stage 2: Compile skink.skink using the Stage 1 binary (full self-hosting verification)
stage2: bump-version | $(SKINK_BIN)
	@echo "Regenerating version module..."
	@$(MAKE) $(VERSION_SKINK)
	@echo "Building Stage 2 self-hosted skink compiler with Stage 1 binary..."
	SKINK_HOME=$(BOOTSTRAP_HOME) ./$(SKINK_BIN) -o $(SKINK_BIN)_stage2 skink.skink
	@echo "Verifying Stage 2 binary..."
	SKINK_HOME=$(BOOTSTRAP_HOME) ./$(SKINK_BIN)_stage2 -check skink.skink
	@cp -f $(SKINK_BIN)_stage2 $(SKINK_BIN)
	@rm -f $(SKINK_BIN)_stage2
	@echo "Stage 2 self-hosted compiler verified and updated: $(SKINK_BIN)"

# Install compiler binary, standard library, and C runtime
install: build
	@echo "Installing pure skink compiler to $(BINDIR)..."
	$(SUDO) install -d $(BINDIR)
	$(SUDO) cp -f $(SKINK_BIN) $(BINDIR)/
	$(SUDO) chmod 755 $(BINDIR)/$(SKINK_BIN)
	$(SUDO) cp -f skink_test.sh $(BINDIR)/
	$(SUDO) chmod 755 $(BINDIR)/skink_test.sh
	$(SUDO) cp -f skink_bench.sh $(BINDIR)/
	$(SUDO) chmod 755 $(BINDIR)/skink_bench.sh
	@echo "Installing standard library to $(LIBDIR)..."
	$(SUDO) install -d $(LIBDIR)
	$(SUDO) rm -rf $(LIBDIR)/std $(LIBDIR)/runtime
	$(SUDO) install -d $(LIBDIR)/std
	@for f in $(STD_FILES); do \
	    rel=$$(echo $$f | sed 's|^$(STD_DIR)/||'); \
	    target=$(LIBDIR)/std/$$rel; \
	    $(SUDO) install -d $$(dirname $$target); \
	    $(SUDO) install -m 644 $$f $$target; \
	done
	@echo "Installing runtime support to $(LIBDIR)/runtime..."
	$(SUDO) install -d $(LIBDIR)/runtime
	$(SUDO) install -m 644 $(RUNTIME_FILES) $(LIBDIR)/runtime/
ifneq ($(RUNTIME_HEADERS),)
	$(SUDO) install -m 644 $(RUNTIME_HEADERS) $(LIBDIR)/runtime/
endif
ifneq ($(RUNTIME_LL),)
	$(SUDO) install -m 644 $(RUNTIME_LL) $(LIBDIR)/runtime/
endif
ifneq ($(RUNTIME_VENDOR),)
	$(SUDO) install -d $(LIBDIR)/runtime/vendor
	$(SUDO) install -m 644 $(RUNTIME_VENDOR) $(LIBDIR)/runtime/vendor/
endif
	@echo ""
	@echo "Installation complete: pure Skink compiler installed at $(BINDIR)/$(SKINK_BIN)."
	@echo "Standard library & runtime installed at $(LIBDIR)."

# Install pre-built compiler and stdlib without re-compiling
install-only:
	@echo "Installing pre-built skink to $(BINDIR)..."
	$(SUDO) install -d $(BINDIR)
	$(SUDO) cp -f $(SKINK_BIN) $(BINDIR)/
	$(SUDO) chmod 755 $(BINDIR)/$(SKINK_BIN)
	$(SUDO) cp -f skink_test.sh $(BINDIR)/
	$(SUDO) chmod 755 $(BINDIR)/skink_test.sh
	$(SUDO) cp -f skink_bench.sh $(BINDIR)/
	$(SUDO) chmod 755 $(BINDIR)/skink_bench.sh
	@echo "Installing standard library to $(LIBDIR)..."
	$(SUDO) install -d $(LIBDIR)
	$(SUDO) rm -rf $(LIBDIR)/std $(LIBDIR)/runtime
	$(SUDO) install -d $(LIBDIR)/std
	@for f in $(STD_FILES); do \
	    rel=$$(echo $$f | sed 's|^$(STD_DIR)/||'); \
	    target=$(LIBDIR)/std/$$rel; \
	    $(SUDO) install -d $$(dirname $$target); \
	    $(SUDO) install -m 644 $$f $$target; \
	done
	@echo "Installing runtime support to $(LIBDIR)/runtime..."
	$(SUDO) install -d $(LIBDIR)/runtime
	$(SUDO) install -m 644 $(RUNTIME_FILES) $(LIBDIR)/runtime/
ifneq ($(RUNTIME_HEADERS),)
	$(SUDO) install -m 644 $(RUNTIME_HEADERS) $(LIBDIR)/runtime/
endif
ifneq ($(RUNTIME_LL),)
	$(SUDO) install -m 644 $(RUNTIME_LL) $(LIBDIR)/runtime/
endif
ifneq ($(RUNTIME_VENDOR),)
	$(SUDO) install -d $(LIBDIR)/runtime/vendor
	$(SUDO) install -m 644 $(RUNTIME_VENDOR) $(LIBDIR)/runtime/vendor/
endif

# Uninstall compiler and stdlib
uninstall:
	@echo "Removing $(BINDIR)/$(SKINK_BIN)..."
	$(SUDO) rm -f $(BINDIR)/$(SKINK_BIN)
	$(SUDO) rm -f $(BINDIR)/skink_test.sh
	$(SUDO) rm -f $(BINDIR)/skink_bench.sh
	@echo "Removing $(LIBDIR)..."
	$(SUDO) rm -rf $(LIBDIR)
	@echo "Uninstall complete."

# Remove build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(SKINK_BIN) $(SKINK_BIN)_stage2 *.o *.ll $(VERSION_SKINK)
	rm -rf dist

# Build a distributable release archive
release:
	@./release.sh

# Alias for release
dist: release

# Check that std/ and bootstrap-home/std/ are in sync.
# The bootstrap compiler uses bootstrap-home/std/ to build the stage-1
# compiler. If std/ and bootstrap-home/std/ diverge, the bootstrap compiler
# and the stage-2 compiler can produce different results. This target
# verifies they match. Run `make sync-bootstrap` to fix any divergence.
#
# We only compare .skink source files (excluding build artifacts like
# __skink_test_bin and checker_installed.skink which are generated during
# testing and should not be tracked).
check-bootstrap-sync:
	@echo "Checking std/ vs bootstrap-home/std/ sync..."
	@diff -rq std/ bootstrap-home/std/ --exclude='__skink_test_bin' --exclude='checker_installed.skink' --exclude='*.o' --exclude='*.ll' && \
		echo "std/ and bootstrap-home/std/ are in sync." || \
		(echo "ERROR: std/ and bootstrap-home/std/ are out of sync." && \
		 echo "Run 'make sync-bootstrap' to copy std/ to bootstrap-home/std/." && exit 1)
	@echo "Checking std/runtime/ vs runtime/ and bootstrap-home/runtime/ sync..."
	@diff -rq std/runtime/ runtime/ && \
		diff -rq std/runtime/ bootstrap-home/runtime/ && \
		echo "runtime copies are in sync." || \
		(echo "ERROR: runtime copies are out of sync." && \
		 echo "Run 'make sync-bootstrap' to propagate std/runtime/." && exit 1)

# Sync std/ and std/runtime/ to bootstrap-home/std/ and bootstrap-home/runtime/.
# Per AGENTS.md: changes to std/*.skink must be copied to bootstrap-home/std/,
# and changes to std/runtime/ must be copied to bootstrap-home/runtime/.
# The top-level runtime/ is what the built compiler links when SKINK_HOME is
# the repo root — it must mirror std/runtime/ or test binaries silently link
# a stale runtime.
sync-bootstrap:
	@echo "Syncing std/ to bootstrap-home/std/..."
	@cp -r std/. bootstrap-home/std/
	@cp -r std/runtime/* bootstrap-home/runtime/
	@cp -r std/runtime/* runtime/
	@echo "Sync complete."

help:
	@echo "Skink-Pure Build System (v$(VERSION))"
	@echo "Targets:"
	@echo "  make              - Build self-hosted skink binary"
	@echo "  make stage2       - Self-compile with stage 1 compiler"
	@echo "  make install      - Install pure compiler, stdlib, and runtime"
	@echo "  make install-only - Install without rebuilding"
	@echo "  make uninstall    - Uninstall from system"
	@echo "  make release      - Build distributable archive (dist/)"
	@echo "  make check-bootstrap-sync - Verify std/ and bootstrap-home/std/ match"
	@echo "  make sync-bootstrap - Copy std/ to bootstrap-home/std/"
	@echo "  make clean        - Remove build artifacts and dist/"
