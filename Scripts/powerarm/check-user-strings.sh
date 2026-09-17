#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# check-user-strings.sh BUILD_DIR
#
# Fails when anything a POWERarm user can see still carries the FEX product
# name. Source identifiers, namespaces, file names and CMake targets stay
# FEX-named on purpose (backend commits cherry-pick from fastppcx86); this
# script only looks at what ends up in front of a user:
#
#   1. printable strings of every binary in BUILD_DIR/Bin and of
#      libPOWERarmCore.so wherever the build put it (strings -a -n 3)
#   2. the generated man page(s) in BUILD_DIR/generated (config option help)
#   3. --help / --version output of every tool that has an option parser
#   4. the QML sources compiled into POWERarmConfig as Qt resources (rcc may
#      compress them, so the binary scan cannot see them), minus // comments
#
# A hit is "\bFEX" (case-sensitive) or "fex-emu" (any case). Hits covered by
# ALLOWLIST below are ignored; everything else is printed as
# "<file>: <string>" and the script exits 1. Exit 0 means clean, 2 means the
# script could not run (bad BUILD_DIR, missing tools).
#
# ALLOWLIST (keep it short; prefer fixing the string over adding an entry):
#
#   A1 symbol names   A string that is exactly a symbol name in the same
#                     binary's symbol tables (nm -a / nm -D), mangled
#                     "_Z..." names, and "FEX...::" C++ namespace qualifiers
#                     (as found in __PRETTY_FUNCTION__ text). These are C++
#                     identifiers, kept FEX-named so cherry-picks apply.
#   A2 env keys       A whole string of the form FEX_[A-Z0-9_]* : the raw
#                     getenv()/setenv() keys that Source/POWERarm/EnvPrefix.cpp
#                     translates to POWERARM_* at link time, and that
#                     wrapper's own "FEX_" prefix constant. Only a whole
#                     string qualifies; a message that embeds FEX_<NAME> is a
#                     hit. One leading punctuation byte is tolerated because
#                     strings(1) glues a printable byte of adjacent data onto
#                     the front of a string (seen as "|FEX_R0TRAP").
#   A3 rootfs URL     https://rootfs.fex-emu.gg/RootFS_links.json, the only
#                     rootfs index POWERarmRootFSFetcher knows.
#   A4 rpmalloc       "FEXAllocator", the compiled-in default mapping name in
#                     the External/rpmalloc submodule. AllocatorHooks.cpp
#                     installs "POWERarmAllocator" when it initialises the
#                     allocator; the default only survives if rpmalloc
#                     initialises itself first. Fixing it needs a submodule
#                     change.
#   A5 source paths   A whole string that is a source file path
#                     (.../FEXCore/Source/foo.cpp and the like), which
#                     __FILE__ puts into assertion-enabled and Debug builds.
#                     Assertion messages are user-visible, but the path is
#                     a location in a source tree whose directories keep
#                     their FEX names by design; the message text is a
#                     separate string and is still checked.

set -u -o pipefail

usage() {
  echo "usage: $0 BUILD_DIR" >&2
  exit 2
}

