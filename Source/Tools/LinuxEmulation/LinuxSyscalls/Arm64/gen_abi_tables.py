#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Generates the arm64 (guest) <-> ppc64le (host) Linux user-ABI translation
tables from a kernel tree's uapi headers.

Nothing here is hand-typed. For each constant family the script:

  1. collects every object-like #define from the listed uapi headers, as each
     architecture resolves them (arch/<arch>/include/uapi first, then the
     asm-generic fallbacks the uapi Kbuild files declare);
  2. requires every collected name to match exactly one classification rule
     (flag bit, field, field value, scalar, must-be-identical, alias, ...).
     A name no rule claims, or a value that contradicts its rule, stops the
     run with an error;
  3. evaluates the values with the real target ABI by compiling the headers
     for aarch64-linux-gnu and powerpc64le-linux-gnu and reading the constants
     back out of the object files (nothing is executed, so no cross toolchain
     or emulator is needed).

Struct layouts come from the same object-file probe (sizeof/offsetof of every
field the header declares).

Outputs:
  --header  C++ header with GUEST_*/HOST_* constants, flag and field tables,
            the ioctl table, errno fixups, struct layouts and converters.
  --doc     Markdown list of every constant and layout that differs.
  --names   Plain list of the emitted constant names (input for the native
            reference probes used by the unit tests).

Usage:
  gen_abi_tables.py --kernel ~/Development/linux-7.2.6 \
      --header GeneratedABI.h --doc ABI-DIFFERENCES.md [--cc clang]
  gen_abi_tables.py ... --check    (regenerate into a temp dir and diff)
"""

import argparse
import difflib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile


class GenError(Exception):
    pass


def fail(msg):
    raise GenError(msg)


ROLES = {
    'guest': dict(label='arm64', srcarch='arm64', triple='aarch64-linux-gnu'),
    'host': dict(label='ppc64le', srcarch='powerpc', triple='powerpc64le-linux-gnu'),
}

# ---------------------------------------------------------------------------
# Header environments
# ---------------------------------------------------------------------------


def parse_kbuild(path, var):
    out = []
    if not os.path.exists(path):
        return out
    for line in open(path):
        m = re.match(r'\s*' + re.escape(var) + r'\s*\+=\s*(\S+)', line)
        if m:
            out.append(m.group(1))
    return out


class Env:
    def __init__(self, kernel, role, cc, work):
        a = ROLES[role]
        self.role = role
        self.label = a['label']
        self.triple = a['triple']
        self.cc = cc
        self.work = work
        self.arch_uapi = os.path.join(kernel, 'arch', a['srcarch'], 'include', 'uapi')
        self.gen_uapi = os.path.join(kernel, 'include', 'uapi')
        self.shim = os.path.join(work, role + '-shim')
        os.makedirs(os.path.join(self.shim, 'asm'), exist_ok=True)
        os.makedirs(os.path.join(self.shim, 'linux'), exist_ok=True)
        mandatory = parse_kbuild(os.path.join(self.gen_uapi, 'asm-generic', 'Kbuild'), 'mandatory-y')
        generic = parse_kbuild(os.path.join(self.arch_uapi, 'asm', 'Kbuild'), 'generic-y')
        if not mandatory:
            fail('no mandatory-y list in include/uapi/asm-generic/Kbuild')
        self.generic_asm = set()
        for h in sorted(set(mandatory) | set(generic)):
            if h in generic or not os.path.exists(os.path.join(self.arch_uapi, 'asm', h)):
                self.generic_asm.add(h)
                with open(os.path.join(self.shim, 'asm', h), 'w') as f:
                    f.write('#include <asm-generic/%s>\n' % h)
        # headers_install.sh strips this include; the uapi tree still has it.
        open(os.path.join(self.shim, 'linux', 'compiler.h'), 'w').close()
        self.counter = 0

    def resolve(self, name):
        """Real file an #include <name> reaches for this architecture."""
        if name.startswith('asm/'):
            h = name[4:]
            if h in self.generic_asm:
                return os.path.join(self.gen_uapi, 'asm-generic', h)
            p = os.path.join(self.arch_uapi, 'asm', h)
            if os.path.exists(p):
                return p
            fail('%s: <%s> does not exist' % (self.label, name))
        p = os.path.join(self.gen_uapi, name)
        if not os.path.exists(p):
            fail('%s: <%s> does not exist' % (self.label, name))
        return p

    def cflags(self):
        # Mirrors scripts/headers_install.sh (annotation stripping, unifdef).
        return ['--target=' + self.triple, '-nostdinc', '-ffreestanding', '-std=gnu11', '-w',
                '-I', self.shim, '-I', self.arch_uapi, '-I', self.gen_uapi,
                '-U__KERNEL__', '-D__EXPORTED_HEADERS__', '-D__user=', '-D__force=', '-D__iomem=',
                '-D__attribute_const__=', '-D__packed=__attribute__((packed))']

    def _tu(self, text):
        self.counter += 1
        path = os.path.join(self.work, '%s-%d.c' % (self.role, self.counter))
        with open(path, 'w') as f:
            f.write(text)
        return path

    def run(self, args, what):
        p = subprocess.run([self.cc] + self.cflags() + args, capture_output=True, text=True)
        return p

    def macros(self, includes):
        src = self._tu(include_text(includes))
        p = self.run(['-E', '-dM', src], 'macros')
        if p.returncode != 0:
            fail('%s: preprocessing %s failed:\n%s' % (self.label, includes, p.stderr))
        out = {}
        for line in p.stdout.splitlines():
            m = re.match(r'#define\s+(\w+)(\()?\s*(.*)$', line)
            if m:
                out[m.group(1)] = (m.group(2) is not None, m.group(3))
        return out

    def preprocess(self, includes):
        src = self._tu(include_text(includes))
        p = self.run(['-E', '-P', src], 'preprocess')
        if p.returncode != 0:
            fail('%s: preprocessing %s failed:\n%s' % (self.label, includes, p.stderr))
        return p.stdout

    def eval_exprs(self, includes, exprs):
        """Evaluates integer constant expressions. Returns (values, failed) where
        failed maps index -> diagnostic for expressions that do not compile."""
        pending = list(range(len(exprs)))
        failed = {}
        while True:
            head = include_text(includes)
            head += '#define powerarm_offsetof(T, f) __builtin_offsetof(T, f)\n'
            body = ['__attribute__((used, section(".powerarm_abi")))',
                    'static const unsigned long long powerarm_abi_values[] = {']
            first = head.count('\n') + len(body) + 1
            linemap = {}
            for n, idx in enumerate(pending):
                linemap[first + n] = idx
                body.append('  (unsigned long long)(%s),' % exprs[idx])
            body.append('  0ULL };')
            src = self._tu(head + '\n'.join(body) + '\n')
            obj = src[:-2] + '.o'
            p = self.run(['-c', src, '-o', obj], 'eval')
            if p.returncode == 0:
                break
            bad = set()
            for m in re.finditer(r'^%s:(\d+):\d+: error: (.*)$' % re.escape(src), p.stderr, re.M):
                ln = int(m.group(1))
                if ln in linemap:
                    bad.add(linemap[ln])
                    failed.setdefault(linemap[ln], m.group(2))
            if not bad:
                fail('%s: evaluation compile failed outside the value table:\n%s' % (self.label, p.stderr))
            pending = [i for i in pending if i not in bad]
            if not pending:
                return {}, failed
        raw = read_elf_section(obj, '.powerarm_abi')
        vals = struct.unpack('<%dQ' % (len(raw) // 8), raw)
        if len(vals) != len(pending) + 1:
            fail('%s: value table size mismatch' % self.label)
        return {idx: vals[n] for n, idx in enumerate(pending)}, failed


def include_text(includes):
    # An entry starting with '!' is a raw source line (e.g. a typedef userspace
    # normally supplies), everything else is a header name.
    return ''.join((i[1:] + '\n') if i.startswith('!') else ('#include <%s>\n' % i) for i in includes)


def read_elf_section(path, name):
    data = open(path, 'rb').read()
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        fail('%s: not an ELF64 little-endian object' % path)
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)

    def sh(i):
        return struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize)

    strtab = sh(shstrndx)
    stroff = strtab[4]
    for i in range(shnum):
        s = sh(i)
        end = data.index(b'\0', stroff + s[0])
        if data[stroff + s[0]:end].decode() == name:
            return data[s[4]:s[4] + s[5]]
    fail('%s: no %s section' % (path, name))


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------

