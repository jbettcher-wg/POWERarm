#!/usr/bin/env python3
"""Derives the PPC64VSXView re-export list.

Run from the repo root; writes to stdout. The committed output is
FEXCore/Source/Interface/Core/JIT/PPC64LE/PPC64VSXView.inc, whose header
comment says what is deliberately absent from the list and why.

The rule, stated once and mechanically applied:
  re-export a member NAME of the three backend classes iff no overload of that
  name takes OR RETURNS a `VR` or an `FPR`, and the name is not one of the raw
  Emit* encoders (which can splice any primary opcode, including VMX).

The return type matters as much as the arguments, and leaving it out was a real
hole the first time: GetVReg takes a PhysicalRegister and HANDS BACK a VR, so a
parameter-only rule re-exported the one name the whole view exists to remove.
The four typed VSX encoders EmitXX2VSX / EmitXX2VSXWithRA / EmitXX3VSX /
EmitXX3BF are kept: they hardcode primary opcode 60, so they cannot reach a
VMX form however they are called.
"""
import re, collections, sys

# The raw form encoders, named rather than matched on an `Emit` prefix: each
# one takes a primary opcode or raw field values, so it can splice a VMX
# instruction however it is called. The four typed VSX encoders
# (EmitXX2VSX, EmitXX2VSXWithRA, EmitXX3VSX, EmitXX3BF) are NOT here: they
# hardcode primary opcode 60. Everything else called Emit* -- the lowering
# helpers -- is judged by its signature like any other member.
RAW_ENCODERS = {
    'Emit32', 'EmitXO', 'EmitX', 'EmitD', 'EmitDQ', 'EmitM', 'EmitMD',
    'EmitA63', 'EmitA59', 'EmitFX63', 'EmitFX59', 'EmitVX', 'EmitVA',
    'EmitAtom', 'EmitSPR',
}

# Names the signature regex picks up that are not public members of the three
# classes, so a using-declaration for them does not compile. They need no
# re-export: the free functions are already at namespace scope and visible, and
# the private ones are the emitter's own branch bookkeeping, which no lowering
# calls. Keep this list rather than teaching the regex about access specifiers
# and namespace scope -- the compiler is the check either way.
NOT_A_PUBLIC_MEMBER = {
    'AddPendingBranch', 'PatchPending',             # private to PPC64Emitter::Emitter
    'InvertCond', 'AVXHighBankReg', 'AllGPRsNonVolatile',
    'PPC64BranchDisplacementInRange', 'PPC64EncodeBranch',
    'GetPPC64HelperTable',                          # free functions in FEXCore::CPU
    'alignas',                                      # a declaration specifier, not a call
}

# Members the signature regex cannot see, or that the rule above excludes too
# strictly, and that a VSX-clean lowering genuinely needs. Each one is here for
# a stated reason, and none of them can take an operand:
#
#  * Op_Unhandled is declared through the DEF_OP macro.
#  * The FPSCR accessors take an FPR, but always the backend's fixed f0
#    scratch. An operand cannot become one: operands arrive as VSXR and VSXR
#    has no conversion to FPR, so the only way to name an FPR inside a view is
#    to write a literal register number.
#  * The data members are the A64 FP cold-block bookkeeping and the two
#    context pointers (for HostFeatures.SupportsISA30). Data, not registers.
EXTRA = [
    'Op_Unhandled',
    'mffs', 'mffsl', 'mtfsf', 'mffscrn', 'mffscrni', 'mffprd', 'mtfprd',
    'CTX', 'EmitterCTX',
    'FPColdEnabled', 'FPColdStubs', 'FPNaNFixBody', 'FPNaNFixBodyUsed',
    'FPNMPrepBody', 'FPNMPrepBodyUsed', 'FPFMAFixBody', 'FPFMAFixBodyUsed',
    'FPColdStub',
    'FlagsFromFCmp',
]
FILES = [
  ('PPC64Emitter::Emitter', 'CodeEmitter/PPC64LE/Emitter.h'),
  ('PPC64EmitterBase', 'FEXCore/Source/Interface/Core/ArchHelpers/PPC64Emitter.h'),
  ('PPC64JITCore', 'FEXCore/Source/Interface/Core/JIT/PPC64LE/JITClass.h'),
]

def scan(path):
    meth = collections.defaultdict(list)
    for l in open(path):
        m = re.match(r'\s*(?:\[\[nodiscard\]\]\s*)?(?:static\s+|constexpr\s+|inline\s+|virtual\s+)*'
                     r'([A-Za-z_][\w:<>,\s\*&]*?)\s+(\w+)\s*\(([^;{]*)\)', l)
        if not m:
            continue
        ret, name, params = m.group(1), m.group(2), m.group(3)
        if name in ('if','for','while','switch','return','sizeof','assert','static_assert'):
            continue
        # The return type is checked alongside the arguments, so a getter that
        # HANDS BACK a VR is excluded even though it takes none.
        meth[name].append(ret + ' | ' + params)
    return meth

seen = set()
out = []
for cls, path in FILES:
    names = []
    for name, plist in sorted(scan(path).items()):
        if name in seen:
            continue
        if name in RAW_ENCODERS:
            continue
        if any(re.search(r'\b(VR|FPR)\b', p) for p in plist):
            continue
        if name in NOT_A_PUBLIC_MEMBER:
            continue
        if name.startswith('Op_') and name != 'Op_Unhandled':
            continue
        if name.startswith('~') or name == cls.split('::')[-1]:
            continue
        names.append(name)
        seen.add(name)
    out.append((cls, names))

out.append(('declared through DEF_OP', [n for n in EXTRA if n not in seen]))

for cls, names in out:
    print('  // --- %s' % cls)
    for n in names:
        # Named through PPC64JITCore because that is the direct base; the member
        # may be inherited into it from either emitter class.
        print('  using %s::%s;' % ('PPC64JITCore', n))
