// Portable XMA2 packet parser and WMA Pro frame decoder.
//
// Canonical Huffman / run-level tables and decoding structure follow FFmpeg's libavcodec/wmaprodec.c and
// libavcodec/wmaprodata.h (LGPL-2.1-or-later, used under GPL-3.0-or-later), specialized for Xbox XMA2 streams
// (1–2 channels, 512-sample frames, 128/256/512 subframes, 2048-byte packets).
#include "xma2_decoder.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace leonida::xma2 {
namespace {

constexpr std::uint32_t kLengthBits = 15;
constexpr double kPi = 3.14159265358979323846264338327950288;

struct SymLen {
    std::uint16_t sym;
    std::uint8_t len;
};

// Critical band cutoff frequencies (Hz) and canonical VLC tables from FFmpeg libavcodec/wmaprodata.h.
constexpr std::uint16_t kCriticalFreq[28] = {
    100,   200,   300,   400,   510,   630,   770,   920,   1080,  1270,  1480,  1720,  2000,  2320,
    2700,  3150,  3700,  4400,  5300,  6400,  7700,  9500,  12000, 15500, 20675, 28575, 41375, 63875,
};

constexpr SymLen kScaleTable[121] = {
    {58, 5},  {64, 6},  {66, 7},  {65, 7},  {62, 5},  {63, 6},  {68, 9},  {69, 10}, {54, 15}, {19, 19}, {20, 19},
    {21, 19}, {22, 19}, {23, 19}, {24, 19}, {25, 19}, {26, 19}, {27, 19}, {28, 19}, {29, 19}, {30, 19}, {31, 19},
    {32, 19}, {33, 19}, {34, 19}, {17, 19}, {36, 19}, {37, 19}, {38, 19}, {39, 19}, {40, 19}, {41, 19}, {42, 19},
    {43, 19}, {44, 19}, {45, 19}, {46, 19}, {47, 19}, {48, 19}, {49, 19}, {50, 19}, {51, 19}, {52, 19}, {15, 19},
    {16, 19}, {14, 19}, {13, 19}, {12, 19}, {11, 19}, {10, 19}, {0, 19},  {9, 19},  {8, 19},  {7, 19},  {6, 19},
    {5, 19},  {4, 19},  {55, 13}, {70, 13}, {3, 19},  {2, 19},  {1, 19},  {35, 19}, {71, 19}, {72, 19}, {73, 19},
    {74, 19}, {75, 19}, {76, 19}, {77, 19}, {78, 19}, {79, 19}, {80, 19}, {81, 19}, {82, 19}, {83, 19}, {84, 19},
    {85, 19}, {86, 19}, {87, 19}, {88, 19}, {89, 19}, {90, 19}, {91, 19}, {92, 19}, {93, 19}, {94, 19}, {95, 19},
    {96, 19}, {97, 19}, {98, 19}, {99, 19}, {100, 19},{101, 19},{102, 19},{103, 19},{104, 19},{105, 19},{106, 19},
    {107, 19},{108, 19},{109, 19},{110, 19},{111, 19},{112, 19},{113, 19},{114, 19},{115, 19},{116, 19},{117, 19},
    {118, 19},{119, 19},{120, 19},{18, 18}, {53, 16}, {56, 11}, {57, 8},  {67, 7},  {61, 3},  {59, 2},  {60, 1},
};

constexpr SymLen kScaleRlTable[120] = {
    {103, 7}, {80, 11}, {60, 11}, {18, 10}, {56, 10}, {21, 12}, {90, 12}, {58, 11}, {27, 11}, {69, 12}, {84, 15},
    {48, 15}, {86, 14}, {47, 13}, {19, 10}, {32, 9},  {78, 6},  {5, 5},   {28, 4},  {53, 5},  {9, 7},   {31, 8},
    {38, 8},  {10, 7},  {88, 11}, {25, 12}, {105, 12},{118, 11},{23, 12}, {82, 14}, {98, 16}, {110, 16},{108, 15},
    {93, 13}, {68, 10}, {72, 12}, {97, 12}, {81, 12}, {42, 12}, {64, 8},  {4, 4},   {1, 2},   {7, 6},   {14, 7},
    {0, 9},   {55, 9},  {61, 9},  {117, 10},{24, 12}, {44, 12}, {67, 12}, {70, 16}, {99, 18}, {96, 21}, {95, 21},
    {2, 21},  {77, 21}, {52, 21}, {111, 21},{102, 20},{101, 17},{46, 15}, {73, 15}, {109, 15},{51, 14}, {92, 14},
    {30, 7},  {11, 7},  {66, 7},  {15, 8},  {16, 8},  {116, 9}, {65, 9},  {57, 10}, {59, 10}, {115, 9}, {12, 7},
    {35, 9},  {17, 9},  {41, 9},  {20, 11}, {91, 11}, {26, 12}, {75, 15}, {45, 15}, {107, 14},{83, 14}, {100, 15},
    {89, 15}, {43, 11}, {62, 9},  {37, 9},  {104, 8}, {6, 5},   {39, 8},  {40, 9},  {34, 9},  {79, 7},  {8, 6},
    {63, 6},  {87, 12}, {94, 14}, {49, 14}, {50, 13}, {22, 11}, {119, 10},{33, 9},  {36, 9},  {113, 11},{106, 12},
    {112, 13},{71, 15}, {85, 15}, {74, 14}, {76, 10}, {114, 7}, {29, 5},  {54, 6},  {13, 6},  {3, 2},
};

constexpr std::uint8_t kScaleRlRun[120] = {
    0,  0,  0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18,
    19, 20, 21, 22, 23, 24, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 0,  1,  0,  1,  0,  1,
};

constexpr std::uint8_t kScaleRlLevel[120] = {
    0, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 8, 8, 9, 9,
};

constexpr std::uint8_t kCoef0Lens[272] = {
    2,  9,  14, 14, 13, 12, 13, 14, 15, 15, 12, 10, 10, 10, 13, 14, 15, 15, 12, 11, 13, 14, 14, 13,
    15, 15, 14, 12, 12, 8,  10, 10, 15, 15, 14, 13, 14, 14, 13, 15, 20, 20, 19, 21, 21, 20, 19, 17,
    17, 18, 18, 15, 15, 13, 12, 14, 15, 15, 14, 15, 15, 12, 11, 6,  7,  8,  9,  13, 13, 13, 14, 14,
    11, 10, 7,  8,  14, 14, 14, 14, 12, 13, 13, 12, 12, 12, 11, 9,  13, 14, 14, 12, 11, 11, 11, 9,
    8,  7,  14, 15, 15, 14, 14, 12, 13, 15, 16, 17, 17, 14, 12, 12, 12, 15, 15, 14, 14, 14, 13, 13,
    9,  9,  11, 11, 10, 7,  6,  13, 15, 15, 14, 14, 14, 13, 14, 15, 15, 13, 14, 14, 14, 14, 10, 9,
    10, 10, 11, 11, 10, 8,  9,  13, 14, 14, 12, 11, 14, 15, 15, 13, 12, 14, 14, 14, 14, 13, 14, 14,
    3,  5,  8,  10, 10, 15, 15, 14, 14, 16, 16, 15, 12, 11, 11, 11, 7,  8,  8,  9,  12, 13, 13, 12,
    14, 15, 15, 13, 10, 11, 11, 13, 14, 14, 13, 14, 14, 11, 10, 13, 15, 15, 14, 12, 11, 4,  6,  6,
    8,  12, 12, 12, 13, 13, 12, 13, 13, 14, 14, 13, 13, 13, 9,  7,  9,  11, 14, 14, 13, 14, 14, 13,
    10, 8,  7,  5,  9,  12, 13, 14, 15, 15, 12, 12, 10, 14, 14, 13, 12, 13, 14, 14, 12, 13, 13, 12,
    12, 12, 9,  7,  6,  3,  4,  4,
};

constexpr std::uint16_t kCoef0Syms[272] = {
    2,   25,  111, 94,  69,  58,  87,  93,  136, 135, 59,  37,  34,  36,  82,  182, 120, 138, 195, 45,  168, 216, 178,
    86,  140, 219, 186, 162, 239, 18,  156, 35,  127, 236, 109, 85,  180, 253, 88,  147, 268, 264, 256, 266, 270, 262,
    260, 248, 246, 252, 258, 137, 189, 230, 64,  179, 146, 208, 101, 118, 238, 163, 46,  9,   153, 0,   26,  247, 169,
    76,  202, 131, 194, 38,  13,  19,  132, 106, 191, 97,  65,  198, 77,  62,  66,  164, 48,  27,  81,  183, 102, 60,
    47,  49,  159, 227, 20,  14,  112, 263, 144, 217, 104, 63,  79,  209, 269, 250, 254, 203, 241, 196, 61,  220, 148,
    124, 185, 100, 80,  78,  193, 28,  50,  235, 41,  1,   10,  171, 226, 150, 103, 114, 115, 170, 105, 211, 149, 249,
    108, 188, 107, 255, 231, 155, 42,  40,  55,  160, 39,  21,  29,  215, 234, 184, 228, 51,  116, 142, 145, 172, 165,
    181, 130, 113, 117, 89,  128, 204, 3,   7,   154, 157, 43,  141, 265, 133, 225, 271, 244, 221, 74,  54,  56,  52,
    15,  222, 22,  30,  83,  199, 173, 73,  123, 210, 143, 175, 44,  53,  237, 174, 139, 134, 110, 218, 129, 161, 213,
    177, 267, 151, 125, 67,  223, 5,   11,  192, 23,  214, 243, 166, 200, 176, 68,  224, 187, 257, 261, 232, 96,  251,
    31,  16,  32,  57,  207, 121, 91,  126, 119, 99,  158, 24,  212, 8,   33,  70,  92,  205, 240, 242, 75,  197, 233,
    259, 190, 98,  71,  201, 122, 206, 72,  90,  95,  84,  167, 245, 229, 17,  12,  4,   152, 6,
};

constexpr SymLen kCoef1Table[244] = {
    {2, 2},   {3, 3},   {102, 3}, {4, 4},   {148, 6}, {134, 9}, {171, 10},{18, 10}, {11, 8},  {159, 8}, {14, 9},
    {156, 14},{235, 15},{61, 15}, {38, 13}, {153, 13},{48, 14}, {49, 14}, {23, 11}, {203, 13},{208, 19},{204, 19},
    {129, 18},{94, 17}, {87, 16}, {62, 15}, {174, 15},{147, 15},{29, 12}, {191, 12},{64, 15}, {65, 15}, {146, 14},
    {164, 13},{142, 5}, {132, 4}, {103, 5}, {154, 7}, {165, 9}, {181, 11},{109, 12},{30, 12}, {86, 16}, {92, 16},
    {239, 15},{138, 14},{39, 13}, {50, 14}, {115, 15},{238, 21},{228, 21},{236, 21},{222, 21},{216, 20},{226, 20},
    {196, 18},{192, 17},{120, 16},{221, 14},{51, 14}, {24, 11}, {143, 8}, {7, 6},   {9, 7},   {152, 10},{136, 12},
    {160, 12},{241, 15},{66, 15}, {168, 14},{219, 14},{113, 14},{193, 12},{19, 10}, {173, 10},{105, 8}, {149, 9},
    {15, 9},  {205, 13},{207, 13},{125, 17},{190, 17},{182, 16},{68, 15}, {70, 15}, {67, 15}, {137, 13},{31, 12},
    {223, 14},{116, 15},{210, 19},{220, 19},{198, 18},{126, 17},{88, 16}, {41, 13}, {25, 11}, {40, 13}, {73, 15},
    {243, 15},{53, 14}, {195, 12},{183, 11},{225, 14},{52, 14}, {71, 15}, {121, 16},{89, 16}, {170, 14},{55, 14},
    {69, 15}, {83, 15}, {209, 13},{108, 11},{32, 12}, {54, 14}, {122, 16},{184, 16},{176, 15},{42, 13}, {12, 8},
    {161, 8}, {6, 5},   {167, 9}, {106, 9}, {20, 10}, {145, 12},{111, 13},{43, 13}, {26, 11}, {175, 10},{107, 10},
    {34, 12}, {33, 12}, {197, 12},{74, 15}, {128, 17},{232, 20},{212, 20},{224, 19},{202, 18},{90, 16}, {57, 14},
    {227, 14},{97, 16}, {93, 16}, {140, 15},{185, 11},{27, 11}, {16, 9},  {158, 11},{211, 13},{56, 14}, {117, 15},
    {72, 15}, {166, 13},{91, 16}, {95, 16}, {80, 15}, {101, 16},{194, 17},{127, 17},{82, 15}, {21, 10}, {144, 10},
    {177, 10},{151, 6}, {10, 7},  {157, 7}, {8, 6},   {5, 4},   {13, 8},  {0, 9},   {213, 13},{46, 13}, {199, 12},
    {35, 12}, {162, 12},{135, 10},{169, 9}, {45, 13}, {59, 14}, {114, 14},{44, 13}, {188, 16},{186, 16},{75, 15},
    {79, 15}, {118, 15},{187, 11},{112, 13},{139, 14},{178, 15},{81, 15}, {110, 12},{28, 11}, {163, 8}, {133, 6},
    {104, 6}, {17, 9},  {22, 10}, {229, 14},{172, 14},{217, 13},{201, 12},{36, 12}, {218, 20},{242, 22},{240, 22},
    {234, 21},{230, 19},{206, 18},{200, 18},{214, 18},{130, 17},{131, 17},{141, 15},{84, 15}, {76, 15}, {215, 13},
    {58, 14}, {231, 14},{233, 14},{180, 15},{77, 15}, {37, 12}, {189, 11},{179, 10},{155, 10},{47, 13}, {96, 16},
    {99, 16}, {119, 15},{63, 14}, {237, 14},{78, 15}, {85, 15}, {60, 14}, {98, 16}, {100, 16},{124, 16},{123, 16},
    {150, 11},{1, 7},
};

constexpr std::uint16_t kCoef0Run[272] = {
    0,   0,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,
    21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,
    44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,
    67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,
    90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112,
    113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135,
    136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 0,   1,   2,   3,   4,   5,   6,   7,   8,
    9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,
    32,  33,  34,  35,  36,  37,  38,  39,  0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   0,   1,   2,   3,   4,   0,   1,   0,
    1,   0,   1,   0,   1,   0,   1,   0,   1,   0,   1,   0,   1,   0,   1,   0,   1,   0,   1,   0,   1,   0,   1,
    0,   1,   0,   1,   0,   1,   0,   1,   0,   1,   0,   1,   0,   1,   0,   1,   0,   1,   0,
};

constexpr float kCoef0Level[272] = {
    0.0f,  0.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,
    2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,
    2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,
    3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,
    3.0f,  3.0f,  3.0f,  3.0f,  4.0f,  4.0f,  4.0f,  4.0f,  4.0f,  4.0f,  4.0f,  4.0f,  4.0f,  4.0f,  5.0f,  5.0f,
    5.0f,  5.0f,  5.0f,  6.0f,  6.0f,  7.0f,  7.0f,  8.0f,  8.0f,  9.0f,  9.0f,  10.0f, 10.0f, 11.0f, 11.0f, 12.0f,
    12.0f, 13.0f, 13.0f, 14.0f, 14.0f, 15.0f, 15.0f, 16.0f, 16.0f, 17.0f, 17.0f, 18.0f, 18.0f, 19.0f, 19.0f, 20.0f,
    20.0f, 21.0f, 21.0f, 22.0f, 22.0f, 23.0f, 23.0f, 24.0f, 24.0f, 25.0f, 25.0f, 26.0f, 26.0f, 27.0f, 27.0f, 28.0f,
};

constexpr std::uint16_t kCoef1Run[244] = {
    0,  0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
    54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81,
    82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 0,  1,  2,  3,  4,  5,  6,  7,
    8,  9,  0,  1,  2,  3,  4,  5,  0,  1,  2,  0,  1,  2,  0,  1,  2,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,
    1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,
    1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,
    1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  1,  0,  0,  0,
};

constexpr float kCoef1Level[244] = {
    0.0f,  0.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,
    2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,  2.0f,
    2.0f,  2.0f,  2.0f,  2.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  3.0f,  4.0f,  4.0f,
    4.0f,  4.0f,  4.0f,  4.0f,  5.0f,  5.0f,  5.0f,  6.0f,  6.0f,  6.0f,  7.0f,  7.0f,  7.0f,  8.0f,  8.0f,  9.0f,
    9.0f,  10.0f, 10.0f, 11.0f, 11.0f, 12.0f, 12.0f, 13.0f, 13.0f, 14.0f, 14.0f, 15.0f, 15.0f, 16.0f, 16.0f, 17.0f,
    17.0f, 18.0f, 18.0f, 19.0f, 19.0f, 20.0f, 20.0f, 21.0f, 21.0f, 22.0f, 22.0f, 23.0f, 23.0f, 24.0f, 24.0f, 25.0f,
    25.0f, 26.0f, 26.0f, 27.0f, 27.0f, 28.0f, 28.0f, 29.0f, 29.0f, 30.0f, 30.0f, 31.0f, 31.0f, 32.0f, 32.0f, 33.0f,
    33.0f, 34.0f, 34.0f, 35.0f, 35.0f, 36.0f, 36.0f, 37.0f, 37.0f, 38.0f, 38.0f, 39.0f, 39.0f, 40.0f, 40.0f, 41.0f,
    41.0f, 42.0f, 42.0f, 43.0f, 43.0f, 44.0f, 44.0f, 45.0f, 45.0f, 46.0f, 46.0f, 47.0f, 47.0f, 48.0f, 48.0f, 49.0f,
    49.0f, 50.0f, 51.0f, 52.0f,
};

constexpr std::uint8_t kVec4Lens[127] = {
    1,  6,  8,  10, 10, 10, 10, 8,  8,  10, 10, 9,  8,  8,  9,  12, 12, 11, 12, 12, 11, 9,  9,  8,  8,  9,
    9,  8,  8,  9,  9,  12, 12, 12, 14, 14, 13, 11, 11, 9,  8,  9,  9,  11, 11, 10, 9,  8,  6,  6,  6,  6,
    6,  6,  11, 11, 10, 11, 11, 10, 10, 11, 11, 9,  7,  6,  7,  7,  6,  6,  6,  5,  7,  11, 11, 10, 9,  8,
    6,  9,  9,  10, 10, 9,  8,  8,  6,  6,  6,  8,  8,  9,  12, 12, 11, 10, 8,  8,  8,  10, 10, 10, 10, 9,
    9,  8,  10, 11, 11, 9,  8,  8,  8,  9,  9,  10, 11, 12, 12, 9,  9,  9,  8,  8,  7,  7,  7,
};

constexpr std::uint16_t kVec4Syms[127] = {
    0,     4370,  275,   8195,  4146,  12545, 8225,  290,   4625,  515,   20,    8706,  8210,  4355,  4131,  16385,
    5121,  8961,  321,   1041,  51,    4641,  546,   4610,  530,   513,   8451,  4385,  4130,  33,    8211,  5,
    66,    4161,  1281,  81,    6,     801,   8196,  8481,  8449,  4611,  531,   561,   769,   12290, 8226,  19,
    4097,  2,     4369,  274,   4354,  4114,  12291, 16641, 12305, 49,    12321, 260,   4100,  516,   21,    12546,
    8466,  4353,  4371,  4626,  257,   18,    17,    1,     4386,  8241,  771,   4865,  8705,  8194,  4098,  12561,
    276,   50,    785,   4116,  8209,  4099,  273,   4113,  258,   259,   4609,  35,    1026,  1025,  16401, 305,
    34,    529,   289,   770,   12289, 4,     4145,  4356,  12306, 8193,  12801, 261,   16386, 4881,  3,     514,
    4129,  545,   306,   36,    4101,  65,    20481, 786,   4401,  4866,  8721,  291,   8450,  8465,  4115,
};

constexpr SymLen kVec2Table[137] = {
    {19, 5},  {165, 10},{211, 11},{46, 11}, {75, 10}, {177, 11},{12, 11}, {86, 8},  {83, 7},  {38, 7},  {133, 9},
    {178, 10},{28, 10}, {104, 9}, {73, 9},  {35, 5},  {52, 6},  {113, 9}, {8, 9},   {101, 8}, {69, 7},  {0, 3},
    {71, 8},  {119, 9}, {91, 10}, {179, 10},{114, 8}, {166, 10},{10, 10}, {44, 10}, {145, 10},{66, 6},  {21, 6},
    {24, 8},  {146, 9}, {26, 9},  {65, 7},  {5, 7},   {226, 11},{225, 12},{15, 12}, {180, 10},{147, 9}, {115, 8},
    {40, 8},  {89, 9},  {134, 9}, {84, 7},  {54, 7},  {42, 9},  {60, 10}, {31, 11}, {193, 11},{181, 10},{76, 10},
    {148, 9}, {37, 6},  {67, 6},  {33, 6},  {3, 6},   {17, 6},  {2, 6},   {102, 8}, {87, 8},  {116, 8}, {56, 8},
    {50, 5},  {20, 5},  {120, 9}, {58, 9},  {29, 10}, {194, 10},{135, 9}, {97, 8},  {7, 8},   {105, 9}, {13, 11},
    {241, 12},{16, 12}, {45, 10}, {149, 9}, {74, 9},  {98, 7},  {23, 7},  {85, 7},  {70, 7},  {195, 10},{161, 10},
    {129, 9}, {72, 8},  {51, 5},  {36, 5},  {117, 8}, {61, 10}, {11, 10}, {162, 9}, {1, 7},   {4, 6},   {49, 6},
    {68, 6},  {9, 9},   {27, 9},  {130, 8}, {39, 7},  {53, 6},  {99, 7},  {25, 8},  {150, 9}, {90, 9},  {103, 8},
    {163, 9}, {196, 10},{210, 10},{136, 9}, {121, 9}, {41, 8},  {131, 8}, {43, 9},  {164, 9}, {118, 8}, {88, 8},
    {81, 7},  {6, 7},   {55, 7},  {59, 9},  {30, 10}, {209, 11},{14, 11}, {151, 9}, {106, 9}, {82, 6},  {22, 6},
    {100, 7}, {132, 8}, {57, 8},  {18, 4},  {34, 4},
};

constexpr SymLen kVec1Table[101] = {
    {7, 5},   {32, 8},  {59, 10}, {60, 10}, {83, 11}, {82, 11}, {62, 10}, {33, 8},  {45, 9},  {61, 10}, {84, 11},
    {85, 11}, {1, 6},   {13, 5},  {19, 6},  {25, 7},  {34, 8},  {46, 9},  {47, 9},  {14, 5},  {6, 5},   {64, 10},
    {87, 11}, {86, 11}, {63, 10}, {88, 11}, {90, 11}, {35, 8},  {26, 7},  {0, 7},   {48, 9},  {65, 10}, {66, 10},
    {36, 8},  {15, 5},  {20, 6},  {91, 11}, {89, 11}, {67, 10}, {49, 9},  {50, 9},  {69, 10}, {92, 11}, {93, 11},
    {27, 7},  {5, 5},   {37, 8},  {68, 10}, {71, 10}, {51, 9},  {52, 9},  {70, 10}, {94, 11}, {96, 11}, {38, 8},
    {21, 6},  {16, 5},  {4, 5},   {28, 7},  {53, 9},  {95, 11}, {97, 11}, {73, 10}, {39, 8},  {29, 7},  {72, 10},
    {98, 11}, {99, 11}, {54, 9},  {40, 8},  {22, 6},  {30, 7},  {55, 9},  {74, 10}, {76, 10}, {56, 9},  {75, 10},
    {77, 10}, {17, 5},  {3, 5},   {23, 6},  {41, 8},  {57, 9},  {78, 10}, {79, 10}, {31, 7},  {10, 4},  {9, 4},
    {100, 5}, {2, 5},   {11, 4},  {8, 4},   {18, 5},  {42, 8},  {58, 9},  {80, 10}, {81, 10}, {43, 8},  {44, 8},
    {24, 6},  {12, 4},
};

std::uint32_t floor_log2(std::size_t v) {
    const std::size_t clamped = std::max<std::size_t>(v, 1u);
    return static_cast<std::uint32_t>(std::bit_width(clamped) - 1u);
}

struct PacketCursor {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos;