BIT = 'bit'              # single-bit flag, translated by name
ZERO = 'zero'            # a flag whose value is 0 on both (O_RDONLY, MAP_FILE)
FIELD = 'field'          # multi-bit field mask; its values are FIELDVAL rules
FIELDVAL = 'fieldval'    # a value of a field (extra = mask name)
SCALAR = 'scalar'        # enumerated number, translated by name when it differs
SAME = 'same'            # must be identical: the handlers pass it through untranslated
INFO = 'info'            # recorded only (limits, feature bits, sizes)
ALIAS = 'alias'          # another name for a constant (extra = target)
COMPOSITE = 'composite'  # an OR of bits of the same group
NONINT = 'nonint'        # not an integer constant expression in a bare uapi context
IOCTL = 'ioctl'          # terminal ioctl number (extra = argument kind)
IGNORE = 'ignore'        # header plumbing

ALL_KINDS = {BIT, ZERO, FIELD, FIELDVAL, SCALAR, SAME, INFO, ALIAS, COMPOSITE, NONINT, IOCTL, IGNORE}


class Rule:
    def __init__(self, pattern, kind, group=None, extra=None):
        assert kind in ALL_KINDS
        self.rx = re.compile('(?:%s)\\Z' % pattern)
        self.pattern = pattern
        self.kind = kind
        self.group = group
        self.extra = extra


def R(pattern, kind, group=None, extra=None):
    return Rule(pattern, kind, group, extra)


GUARDS = R(r'_+[A-Z0-9_]*_H_?|_UAPI\w*_H|__ASM\w*_H', IGNORE)

# Argument kinds for terminal ioctls. 'plain' means the argument (a value, or a
# pointer to int/char/pid/struct winsize, all of identical layout) is passed
# through and only the request number is translated.
IOCTL_ARGS = {
    'TCGETS': 'termios_get', 'TCSETS': 'termios_set', 'TCSETSW': 'termios_set', 'TCSETSF': 'termios_set',
    'TCGETS2': 'termios2_get', 'TCSETS2': 'termios2_set', 'TCSETSW2': 'termios2_set', 'TCSETSF2': 'termios2_set',
    'TCGETA': 'unsupported', 'TCSETA': 'unsupported', 'TCSETAW': 'unsupported', 'TCSETAF': 'unsupported',
    'TCSBRK': 'plain', 'TCXONC': 'plain', 'TCFLSH': 'plain', 'TIOCEXCL': 'plain', 'TIOCNXCL': 'plain',
    'TIOCSCTTY': 'plain', 'TIOCGPGRP': 'plain', 'TIOCSPGRP': 'plain', 'TIOCOUTQ': 'plain', 'TIOCSTI': 'plain',
    'TIOCGWINSZ': 'plain', 'TIOCSWINSZ': 'plain', 'TIOCMGET': 'plain', 'TIOCMBIS': 'plain', 'TIOCMBIC': 'plain',
    'TIOCMSET': 'plain', 'TIOCGSOFTCAR': 'plain', 'TIOCSSOFTCAR': 'plain', 'FIONREAD': 'plain',
    'TIOCLINUX': 'plain', 'TIOCCONS': 'plain', 'TIOCGSERIAL': 'unsupported', 'TIOCSSERIAL': 'unsupported',
    'TIOCPKT': 'plain', 'FIONBIO': 'plain', 'TIOCNOTTY': 'plain', 'TIOCSETD': 'plain', 'TIOCGETD': 'plain',
    'TCSBRKP': 'plain', 'TIOCSBRK': 'plain', 'TIOCCBRK': 'plain', 'TIOCGSID': 'plain',
    'TIOCGRS485': 'unsupported', 'TIOCSRS485': 'unsupported', 'TIOCGPTN': 'plain', 'TIOCSPTLCK': 'plain',
    'TIOCGDEV': 'plain', 'TCGETX': 'unsupported', 'TCSETX': 'unsupported', 'TCSETXF': 'unsupported',
    'TCSETXW': 'unsupported', 'TIOCSIG': 'plain', 'TIOCVHANGUP': 'plain', 'TIOCGPKT': 'plain',
    'TIOCGPTLCK': 'plain', 'TIOCGEXCL': 'plain', 'TIOCGPTPEER': 'openflags',
    'TIOCGISO7816': 'unsupported', 'TIOCSISO7816': 'unsupported', 'FIONCLEX': 'plain', 'FIOCLEX': 'plain',
    'FIOASYNC': 'plain', 'TIOCSERCONFIG': 'plain', 'TIOCSERGWILD': 'unsupported', 'TIOCSERSWILD': 'unsupported',
    'TIOCGLCKTRMIOS': 'unsupported', 'TIOCSLCKTRMIOS': 'unsupported', 'TIOCSERGSTRUCT': 'unsupported',
    'TIOCSERGETLSR': 'plain', 'TIOCSERGETMULTI': 'unsupported', 'TIOCSERSETMULTI': 'unsupported',
    'TIOCMIWAIT': 'plain', 'TIOCGICOUNT': 'unsupported',
    # powerpc-only legacy requests; an arm64 guest cannot name them.
    'TIOCGETP': 'hostonly', 'TIOCSETP': 'hostonly', 'TIOCSETN': 'hostonly', 'TIOCSETC': 'hostonly',
    'TIOCGETC': 'hostonly', 'TIOCSTART': 'hostonly', 'TIOCSTOP': 'hostonly', 'TIOCGLTC': 'hostonly',
    'TIOCSLTC': 'hostonly', 'FIOQSIZE': 'plain',
}


class Family:
    def __init__(self, name, includes, scan, rules, title, roles=('guest', 'host')):
        self.roles = roles
        self.name = name
        self.includes = includes
        self.scan = scan
        self.rules = rules + [GUARDS]
        self.title = title


IFLAG = 'IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK|ISTRIP|INLCR|IGNCR|ICRNL|IUCLC|IXON|IXANY|IXOFF|IMAXBEL|IUTF8'
OFLAG_BITS = 'OPOST|OLCUC|ONLCR|OCRNL|ONOCR|ONLRET|OFILL|OFDEL'
CFLAG_BITS = 'CSTOPB|CREAD|PARENB|PARODD|HUPCL|CLOCAL|ADDRB|CMSPAR|CRTSCTS'
LFLAG = 'ISIG|ICANON|XCASE|ECHO|ECHOE|ECHOK|ECHONL|NOFLSH|TOSTOP|ECHOCTL|ECHOPRT|ECHOKE|FLUSHO|PENDIN|IEXTEN|EXTPROC'
CC = 'VINTR|VQUIT|VERASE|VKILL|VEOF|VTIME|VMIN|VSWTC|VSTART|VSTOP|VSUSP|VEOL|VREPRINT|VDISCARD|VWERASE|VLNEXT|VEOL2'

