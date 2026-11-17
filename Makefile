# =============================================================================
# camb.io Database Manager — Makefile
#
#   make            build camb.io_db_mgr.app
#   make run        run the daemon on a sandbox database in dev/data
#   make test       the release checks: test-build test-unit test-features
#                   test-ui (never part of all/run/install)
#   make install    install the bundle in ~/Applications
#   make clean      remove build/ and the bundle (dev/ is kept)
#   make clean-dev  remove dev/ (the sandbox and test data)
#
# Requires the Xcode command-line tools, MacPorts sqlite3 + pkgconf, and
# the shared_data and shared_ui repositories beside this one.  See BUILD.md.
# =============================================================================

# The names of things live in src/identity.h and nowhere else: `ident` reads
# one #define out of it.
ident        = $(shell sed -n 's/^\#define $(1) *"\(.*\)"/\1/p' src/identity.h)
APP_NAME    := $(call ident,CDB_APP_NAME)
BUNDLE_NAME := $(call ident,CDB_BUNDLE_NAME)
BUNDLE_ID   := $(call ident,CDB_BUNDLE_ID)
APP_ICON    := icons/$(call ident,CDB_APP_ICON)
BUILD_DATE_KEY := $(call ident,CDB_BUILD_DATE_KEY)
ifeq ($(and $(APP_NAME),$(BUNDLE_NAME),$(BUNDLE_ID),$(notdir $(APP_ICON)),$(BUILD_DATE_KEY)),)
$(error src/identity.h does not define every name the Makefile reads)
endif

BUNDLE   = $(BUNDLE_NAME).app
CONTENTS = $(BUNDLE)/Contents
MACOS    = $(CONTENTS)/MacOS
RSRC     = $(CONTENTS)/Resources
APP_BIN  = $(MACOS)/$(BUNDLE_NAME)

CC       = clang

# The version shown in the About panel; edit VERSION to change it.
VERSION := $(strip $(shell cat VERSION))
ifeq ($(VERSION),)
$(error the VERSION file is missing or empty)
endif

# pkg-config: MacPorts installs it in /opt/local/bin, which is not on every
# shell's PATH.  SQLite is MacPorts' on purpose: Notes and Tasks link the
# same build, so every process on the one file runs one SQLite (WAL, file
# format, locking).
PKGCONF    := $(shell command -v pkg-config 2>/dev/null || echo /opt/local/bin/pkg-config)
PKG_CFLAGS := $(shell $(PKGCONF) --cflags sqlite3)
PKG_LIBS   := $(shell $(PKGCONF) --libs sqlite3)
ifeq ($(PKG_LIBS),)
$(error sqlite3 not found by $(PKGCONF): sudo port install sqlite3 pkgconf)
endif

# ── The shared modules ───────────────────────────────────────────────────
# The database module (schema, location, connections, backups, checks) is
# the shared_data repository's and is compiled in; the tool panel and the
# small AppKit helpers are the shared_ui repository's.
SHARED_DATA_DIR ?= $(CURDIR)/../shared_data
SHARED_SRC       = $(SHARED_DATA_DIR)/src/shared_db.c
ifeq ($(wildcard $(SHARED_SRC)),)
$(error $(SHARED_SRC) not found: check out shared_data beside this repository or set SHARED_DATA_DIR)
endif
SHARED_UI_DIR ?= $(CURDIR)/../shared_ui
SHARED_UI_SRCS = $(SHARED_UI_DIR)/src/shared_ui.m $(SHARED_UI_DIR)/src/shared_tool_panel.m
ifeq ($(wildcard $(SHARED_UI_SRCS)),)
$(error $(SHARED_UI_DIR)/src has no sources: check out shared_ui beside this repository or set SHARED_UI_DIR)
endif

# Compiled for macOS 14; linked for the build machine's macOS because
# MacPorts builds its SQLite for the system it runs on (the linker warns
# otherwise).
MIN_MACOS   = 14.0
LINK_MACOS := $(shell sw_vers -productVersion | cut -d. -f1).0

