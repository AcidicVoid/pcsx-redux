// SPDX-FileCopyrightText: 2024 PCSX-Redux authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>

namespace PCSX {

struct GTEState {
    enum class Command { Unknown, RTPT, RTPS, NCLIP };

    Command command = Command::Unknown;
    std::array<std::array<int16_t, 3>, 3> vertices;
    std::array<std::array<int16_t, 2>, 3> screenCoords;
    std::array<std::array<int16_t, 3>, 3> rotationMatrix;
    std::array<std::array<int16_t, 3>, 3> lightMatrix;
    std::array<std::array<int16_t, 3>, 3> colorMatrix;
    std::array<int32_t, 3> translation;
    int32_t offsetX = 0;
    int32_t offsetY = 0;
    int16_t projectionPlaneDistance = 0;
    int16_t depthQueueA = 0;
    int32_t depthQueueB = 0;
    int16_t depthScaleFactor3 = 0;
    int16_t depthScaleFactor4 = 0;
};

}  // namespace PCSX