FAMILIES = [
    Family('fcntl', ['asm/fcntl.h'], ['asm/fcntl.h', 'asm-generic/fcntl.h'], [
        R(r'O_ACCMODE', FIELD, 'open'),
        R(r'O_RDONLY', FIELDVAL, 'open', 'O_ACCMODE'),
        R(r'O_WRONLY|O_RDWR', FIELDVAL, 'open', 'O_ACCMODE'),
        R(r'O_SYNC|O_TMPFILE', COMPOSITE, 'open'),
        R(r'O_NDELAY', ALIAS, 'open', 'O_NONBLOCK'),
        R(r'O_\w+|__O_SYNC|__O_TMPFILE|FASYNC', BIT, 'open'),
        R(r'F_(DUPFD|GETFD|SETFD|GETFL|SETFL|GETLK|SETLK|SETLKW|SETOWN|GETOWN|SETSIG|GETSIG|GETLK64|SETLK64|SETLKW64|'
          r'SETOWN_EX|GETOWN_EX|GETOWNER_UIDS|OFD_GETLK|OFD_SETLK|OFD_SETLKW)', SAME, 'fcntl_cmd'),
        R(r'F_OWNER_\w+|FD_CLOEXEC|F_RDLCK|F_WRLCK|F_UNLCK|F_EXLCK|F_SHLCK|LOCK_\w+|F_LINUX_SPECIFIC_BASE',
          SAME, 'fcntl_arg'),
    ], 'open flags and fcntl'),
    Family('mman', ['linux/mman.h'],
           ['asm/mman.h', 'asm-generic/mman.h', 'asm-generic/mman-common.h', 'linux/mman.h'], [
        R(r'PROT_NONE', ZERO, 'prot'),
        R(r'PROT_\w+', BIT, 'prot'),
        R(r'MAP_TYPE', FIELD, 'map'),
        R(r'MAP_SHARED|MAP_PRIVATE|MAP_SHARED_VALIDATE|MAP_DROPPABLE', FIELDVAL, 'map', 'MAP_TYPE'),
        R(r'MAP_FILE', ZERO, 'map'),
        R(r'MAP_RENAME', ALIAS, 'map', 'MAP_ANONYMOUS'),
        R(r'MAP_HUGE_SHIFT|MAP_HUGE_MASK|MAP_HUGE_\d+[KMG]B', SAME, 'map_huge'),
        R(r'MAP_\w+', BIT, 'map'),
        R(r'MCL_\w+', BIT, 'mcl'),
        R(r'MLOCK_ONFAULT|MS_ASYNC|MS_INVALIDATE|MS_SYNC|MADV_\w+|MREMAP_\w+|OVERCOMMIT_\w+', SAME, 'mm'),
        R(r'PKEY_DISABLE_READ', INFO, 'pkey'),
        R(r'PKEY_UNRESTRICTED|PKEY_DISABLE_ACCESS|PKEY_DISABLE_WRITE|PKEY_DISABLE_EXECUTE', SAME, 'pkey'),
        R(r'PKEY_ACCESS_MASK', INFO, 'pkey'),
        R(r'SHADOW_STACK_SET_TOKEN|SHADOW_STACK_SET_MARKER', INFO, 'shstk'),
    ], 'mmap, mprotect, mlockall and madvise'),
    Family('signal', ['asm/signal.h', 'asm/siginfo.h'],
           ['asm/signal.h', 'asm-generic/signal.h', 'asm-generic/signal-defs.h', 'asm/siginfo.h',
            'asm-generic/siginfo.h'], [
        R(r'SA_\w+', SAME, 'sa'),
        R(r'SIG_BLOCK|SIG_UNBLOCK|SIG_SETMASK', SAME, 'sigprocmask'),
        R(r'SIG_DFL|SIG_IGN|SIG_ERR', SAME, 'handler'),
        R(r'SIGIOT', ALIAS, 'signo', 'SIGABRT'),
        R(r'SIGPOLL', ALIAS, 'signo', 'SIGIO'),
        R(r'MINSIGSTKSZ|SIGSTKSZ', INFO, 'stack'),
        R(r'SIG[A-Z0-9]+', SAME, 'signo'),
        R(r'_NSIG|_NSIG_BPW|_NSIG_WORDS', SAME, 'signo'),
        R(r'SIG_DBG_\w+', INFO, 'ppc32'),
        R(r'__ARCH_HAS_SA_RESTORER', INFO, 'sa'),
        R(r'__ARCH_SI_\w+|__SI_\w+|__ARCH_SIGEV_PREAMBLE_SIZE|__SIGEV_MAX_SIZE|SIGEV_MAX_SIZE|SIGEV_PAD_SIZE|'
          r'SI_MAX_SIZE|SI_PAD_SIZE|_SIGCHLD|_SIGFAULT|_SIGPOLL|_SIGSYS|_SIGINFO_COMMON|_KILL|_TIMER|_RT|'
          r'__SIGINFO|__SI_FAULT', INFO, 'siginfo'),
        R(r'si_\w+|sigev_notify_function|sigev_notify_attributes|sigev_notify_thread_id', INFO, 'siginfo_field'),
        R(r'SI_\w+|ILL_\w+|FPE_\w+|SEGV_\w+|BUS_\w+|TRAP_\w+|CLD_\w+|POLL_\w+|SYS_SECCOMP|SYS_USER_DISPATCH|'
          r'NSIG\w+|EMT_\w+|SIGEV_\w+|__FPE_\w+|__ILL_\w+|__SEGV_\w+|__ADDR_BND_PKEY_PAD', SAME, 'siginfo_code'),
    ], 'signals'),
    Family('stat', ['asm/stat.h'], ['asm/stat.h', 'asm-generic/stat.h'], [
        R(r'STAT_HAVE_NSEC', SAME, 'stat'),
    ], 'struct stat'),
    Family('termios', ['linux/types.h', 'asm/termios.h', 'linux/serial.h', '!typedef __kernel_loff_t loff_t;'],
           ['asm/termbits.h', 'asm-generic/termbits.h', 'asm-generic/termbits-common.h', 'asm/termios.h',
            'asm-generic/termios.h', 'asm/ioctls.h', 'asm-generic/ioctls.h'], [
        R(IFLAG, BIT, 'c_iflag'),
        R(OFLAG_BITS, BIT, 'c_oflag'),
        R(r'NLDLY|CRDLY|TABDLY|BSDLY|VTDLY|FFDLY', FIELD, 'c_oflag'),
        R(r'NL[0-3]', FIELDVAL, 'c_oflag', 'NLDLY'),
        R(r'CR[0-3]', FIELDVAL, 'c_oflag', 'CRDLY'),
        R(r'TAB[0-3]', FIELDVAL, 'c_oflag', 'TABDLY'),
        R(r'XTABS', ALIAS, 'c_oflag', 'TAB3'),
        R(r'BS[01]', FIELDVAL, 'c_oflag', 'BSDLY'),
        R(r'VT[01]', FIELDVAL, 'c_oflag', 'VTDLY'),
        R(r'FF[01]', FIELDVAL, 'c_oflag', 'FFDLY'),
        R(r'CBAUD|CSIZE|CIBAUD', FIELD, 'c_cflag'),
        R(r'B\d+|BOTHER', FIELDVAL, 'c_cflag', 'CBAUD'),
        R(r'EXTA', ALIAS, 'c_cflag', 'B19200'),
        R(r'EXTB', ALIAS, 'c_cflag', 'B38400'),
        R(r'CBAUDEX', INFO, 'c_cflag'),
        R(r'CS[5-8]', FIELDVAL, 'c_cflag', 'CSIZE'),
        R(CFLAG_BITS, BIT, 'c_cflag'),
        R(r'IBSHIFT', SAME, 'c_cflag'),
        R(LFLAG, BIT, 'c_lflag'),
        R(CC, SCALAR, 'c_cc'),
        R(r'NCCS', SAME, 'c_cc'),
        R(r'NCC|_V[A-Z0-9]+', INFO, 'termio'),
        R(r'TCSANOW|TCSADRAIN|TCSAFLUSH|TCOOFF|TCOON|TCIOFF|TCION|TCIFLUSH|TCOFLUSH|TCIOFLUSH', SAME, 'tc_arg'),
        R(r'TIOCM_(CD|RI)', ALIAS, 'tiocm', None),
        R(r'TIOCM_\w+|TIOCPKT_\w+|TIOCSER_TEMT', SAME, 'tioc_arg'),
        R(r'TIOCINQ', ALIAS, 'ioctl', 'FIONREAD'),
        R('|'.join(sorted(IOCTL_ARGS)), IOCTL, 'ioctl'),
    ], 'termios and terminal ioctls'),
    Family('ioc', ['asm/ioctl.h'], ['asm/ioctl.h', 'asm-generic/ioctl.h'], [
        R(r'_IOC_(NRBITS|TYPEBITS|SIZEBITS|DIRBITS|NRMASK|TYPEMASK|SIZEMASK|DIRMASK|NRSHIFT|TYPESHIFT|'
          r'SIZESHIFT|DIRSHIFT|NONE|WRITE|READ)', SCALAR, 'ioc'),
        R(r'IOC_IN|IOC_OUT|IOC_INOUT|IOCSIZE_MASK|IOCSIZE_SHIFT', SCALAR, 'ioc'),
    ], '_IOC request encoding'),
    Family('errno', ['asm/errno.h'], ['asm/errno.h', 'asm-generic/errno.h', 'asm-generic/errno-base.h'], [
        R(r'EWOULDBLOCK', ALIAS, 'errno', 'EAGAIN'),
        R(r'E[A-Z0-9]+', SCALAR, 'errno'),
    ], 'errno'),
    Family('socket', ['asm/socket.h', 'asm/sockios.h'],
           ['asm/socket.h', 'asm-generic/socket.h', 'asm/sockios.h', 'asm-generic/sockios.h'], [
        R(r'SO_\w+|SCM_\w+|SOL_SOCKET', SCALAR, 'so'),
        R(r'FIOSETOWN|SIOCSPGRP|FIOGETOWN|SIOCGPGRP|SIOCATMARK|SIOCGSTAMP_OLD|SIOCGSTAMPNS_OLD|SIOCGSTAMP|SIOCGSTAMPNS', SCALAR, 'sockios'),
    ], 'socket options and socket ioctls'),
    Family('auxv', ['linux/auxvec.h'],
           ['asm/auxvec.h', 'linux/auxvec.h'], [
        R(r'AT_\w+', SCALAR, 'auxv'),
        R(r'AT_VECTOR_SIZE_ARCH', INFO, 'auxv'),
    ], 'auxiliary vector'),
    Family('hwcap', ['asm/hwcap.h'], ['asm/hwcap.h'], [
        R(r'HWCAP\d?_\w+', INFO, 'hwcap'),
    ], 'arm64 hwcaps', roles=('guest',)),
    Family('limits', ['linux/types.h', 'asm/resource.h', 'asm/poll.h'],
           ['asm/resource.h', 'asm-generic/resource.h', 'asm/poll.h', 'asm-generic/poll.h'], [
        R(r'RLIMIT_\w+|RLIM_NLIMITS|RLIM_INFINITY', SAME, 'rlimit'),
        R(r'POLL\w+', SAME, 'poll'),
    ], 'rlimits and poll'),
]

