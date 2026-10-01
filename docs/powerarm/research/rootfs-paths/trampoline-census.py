#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# trampoline-census.py <strace output> [rootfs]
#
# Counts the FileManager::Readlink/Readlinkat trampoline in an strace of a
# guest run and classifies what each instance asked about -- the numbers in
# section 1 of ROOTFS-PATH-CACHE.md. The trampoline is the triple
#
#   openat2(RootFSFD, rel, {O_PATH|O_NOFOLLOW|O_CLOEXEC, RESOLVE_IN_ROOT}) = fd
#   readlinkat(fd, "", ...)
#   close(fd)
#
# Produce the trace with, for example (POWERARM_PORTABLE=1 matters -- see
# section 8 of the document):
#
#   strace -f -e trace=openat2,openat,readlinkat,close,newfstatat,access,statx \
#     -o trace.txt env POWERARM_ROOTFS=$SR POWERARM_PORTABLE=1 \
#     <build>/Bin/POWERarm /usr/bin/gcc -O2 -c foo.c
import collections
import os
import re
import stat
import sys

Trace = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
Root = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser(
    '~/.local/share/powerarm/RootFS/ArchLinuxARM-m2')

Lines = open(Trace).read().splitlines()
Paths = []
for i in range(len(Lines) - 2):
    a, b, c = Lines[i], Lines[i + 1], Lines[i + 2]
    m = re.search(r'openat2\(\d+, "([^"]*)".*O_PATH.*= \d+$', a)
    if m and re.search(r'readlinkat\(\d+, ""', b) and 'close(' in c:
        Paths.append(m.group(1))

Kinds = collections.Counter()
Uniq = collections.Counter(Paths)
for p in Paths:
    try:
        St = os.lstat(os.path.join(Root, p))
    except OSError:
        Kinds['missing'] += 1
        continue
    if stat.S_ISDIR(St.st_mode):
        Kinds['dir'] += 1
    elif stat.S_ISLNK(St.st_mode):
        Kinds['symlink'] += 1
    else:
        Kinds['other'] += 1

Total = len(Paths)
if not Total:
    print('no trampolines found in', Trace)
    sys.exit(0)
print('trampolines: %d (%d host syscalls)  distinct paths: %d'
      % (Total, 3 * Total, len(Uniq)))
for k, v in Kinds.most_common():
    print('  %-8s %5d  %5.1f%%' % (k, v, 100.0 * v / Total))
print('\nmost repeated:')
for p, n in Uniq.most_common(8):
    print('  %4d  %s' % (n, p))
