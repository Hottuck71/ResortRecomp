// The launcher's Mii Maker: the Mii on the left, a toolbar of parts along the bottom, and each part's choices on
// the right, in the style of the Wii's own Mii editors. The Mii and the choice icons are My Avatar Editor's art
// (Trevor McCauley, 2009, MIT licence), drawn by mii_render.cpp and kept here as textures. The left view shows
// the whole Mii on the body pages and zooms to the face on the face pages, as that editor does; the choices are
// in the order the Wii shows them.
#include "mii_maker.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "mii_db.h"
#include "mii_render.h"

namespace {

using mii::Mii;

// ---- colors ----
const ImU32 kSkin[6] = {IM_COL32(255, 212, 172, 255), IM_COL32(255, 182, 107, 255), IM_COL32(222, 121, 66, 255),
                        IM_COL32(255, 170, 140, 255), IM_COL32(173, 81, 41, 255), IM_COL32(99, 44, 24, 255)};
const ImU32 kHair[8] = {IM_COL32(30, 26, 24, 255),   IM_COL32(64, 32, 16, 255),   IM_COL32(92, 24, 10, 255),
                        IM_COL32(124, 58, 20, 255),  IM_COL32(120, 120, 128, 255), IM_COL32(78, 62, 16, 255),
                        IM_COL32(136, 88, 24, 255),  IM_COL32(208, 160, 74, 255)};
const ImU32 kEye[6] = {IM_COL32(0, 0, 0, 255),    IM_COL32(108, 112, 112, 255), IM_COL32(102, 60, 44, 255),
                       IM_COL32(96, 94, 48, 255), IM_COL32(70, 84, 168, 255),   IM_COL32(56, 112, 88, 255)};
const ImU32 kLip[3] = {IM_COL32(216, 82, 8, 255), IM_COL32(240, 12, 8, 255), IM_COL32(245, 72, 72, 255)};
const ImU32 kGlasses[6] = {IM_COL32(20, 20, 20, 255),   IM_COL32(96, 56, 16, 255),  IM_COL32(168, 16, 16, 255),
                           IM_COL32(40, 56, 136, 255),  IM_COL32(200, 120, 24, 255), IM_COL32(120, 128, 136, 255)};
const ImU32 kFavorite[12] = {IM_COL32(210, 30, 20, 255),  IM_COL32(244, 112, 32, 255), IM_COL32(248, 216, 32, 255),
                             IM_COL32(128, 200, 40, 255), IM_COL32(0, 128, 48, 255),   IM_COL32(32, 72, 168, 255),
                             IM_COL32(64, 176, 224, 255), IM_COL32(240, 104, 136, 255), IM_COL32(120, 48, 176, 255),
                             IM_COL32(96, 64, 32, 255),   IM_COL32(236, 236, 236, 255), IM_COL32(32, 32, 28, 255)};
const char* kFavoriteNames[12] = {"Red", "Orange", "Yellow", "Light green", "Green", "Blue",
                                  "Light blue", "Pink", "Purple", "Brown", "White", "Black"};
const ImU32 kInk = IM_COL32(60, 52, 44, 255), kCream = IM_COL32(255, 248, 228, 255), kAccent = IM_COL32(236, 140, 20, 255);
const ImU32 kSilhouette = IM_COL32(64, 64, 64, 255);

std::string narrow(const std::u16string& s) {
    std::string out;
    for (char16_t c : s) {
        if (c < 0x80) out.push_back(char(c));
        else if (c < 0x800) out += {char(0xC0 | (c >> 6)), char(0x80 | (c & 0x3F))};
        else out += {char(0xE0 | (c >> 12)), char(0x80 | ((c >> 6) & 0x3F)), char(0x80 | (c & 0x3F))};
    }
    return out;
}

std::u16string widen(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if (c < 0x80) out.push_back(c), i += 1;
        else if ((c >> 5) == 6 && i + 1 < s.size()) out.push_back(char16_t((c & 0x1F) << 6 | (s[i + 1] & 0x3F))), i += 2;
        else if (i + 2 < s.size()) out.push_back(char16_t((c & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F))), i += 3;
        else break;
    }
    return out;
}

// ---- pictures: the Mii and the choice icons, drawn by mii_render and kept as textures ----

SDL_Renderer* g_renderer = nullptr;
struct Picture {
    SDL_Texture* texture = nullptr;
    uint64_t used = 0;  // the frame it was last drawn in
};
std::unordered_map<std::string, Picture> g_pictures;
uint64_t g_frame = 1;
Uint64 g_frame_start = 0;
int g_waiting = 0;  // pictures put off to the next frame, in the last frame drawn