# Structs that must have identical layout on both architectures, because the
# handlers pass pointers to them straight through. (name, type, tag, includes,
# sizeof-only)
IDENTICAL_STRUCTS = [
    ('statx', 'struct statx', 'statx', ['linux/stat.h'], False),
    ('flock', 'struct flock', 'flock', ['asm/fcntl.h'], False),
    ('f_owner_ex', 'struct f_owner_ex', 'f_owner_ex', ['asm/fcntl.h'], False),
    ('open_how', 'struct open_how', 'open_how', ['linux/openat2.h'], False),
    ('winsize', 'struct winsize', 'winsize', ['asm/termios.h'], False),
    ('sigaction', 'struct sigaction', 'sigaction', ['asm/signal.h'], False),
    ('stack_t', 'stack_t', 'sigaltstack', ['asm/signal.h'], False),
    ('sigset_t', 'sigset_t', None, ['asm/signal.h'], True),
    ('siginfo_t', 'siginfo_t', None, ['asm/siginfo.h'], True),
    ('rlimit', 'struct rlimit', 'rlimit', ['linux/resource.h'], False),
    ('rusage', 'struct rusage', 'rusage', ['linux/resource.h'], False),
    ('sysinfo', 'struct sysinfo', 'sysinfo', ['linux/sysinfo.h'], False),
    ('epoll_event', 'struct epoll_event', 'epoll_event', ['linux/eventpoll.h'], False),
    ('pollfd', 'struct pollfd', 'pollfd', ['asm/poll.h'], False),
    ('new_utsname', 'struct new_utsname', 'new_utsname', ['linux/utsname.h'], False),
    ('kernel_timespec', 'struct __kernel_timespec', '__kernel_timespec', ['linux/time_types.h'], False),
    ('kernel_old_timeval', 'struct __kernel_old_timeval', '__kernel_old_timeval', ['linux/time_types.h'], False),
    ('clone_args', 'struct clone_args', 'clone_args', ['linux/sched.h'], False),
]

# Structs whose layout differs and is converted field by field.
CONVERTED_STRUCTS = [
    ('Stat', 'struct stat', 'stat', ['asm/stat.h']),
    ('Termios', 'struct termios', 'termios', ['asm/termbits.h']),
    ('Termio', 'struct termio', 'termio', ['asm/termios.h']),
]
GUEST_ONLY_STRUCTS = [
    ('Termios2', 'struct termios2', 'termios2', ['asm/termbits.h']),
]

# ---------------------------------------------------------------------------
# Collection
# ---------------------------------------------------------------------------

DEFINE_RX = re.compile(r'^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(\()?', re.M)


class Const:
    def __init__(self, name, family):
        self.name = name
        self.family = family
        self.rule = None
        self.values = {}       # role -> int
        self.nonint = {}       # role -> diagnostic
        self.defined = set()   # roles where it is an object-like macro after preprocessing
        self.empty = set()     # roles where it is defined with no value (a presence marker)

    @property
    def kind(self):
        return self.rule.kind

    def v(self, role):
        return self.values.get(role)


def to_signed(v):
    return v - (1 << 64) if v >= (1 << 63) else v


def collect(envs, families):
    consts = {}
    for fam in families:
        names = set()
        scanned = {}
        for role, env in envs.items():
            if role not in fam.roles:
                continue
            for inc in fam.scan:
                path = env.resolve(inc)
                if path in scanned:
                    continue
                text = open(path).read()
                found = [m.group(1) for m in DEFINE_RX.finditer(text) if not m.group(2)]
                scanned[path] = found
                names.update(found)
        exprs = {}
        for role, env in envs.items():
            if role not in fam.roles:
                continue
            mac = env.macros(fam.includes)
            live = sorted(n for n in names if n in mac and not mac[n][0] and mac[n][1].strip())
            vals, failed = env.eval_exprs(fam.includes, live)
            for n in names:
                c = consts.get((fam.name, n))
                if c is None:
                    c = consts[(fam.name, n)] = Const(n, fam)
                if n in mac and not mac[n][0]:
                    c.defined.add(role)
                    if not mac[n][1].strip():
                        c.empty.add(role)
            for i, n in enumerate(live):
                c = consts[(fam.name, n)]
                if i in vals:
                    c.values[role] = vals[i]
                else:
                    c.nonint[role] = failed.get(i, 'did not compile')
        unclassified = []
        for n in sorted(names):
            c = consts[(fam.name, n)]
            matches = [r for r in fam.rules if r.rx.match(n)]
            if not matches:
                unclassified.append('%s (arm64=%s ppc64le=%s)' % (n, fmt(c.v('guest')), fmt(c.v('host'))))
                continue
            c.rule = matches[0]
        if unclassified:
            fail('family %s: no classification rule for:\n  %s' % (fam.name, '\n  '.join(unclassified)))
    return consts


def fmt(v):
    if v is None:
        return '-'
    s = to_signed(v)
    if -(1 << 20) < s < 0:
        return str(s)
    return hex(v)