# Code-signing identity (as in Files and Notes): the first "Apple
# Development" identity in the keychain, else ad hoc.  Override with
#   make CODESIGN_IDENTITY="…"   (list: security find-identity -v -p codesigning)
# Only goals that build the bundle look the identity up.
SIGNING_GOALS := $(if $(MAKECMDGOALS),$(filter-out clean clean-dev,$(MAKECMDGOALS)),all)
ifneq ($(SIGNING_GOALS),)
ifeq ($(origin CODESIGN_IDENTITY),undefined)
CODESIGN_IDENTITY := $(or $(shell security find-identity -v -p codesigning 2>/dev/null \
    | sed -n 's/.*"\(Apple Development: [^"]*\)".*/\1/p' | head -n 1),-)
endif
# build/codesign.id records the signing identity; a changed identity
# backdates the bundle, which forces a re-sign.
$(shell mkdir -p build && (echo '$(CODESIGN_IDENTITY)' | cmp -s - build/codesign.id \
    || { echo '$(CODESIGN_IDENTITY)' > build/codesign.id; \
         [ -d $(BUNDLE) ] && touch -t 200001010000 $(BUNDLE); true; }))
endif

# sign: codesign one thing, saying nothing unless it fails.
sign = codesign --force --sign "$(CODESIGN_IDENTITY)" $(1) 2>build/codesign.log \
       || { cat build/codesign.log >&2; exit 1; }

# -Werror=unguarded-availability: an API newer than the deployment target
# without an @available check is a build error.  -MMD -MP: header
# dependency files, so editing a header rebuilds its users.
# -Wno-unused-parameter, Objective-C only: AppKit's delegate and action
# signatures leave parameters unused.  The C core keeps the warning.
COMMON   = -O2 -Wall -Wextra -Werror=unguarded-availability \
           -mmacosx-version-min=$(MIN_MACOS) -I src -I $(SHARED_DATA_DIR)/src -I $(SHARED_UI_DIR)/src \
           -MMD -MP -DCDB_VERSION='"$(VERSION)"' $(PKG_CFLAGS)
CFLAGS   = -std=c11 $(COMMON)
MFLAGS   = -fobjc-arc $(COMMON) -Wno-unused-parameter

LDFLAGS   = -mmacosx-version-min=$(LINK_MACOS)
CORE_LIBS = $(PKG_LIBS) -framework CoreFoundation
APP_LIBS  = $(CORE_LIBS) -framework Cocoa -framework ServiceManagement \
            -framework UniformTypeIdentifiers