// Every number that changes how a Mii looks.
std::string look_key(const Mii& m) {
    const int fields[] = {m.female, m.favorite_color, m.height, m.weight, m.face_shape, m.skin, m.face_feature,
                          m.hair, m.hair_color, m.hair_part, m.brow, m.brow_rotation, m.brow_color, m.brow_size,
                          m.brow_y, m.brow_x, m.eye, m.eye_rotation, m.eye_y, m.eye_color, m.eye_size, m.eye_x,
                          m.nose, m.nose_size, m.nose_y, m.mouth, m.mouth_color, m.mouth_size, m.mouth_y,
                          m.glasses, m.glasses_color, m.glasses_size, m.glasses_y, m.mustache, m.beard,
                          m.facial_hair_color, m.mustache_size, m.mustache_y, m.mole, m.mole_size, m.mole_y, m.mole_x};
    return "mii:" + std::string(reinterpret_cast<const char*>(fields), sizeof fields);
}

// A picture's texture, made now if it's new. Ones that aren't urgent wait for a later frame once this frame has
// spent 10 ms making pictures, so opening a page of 72 icons doesn't stall the window.
ImTextureID picture(const std::string& key, int w, int h, const std::function<mii_render::Image(int, int)>& make, bool urgent) {
    if (!g_renderer || w <= 0 || h <= 0) return ImTextureID(0);
    std::string full = key + "@" + std::to_string(w) + "x" + std::to_string(h);
    auto it = g_pictures.find(full);
    if (it == g_pictures.end()) {
        double spent = double(SDL_GetPerformanceCounter() - g_frame_start) / double(SDL_GetPerformanceFrequency());
        if (!urgent && spent > 0.010) {
            g_waiting++;
            return ImTextureID(0);
        }
        mii_render::Image img = make(w, h);
        SDL_Texture* t = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!t) return ImTextureID(0);
        SDL_UpdateTexture(t, nullptr, img.rgba.data(), w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND_PREMULTIPLIED);  // mii_render's pixels are premultiplied
        it = g_pictures.emplace(full, Picture{t, 0}).first;
    }
    it->second.used = g_frame;
    return ImTextureID(intptr_t(it->second.texture));
}

// Once a frame, before drawing: textures not drawn for about two seconds are let go. The last frame's textures
// were already drawn, so none in use goes.
void new_picture_frame() {
    g_frame++;
    g_frame_start = SDL_GetPerformanceCounter();
    g_waiting = 0;
    for (auto it = g_pictures.begin(); it != g_pictures.end();) {
        if (g_frame - it->second.used > 120) {
            SDL_DestroyTexture(it->second.texture);
            it = g_pictures.erase(it);
        } else {
            ++it;
        }
    }
}

float pixel_scale() { return std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x); }

ImTextureID mii_picture(const Mii& m, mii_render::View view, ImVec2 size, bool urgent) {
    float s = pixel_scale();
    return picture(look_key(m) + (view == mii_render::View::Body ? "B" : "H"), int(size.x * s), int(size.y * s),
                   [&](int w, int h) { return mii_render::draw(m, w, h, view); }, urgent);
}

ImTextureID icon_picture(const char* set, int position, ImVec2 size, bool urgent) {
    float s = pixel_scale();
    return picture(std::string("icon:") + set + ":" + std::to_string(position), int(size.x * s), int(size.y * s),
                   [&](int w, int h) { return mii_render::icon(set, position, w, h); }, urgent);
}

// ---- toolbar icons ----
enum Part { kSave, kList, kBody, kFace, kFeature, kHairPart, kBrows, kEyes, kGlassesPart, kNose, kMouth, kBeard, kMole, kProfile, kParts };
const char* kPartNames[kParts] = {"Save", "Your Miis", "Height and weight", "Face shape and skin", "Face details",
                                  "Hair", "Eyebrows", "Eyes", "Glasses", "Nose", "Mouth", "Mustache and beard",
                                  "Mole", "Profile"};

// The editor's icon for a part's toolbar button: its set and which choice.
bool part_icon(int part, const char** set, int* position) {
    switch (part) {
    case kFace: *set = "head", *position = 0; return true;
    case kFeature: *set = "face", *position = 1; return true;
    case kHairPart: *set = "hair", *position = 0; return true;
    case kBrows: *set = "eyebrow", *position = 1; return true;
    case kEyes: *set = "eye", *position = 10; return true;
    case kGlassesPart: *set = "glasses", *position = 1; return true;
    case kNose: *set = "nose", *position = 0; return true;
    case kMouth: *set = "mouth", *position = 2; return true;
    case kBeard: *set = "beard", *position = 3; return true;
    case kMole: *set = "mole", *position = 1; return true;
    default: return false;
    }
}