def popcount(v):
    return bin(v).count('1')


def validate(consts):
    byfam = {}
    for (fam, n), c in consts.items():
        byfam.setdefault(fam, {})[n] = c
    for fam, cs in byfam.items():
        for n, c in sorted(cs.items()):
            k = c.kind
            if k == IGNORE:
                continue
            if not c.defined:
                # Declared only inside a branch neither architecture takes.
                if k not in (INFO, NONINT):
                    c.rule = Rule(re.escape(n), INFO, c.rule.group, c.rule.extra)
                continue
            if c.empty:
                if k == SAME and c.empty == c.defined == {'guest', 'host'}:
                    continue
                if k in (INFO, IGNORE):
                    continue
                fail('%s: %s is an empty marker macro on %s but is classified %s' % (fam, n, sorted(c.empty), k))
            if k == NONINT:
                for role in c.defined:
                    if role in c.values:
                        fail('%s: %s is classified nonint but evaluates to %s on %s' % (fam, n, fmt(c.values[role]), role))
                continue
            for role in c.defined:
                if role in c.nonint and k != INFO and not (k == IOCTL and IOCTL_ARGS.get(n) == 'hostonly'):
                    fail('%s: %s does not evaluate on %s: %s' % (fam, n, ROLES[role]['label'], c.nonint[role]))
            if k == BIT:
                for role, v in c.values.items():
                    if popcount(v) != 1:
                        fail('%s: %s is classified as a flag bit but is %s on %s' % (fam, n, fmt(v), role))
            elif k == ZERO:
                for role, v in c.values.items():
                    if v != 0:
                        fail('%s: %s is classified as a zero flag but is %s on %s' % (fam, n, fmt(v), role))
            elif k == FIELD:
                for role, v in c.values.items():
                    if v == 0:
                        fail('%s: field mask %s is 0 on %s' % (fam, n, role))
            elif k == FIELDVAL:
                mask = cs.get(c.rule.extra)
                if mask is None:
                    fail('%s: %s names missing field %s' % (fam, n, c.rule.extra))
                for role, v in c.values.items():
                    mv = mask.values.get(role)
                    if mv is None or (v & ~mv):
                        fail('%s: %s=%s is outside field %s=%s on %s' % (fam, n, fmt(v), c.rule.extra, fmt(mv), role))
            elif k == ALIAS:
                target = c.rule.extra
                if target is None:
                    continue
                t = cs.get(target)
                if t is None:
                    fail('%s: alias %s targets unknown %s' % (fam, n, target))
                for role, v in c.values.items():
                    if t.values.get(role) != v:
                        fail('%s: alias %s=%s differs from %s=%s on %s' % (fam, n, fmt(v), target, fmt(t.values.get(role)), role))
            elif k == SAME:
                if set(c.values) != {'guest', 'host'}:
                    fail('%s: %s must exist on both architectures (the handlers pass it through), arm64=%s ppc64le=%s' %
                         (fam, n, fmt(c.v('guest')), fmt(c.v('host'))))
                if c.v('guest') != c.v('host'):
                    fail('%s: %s must be identical (the handlers pass it through) but arm64=%s ppc64le=%s' %
                         (fam, n, fmt(c.v('guest')), fmt(c.v('host'))))
            elif k == COMPOSITE:
                for role, v in c.values.items():
                    bits = 0
                    for o in cs.values():
                        if o.rule and o.rule.kind == BIT and o.rule.group == c.rule.group and role in o.values:
                            bits |= o.values[role]
                    if v & ~bits:
                        fail('%s: composite %s=%s has bits outside its group on %s' % (fam, n, fmt(v), role))
            elif k == IOCTL:
                if c.rule.group == 'ioctl' and n in IOCTL_ARGS and IOCTL_ARGS[n] == 'hostonly' and 'guest' in c.values:
                    fail('%s: %s is marked powerpc-only but the arm64 headers define it' % (fam, n))
    # Flag groups: no two bits may share a value, and bits may not overlap a field.
    groups = {}
    for (fam, n), c in consts.items():
        if c.rule.kind in (BIT, FIELD) and c.defined:
            groups.setdefault((fam, c.rule.group), []).append(c)
    for (fam, g), cs in groups.items():
        for role in ('guest', 'host'):
            seen = {}
            fields = [c for c in cs if c.kind == FIELD and role in c.values]
            for c in cs:
                if role not in c.values:
                    continue
                v = c.values[role]
                if c.kind == BIT:
                    if v in seen:
                        fail('%s/%s: %s and %s share bit %s on %s' % (fam, g, seen[v], c.name, fmt(v), role))
                    seen[v] = c.name
                    for f in fields:
                        if f.name == 'CIBAUD':
                            continue
                        if v & f.values[role]:
                            fail('%s/%s: bit %s=%s overlaps field %s on %s' % (fam, g, c.name, fmt(v), f.name, role))


# ---------------------------------------------------------------------------
# Struct layouts
# ---------------------------------------------------------------------------


def split_depth0(body, sep):
    out, depth, cur = [], 0, []
    for ch in body:
        if ch in '({[':
            depth += 1
        elif ch in ')}]':
            depth -= 1
        if ch == sep and depth == 0:
            out.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append(''.join(cur))
    return out


def struct_fields(pre, tag, label):
    m = re.search(r'\bstruct\s+%s\s*\{' % re.escape(tag), pre)
    if not m:
        fail('%s: struct %s not found' % (label, tag))
    i = m.end()
    depth = 1
    j = i
    while depth:
        if pre[j] == '{':
            depth += 1
        elif pre[j] == '}':
            depth -= 1
        j += 1
    body = pre[i:j - 1]
    body = re.sub(r'__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)', ' ', body)
    fields = []
    for decl in split_depth0(body, ';'):
        decl = ' '.join(decl.split())
        if not decl:
            continue
        if '{' in decl:
            fail('%s: struct %s has a nested aggregate member (%s); add explicit support' % (label, tag, decl[:60]))
        if ':' in decl:
            fail('%s: struct %s has a bitfield (%s); add explicit support' % (label, tag, decl))
        # int a, b;  ->  first declarator carries the type
        parts = split_depth0(decl, ',')
        for k, p in enumerate(parts):
            p = p.strip()
            fm = re.search(r'\(\s*\*\s*(\w+)\s*\)', p)
            if fm:
                fields.append((fm.group(1), False))
                continue
            am = re.search(r'(\w+)\s*((?:\[[^\]]*\]\s*)*)$', p)
            if not am:
                fail('%s: cannot parse member "%s" of struct %s' % (label, p, tag))
            fields.append((am.group(1), bool(am.group(2))))
    return fields


class Layout:
    def __init__(self, ctype, size, align, fields):
        self.ctype = ctype
        self.size = size
        self.align = align
        self.fields = fields   # list of dict(name, off, size, cls, signed, count, elem)

    def key(self):
        return (self.size, self.align, tuple((f['name'], f['off'], f['size'], f.get('count')) for f in self.fields))


