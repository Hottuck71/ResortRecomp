// The Wii's Mii database (/shared2/menu/FaceLib/RFL_DB.dat), read and written for the launcher's Mii Maker.
//
// Layout, checked against this game's own code (docs/12-launcher.md):
//   0x00000  "RNOD"
//   0x00004  100 Miis, 0x4A bytes each
//   0x01CEC  a flags word; the game's format routine sets its top bit
//   0x01D00  "RNHD", two halfwords of -1, then 10,000 entries of 12 bytes ending in 0x7FFF, 0x7FFF
//   0x1F1DE  CRC-16 (polynomial 0x1021, start 0) of everything before it, big-endian
// The file is 0x1F1E0 bytes, the size the game allocates for it.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mii {

constexpr size_t kDbSize = 0x1F1E0;
constexpr size_t kSlots = 100;
constexpr size_t kRecordSize = 0x4A;

// One Mii, unpacked. Ranges are the Mii Channel's, which the game checks (RFLiCheckValidInfo).
struct Mii {
    std::u16string name = u"Player", creator = u"";
    int female = 0, birth_month = 0, birth_day = 0, favorite_color = 0, favorite = 0;
    int height = 64, weight = 64;
    uint32_t mii_id = 0, system_id = 0;
    int face_shape = 0, skin = 0, face_feature = 0;
    int hair = 33, hair_color = 1, hair_part = 0;
    int brow = 6, brow_rotation = 6, brow_color = 1, brow_size = 4, brow_y = 10, brow_x = 2;
    int eye = 2, eye_rotation = 4, eye_y = 12, eye_color = 0, eye_size = 4, eye_x = 2;
    int nose = 1, nose_size = 4, nose_y = 9;
    int mouth = 23, mouth_color = 0, mouth_size = 4, mouth_y = 13;
    int glasses = 0, glasses_color = 0, glasses_size = 4, glasses_y = 10;
    int mustache = 0, beard = 0, facial_hair_color = 0, mustache_size = 4, mustache_y = 10;
    int mole = 0, mole_size = 4, mole_y = 20, mole_x = 2;
    // Bits the editor doesn't show, kept as read so an edited Mii loses nothing.
    uint16_t face_low_bits = 0, hair_low_bits = 0;
};

struct Range {
    const char* label;
    int Mii::*field;
    int lo, hi;
};
const std::vector<Range>& ranges();  // every number field with its limits, for the editor and for checking

std::array<uint8_t, kRecordSize> pack(const Mii& m);
Mii unpack(const uint8_t* record);
bool valid(const Mii& m, std::string* why = nullptr);
bool slot_used(const uint8_t* record);  // a Mii ID or system ID that isn't zero, as the game checks

struct Database {
    std::vector<uint8_t> bytes;
    bool load(const std::string& path, std::string* error);  // creates an empty database if there's no file
    bool save(const std::string& path, std::string* error);  // writes the CRC, keeps a .bak of the old file
    void format();                                           // what the game's format routine writes
    std::vector<int> used_slots() const;
    Mii get(int slot) const;
    void put(int slot, const Mii& m);
    void erase(int slot);
    int free_slot() const;  // -1 when all 100 are taken
};

uint16_t crc16(const uint8_t* data, size_t size);       // the stored CRC
uint16_t game_crc(const uint8_t* data, size_t size);    // the game's own routine: 0 over the whole file when right
uint32_t new_mii_id();                                   // a normal Mii's ID, from the time now
int self_test();                                         // returns failures; prints what it checks
}  // namespace mii
