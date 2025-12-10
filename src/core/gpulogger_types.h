// SPDX-FileCopyrightText: 2024 PCSX-Redux authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace PCSX {

struct GTEFetchContext {
    uint32_t pc = 0;
    uint32_t address = 0;
    uint32_t baseRegister = 0;
    uint32_t baseValue = 0;
    int16_t offset = 0;
    uint32_t targetRegister = 0;
    uint32_t value = 0;
};

struct GTELogMetadata {
    std::vector<GTEFetchContext> vertexFetches;
};

struct GTEState {
    enum class Command {
        Unknown,
        RTPT,
        RTPS,
        NCLIP,
        OP,
        DPCS,
        INTPL,
        MVMVA,
        NCDS,
        CDP,
        NCDT,
        NCCS,
        CC,
        NCS,
        NCT,
        SQR,
        DCPL,
        DPCT,
        AVSZ3,
        AVSZ4,
        GPL,
        GPF,
        NCCT,
    };

    struct Snapshot {
        std::array<std::array<int16_t, 3>, 3> vertices;
        std::array<std::array<int16_t, 2>, 3> screenCoords;
        std::array<std::array<int16_t, 3>, 3> rotationMatrix;
        std::array<std::array<int16_t, 3>, 3> lightMatrix;
        std::array<std::array<int16_t, 3>, 3> colorMatrix;
        std::array<int32_t, 3> translation;
        std::array<uint32_t, 32> dataRegisters{};
        std::array<uint32_t, 32> controlRegisters{};
        int32_t offsetX = 0;
        int32_t offsetY = 0;
        int16_t projectionPlaneDistance = 0;
        int16_t depthQueueA = 0;
        int32_t depthQueueB = 0;
        int16_t depthScaleFactor3 = 0;
        int16_t depthScaleFactor4 = 0;
    };

    Command command = Command::Unknown;
    uint32_t pc = 0;
    Snapshot input;
    Snapshot output;
    GTELogMetadata metadata;
};

#pragma pack(push, 4)
struct LogEntry {
    uint32_t frame = 0;
    uint32_t pc = 0;

    uint32_t gp0_cmd = 0;
    uint16_t primitive_type = 0;
    uint16_t vertex_count = 0;

    uint32_t packet_words[12] = {};

    int16_t vx[4] = {};
    int16_t vy[4] = {};
    int16_t vz[4] = {};

    int16_t sx[4] = {};
    int16_t sy[4] = {};

    int16_t rot[3][3] = {};
    int32_t trx = 0, try_ = 0, trz = 0;

    int32_t ofx = 0, ofy = 0;
    int16_t h = 0;
    int16_t dqa = 0, dqb = 0;
    int16_t zsf3 = 0, zsf4 = 0;

    uint16_t clut = 0;
    uint16_t tpage = 0;
    uint8_t u[4] = {};
    uint8_t v[4] = {};
};
#pragma pack(pop)

inline constexpr std::size_t LogEntrySizeBytes() {
    // Padding is part of the on-disk format to preserve alignment requirements of the
    // GPU logger, so the helper intentionally returns the padded structure size.
    return sizeof(LogEntry);
}

static_assert(LogEntrySizeBytes() == 168, "LogEntry size changed; update metadata expectations");

}  // namespace PCSX