def probe_layout(env, ctype, tag, includes, sizeonly):
    if sizeonly or tag is None:
        vals, failed = env.eval_exprs(includes, ['sizeof(%s)' % ctype, '_Alignof(%s)' % ctype])
        if failed:
            fail('%s: cannot size %s: %s' % (env.label, ctype, failed))
        return Layout(ctype, vals[0], vals[1], [])
    pre = env.preprocess(includes)
    fields = struct_fields(pre, tag, env.label)
    T = '%s' % ctype
    exprs = ['sizeof(%s)' % T, '_Alignof(%s)' % T]
    for name, is_array in fields:
        acc = '((%s*)0)->%s' % (T, name)
        exprs += ['powerarm_offsetof(%s, %s)' % (T, name), 'sizeof(%s)' % acc, '__builtin_classify_type(%s)' % acc]
        exprs.append('sizeof(%s[0])' % acc if is_array else '0')
    vals, failed = env.eval_exprs(includes, exprs)
    if failed:
        fail('%s: layout probe of %s failed: %s' % (env.label, ctype, sorted(failed.values())[:3]))
    out = []
    for k, (name, is_array) in enumerate(fields):
        b = 2 + 4 * k
        f = dict(name=name, off=vals[b], size=vals[b + 1], cls=vals[b + 2], array=is_array)
        if is_array:
            f['elem'] = vals[b + 3]
            f['count'] = f['size'] // f['elem']
        out.append(f)
    # Signedness of integer scalars and array elements.
    sexprs, sidx = [], []
    for k, f in enumerate(out):
        acc = '((%s*)0)->%s' % (T, f['name'])
        if f['array']:
            acc += '[0]'
            sexprs.append('__builtin_classify_type(%s)' % acc)
            sidx.append((k, 'ecls'))
        elif f['cls'] in (1, 2, 3, 4):
            sexprs.append('((__typeof__(%s))-1 < (__typeof__(%s))0)' % (acc, acc))
            sidx.append((k, 'signed'))
    if sexprs:
        svals, failed = env.eval_exprs(includes, sexprs)
        if failed:
            fail('%s: signedness probe of %s failed' % (env.label, ctype))
        for n, (k, what) in enumerate(sidx):
            out[k][what] = svals[n]
        # Array element signedness (integer elements only).
        aexprs, aidx = [], []
        for k, f in enumerate(out):
            if f['array'] and f.get('ecls') in (1, 2, 3, 4):
                acc = '((%s*)0)->%s[0]' % (T, f['name'])
                aexprs.append('((__typeof__(%s))-1 < (__typeof__(%s))0)' % (acc, acc))
                aidx.append(k)
        if aexprs:
            avals, failed = env.eval_exprs(includes, aexprs)
            if failed:
                fail('%s: element signedness probe of %s failed' % (env.label, ctype))
            for n, k in enumerate(aidx):
                out[k]['signed'] = avals[n]
    return Layout(ctype, vals[0], vals[1], out)


def is_padding(name):
    return bool(re.match(r'__?(pad|unused|glibc_reserved|reserved)\w*|__?spare\w*', name))


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------


def cname(prefix, name):
    lead = len(name) - len(name.lstrip('_'))
    if lead:
        return '%s_%s_%s' % (prefix, 'U' * lead, name.lstrip('_'))
    return '%s_%s' % (prefix, name)


def member(name):
    # glibc's <sys/stat.h> defines st_atime and friends as macros, and names
    # with leading underscores are reserved; emit neutral C++ member names.
    if re.fullmatch(r'st_[amc]time', name):
        return name + '_sec'
    if name.startswith('_'):
        return name.lstrip('_') + '_'
    return name


def cxx_int(size, signed):
    return '%sint%d_t' % ('' if signed else 'u', size * 8)


def emit_struct(out, sname, lay):
    out.append('struct %s {' % sname)
    pos = 0
    npad = 0
    for f in lay.fields:
        if f['off'] > pos:
            out.append('  uint8_t PowerarmPadding%d[%d];' % (npad, f['off'] - pos))
            npad += 1
        if f['array']:
            if f.get('ecls') in (1, 2, 3, 4):
                t = cxx_int(f['elem'], f.get('signed', 0))
            else:
                t = 'uint8_t'
                f = dict(f, count=f['size'])
            out.append('  %s %s[%d];' % (t, member(f['name']), f['count']))
        elif f['cls'] in (1, 2, 3, 4):
            out.append('  %s %s;' % (cxx_int(f['size'], f.get('signed', 0)), member(f['name'])))
        elif f['cls'] == 5:
            out.append('  uint64_t %s; // pointer' % member(f['name']))
        else:
            out.append('  uint8_t %s[%d]; // aggregate' % (member(f['name']), f['size']))
        pos = f['off'] + f['size']
    if lay.size > pos:
        out.append('  uint8_t PowerarmPadding%d[%d];' % (npad, lay.size - pos))
    out.append('};')
    out.append('static_assert(sizeof(%s) == %d);' % (sname, lay.size))
    out.append('static_assert(alignof(%s) == %d);' % (sname, lay.align))
    for f in lay.fields:
        out.append('static_assert(offsetof(%s, %s) == %d);' % (sname, member(f['name']), f['off']))
    out.append('')


def generate(kernel, cc):
    work = tempfile.mkdtemp(prefix='powerarm-abi-')
    try:
        envs = {role: Env(kernel, role, cc, work) for role in ROLES}
        consts = collect(envs, FAMILIES)
        validate(consts)
        identical = []
        for name, ctype, tag, inc, sizeonly in IDENTICAL_STRUCTS:
            g = probe_layout(envs['guest'], ctype, tag, inc, sizeonly)
            h = probe_layout(envs['host'], ctype, tag, inc, sizeonly)
            if g.key() != h.key():
                fail('%s: layouts differ between arm64 and ppc64le but the handlers pass it through untranslated:\n'
                     '  arm64:   %s\n  ppc64le: %s' % (ctype, g.key(), h.key()))
            identical.append((name, ctype, g))
        converted = []
        for name, ctype, tag, inc in CONVERTED_STRUCTS:
            g = probe_layout(envs['guest'], ctype, tag, inc, False)
            h = probe_layout(envs['host'], ctype, tag, inc, False)
            if g.key() == h.key():
                fail('%s: expected a layout difference, found none; move it to IDENTICAL_STRUCTS' % ctype)
            converted.append((name, ctype, g, h))
        guest_only = []
        for name, ctype, tag, inc in GUEST_ONLY_STRUCTS:
            guest_only.append((name, ctype, probe_layout(envs['guest'], ctype, tag, inc, False)))
        version = kernel_version(kernel)
        return consts, identical, converted, guest_only, version
    finally:
        shutil.rmtree(work, ignore_errors=True)


def kernel_version(kernel):
    v = {}
    for line in open(os.path.join(kernel, 'Makefile')):
        m = re.match(r'(VERSION|PATCHLEVEL|SUBLEVEL)\s*=\s*(\d+)', line)
        if m:
            v[m.group(1)] = m.group(2)
        if len(v) == 3:
            break
    return '%s.%s.%s' % (v['VERSION'], v['PATCHLEVEL'], v['SUBLEVEL'])


def u64(v):
    return '0x%xULL' % v


def build_flagset(consts, fam, group, shifted=None):
    cs = [c for (f, n), c in sorted(consts.items()) if f == fam and c.rule.group == group and c.defined]
    bits, guest_only, host_only = [], [], []
    fields = []
    for c in cs:
        if c.kind == BIT:
            if 'guest' in c.values and 'host' in c.values:
                bits.append(c)
            elif 'guest' in c.values:
                guest_only.append(c)
            else:
                host_only.append(c)
    for c in cs:
        if c.kind != FIELD:
            continue
        vals = []
        if shifted and c.name in shifted:
            base, shift = shifted[c.name]
            sh = consts[(fam, shift)]
            srcs = [o for o in cs if o.kind == FIELDVAL and o.rule.extra == base]
            for o in srcs:
                if 'guest' in o.values and 'host' in o.values:
                    vals.append((o.name, o.values['guest'] << sh.values['guest'], o.values['host'] << sh.values['host']))
        else:
            for o in cs:
                if o.kind == FIELDVAL and o.rule.extra == c.name and 'guest' in o.values and 'host' in o.values:
                    vals.append((o.name, o.values['guest'], o.values['host']))
        fields.append((c, vals))
    return bits, guest_only, host_only, fields


FLAGSETS = [
    # (C++ name, family, group, shifted fields)
    ('OpenFlags', 'fcntl', 'open', None),
    ('ProtFlags', 'mman', 'prot', None),
    ('MapFlags', 'mman', 'map', None),
    ('MclFlags', 'mman', 'mcl', None),
    ('TermiosIFlag', 'termios', 'c_iflag', None),
    ('TermiosOFlag', 'termios', 'c_oflag', None),
    ('TermiosCFlag', 'termios', 'c_cflag', {'CIBAUD': ('CBAUD', 'IBSHIFT')}),
    ('TermiosLFlag', 'termios', 'c_lflag', None),
]


