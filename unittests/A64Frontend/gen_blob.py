import struct, sys
size = int(sys.argv[2]) if len(sys.argv) > 2 else 144 * 1024 * 1024
unit = bytes(((i % 251) + 1) for i in range(65536))
n = (size + 65535) // 65536
blob = bytearray(unit * n)
for b in range(n):
    struct.pack_into("<Q", blob, b * 65536, b * 65536)
open(sys.argv[1], "wb").write(bytes(blob[:size]))
