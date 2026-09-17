# SPDX-License-Identifier: MIT
# M1 busybox applet script.  Run as:  busybox sh applets.sh BUSYBOX CORPUS
# Output must be byte-identical to the golden run.  Keep it free of anything
# that legitimately differs between machines: absolute paths, times, owners,
# inode numbers, readdir order (pipe find/ls through sort).
set -e
BB=$1
C=$2
W=$(pwd)

section() { echo "== $1"; }

section echo
$BB echo plain words
$BB echo -n "no newline|"; echo
$BB echo -e 'tab\there\nnewline'

section cat
$BB cat "$C/tree/b.txt" "$C/tree/a.txt"
$BB cat < "$C/tree/sub/c.txt" | $BB wc -l

section ls
cd "$C/tree"
$BB ls -1
$BB ls -1a sub/deep
$BB ls -R | $BB sort
cd "$W"

section wc
$BB wc < "$C/lorem.txt"
$BB wc -w -l -c < "$C/words.txt"

section sort
$BB sort "$C/words.txt" | $BB uniq -c | $BB head -n 8
$BB sort -k2,2n "$C/numbers.txt" | $BB head -n 5
$BB sort -t: -k3,3nr "$C/colon.txt" | $BB cut -d: -f1 | $BB head -n 4

section sums
$BB sha256sum < "$C/lorem.txt"
$BB md5sum < "$C/tree/sub/deep/d.dat"

section grep
$BB grep -n charlie "$C/lorem.txt" | $BB head -n 5
$BB grep -c -E '^(alpha|zulu)' "$C/words.txt"
$BB grep -v -i -e a -e e "$C/words.txt" | $BB sort -u
if $BB grep -q nomatch-xyz "$C/lorem.txt"; then echo matched; else echo "no match rc=$?"; fi

section sed
$BB sed -n '3,6p' "$C/lorem.txt"
$BB sed -e 's/a/A/g' -e 's/^\([a-z]*\) /[\1] /' "$C/lorem.txt" | $BB tail -n 3
$BB sed '/^$/d; y/abc/XYZ/' "$C/tree/b.txt"

section awk
$BB awk '{ s += $2; n++ } END { print n, s }' "$C/numbers.txt"
$BB awk -F: '$3 % 2 == 0 { printf "%s-%d\n", $1, $3 }' "$C/colon.txt"
$BB awk '{ c[$1]++ } END { for (k in c) print k, c[k] }' "$C/words.txt" | $BB sort

section find
cd "$C"
$BB find tree | $BB sort
$BB find tree -type f -name '*.txt' | $BB sort
$BB find tree -type d | $BB sort
cd "$W"

section tar
$BB tar -C "$C" -cf - tree | $BB tar -tf - | $BB sort
mkdir -p x
$BB tar -C "$C" -cf - tree | $BB tar -C x -xf -
cd x
$BB find tree -type f | $BB sort | while read -r f; do $BB sha256sum "$f"; done
cd "$W"

section gzip
$BB gzip -c < "$C/lorem.txt" | $BB md5sum
$BB gzip -9 -c < "$C/tree/sub/deep/d.dat" | $BB gzip -dc | $BB sha256sum
$BB gzip -c < "$C/words.txt" > w.gz
$BB gzip -t w.gz && echo "gzip -t ok"
$BB zcat w.gz | $BB tail -n 2

section sh
i=0; while [ $i -lt 4 ]; do printf '%d:' $i; i=$((i + 1)); done; echo
f() { echo "$#[$*]"; return 3; }
f a "b c" d || echo "f rc=$?"
case "hello.txt" in *.txt) echo text ;; *) echo other ;; esac
x=$($BB echo nested $($BB echo deeper)); echo "$x"
set +e; (exit 5); echo "subshell rc=$?"
echo done
