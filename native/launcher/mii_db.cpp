// See mii_db.h. Field layout from wiibrew.org/wiki/Mii_Data, cross-checked against this game's validity check.
#include "mii_db.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace mii {
namespace {

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void put16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint32_t bits(uint32_t word, int high, int width) { return (word >> (high - width + 1)) & ((1u << width) - 1u); }
uint32_t field(int value, int high, int width) { return (uint32_t(value) & ((1u << width) - 1u)) << (high - width + 1); }

std::u16string read_name(const uint8_t* p) {
    std::u16string s;
    for (int i = 0; i < 10; i++) {
        char16_t c = char16_t(be16(p + i * 2));
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

void write_name(uint8_t* p, const std::u16string& s) {
    for (int i = 0; i < 10; i++) put16(p + i * 2, i < int(s.size()) ? s[i] : 0);
}

constexpr size_t kMiiStart = 4, kFlags = 0x1CEC, kHidden = 0x1D00, kCrc = 0x1F1DE;
}  // namespace

const std::vector<Range>& ranges() {
    static const std::vector<Range> r = {
        {"Height", &Mii::height, 0, 127},          {"Weight", &Mii::weight, 0, 127},
        {"Favorite color", &Mii::favorite_color, 0, 11}, {"Birth month", &Mii::birth_month, 0, 12},
        {"Birth day", &Mii::birth_day, 0, 31},
        {"Face shape", &Mii::face_shape, 0, 7},    {"Skin", &Mii::skin, 0, 5},
        {"Face feature", &Mii::face_feature, 0, 11},
        {"Hair", &Mii::hair, 0, 71},               {"Hair color", &Mii::hair_color, 0, 7},
        {"Hair part", &Mii::hair_part, 0, 1},
        {"Eyebrows", &Mii::brow, 0, 23},           {"Eyebrow angle", &Mii::brow_rotation, 0, 11},
        {"Eyebrow color", &Mii::brow_color, 0, 7}, {"Eyebrow size", &Mii::brow_size, 0, 8},
        {"Eyebrow height", &Mii::brow_y, 3, 18},   {"Eyebrow spacing", &Mii::brow_x, 0, 12},
        {"Eyes", &Mii::eye, 0, 47},                {"Eye angle", &Mii::eye_rotation, 0, 7},
        {"Eye height", &Mii::eye_y, 0, 18},        {"Eye color", &Mii::eye_color, 0, 5},
        {"Eye size", &Mii::eye_size, 0, 7},        {"Eye spacing", &Mii::eye_x, 0, 12},
        {"Nose", &Mii::nose, 0, 11},               {"Nose size", &Mii::nose_size, 0, 8},
        {"Nose height", &Mii::nose_y, 0, 18},
        {"Mouth", &Mii::mouth, 0, 23},             {"Lip color", &Mii::mouth_color, 0, 2},
        {"Mouth size", &Mii::mouth_size, 0, 8},    {"Mouth height", &Mii::mouth_y, 0, 18},
        {"Glasses", &Mii::glasses, 0, 8},          {"Glasses color", &Mii::glasses_color, 0, 5},
        {"Glasses size", &Mii::glasses_size, 0, 7}, {"Glasses height", &Mii::glasses_y, 0, 20},
        {"Mustache", &Mii::mustache, 0, 3},        {"Beard", &Mii::beard, 0, 3},
        {"Facial hair color", &Mii::facial_hair_color, 0, 7},
        {"Mustache size", &Mii::mustache_size, 0, 8}, {"Mustache height", &Mii::mustache_y, 0, 16},
        {"Mole", &Mii::mole, 0, 1},                {"Mole size", &Mii::mole_size, 0, 8},
        {"Mole height", &Mii::mole_y, 0, 30},      {"Mole sideways", &Mii::mole_x, 0, 16},
    };
    return r;
}

std::array<uint8_t, kRecordSize> pack(const Mii& m) {
    std::array<uint8_t, kRecordSize> r{};
    uint8_t* p = r.data();
    put16(p + 0x00, field(m.female, 14, 1) | field(m.birth_month, 13, 4) | field(m.birth_day, 9, 5) |
                        field(m.favorite_color, 4, 4) | field(m.favorite, 0, 1));
    write_name(p + 0x02, m.name);
    p[0x16] = uint8_t(m.height);
    p[0x17] = uint8_t(m.weight);
    put32(p + 0x18, m.mii_id);
    put32(p + 0x1C, m.system_id);
    put16(p + 0x20, field(m.face_shape, 15, 3) | field(m.skin, 12, 3) | field(m.face_feature, 9, 4) |
                        (m.face_low_bits & 0x3Fu));
    put16(p + 0x22, field(m.hair, 15, 7) | field(m.hair_color, 8, 3) | field(m.hair_part, 5, 1) |
                        (m.hair_low_bits & 0x1Fu));
    put32(p + 0x24, field(m.brow, 31, 5) | field(m.brow_rotation, 26, 5) | field(m.brow_color, 15, 3) |
                        field(m.brow_size, 12, 4) | field(m.brow_y, 8, 5) | field(m.brow_x, 3, 4));
    put32(p + 0x28, field(m.eye, 31, 6) | field(m.eye_rotation, 25, 5) | field(m.eye_y, 20, 5) |
                        field(m.eye_color, 15, 3) | field(m.eye_size, 12, 4) | field(m.eye_x, 8, 4));
    put16(p + 0x2C, field(m.nose, 15, 4) | field(m.nose_size, 11, 4) | field(m.nose_y, 7, 5));
    put16(p + 0x2E, field(m.mouth, 15, 5) | field(m.mouth_color, 10, 2) | field(m.mouth_size, 8, 4) |
                        field(m.mouth_y, 4, 5));
    put16(p + 0x30, field(m.glasses, 15, 4) | field(m.glasses_color, 11, 3) | field(m.glasses_size, 7, 3) |
                        field(m.glasses_y, 4, 5));
    put16(p + 0x32, field(m.mustache, 15, 2) | field(m.beard, 13, 2) | field(m.facial_hair_color, 11, 3) |
                        field(m.mustache_size, 8, 4) | field(m.mustache_y, 4, 5));
    put16(p + 0x34, field(m.mole, 15, 1) | field(m.mole_size, 14, 4) | field(m.mole_y, 10, 5) |
                        field(m.mole_x, 5, 5));
    write_name(p + 0x36, m.creator);
    return r;
}

Mii unpack(const uint8_t* p) {
    Mii m;
    uint32_t w = be16(p);
    m.female = bits(w, 14, 1);
    m.birth_month = bits(w, 13, 4);
    m.birth_day = bits(w, 9, 5);
    m.favorite_color = bits(w, 4, 4);
    m.favorite = bits(w, 0, 1);
    m.name = read_name(p + 0x02);
    m.height = p[0x16];
    m.weight = p[0x17];
    m.mii_id = be32(p + 0x18);
    m.system_id = be32(p + 0x1C);
    w = be16(p + 0x20);
    m.face_shape = bits(w, 15, 3);
    m.skin = bits(w, 12, 3);
    m.face_feature = bits(w, 9, 4);
    m.face_low_bits = uint16_t(w & 0x3F);
    w = be16(p + 0x22);
    m.hair = bits(w, 15, 7);
    m.hair_color = bits(w, 8, 3);
    m.hair_part = bits(w, 5, 1);
    m.hair_low_bits = uint16_t(w & 0x1F);
    w = be32(p + 0x24);
    m.brow = bits(w, 31, 5);
    m.brow_rotation = bits(w, 26, 5);
    m.brow_color = bits(w, 15, 3);
    m.brow_size = bits(w, 12, 4);
    m.brow_y = bits(w, 8, 5);
    m.brow_x = bits(w, 3, 4);
    w = be32(p + 0x28);
    m.eye = bits(w, 31, 6);
    m.eye_rotation = bits(w, 25, 5);
    m.eye_y = bits(w, 20, 5);
    m.eye_color = bits(w, 15, 3);
    m.eye_size = bits(w, 12, 4);
    m.eye_x = bits(w, 8, 4);
    w = be16(p + 0x2C);
    m.nose = bits(w, 15, 4);
    m.nose_size = bits(w, 11, 4);
    m.nose_y = bits(w, 7, 5);
    w = be16(p + 0x2E);
    m.mouth = bits(w, 15, 5);
    m.mouth_color = bits(w, 10, 2);
    m.mouth_size = bits(w, 8, 4);
    m.mouth_y = bits(w, 4, 5);
    w = be16(p + 0x30);
    m.glasses = bits(w, 15, 4);
    m.glasses_color = bits(w, 11, 3);
    m.glasses_size = bits(w, 7, 3);
    m.glasses_y = bits(w, 4, 5);
    w = be16(p + 0x32);
    m.mustache = bits(w, 15, 2);
    m.beard = bits(w, 13, 2);
    m.facial_hair_color = bits(w, 11, 3);
    m.mustache_size = bits(w, 8, 4);
    m.mustache_y = bits(w, 4, 5);
    w = be16(p + 0x34);
    m.mole = bits(w, 15, 1);
    m.mole_size = bits(w, 14, 4);
    m.mole_y = bits(w, 10, 5);
    m.mole_x = bits(w, 5, 5);
    m.creator = read_name(p + 0x36);
    return m;
}

bool valid(const Mii& m, std::string* why) {
    for (const Range& r : ranges()) {
        int v = m.*r.field;
        if (v < r.lo || v > r.hi) {
            if (why) *why = std::string(r.label) + " is out of range";
            return false;
        }
    }
    if (m.name.empty()) {
        if (why) *why = "the Mii needs a name";
        return false;
    }
    if (m.mii_id == 0 && m.system_id == 0) {
        if (why) *why = "the Mii has no ID";
        return false;
    }
    return true;
}

bool slot_used(const uint8_t* record) { return be32(record + 0x18) != 0 || be32(record + 0x1C) != 0; }

uint16_t crc16(const uint8_t* data, size_t size) {
    uint16_t crc = 0;
    for (size_t i = 0; i < size; i++) {
        crc ^= uint16_t(data[i] << 8);
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

// The routine at 0x801089C0, bit for bit: the message is shifted in at the bottom, so running it over data
// followed by its correct CRC gives 0.
uint16_t game_crc(const uint8_t* data, size_t size) {
    uint32_t crc = 0;
    for (size_t i = 0; i < size; i++) {
        uint32_t byte = data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (((crc << 1) & 0xFFFE) ^ 0x1021) : ((crc << 1) & 0xFFFE);
            if (byte & 0x80) crc ^= 1;
            byte = (byte << 1) & 0xFF;
        }
    }
    return uint16_t(crc);
}

uint32_t new_mii_id() {
    // A normal Mii's ID has its top bit set; the rest counts four-second steps since 2006-01-01, as the
    // Mii Channel's do. Two Miis made in the same four seconds get different IDs from the counter.
    static uint32_t extra = 0;
    std::time_t now = std::time(nullptr);
    std::tm base{};
    base.tm_year = 106;
    base.tm_mday = 1;
    std::time_t start = std::mktime(&base);
    uint32_t steps = uint32_t((now - start) / 4) + extra++;
    return 0x80000000u | (steps & 0x1FFFFFFFu);
}

void Database::format() {
    bytes.assign(kDbSize, 0);
    std::memcpy(bytes.data(), "RNOD", 4);
    put32(bytes.data() + kFlags, 0x80000000u);
    std::memcpy(bytes.data() + kHidden, "RNHD", 4);
    put16(bytes.data() + kHidden + 4, 0xFFFF);
    put16(bytes.data() + kHidden + 6, 0xFFFF);
    for (size_t i = 0; i < 10000; i++) {
        uint8_t* e = bytes.data() + kHidden + 8 + i * 12;
        put16(e + 8, 0x7FFF);
        put16(e + 10, 0x7FFF);
    }
}

bool Database::load(const std::string& path, std::string* error) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        format();
        return true;
    }
    bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (bytes.size() != kDbSize || std::memcmp(bytes.data(), "RNOD", 4) != 0) {
        if (error) *error = "the Mii database isn't in the expected format";
        return false;
    }
    return true;
}

bool Database::save(const std::string& path, std::string* error) {
    put16(bytes.data() + kCrc, crc16(bytes.data(), kCrc));
    namespace fs = std::filesystem;
    fs::path p = fs::u8path(path);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (fs::exists(p, ec)) fs::copy_file(p, fs::path(p).concat(".bak"), fs::copy_options::overwrite_existing, ec);
    fs::path temp = fs::path(p).concat(".new");
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()))) {
            if (error) *error = "couldn't write the Mii database";
            return false;
        }
    }
    fs::rename(temp, p, ec);
    if (ec) {
        if (error) *error = "couldn't replace the Mii database: " + ec.message();
        return false;
    }
    return true;
}