void draw_icon(ImDrawList* d, int part, ImVec2 c, float r, ImU32 col) {
    const char* set = nullptr;
    int position = 0;
    if (part_icon(part, &set, &position)) {
        ImVec2 size(r * 2.4f, r * 2.4f);
        ImTextureID t = icon_picture(set, position, size, true);
        // the texture is premultiplied, so dimming scales every channel alike
        ImU32 tint = col == IM_COL32(40, 40, 40, 255) ? IM_COL32(255, 255, 255, 255) : IM_COL32(110, 110, 110, 110);
        if (t) d->AddImage(t, ImVec2(c.x - size.x / 2, c.y - size.y / 2), ImVec2(c.x + size.x / 2, c.y + size.y / 2), ImVec2(0, 0), ImVec2(1, 1), tint);
        return;
    }
    switch (part) {
    case kSave:
        d->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 3);
        d->AddRectFilled(ImVec2(c.x - r * 0.55f, c.y - r), ImVec2(c.x + r * 0.55f, c.y - r * 0.25f), IM_COL32(230, 230, 230, 255));
        d->AddRectFilled(ImVec2(c.x - r * 0.7f, c.y + r * 0.15f), ImVec2(c.x + r * 0.7f, c.y + r * 0.85f), IM_COL32(250, 250, 250, 255), 2);
        break;
    case kList:
        for (int i = -1; i <= 1; i++) d->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y + i * r * 0.6f - r * 0.18f), ImVec2(c.x + r * 0.8f, c.y + i * r * 0.6f + r * 0.18f), col, 2);
        break;
    case kBody:
        d->AddCircleFilled(ImVec2(c.x, c.y - r * 0.6f), r * 0.32f, col);
        d->AddRectFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.25f), ImVec2(c.x + r * 0.35f, c.y + r * 0.45f), col, 3);
        d->AddRectFilled(ImVec2(c.x - r * 0.3f, c.y + r * 0.4f), ImVec2(c.x + r * 0.3f, c.y + r), col, 2);
        break;
    case kProfile:
        d->AddRectFilled(ImVec2(c.x - r * 0.85f, c.y - r * 0.6f), ImVec2(c.x + r * 0.85f, c.y + r * 0.6f), col, 3);
        for (int i = 0; i < 2; i++) d->AddRectFilled(ImVec2(c.x - r * 0.1f, c.y - r * 0.3f + i * r * 0.35f), ImVec2(c.x + r * 0.65f, c.y - r * 0.18f + i * r * 0.35f), IM_COL32(250, 250, 250, 255));
        d->AddCircleFilled(ImVec2(c.x - r * 0.45f, c.y), r * 0.22f, IM_COL32(250, 250, 250, 255));
        break;
    }
}

// ---- widgets ----

// A button with a picture: image() gives the texture for the inside's size, and is only asked when the tile
// is on screen.
bool tile(const char* id, bool selected, ImVec2 size, float inset, const std::function<ImTextureID(ImVec2)>& image) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, size);
    if (!ImGui::IsItemVisible()) return pressed;
    bool hover = ImGui::IsItemHovered();
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), selected ? IM_COL32(255, 226, 150, 255) : hover ? IM_COL32(255, 244, 214, 255) : IM_COL32(255, 255, 255, 255), 8);
    d->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), selected ? kAccent : IM_COL32(220, 206, 180, 255), 8, 0, selected ? 3.0f : 1.5f);
    ImVec2 inner(size.x - 2 * inset, size.y - 2 * inset);
    ImTextureID t = image(inner);
    if (t) d->AddImage(t, ImVec2(p.x + inset, p.y + inset), ImVec2(p.x + inset + inner.x, p.y + inset + inner.y));
    return pressed;
}

bool swatch(const char* id, ImU32 color, bool selected, float size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + size / 2, p.y + size / 2);
    d->AddCircleFilled(c, size * 0.42f, color);
    d->AddCircle(c, size * 0.42f, selected ? kAccent : IM_COL32(200, 190, 170, 255), 24, selected ? 4.0f : 1.5f);
    return pressed;
}

