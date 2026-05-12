# =============================================================================
#  ExtendedC Compiler -- Makefile
# =============================================================================

ifeq ($(origin CXX),default)
  ifneq ($(shell command -v clang++ 2>/dev/null),)
    CXX := clang++
  else ifneq ($(shell command -v g++ 2>/dev/null),)
    CXX := g++
  else
    $(error No C++ compiler found)
  endif
endif

ifeq ($(origin CC),default)
  CC := $(shell command -v clang 2>/dev/null || echo gcc)
endif

LLVM_CC := $(shell command -v clang-20 2>/dev/null || \
            command -v clang-18 2>/dev/null || \
            command -v clang   2>/dev/null)

PYTHON := $(shell command -v python3 2>/dev/null || echo python)

SRCDIR    := src
INCDIR    := include
BUILDDIR  := build
STDLIBDIR := stdlib
COMPDIR   := compiler
EXECDIR   := /exec

DRIVER    := $(BUILDDIR)/ec
STATICLIB := $(BUILDDIR)/libec.a
RUNTIME   := $(BUILDDIR)/runtime.o
IR        := $(BUILDDIR)/output.ll
PROGRAM   := $(BUILDDIR)/program

RUNTIME_BLOB_SRC := $(BUILDDIR)/runtime_blob.c
RUNTIME_BLOB_OBJ := $(BUILDDIR)/runtime_blob.o

LIB_SRCS := $(SRCDIR)/Lexer.cpp \
             $(SRCDIR)/Parser.cpp \
             $(SRCDIR)/TypeChecker.cpp \
             $(SRCDIR)/CodeGen.cpp \
             $(SRCDIR)/Compiler.cpp

