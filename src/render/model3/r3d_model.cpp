// SPDX-License-Identifier: GPL-3.0-or-later
//
// Culling-node attribute stack.
//
// Derived from Supermodel's Src/Graphics/New3D/Model.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later.
// Copyright (c) 2003-2026 The Supermodel Team (New3D by Ian Curtis).
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.

#include "render/model3/r3d_model.h"

namespace sm2::render::model3 {

bool NodeAttributes::Push()
{
    if (m_depth >= kMaxDepth) {
        return false;
    }

    NodeAttribs& na   = m_vecAttribs[m_depth++];
    na.page           = currentPage;
    na.texOffsetX     = currentTexOffsetX;
    na.texOffsetY     = currentTexOffsetY;
    na.modelScale     = currentModelScale;
    na.modelAlpha     = currentModelAlpha;
    na.disableCulling = currentDisableCulling;
    return true;
}

bool NodeAttributes::Pop()
{
    if (m_depth == 0) {
        return false;
    }

    const NodeAttribs& last = m_vecAttribs[--m_depth];

    currentPage           = last.page;
    currentTexOffsetX     = last.texOffsetX;
    currentTexOffsetY     = last.texOffsetY;
    currentModelScale     = last.modelScale;
    currentModelAlpha     = last.modelAlpha;
    currentDisableCulling = last.disableCulling;
    return true;
}

void NodeAttributes::Reset()
{
    currentPage           = 0;
    currentTexOffsetX     = 0;
    currentTexOffsetY     = 0;
    currentModelScale     = 1.0f;
    currentModelAlpha     = 1.0f;
    currentDisableCulling = false;

    m_depth = 0;
}

}  // namespace sm2::render::model3