    bool read_bit(std::uint32_t& out) {
        if ((pos % kPacketBits) == 0) {
            const std::size_t prev = pos / kPacketBits;
            if (prev > 0) {
                const auto hdr = parse_packet(data, size, prev - 1);
                if (!hdr) return false;
                pos = (prev + static_cast<std::size_t>(hdr->skip)) * kPacketBits;
            }
            pos += kHeaderBits;
        }
        const std::size_t byte_idx = pos / 8;
        if (byte_idx >= size) return false;
        out = static_cast<std::uint32_t>((data[byte_idx] >> (7u - (pos % 8u))) & 1u);
        ++pos;
        return true;
    }

    bool read_bits(std::uint32_t n, std::uint32_t& out) {
        std::uint32_t v = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t b = 0;
            if (!read_bit(b)) return false;
            v = (v << 1) | b;
        }
        out = v;
        return true;
    }
};

std::optional<std::size_t> find_next_frame(const std::uint8_t* data, std::size_t size, std::size_t start,
                                           std::size_t end) {
    const std::size_t index = start / kPacketBits;
    const auto header = parse_packet(data, size, index);
    if (!header) return std::nullopt;

    std::size_t at = index * kPacketBits + kHeaderBits + static_cast<std::size_t>(header->first_frame_bit);
    std::uint32_t number = 0;
    while (at < start) {
        PacketCursor cur{data, size, at};
        std::uint32_t bits = 0;
        if (!cur.read_bits(kLengthBits, bits) || bits <= kLengthBits) return std::nullopt;
        at += static_cast<std::size_t>(bits);
        ++number;
    }
    const bool last = (number + 1u) >= header->frames;
    if ((end % kPacketBits) == 0 || (last && (end / kPacketBits) == index)) {
        const std::size_t end_pkt = (end - 1u) / kPacketBits;
        const auto end_hdr = parse_packet(data, size, end_pkt);
        if (!end_hdr) return std::nullopt;
        const std::size_t next_pkt = end_pkt + 1u + static_cast<std::size_t>(end_hdr->skip);
        const auto next_hdr = parse_packet(data, size, next_pkt);
        if (!next_hdr) return std::nullopt;
        end = next_pkt * kPacketBits + kHeaderBits + static_cast<std::size_t>(next_hdr->first_frame_bit);
    }
    if ((end / 8u) >= size) return std::nullopt;
    return end;
}

