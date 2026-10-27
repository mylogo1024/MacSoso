#!/usr/bin/env bash
#
# Build, bundle and package MacSoSo for release.
#
# - Builds the Xcode Release configuration for one architecture.
# - Copies every non-system dylib (Homebrew re2 + abseil) into
#   MacSoSo.app/Contents/Frameworks and rewrites the load commands.
# - Ad-hoc signs by default; pass --sign-identity "Developer ID Application: …"
#   to sign with a real certificate (hardened runtime enabled).
# - Produces dist/MacSoSo-<version>-<arch>.dmg and a .sha256 file.
#
# Usage:
#   ./scripts/package_release.sh [options]
#
# Options:
#   --version <x.y.z>     Override CFBundleShortVersionString
#   --build <number>      Override CFBundleVersion
#   --arch <arm64|x86_64> Build one architecture (default: host arch)
#   --output-dir <dir>    Output directory (default: ./dist)
#   --sign-identity <id>  codesign identity, or "-" for ad-hoc (default)
#   --skip-build          Package an existing build/DerivedData app
#   -h, --help            Show this help
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_NAME="MacSoSo"
SCHEME="MacSoSo"
PROJECT="$ROOT/MacSoSo.xcodeproj"
BUILD_DIR="$ROOT/build/DerivedData"
OUTPUT_DIR="$ROOT/dist"
VERSION=""
BUILD_NUMBER=""
ARCH=""
SIGN_IDENTITY="-"
SKIP_BUILD=0

usage() {
    sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

die() {
    echo "error: $*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="${2:-}"; shift 2 ;;
        --build) BUILD_NUMBER="${2:-}"; shift 2 ;;
        --arch) ARCH="${2:-}"; shift 2 ;;
        --output-dir) OUTPUT_DIR="${2:-}"; shift 2 ;;
        --sign-identity) SIGN_IDENTITY="${2:-}"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        -h|--help) usage ;;
        *) die "unknown argument: $1 (use --help)" ;;
    esac
done

[[ "$(uname -s)" == "Darwin" ]] || die "macOS is required"

cd "$ROOT"

# ---------------------------------------------------------------------------
# Toolchain selection
# ---------------------------------------------------------------------------
if ! xcodebuild -version >/dev/null 2>&1; then
    shopt -s nullglob
    for candidate in /Applications/Xcode.app /Applications/Xcode*.app; do
        if [[ -x "$candidate/Contents/Developer/usr/bin/xcodebuild" ]]; then
            export DEVELOPER_DIR="$candidate/Contents/Developer"
            break
        fi
    done
    xcodebuild -version >/dev/null 2>&1 \
        || die "xcodebuild is unavailable; install Xcode or set DEVELOPER_DIR"
fi

BREW_BIN="${BREW_BIN:-$(command -v brew || true)}"
[[ -n "$BREW_BIN" ]] || die "Homebrew is required (re2/abseil)"
RE2_PREFIX="${RE2_PREFIX:-$("$BREW_BIN" --prefix re2)}"
ABSEIL_PREFIX="${ABSEIL_PREFIX:-$("$BREW_BIN" --prefix abseil)}"
[[ -f "$RE2_PREFIX/include/re2/re2.h" ]] \
    || die "re2 headers not found under $RE2_PREFIX (brew install re2)"
[[ -d "$ABSEIL_PREFIX/include/absl" ]] \
    || die "abseil headers not found under $ABSEIL_PREFIX (brew install abseil)"

# ---------------------------------------------------------------------------
# Version / architecture
# ---------------------------------------------------------------------------
PLIST="$BUILD_DIR/Build/Products/Release/$APP_NAME.app/Contents/Info.plist"

if [[ -z "$VERSION" ]]; then
    VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
        "$ROOT/MacSoSo/Info.plist")"
fi
VERSION="${VERSION#v}"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]] \
    || die "invalid version: $VERSION"

if [[ -z "$BUILD_NUMBER" ]]; then
    BUILD_NUMBER="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' \
        "$ROOT/MacSoSo/Info.plist")"
fi
[[ "$BUILD_NUMBER" =~ ^[0-9]+$ ]] || die "invalid build number: $BUILD_NUMBER"

if [[ -z "$ARCH" ]]; then
    case "$(uname -m)" in
        arm64) ARCH="arm64" ;;
        x86_64) ARCH="x86_64" ;;
        *) die "unsupported host architecture: $(uname -m)" ;;
    esac
fi
[[ "$ARCH" == "arm64" || "$ARCH" == "x86_64" ]] \
    || die "unsupported --arch: $ARCH (use arm64 or x86_64)"

