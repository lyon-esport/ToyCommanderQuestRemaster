"""List code windows (in a Flycast RAM dump) where FTRV (matrix x vector) and a reciprocal
(FSRRA, or FDIV) are close together: candidates for the 3D -> screen projection loops."""
import struct, sys
ram = open(sys.argv[1], 'rb').read()
end = int(sys.argv[2], 16) - 0x8C000000 if len(sys.argv) > 2 else 0x100000
ops = {}
for o in range(0x10000, end, 2):
    w = struct.unpack_from('<H', ram, o)[0]
    if w & 0xF3FF == 0xF1FD: ops[o] = 'ftrv'
    elif w & 0xF0FF == 0xF07D: ops[o] = 'fsrra'
    elif w & 0xF00F == 0xF003: ops[o] = 'fdiv'
    elif w & 0xF0FF == 0xF06D: ops[o] = 'fsqrt'
keys = sorted(ops)
for i, o in enumerate(keys):
    if ops[o] != 'ftrv':
        continue
    near = [ops[k] for k in keys if 0 < k - o <= 0x40 and ops[k] in ('fsrra', 'fdiv')]
    if near:
        print(f'{0x8C000000 + o:08X} ftrv -> {",".join(near)}')
