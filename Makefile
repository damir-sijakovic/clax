# Makefile for clax — the Clax V2 transpiler.
#
# Configure, build and install with:
#     ./configure [--prefix=...]
#     make
#     sudo make install
#
# Honours DESTDIR for staged installs. Requires GNU make.

# Pull in ./configure output. If it's missing, the rule below explains how to
# generate it (and stops the build with a clear message).
include config.mk

# `all` is the default goal, not the config.mk remake rule below.
.DEFAULT_GOAL := all

config.mk:
	@echo "Run ./configure first (config.mk is missing)." >&2; exit 1

INSTALL         ?= install
INSTALL_PROGRAM ?= $(INSTALL) -m 0755
INSTALL_DATA    ?= $(INSTALL) -m 0644
LN_S            ?= ln -sf

# Bake the install-time data dir into the binary so `clax --init-project`
# can locate system/*.cx after installation. Single-quoted so the literal
# string survives into the -D macro.
DATADIR_DEF = -DCLAX_DATADIR='"$(claxdatadir)"'

BIN     = clax/clax
SRC     = clax/clax.c
SYSSRC  = $(wildcard system/*.cx)
DOC     = Clax-Specification-V2.md
MAN1    = clax.1

.PHONY: all clean distclean install uninstall examples examples-glfw examples-sdl2 check

all: $(BIN) clax/clax_lint

$(BIN): $(SRC) config.mk
	$(CC) $(CFLAGS) $(DATADIR_DEF) -o $@ $(SRC) $(LDFLAGS)

# clax_lint is the same binary; argv[0] selects lint-only mode.
clax/clax_lint: $(BIN)
	$(LN_S) clax $@

# ----- install ------------------------------------------------------------
install: all
	@echo ">> installing clax to $(DESTDIR)$(bindir)"
	$(INSTALL) -d "$(DESTDIR)$(bindir)"
	$(INSTALL_PROGRAM) $(BIN) "$(DESTDIR)$(bindir)/clax"
	$(LN_S) clax "$(DESTDIR)$(bindir)/clax_lint"
	@echo ">> installing system library to $(DESTDIR)$(claxdatadir)/system"
	$(INSTALL) -d "$(DESTDIR)$(claxdatadir)/system"
	@for f in $(SYSSRC); do \
	    echo "   $$f"; \
	    $(INSTALL_DATA) "$$f" "$(DESTDIR)$(claxdatadir)/system/" || exit 1; \
	done
	@echo ">> installing documentation to $(DESTDIR)$(docdir)"
	$(INSTALL) -d "$(DESTDIR)$(docdir)"
	$(INSTALL_DATA) $(DOC) "$(DESTDIR)$(docdir)/"
	@if [ -f README.md ]; then $(INSTALL_DATA) README.md "$(DESTDIR)$(docdir)/"; fi
	@echo ">> installing man page to $(DESTDIR)$(mandir)/man1"
	$(INSTALL) -d "$(DESTDIR)$(mandir)/man1"
	$(INSTALL_DATA) $(MAN1) "$(DESTDIR)$(mandir)/man1/clax.1"
	printf '.so man1/clax.1\n' > "$(DESTDIR)$(mandir)/man1/clax_lint.1"
	@echo "install complete."

uninstall:
	rm -f "$(DESTDIR)$(bindir)/clax" "$(DESTDIR)$(bindir)/clax_lint"
	rm -rf "$(DESTDIR)$(claxdatadir)"
	rm -rf "$(DESTDIR)$(docdir)"
	rm -f "$(DESTDIR)$(mandir)/man1/clax.1" "$(DESTDIR)$(mandir)/man1/clax_lint.1"
	@echo "uninstall complete."

# ----- convenience --------------------------------------------------------
EXAMPLES = $(CURDIR)/examples

# Transpile + compile the dependency-free example project against ./system.
examples: all
	$(BIN) --include="$(CURDIR)/system" "$(EXAMPLES)/project"
	$(CC) $(CFLAGS) -o "$(EXAMPLES)/project/program" "$(EXAMPLES)/project"/generated_c/*.c $(LDFLAGS)
	@echo "built: examples/project/program"

# GUI examples link their libraries via pkg-config; build them explicitly.
examples-glfw: all
	$(BIN) "$(EXAMPLES)/glfw_project"
	$(CC) $(CFLAGS) `pkg-config --cflags glfw3` -o "$(EXAMPLES)/glfw_project/program" \
	    "$(EXAMPLES)/glfw_project"/generated_c/*.c `pkg-config --libs glfw3 gl` $(LDFLAGS)
	@echo "built: examples/glfw_project/program"

examples-sdl2: all
	$(BIN) "$(EXAMPLES)/sdl2_project"
	$(CC) $(CFLAGS) `pkg-config --cflags sdl2` -o "$(EXAMPLES)/sdl2_project/program" \
	    "$(EXAMPLES)/sdl2_project"/generated_c/*.c `pkg-config --libs sdl2` $(LDFLAGS)
	@echo "built: examples/sdl2_project/program"

# Lint the bundled example project as a smoke test.
check: all
	$(BIN) --lint --include="$(CURDIR)/system" "$(EXAMPLES)/project"

clean:
	rm -f $(BIN) clax/clax_lint

distclean: clean
	rm -f config.mk
