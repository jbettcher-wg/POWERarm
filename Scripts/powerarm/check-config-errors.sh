#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# check-config-errors.sh BUILD_DIR [ROOTFS]
#
# A config that cannot be fully applied must not be partially applied in
# silence. This checks the ways a Config.json or a POWERARM_* variable can be
# wrong, and that each one says so instead of quietly running with a value the
# user did not ask for -- which is how two Octane runs came to be attributed to
# VSXClasses while it was off.
#
#   bool      A bool option takes 1/0 and the string option next to it in the
#             same file takes words (NZCVExitDead: off/on/canary/strict), so
#             `"VSXClasses": "on"` is a thing people write. It used to parse as
#             false: bool satisfies std::integral, so the value went through
#             strtoull in FEXCore::StrConv::Conv, which returned 0 and reported
#             success. Now the obvious spellings are accepted and everything
#             else is refused by name. Checked through both loaders -- the JSON
#             file and the environment variable do not share a code path -- and
#             on more than one option, since the handler is shared by all 66.
#
#             "on" being accepted is not enough; it has to reach the feature.
#             VSXCLASSES is hashed into the code-cache config id, so a run with
#             it on names its cache files differently from a run with it off.
#             That id is the evidence: the truthy spellings must all produce the
#             id that "1" produces, and the falsy ones the id that "0" does.
#
# Every run here is sealed off with POWERARM_PORTABLE, its own
# POWERARM_APP_CONFIG and its own POWERARM_APP_CACHE_LOCATION, so it neither
# reads the owner's ~/.config/powerarm/Config.json nor touches the shared
# ~/.cache/powerarm/cache/ that check-code-cache.sh and the A64Frontend suite
# fight over.
#
# Exit 0 means every check passed, 1 means one did not, 2 means the script
# could not run.

set -u

build=${1:?usage: check-config-errors.sh BUILD_DIR [ROOTFS]}
emu=$build/Bin/POWERarm
[ -x "$emu" ] || {
  echo "check-config-errors: no POWERarm in $build/Bin" >&2
  exit 2
}
rootfs=${2:-${XDG_DATA_HOME:-$HOME/.local/share}/powerarm/RootFS/ArchLinuxARM-m2}
[ -x "$rootfs/usr/bin/uname" ] || {
  echo "check-config-errors: no uname in rootfs $rootfs" >&2
  exit 2
}

w=$(mktemp -d "${TMPDIR:-/tmp}/check-config-errors.XXXXXX") || exit 2
trap 'rm -rf "$w"' EXIT
mkdir -p "$w/home" "$w/run" "$w/cfg"
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

# emu CONFIG_TEXT [ENV=V...] -- : one POWERarm process running `uname -m`, with
# CONFIG_TEXT as its whole config file ("-" for no file at all). Prints the
# guest's output and the emulator's diagnostics together; the caller looks at
# $? for the verdict.
run() {
  local cfg=$1 cache=$2
  shift 2
  local envs=()
  if [ "$cfg" != - ]; then
    printf '%s' "$cfg" > "$w/cfg/Config.json"
    envs+=(POWERARM_APP_CONFIG="$w/cfg/Config.json")
  fi
  if [ "$cache" != - ]; then
    envs+=(POWERARM_ENABLECODECACHINGWIP=1 POWERARM_CODECACHESCOPE=all POWERARM_APP_CACHE_LOCATION="$cache/")
  fi
  while [ $# -gt 0 ] && [ "$1" != -- ]; do
    envs+=("$1")
    shift
  done
  local out rc attempt
  for attempt in 1 2 3; do
    out=$(env -i PATH=/usr/bin:/bin HOME="$w/home" TMPDIR="$w/run" LC_ALL=C POWERARM_SERVERSOCKETPATH="$w/run/server.sock" \
      POWERARM_PORTABLE=1 POWERARM_ROOTFS="$rootfs" "${envs[@]}" "$emu" /usr/bin/uname -m 2>&1)
    rc=$?
    # POWERarmServer is started on demand and the client gives up after about
    # 5 s. Losing that race says nothing about the config handling under test
    # here, and it does happen on the first launch of a script run, so retry
    # rather than report it as a config failure.
    case $out in
    *"connect to POWERarmServer"*)
      sleep 1
      continue
      ;;
    esac
    break
  done
  printf '%s' "$out"
  return $rc
}

# cfg KEY VALUE : a minimal config file setting one option.
cfg() { printf '{"Config":{"%s":"%s"}}' "$1" "$2"; }