SRCS := $(LIB_SRCS) $(SRCDIR)/main.cpp
HDRS := $(wildcard $(INCDIR)/*.h)

CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Wno-unused-parameter -I$(INCDIR)
RELFLAGS := -O2
DBGFLAGS := -g -O0 -fsanitize=address,undefined

LIB_OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(LIB_SRCS))

.PHONY: all release release-simple debug install install-system \
        run compile-file lex-port parser-port ir string clean install-llvm

all: release

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ── Per-file objects (for static library) ─────────────────────────────────────
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp $(HDRS) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(RELFLAGS) -c $< -o $@

# ── Static library ────────────────────────────────────────────────────────────
$(STATICLIB): $(LIB_OBJS) | $(BUILDDIR)
	ar rcs $@ $^
	@echo "Built: $@"

# ── Runtime object ────────────────────────────────────────────────────────────
$(RUNTIME): $(SRCDIR)/runtime.c | $(BUILDDIR)
	$(CC) -O2 -pthread -c $< -o $@
	@echo "Built: $@"

# ── Embed runtime.o as a C byte array ─────────────────────────────────────────
# Allows the compiler driver to write runtime.o to a temp path at link time
# so users never need to compile runtime.c themselves.
$(RUNTIME_BLOB_SRC): $(RUNTIME) | $(BUILDDIR)
	@echo "Embedding runtime.o..."
	@$(PYTHON) -c "\
data = open('$(RUNTIME)', 'rb').read(); \
lines = ['#include <stddef.h>', \
         '/* Embedded runtime.o */', \
         'const unsigned char ec_runtime_blob[] = {']; \
[lines.append('    ' + ', '.join(hex(b) for b in data[i:i+16]) + ',') \
    for i in range(0, len(data), 16)]; \
lines += ['};', 'const size_t ec_runtime_blob_size = %d;' % len(data)]; \
open('$@', 'w').write('\n'.join(lines) + '\n'); \
print('  Embedded %d bytes of runtime.o' % len(data))"

$(RUNTIME_BLOB_OBJ): $(RUNTIME_BLOB_SRC) | $(BUILDDIR)
	$(CC) -O2 -c $< -o $@

# ── Compiler driver (links static lib + embedded runtime) ─────────────────────
release: $(STATICLIB) $(RUNTIME_BLOB_OBJ) | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(RELFLAGS) \
	    $(SRCDIR)/main.cpp \
	    $(RUNTIME_BLOB_OBJ) \
	    $(STATICLIB) \
	    -o $(DRIVER)
	@echo "Built: $(DRIVER)"

# Fast rebuild without embedded runtime (for development)
release-simple: | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(RELFLAGS) -o $(DRIVER) $(SRCS)
	@echo "Built (simple): $(DRIVER)"

debug: | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(DBGFLAGS) -o $(BUILDDIR)/ec_debug $(SRCS)
	@echo "Built (debug): $(BUILDDIR)/ec_debug"

# ── Link IR using pre-built runtime ──────────────────────────────────────────
define link_ir
	@if [ -n "$(LLVM_CC)" ]; then \
		$(LLVM_CC) -O2 $(1) $(RUNTIME) -lm -o $(2); \
	else \
		echo "ERROR: clang not found. Run: sudo apt install clang"; exit 1; \
	fi
endef

# ── Targets ───────────────────────────────────────────────────────────────────
run: release $(RUNTIME)
	$(DRIVER) --sample -o $(IR) -I $(STDLIBDIR)
	$(call link_ir,$(IR),$(PROGRAM))
	@echo "────────────────────────────────────────"
	@$(PROGRAM)

compile-file: release $(RUNTIME)
	$(DRIVER) $(SRC) -o $(or $(OUT),$(IR)) -I $(STDLIBDIR)
	$(call link_ir,$(IR),$(PROGRAM))
	@echo "Built: $(PROGRAM)"

lex-port: release $(RUNTIME)
	$(DRIVER) $(COMPDIR)/Lexer.ec -o $(BUILDDIR)/lexer_port.ll -I $(STDLIBDIR)
	$(call link_ir,$(BUILDDIR)/lexer_port.ll,$(BUILDDIR)/lexer_port)
	@echo "Built: $(BUILDDIR)/lexer_port"

parser-port: release $(RUNTIME)
	$(DRIVER) $(COMPDIR)/Parser.ec $(COMPDIR)/Lexer.ec \
	    -o $(BUILDDIR)/parser_port.ll -I $(STDLIBDIR)
	$(call link_ir,$(BUILDDIR)/parser_port.ll,$(BUILDDIR)/parser_port)
	@echo "Built: $(BUILDDIR)/parser_port"

ir: release
	$(DRIVER) --sample -o $(IR) -I $(STDLIBDIR)
	@echo "IR: $(IR)  ($$(wc -c < $(IR)) bytes)"

string: release $(RUNTIME)
	$(DRIVER) --string '$(SRC)' -o $(IR) -I $(STDLIBDIR)
	$(call link_ir,$(IR),$(PROGRAM))
	@$(PROGRAM)

# ── Install to /exec/ ─────────────────────────────────────────────────────────
install: release $(RUNTIME)
	@mkdir -p $(EXECDIR)/stdlib $(EXECDIR)/include $(EXECDIR)/compiler
	@cp $(DRIVER)    $(EXECDIR)/ec
	@cp $(STATICLIB) $(EXECDIR)/libec.a
	@cp $(RUNTIME)   $(EXECDIR)/runtime.o
	@cp $(STDLIBDIR)/* $(EXECDIR)/stdlib/ 2>/dev/null || true
	@cp $(INCDIR)/*.h   $(EXECDIR)/include/
	@cp $(COMPDIR)/*.ec $(EXECDIR)/compiler/ 2>/dev/null || true
	@echo "Installed to $(EXECDIR):"
	@ls -lh $(EXECDIR)/ec $(EXECDIR)/libec.a $(EXECDIR)/runtime.o

install-system: install
	@ln -sf $(EXECDIR)/ec /usr/local/bin/ec
	@echo "Symlinked /usr/local/bin/ec"

clean:
	rm -rf $(BUILDDIR)

install-llvm:
	sudo apt install -y clang llvm

# ── Self-hosted compiler ports ────────────────────────────────────────────────
codegen-port: release $(RUNTIME)
	$(DRIVER) $(COMPDIR)/CodeGen.ec $(COMPDIR)/TypeChecker.ec \
	    $(COMPDIR)/Parser.ec $(COMPDIR)/Lexer.ec \
	    -o $(BUILDDIR)/codegen_port.ll -I $(STDLIBDIR)
	$(call link_ir,$(BUILDDIR)/codegen_port.ll,$(BUILDDIR)/codegen_port)
	@echo "Built: $(BUILDDIR)/codegen_port"

compiler-port: release $(RUNTIME)
	$(DRIVER) $(COMPDIR)/Compiler.ec $(COMPDIR)/CodeGen.ec \
	    $(COMPDIR)/TypeChecker.ec $(COMPDIR)/Parser.ec $(COMPDIR)/Lexer.ec \
	    -o $(BUILDDIR)/compiler_port.ll -I $(STDLIBDIR)
	$(call link_ir,$(BUILDDIR)/compiler_port.ll,$(BUILDDIR)/compiler_port)
	@echo "Built: $(BUILDDIR)/compiler_port"

# Build the full self-hosted compiler (Driver + all stages)
self-hosted: release $(RUNTIME)
	$(DRIVER) \
	    $(COMPDIR)/Driver.ec \
	    $(COMPDIR)/Compiler.ec \
	    $(COMPDIR)/CodeGen.ec \
	    $(COMPDIR)/TypeChecker.ec \
	    $(COMPDIR)/Parser.ec \
	    $(COMPDIR)/Lexer.ec \
	    -o $(BUILDDIR)/self_hosted.ll -I $(STDLIBDIR)
	$(call link_ir,$(BUILDDIR)/self_hosted.ll,$(BUILDDIR)/self_hosted)
	@echo ""
	@echo "Built self-hosted compiler: $(BUILDDIR)/self_hosted"
	@echo "Running self-hosted compiler..."
	@$(BUILDDIR)/self_hosted

# ── ecvim — the editor written in ExtendedC ───────────────────────────────────
ecvim: release $(RUNTIME)
	$(DRIVER) ecvim.ec -o $(BUILDDIR)/ecvim.ll -I $(STDLIBDIR)
	$(call link_ir,$(BUILDDIR)/ecvim.ll,$(BUILDDIR)/ecvim)
	@echo ""
	@echo "Built editor: $(BUILDDIR)/ecvim"
	@echo "Run with:     $(BUILDDIR)/ecvim file.ec"