struct VlcEntry {
    std::uint32_t code;
    std::uint8_t len;
    std::int32_t symbol;
};

struct Vlc {
    std::vector<VlcEntry> codes;

    template <typename Fn>
    static Vlc build(std::size_t count, Fn&& get_sym_len) {
        Vlc vlc;
        vlc.codes.reserve(count);
        std::uint64_t code = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto [sym, len] = get_sym_len(i);
            vlc.codes.push_back({static_cast<std::uint32_t>(code), len, sym});
            code += (1ull << (32u - len));
        }
        return vlc;
    }

    std::int32_t read(std::uint32_t peek32, std::size_t& pos) const {
        const auto it = std::upper_bound(codes.begin(), codes.end(), peek32,
                                         [](std::uint32_t val, const VlcEntry& e) { return val < e.code; });
        const std::size_t idx = (it == codes.begin()) ? 0u : static_cast<std::size_t>((it - codes.begin()) - 1);
        pos += codes[idx].len;
        return codes[idx].symbol;
    }
};

struct CodeTables {
    Vlc scale;
    Vlc scale_rl;
    Vlc coef[2];
    Vlc vec4;
    Vlc vec2;
    Vlc vec1;
};

const CodeTables& code_tables() {
    static const CodeTables tables = [] {
        CodeTables t;
        t.scale = Vlc::build(std::size(kScaleTable), [](std::size_t i) {
            return std::pair<std::int32_t, std::uint8_t>{static_cast<std::int32_t>(kScaleTable[i].sym) - 60,
                                                         kScaleTable[i].len};
        });
        t.scale_rl = Vlc::build(std::size(kScaleRlTable), [](std::size_t i) {
            return std::pair<std::int32_t, std::uint8_t>{static_cast<std::int32_t>(kScaleRlTable[i].sym),
                                                         kScaleRlTable[i].len};
        });
        t.coef[0] = Vlc::build(std::size(kCoef0Syms), [](std::size_t i) {
            return std::pair<std::int32_t, std::uint8_t>{static_cast<std::int32_t>(kCoef0Syms[i]), kCoef0Lens[i]};
        });
        t.coef[1] = Vlc::build(std::size(kCoef1Table), [](std::size_t i) {
            return std::pair<std::int32_t, std::uint8_t>{static_cast<std::int32_t>(kCoef1Table[i].sym),
                                                         kCoef1Table[i].len};
        });
        t.vec4 = Vlc::build(std::size(kVec4Syms), [](std::size_t i) {
            return std::pair<std::int32_t, std::uint8_t>{static_cast<std::int32_t>(kVec4Syms[i]) - 1, kVec4Lens[i]};
        });
        t.vec2 = Vlc::build(std::size(kVec2Table), [](std::size_t i) {
            return std::pair<std::int32_t, std::uint8_t>{static_cast<std::int32_t>(kVec2Table[i].sym) - 1,
                                                         kVec2Table[i].len};
        });
        t.vec1 = Vlc::build(std::size(kVec1Table), [](std::size_t i) {
            return std::pair<std::int32_t, std::uint8_t>{static_cast<std::int32_t>(kVec1Table[i].sym),
                                                         kVec1Table[i].len};
        });
        return t;
    }();
    return tables;
}

