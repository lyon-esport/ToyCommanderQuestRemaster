"""Scan a Flycast RAM dump for 4x4 float matrices shaped like a perspective projection:
row-major or column-major, m00 and m11 non-zero, the other entries of the first two rows zero
except a possible centre offset, m23 or m32 = +-1."""
import struct, sys, math
ram = open(sys.argv[1], 'rb').read()
N = len(ram) // 4
F = struct.unpack(f'<{N}f', ram[:N * 4])
def ok(v): return math.isfinite(v)
for i in range(0, N - 16):
    m = F[i:i + 16]
    if not all(ok(v) for v in m):
        continue
    a, b = m[0], m[5]
    if not (0.2 < abs(a) < 2000 and 0.2 < abs(b) < 2000):
        continue
    if m[1] != 0 or m[3] != 0 or m[4] != 0 or m[7] != 0:
        continue
    persp = abs(abs(m[11]) - 1) < 1e-6 or abs(abs(m[14]) - 1) < 1e-6
    if not persp or m[15] != 0:
        continue
    print(f'{0x8C000000 + i*4:08X}  ' + ' '.join(f'{v:.5g}' for v in m) + f'   a/b={a/b:.4f}')
