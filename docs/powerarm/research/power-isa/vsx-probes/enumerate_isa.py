import struct, sys
words = []
for lo in range(2048):
    words.append((4 << 26) | (1 << 21) | (2 << 16) | (3 << 11) | lo)
for ra in range(32):
    for lo in range(2048):
        words.append((60 << 26) | (1 << 21) | (ra << 16) | (3 << 11) | lo)
for lo in range(2048):
    words.append((31 << 26) | (1 << 21) | (2 << 16) | (3 << 11) | lo)
for op in (57, 61):
    for lo in range(16):
        words.append((op << 26) | (1 << 21) | (2 << 16) | lo)
sys.stdout.buffer.write(b"".join(struct.pack("<I", w) for w in words))