def emit_header(consts, identical, converted, guest_only, version):
    o = []
    o.append('// SPDX-License-Identifier: MIT')
    o.append('// Generated by gen_abi_tables.py from the Linux %s uapi headers. Do not edit.' % version)
    o.append('//')
    o.append('// GUEST_* values are the arm64 (asm-generic) ABI, HOST_* the ppc64le one. A')
    o.append('// name missing from one side does not exist on that architecture.')
    o.append('#pragma once')
    o.append('')
    o.append('#include <cstddef>')
    o.append('#include <cstdint>')
    o.append('')
    o.append('namespace FEX::HLE::Arm64::ABI {')
    o.append('inline constexpr const char KernelHeadersVersion[] = "%s";' % version)
    o.append('')
    for fam in FAMILIES:
        o.append('// ---- %s ----' % fam.title)
        for (f, n), c in sorted(consts.items()):
            if f != fam.name or c.kind == IGNORE or not c.values:
                continue
            for role, prefix in (('guest', 'GUEST'), ('host', 'HOST')):
                if role in c.values:
                    o.append('inline constexpr uint64_t %s = %s; // %s' % (cname(prefix, n), u64(c.values[role]), c.kind))
        o.append('')

    o.append('// ---- Flag translation tables ----')
    o.append('struct FlagBit {')
    o.append('  uint64_t Guest;')
    o.append('  uint64_t Host;')
    o.append('};')
    o.append('struct FieldValue {')
    o.append('  uint64_t Guest;')
    o.append('  uint64_t Host;')
    o.append('};')
    o.append('struct FlagField {')
    o.append('  uint64_t GuestMask;')
    o.append('  uint64_t HostMask;')
    o.append('  const FieldValue* Values;')
    o.append('  size_t NumValues;')
    o.append('};')
    o.append('struct FlagSet {')
    o.append('  const FlagBit* Bits;')
    o.append('  size_t NumBits;')
    o.append('  const FlagField* Fields;')
    o.append('  size_t NumFields;')
    o.append('  // Bits that exist on one architecture only.')
    o.append('  uint64_t GuestOnly;')
    o.append('  uint64_t HostOnly;')
    o.append('};')
    o.append('')
    for cpp, fam, group, shifted in FLAGSETS:
        bits, gonly, honly, fields = build_flagset(consts, fam, group, shifted)
        o.append('inline constexpr FlagBit %s_Bits[] = {' % cpp)
        for c in bits:
            o.append('  {%s, %s}, // %s' % (u64(c.values['guest']), u64(c.values['host']), c.name))
        o.append('};')
        for c, vals in fields:
            o.append('inline constexpr FieldValue %s_%s_Values[] = {' % (cpp, c.name))
            for name, gv, hv in vals:
                o.append('  {%s, %s}, // %s' % (u64(gv), u64(hv), name))
            if not vals:
                o.append('  {0, 0},')
            o.append('};')
        o.append('inline constexpr FlagField %s_Fields[] = {' % cpp)
        for c, vals in fields:
            o.append('  {%s, %s, %s_%s_Values, %d}, // %s' % (u64(c.values['guest']), u64(c.values['host']), cpp, c.name,
                                                            len(vals), c.name))
        if not fields:
            o.append('  {0, 0, nullptr, 0},')
        o.append('};')
        gm = 0
        for c in gonly:
            gm |= c.values['guest']
        hm = 0
        for c in honly:
            hm |= c.values['host']
        o.append('inline constexpr FlagSet %s = {%s_Bits, %d, %s_Fields, %d, %s, %s}; // guest-only: %s; host-only: %s' %
                 (cpp, cpp, len(bits), cpp, len(fields), u64(gm), u64(hm),
                  ', '.join(c.name for c in gonly) or 'none', ', '.join(c.name for c in honly) or 'none'))
        o.append('')

    o.append('// ---- Control-character indices (c_cc) ----')
    o.append('struct CCIndex {')
    o.append('  uint8_t Guest;')
    o.append('  uint8_t Host;')
    o.append('};')
    o.append('inline constexpr CCIndex TermiosCC[] = {')
    for (f, n), c in sorted(consts.items(), key=lambda kv: (kv[1].values.get('guest', 99), kv[0])):
        if f == 'termios' and c.rule.group == 'c_cc' and c.kind == SCALAR:
            if 'guest' in c.values and 'host' in c.values:
                o.append('  {%d, %d}, // %s' % (c.values['guest'], c.values['host'], n))
            else:
                fail('termios: control character %s is missing on one architecture' % n)
    o.append('};')
    o.append('')

    o.append('// ---- Terminal ioctls ----')
    o.append('enum class IoctlArg : uint8_t {')
    o.append('  Plain,       // argument passed through (value, int*, pid_t*, struct winsize*)')
    o.append('  OpenFlags,   // argument is an O_* flag set')
    o.append('  TermiosGet,  // struct termios* out')
    o.append('  TermiosSet,  // struct termios* in')
    o.append('  Termios2Get, // struct termios2* out (no powerpc equivalent request)')
    o.append('  Termios2Set, // struct termios2* in')
    o.append('  Unsupported, // struct argument not translated yet')
    o.append('};')
    o.append('struct IoctlEntry {')
    o.append('  uint32_t Guest;')
    o.append('  uint32_t Host; // 0 when powerpc has no request of that name')
    o.append('  IoctlArg Arg;')
    o.append('  const char* Name;')
    o.append('};')
    argmap = {'plain': 'Plain', 'openflags': 'OpenFlags', 'termios_get': 'TermiosGet', 'termios_set': 'TermiosSet',
              'termios2_get': 'Termios2Get', 'termios2_set': 'Termios2Set', 'unsupported': 'Unsupported'}
    o.append('inline constexpr IoctlEntry TerminalIoctls[] = {')
    for (f, n), c in sorted(consts.items(), key=lambda kv: (kv[1].values.get('guest', 0), kv[0])):
        if f != 'termios' or c.kind != IOCTL:
            continue
        arg = IOCTL_ARGS[n]
        if arg == 'hostonly':
            continue
        if 'guest' not in c.values:
            fail('ioctl %s: no arm64 value' % n)
        host = c.values.get('host', 0)
        if arg == 'termios2_get' or arg == 'termios2_set':
            host = 0
        o.append('  {%s, %s, IoctlArg::%s, "%s"},' % (hex(c.values['guest']), hex(host), argmap[arg], n))
    o.append('};')
    o.append('')

    o.append('// ---- errno ----')
    o.append('// Host errno values whose guest value differs, as {host, guest}.')
    o.append('struct ErrnoFixup {')
    o.append('  uint16_t Host;')
    o.append('  uint16_t Guest;')
    o.append('};')
    fixes = {}
    for (f, n), c in sorted(consts.items()):
        if f == 'errno' and c.kind == SCALAR and 'guest' in c.values and 'host' in c.values:
            if c.values['guest'] != c.values['host']:
                hv, gv = c.values['host'], c.values['guest']
                if hv in fixes and fixes[hv][0] != gv:
                    fail('errno: host value %d maps to two guest values' % hv)
                # The host value must not also be a valid, different guest errno.
                for (f2, n2), c2 in consts.items():
                    if f2 == 'errno' and c2.values.get('guest') == hv and c2.values.get('host') != hv:
                        pass
                    elif f2 == 'errno' and c2.values.get('guest') == hv:
                        fail('errno: host %s=%d collides with guest %s' % (n, hv, n2))
                fixes[hv] = (gv, n)
    o.append('inline constexpr ErrnoFixup HostToGuestErrno[] = {')
    for hv, (gv, n) in sorted(fixes.items()):
        o.append('  {%d, %d}, // %s' % (hv, gv, n))
    o.append('};')
    o.append('')

    o.append('// ---- Socket option names (SOL_SOCKET) ----')
    o.append('struct ScalarPair {')
    o.append('  uint64_t Guest;')
    o.append('  uint64_t Host;')
    o.append('};')
    o.append('inline constexpr ScalarPair SocketOptions[] = {')
    for (f, n), c in sorted(consts.items(), key=lambda kv: (kv[1].values.get('guest', 0), kv[0])):
        if f == 'socket' and c.rule.group == 'so' and c.name.startswith('SO_') and 'guest' in c.values and 'host' in c.values:
            o.append('  {%d, %d}, // %s' % (c.values['guest'], c.values['host'], n))
    o.append('};')
    o.append('')

    o.append('// ---- Struct layouts ----')
    o.append('// Identical on both architectures (verified at generation time); pointers pass through.')
    for name, ctype, lay in identical:
        o.append('inline constexpr size_t LAYOUT_%s_SIZE = %d; // %s' % (name.upper(), lay.size, ctype))
    o.append('')
    for name, ctype, g, h in converted:
        emit_struct(o, 'Guest%s' % name, g)
        emit_struct(o, 'Host%s' % name, h)
    for name, ctype, g in guest_only:
        emit_struct(o, 'Guest%s' % name, g)

    # stat converter
    for name, ctype, g, h in converted:
        if name != 'Stat':
            continue
        hnames = {f['name']: f for f in h.fields}
        o.append('// Returns false when a value does not fit the guest field (the kernel answers EOVERFLOW).')
        o.append('inline bool ConvertStat(const HostStat& H, GuestStat* G) {')
        o.append('  *G = GuestStat {};')
        o.append('  bool Fits = true;')
        for f in g.fields:
            if is_padding(f['name']):
                continue
            if f['name'] not in hnames:
                fail('struct stat: guest field %s has no powerpc counterpart' % f['name'])
            m = member(f['name'])
            o.append('  G->%s = H.%s;' % (m, m))
            o.append('  Fits &= static_cast<decltype(H.%s)>(G->%s) == H.%s;' % (m, m, m))
        for f in h.fields:
            if not is_padding(f['name']) and f['name'] not in {x['name'] for x in g.fields}:
                fail('struct stat: powerpc field %s has no arm64 counterpart' % f['name'])
        o.append('  return Fits;')
        o.append('}')
        o.append('')
    o.append('} // namespace FEX::HLE::Arm64::ABI')
    o.append('')
    return '\n'.join(o)


