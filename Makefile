CXX = clang++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra
# Emit .d dependency files so header edits trigger a rebuild, and compile each
# translation unit separately so builds are incremental and parallel.
DEPFLAGS = -MMD -MP
FRAMEWORKS = -framework CoreServices
CORE_SRCS = $(wildcard MacSoSo/Core/*.cpp)
CORE_OBJS = $(CORE_SRCS:.cpp=.o)
# Only per-object depfiles exist (link rules do not emit depfiles).
DEPS = $(CORE_OBJS:.o=.d) test_all.d benchmark.d MacSoSo/CLI/daemon_main.d

# --- Dependency discovery (re2 / absl) ---
# Prefer pkg-config; fall back to well-known Homebrew prefixes.
RE2_PREFIX = $(shell brew --prefix re2 2>/dev/null || echo /opt/homebrew/opt/re2)
ABSEIL_PREFIX = $(shell brew --prefix abseil 2>/dev/null || echo /opt/homebrew/opt/abseil)
HAVE_PKGCONFIG_RE2 := $(shell pkg-config --exists re2 2>/dev/null && echo 1)
ifeq ($(HAVE_PKGCONFIG_RE2),1)
RE2_CFLAGS := $(shell pkg-config --cflags re2)
RE2_LDFLAGS := $(shell pkg-config --libs re2)
else
RE2_CFLAGS = -I$(RE2_PREFIX)/include -I$(ABSEIL_PREFIX)/include
RE2_LDFLAGS = -L$(RE2_PREFIX)/lib -L$(ABSEIL_PREFIX)/lib -lre2
endif

INCLUDES = -IMacSoSo/Core

# === Build targets ===
.PHONY: test test-fast test-slow test-all test-asan test-tsan build clean app dmg release daemon icons help

# Per-object compilation: correct header dependencies + parallel-friendly.
MacSoSo/Core/%.o: MacSoSo/Core/%.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(RE2_CFLAGS) $(INCLUDES) -c $< -o $@

# Top-level translation units get their own object + depfile so that editing a
# header under tests/ triggers a rebuild (they include the whole test suite).
test_all.o: test_all.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(RE2_CFLAGS) $(INCLUDES) -c $< -o $@

benchmark.o: benchmark.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(RE2_CFLAGS) $(INCLUDES) -c $< -o $@

MacSoSo/CLI/daemon_main.o: MacSoSo/CLI/daemon_main.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(RE2_CFLAGS) $(INCLUDES) -c $< -o $@

test_all: test_all.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(RE2_CFLAGS) $(FRAMEWORKS) $(RE2_LDFLAGS) $(INCLUDES) $^ -o $@

benchmark: benchmark.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(RE2_CFLAGS) $(FRAMEWORKS) $(RE2_LDFLAGS) $(INCLUDES) $^ -o $@

macsoso-daemon: MacSoSo/CLI/daemon_main.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(RE2_CFLAGS) $(FRAMEWORKS) $(RE2_LDFLAGS) $(INCLUDES) $^ -o $@

daemon: macsoso-daemon

icons:
	python3 scripts/generate_app_icons.py

# === Lint targets ===
lint-bridge:
	$(CXX) $(CXXFLAGS) -fsyntax-only -fobjc-arc -x objective-c++ \
		-IMacSoSo/Core -IMacSoSo/Bridge \
		MacSoSo/Bridge/MacSearchBridge.mm \
		MacSoSo/Bridge/MacSearchBridge+Content.mm

# === Sanitizer targets ===
test-asan: test_all.cpp $(CORE_SRCS)
	$(CXX) -std=c++20 -O1 -g -fsanitize=address -fno-omit-frame-pointer $(DEPFLAGS) $(RE2_CFLAGS) $(FRAMEWORKS) $(RE2_LDFLAGS) $(INCLUDES) $^ -o test_all_asan
	./test_all_asan --fast

test-tsan: test_all.cpp $(CORE_SRCS)
	$(CXX) -std=c++20 -O1 -g -fsanitize=thread $(DEPFLAGS) $(RE2_CFLAGS) $(FRAMEWORKS) $(RE2_LDFLAGS) $(INCLUDES) $^ -o test_all_tsan
	./test_all_tsan --fast

# === Test targets ===
test: test-fast

test-fast: test_all lint-bridge
	./test_all --fast

test-slow: test_all
	./test_all --slow

test-all: test_all
	./test_all

# === Xcode build ===
# Prefer the active developer directory; fall back to an installed Xcode when
# xcode-select points at the Command Line Tools.
XCODE_DEVELOPER_DIR ?= $(shell xcodebuild -version >/dev/null 2>&1 \
	&& xcode-select -p \
	|| ls -d /Applications/Xcode*.app/Contents/Developer 2>/dev/null | head -1)

app:
	DEVELOPER_DIR="$(XCODE_DEVELOPER_DIR)" xcodebuild \
		-project MacSoSo.xcodeproj -scheme MacSoSo \
		-configuration Release -derivedDataPath build/DerivedData \
		RE2_PREFIX="$(RE2_PREFIX)" ABSEIL_PREFIX="$(ABSEIL_PREFIX)" \
		build

# === Package ===
# Produces dist/MacSoSo-<version>-<arch>.dmg with bundled re2/abseil.
dmg:
	./scripts/package_release.sh

release: dmg

# === Cleanup ===
clean:
	rm -f test_all test_all_asan test_all_tsan benchmark macsoso-daemon
	rm -f $(CORE_OBJS) $(DEPS)
	rm -rf build/

# === Help ===
help:
	@echo "Available targets:"
	@echo "  make test       - Run fast unit tests (alias for test-fast)"
	@echo "  make test-fast  - Run fast unit tests (Part 3, 3b, 3c, 3d, 3e, 5)"
	@echo "  make test-slow  - Run slow integration tests (Part 1, 4, 6)"
	@echo "  make test-all   - Run all tests"
	@echo "  make app        - Build MacSoSo.app via Xcode"
	@echo "  make dmg        - Build + package dist/*.dmg (bundled dylibs)"
	@echo "  make release    - Alias for make dmg"
	@echo "  make daemon     - Build CLI daemon (macsoso-daemon)"
	@echo "  make icons      - Regenerate AppIcon PNGs from assets/appicon"
	@echo "  make benchmark  - Build benchmark binary"
	@echo "  make clean      - Remove build artifacts"

-include $(DEPS)