// A choice grid: the editor's icon for each choice of a set, in the Wii's order.
bool choice_grid(const char* set, Mii& m, int Mii::*field, float tile_size) {
    bool changed = false;
    float avail = ImGui::GetContentRegionAvail().x;
    int per_row = std::max(1, int(avail / (tile_size + 8)));
    int count = mii_render::choice_count(set);
    ImGui::PushID(set);
    for (int position = 0; position < count; position++) {
        int v = mii_render::choice_value(set, position);
        if (position % per_row) ImGui::SameLine(0, 8);
        char tid[16];
        std::snprintf(tid, sizeof tid, "%d", position);
        auto image = [&](ImVec2 inner) { return icon_picture(set, position, inner, false); };
        if (tile(tid, m.*field == v, ImVec2(tile_size, tile_size), tile_size * 0.1f, image)) {
            m.*field = v;
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

bool swatches(const char* id, int* value, const ImU32* colors, int count, float size) {
    bool changed = false;
    ImGui::PushID(id);
    for (int i = 0; i < count; i++) {
        if (i) ImGui::SameLine(0, 4);
        char sid[8];
        std::snprintf(sid, sizeof sid, "%d", i);
        if (swatch(sid, colors[i], *value == i, size)) {
            *value = i;
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

// The Mii Channel's adjust buttons: one pair per setting, each press one step.
bool adjust(const char* label, int* value, int lo, int hi, const char* less, const char* more) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(170);
    if (ImGui::ArrowButton("less", ImGuiDir_Left) && *value > lo) --*value, changed = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", less);
    ImGui::SameLine(320);
    float frac = hi > lo ? float(*value - lo) / float(hi - lo) : 0;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(110, ImGui::GetFrameHeight()));
    ImDrawList* d = ImGui::GetWindowDrawList();
    float y = p.y + ImGui::GetFrameHeight() / 2;
    d->AddLine(ImVec2(p.x, y), ImVec2(p.x + 110, y), IM_COL32(200, 190, 170, 255), 4);
    d->AddCircleFilled(ImVec2(p.x + 110 * frac, y), 6, kAccent);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", more);
    ImGui::SameLine();
    if (ImGui::ArrowButton("more", ImGuiDir_Right) && *value < hi) ++*value, changed = true;
    ImGui::PopID();
    return changed;
}

// Height and weight, drawn like the picture the player sent: a track, a block handle, and a small and a
// large silhouette at the ends.
bool body_slider(const char* label, int* value, bool weight) {
    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(IM_COL32(176, 108, 20, 255)));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    float width = std::min(ImGui::GetContentRegionAvail().x, 560.0f), height = 110;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("track", ImVec2(width, height));
    ImDrawList* d = ImGui::GetWindowDrawList();
    float left = p.x + 70, right = p.x + width - 70, y = p.y + height * 0.55f;
    bool changed = false;
    if (ImGui::IsItemActive()) {
        float frac = std::clamp((ImGui::GetIO().MousePos.x - left) / (right - left), 0.0f, 1.0f);
        int v = int(std::lround(frac * 127));
        if (v != *value) *value = v, changed = true;
    }
    auto figure = [&](float cx, float scale_h, float scale_w) {
        float base = p.y + height - 8, h = 70 * scale_h;
        d->AddCircleFilled(ImVec2(cx, base - h + 9 * scale_h), 9 * std::max(scale_h, 0.7f), kSilhouette);
        d->AddRectFilled(ImVec2(cx - 9 * scale_w, base - h + 18 * scale_h), ImVec2(cx + 9 * scale_w, base), kSilhouette, 6 * scale_w);
    };
    if (weight) {
        figure(p.x + 35, 1.0f, 0.55f);
        figure(p.x + width - 35, 1.0f, 1.8f);
    } else {
        figure(p.x + 35, 0.6f, 1.0f);
        figure(p.x + width - 35, 1.25f, 1.0f);
    }
    d->AddRectFilled(ImVec2(left, y - 4), ImVec2(right, y + 4), IM_COL32(190, 190, 186, 255), 4);
    float hx = left + (right - left) * (*value / 127.0f);
    d->AddRectFilled(ImVec2(hx - 16, y - 32), ImVec2(hx + 16, y + 32), IM_COL32(196, 222, 226, 255), 3);
    d->AddRect(ImVec2(hx - 16, y - 32), ImVec2(hx + 16, y + 32), IM_COL32(40, 40, 40, 255), 3, 0, 2);
    ImGui::PopID();
    return changed;
}

void draw_background(ImVec2 o, ImVec2 size) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilledMultiColor(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(255, 250, 232, 255), IM_COL32(255, 238, 196, 255),
                               IM_COL32(255, 232, 180, 255), IM_COL32(255, 248, 226, 255));
    // two soft swooshes, like the editor's backdrop
    for (int i = 0; i < 2; i++) {
        ImVec2 a(o.x + size.x * (0.25f + i * 0.2f), o.y + size.y), b(o.x + size.x * (0.5f + i * 0.15f), o.y + size.y * 0.3f);
        ImVec2 c(o.x + size.x * (0.9f - i * 0.1f), o.y - 20);
        d->AddBezierQuadratic(a, b, c, IM_COL32(255, 255, 255, 110), 60.0f - i * 25);
    }
}

// ---- state ----
struct State {
    mii::Database db;
    bool loaded = false;
    std::string message, path;
    int slot = -1;
    Mii edit;
    char name[32] = "", creator[32] = "";
    bool dirty = false;
    int part = kList;
    void load() {
        std::string error;
        loaded = db.load(path, &error);
        message = loaded ? "" : error;
        slot = -1;
    }
    void select(int s) {
        slot = s;
        edit = db.get(s);
        std::snprintf(name, sizeof name, "%s", narrow(edit.name).c_str());
        std::snprintf(creator, sizeof creator, "%s", narrow(edit.creator).c_str());
        dirty = false;
        part = kBody;
    }
    void save() {
        edit.name = widen(name);
        edit.creator = widen(creator);
        std::string why;
        if (!mii::valid(edit, &why)) {
            message = "Can't save: " + why + ".";
            return;
        }
        db.put(slot, edit);
        std::string error;
        message = db.save(path, &error) ? "Saved. The game shows it the next time it starts." : error;
        if (message[0] == 'S') dirty = false;
    }
} g;

void part_panel() {
    Mii& m = g.edit;
    bool changed = false;
    const float big = 74, small = 64;
    switch (g.part) {
    case kBody:
        changed |= body_slider("Height", &m.height, false);
        ImGui::Spacing();
        changed |= body_slider("Weight", &m.weight, true);
        break;
    case kFace:
        changed |= choice_grid("head", m, &Mii::face_shape, big);
        ImGui::SeparatorText("Skin");
        changed |= swatches("skin", &m.skin, kSkin, 6, 40);
        break;
    case kFeature: changed |= choice_grid("face", m, &Mii::face_feature, big); break;
    case kHairPart:
        ImGui::SeparatorText("Color");
        changed |= swatches("hair color", &m.hair_color, kHair, 8, 36);
        ImGui::SameLine(0, 24);
        if (ImGui::Button(m.hair_part ? "Part: right" : "Part: left")) m.hair_part ^= 1, changed = true;
        ImGui::SeparatorText("Style");
        changed |= choice_grid("hair", m, &Mii::hair, small);
        break;
    case kBrows:
        ImGui::SeparatorText("Color");
        changed |= swatches("brow color", &m.brow_color, kHair, 8, 34);
        changed |= adjust("Up and down", &m.brow_y, 3, 18, "up", "down");
        changed |= adjust("Apart", &m.brow_x, 0, 12, "closer", "farther");
        changed |= adjust("Angle", &m.brow_rotation, 0, 11, "", "");
        changed |= adjust("Size", &m.brow_size, 0, 8, "smaller", "bigger");
        ImGui::SeparatorText("Shape");
        changed |= choice_grid("eyebrow", m, &Mii::brow, small);
        break;
    case kEyes:
        ImGui::SeparatorText("Color");
        changed |= swatches("eye color", &m.eye_color, kEye, 6, 34);
        changed |= adjust("Up and down", &m.eye_y, 0, 18, "up", "down");
        changed |= adjust("Apart", &m.eye_x, 0, 12, "closer", "farther");
        changed |= adjust("Angle", &m.eye_rotation, 0, 7, "", "");
        changed |= adjust("Size", &m.eye_size, 0, 7, "smaller", "bigger");
        ImGui::SeparatorText("Shape");
        changed |= choice_grid("eye", m, &Mii::eye, small);
        break;
    case kGlassesPart:
        ImGui::SeparatorText("Color");
        changed |= swatches("glasses color", &m.glasses_color, kGlasses, 6, 34);
        changed |= adjust("Up and down", &m.glasses_y, 0, 20, "up", "down");
        changed |= adjust("Size", &m.glasses_size, 0, 7, "smaller", "bigger");
        ImGui::SeparatorText("Frames");
        changed |= choice_grid("glasses", m, &Mii::glasses, big);
        break;
    case kNose:
        changed |= adjust("Up and down", &m.nose_y, 0, 18, "up", "down");
        changed |= adjust("Size", &m.nose_size, 0, 8, "smaller", "bigger");
        ImGui::SeparatorText("Shape");
        changed |= choice_grid("nose", m, &Mii::nose, big);
        break;
    case kMouth:
        ImGui::SeparatorText("Lip color");
        changed |= swatches("lip", &m.mouth_color, kLip, 3, 34);
        changed |= adjust("Up and down", &m.mouth_y, 0, 18, "up", "down");
        changed |= adjust("Size", &m.mouth_size, 0, 8, "smaller", "bigger");
        ImGui::SeparatorText("Shape");
        changed |= choice_grid("mouth", m, &Mii::mouth, small);
        break;
    case kBeard:
        ImGui::SeparatorText("Color");
        changed |= swatches("beard color", &m.facial_hair_color, kHair, 8, 34);
        changed |= adjust("Mustache up and down", &m.mustache_y, 0, 16, "up", "down");
        changed |= adjust("Mustache size", &m.mustache_size, 0, 8, "smaller", "bigger");
        ImGui::SeparatorText("Mustache");
        changed |= choice_grid("mustache", m, &Mii::mustache, big);
        ImGui::SeparatorText("Beard");
        changed |= choice_grid("beard", m, &Mii::beard, big);
        break;
    case kMole:
        {
            bool on = m.mole != 0;
            if (ImGui::Checkbox("Mole", &on)) m.mole = on ? 1 : 0, changed = true;
        }
        changed |= adjust("Up and down", &m.mole_y, 0, 30, "up", "down");
        changed |= adjust("Sideways", &m.mole_x, 0, 16, "left", "right");
        changed |= adjust("Size", &m.mole_size, 0, 8, "smaller", "bigger");
        break;
    case kProfile:
        changed |= ImGui::InputText("Name", g.name, 21);
        changed |= ImGui::InputText("Made by", g.creator, 21);
        changed |= ImGui::Combo("Gender", &m.female, "Male\0Female\0");
        changed |= ImGui::SliderInt("Birth month (0 = none)", &m.birth_month, 0, 12);
        changed |= ImGui::SliderInt("Birth day (0 = none)", &m.birth_day, 0, 31);
        ImGui::SeparatorText("Favorite color");
        changed |= swatches("favorite", &m.favorite_color, kFavorite, 12, 36);
        ImGui::TextDisabled("%s", kFavoriteNames[m.favorite_color % 12]);
        break;
    }
    if (changed) g.dirty = true;
}

// ---- .mii files: one Mii, the 74-byte record the Wii keeps (the format Dolphin and Mii websites use) ----

std::mutex g_dialog_lock;
std::vector<std::string> g_import_files;
std::string g_export_file;

void SDLCALL on_import(void*, const char* const* files, int) {
    std::lock_guard<std::mutex> lock(g_dialog_lock);
    for (int i = 0; files && files[i]; i++) g_import_files.push_back(files[i]);
}

void SDLCALL on_export(void*, const char* const* files, int) {
    std::lock_guard<std::mutex> lock(g_dialog_lock);
    if (files && files[0]) g_export_file = files[0];
}

const SDL_DialogFileFilter kMiiFilter[] = {{"Mii files", "mii"}, {"All files", "*"}};

void ask_import() {
    SDL_ShowOpenFileDialog(on_import, nullptr, SDL_GetRenderWindow(g_renderer), kMiiFilter, 2, nullptr, true);
}

void ask_export() {
    std::string name = narrow(g.edit.name);
    for (char& c : name)
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) c = '_';
    std::string suggested = (name.empty() ? std::string("Mii") : name) + ".mii";
    SDL_ShowSaveFileDialog(on_export, nullptr, SDL_GetRenderWindow(g_renderer), kMiiFilter, 1, suggested.c_str());
}

// Answers from the file dialogs arrive on another thread; they're acted on here, in the next frame.
void finish_file_dialogs() {
    std::vector<std::string> imports;
    std::string export_to;
    {
        std::lock_guard<std::mutex> lock(g_dialog_lock);
        imports.swap(g_import_files);
        export_to.swap(g_export_file);
    }
    if (!export_to.empty() && g.slot >= 0) {
        if (export_to.size() < 4 || export_to.substr(export_to.size() - 4) != ".mii") export_to += ".mii";
        auto record = mii::pack(g.edit);
        std::ofstream f(std::filesystem::u8path(export_to), std::ios::binary);
        f.write(reinterpret_cast<const char*>(record.data()), std::streamsize(record.size()));
        g.message = f ? "Saved " + narrow(g.edit.name) + " to " + export_to : "Couldn't write " + export_to;
    }
    if (imports.empty()) return;
    int added = 0, skipped = 0, full = 0;
    std::string problems;
    for (const std::string& path : imports) {
        std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        // 74 bytes; some tools add a 2-byte checksum. 3DS and Wii U Miis (92 or 96 bytes) are another format.
        if (bytes.size() != mii::kRecordSize && bytes.size() != mii::kRecordSize + 2) {
            problems += "\n" + path + ": not a Wii Mii (" + std::to_string(bytes.size()) + " bytes, a Wii Mii is 74)";
            continue;
        }
        Mii m = mii::unpack(bytes.data());
        std::string why;
        if (!mii::slot_used(bytes.data()) || !mii::valid(m, &why)) {
            problems += "\n" + path + ": not a usable Mii" + (why.empty() ? "" : " (" + why + ")");
            continue;
        }
        bool there = false;
        for (int slot : g.db.used_slots())
            if (g.db.get(slot).mii_id == m.mii_id) there = true;
        if (there) {
            skipped++;
            continue;
        }
        int slot = g.db.free_slot();
        if (slot < 0) {
            full++;
            continue;
        }
        g.db.put(slot, m);
        added++;
    }
    std::string error;
    if (added && !g.db.save(g.path, &error)) {
        g.message = "Couldn't save the Mii database: " + error;
        return;
    }
    g.message = "Added " + std::to_string(added) + " Mii" + (added == 1 ? "" : "s") + ".";
    if (skipped) g.message += " " + std::to_string(skipped) + " already here.";
    if (full) g.message += " " + std::to_string(full) + " didn't fit: all 100 places are taken.";
    g.message += problems;
}

void list_panel() {
    ImGui::TextWrapped("Pick a Mii to change it, or make a new one. Saved Miis appear in the game's Mii selection.");
    ImGui::Spacing();
    float size = 96;
    float avail = ImGui::GetContentRegionAvail().x;
    int per_row = std::max(1, int(avail / (size + 10))), n = 0;
    for (int s : g.db.used_slots()) {
        if (n++ % per_row) ImGui::SameLine(0, 10);
        Mii m = g.db.get(s);
        ImGui::BeginGroup();
        char id[16];
        std::snprintf(id, sizeof id, "mii%d", s);
        auto image = [&](ImVec2 inner) { return mii_picture(m, mii_render::View::Head, inner, false); };
        if (tile(id, g.slot == s, ImVec2(size, size), 4, image)) g.select(s);
        ImGui::TextUnformatted(narrow(m.name).c_str());
        ImGui::EndGroup();
    }
    if (n % per_row) ImGui::SameLine(0, 10);
    ImGui::BeginGroup();
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("new", ImVec2(size, size))) {
        int s = g.db.free_slot();
        if (s < 0) {
            g.message = "All 100 places are taken.";
        } else {
            Mii m;
            m.mii_id = mii::new_mii_id();
            m.system_id = 0x52A1C0DEu;  // this build's console
            m.name = u"New Mii";
            g.db.put(s, m);
            g.select(s);
            g.dirty = true;
        }
    }
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRect(p, ImVec2(p.x + size, p.y + size), kAccent, 8, 0, 2.5f);
    d->AddLine(ImVec2(p.x + size / 2, p.y + 24), ImVec2(p.x + size / 2, p.y + size - 24), kAccent, 5);
    d->AddLine(ImVec2(p.x + 24, p.y + size / 2), ImVec2(p.x + size - 24, p.y + size / 2), kAccent, 5);
    ImGui::TextUnformatted("New Mii");
    ImGui::EndGroup();
    ImGui::Spacing();
    if (ImGui::Button("Import Miis...")) ask_import();
    if (g.slot >= 0) {
        ImGui::SameLine();
        if (ImGui::Button("Export this Mii...")) ask_export();
        ImGui::SameLine();
        if (ImGui::Button("Delete this Mii")) ImGui::OpenPopup("delete?");
        if (ImGui::BeginPopupModal("delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete %s? This can't be undone.", g.name);
            if (ImGui::Button("Delete", ImVec2(120, 0))) {
                g.db.erase(g.slot);
                std::string error;
                g.message = g.db.save(g.path, &error) ? "Deleted." : error;
                g.slot = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextDisabled("Mii pictures and icons: My Avatar Editor, (c) 2009 Trevor McCauley, MIT licence");
    ImGui::TextDisabled("(licenses/MyAvatarEditor.txt next to the launcher).");
}

// The face pages zoom the view to the face, as My Avatar Editor's do.
bool zooms_to_face(int part) { return part != kList && part != kBody && part != kProfile; }

}  // namespace

void mii_maker_tab(const std::string& db_path) {
    if (g.path != db_path) {
        g.path = db_path;
        g.loaded = false;
        g.message.clear();
    }
    if (!g.loaded && g.message.empty()) g.load();
    if (g.loaded) finish_file_dialogs();
    new_picture_frame();
    ImVec2 o = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
    draw_background(o, size);
    ImDrawList* d = ImGui::GetWindowDrawList();
    // title
    d->AddCircleFilled(ImVec2(o.x + 26, o.y + 24), 14, kAccent);
    d->AddCircleFilled(ImVec2(o.x + 26, o.y + 20), 6, kCream);
    d->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.35f, ImVec2(o.x + 48, o.y + 8), kInk, "Mii Maker");
    if (!g.loaded) {
        ImGui::SetCursorScreenPos(ImVec2(o.x + 20, o.y + 60));
        ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.2f, 1), "%s", g.message.c_str());
        return;
    }
    const float toolbar_h = 64, left_w = std::min(320.0f, size.x * 0.32f);
    // the Mii: standing, or its face up close
    if (g.slot >= 0) {
        ImVec2 at(o.x + 10, o.y + 50), box(left_w - 20, size.y - toolbar_h - 90);
        if (zooms_to_face(g.part)) {
            float side = std::min(box.x, box.y);
            at = ImVec2(at.x + (box.x - side) / 2, at.y + (box.y - side) * 0.3f);
            box = ImVec2(side, side);
        }
        ImTextureID t = mii_picture(g.edit, zooms_to_face(g.part) ? mii_render::View::Head : mii_render::View::Body, box, true);
        if (t) d->AddImage(t, at, ImVec2(at.x + box.x, at.y + box.y));
        d->AddText(ImVec2(o.x + 20, o.y + size.y - toolbar_h - 34), kInk, g.name);
        if (g.dirty) d->AddText(ImVec2(o.x + 20, o.y + size.y - toolbar_h - 16), IM_COL32(176, 108, 20, 255), "Not saved yet");
    } else {
        d->AddText(ImVec2(o.x + 20, o.y + 80), kInk, "No Mii picked");
    }
    // the part's choices
    ImGui::SetCursorScreenPos(ImVec2(o.x + left_w, o.y + 46));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("choices", ImVec2(size.x - left_w - 10, size.y - toolbar_h - 56));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kInk));
    if (!g.message.empty()) ImGui::TextColored(ImVec4(0.6f, 0.36f, 0.05f, 1), "%s", g.message.c_str());
    if (g.part == kList || g.slot < 0) list_panel();
    else part_panel();
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    // the toolbar
    float y = o.y + size.y - toolbar_h;
    d->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + size.x, o.y + size.y), IM_COL32(222, 222, 220, 255));
    d->AddLine(ImVec2(o.x, y), ImVec2(o.x + size.x, y), IM_COL32(170, 170, 166, 255), 2);
    float bw = std::min(64.0f, (size.x - 20) / kParts);
    for (int p = 0; p < kParts; p++) {
        ImVec2 b(o.x + 10 + p * bw, y + 6);
        ImGui::SetCursorScreenPos(b);
        char id[16];
        std::snprintf(id, sizeof id, "part%d", p);
        bool enabled = p == kList || g.slot >= 0;
        if (ImGui::InvisibleButton(id, ImVec2(bw - 4, toolbar_h - 12)) && enabled) {
            if (p == kSave) g.save();
            else g.part = p;
        }
        bool hover = ImGui::IsItemHovered();
        if (hover) ImGui::SetTooltip("%s", kPartNames[p]);
        bool on = g.part == p && p != kSave;
        ImU32 back = on ? IM_COL32(255, 200, 40, 255) : hover && enabled ? IM_COL32(250, 250, 248, 255) : IM_COL32(238, 238, 236, 255);
        d->AddRectFilled(b, ImVec2(b.x + bw - 4, b.y + toolbar_h - 12), back, 6);
        d->AddRect(b, ImVec2(b.x + bw - 4, b.y + toolbar_h - 12), IM_COL32(150, 150, 146, 255), 6, 0, 1.5f);
        draw_icon(d, p, ImVec2(b.x + (bw - 4) / 2, b.y + (toolbar_h - 12) / 2), (toolbar_h - 12) * 0.32f,
                  enabled ? IM_COL32(40, 40, 40, 255) : IM_COL32(170, 170, 170, 255));
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + size.y));
}

void mii_maker_set_renderer(SDL_Renderer* renderer) { g_renderer = renderer; }

void mii_maker_release() {
    for (auto& [key, pic] : g_pictures) SDL_DestroyTexture(pic.texture);
    g_pictures.clear();
}

bool mii_maker_busy() { return g_waiting > 0; }

int mii_maker_pages() { return kParts - 1; }  // every part but Save

std::string mii_maker_import_test(const std::string& db_path, const std::vector<std::string>& files) {
    g.path = db_path;
    g.message.clear();
    g.load();
    {
        std::lock_guard<std::mutex> lock(g_dialog_lock);
        g_import_files = files;
    }
    finish_file_dialogs();
    return g.message;
}

void mii_maker_show(const std::string& db_path, int page) {
    if (g.path != db_path || !g.loaded) {
        g.path = db_path;
        g.message.clear();
        g.load();
    }
    auto used = g.db.used_slots();
    if (page == 0 || used.empty()) {
        g.part = kList;
        return;
    }
    if (g.slot != used[0]) g.select(used[0]);
    g.part = page + 1;  // skip Save and the list
    if (g.part >= kParts) g.part = kProfile;
}
