"""Takes the Mii art out of My Avatar Editor's character file for the launcher's Mii Maker.

My Avatar Editor (Trevor McCauley, 2009, MIT licence; native/launcher/third_party/myavatareditor/License.txt) draws
Miis from vector art in myavatarcharacter.swf: one sprite per part (heads, eyes, hair, ...) with a frame per
choice, and named children that take their own colour (lips, eyeball, nose skin, lenses). This reads that file,
flattens each part's frames into a list of shapes with their matrices and colour layers, and writes the shapes
as filled outlines (curves cut into short lines, solid or gradient fills) into native/launcher/mii_art.cpp as one
byte array, for mii_render.cpp to draw. The editor's own file, myavatareditor.swf, adds the picker icons (Icons_hair,
Icons_eye, ...: a frame per choice, in the Wii's order), exported as parts named icon.hair, icon.eye, and so on.

Fills keep the SWF's own rule: an edge borders a fill on either side, so the edges touching a fill, filled with
the even-odd rule, give that fill's area. Edges with the same fill on both sides are inside it and dropped.
Strokes (the icons' outlines, and a few lines in the character) become fills too: a quad along each piece of the
line and a round join at each point, all turning the same way, filled with the non-zero rule (kind | 0x80) so
the overlaps don't cancel.

Usage: python scripts/59_export_mii_art.py [path/to/myavatarcharacter.swf [path/to/myavatareditor.swf]]
"""
import math
import struct
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / 'scripts' / 'lib'))
import swfparse  # noqa: E402

third_party = root / 'native/launcher/third_party/myavatareditor'
swf_path = Path(sys.argv[1]) if len(sys.argv) > 1 else third_party / 'myavatarcharacter.swf'
editor_path = Path(sys.argv[2]) if len(sys.argv) > 2 else third_party / 'myavatareditor.swf'
out_path = root / 'native/launcher/mii_art.cpp'
swf = swfparse.load(str(swf_path))
editor = swfparse.load(str(editor_path))
shapes, sprites = swf['shapes'], swf['sprites']

IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
NO_CXFORM = ([1.0, 1.0, 1.0, 1.0], [0, 0, 0, 0])


def mul(m, n):
    """m after n: a point goes through n, then m. (a, b, c, d, tx, ty): x' = a x + c y + tx, y' = b x + d y + ty."""
    a, b, c, d, tx, ty = m
    e, f, g, h, ux, uy = n
    return (a * e + c * f, b * e + d * f, a * g + c * h, b * g + d * h, a * ux + c * uy + tx, b * ux + d * uy + ty)


def invert(m):
    a, b, c, d, tx, ty = m
    det = a * d - b * c
    if abs(det) < 1e-12:
        return (0, 0, 0, 0, 0, 0)
    ia, ib, ic, id_ = d / det, -b / det, -c / det, a / det
    return (ia, ib, ic, id_, -(ia * tx + ic * ty), -(ib * tx + id_ * ty))


def cx_mul(outer, inner):
    """inner, then outer."""
    (om, oa), (im, ia) = outer, inner
    return ([om[i] * im[i] for i in range(4)], [om[i] * ia[i] + oa[i] for i in range(4)])


# ---- shapes ----

def flatten_edge(e):
    x0, y0, cx, cy, x1, y1 = [None if v is None else v / 20.0 for v in e[:6]]
    if cx is None:
        return [(x0, y0, x1, y1)]
    length = math.hypot(cx - x0, cy - y0) + math.hypot(x1 - cx, y1 - cy)
    steps = max(2, min(16, int(length / 2.0)))
    pts = []
    for i in range(steps + 1):
        t = i / steps
        u = 1 - t
        pts.append((u * u * x0 + 2 * u * t * cx + t * t * x1, u * u * y0 + 2 * u * t * cy + t * t * y1))
    return [(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1]) for i in range(steps)]


