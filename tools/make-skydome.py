r"""Build the perk-menu sky (constellation skydome) with N tree slots for the Custom Skills Framework ring - ONE command.

  py -3 tools\make-skydome.py [--bsa "<Stock Game>\Data\Skyrim - Meshes1.bsa" | --src <loose intperkskydome.nif>]
                              [--slots 20] [--pictures blank,mirror:14] [--out testonly\CustomSkillsRing\meshes\interface\NjordlingerCGO\intperkskydome20.nif]

Base: the VANILLA meshes\interface\intperkskydome.nif, read straight out of the game's Skyrim - Meshes1.bsa (nothing is
unpacked anywhere). --bsa defaults to <NJCGO_MO2_INSTANCE>\Stock Game\Data\Skyrim - Meshes1.bsa. --src takes a loose
file instead (any SSE skydome with the vanilla Point/cImage layout).

What the game looks up (why this is the whole job). SkyrimSE.exe builds the sky's node names from two format strings,
"cImage%02i" and "Point%02i", one pair per tree index. Point<i> is the empty node the ring rotates to for tree i;
cImage<i> is the constellation picture that fades in when tree i is selected. Custom Skills Framework 3.2.0 keeps one
fade controller per tree (CustomSkillsManager.cpp:103, sized to the skill count) and hooks the vanilla InitModel loop
that fetches each cImage<i>'s shader (Hooks/Constellation.cpp:21-55, index i = loop counter), and it picks the tree
from the camera angle with an even 360/N split (Hooks/Navigation.cpp:26-37). So an N-tree ring needs Point00..PointN-1
and cImage00..cImageN-1, spaced evenly at 360/N degrees.

What this script does, at the NIF block level (no third-party NIF library; the file's header lists every block's
size, so blocks this script does not touch are copied byte for byte):
 1. Re-spaces the 18 vanilla slots from 20 degrees to 360/N: slot i is turned about the dome's vertical axis by
    (20 - 360/N) * i degrees (both its Point and its cImage, translation and rotation), so each picture keeps its
    vanilla offset from its own tree.
 2. Adds slots 18..N-1: a copy of Point00 turned to its angle, and a cImage per --pictures entry (in slot order):
      copy:<v>    a copy of vanilla cImage<v>, turned from its vanilla angle to the new slot's angle;
      mirror:<v>  the same, with the picture mirrored left-right (vertices, normals, tangents reflected across the
                  vertical plane through the dome axis and the picture's centre; triangle winding reversed);
      blank       a copy of cImage00 with the hidden (APP_CULLED) flag set: the slot exists, so the framework has a
                  shader to fade and nothing can reach a missing slot, but no picture shows.
    Each new cImage has its own copies of the shader and alpha property, so each slot fades on its own. The new
    blocks are appended to OrderedRenderingNode's children and to the object palette. No texture outside the vanilla
    constellation atlas (textures\interface\INTConstellationImages.dds) is referenced.
    Defaults for the 20-tree ring (18 = Diving, 19 = Necromancer): blank,mirror:14. Diving has no constellation
    picture anywhere (Underwater NG's own DivingSkydome.nif has no cImage shape at all; its art is a whole-sky water
    panorama licensed only as part of Underwater NG), so its slot is blank like its own sky. Necromancer has no art of
    its own (its sky is the vanilla mesh), so it gets Conjuration's picture (vanilla slot 14) mirrored.
 3. Checks its own output: re-reads it, refuses if any block other than the ones it meant to change differs from the
    source, and prints the per-slot list.
No machine path is written into the output.
"""
import argparse, math, os, struct, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VANILLA_SLOTS = 18
VANILLA_STEP = 360.0 / VANILLA_SLOTS
BSA_MEMBER = 'meshes/interface/intperkskydome.nif'