void fft_radix2(std::vector<double>& re, std::vector<double>& im) {
    const std::size_t n = re.size();
    std::size_t j = 0;
    for (std::size_t i = 1; i < n; ++i) {
        std::size_t bit = n >> 1;
        while ((j & bit) != 0) {
            j ^= bit;
            bit >>= 1;
        }
        j |= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const double wc = std::cos(ang);
        const double ws = std::sin(ang);
        for (std::size_t start = 0; start < n; start += len) {
            double cr = 1.0;
            double ci = 0.0;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::size_t a = start + k;
                const std::size_t b = a + len / 2;
                const double tr = re[b] * cr - im[b] * ci;
                const double ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                const double next_cr = cr * wc - ci * ws;
                const double next_ci = cr * ws + ci * wc;
                cr = next_cr;
                ci = next_ci;
            }
        }
    }
}

std::vector<float> make_sine_window(std::size_t n) {
    std::vector<float> w(n);
    for (std::size_t i = 0; i < n; ++i) {
        w[i] = static_cast<float>(std::sin((static_cast<double>(i) + 0.5) * kPi / (2.0 * static_cast<double>(n))));
    }
    return w;
}

std::int16_t float_to_i16(float v) {
    const float scaled = std::round(v * 32768.0f);
    const float clamped = std::clamp(scaled, -32768.0f, 32767.0f);
    return static_cast<std::int16_t>(clamped);
}

}  // namespace