def emit_doc(consts, identical, converted, guest_only, version):
    o = []
    o.append('# arm64 vs ppc64le Linux user ABI: generated differences')
    o.append('')
    o.append('Generated by `Source/Tools/LinuxEmulation/LinuxSyscalls/Arm64/gen_abi_tables.py` from the Linux %s '
             'uapi headers. Do not edit; rerun the script. The C++ tables are in `Arm64/GeneratedABI.h`.' % version)
    o.append('')
    o.append('Every object-like `#define` in the scanned headers is classified by a rule in the script. An '
             'unclassified name, or a value that contradicts its rule, stops generation. Constants marked '
             '*must be identical* are passed through by the syscall handlers; the script fails if they ever differ.')
    o.append('')
    o.append('## Summary')
    o.append('')
    o.append('| Family | Names | Identical | Differ | arm64 only | ppc64le only |')
    o.append('|---|---:|---:|---:|---:|---:|')
    tot = [0, 0, 0, 0, 0]
    for fam in FAMILIES:
        cs = [c for (f, n), c in consts.items() if f == fam.name and c.kind != IGNORE and c.values]
        same = sum(1 for c in cs if set(c.values) == {'guest', 'host'} and c.v('guest') == c.v('host'))
        diff = sum(1 for c in cs if set(c.values) == {'guest', 'host'} and c.v('guest') != c.v('host'))
        go = sum(1 for c in cs if set(c.values) == {'guest'})
        ho = sum(1 for c in cs if set(c.values) == {'host'})
        row = [len(cs), same, diff, go, ho]
        tot = [a + b for a, b in zip(tot, row)]
        o.append('| %s | %s |' % (fam.title, ' | '.join(str(x) for x in row)))
    o.append('| **total** | %s |' % ' | '.join('**%d**' % x for x in tot))
    o.append('')
    for fam in FAMILIES:
        cs = [c for (f, n), c in sorted(consts.items()) if f == fam.name and c.kind != IGNORE and c.values]
        rows = [c for c in cs if c.v('guest') != c.v('host')]
        o.append('## %s' % fam.title)
        o.append('')
        if fam.roles == ('guest',):
            o.append('arm64-only family: %d names, listed as `GUEST_*` constants in `GeneratedABI.h`.' % len(cs))
            o.append('')
            continue
        if not rows:
            o.append('No differences (%d names compared).' % len(cs))
            o.append('')
            continue
        o.append('| Name | arm64 | ppc64le | Kind | Group |')
        o.append('|---|---|---|---|---|')
        for c in sorted(rows, key=lambda c: (c.rule.group or '', c.name)):
            o.append('| `%s` | %s | %s | %s | %s |' % (c.name, fmt(c.v('guest')), fmt(c.v('host')), c.kind, c.rule.group or ''))
        o.append('')
    o.append('## Struct layouts')
    o.append('')
    o.append('Identical on both (passed through): ' + ', '.join('`%s` (%d bytes)' % (ct, l.size) for n, ct, l in identical) + '.')
    o.append('')
    for name, ctype, g, h in converted:
        o.append('### `%s` (arm64 %d bytes, ppc64le %d bytes)' % (ctype, g.size, h.size))
        o.append('')
        o.append('| Field | arm64 offset/size | ppc64le offset/size |')
        o.append('|---|---|---|')
        hf = {f['name']: f for f in h.fields}
        names = [f['name'] for f in g.fields] + [f['name'] for f in h.fields if f['name'] not in {x['name'] for x in g.fields}]
        gf = {f['name']: f for f in g.fields}
        for n in names:
            a = gf.get(n)
            b = hf.get(n)
            o.append('| `%s` | %s | %s |' % (n, '%d/%d' % (a['off'], a['size']) if a else '-',
                                             '%d/%d' % (b['off'], b['size']) if b else '-'))
        o.append('')
    for name, ctype, g in guest_only:
        o.append('### `%s`: arm64 only (%d bytes); powerpc has no request that takes it' % (ctype, g.size))
        o.append('')
    return '\n'.join(o)


def emit_names(consts):
    out = []
    for (f, n), c in sorted(consts.items()):
        for role, prefix in (('guest', 'GUEST'), ('host', 'HOST')):
            if c.kind != IGNORE and role in c.values:
                out.append('%s %s %s' % (cname(prefix, n), n, f))
    return '\n'.join(out) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--cc', default='clang')
    ap.add_argument('--header', required=True)
    ap.add_argument('--doc', required=True)
    ap.add_argument('--names')
    ap.add_argument('--check', action='store_true', help='compare with the existing outputs instead of writing')
    args = ap.parse_args()
    try:
        consts, identical, converted, guest_only, version = generate(os.path.expanduser(args.kernel), args.cc)
        outputs = [(args.header, emit_header(consts, identical, converted, guest_only, version)),
                   (args.doc, emit_doc(consts, identical, converted, guest_only, version))]
        if args.names:
            outputs.append((args.names, emit_names(consts)))
    except GenError as e:
        print('gen_abi_tables.py: error: %s' % e, file=sys.stderr)
        return 1
    status = 0
    for path, text in outputs:
        if args.check:
            old = open(path).read() if os.path.exists(path) else ''
            if old != text:
                sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), text.splitlines(True), path, 'generated'))
                status = 1
        else:
            with open(path, 'w') as f:
                f.write(text)
    return status


if __name__ == '__main__':
    sys.exit(main())
