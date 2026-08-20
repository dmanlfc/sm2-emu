// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from MAME's src/mame/sega/315-5881_crypt.cpp
// (BSD-3-Clause, copyright-holders Andreas Naive, Olivier Galibert,
// David Haywood).
//
// The cipher's structure, and the s-box and key-scheduling tables below, are
// the result of Andreas Naive's reverse engineering. Some s-box entries are
// still unknown and appear as 255; no known key reaches them.

#include "hw/sega_315_5881_crypt.h"

#include "core/log.h"

#include <cassert>

namespace sm2::hw {
namespace {

constexpr int kFn1Gk = 38;
constexpr int kFn2Gk = 32;

/// MAME's bitswap<N>: the bit positions are listed most significant first, so
/// the first argument after `value` becomes the top bit of the result.
template <typename... Bits>
[[nodiscard]] constexpr u32 bitswap(u32 value, Bits... positions)
{
    constexpr usize count = sizeof...(Bits);
    const int       from[count] = {positions...};

    u32 result = 0;
    for (usize i = 0; i < count; ++i) {
        result |= ((value >> from[i]) & 1u) << (count - 1 - i);
    }
    return result;
}

constexpr Sega3155881Crypt::Sbox kFn1Sboxes[4][4] = {
    {   // 1st round
        {
            {
                0,3,2,2,1,3,1,2,3,2,1,2,1,2,3,1,3,2,2,0,2,1,3,0,0,3,2,3,2,1,2,0,
                2,3,1,1,2,2,1,1,1,0,2,3,3,0,2,1,1,1,1,1,3,0,3,2,1,0,1,2,0,3,1,3,
            },
            {3,4,5,7,-1,-1},
            {0,4}
        },

        {
            {
                2,2,2,0,3,3,0,1,2,2,3,2,3,0,2,2,1,1,0,3,3,2,0,2,0,1,0,1,2,3,1,1,
                0,1,3,3,1,3,3,1,2,3,2,0,0,0,2,2,0,3,1,3,0,3,2,2,0,3,0,3,1,1,0,2,
            },
            {0,1,2,5,6,7},
            {1,6}
        },

        {
            {
                0,1,3,0,3,1,1,1,1,2,3,1,3,0,2,3,3,2,0,2,1,1,2,1,1,3,1,0,0,2,0,1,
                1,3,1,0,0,3,2,3,2,0,3,3,0,0,0,0,1,2,3,3,2,0,3,2,1,0,0,0,2,2,3,3,
            },
            {0,2,5,6,7,-1},
            {2,3}
        },

        {
            {
                3,2,1,2,1,2,3,2,0,3,2,2,3,1,3,3,0,2,3,0,3,3,2,1,1,1,2,0,2,2,0,1,
                1,3,3,0,0,3,0,3,0,2,1,3,2,1,0,0,0,1,1,2,0,1,0,0,0,1,3,3,2,0,3,3,
            },
            {1,2,3,4,6,7},
            {5,7}
        },
    },
    {   // 2nd round
        {
            {
                3,3,1,2,0,0,2,2,2,1,2,1,3,1,1,3,3,0,0,3,0,3,3,2,1,1,3,2,3,2,1,3,
                2,3,0,1,3,2,0,1,2,1,3,1,2,2,3,3,3,1,2,2,0,3,1,2,2,1,3,0,3,0,1,3,
            },
            {0,1,3,4,5,7},
            {0,4}
        },

        {
            {
                2,0,1,0,0,3,2,0,3,3,1,2,1,3,0,2,0,2,0,0,0,2,3,1,3,1,1,2,3,0,3,0,
                3,0,2,0,0,2,2,1,0,2,3,3,1,3,1,0,1,3,3,0,0,1,3,1,0,2,0,3,2,1,0,1,
            },
            {0,1,3,4,6,-1},
            {1,5}
        },

        {
            {
                2,2,2,3,1,1,0,1,3,3,1,1,2,2,2,0,0,3,2,3,3,0,2,1,2,2,3,0,1,3,0,0,
                3,2,0,3,2,0,1,0,0,1,2,2,3,3,0,2,2,1,3,1,1,1,1,2,0,3,1,0,0,2,3,2,
            },
            {1,2,5,6,7,6},
            {2,7}
        },

        {
            {
                0,1,3,3,3,1,3,3,1,0,2,0,2,0,0,3,1,2,1,3,1,2,3,2,2,0,1,3,0,3,3,3,
                0,0,0,2,1,1,2,3,2,2,3,1,1,2,0,2,0,2,1,3,1,1,3,3,1,1,3,0,2,3,0,0,
            },
            {2,3,4,5,6,7},
            {3,6}
        },
    },
    {   // 3rd round
        {
            {
                0,0,1,0,1,0,0,3,2,0,0,3,0,1,0,2,0,3,0,0,2,0,3,2,2,1,3,2,2,1,1,2,
                0,0,0,3,0,1,1,0,0,2,1,0,3,1,2,2,2,0,3,1,3,0,1,2,2,1,1,1,0,2,3,1,
            },
            {1,2,3,4,5,7},
            {0,5}
        },

        {
            {
                1,2,1,0,3,1,1,2,0,0,2,3,2,3,1,3,2,0,3,2,2,3,1,1,1,1,0,3,2,0,0,1,
                1,0,0,1,3,1,2,3,0,0,2,3,3,0,1,0,0,2,3,0,1,2,0,1,3,3,3,1,2,0,2,1,
            },
            {0,2,4,5,6,7},
            {1,6}
        },

        {
            {
                0,3,0,2,1,2,0,0,1,1,0,0,3,1,1,0,0,3,0,0,2,3,3,2,3,1,2,0,0,2,3,0,
                // unused?
                255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
            },
            {0,2,4,6,7,-1},
            {2,3}
        },

        {
            {
                0,0,1,0,0,1,0,2,3,3,0,3,3,2,3,0,2,2,2,0,3,2,0,3,1,0,0,3,3,0,0,0,
                2,2,1,0,2,0,3,2,0,0,3,1,3,3,0,0,2,1,1,2,1,0,1,1,0,3,1,2,0,2,0,3,
            },
            {0,1,2,3,6,-1},
            {4,7}
        },
    },
    {   // 4th round
        {
            {
                0,3,3,3,3,3,2,0,0,1,2,0,2,2,2,2,1,1,0,2,2,1,3,2,3,2,0,1,2,3,2,1,
                3,2,2,3,1,0,1,0,0,2,0,1,2,1,2,3,1,2,1,1,2,2,1,0,1,3,2,3,2,0,3,1,
            },
            {0,1,3,4,5,6},
            {0,5}
        },

        {
            {
                0,3,0,0,2,0,3,1,1,1,2,2,2,1,3,1,2,2,1,3,2,2,3,3,0,3,1,0,3,2,0,1,
                3,0,2,0,1,0,2,1,3,3,1,2,2,0,2,3,3,2,3,0,1,1,3,3,0,2,1,3,0,2,2,3,
            },
            {0,1,2,3,5,7},
            {1,7}
        },

        {
            {
                0,1,2,3,3,3,3,1,2,0,2,3,2,1,0,1,2,2,1,2,0,3,2,0,1,1,0,1,3,1,3,1,
                3,1,0,0,1,0,0,0,0,1,2,2,1,1,3,3,1,2,3,3,3,2,3,0,2,2,1,3,3,0,2,0,
            },
            {2,3,4,5,6,7},
            {2,3}
        },

        {
            {
                0,2,1,1,3,2,0,3,1,0,1,0,3,2,1,1,2,2,0,3,1,0,1,2,2,2,3,3,0,0,0,0,
                1,2,1,0,2,1,2,2,2,3,2,3,0,1,3,0,0,1,3,0,0,1,1,0,1,0,0,0,0,2,0,1,
            },
            {0,1,2,4,6,7},
            {4,6}
        },
    },
};

constexpr Sega3155881Crypt::Sbox kFn2Sboxes[4][4] = {
    {   // 1st round
        {
            {
                3,3,0,1,0,1,0,0,0,3,0,0,1,3,1,2,0,3,3,3,2,1,0,1,1,1,2,2,2,3,2,2,
                2,1,3,3,1,3,1,1,0,0,1,2,0,2,2,1,1,2,3,1,2,1,3,1,2,2,0,1,3,0,2,2,
            },
            {1,3,4,5,6,7},
            {0,7}
        },

        {
            {
                0,1,3,0,1,1,2,3,2,0,0,3,2,1,3,1,3,3,0,0,1,0,0,3,0,3,3,2,3,2,0,1,
                3,2,3,2,2,1,3,1,1,1,0,3,3,2,2,1,1,2,0,2,0,1,1,0,1,0,1,1,2,0,3,0,
            },
            {0,3,5,6,5,0},
            {1,2}
        },

        {
            {
                0,2,2,1,0,1,2,1,2,0,1,2,3,3,0,1,3,1,1,2,1,2,1,3,3,2,3,3,2,1,0,1,
                0,1,0,2,0,1,1,3,2,0,3,2,1,1,1,3,2,3,0,2,3,0,2,2,1,3,0,1,1,2,2,2,
            },
            {0,2,3,4,7,-1},
            {3,4}
        },

        {
            {
                2,3,1,3,2,0,1,2,0,0,3,3,3,3,3,1,2,0,2,1,2,3,0,2,0,1,0,3,0,2,1,0,
                2,3,0,1,3,0,3,2,3,1,2,0,3,1,1,2,0,3,0,0,2,0,2,1,2,2,3,2,1,2,3,1,
            },
            {1,2,5,6,-1,-1},
            {5,6}
        },
    },
    {   // 2nd round
        {
            {
                2,3,1,3,1,0,3,3,3,2,3,3,2,0,0,3,2,3,0,3,1,1,2,3,1,1,2,2,0,1,0,0,
                2,1,0,1,2,0,1,2,0,3,1,1,2,3,1,2,0,2,0,1,3,0,1,0,2,2,3,0,3,2,3,0,
            },
            {0,1,4,5,6,7},
            {0,7}
        },

        {
            {
                0,2,2,0,2,2,0,3,2,3,2,1,3,2,3,3,1,1,0,0,3,0,2,1,1,3,3,2,3,2,0,1,
                1,2,3,0,1,0,3,0,3,1,0,2,1,2,0,3,2,3,1,2,2,0,3,2,3,0,0,1,2,3,3,3,
            },
            {0,2,3,6,7,-1},
            {1,5}
        },

        {
            {
                1,0,3,0,0,1,2,1,0,0,1,0,0,0,2,3,2,2,0,2,0,1,3,0,2,0,1,3,2,3,0,1,
                1,2,2,2,1,3,0,3,0,1,1,0,3,2,3,3,2,0,0,3,1,2,1,3,3,2,1,0,2,1,2,3,
            },
            {2,3,4,6,7,2},
            {2,3}
        },

        {
            {
                2,3,1,3,1,1,2,3,3,1,1,0,1,0,2,3,2,1,0,0,2,2,0,1,0,2,2,2,0,2,1,0,
                3,1,2,3,1,3,0,2,1,0,1,0,0,1,2,2,3,2,3,1,3,2,1,1,2,0,2,1,3,3,1,0,
            },
            {1,2,3,4,5,6},
            {4,6}
        },
    },
    {   // 3rd round
        {
            {
                0,3,0,1,3,0,0,2,1,0,1,3,2,2,2,0,3,3,3,0,2,2,0,3,0,0,2,3,0,3,2,1,
                3,3,0,3,0,2,3,3,1,1,1,0,2,2,1,1,3,0,3,1,2,0,2,0,0,0,3,2,1,1,0,0,
            },
            {1,4,5,6,7,5},
            {0,5}
        },

        {
            {
                0,3,0,1,3,0,3,1,3,2,2,2,3,0,3,2,2,1,2,2,0,3,2,2,0,0,2,1,1,3,2,3,
                2,3,3,1,2,0,1,2,2,1,0,0,0,0,2,3,1,2,0,3,1,3,1,2,3,2,1,0,3,0,0,2,
            },
            {0,2,3,4,6,7},
            {1,7}
        },

        {
            {
                2,2,0,3,0,3,1,0,1,1,2,3,2,3,1,0,0,0,3,2,2,0,2,3,1,3,2,0,3,3,1,3,
                // unused?
                255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
            },
            {1,2,4,7,2,-1},
            {2,4}
        },

        {
            {
                0,2,3,1,3,1,1,0,0,1,3,0,2,1,3,3,2,0,2,1,1,2,3,3,0,0,0,2,0,2,3,0,
                3,3,3,3,2,3,3,2,3,0,1,0,2,3,3,2,0,1,3,1,0,1,2,3,3,0,2,0,3,0,3,3,
            },
            {0,1,2,3,5,7},
            {3,6}
        },
    },
    {   // 4th round
        {
            {
                0,1,1,0,0,1,0,2,3,3,0,1,2,3,0,2,1,0,3,3,2,0,3,0,0,2,1,0,1,0,1,3,
                0,3,3,1,2,0,3,0,1,3,2,0,3,3,1,3,0,2,3,3,2,1,1,2,2,1,2,1,2,0,1,1,
            },
            {0,1,2,4,7,-1},
            {0,5}
        },

        {
            {
                2,0,0,2,3,0,2,3,3,1,1,1,2,1,1,0,0,2,1,0,0,3,1,0,0,3,3,0,1,0,1,2,
                0,2,0,2,0,1,2,3,2,1,1,0,3,3,3,3,3,3,1,0,3,0,0,2,0,3,2,0,2,2,0,1,
            },
            {0,1,3,5,6,-1},
            {1,3}
        },

        {
            {
                0,1,1,2,1,3,1,1,0,0,3,1,1,1,2,0,3,2,0,1,1,2,3,3,3,0,3,0,0,2,0,3,
                3,2,0,0,3,2,3,1,2,3,0,3,2,0,1,2,2,2,0,2,0,1,2,2,3,1,2,2,1,1,1,1,
            },
            {0,2,3,4,5,7},
            {2,7}
        },

        {
            {
                0,1,2,0,3,3,0,3,2,1,3,3,0,3,1,1,3,2,3,2,3,0,0,0,3,0,2,2,3,2,2,3,
                2,2,3,1,2,3,1,2,0,3,0,2,3,1,0,0,3,2,1,2,1,2,1,3,1,0,2,3,3,1,3,2,
            },
            {2,3,4,5,6,7},
            {4,6}
        },
    },
};

constexpr int kFn1GameKeyScheduling[kFn1Gk][2] = {
    {1,29},  {1,71},  {2,4},   {2,54},  {3,8},   {4,56},  {4,73},  {5,11},
    {6,51},  {7,92},  {8,89},  {9,9},   {9,39},  {9,58},  {10,90}, {11,6},
    {12,64}, {13,49}, {14,44}, {15,40}, {16,69}, {17,15}, {18,23}, {18,43},
    {19,82}, {20,81}, {21,32}, {22,5},  {23,66}, {24,13}, {24,45}, {25,12},
    {25,35}, {26,61}, {27,10}, {27,59}, {28,25}, {29,86}
};

constexpr int kFn2GameKeyScheduling[kFn2Gk][2] = {
    {0,0},   {1,3},   {2,11},  {3,20},  {4,22},  {5,23},  {6,29},  {7,38},
    {8,39},  {9,55},  {9,86},  {9,87},  {10,50}, {11,57}, {12,59}, {13,61},
    {14,63}, {15,67}, {16,72}, {17,83}, {18,88}, {19,94}, {20,35}, {21,17},
    {22,6},  {23,85}, {24,16}, {25,25}, {26,92}, {27,47}, {28,28}, {29,90}
};

constexpr int kFn1SequenceKeyScheduling[20][2] = {
    {0,52},  {1,34},  {2,17},  {3,36}, {4,84},  {4,88},  {5,57},  {6,48},
    {6,68},  {7,76},  {8,83},  {9,30}, {10,22}, {10,41}, {11,38}, {12,55},
    {13,74}, {14,19}, {14,80}, {15,26}
};

constexpr int kFn2SequenceKeyScheduling[16] = {77,34,8,42,36,27,69,66,13,9,79,31,49,7,24,64};

constexpr int kFn2MiddleResultScheduling[16] = {1,10,44,68,74,78,81,95,2,4,30,40,41,51,53,58};

/* node format
0xxxxxxx - next node index
1a0bbccc - end node
           a - 0 = repeat
               1 = fetch
           b - if a = 1
               00 - fetch  0
               01 - fetch  1
               11 - fetch -1
               if a = 0
               000
           c - repeat/fetch counter
               count = ccc + 1
11111111 - empty node
*/
constexpr u8 kTrees[9][2][32] = {
    {
        {0x01,0x10,0x0f,0x05,0xc4,0x13,0x87,0x0a,0xcc,0x81,0xce,0x0c,0x86,0x0e,0x84,0xc2,
            0x11,0xc1,0xc3,0xcf,0x15,0xc8,0xcd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc7,0x02,0x03,0x04,0x80,0x06,0x07,0x08,0x09,0xc9,0x0b,0x0d,0x82,0x83,0x85,0xc0,
            0x12,0xc6,0xc5,0x14,0x16,0xca,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x02,0x80,0x05,0x04,0x81,0x10,0x15,0x82,0x09,0x83,0x0b,0x0c,0x0d,0xdc,0x0f,0xde,
            0x1c,0xcf,0xc5,0xdd,0x86,0x16,0x87,0x18,0x19,0x1a,0xda,0xca,0xc9,0x1e,0xce,0xff,},
        {0x01,0x17,0x03,0x0a,0x08,0x06,0x07,0xc2,0xd9,0xc4,0xd8,0xc8,0x0e,0x84,0xcb,0x85,
            0x11,0x12,0x13,0x14,0xcd,0x1b,0xdb,0xc7,0xc0,0xc1,0x1d,0xdf,0xc3,0xc6,0xcc,0xff,},
    },
    {
        {0xc6,0x80,0x03,0x0b,0x05,0x07,0x82,0x08,0x15,0xdc,0xdd,0x0c,0xd9,0xc2,0x14,0x10,
            0x85,0x86,0x18,0x16,0xc5,0xc4,0xc8,0xc9,0xc0,0xcc,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0x01,0x02,0x12,0x04,0x81,0x06,0x83,0xc3,0x09,0x0a,0x84,0x11,0x0d,0x0e,0x0f,0x19,
            0xca,0xc1,0x13,0xd8,0xda,0xdb,0x17,0xde,0xcd,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x80,0x0d,0x04,0x05,0x15,0x83,0x08,0xd9,0x10,0x0b,0x0c,0x84,0x0e,0xc0,0x14,
            0x12,0xcb,0x13,0xca,0xc8,0xc2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc5,0x02,0x03,0x07,0x81,0x06,0x82,0xcc,0x09,0x0a,0xc9,0x11,0xc4,0x0f,0x85,0xd8,
            0xda,0xdb,0xc3,0xdc,0xdd,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x80,0x06,0x0c,0x05,0x81,0xd8,0x84,0x09,0xdc,0x0b,0x0f,0x0d,0x0e,0x10,0xdb,
            0x11,0xca,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc4,0x02,0x03,0x04,0xcb,0x0a,0x07,0x08,0xd9,0x82,0xc8,0x83,0xc0,0xc1,0xda,0xc2,
            0xc9,0xc3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x02,0x06,0x0a,0x83,0x0b,0x07,0x08,0x09,0x82,0xd8,0x0c,0xd9,0xda,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc3,0x80,0x03,0x04,0x05,0x81,0xca,0xc8,0xdb,0xc9,0xc0,0xc1,0x0d,0xc2,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x02,0x03,0x04,0x81,0x07,0x08,0xd8,0xda,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc2,0x80,0x05,0xc9,0xc8,0x06,0x82,0xc0,0x09,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0x80,0x04,0xc8,0xc0,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc1,0x02,0x03,0x81,0x05,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
    {
        {0x01,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
        {0xc0,0x80,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,},
    },
};

}  // namespace

const Sega3155881Crypt::Sbox& Sega3155881Crypt::fn1_sbox(int round, int index)
{
    return kFn1Sboxes[round][index];
}

const Sega3155881Crypt::Sbox& Sega3155881Crypt::fn2_sbox(int round, int index)
{
    return kFn2Sboxes[round][index];
}

Sega3155881Crypt::Sega3155881Crypt()
    : m_buffer(kBufferSize, 0)
    , m_line_buffer(kLineSize, 0)
    , m_line_buffer_prev(kLineSize, 0)
{
}

void Sega3155881Crypt::reset()
{
    m_buffer.assign(kBufferSize, 0);
    m_line_buffer.assign(kLineSize, 0);
    m_line_buffer_prev.assign(kLineSize, 0);

    m_prot_cur_address = 0;
    m_subkey           = 0;
    m_dec_hist         = 0;
    m_dec_header       = 0;
    m_enc_ready        = false;
    m_first_read       = false;

    m_buffer_pos       = 0;
    m_line_buffer_pos  = 0;
    m_line_buffer_size = 0;
    m_buffer_bit       = 0;
}

// ---------------------------------------------------------------------------
// Register interface
// ---------------------------------------------------------------------------

void Sega3155881Crypt::addrlo_w(u16 data)
{
    set_addr_low(data);
    m_first_read = true;
}

void Sega3155881Crypt::addrhi_w(u16 data)
{
    // Always forced to zero. MAME notes the honest version breaks LA Machine
    // Gun, so the high half of the source address is deliberately discarded.
    set_addr_high(0);
    if (data != 0) {
        SM2_WARN("315-5881: non-zero high address %04x", data);
    }
    m_first_read = true;
}

void Sega3155881Crypt::subkey_le_w(u16 data)
{
    set_subkey(data);
}

void Sega3155881Crypt::subkey_be_w(u16 data)
{
    set_subkey(static_cast<u16>(((data & 0xff00) >> 8) | ((data & 0x00ff) << 8)));
}

u16 Sega3155881Crypt::decrypt_le_r()
{
    const u16 value = decrypt_be_r();
    return static_cast<u16>(((value & 0xff00) >> 8) | ((value & 0x00ff) << 8));
}

u16 Sega3155881Crypt::decrypt_be_r()
{
    if (m_first_read) {
        // The RAM-based schemes expect a dummy word ahead of the stream. It is
        // probably header data; MAME returns zero to match its earlier
        // Dynamite Cop simulation, and every known title accepts that.
        m_first_read = false;
        return 0;
    }

    return do_decrypt();
}

u16 Sega3155881Crypt::do_decrypt()
{
    if (!m_enc_ready) {
        enc_start();
    }

    const u8* base = nullptr;
    if (m_dec_header & kFlagCompressed) {
        // MAME tests for equality here. An odd line length would step straight
        // past it and read off the end of the buffer, which a well-formed stream
        // never produces but a program staging junk in the RAM window can.
        if (m_line_buffer_pos >= m_line_buffer_size) {
            if (m_done_compression == 1) {
                enc_start();
            }
            line_fill();
        }
        base = m_line_buffer.data() + m_line_buffer_pos;
        m_line_buffer_pos += 2;
    } else {
        if (m_buffer_pos == kBufferSize) {
            enc_fill();
        }
        base = m_buffer.data() + m_buffer_pos;
        m_buffer_pos += 2;
    }

    return static_cast<u16>((base[0] << 8) | base[1]);
}

void Sega3155881Crypt::set_addr_low(u16 data)
{
    m_prot_cur_address = (m_prot_cur_address & 0xffff0000u) | data;
    m_enc_ready        = false;
}

void Sega3155881Crypt::set_addr_high(u16 data)
{
    m_prot_cur_address = (m_prot_cur_address & 0x0000ffffu) | (static_cast<u32>(data) << 16);
    m_enc_ready        = false;

    m_buffer_bit  = 7;
    m_buffer_bit2 = 15;
}

void Sega3155881Crypt::set_subkey(u16 data)
{
    m_subkey    = data;
    m_enc_ready = false;
}

// ---------------------------------------------------------------------------
// Block cipher
// ---------------------------------------------------------------------------

int Sega3155881Crypt::feistel_function(int input, const Sbox* sboxes, u32 subkeys)
{
    int k, m;
    int aux;
    int result = 0;

    for (m = 0; m < 4; ++m) {  // 4 sboxes
        for (k = 0, aux = 0; k < 6; ++k) {
            if (sboxes[m].inputs[k] != -1) {
                aux |= bit(input, sboxes[m].inputs[k]) << k;
            }
        }

        aux = sboxes[m].table[(aux ^ subkeys) & 0x3f];

        for (k = 0; k < 2; ++k) {
            result |= bit(aux, k) << sboxes[m].outputs[k];
        }

        subkeys >>= 6;
    }

    return result;
}

// The key scheduling below is deliberately left in MAME's "educational" form:
// the game-key part could be hoisted out of the per-word path, but the profile
// of a Model 2 boot never gets near making that worth the divergence.
u16 Sega3155881Crypt::block_decrypt(u32 game_key, u16 sequence_key, u16 counter, u16 data)
{
    int j;
    int aux, aux2;
    int A, B;
    int middle_result;
    u32 fn1_subkeys[4]{};
    u32 fn2_subkeys[4]{};

    /* Game-key scheduling; this could be done just once per game at initialization time */
    for (j = 0; j < kFn1Gk; ++j) {
        if (bit(game_key, kFn1GameKeyScheduling[j][0]) != 0) {
            aux  = kFn1GameKeyScheduling[j][1] % 24;
            aux2 = kFn1GameKeyScheduling[j][1] / 24;
            fn1_subkeys[aux2] ^= (1 << aux);
        }
    }

    for (j = 0; j < kFn2Gk; ++j) {
        if (bit(game_key, kFn2GameKeyScheduling[j][0]) != 0) {
            aux  = kFn2GameKeyScheduling[j][1] % 24;
            aux2 = kFn2GameKeyScheduling[j][1] / 24;
            fn2_subkeys[aux2] ^= (1 << aux);
        }
    }

    /* Sequence-key scheduling; this could be done just once per decryption run */
    for (j = 0; j < 20; ++j) {
        if (bit(sequence_key, kFn1SequenceKeyScheduling[j][0]) != 0) {
            aux  = kFn1SequenceKeyScheduling[j][1] % 24;
            aux2 = kFn1SequenceKeyScheduling[j][1] / 24;
            fn1_subkeys[aux2] ^= (1 << aux);
        }
    }

    for (j = 0; j < 16; ++j) {
        if (bit(sequence_key, j) != 0) {
            aux  = kFn2SequenceKeyScheduling[j] % 24;
            aux2 = kFn2SequenceKeyScheduling[j] / 24;
            fn2_subkeys[aux2] ^= (1 << aux);
        }
    }

    // First Feistel Network

    aux = bitswap(counter, 5, 12, 14, 13, 9, 3, 6, 4, 8, 1, 15, 11, 0, 7, 10, 2);

    // 1st round
    B = aux >> 8;
    A = (aux & 0xff) ^ feistel_function(B, kFn1Sboxes[0], fn1_subkeys[0]);

    // 2nd round
    B ^= feistel_function(A, kFn1Sboxes[1], fn1_subkeys[1]);

    // 3rd round
    A ^= feistel_function(B, kFn1Sboxes[2], fn1_subkeys[2]);

    // 4th round
    B ^= feistel_function(A, kFn1Sboxes[3], fn1_subkeys[3]);

    middle_result = (B << 8) | A;

    /* Middle-result-key sheduling */
    for (j = 0; j < 16; ++j) {
        if (bit(middle_result, j) != 0) {
            aux  = kFn2MiddleResultScheduling[j] % 24;
            aux2 = kFn2MiddleResultScheduling[j] / 24;
            fn2_subkeys[aux2] ^= (1 << aux);
        }
    }

    // Second Feistel Network

    aux = bitswap(data, 14, 3, 8, 12, 13, 7, 15, 4, 6, 2, 9, 5, 11, 0, 1, 10);

    // 1st round
    B = aux >> 8;
    A = (aux & 0xff) ^ feistel_function(B, kFn2Sboxes[0], fn2_subkeys[0]);

    // 2nd round
    B ^= feistel_function(A, kFn2Sboxes[1], fn2_subkeys[1]);

    // 3rd round
    A ^= feistel_function(B, kFn2Sboxes[2], fn2_subkeys[2]);

    // 4th round
    B ^= feistel_function(A, kFn2Sboxes[3], fn2_subkeys[3]);

    aux = (B << 8) | A;

    aux = bitswap(aux, 15, 7, 6, 14, 13, 12, 5, 4, 3, 2, 11, 10, 9, 1, 0, 8);

    return static_cast<u16>(aux);
}

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------

u16 Sega3155881Crypt::get_decrypted_16()
{
    const u16 enc = m_read ? m_read(m_prot_cur_address) : 0;

    const u16 dec = block_decrypt(m_key, m_subkey, static_cast<u16>(m_prot_cur_address), enc);

    // Only two bits per block are fresh: the header's leading bits put the
    // plaintext two bits out of step with the cipher's word boundaries.
    const u16 res = static_cast<u16>((dec & 3) | (m_dec_hist & 0xfffc));
    m_dec_hist    = dec;

    m_prot_cur_address++;

    return res;
}

void Sega3155881Crypt::enc_start()
{
    m_block_pos        = 0;
    m_done_compression = 0;
    m_buffer_pos       = kBufferSize;

    if (m_buffer_bit2 < 14) {
        // Bits still pending in the decompression buffer, so the next word must
        // not be fetched yet (twcup98 needs this; twsoc98's title screen leaves
        // it at 14 and expects the normal path).
        m_dec_header = (m_buffer2a & 0x0003u) << 16;
    } else {
        // astrass otherwise lets every call after the first be influenced by
        // the one before it.
        m_dec_hist   = 0;
        m_dec_header = static_cast<u32>(get_decrypted_16()) << 16;
    }

    m_dec_header |= get_decrypted_16();

    // 0x20000 marks the substream compressed; bits 0x1ff00 and 0x000ff hold one
    // factor each of its length, both biased by one. For a compressed substream
    // the 0x1ff00 factor is the line size.
    m_block_numlines = static_cast<int>((m_dec_header & 0x000000ff) >> 0) + 1;
    const int blocky = static_cast<int>((m_dec_header & 0x0001ff00) >> 8) + 1;
    m_block_size     = m_block_numlines * blocky;

    if (m_dec_header & kFlagCompressed) {
        m_line_buffer_size = blocky;
        m_line_buffer_pos  = m_line_buffer_size;
        m_buffer_bit       = 7;
        m_buffer_bit2      = 15;
    }

    m_enc_ready = true;
}

void Sega3155881Crypt::enc_fill()
{
    assert(m_buffer_pos == kBufferSize);
    for (int i = 0; i != kBufferSize; i += 2) {
        const u16 val = get_decrypted_16();
        m_buffer[i]     = static_cast<u8>(val);
        m_buffer[i + 1] = static_cast<u8>(val >> 8);
        m_block_pos += 2;

        if (!(m_dec_header & kFlagCompressed)) {
            if (m_block_pos == m_block_size) {
                // The declared length is exhausted, so a new header follows.
                enc_start();
            }
        }
    }
    m_buffer_pos = 0;
}

int Sega3155881Crypt::get_compressed_bit()
{
    if (m_buffer_bit2 == 15) {
        m_buffer_bit2 = 0;
        m_buffer2a    = get_decrypted_16();
        m_buffer2[0]  = static_cast<u8>(m_buffer2a);
        m_buffer2[1]  = static_cast<u8>(m_buffer2a >> 8);
        m_buffer_pos  = 0;
    } else {
        m_buffer_bit2++;
    }

    const int res = (m_buffer2[(m_buffer_pos & 1) ^ 1] >> m_buffer_bit) & 1;
    m_buffer_bit--;
    if (m_buffer_bit == -1) {
        m_buffer_bit = 7;
        m_buffer_pos++;
    }
    return res;
}

void Sega3155881Crypt::line_fill()
{
    assert(m_line_buffer_pos >= m_line_buffer_size);
    u8* lp = m_line_buffer.data();
    u8* lc = m_line_buffer_prev.data();

    // Swapping the vectors leaves lp pointing at the line just emitted and lc
    // at the one about to be built, which is what the copy-from-previous-line
    // node below reads.
    m_line_buffer.swap(m_line_buffer_prev);

    m_line_buffer_pos = 0;

    // MAME writes `i != line_buffer_size`. A node whose count overshoots the end
    // of the line would then miss the exit test and run away through memory, so
    // the bound is checked here and again on each byte written below. Neither
    // guard changes how many bits a well-formed stream consumes: the count and
    // the literal byte are read before anything is stored.
    for (int i = 0; i < m_line_buffer_size;) {
        // vlc 0: start of line
        // vlc 1: interior of line
        // vlc 2-9: 7-1 bytes from end of line

        const int slot = i ? (i < m_line_buffer_size - 7 ? 1 : (i & 7) + 1) : 0;

        u32 tmp = 0;
        while (!(tmp & 0x80)) {
            tmp = get_compressed_bit() ? kTrees[slot][1][tmp] : kTrees[slot][0][tmp];
        }
        if (tmp != 0xff) {
            const int count = static_cast<int>(tmp & 7) + 1;

            if (tmp & 0x40) {
                // Copy from previous line
                static const int offsets[4] = {0, 1, 0, -1};
                const int        offset     = offsets[(tmp & 0x18) >> 3];
                for (int j = 0; j != count && i < m_line_buffer_size; j++) {
                    lc[i ^ 1] = lp[((i + offset) % m_line_buffer_size) ^ 1];
                    i++;
                }
            } else {
                // Get a byte in the stream and write n times
                u8 byte;
                byte =         get_compressed_bit()  << 1;
                byte = (byte | get_compressed_bit()) << 1;
                byte = (byte | get_compressed_bit()) << 1;
                byte = (byte | get_compressed_bit()) << 1;
                byte = (byte | get_compressed_bit()) << 1;
                byte = (byte | get_compressed_bit()) << 1;
                byte = (byte | get_compressed_bit()) << 1;
                byte =  byte | get_compressed_bit();
                for (int j = 0; j != count && i < m_line_buffer_size; j++) {
                    lc[(i++) ^ 1] = byte;
                }
            }
        }
    }

    m_block_pos++;
    if (m_block_numlines == m_block_pos) {
        m_done_compression = 1;
    }
}

}  // namespace sm2::hw
