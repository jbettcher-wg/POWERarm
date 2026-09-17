#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Builds an independent reference for the generated ABI tables by compiling the
constant names with the NATIVE compiler against the machine's INSTALLED uapi
headers (linux-libc-dev / linux-api-headers) and running the result.

Run it on real arm64 hardware for the guest side and on the POWER9 for the host
side:

  arm64:   gen_reference.py --role guest --out Arm64Reference.inc
  ppc64le: gen_reference.py --role host  --out Ppc64leReference.inc

The unit tests compare every GUEST_*/HOST_* constant and the stat/termios
layouts in GeneratedABI.h against these files. Names the installed headers do
not have (they can be older than the kernel tree the tables came from) are
left out and counted in the file's header comment.
"""

import argparse
import os
import platform
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

HEADERS = [
    'linux/types.h', 'asm/fcntl.h', 'linux/mman.h', 'asm/signal.h', 'asm/siginfo.h', 'asm/stat.h',
    'asm/termios.h', 'linux/serial.h', 'asm/ioctl.h', 'asm/errno.h', 'asm/socket.h', 'asm/sockios.h',
    'linux/auxvec.h', 'asm/resource.h', 'asm/poll.h',
]
GUEST_ONLY_HEADERS = ['asm/hwcap.h']

LAYOUTS = {
    'guest': [
        ('Stat', 'struct stat', ['st_dev', 'st_ino', 'st_mode', 'st_nlink', 'st_uid', 'st_gid', 'st_rdev', 'st_size',
                                 'st_blksize', 'st_blocks', 'st_atime', 'st_atime_nsec', 'st_mtime', 'st_mtime_nsec',
                                 'st_ctime', 'st_ctime_nsec']),
        ('Termios', 'struct termios', ['c_iflag', 'c_oflag', 'c_cflag', 'c_lflag', 'c_line', 'c_cc']),
        ('Termios2', 'struct termios2', ['c_iflag', 'c_oflag', 'c_cflag', 'c_lflag', 'c_line', 'c_cc', 'c_ispeed',
                                         'c_ospeed']),
    ],
    'host': [
        ('Stat', 'struct stat', ['st_dev', 'st_ino', 'st_mode', 'st_nlink', 'st_uid', 'st_gid', 'st_rdev', 'st_size',
                                 'st_blksize', 'st_blocks', 'st_atime', 'st_atime_nsec', 'st_mtime', 'st_mtime_nsec',
                                 'st_ctime', 'st_ctime_nsec']),
        ('Termios', 'struct termios', ['c_iflag', 'c_oflag', 'c_cflag', 'c_lflag', 'c_line', 'c_cc', 'c_ispeed',
                                       'c_ospeed']),
    ],
}


def member(name):
    if re.fullmatch(r'st_[amc]time', name):
        return name + '_sec'
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--role', choices=['guest', 'host'], required=True)
    ap.add_argument('--names', default=os.path.join(HERE, 'ConstantNames.txt'))
    ap.add_argument('--out', required=True)
    ap.add_argument('--cc', default='cc')
    args = ap.parse_args()

    want = {'guest': 'aarch64', 'host': 'ppc64le'}[args.role]
    if platform.machine() != want:
        print('gen_reference.py: --role %s must run natively on %s (this is %s)' % (args.role, want, platform.machine()),
              file=sys.stderr)
        return 1

    prefix = 'GUEST_' if args.role == 'guest' else 'HOST_'
    names = []
    for line in open(args.names):
        cname, name, fam = line.split()
        if cname.startswith(prefix):
            names.append((cname, name))

    headers = HEADERS + (GUEST_ONLY_HEADERS if args.role == 'guest' else [])
    work = tempfile.mkdtemp(prefix='powerarm-ref-')
    src = os.path.join(work, 'ref.c')
    exe = os.path.join(work, 'ref')
    dropped = set()
    while True:
        lines = ['#include <stdio.h>', '#include <stddef.h>']
        lines += ['#include <%s>' % h for h in headers]
        lines += ['typedef __kernel_loff_t loff_t;', 'int main(void) {']
        linemap = {}
        for cname, name in names:
            if cname in dropped:
                continue
            lines.append('#ifdef %s' % name)
            linemap[len(lines) + 1] = cname
            lines.append('  printf("REF(%s, 0x%%llxULL)\\n", (unsigned long long)(%s));' % (cname, name))
            lines.append('#endif')
        for sname, ctype, fields in LAYOUTS[args.role]:
            lines.append('  printf("SIZE(%s%s, %%zu)\\n", sizeof(%s));' % (prefix.title().rstrip('_'), sname, ctype))
            for f in fields:
                lines.append('  printf("FIELD(%s%s, %s, %%zu, %%zu)\\n", offsetof(%s, %s), sizeof(((%s*)0)->%s));' %
                             (prefix.title().rstrip('_'), sname, member(f), ctype, f, ctype, f))
        lines += ['  return 0;', '}']
        open(src, 'w').write('\n'.join(lines) + '\n')
        p = subprocess.run([args.cc, '-O0', '-w', src, '-o', exe], capture_output=True, text=True)
        if p.returncode == 0:
            break
        bad = set()
        for m in re.finditer(r'ref\.c:(\d+):\d+: error', p.stderr):
            ln = int(m.group(1))
            if ln in linemap:
                bad.add(linemap[ln])
        if not bad:
            print(p.stderr, file=sys.stderr)
            return 1
        dropped |= bad
    out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout
    present = out.count('REF(')
    uname = os.uname()
    header = [
        '// SPDX-License-Identifier: MIT',
        '// Generated by Tests/gen_reference.py --role %s on %s (%s %s) from the installed uapi headers.' %
        (args.role, uname.machine, uname.sysname, uname.release),
        '// %d of %d names present; %d did not evaluate with these headers.' %
        (present, len(names), len(dropped)),
    ]
    open(args.out, 'w').write('\n'.join(header) + '\n' + out)
    print('%s: %d constants, %d dropped' % (args.out, present, len(dropped)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