# ---------------------------------------------------------------- BSA (v105, SSE) read
def read_from_bsa(bsa, want):
    with open(bsa, 'rb') as f:
        magic, ver, _off, flags, nfold, _nfile, _tfl, tfil, _ff = struct.unpack('<4sIIIIIIII', f.read(36))
        if magic != b'BSA\0' or ver != 105:
            sys.exit('not an SSE (v105) BSA: %s' % bsa)
        compressed, embed = bool(flags & 4), bool(flags & 0x100)
        folders = [struct.unpack('<QIIQ', f.read(24)) for _ in range(nfold)]
        entries = []
        for _h, cnt, _, _o in folders:
            l = f.read(1)[0]
            folder = f.read(l)[:-1].decode()
            for _ in range(cnt):
                _fh, size, foff = struct.unpack('<QII', f.read(16))
                entries.append((folder, size, foff))
        names = f.read(tfil).split(b'\0')
        for (folder, size, foff), name in zip(entries, names):
            if (folder + '/' + name.decode()).replace('\\', '/').lower() != want:
                continue
            comp = compressed ^ bool(size & 0x40000000)
            f.seek(foff)
            raw = f.read(size & 0x3FFFFFFF)
            if embed:
                raw = raw[1 + raw[0]:]
            if comp:
                import lz4.frame
                raw = lz4.frame.decompress(raw[4:])
            return raw
    sys.exit('%s not found in %s' % (want, bsa))


# ---------------------------------------------------------------- NIF 20.2.0.7 / BS 100 block container
class Nif:
    def __init__(self, data):
        p = data.index(b'\n') + 1
        self.hdrline = data[:p]
        self.version, self.endian, self.user, nb = struct.unpack_from('<IBII', data, p); p += 13
        self.bsver, = struct.unpack_from('<I', data, p); p += 4
        if self.version != 0x14020007 or self.user != 12 or self.bsver != 100:
            sys.exit('expected an SSE NIF (20.2.0.7, user 12, BS 100); got %08X/%d/%d' % (self.version, self.user, self.bsver))
        self.bsstrs = []
        for _ in range(4):  # author, process script, export script, max filepath (bsver >= 103: not here, so 3)
            if len(self.bsstrs) == 3 and self.bsver < 103:
                break
            l = data[p]; self.bsstrs.append(data[p + 1:p + 1 + l]); p += 1 + l
        nt, = struct.unpack_from('<H', data, p); p += 2
        self.types = []
        for _ in range(nt):
            l, = struct.unpack_from('<I', data, p); self.types.append(data[p + 4:p + 4 + l].decode()); p += 4 + l
        self.tidx = list(struct.unpack_from('<%dH' % nb, data, p)); p += 2 * nb
        sizes = struct.unpack_from('<%dI' % nb, data, p); p += 4 * nb
        ns, self.maxlen = struct.unpack_from('<II', data, p); p += 8
        self.strings = []
        for _ in range(ns):
            l, = struct.unpack_from('<I', data, p); self.strings.append(data[p + 4:p + 4 + l]); p += 4 + l
        ng, = struct.unpack_from('<I', data, p); p += 4
        self.groups = list(struct.unpack_from('<%dI' % ng, data, p)); p += 4 * ng
        self.blocks = []
        for sz in sizes:
            self.blocks.append(bytearray(data[p:p + sz])); p += sz
        nr, = struct.unpack_from('<I', data, p); p += 4
        self.roots = list(struct.unpack_from('<%di' % nr, data, p)); p += 4 * nr
        if p != len(data):
            sys.exit('NIF parse did not end at end of file (%d of %d)' % (p, len(data)))

    def write(self):
        o = bytearray(self.hdrline)
        o += struct.pack('<IBII', self.version, self.endian, self.user, len(self.blocks))
        o += struct.pack('<I', self.bsver)
        for b in self.bsstrs:
            o += bytes([len(b)]) + b
        o += struct.pack('<H', len(self.types))
        for t in self.types:
            o += struct.pack('<I', len(t)) + t.encode()
        o += struct.pack('<%dH' % len(self.blocks), *self.tidx)
        o += struct.pack('<%dI' % len(self.blocks), *[len(b) for b in self.blocks])
        o += struct.pack('<II', len(self.strings), max([len(x) for x in self.strings] + [0]))
        for x in self.strings:
            o += struct.pack('<I', len(x)) + x
        o += struct.pack('<I', len(self.groups)) + struct.pack('<%dI' % len(self.groups), *self.groups)
        for b in self.blocks:
            o += b
        o += struct.pack('<I', len(self.roots)) + struct.pack('<%di' % len(self.roots), *self.roots)
        return bytes(o)

    def type(self, i):
        return self.types[self.tidx[i] & 0x7FFF]

    def string_index(self, s):
        s = s.encode()
        if s not in self.strings:
            self.strings.append(s)
        return self.strings.index(s)

    # NiObjectNET: name(str idx) numExtra extra[] controller ; NiAVObject: flags T(3f) R(9f) S collision
    def net_end(self, i):
        ne, = struct.unpack_from('<I', self.blocks[i], 4)
        return 8 + 4 * ne + 4

    def name(self, i):
        idx, = struct.unpack_from('<i', self.blocks[i], 0)
        return None if idx < 0 else self.strings[idx].decode()

    def set_name(self, i, s):
        struct.pack_into('<i', self.blocks[i], 0, self.string_index(s))

    def transform(self, i):
        b, p = self.blocks[i], self.net_end(i) + 4
        return list(struct.unpack_from('<3f', b, p)), list(struct.unpack_from('<9f', b, p + 12)), struct.unpack_from('<f', b, p + 48)[0]

    def set_transform(self, i, t, r):
        b, p = self.blocks[i], self.net_end(i) + 4
        struct.pack_into('<3f', b, p, *t)
        struct.pack_into('<9f', b, p + 12, *r)

    def av_end(self, i):
        return self.net_end(i) + 4 + 12 + 36 + 4 + 4

    def children(self, i):
        p = self.av_end(i)
        n, = struct.unpack_from('<I', self.blocks[i], p)
        return list(struct.unpack_from('<%di' % n, self.blocks[i], p + 4))

    def set_children(self, i, kids):
        b, p = self.blocks[i], self.av_end(i)
        n, = struct.unpack_from('<I', b, p)
        self.blocks[i] = b[:p] + struct.pack('<I', len(kids)) + struct.pack('<%di' % len(kids), *kids) + b[p + 4 + 4 * n:]

    # BSTriShape after NiAVObject: bound sphere (4f), skin ref, shader ref, alpha ref
    def shape_refs_at(self, i):
        return self.av_end(i) + 16

    def add_block(self, typename, data):
        self.tidx.append(self.types.index(typename))
        self.blocks.append(bytearray(data))
        return len(self.blocks) - 1