struct Decoder::Bits {
    const std::uint8_t* data;
    std::size_t byte_len;
    std::size_t pos;
    std::size_t end;

    std::uint32_t peek32() const {
        std::uint64_t v = 0;
        const std::size_t byte = pos / 8u;
        for (std::size_t i = 0; i < 5u; ++i) {
            const std::uint8_t b = (byte + i < byte_len) ? data[byte + i] : 0u;
            v = (v << 8) | static_cast<std::uint64_t>(b);
        }
        return static_cast<std::uint32_t>((v << (24u + (pos % 8u))) >> 32u);
    }

    std::uint32_t read(std::uint32_t n) {
        if (n == 0) return 0;
        const std::uint32_t v = peek32() >> (32u - n);
        pos += static_cast<std::size_t>(n);
        return v;
    }

    bool bit() { return read(1) != 0; }

    std::int32_t read_signed(std::uint32_t n) {
        const std::uint32_t u = read(n);
        return static_cast<std::int32_t>(u << (32u - n)) >> static_cast<int>(32u - n);
    }

    std::int32_t read_vlc(const Vlc& vlc) { return vlc.read(peek32(), pos); }

    std::uint32_t large_value() {
        std::uint32_t n = 8;
        if (bit()) {
            n += 8;
            if (bit()) {
                n += 8;
                if (bit()) n += 7;
            }
        }
        return read(n);
    }

    bool overread() const { return pos > end; }
};

std::optional<PacketHeader> parse_packet(const std::uint8_t* data, std::size_t size, std::size_t packet_index) {
    if (!data || packet_index > std::numeric_limits<std::size_t>::max() / kPacketBytes) return std::nullopt;
    const std::size_t start = packet_index * kPacketBytes;
    if (start > size || (size - start) < 4u) return std::nullopt;
    const std::uint32_t h = (static_cast<std::uint32_t>(data[start]) << 24) |
                            (static_cast<std::uint32_t>(data[start + 1]) << 16) |
                            (static_cast<std::uint32_t>(data[start + 2]) << 8) |
                            static_cast<std::uint32_t>(data[start + 3]);
    return PacketHeader{h >> 26, (h >> 11) & 0x7fffu, h & 0xffu};
}

std::optional<FrameBits> read_frame(const std::uint8_t* data, std::size_t size, std::size_t start_bit) {
    if (!data || size == 0) return std::nullopt;
    PacketCursor probe{data, size, start_bit};
    std::uint32_t bits_u32 = 0;
    if (!probe.read_bits(kLengthBits, bits_u32)) return std::nullopt;
    const std::size_t bits = static_cast<std::size_t>(bits_u32);
    if (bits <= kLengthBits || bits == 0x7fffu) return std::nullopt;

    std::vector<std::uint8_t> out((bits + 7u) / 8u, 0u);
    PacketCursor cur{data, size, start_bit};
    for (std::size_t i = 0; i < bits; ++i) {
        std::uint32_t b = 0;
        if (!cur.read_bit(b)) return std::nullopt;
        out[i / 8u] |= static_cast<std::uint8_t>(b << (7u - (i % 8u)));
    }
    const std::size_t end = cur.pos;
    return FrameBits{std::move(out), bits, start_bit, find_next_frame(data, size, start_bit, end)};
}

void imdct_half(const float* input, float* output, std::size_t n, float scale) {
    const std::size_t m = n / 2u;
    std::vector<double> re(m, 0.0);
    std::vector<double> im(m, 0.0);
    for (std::size_t k = 0; k < m; ++k) {
        const double a = static_cast<double>(input[2u * k]);
        const double b = static_cast<double>(input[n - 1u - 2u * k]);
        const double ang = -kPi * static_cast<double>(k) / static_cast<double>(n);
        const double c = std::cos(ang);
        const double s = std::sin(ang);
        re[k] = a * c - b * s;
        im[k] = a * s + b * c;
    }
    fft_radix2(re, im);
    for (std::size_t k = 0; k < m; ++k) {
        const double ang = -kPi * static_cast<double>(4u * k + 1u) / static_cast<double>(4u * n);
        const double c = std::cos(ang);
        const double s = std::sin(ang);
        const double wr = re[k] * c - im[k] * s;
        const double wi = re[k] * s + im[k] * c;
        output[n - 1u - 2u * k] = static_cast<float>(wr) * scale;
        output[2u * k] = -static_cast<float>(wi) * scale;
    }
}

