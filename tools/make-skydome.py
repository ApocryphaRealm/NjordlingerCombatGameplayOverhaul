r"""Build the perk-menu sky (constellation skydome) with N tree slots for the Custom Skills Framework ring - ONE command.

  py -3 tools\make-skydome.py [--bsa "<Stock Game>\Data\Skyrim - Meshes1.bsa" | --src <loose intperkskydome.nif>]
                              [--map 0..17,diving,necromancer] [--out <nif>]   (one sky)
  py -3 tools\make-skydome.py [--bsa ... | --src ...] --batch <jobs.json>        (several skies in one run)

--map lists the ring's trees in slot order, 2 to 20 entries: a vanilla tree index 0-17 (the game's own order:
0 Enchanting, 1 Smithing, 2 HeavyArmor, 3 Block, 4 TwoHanded, 5 OneHanded, 6 Archery, 7 LightArmor, 8 Sneak,
9 Lockpicking, 10 Pickpocket, 11 Speech, 12 Alchemy, 13 Illusion, 14 Conjuration, 15 Destruction, 16 Restoration,
17 Alteration; a..b ranges allowed), diving, necromancer, or the raw forms blank, copy:<0-17>, mirror:<0-17>.
--out defaults to testonly\CustomSkillsRing\meshes\interface\NjordlingerCGO\intperkskydome20.nif. --batch takes a JSON
list of {"out": <path, relative to the repo unless absolute>, "map": [...]}.

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
 1. Points: slot n uses vanilla Point<n> (n < 18) turned about the dome's vertical axis from its vanilla angle
    (90 - 20n degrees) to 90 - (360/N)n, or a copy of Point00 turned there (n >= 18). Vanilla Points past N are renamed
    PointUnusedNN and hidden.
 2. Pictures, per --map entry in slot order:
      vanilla v   the vanilla cImage<v> block itself, renamed cImage<n> and turned from 90-20v to slot n's angle
                  (so it keeps its small vanilla offset from its tree); a second use of the same v is a copy;
      copy:<v>    a copy of vanilla cImage<v>, turned the same way;
      mirror:<v>  the same, with the picture mirrored left-right (vertices, normals, tangents reflected across the
                  vertical plane through the dome axis and the picture's centre; triangle winding reversed);
      blank       a copy of cImage00 with the hidden (APP_CULLED) flag set: the slot exists, so the framework has a
                  shader to fade and nothing can reach a missing slot, but no picture shows.
    Copies get their own shader and alpha property, so each slot fades on its own, and are appended to
    OrderedRenderingNode's children and to the object palette. Vanilla pictures no slot uses are renamed
    cImageUnusedNN and hidden (blocks are never deleted, so no reference anywhere has to be renumbered); the object
    palette carries every rename. No texture outside the vanilla constellation atlas
    (textures\interface\INTConstellationImages.dds) is referenced.
    diving = blank: Diving has no constellation picture anywhere (Underwater NG's own DivingSkydome.nif has no cImage
    shape at all; its art is a whole-sky water panorama licensed only as part of Underwater NG), so its slot is blank
    like its own sky. necromancer = mirror:14: Necromancer has no art of its own (its sky is the vanilla mesh), so it
    gets Conjuration's picture mirrored.
 3. Checks its own output: re-reads it, refuses if any source block other than the ones it meant to change differs
    (and those only in name, hidden flag and transform), if any Point/cImage name lies outside 00..N-1, if the Points
    are not evenly spaced, if a shown picture sits more than 4 degrees from its Point, or if two slots share a shader;
    prints each slot's source.
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


TREE_NAMES = ['Enchanting', 'Smithing', 'HeavyArmor', 'Block', 'TwoHanded', 'OneHanded', 'Archery', 'LightArmor',
              'Sneak', 'Lockpicking', 'Pickpocket', 'Speech', 'Alchemy', 'Illusion', 'Conjuration', 'Destruction',
              'Restoration', 'Alteration']
# Pictures for the custom trees (see the module notes): Diving has no constellation picture anywhere, so its slot is
# a hidden picture; Necromancer has none of its own, so it shows Conjuration's picture mirrored.
ALIASES = {'diving': ('blank', 0), 'necromancer': ('mirror', 14)}
DEFAULT_MAP = [str(i) for i in range(VANILLA_SLOTS)] + ['diving', 'necromancer']
MAX_SLOTS = 20


def parse_map(items):
    """Map entries -> [(mode, vanilla index, label)]. Entries: 0-17 (vanilla picture), diving, necromancer,
    blank, copy:<0-17>, mirror:<0-17>; a string may also be one comma list, with a..b ranges (0..17)."""
    if isinstance(items, str):
        items = items.split(',')
    flat = []
    for it in items:
        it = str(it).strip().lower()
        if '..' in it:
            a, b = it.split('..')
            flat += [str(x) for x in range(int(a), int(b) + 1)]
        else:
            flat.append(it)
    out = []
    for it in flat:
        if it.isdigit() and int(it) < VANILLA_SLOTS:
            out.append(('vanilla', int(it), '%s (vanilla %02d)' % (TREE_NAMES[int(it)], int(it))))
        elif it in ALIASES:
            m, v = ALIASES[it]
            out.append((m, v, '%s (%s)' % (it.capitalize(), 'hidden' if m == 'blank' else '%s %02d %s' % (m, v, TREE_NAMES[v]))))
        elif it == 'blank':
            out.append(('blank', 0, 'blank (hidden)'))
        elif it.partition(':')[0] in ('copy', 'mirror') and it.partition(':')[2].isdigit() and int(it.partition(':')[2]) < VANILLA_SLOTS:
            m, _, v = it.partition(':')
            out.append((m, int(v), '%s %02d %s' % (m, int(v), TREE_NAMES[int(v)])))
        else:
            sys.exit('bad --map entry %r (use 0-17, diving, necromancer, blank, copy:<0-17> or mirror:<0-17>)' % it)
    if not 2 <= len(out) <= MAX_SLOTS:
        sys.exit('--map gives %d slots; it must give 2 to %d' % (len(out), MAX_SLOTS))
    return out


def set_hidden(nif, i):
    fo = nif.net_end(i)
    struct.pack_into('<I', nif.blocks[i], fo, struct.unpack_from('<I', nif.blocks[i], fo)[0] | 1)


def build(src_bytes, slot_map):
    """slot_map: [(mode, vanilla index, label)] for output slots 0..N-1."""
    nif = Nif(src_bytes)
    if nif.write() != src_bytes:
        sys.exit('round trip of the unmodified source is not byte-identical - refusing to edit it')
    slots = len(slot_map)
    step = 360.0 / slots
    changed = set()

    ordered = find_named(nif, 'OrderedRenderingNode', ('BSOrderedNode',))
    palette = [i for i in range(len(nif.blocks)) if nif.type(i) == 'NiDefaultAVObjectPalette']

    # source transforms of the vanilla slots (taken before anything is moved)
    vanilla = {}
    for n in range(VANILLA_SLOTS):
        pi, ci = slot_blocks(nif, n)
        vanilla[n] = (pi, ci, nif.transform(pi), nif.transform(ci))

    def delta(v, n):
        # vanilla angle 90-20v -> new angle 90-step*n (same float steps as the plain re-space when v == n)
        return (VANILLA_STEP - step) * n if v == n else VANILLA_STEP * v - step * n

    new_kids, renamed, modes = [], {}, {}
    used_vanilla_pics = set()

    # 1. Points: vanilla Point<n> re-spaced for n < 18, copies of Point00 beyond; spare vanilla Points renamed + hidden
    for n in range(max(slots, VANILLA_SLOTS)):
        if n < VANILLA_SLOTS:
            pi, _, (pt, pr, _), _ = vanilla[n]
            if n < slots:
                nif.set_transform(pi, *rotz(delta(n, n), pt, pr))
            else:
                nif.set_name(pi, 'PointUnused%02d' % n)
                set_hidden(nif, pi)
                renamed[pi] = 'PointUnused%02d' % n
            changed.add(pi)

    # 2. pictures, in slot order; extra Points are appended just before each new slot's picture
    for n, (mode, v, label) in enumerate(slot_map):
        if n >= VANILLA_SLOTS:
            p0, _, (pt, pr, _), _ = vanilla[0]
            pnew = nif.add_block(nif.type(p0), nif.blocks[p0])
            nif.set_name(pnew, 'Point%02d' % n)
            nif.set_transform(pnew, *rotz(-step * n, pt, pr))
            new_kids.append(pnew)
            renamed[pnew] = 'Point%02d' % n
        _, cs, _, (ct, cr, _) = vanilla[v]
        if mode == 'vanilla' and v not in used_vanilla_pics:
            used_vanilla_pics.add(v)
            ci = cs                                   # reuse the vanilla block itself
            if v != n:
                nif.set_name(ci, 'cImage%02d' % n)
                renamed[ci] = 'cImage%02d' % n
            nif.set_transform(ci, *rotz(delta(v, n), ct, cr))
            changed.add(ci)
        else:                                         # a copy with its own shader + alpha property
            q = nif.shape_refs_at(cs)
            skin, shader, alpha = struct.unpack_from('<3i', nif.blocks[cs], q)
            if skin != -1:
                sys.exit('cImage%02d is skinned - not expected' % v)
            snew = nif.add_block(nif.type(shader), nif.blocks[shader])
            anew = nif.add_block(nif.type(alpha), nif.blocks[alpha])
            ci = nif.add_block(nif.type(cs), nif.blocks[cs])
            struct.pack_into('<3i', nif.blocks[ci], q, -1, snew, anew)
            nif.set_name(ci, 'cImage%02d' % n)
            if mode == 'mirror':
                mirror_shape(nif, ci, picture_angle(nif, ci))  # mirrored where it stands, then turned into place
            nif.set_transform(ci, *rotz(delta(v, n), ct, cr))
            if mode == 'blank':
                set_hidden(nif, ci)
            new_kids.append(ci)
            renamed[ci] = 'cImage%02d' % n
        modes[n] = label

    # 3. vanilla pictures no slot uses: renamed out of the cImage00.. range and hidden
    for v in range(VANILLA_SLOTS):
        if v not in used_vanilla_pics:
            ci = vanilla[v][1]
            nif.set_name(ci, 'cImageUnused%02d' % v)
            set_hidden(nif, ci)
            renamed[ci] = 'cImageUnused%02d' % v
            changed.add(ci)

    if new_kids:
        nif.set_children(ordered, nif.children(ordered) + new_kids)
        changed.add(ordered)
    # palette: existing entries keep their order (renamed blocks get their new name), new blocks appended in order
    for pal in palette:
        b = nif.blocks[pal]
        scene, cnt = struct.unpack_from('<iI', b, 0)
        p, entries = 8, []
        for _ in range(cnt):
            l, = struct.unpack_from('<I', b, p)
            nm = b[p + 4:p + 4 + l].decode()
            ref, = struct.unpack_from('<i', b, p + 4 + l)
            p += 8 + l
            entries.append((renamed.get(ref, nm), ref))
        listed = {ref for _, ref in entries}
        entries += [(renamed[k], k) for k in new_kids if k not in listed]
        body = b''.join(struct.pack('<I', len(nm)) + nm.encode() + struct.pack('<i', ref) for nm, ref in entries)
        nb = b[:4] + struct.pack('<I', len(entries)) + body
        if nb != b:
            nif.blocks[pal] = bytearray(nb)
            changed.add(pal)
    return nif, changed, modes


def verify(src_bytes, out_bytes, slots, changed, modes, quiet=False):
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
    # changed slot blocks: only the name, the hidden flag and the transform may differ
    for i in changed:
        if a.type(i) in NODE_TYPES + SHAPE_TYPES and a.name(i) and a.name(i)[:5] in ('Point', 'cImag'):
            x, y = bytearray(a.blocks[i]), bytearray(b.blocks[i])
            p = a.net_end(i)
            for z in (x, y):
                z[0:4] = b'\0' * 4
                z[p:p + 4] = bytes([z[p] & 0xFE]) + bytes(z[p + 1:p + 4])
                z[p + 4:p + 52] = b'\0' * 48
            if x != y:
                problems.append('block %d %s changed outside its name, hidden flag and transform' % (i, a.name(i)))
    # no slot-named node or picture outside 00..N-1, exactly one of each inside
    for i in range(len(b.blocks)):
        if b.type(i) in NODE_TYPES + SHAPE_TYPES:
            nm = b.name(i) or ''
            for pre in ('Point', 'cImage'):
                if nm.startswith(pre) and nm[len(pre):].isdigit() and int(nm[len(pre):]) >= slots:
                    problems.append('%s is outside the %d-slot range' % (nm, slots))
    rows = []
    for n in range(slots):
        pi, ci = slot_blocks(b, n)
        pt, _, _ = b.transform(pi)
        q = b.shape_refs_at(ci)
        _, sh, al = struct.unpack_from('<3i', b.blocks[ci], q)
        hidden = struct.unpack_from('<I', b.blocks[ci], b.net_end(ci))[0] & 1
        if bool(hidden) != ('hidden' in modes.get(n, '')):
            problems.append('cImage%02d hidden flag is %d, not what was asked' % (n, hidden))
        if struct.unpack_from('<I', b.blocks[pi], b.net_end(pi))[0] & 1:
            problems.append('Point%02d is hidden' % n)
        pang, cang = math.degrees(math.atan2(pt[1], pt[0])), picture_angle(b, ci)
        if not hidden and abs((cang - pang + 180.0) % 360.0 - 180.0) > 4.0:
            problems.append('cImage%02d sits at %.2f deg, too far from its Point at %.2f' % (n, cang, pang))
        rows.append((n, pi, ci, sh, al, pang, cang))
    if len(set(r[3] for r in rows)) != slots:
        problems.append('two slots share a shader property (they would fade together)')
    step = 360.0 / slots
    for n, pi, ci, sh, al, pang, cang in rows:
        want = (90.0 - step * n + 180.0) % 360.0 - 180.0
        if abs(((pang - want + 180.0) % 360.0) - 180.0) > 0.01:
            problems.append('Point%02d at %.2f deg, expected %.2f' % (n, pang, want))
    if not quiet:
        print('slot  Point blk  cImage blk  shader  alpha  Point angle  picture angle  picture source')
        for n, pi, ci, sh, al, pang, cang in rows:
            print('%4d  %9d  %10d  %6d  %5d  %11.2f  %13.2f  %s' % (n, pi, ci, sh, al, pang, cang, modes.get(n, '?')))
    print('blocks: source %d, output %d; unchanged source blocks %d, changed %d (allowed: %d)'
          % (nsrc, len(b.blocks), same, nsrc - same, len(changed)))
    if problems:
        sys.exit('VERIFY FAILED:\n  ' + '\n  '.join(problems))
    print('VERIFY OK: %d Point/cImage pairs, evenly spaced at %.2f degrees; nothing else changed' % (slots, step))


def make_one(src, slot_map, out, quiet=False):
    nif, changed, modes = build(src, slot_map)
    data = nif.write()
    verify(src, data, len(slot_map), changed, modes, quiet)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(data)
    print('wrote %s (%d bytes, %d slots)' % (os.path.relpath(out, REPO) if os.path.abspath(out).startswith(REPO) else out,
                                             len(data), len(slot_map)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bsa', help='Skyrim - Meshes1.bsa holding the vanilla skydome')
    ap.add_argument('--src', help='a loose skydome NIF to use instead of the BSA')
    ap.add_argument('--map', default=','.join(DEFAULT_MAP),
                    help='slot list: vanilla tree index 0-17 (a..b ranges allowed), diving, necromancer, blank, '
                         'copy:<0-17>, mirror:<0-17>; 2 to 20 entries (default: 0..17,diving,necromancer)')
    ap.add_argument('--batch', help='JSON list of {"out": path, "map": [...]}; relative outs are under the repo')
    ap.add_argument('--out', default=os.path.join(REPO, 'testonly', 'CustomSkillsRing', 'meshes', 'interface', 'NjordlingerCGO', 'intperkskydome20.nif'))
    a = ap.parse_args()
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
    if a.batch:
        import json
        jobs = json.load(open(a.batch, encoding='utf-8'))
        if not isinstance(jobs, list) or not all(isinstance(j, dict) and 'out' in j and 'map' in j for j in jobs):
            sys.exit('--batch file must be a JSON list of {"out": ..., "map": [...]}')
        for j in jobs:
            out = j['out'] if os.path.isabs(j['out']) else os.path.join(REPO, j['out'])
            print('--', j['out'])
            make_one(src, parse_map(j['map']), out)
    else:
        make_one(src, parse_map(a.map), a.out)


if __name__ == '__main__':
    main()
