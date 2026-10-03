#!/usr/bin/env python3
"""Turns a picasso .shbin (DVLB) into a C header for src/os/P3d9.c.

Usage: shbin2c.py in.shbin name > out.h

Emits the vertex shader's code and operand descriptors, its entry point, the
output map the PICA200 needs for its outputs, its constants already packed
for upload, and one NAME_UNIF_<symbol> define per float uniform giving the
register to upload it to. Only the first DVLE is used.

The DVLB layout and the output-map rules follow libctru's shbin.c
(DVLB_ParseFile, DVLE_GenerateOutmap) and shaderProgram.c (zlib licence).
"""
import struct
import sys

# DVLE output types -> (first output-map semantic, components, clock bit, mode)
OUT_TYPES = {
    0: (0x00, 4, None, 0),      # position
    1: (0x04, 4, 24, 0),        # normal quaternion
    2: (0x08, 4, 1, 0),         # colour
    3: (0x0C, 2, 8, 1),         # texcoord 0
    4: (0x10, 1, 16, 1),        # texcoord 0 w
    5: (0x0E, 2, 9, 1),         # texcoord 1
    6: (0x16, 2, 10, 1),        # texcoord 2
    8: (0x12, 3, 24, 0),        # view
}


def words(data, off, n):
    return list(struct.unpack_from('<%dI' % n, data, off))


def f24_pack(c):
    """Four float24 constants (low 24 bits of each word) in upload order."""
    b = b''.join(struct.pack('<I', v & 0xFFFFFF)[:3] for v in c)
    w = struct.unpack('<3I', b)
    return [w[2], w[1], w[0]]


def main():
    path, name = sys.argv[1], sys.argv[2]
    d = open(path, 'rb').read()
    magic, ndvle = struct.unpack_from('<II', d, 0)
    if magic != 0x424C5644 or ndvle < 1:
        sys.exit('not a DVLB')
    dvlp = 8 + 4 * ndvle
    p = words(d, dvlp, 6)
    code = words(d, dvlp + p[2], p[3])
    opdesc = [words(d, dvlp + p[4] + 8 * i, 1)[0] for i in range(p[5])]

    dvle = struct.unpack_from('<I', d, 8)[0]
    e = words(d, dvle, 15)
    if (e[1] >> 16) & 0xFF != 0:
        sys.exit('first DVLE is not a vertex shader')
    main_off = e[2]

    consts = []
    for i in range(e[7]):
        typ, cid = struct.unpack_from('<HH', d, dvle + e[6] + 20 * i)
        data = words(d, dvle + e[6] + 20 * i + 4, 4)
        if typ == 2:  # float24 vector
            consts.append([cid & 0xFF] + f24_pack(data))
        elif typ != 0 or data[0]:
            sys.exit('only float constants are supported (type %d)' % typ)

    outmap = [0x1F1F1F1F] * 7
    mask = total = mode = clock = 0
    for i in range(e[11]):
        typ, reg, omask = struct.unpack_from('<HHB', d, dvle + e[10] + 8 * i)
        if not mask & (1 << reg):
            mask |= 1 << reg
            total += 1
        if typ not in OUT_TYPES:
            continue
        sem, num, cbit, m = OUT_TYPES[typ]
        if cbit is not None:
            clock |= 1 << cbit
        mode |= m
        k = 0
        for j in range(4):
            if k < num and omask & (1 << j):
                outmap[reg] = (outmap[reg] & ~(0xFF << (8 * j))) | (sem << (8 * j))
                sem += 1
                k += 1
                if typ == 0 and k == 3:
                    clock |= 1
    # The registers are filled in output order, skipping unused outputs.
    packed = [outmap[r] for r in range(7) if mask & (1 << r)]
    packed += [0x1F1F1F1F] * (7 - len(packed))

    syms = d[dvle + e[14]:]
    unifs = []
    for i in range(e[13]):
        so, start, end = struct.unpack_from('<IHH', d, dvle + e[12] + 8 * i)
        sym = syms[so:syms.index(b'\0', so)].decode('ascii')
        if 0x10 <= start < 0x70:
            unifs.append((sym, start - 0x10, end - start + 1))

    up = name.upper()
    o = sys.stdout
    o.write('/* Generated from %s by tools/shbin2c.py. DO NOT EDIT. */\n'
            % path.replace('\\', '/').split('/')[-1])
    o.write('#ifndef %s_SHBIN_H\n#define %s_SHBIN_H\n\n#include "p3d.h"\n\n'
            % (up, up))

    def arr(n, vals):
        o.write('static const u32 %s[%d] = {\n' % (n, max(1, len(vals))))
        for i in range(0, len(vals), 4):
            o.write('    ' + ', '.join('0x%08Xu' % v for v in vals[i:i + 4])
                    + ',\n')
        if not vals:
            o.write('    0,\n')
        o.write('};\n\n')

    arr('%s_code' % name, code)
    arr('%s_opdesc' % name, opdesc)
    arr('%s_consts' % name, [w for c in consts for w in c])
    for sym, reg, n in unifs:
        o.write('#define %s_UNIF_%s %du /* %d vector%s */\n'
                % (up, sym.upper(), reg, n, '' if n == 1 else 's'))
    o.write('\nstatic const P3dShader %s = {\n' % name)
    o.write('    %s_code, %d, %s_opdesc, %d, %s_consts, %d,\n'
            % (name, len(code), name, len(opdesc), name, len(consts)))
    o.write('    %du, 0x%Xu, %du, 0x%Xu, 0x%Xu,\n'
            % (main_off, mask, total, mode, clock))
    o.write('    {' + ', '.join('0x%08Xu' % v for v in packed) + '},\n')
    o.write('};\n\n#endif\n')


if __name__ == '__main__':
    main()
