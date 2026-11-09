"""Search a Flycast RAM dump (16 MB at 0x0C000000) for camera/screen float records:
centre (320, 240) or size (640, 480) as consecutive floats, and print the floats around them."""
import struct, sys
ram = open(sys.argv[1], 'rb').read()
def f(o): return struct.unpack_from('<f', ram, o)[0]
pairs = {(320.0, 240.0): 'centre', (640.0, 480.0): 'size', (320.0, -240.0): 'viewport', (0.5, 0.375): 'tanhalf'}
for o in range(0, len(ram) - 8, 4):
    a, b = f(o), f(o + 4)
    for (x, y), name in pairs.items():
        if a == x and b == y:
            ctx = ' '.join(f'{f(o + k):.4g}' for k in range(-24, 28, 4) if 0 <= o + k < len(ram) - 4)
            print(f'{0x0C000000 + o:08X} {name:8s} | {ctx}')
