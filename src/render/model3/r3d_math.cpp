// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real3D scene maths.
//
// Derived from Supermodel's Src/Graphics/New3D/Mat4.cpp and R3DFloat.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later.
// Copyright (c) 2003-2026 The Supermodel Team (New3D by Ian Curtis).
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.

#include "render/model3/r3d_math.h"

#include <cstring>

namespace sm2::render::model3 {

void Mat4::load_identity()
{
    float* m = current.data();
    m[0] = 1.f; m[4] = 0.f; m[8 ] = 0.f; m[12] = 0.f;
    m[1] = 0.f; m[5] = 1.f; m[9 ] = 0.f; m[13] = 0.f;
    m[2] = 0.f; m[6] = 0.f; m[10] = 1.f; m[14] = 0.f;
    m[3] = 0.f; m[7] = 0.f; m[11] = 0.f; m[15] = 1.f;
}

// r may alias a.
void Mat4::multiply(const float a[16], const float b[16], float r[16])
{
#define A(row, col) a[((col) << 2) + (row)]
#define B(row, col) b[((col) << 2) + (row)]
#define P(row, col) r[((col) << 2) + (row)]
    for (int i = 0; i < 4; i++) {
        const float ai0 = A(i, 0), ai1 = A(i, 1), ai2 = A(i, 2), ai3 = A(i, 3);
        P(i, 0) = ai0 * B(0, 0) + ai1 * B(1, 0) + ai2 * B(2, 0) + ai3 * B(3, 0);
        P(i, 1) = ai0 * B(0, 1) + ai1 * B(1, 1) + ai2 * B(2, 1) + ai3 * B(3, 1);
        P(i, 2) = ai0 * B(0, 2) + ai1 * B(1, 2) + ai2 * B(2, 2) + ai3 * B(3, 2);
        P(i, 3) = ai0 * B(0, 3) + ai1 * B(1, 3) + ai2 * B(2, 3) + ai3 * B(3, 3);
    }
#undef A
#undef B
#undef P
}

void Mat4::translate(float x, float y, float z)
{
    float m[16];
    m[0] = 1.f;  m[1] = 0.f;  m[2] = 0.f;  m[3] = 0.f;
    m[4] = 0.f;  m[5] = 1.f;  m[6] = 0.f;  m[7] = 0.f;
    m[8] = 0.f;  m[9] = 0.f;  m[10] = 1.f; m[11] = 0.f;
    m[12] = x;   m[13] = y;   m[14] = z;   m[15] = 1.f;
    multiply(current.data(), m, current.data());
}

void Mat4::frustum_rz(float left, float right, float bottom, float top, float near_val)
{
    const float x = 2.0F / (right - left);
    const float y = 2.0F / (top - bottom);
    const float a = (right + left) / (right - left);
    const float b = (top + bottom) / (top - bottom);

    float m[16];
    m[0] = x;    m[1] = 0.f;  m[2] = 0.f;  m[3] = 0.f;
    m[4] = 0.f;  m[5] = y;    m[6] = 0.f;  m[7] = 0.f;
    m[8] = a;    m[9] = b;    m[10] = 0.f; m[11] = -1.f;
    m[12] = 0.f; m[13] = 0.f; m[14] = near_val; m[15] = 0.f;
    multiply(current.data(), m, current.data());
}

void Mat4::mult_matrix(const float* m)
{
    if (m == nullptr) {
        return;
    }
    multiply(current.data(), m, current.data());
}

void Mat4::load_matrix(const float* m)
{
    if (m == nullptr) {
        return;
    }
    std::memcpy(current.data(), m, sizeof(float) * 16);
}

void Mat4::push_matrix()
{
    if (m_depth > 128) {
        return;  // overflow
    }
    m_stack[m_depth++] = current;
}

void Mat4::pop_matrix()
{
    if (m_depth == 0) {
        return;  // underflow
    }
    current = m_stack[--m_depth];
}

u32 convert_pro_float(u32 a1)
{
    u32 exponent = (a1 & 0x7E000000) >> 25;

    if (exponent <= 31) {  // positive
        exponent += 127;
    } else {  // negative exponent
        exponent += 127 - 64;
    }

    const u32 mantissa = (a1 & 0x1FFFFFF) >> 2;

    return (a1 & 0x80000000) | (exponent << 23) | mantissa;
}

}  // namespace sm2::render::model3