# accepted SOURCE OPTION VALUE OUT RC : the run worked and printed the guest's
# output, i.e. the value was understood.
accepted() {
  local what=$1 opt=$2 val=$3 out=$4 rc=$5
  { [ "$rc" = 0 ] && [ "$out" = aarch64 ]; } &&
    ok "bool: $what $opt=$(printf %q "$val") is accepted" ||
    bad "bool: $what $opt=$(printf %q "$val") should be accepted, got rc=$rc out='$out'"
}

# refused SOURCE OPTION VALUE OUT RC WHERE : the run stopped, and the message
# names the option, the value, where it came from and what is accepted. All
# four matter: the whole point is that the user can tell which line to go fix.
refused() {
  local what=$1 opt=$2 val=$3 out=$4 rc=$5 where=$6
  local why=
  [ "$rc" = 1 ] || why="$why rc=$rc(want 1)"
  case $out in *"'$opt'"*) ;; *) why="$why no-option-name" ;; esac
  case $out in *"'$val'"*) ;; *) why="$why no-value" ;; esac
  case $out in *"$where"*) ;; *) why="$why no-source" ;; esac
  case $out in *"on, off, yes, no"*) ;; *) why="$why no-accepted-list" ;; esac
  case $out in *aarch64*) why="$why guest-ran" ;; *) ;; esac
  [ -z "$why" ] &&
    ok "bool: $what $opt=$(printf %q "$val") is refused by name" ||
    bad "bool: $what $opt=$(printf %q "$val") refusal is wrong ($why): $(echo "$out" | tr '\n' '|')"
}

# Start the on-demand POWERarmServer once, before anything is being measured,
# so no check is the one that pays for it.
run - - -- > /dev/null

# ---------------------------------------------------------------------------
# bool: the spellings a bool option accepts, through the JSON loader.
for v in 1 on true yes ON True YES 0 off false no OFF False NO; do
  out=$(run "$(cfg VSXClasses "$v")" - --)
  accepted json VSXClasses "$v" "$out" "$?"
done

# bool: and the ones it does not. "banana" is a typo, "" is a truncated edit,
# " 1" is a stray space that no eye catches, "2" and "onn" are near misses.
for v in banana "" " 1" 2 onn -1 "1 "; do
  out=$(run "$(cfg VSXClasses "$v")" - --)
  refused json VSXClasses "$v" "$out" "$?" "$w/cfg/Config.json"
done

# bool: the same through the environment loader, which is a separate code path.
for v in 1 on true yes 0 off false no; do
  out=$(run - - POWERARM_VSXCLASSES="$v" --)
  accepted env VSXClasses "$v" "$out" "$?"
done
for v in banana "" " 1" 2 onn; do
  out=$(run - - POWERARM_VSXCLASSES="$v" --)
  refused env VSXClasses "$v" "$out" "$?" POWERARM_VSXCLASSES
done

# bool: not just VSXClasses. The check is driven off the generated OPT_BOOL
# list, so it has to hold for any of the 66 -- including ones whose default is
# true, where a rejected value would silently have meant "false" rather than
# "leave it alone".
for opt in TSOEnabled Multiblock SilentLog CodeCacheForkWriter DisableTelemetry; do
  out=$(run "$(cfg "$opt" on)" - --)
  accepted json "$opt" on "$out" "$?"
  out=$(run "$(cfg "$opt" banana)" - --)
  refused json "$opt" banana "$out" "$?" "$w/cfg/Config.json"
done
for opt in TSOEnabled Multiblock; do
  out=$(run - - "POWERARM_$(echo "$opt" | tr '[:lower:]' '[:upper:]')=banana" --)
  refused env "$opt" banana "$out" "$?" "POWERARM_$(echo "$opt" | tr '[:lower:]' '[:upper:]')"
done

# bool: a spelling that is accepted has to reach the feature, not just survive
# the parse. VSXCLASSES is hashed into the code-cache config id, so the id a
# run writes is a direct readout of the bool the JIT saw.
#
# This is the check that fails loudest against the old code: there, "on", "true"
# and "yes" all produced the OFF id, because strtoull gave 0 and nothing said so.
id_for() {
  local src=$1 val=$2
  local dir="$w/cc-$src-$val"
  rm -rf "$dir"
  mkdir -p "$dir"
  # CodeCacheForkWriter=0 so the segment is published in-process before exit.
  # With the writer on, the files land asynchronously and this helper raced it:
  # a run would see no id at all, or catch a half-written
  # '<name>-<hash>-<id>.tmp.<pid>.<n>' and compare that whole string.
  if [ "$src" = json ]; then
    run "$(cfg VSXClasses "$val")" "$dir" POWERARM_CODECACHEFORKWRITER=0 -- > /dev/null
  else
    run - "$dir" POWERARM_VSXCLASSES="$val" POWERARM_CODECACHEFORKWRITER=0 -- > /dev/null
  fi
  # Match the id as a 16-hex field at end of name, and drop .tmp/.lock outright.
  # The old pattern anchored '[0-9a-f]*$' with a star, so it matched the EMPTY
  # string at the end of any name it did not understand and passed the whole
  # filename through.
  find "$dir" -type f 2> /dev/null | sed 's#.*/##' |
    grep -v -e '\.lock$' -e '\.tmp\.' |
    sed -n 's/.*-\([0-9a-f]\{16\}\)$/\1/p' | sort -u | tr '\n' ' '
}