void window_overlap(float* block, const float* window, std::size_t block_len) {
    const std::size_t half = block_len / 2u;
    for (std::size_t k = 0; k < half; ++k) {
        const std::size_t i = k;
        const std::size_t j = 2u * half - 1u - k;
        const float s0 = block[i];
        const float s1 = block[j];
        const float wi = window[i];
        const float wj = window[j];
        block[i] = s0 * wj - s1 * wi;
        block[j] = s0 * wi + s1 * wj;
    }
}

const std::vector<float>& sine_window(std::size_t n) {
    static const std::vector<float> w128 = make_sine_window(128);
    static const std::vector<float> w256 = make_sine_window(256);
    static const std::vector<float> w512 = make_sine_window(512);
    if (n == 128u) return w128;
    if (n == 256u) return w256;
    return w512;
}

Decoder::Decoder(std::size_t channels, std::uint32_t sample_rate) : channels_(std::clamp<std::size_t>(channels, 1u, 2u)) {
    std::size_t rate = 24000u;
    if (sample_rate > 44100u) rate = 48000u;
    else if (sample_rate > 32000u) rate = 44100u;
    else if (sample_rate > 24000u) rate = 32000u;

    for (std::size_t i = 0; i < kBlockSizes; ++i) {
        const std::size_t sflen = kFrameSamples >> i;
        auto& offsets = sfb_offsets_[i];
        offsets.fill(0);
        std::size_t band = 1;
        for (std::size_t f = 0; f < std::size(kCriticalFreq); ++f) {
            if (offsets[band - 1u] >= sflen) break;
            const std::size_t offset = (((sflen * 2u * static_cast<std::size_t>(kCriticalFreq[f])) / rate) + 2u) & ~std::size_t(3u);
            if (offset > offsets[band - 1u]) {
                offsets[band] = offset;
                ++band;
            }
            if (offset >= sflen) break;
        }
        offsets[band - 1u] = sflen;
        num_sfb_[i] = band - 1u;
    }

    for (std::size_t i = 0; i < kBlockSizes; ++i) {
        for (std::size_t b = 0; b < num_sfb_[i]; ++b) {
            const std::size_t offset = ((sfb_offsets_[i][b] + sfb_offsets_[i][b + 1u] - 1u) << i) >> 1u;
            for (std::size_t x = 0; x < kBlockSizes; ++x) {
                std::size_t v = 0;
                while (v + 1u < kMaxBands && (sfb_offsets_[x][v + 1u] << x) < offset) ++v;
                sf_offsets_[i][x][b] = v;
            }
        }
    }
    reset();
}

void Decoder::reset() { ch_ = {Channel{}, Channel{}}; }

bool Decoder::decode(const std::uint8_t* data, std::size_t bits, std::vector<std::int16_t>& out, const char** error) {
    const std::size_t byte_len = (bits + 7u) / 8u;
    Bits b{data, byte_len, 0u, bits};
    const std::size_t len = static_cast<std::size_t>(b.read(kLengthBits));
    const char* err = nullptr;
    const bool ok = decode_frame(b, len, err);
    if (!ok) {
        if (error) *error = err ? err : "decode error";
        for (std::size_t c = 0; c < channels_; ++c) ch_[c].out.fill(0.0f);
    } else if (error) {
        *error = nullptr;
    }
    out.reserve(out.size() + kFrameSamples * channels_);
    for (std::size_t i = 0; i < kFrameSamples; ++i) {
        for (std::size_t c = 0; c < channels_; ++c) {
            out.push_back(float_to_i16(ch_[c].out[i]));
        }
    }
    for (std::size_t c = 0; c < channels_; ++c) {
        std::copy(ch_[c].out.begin() + static_cast<std::ptrdiff_t>(kFrameSamples), ch_[c].out.end(), ch_[c].out.begin());
    }
    return ok;
}

void Decoder::flush(std::vector<std::int16_t>& out) {
    out.reserve(out.size() + (kFrameSamples / 2u) * channels_);
    for (std::size_t i = 0; i < kFrameSamples / 2u; ++i) {
        for (std::size_t c = 0; c < channels_; ++c) {
            out.push_back(float_to_i16(ch_[c].out[i]));
        }
    }
    reset();
}

bool Decoder::decode_frame(Bits& b, std::size_t len, const char*& err) {
    if (!decode_tile_header(b, err)) return false;
    if (channels_ > 1u && b.bit() && b.bit()) {
        b.read(static_cast<std::uint32_t>(channels_ * channels_ * 4u));
    }
    b.read(8);  // dynamic range compression gain
    if (b.bit()) {
        if (b.bit()) b.read(floor_log2(kFrameSamples * 2u));
        if (b.bit()) b.read(floor_log2(kFrameSamples * 2u));
    }
    parsed_all_subframes_ = false;
    for (std::size_t c = 0; c < channels_; ++c) {
        ch_[c].decoded_samples = 0;
        ch_[c].cur_subframe = 0;
        ch_[c].reuse_sf = false;
    }
    while (!parsed_all_subframes_) {
        if (!decode_subframe(b, err)) return false;
    }
    if (b.overread() || b.pos > len) {
        err = "frame overread";
        return false;
    }
    return true;
}

bool Decoder::decode_subframe_length(Bits& b, std::size_t offset, std::size_t& out_len, const char*& err) const {
    if (offset == kFrameSamples - kSubframeSamples) {
        out_len = kSubframeSamples;
        return true;
    }
    const std::uint32_t shift = b.bit() ? (1u + b.read(1)) : 0u;
    const std::size_t len = kFrameSamples >> shift;
    if (len < kSubframeSamples) {
        err = "subframe length";
        return false;
    }
    out_len = len;
    return true;
}

bool Decoder::decode_tile_header(Bits& b, const char*& err) {
    std::array<std::size_t, 2> num_samples{0, 0};
    std::array<bool, 2> contains{false, false};
    std::size_t cur_channels = channels_;
    std::size_t min_channel_len = 0;
    for (std::size_t c = 0; c < channels_; ++c) ch_[c].num_subframes = 0;
    const bool fixed = b.bit();
    for (;;) {
        for (std::size_t c = 0; c < channels_; ++c) {
            contains[c] = (num_samples[c] == min_channel_len) &&
                          (fixed || cur_channels == 1u || min_channel_len == kFrameSamples - kSubframeSamples || b.bit());
        }
        std::size_t sflen = 0;
        if (!decode_subframe_length(b, min_channel_len, sflen, err)) return false;
        min_channel_len += sflen;
        for (std::size_t c = 0; c < channels_; ++c) {
            auto& ch = ch_[c];
            if (contains[c]) {
                if (ch.num_subframes >= kMaxSubframes) {
                    err = "too many subframes";
                    return false;
                }
                ch.subframe_len[ch.num_subframes] = sflen;
                num_samples[c] += sflen;
                ++ch.num_subframes;
                if (num_samples[c] > kFrameSamples) {
                    err = "channel longer than frame";
                    return false;
                }
            } else if (num_samples[c] <= min_channel_len) {
                if (num_samples[c] < min_channel_len) {
                    cur_channels = 0;
                    min_channel_len = num_samples[c];
                }
                ++cur_channels;
            }
        }
        if (min_channel_len >= kFrameSamples || b.overread()) break;
    }
    if (b.overread()) {
        err = "tile header overread";
        return false;
    }
    return true;
}