APP="$BUILD_DIR/Build/Products/Release/$APP_NAME.app"
APP_BIN="$APP/Contents/MacOS/$APP_NAME"
FRAMEWORKS="$APP/Contents/Frameworks"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if [[ "$SKIP_BUILD" -eq 0 ]]; then
    echo "==> Building $APP_NAME $VERSION ($ARCH, Release)"
    xcodebuild \
        -project "$PROJECT" \
        -scheme "$SCHEME" \
        -configuration Release \
        -derivedDataPath "$BUILD_DIR" \
        ARCHS="$ARCH" \
        ONLY_ACTIVE_ARCH=NO \
        CODE_SIGN_IDENTITY="-" \
        CODE_SIGNING_REQUIRED=YES \
        RE2_PREFIX="$RE2_PREFIX" \
        ABSEIL_PREFIX="$ABSEIL_PREFIX" \
        build
fi

[[ -d "$APP" ]] || die "app not found at $APP"
[[ -x "$APP_BIN" ]] || die "executable not found at $APP_BIN"

if [[ -n "$(/usr/bin/lipo -archs "$APP_BIN" 2>/dev/null)" ]]; then
    BUILT_ARCHS="$(/usr/bin/lipo -archs "$APP_BIN")"
    [[ " $BUILT_ARCHS " == *" $ARCH "* ]] \
        || die "built app is $BUILT_ARCHS, expected $ARCH"
fi

echo "==> Stamping version $VERSION ($BUILD_NUMBER)"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $BUILD_NUMBER" "$PLIST"

# ---------------------------------------------------------------------------
# Bundle external dylibs
# ---------------------------------------------------------------------------
rm -rf "$FRAMEWORKS"
mkdir -p "$FRAMEWORKS"

