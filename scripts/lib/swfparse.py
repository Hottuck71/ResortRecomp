"""A small SWF reader: shapes (DefineShape 1-4), sprites (DefineSprite), placements with names, matrices, and
colour transforms, and exported names. Enough to take the vector art out of My Avatar Editor's character file."""
import struct
import zlib


class Bits:
    def __init__(self, data, pos=0):
        self.data = data
        self.pos = pos * 8

    def align(self):
        self.pos = (self.pos + 7) & ~7

    def ub(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.pos >> 3]
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def sb(self, n):
        if n == 0:
            return 0
        v = self.ub(n)
        if v & (1 << (n - 1)):
            v -= 1 << n
        return v

    def fb(self, n):
        return self.sb(n) / 65536.0

    def byte_pos(self):
        self.align()
        return self.pos >> 3

    def u8(self):
        self.align()
        v = self.data[self.pos >> 3]
        self.pos += 8
        return v

    def u16(self):
        self.align()
        v = struct.unpack_from('<H', self.data, self.pos >> 3)[0]
        self.pos += 16
        return v

    def u32(self):
        self.align()
        v = struct.unpack_from('<I', self.data, self.pos >> 3)[0]
        self.pos += 32
        return v


def read_rect(b):
    n = b.ub(5)
    return [b.sb(n) for _ in range(4)]


def read_matrix(b):
    b.align()
    sx = sy = 1.0
    r0 = r1 = 0.0
    if b.ub(1):
        n = b.ub(5)
        sx, sy = b.fb(n), b.fb(n)
    if b.ub(1):
        n = b.ub(5)
        r0, r1 = b.fb(n), b.fb(n)
    n = b.ub(5)
    tx, ty = b.sb(n), b.sb(n)
    return (sx, r0, r1, sy, tx / 20.0, ty / 20.0)  # a, b, c, d, tx, ty (pixels)


def read_cxform(b, alpha):
    b.align()
    has_add, has_mult = b.ub(1), b.ub(1)
    n = b.ub(4)
    mult = [256, 256, 256, 256]
    add = [0, 0, 0, 0]
    k = 4 if alpha else 3
    if has_mult:
        mult = [b.sb(n) for _ in range(k)] + ([256] if not alpha else [])
    if has_add:
        add = [b.sb(n) for _ in range(k)] + ([0] if not alpha else [])
    return ([m / 256.0 for m in mult], add)


def read_rgb(b, alpha):
    r, g, bl = b.u8(), b.u8(), b.u8()
    a = b.u8() if alpha else 255
    return (r, g, bl, a)


def read_gradient(b, alpha, focal):
    b.align()
    spread, interp, count = b.ub(2), b.ub(2), b.ub(4)
    stops = []
    for _ in range(count):
        ratio = b.u8()
        stops.append((ratio, read_rgb(b, alpha)))
    if focal:
        b.u16()
    return stops


def read_fill_styles(b, tag):
    alpha = tag >= 32
    count = b.u8()
    if count == 0xFF and tag >= 22:
        count = b.u16()
    styles = []
    for _ in range(count):
        kind = b.u8()
        if kind == 0x00:
            styles.append(('solid', read_rgb(b, alpha)))
        elif kind in (0x10, 0x12, 0x13):
            m = read_matrix(b)
            stops = read_gradient(b, alpha, kind == 0x13)
            styles.append(('gradient', stops, m, kind))
        elif kind in (0x40, 0x41, 0x42, 0x43):
            bid = b.u16()
            m = read_matrix(b)
            styles.append(('bitmap', bid, m))
        else:
            raise ValueError('fill kind %x' % kind)
    return styles


def read_line_styles(b, tag):
    alpha = tag >= 32
    count = b.u8()
    if count == 0xFF:
        count = b.u16()
    styles = []
    for _ in range(count):
        width = b.u16() / 20.0
        if tag == 83:
            b.align()
            start_cap, join, has_fill, no_h, no_v, pixel, _r, no_close, end_cap = (b.ub(2), b.ub(2), b.ub(1), b.ub(1), b.ub(1), b.ub(1), b.ub(5), b.ub(1), b.ub(2))
            if join == 2:
                b.u16()
            if has_fill:
                fs = read_fill_styles_one(b, tag)
                color = fs[1] if fs[0] == 'solid' else (0, 0, 0, 255)
            else:
                color = read_rgb(b, True)
        else:
            color = read_rgb(b, alpha)
        styles.append((width, color))
    return styles


def read_fill_styles_one(b, tag):
    alpha = tag >= 32
    kind = b.u8()
    if kind == 0x00:
        return ('solid', read_rgb(b, alpha))
    if kind in (0x10, 0x12, 0x13):
        m = read_matrix(b)
        return ('gradient', read_gradient(b, alpha, kind == 0x13), m, kind)
    bid = b.u16()
    read_matrix(b)
    return ('bitmap', bid, None)