bool Decoder::decode_channel_transform(Bits& b, const char*& err) {
    transform_ = false;
    if (channels_ == 1u) return true;
    if (b.bit()) {
        err = "channel transform bit";
        return false;
    }
    if (channels_for_cur_subframe_ == 2u) {
        if (b.bit()) {
            if (b.bit()) {
                err = "channel transform type";
                return false;
            }
        } else {
            transform_ = true;
        }
    }
    if (transform_) {
        if (!b.bit()) {
            for (std::size_t i = 0; i < num_bands_; ++i) transform_band_[i] = b.bit();
        } else {
            transform_band_.fill(true);
        }
    }
    return true;
}

bool Decoder::decode_scale_factors(Bits& b, const char*& err) {
    const auto& tables = code_tables();
    for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        const std::size_t c = channel_indexes_[i];
        const auto& sf_off = sf_offsets_[table_idx_][ch_[c].table_idx];
        auto& ch = ch_[c];
        const std::size_t target = 1u - ch.scale_factor_idx;
        ch.scale_factors = target;
        if (ch.reuse_sf) {
            const auto from = ch.saved_scale_factors[ch.scale_factor_idx];
            for (std::size_t band = 0; band < num_bands_; ++band) {
                ch.saved_scale_factors[target][band] = from[sf_off[band]];
            }
        }
        if (ch.cur_subframe == 0u || b.bit()) {
            if (!ch.reuse_sf) {
                ch.scale_factor_step = static_cast<std::int32_t>(b.read(2)) + 1;
                std::int32_t val = 45 / ch.scale_factor_step;
                for (std::size_t band = 0; band < num_bands_; ++band) {
                    val += b.read_vlc(tables.scale);
                    ch.saved_scale_factors[target][band] = val;
                }
            } else {
                std::size_t band = 0;
                while (band < num_bands_) {
                    const std::int32_t idx = b.read_vlc(tables.scale_rl);
                    std::uint32_t skip = 0;
                    std::int32_t val = 0;
                    bool neg = false;
                    if (idx == 0) {
                        const std::uint32_t code = b.read(14);
                        skip = (code & 0x3fu) >> 1u;
                        val = static_cast<std::int32_t>(code >> 6u);
                        neg = (code & 1u) == 0u;
                    } else if (idx == 1) {
                        break;
                    } else {
                        skip = kScaleRlRun[static_cast<std::size_t>(idx)];
                        val = static_cast<std::int32_t>(kScaleRlLevel[static_cast<std::size_t>(idx)]);
                        neg = !b.bit();
                    }
                    band += static_cast<std::size_t>(skip);
                    if (band >= num_bands_) {
                        err = "scale factor run";
                        return false;
                    }
                    ch.saved_scale_factors[target][band] += neg ? -val : val;
                    ++band;
                }
            }
            ch.scale_factor_idx = target;
            ch.table_idx = table_idx_;
            ch.reuse_sf = true;
        }
        std::int32_t max_sf = 0;
        if (num_bands_ > 0u) {
            max_sf = ch.saved_scale_factors[ch.scale_factors][0];
            for (std::size_t band = 1; band < num_bands_; ++band) {
                max_sf = std::max(max_sf, ch.saved_scale_factors[ch.scale_factors][band]);
            }
        }
        ch.max_scale_factor = max_sf;
    }
    return true;
}

bool Decoder::decode_coefficients(Bits& b, std::size_t c, const char*& err) {
    const auto& tables = code_tables();
    const std::size_t table = b.bit() ? 1u : 0u;
    const std::uint16_t* run = (table == 1u) ? kCoef1Run : kCoef0Run;
    const float* level = (table == 1u) ? kCoef1Level : kCoef0Level;
    auto& ch = ch_[c];

    bool rl_mode = false;
    std::size_t cur = 0;
    std::size_t num_zeros = 0;
    while ((transmit_num_vec_coeffs_ || !rl_mode) && cur + 3u < ch.num_vec_coeffs) {
        std::array<std::uint32_t, 4> vals{};
        const std::int32_t idx4 = b.read_vlc(tables.vec4);
        if (idx4 < 0) {
            for (std::size_t i = 0; i < 4u; i += 2u) {
                const std::int32_t idx2 = b.read_vlc(tables.vec2);
                if (idx2 < 0) {
                    for (std::size_t k = 0; k < 2u; ++k) {
                        std::uint32_t x = static_cast<std::uint32_t>(b.read_vlc(tables.vec1));
                        if (x == static_cast<std::uint32_t>(std::size(kVec1Table) - 1u)) {
                            x += b.large_value();
                        }
                        vals[i + k] = x;
                    }
                } else {
                    vals[i] = static_cast<std::uint32_t>(idx2 >> 4);
                    vals[i + 1u] = static_cast<std::uint32_t>(idx2 & 0xf);
                }
            }
        } else {
            vals[0] = static_cast<std::uint32_t>((idx4 >> 12) & 0xf);
            vals[1] = static_cast<std::uint32_t>((idx4 >> 8) & 0xf);
            vals[2] = static_cast<std::uint32_t>((idx4 >> 4) & 0xf);
            vals[3] = static_cast<std::uint32_t>(idx4 & 0xf);
        }
        for (std::uint32_t v : vals) {
            if (v != 0u) {
                const bool neg = !b.bit();
                ch.coeffs[cur] = neg ? -static_cast<float>(v) : static_cast<float>(v);
                num_zeros = 0;
            } else {
                ch.coeffs[cur] = 0.0f;
                ++num_zeros;
                if (num_zeros > (subframe_len_ >> 8u)) rl_mode = true;
            }
            ++cur;
        }
        if (b.overread()) {
            err = "vector coefficients overread";
            return false;
        }
    }

    if (cur < subframe_len_) {
        std::fill(ch.coeffs.begin() + static_cast<std::ptrdiff_t>(cur),
                  ch.coeffs.begin() + static_cast<std::ptrdiff_t>(subframe_len_), 0.0f);
        const std::size_t mask = subframe_len_ - 1u;
        std::size_t offset = cur;
        while (offset < subframe_len_) {
            const std::int32_t code = b.read_vlc(tables.coef[table]);
            if (code > 1) {
                offset += static_cast<std::size_t>(run[static_cast<std::size_t>(code)]);
                const bool neg = !b.bit();
                const float v = level[static_cast<std::size_t>(code)];
                ch.coeffs[offset & mask] = neg ? -v : v;
            } else if (code == 1) {
                break;
            } else {
                const std::int32_t value = static_cast<std::int32_t>(b.large_value());
                if (b.bit()) {
                    if (b.bit()) {
                        if (b.bit()) break;  // escape terminator
                        offset += static_cast<std::size_t>(b.read(esc_len_)) + 4u;
                    } else {
                        offset += static_cast<std::size_t>(b.read(2)) + 1u;
                    }
                }
                const bool neg = !b.bit();
                ch.coeffs[offset & mask] = static_cast<float>(neg ? -value : value);
            }
            if (b.overread()) {
                err = "run-level coefficients overread";
                return false;
            }
            ++offset;
        }
    }
    return true;
}

