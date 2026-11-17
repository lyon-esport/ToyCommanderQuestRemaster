"""Find the game camera's rotation in a series of Flycast RAM dumps (Toy Commander VR, P0-8).

Take the dumps while the view turns (tcvr/tools/ramdump-series.sh), then:
    python tcvr/re/find_camera.py dumps/turn/ram_00.bin dumps/turn/ram_01.bin ...

It looks for 3x3 rotation matrices (orthonormal rows, determinant +1) stored as float rows
with a stride of 12 or 16 bytes (3x3, 3x4 or 4x4 layouts), present at the same address in
every dump, whose yaw changes between dumps. For each it prints the yaw per dump (degrees,
about the matrix's Y axis, read as rows and as columns). The camera (or its inverse, the view
matrix) shows a steady yaw change when the vehicle turns, and many objects' matrices do too:
the camera is usually among the few that change in every step and stay level (pitch small).

It also lists 16-bit angle words (65536 = 360 degrees) that follow the best matrix's yaw.

    --self-test   checks the search on made-up data
    --top N       how many matrices to print (default 40)
    --start/--end RAM offsets to scan (default the whole 16 MB)
"""
import argparse
import math
import struct
import sys

BASE = 0x8C000000


def floats(ram, off, n):
    return struct.unpack_from('<%df' % n, ram, off)


def rotation_at(ram, off, stride):
    """The 3x3 matrix at off (rows stride bytes apart), or None when it isn't a rotation."""
    rows = []
    for r in range(3):
        row = floats(ram, off + r * stride, 3)
        if not all(math.isfinite(v) and abs(v) <= 1.0001 for v in row):
            return None
        rows.append(row)
    for r in range(3):
        if abs(sum(v * v for v in rows[r]) - 1.0) > 2e-3:
            return None
    for a, b in ((0, 1), (0, 2), (1, 2)):
        if abs(sum(x * y for x, y in zip(rows[a], rows[b]))) > 2e-3:
            return None
    (a, b, c), (d, e, f), (g, h, i) = rows
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det - 1.0) > 5e-3:
        return None
    return rows


def yaw_pitch(rows, columns=False):
    """Yaw (about Y) and pitch of the forward axis (Z), degrees."""
    if columns:
        z = (rows[0][2], rows[1][2], rows[2][2])
    else:
        z = rows[2]
    yaw = math.degrees(math.atan2(z[0], z[2]))
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, z[1]))))
    return yaw, pitch


def wrap(d):
    return (d + 180.0) % 360.0 - 180.0


def scan(dumps, start, end):
    first = dumps[0]
    found = []
    for off in range(start, min(end, len(first) - 48), 4):
        for stride in (12, 16):
            m0 = rotation_at(first, off, stride)
            if m0 is None:
                continue
            mats = [m0]
            for ram in dumps[1:]:
                m = rotation_at(ram, off, stride)
                if m is None:
                    break
                mats.append(m)
            if len(mats) != len(dumps):
                continue
            for columns in (False, True):
                yp = [yaw_pitch(m, columns) for m in mats]
                steps = [wrap(yp[k + 1][0] - yp[k][0]) for k in range(len(yp) - 1)]
                moving = sum(1 for s in steps if abs(s) > 1.0)
                if moving == 0:
                    continue
                level = max(abs(p) for _, p in yp)
                found.append({'off': off, 'stride': stride, 'columns': columns, 'yp': yp, 'steps': steps,
                              'moving': moving, 'level': level})
    # every step moving first, then the most level, then the largest total turn
    found.sort(key=lambda f: (-f['moving'], f['level'] > 30, -sum(abs(s) for s in f['steps'])))
    return found


def angle_words(dumps, yaws, start, end, limit=20):
    """16-bit words whose angle follows the given yaws (either way, any offset)."""
    out = []
    first = dumps[0]
    n = len(dumps)
    for off in range(start, min(end, len(first) - 2), 2):
        vals = [struct.unpack_from('<H', d, off)[0] for d in dumps]
        if len(set(vals)) < n:
            continue
        angs = [v * 360.0 / 65536.0 for v in vals]
        for sign in (1, -1):
            diffs = [wrap(sign * angs[k] - yaws[k]) for k in range(n)]
            if max(abs(wrap(x - diffs[0])) for x in diffs) < 2.0:
                out.append((off, sign, angs))
                break
        if len(out) >= limit:
            break
    return out


def self_test():
    ram_size = 0x10000
    dumps = []
    yaws = [0, 30, 60, 95, 130]
    for k, yaw in enumerate(yaws):
        ram = bytearray(ram_size)
        r = math.radians(yaw)
        # the camera: a yaw rotation, rows, 3x4 layout at 0x1230
        rows = [(math.cos(r), 0.0, -math.sin(r)), (0.0, 1.0, 0.0), (math.sin(r), 0.0, math.cos(r))]
        for i, row in enumerate(rows):
            struct.pack_into('<4f', ram, 0x1230 + i * 16, *row, 5.0)
        # a static object (never moves): must not be listed
        r2 = math.radians(40)
        rows2 = [(math.cos(r2), 0.0, -math.sin(r2)), (0.0, 1.0, 0.0), (math.sin(r2), 0.0, math.cos(r2))]
        for i, row in enumerate(rows2):
            struct.pack_into('<3f', ram, 0x2000 + i * 12, *row)
        # its yaw as an angle word
        struct.pack_into('<H', ram, 0x3002, int(round(yaw * 65536 / 360)) & 0xFFFF)
        dumps.append(bytes(ram))
    found = scan(dumps, 0, ram_size)
    assert found and found[0]['off'] == 0x1230 and found[0]['stride'] == 16, found[:1]
    assert all(f['off'] != 0x2000 for f in found)
    got = [round(y) for y, _ in found[0]['yp']]
    assert got == yaws, got
    words = angle_words(dumps, [y for y, _ in found[0]['yp']], 0, ram_size)
    assert any(o == 0x3002 for o, _, _ in words), words
    print('self-test passed')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dumps', nargs='*')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--start', type=lambda s: int(s, 0), default=0x10000)
    ap.add_argument('--end', type=lambda s: int(s, 0), default=0x1000000)
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return
    if len(args.dumps) < 2:
        ap.error('give at least two dumps, taken while the view turned')
    dumps = [open(p, 'rb').read() for p in args.dumps]
    found = scan(dumps, args.start, args.end)
    print('%d rotation matrices change yaw across the %d dumps' % (len(found), len(dumps)))
    for f in found[:args.top]:
        yaws = ' '.join('%7.1f' % y for y, _ in f['yp'])
        print('%08X stride %2d %s moving %d/%d max pitch %5.1f  yaw: %s' % (
            BASE + f['off'], f['stride'], 'cols' if f['columns'] else 'rows', f['moving'], len(f['steps']),
            f['level'], yaws))
    if found:
        best = found[0]
        words = angle_words(dumps, [y for y, _ in best['yp']], args.start, args.end)
        print('\n16-bit angle words following %08X:' % (BASE + best['off']))
        for off, sign, angs in words:
            print('%08X %s %s' % (BASE + off, '+' if sign > 0 else '-', ' '.join('%7.1f' % a for a in angs)))


if __name__ == '__main__':
    sys.exit(main())