def stroke_outline(segs, width):
    """Quads along the pieces and round joins, all clockwise on screen (y down), for the non-zero rule."""
    half = max(width, 0.5) / 2
    out = []
    points = set()
    for x0, y0, x1, y1 in segs:
        dx, dy = x1 - x0, y1 - y0
        length = math.hypot(dx, dy)
        if length > 1e-6:
            nx, ny = -dy / length * half, dx / length * half
            quad = [(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)]
            area = sum(quad[i][0] * quad[(i + 1) % 4][1] - quad[(i + 1) % 4][0] * quad[i][1] for i in range(4))
            if area > 0:
                quad.reverse()
            out += [(quad[i][0], quad[i][1], quad[(i + 1) % 4][0], quad[(i + 1) % 4][1]) for i in range(4)]
        points.add((round(x0, 3), round(y0, 3)))
        points.add((round(x1, 3), round(y1, 3)))
    sides = 10
    for cx, cy in points:  # angles going down, which is the same turn as the quads
        ring = [(cx + half * math.cos(-2 * math.pi * i / sides), cy + half * math.sin(-2 * math.pi * i / sides)) for i in range(sides)]
        out += [(ring[i][0], ring[i][1], ring[(i + 1) % sides][0], ring[(i + 1) % sides][1]) for i in range(sides)]
    return out


shape_blob = []   # per shape: list of fills (kind, colour or stops, inverse gradient matrix, segments)
shape_index = {}  # (file, shape id) -> place in shape_blob
for file_no, sid in [(0, k) for k in sorted(shapes)] + [(1, k) for k in sorted(editor['shapes'])]:
    sh = (shapes if file_no == 0 else editor['shapes'])[sid]
    fills = []
    for set_no, (styles, _lines) in enumerate(sh['sets']):
        for k, style in enumerate(styles, start=1):
            segs = []
            for e in sh['edges']:
                if e[9] != set_no or e[6] == e[7] or k not in (e[6], e[7]):
                    continue
                segs.extend(flatten_edge(e))
            if not segs:
                continue
            if style[0] == 'solid':
                fills.append((0, [(0, style[1])], IDENTITY, segs))
            elif style[0] == 'gradient':
                stops, m, kind = style[1], style[2], style[3]
                # The gradient square is -16384..16384 twips; its matrix takes it to the shape, in twips.
                to_shape_px = (m[0] / 20.0, m[1] / 20.0, m[2] / 20.0, m[3] / 20.0, m[4], m[5])
                fills.append((1 if kind == 0x10 else 2, stops, invert(to_shape_px), segs))
            else:  # bitmaps don't occur in this file; a grey stands in
                fills.append((0, [(0, (128, 128, 128, 255))], IDENTITY, segs))
    for set_no, (_styles, lines) in enumerate(sh['sets']):
        for k, (width, color) in enumerate(lines, start=1):
            segs = []
            for e in sh['edges']:
                if e[9] == set_no and e[8] == k:
                    segs.extend(flatten_edge(e))
            if segs:
                fills.append((0x80, [(0, color)], IDENTITY, stroke_outline(segs, width)))
    shape_index[(file_no, sid)] = len(shape_blob)
    shape_blob.append(fills)


# ---- parts: each frame flattened to (shape, matrix, layer, colour transform) ----

def flatten_frame(sprite_id, frame_no, matrix, cxform, layer, named_frame, items, file_no=0):
    shapes, sprites = (swf['shapes'], swf['sprites']) if file_no == 0 else (editor['shapes'], editor['sprites'])
    frames, _labels = sprites[sprite_id]
    frame = frames[min(frame_no, len(frames) - 1)]
    for depth in sorted(frame):
        p = frame[depth]
        cid = p.get('id')
        if cid is None or 'clipdepth' in p:
            continue
        m = mul(matrix, p.get('matrix', IDENTITY))
        cx = cx_mul(cxform, p['cxform']) if 'cxform' in p else cxform
        if cid in shapes:
            items.append((shape_index[(file_no, cid)], m, layer, cx))
        elif cid in sprites:
            name = p.get('name')
            if name in named_frame:  # a child the code colours and steps with its parent
                flatten_frame(cid, named_frame[name], m, cx, name, {}, items, file_no)
            else:
                flatten_frame(cid, 0, m, cx, layer, {}, items, file_no)


def child_placements(sprite_id):
    frames, _ = sprites[sprite_id]
    return {p.get('name'): (p['id'], p.get('matrix', IDENTITY)) for p in frames[0].values() if p.get('name')}


character = 393  # com.myavatareditor.fla.character.AvatarCharacter
top = child_placements(character)
head_id, head_matrix = top['head']
headback_id, headback_matrix = top['headback']
body_id, body_matrix = top['body']
shadow_id, shadow_matrix = top['shadow']
head_parts = child_placements(head_id)
body_parts = child_placements(body_id)
shirt_parts = child_placements(body_parts['shirt'][0])
pants_parts = child_placements(body_parts['pants'][0])
CHILD_LAYERS = {'mouth': 'lips', 'eyesLeft': 'eyeball', 'eyesRight': 'eyeball', 'nose': 'skin', 'glasses': 'lenses'}
LAYER_IDS = {None: 0, 'lips': 1, 'eyeball': 2, 'skin': 3, 'lenses': 4}