on_id=$(id_for json 1)
off_id=$(id_for json 0)
if [ -z "$on_id" ] || [ "$on_id" = "$off_id" ]; then
  echo "skip bool cacheid: this build wrote no distinguishable cache ids (on='$on_id' off='$off_id')"
else
  ok "bool: VSXClasses=1 and =0 write different code-cache config ids"
  for src in json env; do
    for v in on true yes ON; do
      got=$(id_for "$src" "$v")
      [ "$got" = "$on_id" ] &&
        ok "bool: $src VSXClasses=$v reaches the JIT as ON" ||
        bad "bool: $src VSXClasses=$v did not reach the JIT as ON (id '$got', want '$on_id')"
    done
    for v in off false no OFF; do
      got=$(id_for "$src" "$v")
      [ "$got" = "$off_id" ] &&
        ok "bool: $src VSXClasses=$v reaches the JIT as OFF" ||
        bad "bool: $src VSXClasses=$v did not reach the JIT as OFF (id '$got', want '$off_id')"
    done
  done
fi

# ---------------------------------------------------------------------------
# json: a config that cannot be fully applied must not be partially applied.
#
# 19f05bc10 made an unparseable file exit 1 instead of dumping core, and noted
# that tiny-json is lenient so it only covered the case where the parser gives
# up. These are the cases where it does not give up. The last two in the first
# group are the ones that silently lose a setting outright, because tiny-json
# returns the moment the top-level object closes and discards the rest.

# rejected CASE TEXT EXPECT : the file is refused, nothing is applied, and the
# message contains EXPECT.
rejected() {
  local what=$1 text=$2 expect=$3 out rc
  out=$(run "$text" - --)
  rc=$?
  local why=
  [ "$rc" = 1 ] || why="$why rc=$rc(want 1)"
  case $out in *"$expect"*) ;; *) why="$why want:$expect" ;; esac
  case $out in *aarch64*) why="$why guest-ran" ;; *) ;; esac
  [ -z "$why" ] &&
    ok "json: $what is refused" ||
    bad "json: $what should be refused ($why): $(echo "$out" | tr '\n' '|')"
}

# applied CASE TEXT : the file is good and must keep working. Everything the
# project ships, generates or documents goes through here, because a stricter
# parse is only safe if it still accepts all of it.
applied() {
  local what=$1 text=$2 out rc
  out=$(run "$text" - --)
  rc=$?
  { [ "$rc" = 0 ] && [ "$out" = aarch64 ]; } &&
    ok "json: $what is accepted" ||
    bad "json: $what should be accepted, got rc=$rc out='$(echo "$out" | tr '\n' '|')'"
}

rejected "a missing comma" \
  '{"Config":{"RootFS":"r","VSXClasses":"1"
   "Multiblock":"1"}}' "expected ',' or '}'"
rejected "a trailing comma" \
  '{"Config":{"RootFS":"r","VSXClasses":"1",}}' "expected a quoted key"
rejected "a doubled comma" \
  '{"Config":{"RootFS":"r",,"VSXClasses":"1"}}' "expected a quoted key"
rejected "text after the closing brace" \
  '{"Config":{"RootFS":"r"}} and more' "more text after the closing '}'"
rejected "a brace in the wrong place dropping an option" \
  '{"Config":{"RootFS":"r"}},"VSXClasses":"1"}' "more text after the closing '}'"
rejected "a missing colon" \
  '{"Config":{"RootFS" "r"}}' "expected ':' after the key"
rejected "an unterminated string" \
  '{"Config":{"RootFS":"r}}' "the string is never terminated"
rejected "a JSON comment" \
  '{"Config":{ /* nope */ "RootFS":"r"}}' "expected a quoted key"
rejected "a single-quoted key" \
  "{'Config':{'RootFS':'r'}}" "expected a quoted key"
rejected "a UTF-8 byte order mark" \
  "$(printf '\xEF\xBB\xBF{"Config":{"RootFS":"r"}}')" "byte order mark"
rejected "a bad escape" \
  '{"Config":{"RootFS":"a\qb"}}' 'after a backslash'