def rotz(deg, t, r):
    """Turn a transform (translation t, row-major 3x3 r) about the parent's Z axis: R' = Rz*R, T' = Rz*T."""
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    rz = [[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]]
    R = [r[0:3], r[3:6], r[6:9]]
    nr = [sum(rz[a][k] * R[k][b] for k in range(3)) for a in range(3) for b in range(3)]
    nt = [sum(rz[a][k] * t[k] for k in range(3)) for a in range(3)]
    return nt, nr


def find_named(nif, name, types):
    hits = [i for i in range(len(nif.blocks)) if nif.type(i) in types and nif.name(i) == name]
    if len(hits) != 1:
        sys.exit('expected exactly one %s named %s, found %d' % ('/'.join(types), name, len(hits)))
    return hits[0]


NODE_TYPES = ('NiNode',)
SHAPE_TYPES = ('BSTriShape',)


def slot_blocks(nif, n):
    return find_named(nif, 'Point%02d' % n, NODE_TYPES), find_named(nif, 'cImage%02d' % n, SHAPE_TYPES)


def mirror_shape(nif, i, theta):
    """Mirror shape block i (in place) across the vertical plane through the dome axis at angle theta (degrees),
    using the shape's current transform. Positions, normals, tangents, bitangents and the bound are reflected;
    triangle winding is reversed so the faces keep pointing the same way."""
    b = nif.blocks[i]
    t, r, s = nif.transform(i)
    R = [r[0:3], r[3:6], r[6:9]]
    w = (-math.sin(math.radians(theta)), math.cos(math.radians(theta)), 0.0)  # plane normal (dome space)
    refl = lambda p: [p[k] - 2.0 * (p[0] * w[0] + p[1] * w[1]) * w[k] for k in range(3)]
    to_world = lambda v: [t[a] + s * sum(R[a][k] * v[k] for k in range(3)) for a in range(3)]
    to_local = lambda p: [sum(R[k][a] * (p[k] - t[k]) for k in range(3)) / s for a in range(3)]
    vec_world = lambda v: [sum(R[a][k] * v[k] for k in range(3)) for a in range(3)]
    vec_local = lambda v: [sum(R[k][a] * v[k] for k in range(3)) for a in range(3)]
    pt = lambda v: to_local(refl(to_world(v)))
    vec = lambda v: vec_local(refl(vec_world(v)))
    dec = lambda x: x / 255.0 * 2.0 - 1.0
    enc = lambda x: max(0, min(255, int(round((x + 1.0) / 2.0 * 255.0))))
    p = nif.av_end(i)
    c = struct.unpack_from('<4f', b, p)
    struct.pack_into('<3f', b, p, *pt(c[:3]))
    p += 16 + 12
    desc, = struct.unpack_from('<Q', b, p)
    ntri, nv, _ds = struct.unpack_from('<HHI', b, p + 8)
    size, uvo = (desc & 0xF) * 4, ((desc >> 8) & 0xF) * 4
    nro, tgo = ((desc >> 16) & 0xF) * 4, ((desc >> 20) & 0xF) * 4
    flags = desc >> 44
    if uvo != 16 or not (flags & 0x8 and flags & 0x10):
        sys.exit('mirror: unexpected vertex layout %x (need full-precision positions, normals and tangents)' % desc)
    v0 = p + 16
    for k in range(nv):
        q = v0 + k * size
        x, y, z, bx = struct.unpack_from('<4f', b, q)
        nx, ny, nz, by = (dec(v) for v in b[q + nro:q + nro + 4])
        tx, ty, tz, bz = (dec(v) for v in b[q + tgo:q + tgo + 4])
        x, y, z = pt((x, y, z))
        bx, by, bz = vec((bx, by, bz))
        n_, t_ = vec((nx, ny, nz)), vec((tx, ty, tz))
        struct.pack_into('<4f', b, q, x, y, z, bx)
        b[q + nro:q + nro + 4] = bytes([enc(n_[0]), enc(n_[1]), enc(n_[2]), enc(by)])
        b[q + tgo:q + tgo + 4] = bytes([enc(t_[0]), enc(t_[1]), enc(t_[2]), enc(bz)])
    t0 = v0 + nv * size
    for k in range(ntri):
        i0, i1, i2 = struct.unpack_from('<3H', b, t0 + 6 * k)
        struct.pack_into('<3H', b, t0 + 6 * k, i0, i2, i1)


