// SPDX-FileCopyrightText: 2024 PCSX-Redux authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>

namespace PCSX {

struct GTEState {
    std::array<int16_t, 3> vx0;
    std::array<int16_t, 3> vx1;
    std::array<int16_t, 3> vx2;
    std::array<int16_t, 3> rotationMatrix;
    std::array<int16_t, 3> rotationMatrixRow2;
    std::array<int16_t, 3> rotationMatrixRow3;
    std::array<int16_t, 3> lightMatrix;
    std::array<int16_t, 3> lightMatrixRow2;
    std::array<int16_t, 3> lightMatrixRow3;
    std::array<int16_t, 3> colorMatrix;
    std::array<int16_t, 3> colorMatrixRow2;
    std::array<int16_t, 3> colorMatrixRow3;
    std::array<int32_t, 3> translation;
};

}  // namespace PCSX
