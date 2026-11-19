// Draws a Mii the way My Avatar Editor does (Trevor McCauley, 2009, MIT licence; third_party/myavatareditor).
//
// The art is its character file's vector shapes, exported by scripts/59_export_mii_art.py into mii_art.cpp: per
// part (face, eyes, hair, ...) a frame per choice, each a list of shapes with matrices and colour layers. This
// follows AvatarCharacter.as and AvatarBody.as: which frame each value shows (their lookup tables), where each
// part goes and how big (their ranges, mapped linearly from the Mii's value), the colour transforms (their
// palettes), the body scaled by height and weight, the head set on the neck, and the eyes and eyebrows moved in
// front of the hair when they sit outside it.
//
// Drawing is done here, on the CPU: each fill's edges are filled with the even-odd rule, four sub-rows a pixel
// with exact coverage across, over solid colours or the linear and radial gradients the art uses for shading.
#include "mii_render.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

extern const unsigned char kMiiArt[];
extern const size_t kMiiArtSize;

namespace mii_render {
namespace {

struct Mat {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    Mat operator*(const Mat& n) const { // n first, then this
        return {a * n.a + c * n.b, b * n.a + d * n.b, a * n.c + c * n.d, b * n.c + d * n.d,
                a * n.tx + c * n.ty + tx, b * n.tx + d * n.ty + ty};
    }
    void apply(float x, float y, float& ox, float& oy) const {
        ox = a * x + c * y + tx;
        oy = b * x + d * y + ty;
    }
    Mat inverse() const {
        float det = a * d - b * c;
        if (std::fabs(det) < 1e-12f) return {0, 0, 0, 0, 0, 0};
        Mat r{d / det, -b / det, -c / det, a / det, 0, 0};
        r.tx = -(r.a * tx + r.c * ty);
        r.ty = -(r.b * tx + r.d * ty);
        return r;
    }
};

// Flash's ColorTransform: each channel times a multiplier, plus an offset.
struct CT {
    float m[4] = {1, 1, 1, 1};
    float o[4] = {0, 0, 0, 0};
};
CT ct(float rm, float gm, float bm, float am, float ro, float go, float bo, float ao) {
    CT t;
    t.m[0] = rm, t.m[1] = gm, t.m[2] = bm, t.m[3] = am;
    t.o[0] = ro, t.o[1] = go, t.o[2] = bo, t.o[3] = ao;
    return t;
}
CT after(const CT& outer, const CT& inner) { // inner first
    CT t;
    for (int i = 0; i < 4; i++) {
        t.m[i] = outer.m[i] * inner.m[i];
        t.o[i] = outer.m[i] * inner.o[i] + outer.o[i];
    }
    return t;
}

struct Seg {
    float x0, y0, x1, y1;
};
struct Fill {
    int kind = 0; // 0 solid, 1 linear gradient, 2 radial
    bool nonzero = false; // a stroke's outline: filled where any of its pieces is, rather than even-odd
    std::vector<std::pair<uint8_t, std::array<uint8_t, 4>>> stops;
    Mat inv;      // shape pixels to the gradient square, -16384 to 16384
    std::vector<Seg> segs;
};
struct Shape {
    std::vector<Fill> fills;
};
struct Item {
    int shape = 0, layer = 0;
    Mat m;
    CT ct;
};
struct Part {
    Mat place;
    std::vector<std::vector<Item>> frames;
};
struct Art {
    bool ok = false;
    std::vector<Shape> shapes;
    std::map<std::string, Mat> places;
    std::map<std::string, Part> parts;
};

struct Reader {
    const unsigned char* p;
    const unsigned char* end;
    bool bad = false;
    template <class T> T get() {
        T v{};
        if (p + sizeof(T) > end) {
            bad = true;
            return v;
        }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::string str() {
        std::string s;
        while (p < end && *p) s += char(*p++);
        if (p < end) p++;
        return s;
    }
    Mat mat() {
        Mat m;
        m.a = get<float>(), m.b = get<float>(), m.c = get<float>(), m.d = get<float>(), m.tx = get<float>(), m.ty = get<float>();
        return m;
    }
};

const Art& art() {
    static Art a = [] {
        Art art;
        Reader r{kMiiArt, kMiiArt + kMiiArtSize};
        if (kMiiArtSize < 8 || std::memcmp(kMiiArt, "MIIART2", 7)) return art;
        r.p += 8;
        uint32_t shapes = r.get<uint32_t>();
        art.shapes.resize(shapes);
        for (auto& s : art.shapes) {
            s.fills.resize(r.get<uint32_t>());
            for (auto& f : s.fills) {
                f.kind = r.get<uint8_t>();
                f.nonzero = f.kind & 0x80;
                f.kind &= 0x7F;
                int n = r.get<uint8_t>();
                for (int i = 0; i < n; i++) {
                    uint8_t ratio = r.get<uint8_t>();
                    std::array<uint8_t, 4> rgba{r.get<uint8_t>(), r.get<uint8_t>(), r.get<uint8_t>(), r.get<uint8_t>()};
                    f.stops.push_back({ratio, rgba});
                }
                f.inv = r.mat();
                f.segs.resize(r.get<uint32_t>());
                for (auto& sg : f.segs) sg = {r.get<float>(), r.get<float>(), r.get<float>(), r.get<float>()};
            }
        }
        uint32_t places = r.get<uint32_t>();
        for (uint32_t i = 0; i < places; i++) {
            std::string name = r.str();
            art.places[name] = r.mat();
        }
        uint32_t parts = r.get<uint32_t>();
        for (uint32_t i = 0; i < parts; i++) {
            std::string name = r.str();
            Part& part = art.parts[name];
            part.place = r.mat();
            part.frames.resize(r.get<uint32_t>());
            for (auto& items : part.frames) {
                items.resize(r.get<uint32_t>());
                for (auto& it : items) {
                    it.shape = r.get<uint16_t>();
                    it.layer = r.get<uint8_t>();
                    it.m = r.mat();
                    for (float& v : it.ct.m) v = r.get<float>();
                    for (float& v : it.ct.o) v = r.get<int16_t>();
                }
            }
        }
        art.ok = !r.bad && !art.parts.empty();
        return art;
    }();
    return a;
}

// ---- Filling ----

struct Canvas {
    int w, h;
    std::vector<float> px; // premultiplied RGBA, 0 to 1
};

std::array<float, 4> stop_color(const Fill& f, float t, const CT& t_ct) {
    float ratio = std::clamp(t, 0.0f, 1.0f) * 255.0f;
    const auto& s = f.stops;
    std::array<float, 4> c{};
    if (s.empty()) return c;
    if (ratio <= s.front().first || s.size() == 1) {
        for (int i = 0; i < 4; i++) c[i] = s.front().second[i];
    } else if (ratio >= s.back().first) {
        for (int i = 0; i < 4; i++) c[i] = s.back().second[i];
    } else {
        size_t k = 1;
        while (k < s.size() && s[k].first < ratio) k++;
        float span = float(s[k].first - s[k - 1].first), u = span > 0 ? (ratio - s[k - 1].first) / span : 0;
        for (int i = 0; i < 4; i++) c[i] = s[k - 1].second[i] * (1 - u) + s[k].second[i] * u;
    }
    for (int i = 0; i < 4; i++) c[i] = std::clamp(c[i] * t_ct.m[i] + t_ct.o[i], 0.0f, 255.0f) / 255.0f;
    return c;
}

void fill(Canvas& cv, const Fill& f, const Mat& m, const CT& t, bool multiply) {
    struct Edge {
        float y0, y1, x0, dxdy;
        int dir; // +1 going down, -1 going up, for the non-zero rule
    };
    std::vector<Edge> edges;
    edges.reserve(f.segs.size());
    float top = 1e9f, bottom = -1e9f, left = 1e9f, right = -1e9f;
    for (const Seg& s : f.segs) {
        float x0, y0, x1, y1;
        m.apply(s.x0, s.y0, x0, y0);
        m.apply(s.x1, s.y1, x1, y1);
        left = std::min({left, x0, x1});
        right = std::max({right, x0, x1});
        if (y0 == y1) continue;
        int dir = 1;
        if (y0 > y1) {
            std::swap(y0, y1);
            std::swap(x0, x1);
            dir = -1;
        }
        edges.push_back({y0, y1, x0, (x1 - x0) / (y1 - y0), dir});
        top = std::min(top, y0);
        bottom = std::max(bottom, y1);
    }
    if (edges.empty()) return;
    int row0 = std::max(0, int(std::floor(top))), row1 = std::min(cv.h - 1, int(std::ceil(bottom)));
    int col0 = std::max(0, int(std::floor(left))), col1 = std::min(cv.w - 1, int(std::ceil(right)));
    if (row0 > row1 || col0 > col1) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    std::vector<float> cover(cv.w + 2, 0.0f);
    std::vector<std::pair<float, int>> xs; // crossings and their directions
    std::vector<std::pair<float, float>> spans;
    std::vector<const Edge*> active;
    size_t next = 0;
    // From the canvas back to the gradient square: shape pixels first, then the fill's own matrix.
    Mat to_gradient = f.inv * m.inverse();
    std::array<float, 4> solid = stop_color(f, 0, t);
    constexpr int kSub = 4;
    for (int row = row0; row <= row1; row++) {
        std::fill(cover.begin() + col0, cover.begin() + col1 + 2, 0.0f);
        bool any = false;
        for (int sub = 0; sub < kSub; sub++) {
            float y = row + (sub + 0.5f) / kSub;
            while (next < edges.size() && edges[next].y0 <= y) active.push_back(&edges[next++]);
            xs.clear();
            for (size_t i = 0; i < active.size();) {
                if (active[i]->y1 <= y) {
                    active[i] = active.back();
                    active.pop_back();
                    continue;
                }
                if (active[i]->y0 <= y) xs.push_back({active[i]->x0 + (y - active[i]->y0) * active[i]->dxdy, active[i]->dir});
                i++;
            }
            std::sort(xs.begin(), xs.end());
            spans.clear();
            if (!f.nonzero) {
                for (size_t i = 0; i + 1 < xs.size(); i += 2) spans.push_back({xs[i].first, xs[i + 1].first}); // even-odd
            } else {
                int winding = 0;
                for (size_t i = 0; i + 1 < xs.size(); i++) {
                    winding += xs[i].second;
                    if (winding != 0) {
                        if (!spans.empty() && spans.back().second == xs[i].first) spans.back().second = xs[i + 1].first;
                        else spans.push_back({xs[i].first, xs[i + 1].first});
                    }
                }
            }
            for (const auto& span : spans) {
                float a = std::clamp(span.first, float(col0), float(col1 + 1)), b = std::clamp(span.second, float(col0), float(col1 + 1));
                if (b <= a) continue;
                any = true;
                int ia = int(a), ib = int(b);
                const float w = 1.0f / kSub;
                if (ia == ib) {
                    cover[ia] += (b - a) * w;
                    continue;
                }
                cover[ia] += (ia + 1 - a) * w;
                for (int x = ia + 1; x < ib; x++) cover[x] += w;
                if (ib <= col1) cover[ib] += (b - ib) * w;
            }
        }
        if (!any) continue;
        float* out = &cv.px[size_t(row) * cv.w * 4];
        for (int x = col0; x <= col1; x++) {
            float c = std::min(cover[x], 1.0f);
            if (c <= 0.002f) continue;
            std::array<float, 4> col = solid;
            if (f.kind) {
                float gx, gy;
                to_gradient.apply(x + 0.5f, row + 0.5f, gx, gy);
                float tt = f.kind == 1 ? (gx + 16384.0f) / 32768.0f : std::sqrt(gx * gx + gy * gy) / 16384.0f;
                col = stop_color(f, tt, t);
            }
            float a = col[3] * c;
            float* p = out + x * 4;
            if (multiply) { /* the face details' multiply blend: darkens the skin beneath */
                for (int i = 0; i < 3; i++) p[i] = p[i] * (1 - a) + p[i] * col[i] * a;
            } else {
                for (int i = 0; i < 3; i++) p[i] = col[i] * a + p[i] * (1 - a);
                p[3] = a + p[3] * (1 - a);
            }
        }
    }
}

void draw_items(Canvas& cv, const std::vector<Item>& items, const Mat& m, const CT& own, const CT* layers, bool multiply) {
    const Art& a = art();
    for (const Item& it : items) {
        if (it.shape < 0 || size_t(it.shape) >= a.shapes.size()) continue;
        CT t = after(it.layer ? after(own, layers[it.layer]) : own, it.ct);
        Mat mm = m * it.m;
        for (const Fill& f : a.shapes[it.shape].fills) fill(cv, f, mm, t, multiply);
    }
}

// The points a frame's shapes reach, through m: for the body's footing and for fitting the view.
void extent(const std::vector<Item>& items, const Mat& m, float& top, float& bottom, float& left, float& right) {
    const Art& a = art();
    for (const Item& it : items) {
        if (size_t(it.shape) >= a.shapes.size()) continue;
        Mat mm = m * it.m;
        for (const Fill& f : a.shapes[it.shape].fills)
            for (const Seg& s : f.segs) {
                float x, y;
                mm.apply(s.x0, s.y0, x, y);
                top = std::min(top, y), bottom = std::max(bottom, y), left = std::min(left, x), right = std::max(right, x);
            }
    }
}

// ---- Flash display object transforms: scale, skew, and position, as AS3 sets them ----

struct Xform {
    float x, y, sx, sy, skew_x, skew_y; // skew_y is the x axis's angle, which AS3 calls rotation
    static Xform of(const Mat& m) {
        Xform t;
        t.x = m.tx, t.y = m.ty;
        t.sx = std::sqrt(m.a * m.a + m.b * m.b);
        t.sy = std::sqrt(m.c * m.c + m.d * m.d);
        t.skew_y = std::atan2(m.b, m.a);
        t.skew_x = std::atan2(-m.c, m.d);
        return t;
    }
    void rotation_degrees(float r) {
        float delta = r * 3.14159265f / 180.0f - skew_y;
        skew_y += delta;
        skew_x += delta;
    }
    Mat mat() const {
        return {sx * std::cos(skew_y), sx * std::sin(skew_y), -sy * std::sin(skew_x), sy * std::cos(skew_x), x, y};
    }
};

// ---- My Avatar Editor's tables (AvatarCharacter.as, AvatarBody.as) ----

const int kEyeFrames[48] = {3, 7, 1, 43, 2, 25, 30, 37, 4, 17, 46, 14, 18, 27, 47, 10, 9, 6, 34, 15, 12, 21, 45, 19,
                            31, 22, 8, 11, 35, 42, 32, 33, 16, 13, 20, 24, 28, 29, 39, 5, 23, 26, 40, 44, 38, 41, 36, 48};
const int kBrowFrames[24] = {2, 4, 15, 16, 12, 11, 1, 7, 9, 5, 14, 13, 3, 20, 17, 19, 23, 10, 22, 6, 18, 8, 21, 24};
const int kHairFrames[72] = {60, 43, 66, 50, 41, 45, 53, 48, 46, 64, 52, 55, 37, 38, 49, 71, 62, 57, 65, 44, 54, 59, 51, 28,
                             70, 42, 40, 47, 67, 72, 34, 12, 13, 1, 36, 58, 31, 15, 26, 5, 2, 32, 27, 25, 4, 7, 63, 14,
                             16, 8, 20, 3, 18, 68, 30, 21, 10, 35, 19, 9, 23, 61, 24, 56, 22, 33, 17, 29, 11, 39, 6, 69};
const int kNoseFrames[12] = {6, 1, 3, 4, 8, 7, 5, 11, 9, 10, 2, 12};
const int kMouthFrames[24] = {7, 2, 15, 17, 18, 6, 11, 13, 8, 14, 9, 20, 24, 12, 23, 19, 10, 16, 22, 3, 21, 4, 5, 1};
const float kHeadOffsets[8] = {80, 77, 94, 86, 83, 77, 93, 96};
constexpr int kHeadTypes = 8, kShadedGlasses = 7, kMaxFaceDepth = 10, kMinXBelowHair = 37, kMaxYBelowHair = -67;

// value in [lo, hi] mapped onto [a, b], as Range.valueAt(percentAt(value))
float range(int value, int lo, int hi, float a, float b) {
    float v = float(std::clamp(value, lo, hi));
    return hi == lo ? a : a + (b - a) * (v - lo) / float(hi - lo);
}

int frame_index(const int* table, int n, int value) { return table[std::clamp(value, 0, n - 1)] - 1; }

const CT kSkin[6] = {ct(0.88f, 0.85f, 0.8f, 1, 235, 205, 187, 0), ct(-0.02f, 0.23f, 0.39f, 1, 248, 187, 124, 0),
                     ct(0.13f, 0.42f, 0.44f, 1, 214, 134, 71, 0), ct(-0.02f, 0.28f, 0.38f, 1, 246, 174, 136, 0),
                     ct(0.42f, 0.44f, 0.45f, 1, 142, 73, 31, 0),  ct(0.44f, 0.44f, 0.43f, 1, 72, 38, 13, 0)};
const CT kHair[8] = {ct(0.43f, 0.43f, 0.42f, 1, 15, 14, 12, 0),  ct(0.44f, 0.44f, 0.45f, 1, 46, 25, 6, 0),
                     ct(0.4f, 0.4f, 0.39f, 1, 82, 37, 14, 0),    ct(0.43f, 0.43f, 0.43f, 1, 102, 56, 22, 0),
                     ct(0.43f, 0.43f, 0.43f, 1, 105, 106, 108, 0), ct(0.43f, 0.42f, 0.42f, 1, 61, 44, 14, 0),
                     ct(0.44f, 0.44f, 0.44f, 1, 113, 82, 25, 0), ct(0.24f, 0.37f, 0.42f, 1, 185, 149, 87, 0)};
const CT kEye[6] = {ct(0, 0, 0, 1, 0, 0, 0, 0),                    ct(0.09f, 0.11f, 0.1f, 1, 109, 123, 124, 0),
                    ct(0.22f, 0.24f, 0.25f, 1, 108, 80, 65, 0),   ct(0.13f, 0.14f, 0.22f, 1, 122, 122, 40, 0),
                    ct(0.21f, 0.21f, 0.27f, 1, 84, 110, 180, 0),  ct(0.16f, 0.15f, 0.11f, 1, 48, 140, 79, 0)};
const CT kMouth[3] = {ct(0.7f, 0.6f, 0.55f, 1, 180, 69, 35, 0), ct(1, 0.45f, 0.3f, 1, 220, 51, 40, 0),
                      ct(1, 0.6f, 0.6f, 1, 197, 84, 86, 0)};
const CT kGlasses[6] = {ct(0.17f, 0.18f, 0.18f, 1, 76, 85, 84, 0),  ct(0.28f, 0.31f, 0.37f, 1, 105, 81, 21, 0),
                        ct(0.09f, 0.37f, 0.41f, 1, 163, 43, 16, 0), ct(0.37f, 0.31f, 0.19f, 1, 23, 69, 128, 0),
                        ct(0.15f, 0.19f, 0.29f, 1, 169, 107, 8, 0), ct(0.21f, 0.22f, 0.24f, 1, 171, 172, 164, 0)};
const CT kFavorite[12] = {ct(0.9911f, 0.47f, 0.51f, 1, 191, 38, 2, 0), ct(0.05f, 0.37f, 0.44f, 1, 240, 128, 42, 0),
                          ct(0.32f, 0.47f, 0.55f, 1, 235, 190, 27, 0),  ct(0.53f, 0.52f, 0.53f, 1, 131, 167, 19, 0),
                          ct(0.58f, 0.47f, 0.47f, 1, 1, 82, 28, 0),     ct(0.42f, 0.43f, 0.22f, 1, 5, 41, 151, 0),
                          ct(0.51f, 0.42f, 0.36f, 1, 78, 142, 199, 0),  ct(0.35f, 0.50f, 0.50f, 1, 241, 96, 96, 0),
                          ct(0.38f, 0.48f, 0.32f, 1, 93, 30, 153, 0),   ct(0.40f, 0.42f, 0.42f, 1, 82, 53, 22, 0),
                          ct(0.45f, 0.45f, 0.45f, 1, 190, 190, 190, 0), ct(0.8f, 0.8f, 0.8f, 1, 5, 5, 5, 0)};
const CT kPants = ct(0.4f, 0.41f, 0.44f, 1, 40, 42, 50, 0); // a Mii made on this console

template <class T, size_t N> const T& pick(const T (&table)[N], int value) { return table[std::clamp(value, 0, int(N) - 1)]; }

CT reduce(const CT& c, float amount) { // ColorUtils.reduceColor
    float a = 1 - amount;
    CT r;
    for (int i = 0; i < 4; i++) {
        r.m[i] = c.m[i] - (c.m[i] - 1) * a;
        r.o[i] = c.o[i] * a;
    }
    return r;
}

struct Placed {
    std::string part;
    int frame;
    Mat m;
    CT own;
    CT layers[5];
    bool multiply = false;
};

// Everything to draw, in order, in the character's own space (feet at y 0).
std::vector<Placed> layout(const mii::Mii& mi, bool with_body) {
    const Art& a = art();
    std::vector<Placed> out;
    auto part = [&](const std::string& name) -> const Part* {
        auto it = a.parts.find(name);
        return it == a.parts.end() ? nullptr : &it->second;
    };
    // Body (AvatarBody): scales from height and weight, limbs placed from the torso out.
    float hf = std::clamp(mi.height, 0, 127) / 127.0f;
    float scale_h = 0.40f + 0.60f * hf;
    float scale_w = (0.45f + 0.55f * std::clamp(mi.weight, 0, 127) / 127.0f) * (0.60f + 0.40f * hf);
    std::map<std::string, Mat> body;
    auto set = [&](const std::string& name, float sx, float sy, float x, float y, bool keep_xy) {
        const Part* p = part(name);
        if (!p) return;
        Xform t = Xform::of(p->place);
        t.sx = sx;
        t.sy = sy;
        if (!keep_xy) t.x = x, t.y = y;
        body[name] = t.mat();
    };
    auto at = [&](const std::string& name, float x, float y) {
        float ox, oy;
        body[name].apply(x, y, ox, oy);
        return std::pair<float, float>{ox, oy};
    };
    set("shirt.torso", scale_w, scale_h, 0, 0, true);
    auto arm = at("shirt.torso", 20, -64);
    set("shirt.upperarm_left", scale_h, scale_w, -arm.first, arm.second, false);
    set("shirt.upperarm_right", scale_h, scale_w, arm.first, arm.second, false);
    auto lower = at("shirt.upperarm_right", 50, 0);
    set("shirt.lowerarm_left", scale_h, scale_w, -lower.first, lower.second, false);
    set("shirt.lowerarm_right", scale_h, scale_w, lower.first, lower.second, false);
    auto hand = at("shirt.lowerarm_right", 48, -1);
    set("shirt.hand_left", scale_w, scale_w, -hand.first, hand.second, false);
    set("shirt.hand_right", scale_w, scale_w, hand.first, hand.second, false);
    set("pants.torso", scale_w, scale_h, 0, 0, true);
    auto leg = at("pants.torso", 20, 3);
    set("pants.leg_left", scale_w, scale_h, -leg.first, leg.second, false);
    set("pants.leg_right", scale_w, scale_h, leg.first, leg.second, false);
    auto foot = at("pants.leg_right", -2, 95);
    set("pants.foot_left", scale_w, scale_w, -foot.first, foot.second, false);
    set("pants.foot_right", scale_w, scale_w, foot.first, foot.second, false);
    Mat body_m = a.places.count("body") ? a.places.at("body") : Mat{};
    Mat shirt_m = body_m * (a.places.count("shirt") ? a.places.at("shirt") : Mat{});
    Mat pants_m = body_m * (a.places.count("pants") ? a.places.at("pants") : Mat{});
    // Feet on the ground: the body moves up by its lowest point.
    float top = 1e9f, bottom = -1e9f, left = 1e9f, right = -1e9f;
    for (const auto& [name, m] : body) {
        const Part* p = part(name);
        int frame = name == "shirt.torso" ? std::clamp(mi.female, 0, 1) : 0;
        if (p && !p->frames.empty()) extent(p->frames[std::min<size_t>(frame, p->frames.size() - 1)],
                                             (name.rfind("shirt.", 0) == 0 ? shirt_m : pants_m) * m, top, bottom, left, right);
    }
    Mat lift{1, 0, 0, 1, 0, bottom > -1e8f ? -bottom : 0};
    CT favorite = pick(kFavorite, mi.favorite_color);
    if (with_body) {
        out.push_back({"shadow", 0, a.places.count("shadow") ? a.places.at("shadow") : Mat{}, CT{}, {}, false});
    }
    // The neck: where the head sits.
    float neck_x, neck_y;
    (lift * shirt_m * body["shirt.torso"]).apply(0, -75, neck_x, neck_y);
    int face_frame = std::clamp(mi.face_shape, 0, kHeadTypes - 1);
    Mat head_m = a.places.count("head") ? a.places.at("head") : Mat{};
    Mat back_m = a.places.count("headback") ? a.places.at("headback") : Mat{};
    head_m.ty = neck_y - kHeadOffsets[face_frame];
    back_m.ty = head_m.ty;
    int hair = frame_index(kHairFrames, 72, mi.hair);
    const CT hair_ct = pick(kHair, mi.hair_color);
    {
        const Part* p = part("headback.hair");
        if (p) {
            Xform t = Xform::of(p->place);
            t.sx = mi.hair_part ? -1.0f : 1.0f;
            out.push_back({"headback.hair", hair, back_m * t.mat(), hair_ct, {}, false});
        }
    }
    if (with_body) {
        const char* order[] = {"pants.leg_right", "pants.leg_left", "pants.foot_right", "pants.foot_left", "pants.torso",
                               "shirt.upperarm_left", "shirt.upperarm_right", "shirt.lowerarm_right", "shirt.lowerarm_left",
                               "shirt.torso", "shirt.hand_right", "shirt.hand_left"};
        for (const char* name : order) {
            bool shirt = std::strncmp(name, "shirt.", 6) == 0;
            int frame = std::strcmp(name, "shirt.torso") == 0 ? std::clamp(mi.female, 0, 1) : 0;
            out.push_back({name, frame, lift * (shirt ? shirt_m : pants_m) * body[name], shirt ? favorite : kPants, {}, false});
        }
    }
    // Head parts, placed and coloured as AvatarCharacter.draw does.
    float eye_x = range(mi.eye_x, 0, 12, 0, 54), eye_y = range(mi.eye_y, 0, 18, -69, 35);
    float brow_x = range(mi.brow_x, 0, 12, 0, 52.5f), brow_y = range(mi.brow_y, 3, 18, -59, 25);
    std::map<std::string, Placed> head;
    auto put = [&](const std::string& name, int frame, CT own) -> Placed* {
        const Part* p = part("head." + name);
        if (!p) return nullptr;
        Placed pl{"head." + name, frame, p->place, own, {}, false};
        head[name] = pl;
        return &head[name];
    };
    auto move = [&](Placed* pl, float* x, float* y, float* scale, float* rotation, float* scale_x) {
        if (!pl) return;
        Xform t = Xform::of(pl->m);
        if (x) t.x = *x;
        if (y) t.y = *y;
        if (scale) t.sx = t.sy = *scale;
        if (scale_x) t.sx = *scale_x;
        if (rotation) t.rotation_degrees(*rotation);
        pl->m = t.mat();
    };
    CT skin = pick(kSkin, mi.skin);
    put("face", face_frame, skin);
    Placed* features = put("features", std::clamp(mi.face_feature, 0, 11) * kHeadTypes + face_frame, CT{});
    if (features) features->multiply = mi.face_feature >= 3;
    CT beard_ct = pick(kHair, mi.facial_hair_color);
    put("beard", std::clamp(mi.beard, 0, 3) * kHeadTypes + face_frame, beard_ct);
    Placed* mole = put("mole", std::clamp(mi.mole, 0, 1), CT{});
    Placed* mouth = put("mouth", frame_index(kMouthFrames, 24, mi.mouth), CT{});
    if (mouth) mouth->layers[1] = reduce(pick(kMouth, mi.mouth_color), 0.4f);
    CT eye_ct = pick(kEye, mi.eye_color);
    Placed* eye_l = put("eyesLeft", frame_index(kEyeFrames, 48, mi.eye), CT{});
    Placed* eye_r = put("eyesRight", frame_index(kEyeFrames, 48, mi.eye), CT{});
    if (eye_l) eye_l->layers[2] = eye_ct;
    if (eye_r) eye_r->layers[2] = eye_ct;
    CT brow_ct = pick(kHair, mi.brow_color);
    Placed* brow_l = put("eyebrowsLeft", frame_index(kBrowFrames, 24, mi.brow), brow_ct);
    Placed* brow_r = put("eyebrowsRight", frame_index(kBrowFrames, 24, mi.brow), brow_ct);
    Placed* hair_front = put("hairFront", hair, hair_ct);
    Placed* hats = put("hats", hair, favorite);
    Placed* mustache = put("mustache", std::clamp(mi.mustache, 0, 3), beard_ct);
    Placed* nose = put("nose", frame_index(kNoseFrames, 12, mi.nose), CT{});
    if (nose) nose->layers[3] = skin;
    int glasses_frame = std::clamp(mi.glasses, 0, 8);
    CT glasses_ct = pick(kGlasses, mi.glasses_color);
    Placed* glasses = put("glasses", glasses_frame, glasses_frame < kShadedGlasses - 1 ? glasses_ct : CT{});
    if (glasses) glasses->layers[4] = glasses_ct;

    float neg_eye_x = -eye_x, neg_brow_x = -brow_x;
    float eye_size = range(mi.eye_size, 0, 7, 0.26f, 1), brow_size = range(mi.brow_size, 0, 8, 0.25f, 1);
    float eye_rot = range(mi.eye_rotation, 0, 7, -81.5f, 0), brow_rot = range(mi.brow_rotation, 0, 11, -125, 0);
    float r_right = -eye_rot, r_left = 180 + eye_rot, b_right = -brow_rot, b_left = 180 + brow_rot;
    move(eye_r, &eye_x, &eye_y, &eye_size, &r_right, nullptr);
    move(eye_l, &neg_eye_x, &eye_y, &eye_size, &r_left, nullptr);
    move(brow_r, &brow_x, &brow_y, &brow_size, &b_right, nullptr);
    move(brow_l, &neg_brow_x, &brow_y, &brow_size, &b_left, nullptr);
    float nose_y = range(mi.nose_y, 0, 18, -10, 70), nose_s = range(mi.nose_size, 0, 8, 0.2f, 1);
    move(nose, nullptr, &nose_y, &nose_s, nullptr, nullptr);
    float mouth_y = range(mi.mouth_y, 0, 18, -11, 89), mouth_s = range(mi.mouth_size, 0, 8, 0.218f, 1);
    move(mouth, nullptr, &mouth_y, &mouth_s, nullptr, nullptr);
    float glasses_y = range(mi.glasses_y, 0, 20, -47, 49), glasses_s = range(mi.glasses_size, 0, 7, 0.4f, 1.46f);
    move(glasses, nullptr, &glasses_y, &glasses_s, nullptr, nullptr);
    float mole_x = range(mi.mole_x, 0, 16, -71, 71), mole_y = range(mi.mole_y, 0, 30, -60, 100), mole_s = range(mi.mole_size, 0, 8, 0.4f, 1.25f);
    move(mole, &mole_x, &mole_y, &mole_s, nullptr, nullptr);
    float mus_y = range(mi.mustache_y, 0, 16, 0, 86), mus_s = range(mi.mustache_size, 0, 8, 0.25f, 1);
    move(mustache, nullptr, &mus_y, &mus_s, nullptr, nullptr);
    float flip = mi.hair_part ? -1.0f : 1.0f;
    move(hair_front, nullptr, nullptr, nullptr, nullptr, &flip);
    move(hats, nullptr, nullptr, nullptr, nullptr, &flip);

    // Drawing order: the head's own, then setChildIndex as the editor calls it.
    std::vector<std::string> order = {"face", "features", "beard", "mole", "mouth", "eyesLeft", "eyesRight",
                                      "eyebrowsLeft", "eyebrowsRight", "hairFront", "hats", "mustache", "nose", "glasses"};
    auto to_index = [&](const std::string& name, int index) {
        auto it = std::find(order.begin(), order.end(), name);
        if (it == order.end()) return;
        order.erase(it);
        order.insert(order.begin() + std::min<size_t>(index, order.size()), name);
    };
    for (const char* n : {"eyesLeft", "eyesRight", "eyebrowsLeft", "eyebrowsRight", "hairFront", "hats"}) to_index(n, kMaxFaceDepth);
    if (eye_x > kMinXBelowHair || eye_y < kMaxYBelowHair) {
        to_index("eyesLeft", kMaxFaceDepth);
        to_index("eyesRight", kMaxFaceDepth);
    }
    if (brow_x > kMinXBelowHair || eye_y < kMaxYBelowHair) {
        to_index("eyebrowsLeft", kMaxFaceDepth);
        to_index("eyebrowsRight", kMaxFaceDepth);
    }
    for (const auto& name : order) {
        auto it = head.find(name);
        if (it == head.end()) continue;
        Placed pl = it->second;
        pl.m = head_m * pl.m;
        out.push_back(pl);
    }
    return out;
}

// The extent of the placed parts: all of them, the head's, or one part's.
struct Box {
    float top = 1e9f, bottom = -1e9f, left = 1e9f, right = -1e9f;
    bool empty() const { return top > bottom; }
};
Box box_of(const std::vector<Placed>& items, const char* prefix) {
    Box b;
    for (const Placed& p : items) {
        if (prefix && p.part.rfind(prefix, 0) != 0) continue;
        auto it = art().parts.find(p.part);
        if (it == art().parts.end() || it->second.frames.empty()) continue;
        extent(it->second.frames[std::min<size_t>(p.frame, it->second.frames.size() - 1)], p.m, b.top, b.bottom, b.left, b.right);
    }
    return b;
}

// Fixed frames, so a short Mii looks short and every thumbnail of a choice is at the same scale: the body view
// fits the tallest, widest Mii; the head view is sized from a default face and centred on this Mii's face.
const Box& body_frame() {
    static Box b = [] {
        mii::Mii big;
        big.height = 127;
        big.weight = 127;
        Box r = box_of(layout(big, true), nullptr);
        float half = std::max(-r.left, r.right);
        r.left = -half, r.right = half;
        r.top -= (r.bottom - r.top) * 0.05f; // room for tall hair and hats
        return r;
    }();
    return b;
}
float face_height() {
    static float h = [] {
        Box f = box_of(layout(mii::Mii{}, false), "head.face");
        return f.empty() ? 100.0f : f.bottom - f.top;
    }();
    return h;
}

}  // namespace

bool art_ok() { return art().ok; }

Image draw(const mii::Mii& m, int width, int height, View view) {
    Image img;
    img.width = width;
    img.height = height;
    img.rgba.assign(size_t(width) * height * 4, 0);
    if (!art().ok || width <= 0 || height <= 0) return img;
    std::vector<Placed> items = layout(m, view == View::Body);
    Box frame;
    if (view == View::Body) {
        frame = body_frame();
    } else {
        Box face = box_of(items, "head.face");
        if (face.empty()) return img;
        float fh = face_height(), cx = (face.left + face.right) * 0.5f, cy = (face.top + face.bottom) * 0.5f - fh * 0.1f;
        frame.top = cy - fh * 0.7f, frame.bottom = cy + fh * 0.7f, frame.left = cx - fh * 0.7f, frame.right = cx + fh * 0.7f;
    }
    if (frame.empty()) return img;
    float margin = 0.03f;
    float sw = (frame.right - frame.left) * (1 + 2 * margin), sh = (frame.bottom - frame.top) * (1 + 2 * margin);
    float scale = std::min(width / sw, height / sh);
    Mat view_m{scale, 0, 0, scale, width * 0.5f - (frame.left + frame.right) * 0.5f * scale,
               height * 0.5f - (frame.top + frame.bottom) * 0.5f * scale};
    Canvas cv{width, height, std::vector<float>(size_t(width) * height * 4, 0.0f)};
    for (const Placed& p : items) {
        auto it = art().parts.find(p.part);
        if (it == art().parts.end() || it->second.frames.empty()) continue;
        const auto& frame_items = it->second.frames[std::min<size_t>(p.frame, it->second.frames.size() - 1)];
        draw_items(cv, frame_items, view_m * p.m, p.own, p.layers, p.multiply);
    }
    for (size_t i = 0; i < cv.px.size(); i++) img.rgba[i] = uint8_t(std::clamp(cv.px[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    return img;
}

namespace {
// Which table orders a set's choices: the grid shows frames in order, and a frame's value is found through the
// same lookup the character uses (AbstractCharacteristicsGUI.selectItemHandler, frameToValue).
const int* order_table(const std::string& set, int* count) {
    struct Entry {
        const char* name;
        const int* table;
        int count;
    };
    static const Entry entries[] = {{"hair", kHairFrames, 72}, {"eye", kEyeFrames, 48}, {"eyebrow", kBrowFrames, 24},
                                    {"nose", kNoseFrames, 12},  {"mouth", kMouthFrames, 24}};
    for (const Entry& e : entries)
        if (set == e.name) {
            *count = e.count;
            return e.table;
        }
    return nullptr;
}
}  // namespace

int choice_count(const char* set) {
    auto it = art().parts.find(std::string("icon.") + set);
    return it == art().parts.end() ? 0 : int(it->second.frames.size());
}

int choice_value(const char* set, int position) {
    int n = 0;
    const int* table = order_table(set, &n);
    if (!table) return position;
    for (int v = 0; v < n; v++)
        if (table[v] - 1 == position) return v;
    return position;
}

int choice_position(const char* set, int value) {
    int n = 0;
    const int* table = order_table(set, &n);
    return table && value >= 0 && value < n ? table[value] - 1 : value;
}

Image icon(const char* set, int position, int width, int height) {
    Image img;
    img.width = width;
    img.height = height;
    img.rgba.assign(size_t(width) * height * 4, 0);
    auto it = art().parts.find(std::string("icon.") + set);
    if (it == art().parts.end() || width <= 0 || height <= 0) return img;
    const Part& part = it->second;
    if (position < 0 || size_t(position) >= part.frames.size()) return img;
    static std::map<std::string, Box> frames; // the set's extent over all its icons
    Box& b = frames[set];
    if (b.empty())
        for (const auto& items : part.frames) extent(items, Mat{}, b.top, b.bottom, b.left, b.right);
    if (b.empty()) return img;
    float margin = 0.04f;
    float sw = (b.right - b.left) * (1 + 2 * margin), sh = (b.bottom - b.top) * (1 + 2 * margin);
    float scale = std::min(width / sw, height / sh);
    Mat view_m{scale, 0, 0, scale, width * 0.5f - (b.left + b.right) * 0.5f * scale, height * 0.5f - (b.top + b.bottom) * 0.5f * scale};
    Canvas cv{width, height, std::vector<float>(size_t(width) * height * 4, 0.0f)};
    CT none;
    CT layers[5];
    draw_items(cv, part.frames[position], view_m, none, layers, false);
    for (size_t i = 0; i < cv.px.size(); i++) img.rgba[i] = uint8_t(std::clamp(cv.px[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    return img;
}

namespace {
// The rows of an image that have something drawn: the first and the last, or -1.
std::pair<int, int> drawn_rows(const Image& im) {
    int first = -1, last = -1;
    for (int y = 0; y < im.height; y++)
        for (int x = 0; x < im.width; x++)
            if (im.rgba[(size_t(y) * im.width + x) * 4 + 3] > 16) {
                if (first < 0) first = y;
                last = y;
                break;
            }
    return {first, last};
}
}  // namespace

int self_test() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) failures++;
    };
    check(art_ok(), "the Mii art loads");
    struct Set {
        const char* name;
        int count, first; // choices, and the Wii's first choice (a new Mii's value)
    };
    const Set sets[] = {{"head", 8, 0},   {"face", 12, 0}, {"hair", 72, 33},   {"eyebrow", 24, 6},
                        {"eye", 48, 2},   {"nose", 12, 1}, {"mouth", 24, 23},  {"glasses", 9, 0},
                        {"mustache", 4, 0}, {"beard", 4, 0}, {"mole", 2, 0}};
    bool counts = true, orders = true, firsts = true, icons = true;
    for (const Set& set : sets) {
        counts = counts && choice_count(set.name) == set.count;
        std::vector<int> seen(set.count, 0);
        for (int p = 0; p < set.count; p++) {
            int v = choice_value(set.name, p);
            if (v < 0 || v >= set.count || seen[v]++ || choice_position(set.name, v) != p) orders = false;
            // every icon draws something, but the last eyebrow, which is "no eyebrows"
            bool none = std::string(set.name) == "eyebrow" && p == 23;
            if (!none && drawn_rows(icon(set.name, p, 32, 32)).first < 0) icons = false;
        }
        firsts = firsts && choice_value(set.name, 0) == set.first;
    }
    check(counts, "11 icon sets with the Mii Channel's numbers of choices");
    check(orders, "each set's grid order covers every value once and goes back and forth");
    check(firsts, "each grid starts with a new Mii's choice, as the Wii's does");
    check(icons, "every icon draws something (but \"no eyebrows\")");
    mii::Mii m;
    auto body = drawn_rows(draw(m, 120, 200, View::Body));
    check(body.first > 0 && body.second > 180, "a Mii's body is drawn, standing at the bottom");
    m.height = 0;
    auto small = drawn_rows(draw(m, 120, 200, View::Body));
    m.height = 127;
    auto tall = drawn_rows(draw(m, 120, 200, View::Body));
    check(small.first > body.first && body.first > tall.first && std::abs(small.second - tall.second) <= 2,
          "a shorter Mii is drawn shorter, on the same ground");
    bool heads = true;
    for (int v = 0; v < 72; v++) {
        mii::Mii h;
        h.hair = v;
        if (drawn_rows(draw(h, 48, 48, View::Head)).first < 0) heads = false;
    }
    check(heads, "every hairstyle draws a head");
    return failures;
}

}  // namespace mii_render