# json: valid JSON that still cannot be applied. A strict parse cannot catch
# any of these -- every one of them is well-formed and every one of them is a
# setting that silently never takes effect.
rejected "a duplicate option key" \
  '{"Config":{"RootFS":"r","VSXClasses":"1","VSXClasses":"0"}}' 'set more than once'
rejected "an unknown option key" \
  '{"Config":{"RootFS":"r","Banana":"1"}}' 'is not a POWERarm config option'
rejected "a misspelled option key" \
  '{"Config":{"RootFS":"r","VSXClases":"1"}}' 'is not a POWERarm config option'
rejected "an option outside the Config block" \
  '{"Config":{"RootFS":"r"},"VSXClasses":"1"}' 'is at the top level'
rejected "a duplicate top-level key" \
  '{"Config":{"RootFS":"r"},"Config":{"VSXClasses":"1"}}' 'more than once at the top level'
rejected "a bare number value" \
  '{"Config":{"MaxInst":50000,"RootFS":"r"}}' 'has to be a JSON string'
rejected "a bare true value" \
  '{"Config":{"VSXClasses":true}}' 'has to be a JSON string'
rejected "a null value" \
  '{"Config":{"VSXClasses":null}}' 'has to be a JSON string'
rejected "an array value" \
  '{"Config":{"Env":["A=1"]}}' 'has to be a JSON string'
rejected "a Config block that is not an object" \
  '{"Config":"RootFS"}' 'has to be a JSON object'
rejected "an unknown key in an AppOverrides block" \
  '{"Config":{"RootFS":"r"},"AppOverrides":{"someotherprogram":{"Banana":"1"}}}' 'is not a POWERarm config option'

# json: and everything that must keep working.
applied "the config shape the owner runs" \
  '{"Config":{"RootFS":"r","CodeCacheForkWriter":"0","VSXClasses":"1","NZCVExitDead":"canary"}}'
# packaging/archpower/PKGBUILD writes this as the global layer, so it is read on
# every launch of an installed POWERarm. Its ThunksDB values are bare integers,
# which json_getInteger wants -- the string rule is for option blocks only.
applied "the global config the package ships" \
  '{
  "Config": {},
  "ThunksDB": {
    "Vulkan": 1,
    "GL": 1,
    "EGL": 1,
    "WaylandClient": 1,
    "drm": 1,
    "xshmfence": 1,
    "asound": 0
  }
}'
# A repeated key is how SaveLayerToJSON encodes a string-array option (one
# json_str per element), so Env/HostEnv/AdditionalArguments must be exempt from
# the duplicate-key rule or POWERarmConfig and POWERarmRootFSFetcher would
# write files their own loader refuses.
applied "repeated keys for a string-array option" \
  '{"Config":{"RootFS":"r","Env":"A=1","Env":"B=2","HostEnv":"C=3"},"ThunksDB":{"GL":1}}'
applied "the README example" '{ "Config": { "RootFS": "ArchLinuxARM-m2" } }'
applied "the ENV_REFERENCE example" '{ "Config": { "SMCChecks": "mtrack", "MaxInst": "50000" } }'
applied "the APPS-TUI-DESIGN example" \
  '{
  "Config": {
    "RootFS": "r",
    "EnableCodeCachingWIP": "1",
    "CodeCacheScope": "home",
    "DisableCmpBranchFusion": "0",
    "ProfileStats": "0"
  }
}'
applied "an AppOverrides block" \
  '{"Config":{"RootFS":"r"},"AppOverrides":{"*uname*":{"MaxInst":"500"},"chrome":{"TSOEnabled":"1"}}}'
applied "an empty Config block" '{"Config":{}}'
applied "an empty document" '{}'
applied "a file with no Config section" '{"ThunksDB":{"GL":1}}'
applied "odd but legal whitespace" '{
   "Config"  :  {
       "RootFS" : "r"
   }
}'
applied "escapes in a value" '{"Config":{"RootFS":"r","OutputLog":"stderr","ThunkHostLibs":"/a\/bA"}}'

# An empty file was already covered by 19f05bc10 and must stay covered.
rejected "an empty file" '' 'not valid JSON'

# ---------------------------------------------------------------------------
# The normal path still works.
out=$(run '{"Config":{"CodeCacheForkWriter":"0","VSXClasses":"1","NZCVExitDead":"canary"}}' - --)
rc=$?
{ [ "$rc" = 0 ] && [ "$out" = aarch64 ]; } &&
  ok "normal: a valid config runs the guest" || bad "normal: a valid config failed, rc=$rc out='$out'"

out=$(run - - --)
rc=$?
{ [ "$rc" = 0 ] && [ "$out" = aarch64 ]; } &&
  ok "normal: no config file at all runs the guest" || bad "normal: no config file failed, rc=$rc out='$out'"

exit $fail