# is_external_dep: true for Homebrew / user-installed dylibs, false for the
# OS runtime and already-relocatable references.
is_external_dep() {
    local dep="$1"
    case "$dep" in
        @*|/System/*|/usr/lib/*|/Library/Apple/*) return 1 ;;
        /*) return 0 ;;
        *) return 1 ;;
    esac
}

# copy_dependencies <mach-o file>
copy_dependencies() {
    local binary="$1"
    local dep base target
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        is_external_dep "$dep" || continue
        base="$(basename "$dep")"
        target="$FRAMEWORKS/$base"
        if [[ -e "$target" ]]; then
            cmp -s "$dep" "$target" \
                || die "dylib name conflict: $dep vs $target"
            continue
        fi
        [[ -f "$dep" ]] || die "missing dependency: $dep"
        cp -f "$dep" "$target"
        chmod u+w "$target"
        copy_dependencies "$target"
    done < <(otool -L "$binary" | awk 'NR > 1 { print $1 }')
}

# Collect every nested Mach-O executable inside the bundle so that (a) their
# external dylibs get bundled/rewritten and (b) they are all individually
# signed before the bundle itself. We scan the standard nested-code locations
# and also follow symlinks, because Xcode's "Copy Files" phase can land a
# helper/CLI tool (e.g. MacSoSoMCP) that must be code-signed or the final
# bundle sign fails with "code object is not signed at all In subcomponent".
APP_MACHOS=()
while IFS= read -r -d '' candidate; do
    # `file` and `codesign` both follow symlinks, so testing the bundle path
    # directly is sufficient (no need for readlink -f, which is GNU-only).
    [[ -f "$candidate" ]] || continue
    if file "$candidate" | grep -q 'Mach-O'; then
        APP_MACHOS+=("$candidate")
    fi
done < <(find "$APP/Contents" \
    \( -path '*/MacOS/*' -o -path '*/Helpers/*' -o -path '*/PlugIns/*' \
       -o -path '*/XPCServices/*' -o -path '*/Library/*' \) \
    \( -type f -o -type l \) -print0)

for binary in "${APP_MACHOS[@]}"; do
    copy_dependencies "$binary"
done

# rewrite_dependencies <binary> <mode:app|lib>
rewrite_dependencies() {
    local binary="$1" mode="$2"
    local dep base replacement
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        case "$dep" in
            @*) continue ;;
            /System/*|/usr/lib/*|/Library/Apple/*) continue ;;
        esac
        base="$(basename "$dep")"
        [[ -f "$FRAMEWORKS/$base" ]] \
            || die "unbundled dependency $dep in $binary"
        if [[ "$mode" == "app" ]]; then
            replacement="@rpath/$base"
        else
            replacement="@loader_path/$base"
        fi
        install_name_tool -change "$dep" "$replacement" "$binary"
    done < <(otool -L "$binary" | awk 'NR > 1 { print $1 }')
}

if [[ -d "$FRAMEWORKS" ]]; then
    while IFS= read -r -d '' dylib; do
        install_name_tool -id "@rpath/$(basename "$dylib")" "$dylib"
        rewrite_dependencies "$dylib" lib
    done < <(find "$FRAMEWORKS" -type f -name '*.dylib' -print0)
fi

for binary in "${APP_MACHOS[@]}"; do
    rewrite_dependencies "$binary" app
    # The main bundle already declares this rpath; add it defensively for
    # binaries whose load commands were stripped or hand-assembled.
    if ! otool -l "$binary" | grep -q '@executable_path/../Frameworks'; then
        install_name_tool -add_rpath "@executable_path/../Frameworks" "$binary" 2>/dev/null || true
    fi
done

# ---------------------------------------------------------------------------
# Verify relocation
# ---------------------------------------------------------------------------
echo "==> Verifying bundled dependencies"
for binary in "${APP_MACHOS[@]}" "$FRAMEWORKS"/*.dylib; do
    [[ -e "$binary" ]] || continue
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        is_external_dep "$dep" \
            && die "$(basename "$binary") still references $dep"
    done < <(otool -L "$binary" | awk 'NR > 1 { print $1 }')
done

# ---------------------------------------------------------------------------
# Sign
# ---------------------------------------------------------------------------
echo "==> Signing (identity: $SIGN_IDENTITY)"
SIGN_ARGS=(--force --sign "$SIGN_IDENTITY")
if [[ "$SIGN_IDENTITY" == "-" ]]; then
    SIGN_ARGS+=(--timestamp=none)
else
    SIGN_ARGS+=(--options runtime --timestamp)
fi

if [[ -d "$FRAMEWORKS" ]]; then
    while IFS= read -r -d '' dylib; do
        codesign "${SIGN_ARGS[@]}" "$dylib"
    done < <(find "$FRAMEWORKS" -type f -name '*.dylib' -print0)
fi

# Sign any nested Mach-O payload (for example MacSoSo.debug.dylib),
# then the bundle itself with its entitlements.
for ((i = ${#APP_MACHOS[@]} - 1; i >= 0; i--)); do
    codesign "${SIGN_ARGS[@]}" "${APP_MACHOS[$i]}"
done

# Belt-and-suspenders: explicitly sign every non-main executable copied into
# Contents/MacOS (Xcode's "Copy Files" phase drops helpers like MacSoSoMCP
# there unsigned). If any nested Mach-O is left unsigned, the bundle sign
# below fails with "code object is not signed at all In subcomponent".
# Scan every file under Contents/MacOS and sign it, skipping only the main
# executable. We intentionally do NOT test for the executable bit (-x): the
# "Copy Files" build phase can drop +x on the nested CLI tool (MacSoSoMCP),
# and a missing +x would let the old `[[ -x "$helper" ]]` guard skip it,
# leaving it unsigned and breaking the final bundle sign with
# "code object is not signed at all / In subcomponent: …".
while IFS= read -r -d '' helper; do
    [[ "$helper" == "$APP_BIN" ]] && continue
    file "$helper" | grep -q 'Mach-O' || continue
    echo "==> Signing nested component: $(basename "$helper")"
    codesign "${SIGN_ARGS[@]}" "$helper"
done < <(find "$APP/Contents/MacOS" -type f -print0)

# Sign the bundle container itself. Every nested component (Frameworks dylibs,
# the Contents/MacOS helper MacSoSoMCP, plus any Helpers/PlugIns/XPCServices
# discovered above) has already been signed individually, so we sign the bundle
# WITHOUT --deep. Apple discourages --deep (it re-signs nested code in
# unpredictable order and is a common source of "code object is not signed at
# all / In subcomponent" failures on newer macOS), and it is unnecessary here
# because the loops above already signed each component in the correct order.
codesign "${SIGN_ARGS[@]}" \
    --entitlements "$ROOT/MacSoSo/MacSoSo.entitlements" \
    "$APP"
codesign --verify --deep --strict "$APP"

# ---------------------------------------------------------------------------
# DMG
# ---------------------------------------------------------------------------
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
DMG_BASENAME="$APP_NAME-$VERSION-$ARCH.dmg"
DMG="$OUTPUT_DIR/$DMG_BASENAME"
STAGING="$(mktemp -d "$OUTPUT_DIR/.dmg.XXXXXX")"
trap 'rm -rf "$STAGING"' EXIT

cp -R "$APP" "$STAGING/"
ln -s /Applications "$STAGING/Applications"
mkdir -p "$STAGING/Licenses"
[[ -f "$ROOT/LICENSE" ]] && cp "$ROOT/LICENSE" "$STAGING/Licenses/LICENSE"
[[ -f "$ROOT/NOTICE" ]] && cp "$ROOT/NOTICE" "$STAGING/Licenses/NOTICE"
[[ -f "$ROOT/THIRD_PARTY_NOTICES.md" ]] \
    && cp "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGING/Licenses/THIRD_PARTY_NOTICES.md"
if [[ -d "$ROOT/third_party" ]]; then
    cp "$ROOT"/third_party/*-LICENSE "$STAGING/Licenses/"
fi

echo "==> Creating $DMG_BASENAME"
hdiutil create -quiet -volname "$APP_NAME" \
    -srcfolder "$STAGING" -ov -format UDZO "$DMG"

rm -rf "$STAGING"
trap - EXIT

(
    cd "$OUTPUT_DIR"
    shasum -a 256 "$DMG_BASENAME" > "$DMG_BASENAME.sha256"
)

echo
echo "==> Done"
echo "    App:    $APP"
echo "    DMG:    $DMG"
echo "    SHA256: $DMG.sha256"