def picture_angle(nif, i):
    """Angle (degrees) of the picture's centre around the dome axis: bound-sphere centre through the transform."""
    c = struct.unpack_from('<3f', nif.blocks[i], nif.av_end(i))
    t, r, s = nif.transform(i)
    w = [t[a] + s * sum(r[3 * a + k] * c[k] for k in range(3)) for a in range(3)]
    return math.degrees(math.atan2(w[1], w[0]))


def parse_pictures(spec):
    out = []
    for item in spec.split(','):
        item = item.strip()
        if item == 'blank':
            out.append(('blank', 0))
            continue
        mode, _, v = item.partition(':')
        if mode not in ('copy', 'mirror') or not v.isdigit() or int(v) >= VANILLA_SLOTS:
            sys.exit('bad --pictures entry %r (use blank, copy:<0-17> or mirror:<0-17>)' % item)
        out.append((mode, int(v)))
    return out


def build(src_bytes, slots, pictures):
    nif = Nif(src_bytes)
    if nif.write() != src_bytes:
        sys.exit('round trip of the unmodified source is not byte-identical - refusing to edit it')
    step = 360.0 / slots
    changed = set()

    ordered = find_named(nif, 'OrderedRenderingNode', ('BSOrderedNode',))
    palette = [i for i in range(len(nif.blocks)) if nif.type(i) == 'NiDefaultAVObjectPalette']

    # source transforms of the vanilla slots (taken before anything is moved)
    vanilla = {}
    for n in range(VANILLA_SLOTS):
        pi, ci = slot_blocks(nif, n)
        vanilla[n] = (pi, ci, nif.transform(pi), nif.transform(ci))

    # 1. re-space the vanilla slots
    for n in range(VANILLA_SLOTS):
        pi, ci, (pt, pr, _), (ct, cr, _) = vanilla[n]
        delta = (VANILLA_STEP - step) * n          # vanilla angle 90-20n -> new angle 90-step*n
        nif.set_transform(pi, *rotz(delta, pt, pr))
        nif.set_transform(ci, *rotz(delta, ct, cr))
        changed.update((pi, ci))

    # 2. add the extra slots
    new_kids, new_palette, modes = [], [], {}
    for k, n in enumerate(range(VANILLA_SLOTS, slots)):
        mode, src = pictures[k] if k < len(pictures) else ('blank', 0)
        # Point: copy of Point00 (identity rotation at the 12 o'clock position) turned to -step*n
        p0, _, (pt, pr, _), _ = vanilla[0]
        pnew = nif.add_block(nif.type(p0), nif.blocks[p0])
        nif.set_name(pnew, 'Point%02d' % n)
        nif.set_transform(pnew, *rotz(-step * n, pt, pr))
        # cImage: copy of cImage<src> with its own shader + alpha property, turned from 90-20*src to 90-step*n
        _, cs, _, (ct, cr, _) = vanilla[src]
        q = nif.shape_refs_at(cs)
        skin, shader, alpha = struct.unpack_from('<3i', nif.blocks[cs], q)
        if skin != -1:
            sys.exit('cImage%02d is skinned - not expected' % src)
        snew = nif.add_block(nif.type(shader), nif.blocks[shader])
        anew = nif.add_block(nif.type(alpha), nif.blocks[alpha])
        cnew = nif.add_block(nif.type(cs), nif.blocks[cs])
        struct.pack_into('<3i', nif.blocks[cnew], q, -1, snew, anew)
        nif.set_name(cnew, 'cImage%02d' % n)
        if mode == 'mirror':
            mirror_shape(nif, cnew, picture_angle(nif, cnew))  # mirrored where it stands, then turned into place
        nif.set_transform(cnew, *rotz(-step * n + VANILLA_STEP * src, ct, cr))
        if mode == 'blank':
            fo = nif.net_end(cnew)
            struct.pack_into('<I', nif.blocks[cnew], fo, struct.unpack_from('<I', nif.blocks[cnew], fo)[0] | 1)
        modes[n] = 'blank (hidden)' if mode == 'blank' else '%s:%02d' % (mode, src)
        new_kids += [pnew, cnew]
        new_palette += [('Point%02d' % n, pnew), ('cImage%02d' % n, cnew)]

    nif.set_children(ordered, nif.children(ordered) + new_kids)
    changed.add(ordered)
    for pal in palette:
        b = nif.blocks[pal]
        scene, cnt = struct.unpack_from('<iI', b, 0)
        tail = b''.join(struct.pack('<I', len(nm)) + nm.encode() + struct.pack('<i', ref) for nm, ref in new_palette)
        nif.blocks[pal] = b[:4] + struct.pack('<I', cnt + len(new_palette)) + b[8:] + tail
        changed.add(pal)
    return nif, changed, modes