# The core: no UI, shared by the app and the tests.
CORE_SRCS = src/cdb.c src/service.c
UI_M_SRCS = $(wildcard src/*.m)

CORE_OBJS = $(patsubst src/%.c,build/%.o,$(CORE_SRCS)) build/shared_db.o
UI_OBJS   = $(patsubst src/%.m,build/%.o,$(UI_M_SRCS)) \
            $(patsubst $(SHARED_UI_DIR)/src/%.m,build/%.o,$(SHARED_UI_SRCS))
APP_OBJS  = $(CORE_OBJS) $(filter-out build/main.o,$(UI_OBJS))
DEPS      = $(CORE_OBJS:.o=.d) $(UI_OBJS:.o=.d)

.PHONY: all run test test-build test-unit test-features test-ui install clean clean-dev

all: $(BUNDLE)

ICON    = build/app_icon.icns
ICONSET = build/app_icon.iconset

# sips renders every iconset size from the PNG; iconutil packs them.
$(ICON): $(APP_ICON) | build
	@rm -rf $(ICONSET) && mkdir -p $(ICONSET)
	@for s in 16 32 128 256 512; do \
	    sips -z $$s $$s $< --out $(ICONSET)/icon_$${s}x$${s}.png >/dev/null; \
	    sips -z $$((s * 2)) $$((s * 2)) $< --out $(ICONSET)/icon_$${s}x$${s}@2x.png >/dev/null; \
	done
	@iconutil -c icns -o $@ $(ICONSET)

# Info.plist gets every name from src/identity.h, the version from VERSION
# and the time the bundle was assembled (UTC), shown in the About panel.
# The icon PNG (the menu bar item and the About panel load it by name) and
# the .icns (Finder, through CFBundleIconFile) go into Resources.
$(BUNDLE): $(APP_BIN) resources/Info.plist VERSION src/identity.h $(APP_ICON) $(ICON)
	@rm -rf $(RSRC) && mkdir -p $(RSRC)
	@cp resources/Info.plist $(CONTENTS)/Info.plist
	@plutil -replace CFBundleName -string "$(APP_NAME)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleDisplayName -string "$(APP_NAME)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleIdentifier -string "$(BUNDLE_ID)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleExecutable -string "$(BUNDLE_NAME)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleIconFile -string "app_icon" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleShortVersionString -string "$(VERSION)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleVersion -string "$(VERSION)" $(CONTENTS)/Info.plist
	@plutil -replace LSMinimumSystemVersion -string "$(LINK_MACOS)" $(CONTENTS)/Info.plist
	@plutil -replace $(BUILD_DATE_KEY) -string "$$(date -u +%Y-%m-%dT%H:%M:%SZ)" $(CONTENTS)/Info.plist
	@cp $(APP_ICON) $(RSRC)/
	@cp -X $(ICON) $(RSRC)/
	@$(call sign,$(BUNDLE))
	@touch $(BUNDLE)  # codesign rewrites the binary; keep the bundle newer

# Linked without the linker's own ad-hoc signature: the bundle rule signs.
$(APP_BIN): $(CORE_OBJS) $(UI_OBJS)
	@mkdir -p $(MACOS)
	$(CC) $(LDFLAGS) -Wl,-no_adhoc_codesign -o $@ $^ $(APP_LIBS)

# Objects also depend on the Makefile so flag changes rebuild everything,
# and on VERSION because CDB_VERSION is compiled in.
build/%.o: src/%.c Makefile VERSION | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: src/%.m Makefile VERSION | build
	$(CC) $(MFLAGS) -c -o $@ $<

build/%.o: $(SHARED_UI_DIR)/src/%.m Makefile VERSION | build
	$(CC) $(MFLAGS) -c -o $@ $<

build/shared_db.o: $(SHARED_SRC) Makefile | build
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEPS)

build:
	@mkdir -p build

# ── The sandbox ─────────────────────────────────────────────────────────────
# Every run and test from this tree sets CAMBIO_SHARED_DB to a file under
# dev/ (gitignored), so the per-user database in ~/Library/Application
# Support/camb.io is never touched.  The daemon creates the sandbox
# database itself on its first run — that is its job.
DEV_DATA = $(CURDIR)/dev/data
override DEV_SHARED_DB = $(DEV_DATA)/shared_data.db

# run: the daemon in the foreground on the sandbox, its log on this
# terminal.  Quit it from its menu, or with Ctrl-C.
run: $(BUNDLE)
	@mkdir -p "$(DEV_DATA)"
	@echo "CAMBIO_SHARED_DB=$(DEV_SHARED_DB)"
	@CAMBIO_SHARED_DB="$(DEV_SHARED_DB)" $(APP_BIN)

# ── Install ─────────────────────────────────────────────────────────────────
# Per-user install: no sudo.  A running installed copy is asked to quit
# first (SIGTERM, an ordinary Quit: the database is closed, the lock
# released) and reopened afterwards; killed if it has not exited after 5 s.
INSTALL_DIR   = $(HOME)/Applications
INSTALLED_BIN = $(INSTALL_DIR)/$(BUNDLE)/Contents/MacOS/$(BUNDLE_NAME)

install: all
	@mkdir -p "$(INSTALL_DIR)"
	@running=0; \
	if pgrep -qxf "$(INSTALLED_BIN)"; then \
	    running=1; \
	    pkill -TERM -xf "$(INSTALLED_BIN)"; \
	    for i in 1 2 3 4 5 6 7 8 9 10; do \
	        pgrep -qxf "$(INSTALLED_BIN)" || break; sleep 0.5; \
	    done; \
	    pkill -KILL -xf "$(INSTALLED_BIN)" 2>/dev/null; \
	    echo "Stopped the running $(APP_NAME)"; \
	fi; \
	rm -rf "$(INSTALL_DIR)/$(BUNDLE)" && \
	cp -R $(BUNDLE) "$(INSTALL_DIR)/$(BUNDLE)" && \
	echo "Installed to $(INSTALL_DIR)/$(BUNDLE)" && \
	if [ $$running = 1 ]; then open "$(INSTALL_DIR)/$(BUNDLE)" && echo "Reopened $(APP_NAME)"; fi

clean:
	@rm -rf build $(BUNDLE)

clean-dev:
	@rm -rf dev

# ── Release checks ───────────────────────────────────────────────────────
# Not dependencies of all/run/install: they run only when asked for (a
# release).  Every test gets a scratch folder of its own under dev/test,
# with CAMBIO_SHARED_DB pointing inside it.
TEST_DATA = $(CURDIR)/dev/test

test: test-build test-unit test-features test-ui
	@echo "all release checks passed"

# test-build: a from-scratch build has zero warnings, and the bundle is
# sound (valid Info.plist, executable present, signature verifies).
BUILD_LOG = build/build.log

test-build:
	@$(MAKE) clean
	@mkdir -p build
	@$(MAKE) all > $(BUILD_LOG) 2>&1 || { cat $(BUILD_LOG); exit 1; }
	@! grep -q 'warning:' $(BUILD_LOG) || { grep 'warning:' $(BUILD_LOG); echo "test-build: warnings"; exit 1; }
	@plutil -lint $(CONTENTS)/Info.plist >/dev/null
	@test -x $(APP_BIN)
	@codesign --verify --strict $(BUNDLE)
	@echo "test-build: ok"

# Unit (tests/unit/*_test.c) and feature (tests/features/*_test.c) tests
# link the core alone: no AppKit, no display.  The UI check
# (tests/ui/ui_check.m) links every app object but main.o and runs from a
# bundle of its own that borrows the app's Resources, so the icon loads.
UNIT_BINS    = $(patsubst tests/unit/%.c,build/unit_%,$(wildcard tests/unit/*_test.c))
FEATURE_BINS = $(patsubst tests/features/%.c,build/feature_%,$(wildcard tests/features/*_test.c))
UI_CHECK     = build/ui_check.app

build/unit_%: tests/unit/%.c tests/check.h $(CORE_OBJS) Makefile | build
	$(CC) $(CFLAGS) -I tests $(LDFLAGS) -o $@ $< $(CORE_OBJS) $(CORE_LIBS)

build/feature_%: tests/features/%.c tests/check.h $(CORE_OBJS) Makefile | build
	$(CC) $(CFLAGS) -I tests $(LDFLAGS) -o $@ $< $(CORE_OBJS) $(CORE_LIBS)

$(UI_CHECK): tests/ui/ui_check.m tests/check.h $(APP_OBJS) $(BUNDLE) Makefile | build
	@rm -rf $@ && mkdir -p $@/Contents/MacOS
	$(CC) $(MFLAGS) -I tests $(LDFLAGS) -o $@/Contents/MacOS/ui_check $< $(APP_OBJS) $(APP_LIBS)
	@ln -s $(CURDIR)/$(RSRC) $@/Contents/Resources

# run_tests: run each test program in $(1) on a scratch folder of its own.
define run_tests
	@for t in $(1); do \
	    name=$$(basename $$t .app); echo "== $$t"; \
	    case $$t in *.app) run=$$t/Contents/MacOS/$$name ;; *) run=$$t ;; esac; \
	    rm -rf "$(TEST_DATA)/$$name" && mkdir -p "$(TEST_DATA)/$$name" && \
	    CAMBIO_SHARED_DB="$(TEST_DATA)/$$name/shared_data.db" ./$$run $(2) || exit 1; \
	done
endef

test-unit: $(UNIT_BINS)
	$(call run_tests,$(UNIT_BINS))
	@echo "test-unit: ok"

test-features: $(FEATURE_BINS)
	$(call run_tests,$(FEATURE_BINS))
	@echo "test-features: ok"

# test-ui: the window built in-process on a scratch database, its chrome
# checked, light and dark PNG snapshots written to dev/snapshots.
test-ui: $(UI_CHECK)
	@rm -rf dev/snapshots && mkdir -p dev/snapshots
	$(call run_tests,$(UI_CHECK),$(CURDIR)/dev/snapshots)
	@echo "test-ui: ok"
