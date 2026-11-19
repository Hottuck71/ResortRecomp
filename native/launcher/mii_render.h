// Draws a Mii the way My Avatar Editor does, from its vector art (mii_art.cpp). See mii_render.cpp.
#pragma once
#include <cstdint>
#include <vector>

#include "mii_db.h"

namespace mii_render {

// Premultiplied RGBA, 8 bits a channel, row after row.
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// Body: the whole Mii, at one scale for every height and weight, feet at the bottom. Head: the head, centred on
// the face, at one scale for every face.
enum class View { Body, Head };

// Draws the Mii into an image of this size, on a transparent background.
Image draw(const mii::Mii& m, int width, int height, View view);
bool art_ok(); // the art loaded

// The editor's picker icons: a frame per choice, in the order the Wii shows them. Sets: "head" (face shape),
// "face" (face details), "hair", "eyebrow", "eye", "nose", "mouth", "glasses", "mustache", "beard", "mole".
// Every icon of a set is drawn at the same scale.
Image icon(const char* set, int position, int width, int height);
int choice_count(const char* set);
int choice_value(const char* set, int position); // the Mii's value for the choice at this place in the grid
int choice_position(const char* set, int value);

int self_test(); // returns failures; prints what it checks

}  // namespace mii_render
