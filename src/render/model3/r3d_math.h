// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real3D matrix stack and floating-point formats.
//
// Derived from Supermodel's Src/Graphics/New3D/Mat4.cpp, Mat4.h, R3DFloat.cpp,
// R3DFloat.h and Vec.cpp (https://github.com/trzy/Supermodel), GPL-3.0-or-later.
// Copyright (c) 2003-2026 The Supermodel Team (New3D by Ian Curtis).
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
#pragma once

#include "core/types.h"

#include <array>
#include <bit>

namespace sm2::render::model3 {

class Mat4 {
public:
    Mat4() { load_identity(); }

    void load_identity();
    void translate(float x, float y, float z);
    void frustum_rz(float left, float right, float bottom, float top, float near_val);
    void mult_matrix(const float* m);
    void load_matrix(const float* m);
    void push_matrix();
    void pop_matrix();
    void release() { m_depth = 0; }

    [[nodiscard]] const float* data() const { return current.data(); }

    std::array<float, 16> current{};

private:
    static void multiply(const float a[16], const float b[16], float r[16]);

    static constexpr u32 kStackSize = 130;  // push_matrix() stops once the depth passes 128
    std::array<std::array<float, 16>, kStackSize> m_stack{};
    u32 m_depth = 0;
};

inline void invert3(float v[3])
{
    v[0] = -v[0];
    v[1] = -v[1];
    v[2] = -v[2];
}

// -- Real3D floats -------------------------------------------------------------

[[nodiscard]] inline float u32_as_float(u32 v) { return std::bit_cast<float>(v); }
[[nodiscard]] inline u32   float_as_u32(float v) { return std::bit_cast<u32>(v); }

[[nodiscard]] u32 convert_pro_float(u32 a1);
[[nodiscard]] inline u32 convert_16bit_pro_float(u32 a1) { return convert_pro_float(a1 << 15); }
[[nodiscard]] inline float pro_float16(u16 f) { return u32_as_float(convert_16bit_pro_float(f)); }
[[nodiscard]] inline float pro_float32(u32 f) { return u32_as_float(convert_pro_float(f)); }

}  // namespace sm2::render::model3