[ $# -eq 1 ] || usage
[ -d "$1/Bin" ] || { echo "$0: $1/Bin does not exist; is $1 a POWERarm build directory?" >&2; exit 2; }
BUILD_DIR=$(cd "$1" && pwd)

for Tool in strings nm grep sed timeout; do
  command -v "$Tool" >/dev/null || { echo "$0: $Tool not found" >&2; exit 2; }
done

HIT_RE='\bFEX|(?i:fex-emu)'

WORK=$(mktemp -d "${TMPDIR:-/tmp}/check-user-strings.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT
FAILURES="$WORK/failures"
: > "$FAILURES"
EMPTY_SYMS="$WORK/nosyms"
: > "$EMPTY_SYMS"

# Is this (hit-bearing) string covered by the allowlist? $2 is a sorted symbol
# list for the file it came from (empty for text sources).
allowed() {
  local S=$1 Syms=$2 Stripped

  # A1: exact symbol name, with or without one leading strings(1) artifact byte.
  Stripped=${S#?}
  if [ -s "$Syms" ] && { grep -Fxq -- "$S" "$Syms" || grep -Fxq -- "$Stripped" "$Syms"; }; then
    return 0
  fi
  # A1: mangled names, then C++ namespace qualifiers (masked below).
  if printf '%s\n' "$S" | grep -Pq '(^|[^A-Za-z0-9_])_Z[A-Za-z0-9_]*FEX'; then
    return 0
  fi
  local Masked
  Masked=$(printf '%s\n' "$S" | sed -E 's/FEX[A-Za-z0-9_]*::/::/g')
  if ! printf '%s\n' "$Masked" | grep -Pq "$HIT_RE"; then
    return 0
  fi
  # A2: whole-string environment key.
  if printf '%s\n' "$S" | grep -Pq '^[^[:alnum:][:space:]]?FEX_[A-Z0-9_]*$'; then
    return 0
  fi
  # A3, A4: exact strings.
  case "$S" in
    "https://rootfs.fex-emu.gg/RootFS_links.json") return 0 ;;
    "FEXAllocator") return 0 ;;
  esac
  # A5: whole-string source path.
  if printf '%s\n' "$S" | grep -Pq '^[^[:space:]]*(^|/)(FEXCore|FEXHeaderUtils|CodeEmitter|Source|External)/[^[:space:]]*\.(c|cc|cpp|h|hpp|inl)$'; then
    return 0
  fi
  return 1
}

# scan_lines LABEL SYMFILE < lines
scan_lines() {
  local Label=$1 Syms=$2 Line
  while IFS= read -r Line; do
    if ! allowed "$Line" "$Syms"; then
      printf '%s: %s\n' "$Label" "$Line" >> "$FAILURES"
    fi
  done
}

# 1. Binaries.
Binaries=()
while IFS= read -r -d '' F; do
  Binaries+=("$F")
done < <(find "$BUILD_DIR/Bin" -maxdepth 1 -type f -print0 | sort -z)
while IFS= read -r -d '' F; do
  Binaries+=("$F")
done < <(find "$BUILD_DIR" -path '*/CMakeFiles' -prune -o -type f -name 'libPOWERarmCore.so*' -print0 | sort -z)

if ! printf '%s\n' "${Binaries[@]}" | grep -q 'libPOWERarmCore\.so'; then
  echo "$0: note: no libPOWERarmCore.so under $BUILD_DIR" >&2
fi

for F in "${Binaries[@]}"; do
  Label=${F#"$BUILD_DIR"/}
  Syms="$WORK/syms"
  { nm -a "$F"; nm -D "$F"; } 2>/dev/null | awk 'NF { print $NF }' | sort -u > "$Syms"
  strings -a -n 3 "$F" | grep -P "$HIT_RE" | sort -u | scan_lines "$Label" "$Syms"
done

# 2. Generated man pages. Drop roff font escapes first so "\fBFEX_X\fR"
# still has a word boundary in front of FEX.
while IFS= read -r -d '' F; do
  Label=${F#"$BUILD_DIR"/}
  case "$F" in
    *.gz) Cat=(gzip -dc) ;;
    *) Cat=(cat) ;;
  esac
  "${Cat[@]}" "$F" | sed -E 's/\\f[BIRP]//g' | grep -nP "$HIT_RE" | scan_lines "$Label" "$EMPTY_SYMS"
done < <(find "$BUILD_DIR/generated" -maxdepth 1 -type f \( -name '*.1' -o -name '*.1.gz' \) -print0 2>/dev/null | sort -z)

# 3. --help / --version. Tools without an option parser are not run:
#   POWERarm         treats every argument as the guest program to run
#   POWERarmBash     forwards its arguments to a guest shell
#   POWERarmConfig   GUI; its visible text is the QML checked in step 4
#   POWERarmGDBReader has no options
HOME_DIR="$WORK/home"
mkdir -p "$HOME_DIR"
run_tool() {
  env HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_DATA_HOME="$HOME_DIR/.local/share" \
    XDG_CACHE_HOME="$HOME_DIR/.cache" QT_QPA_PLATFORM=offscreen \
    timeout 20 "$@" < /dev/null 2>&1
}
for F in "$BUILD_DIR"/Bin/*; do
  [ -x "$F" ] && [ -f "$F" ] || continue
  Name=${F##*/}
  case "$Name" in
    POWERarm | POWERarmBash | POWERarmConfig | POWERarmGDBReader) continue ;;
  esac
  run_tool "$F" --help | grep -P "$HIT_RE" | scan_lines "$Name --help" "$EMPTY_SYMS"
  case "$Name" in
    POWERarmServer | POWERarmGetConfig)
      run_tool "$F" --version | grep -P "$HIT_RE" | scan_lines "$Name --version" "$EMPTY_SYMS"
      ;;
  esac
done

# 4. QML compiled into POWERarmConfig.
SRC_DIR=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null)
if [ -f "$BUILD_DIR/Bin/POWERarmConfig" ] && [ -n "$SRC_DIR" ]; then
  ConfigDir="$SRC_DIR/Source/Tools/FEXConfig"
  for Qrc in "$ConfigDir"/*.qrc; do
    [ -f "$Qrc" ] || continue
    sed -n 's|.*<file[^>]*>\(.*\.qml\)</file>.*|\1|p' "$Qrc"
  done | sort -u | while IFS= read -r Qml; do
    [ -f "$ConfigDir/$Qml" ] || continue
    # Line comments are not shown by the UI; drop them before matching.
    sed -E 's#(^|[[:space:]])//.*$##' "$ConfigDir/$Qml" | grep -nP "$HIT_RE" | scan_lines "POWERarmConfig resource $Qml" "$EMPTY_SYMS"
  done
fi

if [ -s "$FAILURES" ]; then
  Count=$(wc -l < "$FAILURES")
  echo "check-user-strings: $Count user-visible string(s) still name FEX:"
  cat "$FAILURES"
  exit 1
fi
echo "check-user-strings: OK (${#Binaries[@]} binaries, man pages, tool help, QML)"
exit 0