parts = []  # (name, placement matrix, frames: list of item lists)


def add_part(name, sprite_id, placement, child=None):
    frames = []
    for f in range(len(sprites[sprite_id][0])):
        items = []
        flatten_frame(sprite_id, f, IDENTITY, NO_CXFORM, None, {child: f} if child else {}, items)
        frames.append(items)
    parts.append((name, placement, frames))


head_order = sorted(head_parts.items(), key=lambda kv: [p.get('depth') for p in sprites[head_id][0][0].values() if p.get('name') == kv[0]][0])
for name, (sid, m) in head_order:
    add_part('head.' + name, sid, m, CHILD_LAYERS.get(name))
add_part('headback.hair', child_placements(headback_id)['hair'][0], child_placements(headback_id)['hair'][1])
add_part('shadow', shadow_id, shadow_matrix)
for name, (sid, m) in shirt_parts.items():
    add_part('shirt.' + name, sid, m)
for name, (sid, m) in pants_parts.items():
    add_part('pants.' + name, sid, m)
# The picker icons, by their class names in the editor's file. Icon frames are in the character's frame order.
# The last eyebrow is empty in both files: it's the "no eyebrows" choice.
icon_ids = {n: cid for cid, n in editor['names'].items() if n.startswith('Icons_')}
for name in ['head', 'face', 'hair', 'eyebrow', 'eye', 'nose', 'mouth', 'glasses', 'mustache', 'beard', 'mole']:
    sid = icon_ids['Icons_' + name]
    frames = []
    for f in range(len(editor['sprites'][sid][0])):
        items = []
        flatten_frame(sid, f, IDENTITY, NO_CXFORM, None, {}, items, 1)
        frames.append(items)
    parts.append(('icon.' + name, IDENTITY, frames))
placements = {'head': head_matrix, 'headback': headback_matrix, 'body': body_matrix, 'shadow': shadow_matrix,
              'shirt': body_parts['shirt'][1], 'pants': body_parts['pants'][1]}

# ---- the blob ----
out = bytearray(b'MIIART2\0')
out += struct.pack('<I', len(shape_blob))
for fills in shape_blob:
    out += struct.pack('<I', len(fills))
    for kind, stops, inv, segs in fills:
        out += struct.pack('<BB', kind, len(stops))
        for ratio, rgba in stops:
            out += struct.pack('<5B', ratio, *rgba)
        out += struct.pack('<6f', *inv)
        out += struct.pack('<I', len(segs))
        for s in segs:
            out += struct.pack('<4f', *s)
out += struct.pack('<I', len(placements))
for name, m in placements.items():
    out += name.encode() + b'\0' + struct.pack('<6f', *m)
out += struct.pack('<I', len(parts))
for name, placement, frames in parts:
    out += name.encode() + b'\0' + struct.pack('<6f', *placement) + struct.pack('<I', len(frames))
    for items in frames:
        out += struct.pack('<I', len(items))
        for shape, m, layer, (cm, ca) in items:
            out += struct.pack('<HB', shape, LAYER_IDS.get(layer, 0)) + struct.pack('<6f', *m)
            out += struct.pack('<4f', *cm) + struct.pack('<4h', *[int(v) for v in ca])

lines = ['// Generated by scripts/59_export_mii_art.py from My Avatar Editor\'s myavatarcharacter.swf',
         '// and myavatareditor.swf (the picker icons).',
         '// Art copyright (c) 2009 Trevor McCauley, MIT licence: third_party/myavatareditor/License.txt.',
         '#include <cstddef>', '', 'extern const unsigned char kMiiArt[];', 'extern const size_t kMiiArtSize;',
         'const size_t kMiiArtSize = %d;' % len(out), 'const unsigned char kMiiArt[] = {']
for i in range(0, len(out), 24):
    lines.append('    ' + ','.join(str(b) for b in out[i:i + 24]) + ',')
lines.append('};')
out_path.write_text('\n'.join(lines) + '\n')
segments = sum(len(f[3]) for fills in shape_blob for f in fills)
print('%d shapes, %d fills, %d segments, %d parts, %d bytes -> %s' % (len(shape_blob), sum(len(f) for f in shape_blob),
                                                                   segments, len(parts), len(out), out_path))
for name, placement, frames in parts:
    print('  %-22s %3d frames' % (name, len(frames)))