def read_shape(data, tag):
    b = Bits(data)
    sid = b.u16()
    bounds = read_rect(b)
    if tag == 83:
        b.align()
        read_rect(b)
        b.u8()
    fills = read_fill_styles(b, tag)
    lines = read_line_styles(b, tag)
    b.align()
    nf, nl = b.ub(4), b.ub(4)
    x = y = 0
    fill0 = fill1 = line = 0
    edges = []  # (x0,y0,cx,cy,x1,y1 or None for straight, fill0, fill1, line, styleset)
    styleset = 0
    sets = [(fills, lines)]
    while True:
        if b.ub(1) == 0:  # style change or end
            flags = b.ub(5)
            if flags == 0:
                break
            if flags & 1:
                n = b.ub(5)
                x, y = b.sb(n), b.sb(n)
            if flags & 2:
                fill0 = b.ub(nf)
            if flags & 4:
                fill1 = b.ub(nf)
            if flags & 8:
                line = b.ub(nl)
            if flags & 16:
                fills = read_fill_styles(b, tag)
                lines = read_line_styles(b, tag)
                sets.append((fills, lines))
                styleset = len(sets) - 1
                b.align()
                nf, nl = b.ub(4), b.ub(4)
                fill0 = fill1 = line = 0
        else:
            if b.ub(1):  # straight
                n = b.ub(4) + 2
                if b.ub(1):
                    dx, dy = b.sb(n), b.sb(n)
                elif b.ub(1):
                    dx, dy = 0, b.sb(n)
                else:
                    dx, dy = b.sb(n), 0
                edges.append((x, y, None, None, x + dx, y + dy, fill0, fill1, line, styleset))
                x += dx
                y += dy
            else:
                n = b.ub(4) + 2
                cdx, cdy, adx, ady = b.sb(n), b.sb(n), b.sb(n), b.sb(n)
                cx, cy = x + cdx, y + cdy
                ax, ay = cx + adx, cy + ady
                edges.append((x, y, cx, cy, ax, ay, fill0, fill1, line, styleset))
                x, y = ax, ay
    return sid, {'bounds': [v / 20.0 for v in bounds], 'sets': sets, 'edges': edges}


def read_place(data, tag):
    b = Bits(data)
    p = {'tag': tag}
    if tag == 4:
        p['id'] = b.u16()
        p['depth'] = b.u16()
        p['matrix'] = read_matrix(b)
        return p
    flags = b.u8()
    flags2 = b.u8() if tag == 70 else 0
    p['depth'] = b.u16()
    p['move'] = bool(flags & 1)
    if tag == 70 and (flags2 & 8 or (flags2 & 16 and flags & 2)):
        pass
    if tag == 70 and flags2 & 8:  # class name
        name = bytearray()
        while True:
            c = b.u8()
            if c == 0:
                break
            name.append(c)
        p['class'] = name.decode('latin1')
    if flags & 2:
        p['id'] = b.u16()
    if flags & 4:
        p['matrix'] = read_matrix(b)
    if flags & 8:
        p['cxform'] = read_cxform(b, True)
    if flags & 16:
        p['ratio'] = b.u16()
    if flags & 32:
        name = bytearray()
        while True:
            c = b.u8()
            if c == 0:
                break
            name.append(c)
        p['name'] = name.decode('latin1')
    if flags & 64:
        p['clipdepth'] = b.u16()
    return p


def read_tags(data, pos, end):
    tags = []
    while pos < end:
        code_len = struct.unpack_from('<H', data, pos)[0]
        pos += 2
        code, length = code_len >> 6, code_len & 0x3F
        if length == 0x3F:
            length = struct.unpack_from('<I', data, pos)[0]
            pos += 4
        tags.append((code, data[pos:pos + length]))
        pos += length
        if code == 0:
            break
    return tags


def timeline(tags):
    """Frames of display lists: list of frames, each a dict depth -> placement (with id)."""
    frames = []
    labels = {}
    current = {}
    for code, body in tags:
        if code in (4, 26, 70):
            p = read_place(body, code)
            d = p['depth']
            if p.get('move') and d in current:
                merged = dict(current[d])
                merged.update({k: v for k, v in p.items() if k not in ('tag', 'move')})
                current[d] = merged
            else:
                current[d] = p
        elif code in (5, 28):
            b = Bits(body)
            if code == 5:
                b.u16()
            d = b.u16()
            current.pop(d, None)
        elif code == 43:
            labels[len(frames)] = body.split(b'\0')[0].decode('latin1')
        elif code == 1:
            frames.append({d: dict(p) for d, p in current.items()})
    return frames, labels


def load(path):
    raw = open(path, 'rb').read()
    sig = raw[:3]
    if sig == b'CWS':
        data = raw[:8] + zlib.decompress(raw[8:])
    elif sig == b'FWS':
        data = raw
    else:
        raise ValueError('not an uncompressed or zlib SWF: %r' % sig)
    b = Bits(data, 8)
    read_rect(b)
    pos = b.byte_pos() + 4
    tags = read_tags(data, pos, len(data))
    shapes, sprites, names = {}, {}, {}
    for code, body in tags:
        if code in (2, 22, 32, 83):
            sid, shape = read_shape(body, code)
            shapes[sid] = shape
        elif code == 39:
            sid = struct.unpack_from('<H', body, 0)[0]
            inner = read_tags(body, 4, len(body))
            sprites[sid] = timeline(inner)
        elif code in (56, 76):
            count = struct.unpack_from('<H', body, 0)[0]
            p = 2
            for _ in range(count):
                cid = struct.unpack_from('<H', body, p)[0]
                p += 2
                end = body.index(b'\0', p)
                names[cid] = body[p:end].decode('latin1')
                p = end + 1
    main = timeline(tags)
    return {'shapes': shapes, 'sprites': sprites, 'names': names, 'main': main, 'tags': tags}