def verify(src_bytes, out_bytes, slots, changed, modes):
    a, b = Nif(src_bytes), Nif(out_bytes)
    if b.write() != out_bytes:
        sys.exit('VERIFY: written file does not round-trip')
    nsrc = len(a.blocks)
    problems = []
    if a.strings != b.strings[:len(a.strings)]:
        problems.append('string table prefix changed')
    if a.types != b.types or a.tidx != b.tidx[:nsrc] or a.roots != b.roots or a.groups != b.groups or a.bsstrs != b.bsstrs:
        problems.append('header changed')
    same = 0
    for i in range(nsrc):
        if a.blocks[i] == b.blocks[i]:
            same += 1
        elif i not in changed:
            problems.append('block %d (%s) changed unexpectedly' % (i, a.type(i)))
    # for the changed slot blocks: everything except the transform must be identical
    for i in changed:
        if a.type(i) in NODE_TYPES + SHAPE_TYPES and a.name(i) and a.name(i)[:5] in ('Point', 'cImag'):
            x, y = bytearray(a.blocks[i]), bytearray(b.blocks[i])
            p = a.net_end(i) + 4
            x[p:p + 48] = y[p:p + 48] = b'\0' * 48
            if x != y:
                problems.append('block %d %s changed outside its transform' % (i, a.name(i)))
    rows = []
    for n in range(slots):
        pi, ci = slot_blocks(b, n)
        pt, _, _ = b.transform(pi)
        q = b.shape_refs_at(ci)
        _, sh, al = struct.unpack_from('<3i', b.blocks[ci], q)
        # picture centre in dome space (bound-sphere centre through the shape's transform)
        c = struct.unpack_from('<3f', b.blocks[ci], b.av_end(ci))
        ct, cr, cs = b.transform(ci)
        w = [ct[r] + cs * sum(cr[3 * r + k] * c[k] for k in range(3)) for r in range(3)]
        rows.append((n, pi, ci, sh, al, math.degrees(math.atan2(pt[1], pt[0])), math.degrees(math.atan2(w[1], w[0]))))
    shaders = [r[3] for r in rows]
    if len(set(shaders)) != slots:
        problems.append('two slots share a shader property (they would fade together)')
    step = 360.0 / slots
    for n, pi, ci, sh, al, pang, cang in rows:
        want = (90.0 - step * n + 180.0) % 360.0 - 180.0
        if abs(((pang - want + 180.0) % 360.0) - 180.0) > 0.01:
            problems.append('Point%02d at %.2f deg, expected %.2f' % (n, pang, want))
    print('slot  Point blk  cImage blk  shader  alpha  Point angle  picture angle')
    for n, pi, ci, sh, al, pang, cang in rows:
        print('%4d  %9d  %10d  %6d  %5d  %11.2f  %13.2f' % (n, pi, ci, sh, al, pang, cang))
    print('blocks: source %d, output %d; unchanged source blocks %d, changed %d (allowed: %d)'
          % (nsrc, len(b.blocks), same, nsrc - same, len(changed)))
    if problems:
        sys.exit('VERIFY FAILED:\n  ' + '\n  '.join(problems))
    print('VERIFY OK: %d Point/cImage pairs, evenly spaced at %.1f degrees; nothing else changed' % (slots, step))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bsa', help='Skyrim - Meshes1.bsa holding the vanilla skydome')
    ap.add_argument('--src', help='a loose skydome NIF to use instead of the BSA')
    ap.add_argument('--slots', type=int, default=20)
    ap.add_argument('--copy-from', default='16,17', help='vanilla slots whose pictures the new slots copy, in order')
    ap.add_argument('--out', default=os.path.join(REPO, 'testonly', 'CustomSkillsRing', 'meshes', 'interface', 'NjordlingerCGO', 'intperkskydome20.nif'))
    a = ap.parse_args()
    if a.slots < VANILLA_SLOTS:
        sys.exit('--slots must be at least %d' % VANILLA_SLOTS)
    if a.src:
        src = open(a.src, 'rb').read()
    else:
        bsa = a.bsa
        if not bsa:
            inst = os.environ.get('NJCGO_MO2_INSTANCE')
            if not inst:
                sys.exit('give --bsa or --src, or set NJCGO_MO2_INSTANCE')
            bsa = os.path.join(inst, 'Stock Game', 'Data', 'Skyrim - Meshes1.bsa')
        src = read_from_bsa(bsa, BSA_MEMBER)
    copy_from = [int(x) for x in a.copy_from.split(',')]
    nif, changed = build(src, a.slots, copy_from)
    out = nif.write()
    verify(src, out, a.slots, changed)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, 'wb') as f:
        f.write(out)
    print('wrote %s (%d bytes)' % (os.path.relpath(a.out, REPO) if a.out.startswith(REPO) else a.out, len(out)))


if __name__ == '__main__':
    main()