std::vector<int> Database::used_slots() const {
    std::vector<int> out;
    for (size_t i = 0; i < kSlots; i++)
        if (slot_used(bytes.data() + kMiiStart + i * kRecordSize)) out.push_back(int(i));
    return out;
}

Mii Database::get(int slot) const { return unpack(bytes.data() + kMiiStart + size_t(slot) * kRecordSize); }

void Database::put(int slot, const Mii& m) {
    auto r = pack(m);
    std::memcpy(bytes.data() + kMiiStart + size_t(slot) * kRecordSize, r.data(), kRecordSize);
}

void Database::erase(int slot) { std::memset(bytes.data() + kMiiStart + size_t(slot) * kRecordSize, 0, kRecordSize); }

int Database::free_slot() const {
    for (size_t i = 0; i < kSlots; i++)
        if (!slot_used(bytes.data() + kMiiStart + i * kRecordSize)) return int(i);
    return -1;
}

int self_test() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) failures++;
    };
    // Every field survives a round trip, at both ends of its range.
    for (int end = 0; end < 2; end++) {
        Mii m;
        m.name = u"Testname12";
        m.creator = u"Maker";
        m.mii_id = 0x8ABCDEF1u;
        m.system_id = 0x12345678u;
        m.female = end;
        for (const Range& r : ranges()) m.*r.field = end ? r.hi : r.lo;
        Mii back = unpack(pack(m).data());
        bool same = back.name == m.name && back.creator == m.creator && back.mii_id == m.mii_id &&
                    back.system_id == m.system_id && back.female == m.female;
        for (const Range& r : ranges()) same = same && back.*r.field == m.*r.field;
        check(same, end ? "every field round-trips at its highest value" : "every field round-trips at its lowest value");
    }
    // The two CRCs agree the way the game uses them: its routine over a record with the stored CRC gives 0.
    uint8_t rec[76] = {};
    auto packed = pack(Mii{});
    std::memcpy(rec, packed.data(), 74);
    uint16_t stored = game_crc(rec, 76);  // the game computes it with the CRC field zeroed
    check(stored == crc16(rec, 74), "the game's routine over a zeroed CRC field equals CRC-16/XMODEM");
    put16(rec + 74, stored);
    check(game_crc(rec, 76) == 0, "the game's routine over data and stored CRC gives 0");
    // A formatted database matches the game's format routine and checks out.
    Database db;
    db.format();
    check(db.bytes.size() == kDbSize && std::memcmp(db.bytes.data(), "RNOD", 4) == 0 &&
              std::memcmp(db.bytes.data() + kHidden, "RNHD", 4) == 0 && be32(db.bytes.data() + kFlags) == 0x80000000u &&
              be16(db.bytes.data() + kHidden + 8 + 9999 * 12 + 10) == 0x7FFF,
          "a new database has the game's layout");
    check(db.used_slots().empty() && db.free_slot() == 0, "a new database has no Miis");
    Mii m;
    m.name = u"Ripley";
    m.mii_id = new_mii_id();
    m.system_id = 0x52A1C0DEu;
    check(valid(m), "a default Mii is valid");
    db.put(db.free_slot(), m);
    put16(db.bytes.data() + kCrc, crc16(db.bytes.data(), kCrc));
    check(game_crc(db.bytes.data(), kDbSize) == 0, "the database CRC checks out with the game's routine");
    check(db.used_slots().size() == 1 && db.get(0).name == u"Ripley", "the Mii is in slot 0");
    db.erase(0);
    check(db.used_slots().empty(), "erasing frees the slot");
    Mii bad = m;
    bad.brow_y = 2;
    check(!valid(bad), "an eyebrow height of 2 is refused, as the game refuses it");
    std::printf("Mii database self-test: %d failures\n", failures);
    return failures;
}
}  // namespace mii