void Decoder::inverse_channel_transform() {
    if (!transform_ || channels_for_cur_subframe_ != 2u) return;
    const auto& sfb = sfb_offsets_[table_idx_];
    auto& a = ch_[0];
    auto& b = ch_[1];
    for (std::size_t band = 0; band < num_bands_; ++band) {
        const std::size_t end = std::min(sfb[band + 1u], subframe_len_);
        if (transform_band_[band]) {
            for (std::size_t y = sfb[band]; y < end; ++y) {
                const float l = a.coeffs[y];
                const float r = b.coeffs[y];
                a.coeffs[y] = l - r;
                b.coeffs[y] = l + r;
            }
        } else {
            for (std::size_t y = sfb[band]; y < end; ++y) {
                a.coeffs[y] *= 181.0f / 128.0f;
                b.coeffs[y] *= 181.0f / 128.0f;
            }
        }
    }
}

bool Decoder::decode_subframe(Bits& b, const char*& err) {
    std::size_t offset = kFrameSamples;
    std::size_t sflen = kFrameSamples;
    std::size_t total_samples = kFrameSamples * channels_;
    for (std::size_t c = 0; c < channels_; ++c) {
        const auto& ch = ch_[c];
        if (offset > ch.decoded_samples) {
            offset = ch.decoded_samples;
            sflen = ch.subframe_len[std::min(ch.cur_subframe, kMaxSubframes - 1u)];
        }
    }
    channels_for_cur_subframe_ = 0;
    for (std::size_t c = 0; c < channels_; ++c) {
        auto& ch = ch_[c];
        total_samples -= ch.decoded_samples;
        if (offset == ch.decoded_samples && ch.cur_subframe < ch.num_subframes &&
            sflen == ch.subframe_len[ch.cur_subframe]) {
            total_samples -= sflen;
            ch.decoded_samples += sflen;
            channel_indexes_[channels_for_cur_subframe_] = c;
            ++channels_for_cur_subframe_;
        }
    }
    if (channels_for_cur_subframe_ == 0u) {
        err = "no channel in subframe";
        return false;
    }
    if (total_samples == 0u) parsed_all_subframes_ = true;

    table_idx_ = static_cast<std::size_t>(floor_log2(kFrameSamples / sflen));
    num_bands_ = num_sfb_[table_idx_];
    subframe_len_ = sflen;
    esc_len_ = floor_log2(sflen - 1u) + 1u;

    if (b.bit()) {
        std::size_t fill = static_cast<std::size_t>(b.read(2));
        if (fill == 0u) {
            const std::uint32_t len = b.read(4);
            fill = static_cast<std::size_t>(b.read(len)) + 1u;
        }
        b.pos += fill;
    }
    if (b.bit()) {
        err = "reserved bit set";
        return false;
    }
    if (!decode_channel_transform(b, err)) return false;

    bool transmit_coeffs = false;
    for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        const std::size_t c = channel_indexes_[i];
        ch_[c].transmit_coefs = b.bit();
        transmit_coeffs = transmit_coeffs || ch_[c].transmit_coefs;
    }
    if (transmit_coeffs) {
        transmit_num_vec_coeffs_ = b.bit();
        if (transmit_num_vec_coeffs_) {
            const std::uint32_t num_bits = floor_log2((sflen + 3u) / 4u) + 1u;
            for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
                const std::size_t c = channel_indexes_[i];
                const std::size_t n = static_cast<std::size_t>(b.read(num_bits)) << 2u;
                if (n > sflen) {
                    err = "vector coefficient count";
                    return false;
                }
                ch_[c].num_vec_coeffs = n;
            }
        } else {
            for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
                ch_[channel_indexes_[i]].num_vec_coeffs = sflen;
            }
        }
        std::int32_t quant_step = 90;
        std::int32_t step = b.read_signed(6);
        quant_step += step;
        if (step == -32 || step == 31) {
            const bool negative = (step == -32);
            std::int32_t quant = 0;
            while (b.pos + 5u < b.end) {
                step = static_cast<std::int32_t>(b.read(5));
                if (step != 31) break;
                quant += 31;
            }
            quant_step += negative ? -(quant + step) : (quant + step);
        }
        if (channels_for_cur_subframe_ == 1u) {
            ch_[channel_indexes_[0]].quant_step = quant_step;
        } else {
            const std::uint32_t mod_len = b.read(3);
            for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
                const std::size_t c = channel_indexes_[i];
                ch_[c].quant_step = quant_step;
                if (b.bit()) {
                    ch_[c].quant_step += (mod_len > 0u) ? (static_cast<std::int32_t>(b.read(mod_len)) + 1) : 1;
                }
            }
        }
        if (!decode_scale_factors(b, err)) return false;
    }

    for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        const std::size_t c = channel_indexes_[i];
        if (ch_[c].transmit_coefs && b.pos < b.end) {
            if (!decode_coefficients(b, c, err)) return false;
        } else {
            std::fill(ch_[c].coeffs.begin(), ch_[c].coeffs.begin() + static_cast<std::ptrdiff_t>(sflen), 0.0f);
        }
    }

    const std::size_t at = offset + kFrameSamples / 2u;
    if (transmit_coeffs) {
        inverse_channel_transform();
        const float scale = 2.0f / static_cast<float>(sflen) / 32768.0f;
        const auto& sfb = sfb_offsets_[table_idx_];
        std::array<float, kFrameSamples> tmp{};
        for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
            auto& ch = ch_[channel_indexes_[i]];
            const auto& sf = ch.saved_scale_factors[ch.scale_factors];
            for (std::size_t band = 0; band < num_bands_; ++band) {
                const std::size_t end = std::min(sfb[band + 1u], sflen);
                const std::int32_t exp = ch.quant_step - (ch.max_scale_factor - sf[band]) * ch.scale_factor_step;
                const float quant = std::pow(10.0f, static_cast<float>(exp) / 20.0f);
                for (std::size_t y = sfb[band]; y < end; ++y) tmp[y] = ch.coeffs[y] * quant;
            }
            imdct_half(tmp.data(), ch.out.data() + at, sflen, scale);
        }
    } else {
        for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
            auto& out_buf = ch_[channel_indexes_[i]].out;
            std::fill(out_buf.begin() + static_cast<std::ptrdiff_t>(at),
                      out_buf.begin() + static_cast<std::ptrdiff_t>(at + sflen), 0.0f);
        }
    }

    for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        auto& ch = ch_[channel_indexes_[i]];
        std::size_t winlen = ch.prev_block_len;
        std::size_t start = at - winlen / 2u;
        if (sflen < winlen) {
            start += (winlen - sflen) / 2u;
            winlen = sflen;
        }
        window_overlap(ch.out.data() + start, sine_window(winlen).data(), winlen);
        ch.prev_block_len = sflen;
    }

    for (std::size_t i = 0; i < channels_for_cur_subframe_; ++i) {
        auto& ch = ch_[channel_indexes_[i]];
        if (ch.cur_subframe >= ch.num_subframes) {
            err = "subframe overrun";
            return false;
        }
        ++ch.cur_subframe;
    }
    if (b.overread()) {
        err = "subframe overread";
        return false;
    }
    return true;
}

}  // namespace leonida::xma2
