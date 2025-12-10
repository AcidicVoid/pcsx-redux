/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include "core/gpulogger.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include "core/gpu.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "imgui/imgui.h"

namespace {
constexpr size_t c_maxLoggedWords = 1024;
}

static const char* const c_vtx = R"(
#version 330 core

// inPos: The vertex position.

layout (location = 0) in ivec2 inPos;

// We always apply a 0.5 offset in addition to the drawing offsets, to cover up OpenGL inaccuracies
const vec2 vertexOffsets = vec2(+0.5, -0.5);

void main() {
    // Normalize coords to [0, 2]
    float x = float(inPos.x);
    float y = float(inPos.y);
    float xx = (x + vertexOffsets.x) / 512.0;
    float yy = (y + vertexOffsets.y) / 256.0;

    // Normalize to [-1, 1]
    xx -= 1.0;
    yy -= 1.0;

    gl_Position = vec4(xx, yy, 1.0, 1.0);
}
)";

static const char* const c_frg = R"(
#version 330 core

out vec4 outColor;

void main() {
    outColor = vec4(1.0f, 1.0f, 1.0f, 1.0f);
}
)";

PCSX::GPULogger::GPULogger() : m_listener(g_system->m_eventBus) {
    m_listener.listen<Events::GPU::VSync>([this](auto event) {
        m_frameCounter++;
        if (m_breakOnVSync) {
            g_system->pause();
        }
    });
}

void PCSX::GPULogger::handleFrameChange() {
    if (m_lastGteFrame != m_frameCounter) {
        m_lastGteFrame = m_frameCounter;
        m_gteFrameLog.clear();
        m_lastGteState.reset();
        m_pendingFetches.clear();
    }
}

void PCSX::GPULogger::clearPendingFetches() {
    m_pendingFetches.clear();
}

void PCSX::GPULogger::clearFrameLog() {
    m_list.destroyAll();
    m_gteFrameLog.clear();
    m_lastGteState.reset();
    m_lastGteFrame = m_frameCounter;
    clearPendingFetches();
}

void PCSX::GPULogger::recordGteState(const GTEState& state) {
    if (!m_enabled && !m_logGteStates && !m_logVertexFetches) return;

    handleFrameChange();

    GTEState loggedState = state;
    if (m_logVertexFetches && !m_pendingFetches.empty()) {
        loggedState.metadata.vertexFetches = m_pendingFetches;
    }

    m_pendingFetches.clear();
    m_lastGteState = loggedState;
    if (m_logGteStates || m_logVertexFetches) {
        m_gteFrameLog.push_back(loggedState);
    }
}

void PCSX::GPULogger::recordVertexFetch(const GTEFetchContext& fetch) {
    if (!m_logVertexFetches) return;

    handleFrameChange();
    m_pendingFetches.push_back(fetch);
}

namespace {
struct FrameLogMetadataBlock {
    uint64_t metadataSize = 0;
    uint64_t logEntrySize = 0;
    uint64_t entryCount = 0;
    uint64_t metadataVersion = 1;
};

std::string colorToHex(uint32_t color) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setw(6) << std::setfill('0') << (color & 0xffffff);
    return stream.str();
}

const char* boolString(bool value) { return value ? "true" : "false"; }

PCSX::LogEntry buildLogEntry(const PCSX::GPU::Logged& logged) {
    PCSX::LogEntry entry{};

    entry.frame = static_cast<uint32_t>(logged.frame);
    entry.pc = logged.pc;
    entry.gp0_cmd = logged.words.empty() ? 0 : logged.words.front();
    entry.primitive_type = static_cast<uint16_t>((entry.gp0_cmd >> 24) & 0xff);
    entry.vertex_count = static_cast<uint16_t>(logged.words.size());

    const auto packetCount = std::min<size_t>(12, logged.words.size());
    for (size_t i = 0; i < packetCount; ++i) {
        entry.packet_words[i] = logged.words[i];
    }

    if (logged.gteState) {
        const auto& input = logged.gteState->input;
        const auto& output = logged.gteState->output;

        const auto vertexCount = std::min<size_t>(4, input.vertices.size());
        for (size_t i = 0; i < vertexCount; ++i) {
            entry.vx[i] = input.vertices[i][0];
            entry.vy[i] = input.vertices[i][1];
            entry.vz[i] = input.vertices[i][2];
        }

        const auto screenCount = std::min<size_t>(4, output.screenCoords.size());
        for (size_t i = 0; i < screenCount; ++i) {
            entry.sx[i] = output.screenCoords[i][0];
            entry.sy[i] = output.screenCoords[i][1];
        }

        for (size_t y = 0; y < 3; ++y) {
            for (size_t x = 0; x < 3; ++x) {
                entry.rot[y][x] = input.rotationMatrix[y][x];
            }
        }

        entry.trx = input.translation[0];
        entry.try_ = input.translation[1];
        entry.trz = input.translation[2];

        entry.ofx = input.offsetX;
        entry.ofy = input.offsetY;
        entry.h = input.projectionPlaneDistance;
        entry.dqa = input.depthQueueA;
        entry.dqb = static_cast<int16_t>(input.depthQueueB);
        entry.zsf3 = input.depthScaleFactor3;
        entry.zsf4 = input.depthScaleFactor4;
    }

    return entry;
}

}  // namespace

void PCSX::GPULogger::enable() {
    GLint textureUnits;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &textureUnits);
    if (textureUnits < 5) return;

    if (!m_vbo.exists()) {
        m_vbo.createFixedSize(sizeof(OpenGL::ivec2) * m_vertices.size(), GL_STREAM_DRAW);
    }
    m_vbo.bind();
    if (!m_vao.exists()) {
        m_vao.create();
    }
    m_vao.bind();
    m_vao.setAttributeInt<GLint>(0, 2, sizeof(OpenGL::ivec2), size_t(0));
    m_vao.enableAttribute(0);

    if (!m_program.exists()) {
        m_writtenHeatmapTex.create(1024, 512, GL_R32F);
        m_readHeatmapTex.create(1024, 512, GL_R32F);
        m_writtenHighlightTex.create(1024, 512, GL_R32F);
        m_readHighlightTex.create(1024, 512, GL_R32F);
        if (!m_writtenHeatmapTex.exists()) return;
        if (!m_readHeatmapTex.exists()) return;
        if (!m_writtenHighlightTex.exists()) return;
        if (!m_readHighlightTex.exists()) return;
        m_writtenHeatmapFB.createWithTexture(m_writtenHeatmapTex);
        m_readHeatmapFB.createWithTexture(m_readHeatmapTex);
        m_writtenHighlightFB.createWithTexture(m_writtenHighlightTex);
        m_readHighlightFB.createWithTexture(m_readHighlightTex);
        if (!m_writtenHeatmapFB.exists()) return;
        if (!m_readHeatmapFB.exists()) return;
        if (!m_writtenHighlightFB.exists()) return;
        if (!m_readHighlightFB.exists()) return;

        OpenGL::Shader vs, ps;
        auto status = vs.create(c_vtx, OpenGL::Vertex);
        if (!status.isOk()) return;
        status = ps.create(c_frg, OpenGL::Fragment);
        if (!status.isOk()) return;

        status = m_program.create({vs, ps});
        if (!status.isOk()) return;
    }

    m_hasFramebuffers = true;
}

void PCSX::GPULogger::disable() {
    m_hasFramebuffers = false;
    m_vram.reset();
}

void PCSX::GPULogger::addTri(OpenGL::ivec2& v1, OpenGL::ivec2& v2, OpenGL::ivec2& v3) {
    if ((m_verticesCount + 3) >= m_vertices.size()) flush();
    m_vertices[m_verticesCount++] = v1;
    m_vertices[m_verticesCount++] = v2;
    m_vertices[m_verticesCount++] = v3;
}

void PCSX::GPULogger::flush() {
    if (m_verticesCount == 0) return;
    m_vbo.bufferVertsSub(&m_vertices[0], m_verticesCount);
    OpenGL::draw(OpenGL::Triangles, m_verticesCount);
    m_verticesCount = 0;
}

void PCSX::GPULogger::addNodeInternal(GPU::Logged* node, GPU::Logged::Origin origin, uint32_t value, uint32_t length) {
    auto frame = m_frameCounter;

    bool gotNewFrame = false;
    for (auto i = m_list.begin(); (i != m_list.end()) && (i->frame != frame); i = m_list.begin()) {
        delete &*i;
        gotNewFrame = true;
    }
    if (gotNewFrame) startNewFrame();

    node->origin = origin;
    node->length = length;
    node->sourceAddr = value;
    if (node->words.empty()) node->words.push_back(value);
    node->wordsTruncated = false;
    if (node->words.size() > c_maxLoggedWords) {
        node->words.resize(c_maxLoggedWords);
        node->words.shrink_to_fit();
        node->wordsTruncated = true;
    }
    node->gteState = m_lastGteState;
    node->pc = g_emulator->m_cpu->m_regs.pc;
    node->frame = frame;
    node->generateStatsInfo();
    m_list.push_back(node);

    if (!m_hasFramebuffers) return;

    const auto oldFBO = OpenGL::getDrawFramebuffer();

    m_vbo.bind();
    m_vao.bind();
    m_program.use();
    OpenGL::disableScissor();

    OpenGL::setViewport(m_writtenHeatmapTex.width(), m_writtenHeatmapTex.height());
    m_writtenHeatmapFB.bind(OpenGL::DrawFramebuffer);
    node->getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); }, GPU::Logged::PixelOp::WRITE);
    flush();

    OpenGL::setViewport(m_readHeatmapTex.width(), m_readHeatmapTex.height());
    m_readHeatmapFB.bind(OpenGL::DrawFramebuffer);
    node->getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); }, GPU::Logged::PixelOp::READ);
    flush();

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldFBO);
    g_emulator->m_gpu->setOpenGLContext();
}

bool PCSX::GPULogger::saveFrameLog(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary);
    if (!output.is_open()) return false;

    size_t entryCount = 0;
    for (const auto& logged : m_list) {
        const auto entry = buildLogEntry(logged);
        output.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
        if (!output.good()) return false;
        ++entryCount;
    }

    FrameLogMetadataBlock metadata{};
    metadata.metadataSize = sizeof(metadata);
    metadata.logEntrySize = sizeof(LogEntry);
    metadata.entryCount = entryCount;

    output.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
    if (!output.good()) return false;

    auto metadataTextPath = path;
    metadataTextPath.replace_extension(".txt");

    std::ofstream metadataText(metadataTextPath);
    if (!metadataText.is_open()) return false;

    metadataText << "GPU frame log metadata\n";
    metadataText << "Entry count: " << metadata.entryCount << "\n";
    metadataText << "LogEntry size (bytes): " << metadata.logEntrySize << "\n";
    metadataText << "Metadata block size (bytes): " << metadata.metadataSize << "\n";
    metadataText << "Metadata version: " << metadata.metadataVersion << "\n";
    metadataText << "Notes: Raw structures are written with the host ABI, including any implicit padding;"
                 << " consumers should validate sizes before interpreting fields.";

    return output.good() && metadataText.good();
}

void PCSX::GPULogger::startNewFrame() {
    m_vram = g_emulator->m_gpu->getVRAM(GPU::Ownership::ACQUIRE);
    m_gteFrameLog.clear();
    m_lastGteState.reset();
    m_lastGteFrame = m_frameCounter;
    clearPendingFetches();
}

void PCSX::GPULogger::replay(GPU* gpu) {
    if (m_vram.data()) gpu->partialUpdateVRAM(0, 0, 1024, 512, m_vram.data<uint16_t>());
    for (auto& node : m_list) {
        if (node.enabled) node.execute(gpu);
    }
    gpu->vblank(true);
}

void PCSX::GPULogger::highlight(GPU::Logged* node, bool only) {
    if (!m_hasFramebuffers) return;

    const auto oldFBO = OpenGL::getDrawFramebuffer();

    m_vbo.bind();
    m_vao.bind();
    m_program.use();
    OpenGL::disableScissor();

    OpenGL::setViewport(m_writtenHighlightTex.width(), m_writtenHighlightTex.height());
    m_writtenHighlightFB.bind(OpenGL::DrawFramebuffer);
    OpenGL::setClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    OpenGL::clearColor();
    if (node) {
        node->getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); }, GPU::Logged::PixelOp::WRITE);
    }
    if (!only) {
        for (auto& node : m_list) {
            if (node.highlight) {
                node.getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); },
                                 GPU::Logged::PixelOp::WRITE);
            }
        }
    }
    flush();

    OpenGL::setViewport(m_readHighlightTex.width(), m_readHighlightTex.height());
    m_readHighlightFB.bind(OpenGL::DrawFramebuffer);
    OpenGL::setClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    OpenGL::clearColor();
    if (node) {
        node->getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); }, GPU::Logged::PixelOp::READ);
    }
    if (!only) {
        for (auto& node : m_list) {
            if (node.highlight) {
                node.getVertices([this](auto v1, auto v2, auto v3) { addTri(v1, v2, v3); }, GPU::Logged::PixelOp::READ);
            }
        }
    }
    flush();

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, oldFBO);
    g_emulator->m_gpu->setOpenGLContext();
}

PCSX::GPU::CtrlDisplayMode::CtrlDisplayMode(uint32_t value) {
    if ((value >> 6) & 1) {
        switch (value & 3) {
            case 0:
                hres = HR_368;
                break;
            case 1:
                hres = HR_384;
                break;
            case 2:
                hres = HR_512;
                break;
            case 3:
                hres = HR_640;
                break;
        }
    } else {
        hres = magic_enum::enum_cast<decltype(hres)>(value & 3).value();
    }
    vres = magic_enum::enum_cast<decltype(vres)>((value >> 2) & 1).value();
    mode = magic_enum::enum_cast<decltype(mode)>((value >> 3) & 1).value();
    depth = magic_enum::enum_cast<decltype(depth)>((value >> 4) & 1).value();
    interlace = (value >> 5) & 1;
    widthRaw = ((value >> 6) & 1) | ((value & 3) << 1);
}

void PCSX::GPU::ClearCache::drawLogNode(unsigned, const DrawLogSettings&) {}

void PCSX::GPU::FastFill::drawLogNode(unsigned itemIndex, const DrawLogSettings& settings) {
    drawColorBox(color, itemIndex, 0, settings);
    ImGui::Separator();
    ImGui::Text("  X0: %i, Y0: %i", x, y);
    ImGui::Text("  X1: %i, Y1: %i", x + w, y + h);
    ImGui::Text("  W: %i, H: %i", w, h);
}

void PCSX::GPU::BlitVramVram::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  From X: %i, Y: %i", sX, sY);
    ImGui::Text("  To X: %i, Y: %i", dX, dY);
    ImGui::Text("  W: %i, H: %i", w, h);
}

void PCSX::GPU::BlitRamVram::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  X: %i, Y: %i", x, y);
    ImGui::Text("  W: %i, H: %i", w, h);
}

void PCSX::GPU::BlitVramRam::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  X: %i, Y: %i", x, y);
    ImGui::Text("  W: %i, H: %i", w, h);
}

void PCSX::GPU::TPage::drawLogNodeCommon() {
    ImGui::Text(_("Texture Page X: %i, Texture Page Y: %i"), tx, ty);
    ImGui::TextUnformatted(_("Blending:"));
    ImGui::SameLine();
    switch (blendFunction) {
        case BlendFunction::HalfBackAndHalfFront:
            ImGui::TextUnformatted(_("50% Back + 50% Front"));
            break;
        case BlendFunction::FullBackAndFullFront:
            ImGui::TextUnformatted(_("100% Back + 100% Front"));
            break;
        case BlendFunction::FullBackSubFullFront:
            ImGui::TextUnformatted(_("100% Back - 100% Front"));
            break;
        case BlendFunction::FullBackAndQuarterFront:
            ImGui::TextUnformatted(_("100% Back + 25% Front"));
            break;
    }
    ImGui::TextUnformatted(_("Texture depth:"));
    ImGui::SameLine();
    switch (texDepth) {
        case TexDepth::Tex4Bits:
            ImGui::TextUnformatted(_("4 bits"));
            break;
        case TexDepth::Tex8Bits:
            ImGui::TextUnformatted(_("8 bits"));
            break;
        case TexDepth::Tex16Bits:
            ImGui::TextUnformatted(_("16 bits"));
            break;
    }
}

void PCSX::GPU::TPage::drawLogNode(unsigned, const DrawLogSettings&) {
    drawLogNodeCommon();
    ImGui::Text(_("Dithering: %s"), dither ? _("Yes") : _("No"));
}

void PCSX::GPU::TWindow::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  X: %i, Y: %i", x, y);
    ImGui::Text("  W: %i, H: %i", w, h);
}

void PCSX::GPU::DrawingAreaStart::drawLogNode(unsigned, const DrawLogSettings&) { ImGui::Text("  X: %i, Y: %i", x, y); }

void PCSX::GPU::DrawingAreaEnd::drawLogNode(unsigned, const DrawLogSettings&) { ImGui::Text("  X: %i, Y: %i", x, y); }

void PCSX::GPU::DrawingOffset::drawLogNode(unsigned, const DrawLogSettings&) { ImGui::Text("  X: %i, Y: %i", x, y); }

void PCSX::GPU::MaskBit::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text(_("  Set: %s, Check: %s"), set ? _("Yes") : _("No"), check ? _("Yes") : _("No"));
}

void PCSX::GPU::CtrlReset::drawLogNode(unsigned, const DrawLogSettings&) {}
void PCSX::GPU::CtrlClearFifo::drawLogNode(unsigned, const DrawLogSettings&) {}
void PCSX::GPU::CtrlIrqAck::drawLogNode(unsigned, const DrawLogSettings&) {}

void PCSX::GPU::CtrlDisplayEnable::drawLogNode(unsigned, const DrawLogSettings&) {
    if (enable) {
        ImGui::TextUnformatted(_("Display Enabled"));
    } else {
        ImGui::TextUnformatted(_("Display Disabled"));
    }
}

void PCSX::GPU::CtrlDmaSetting::drawLogNode(unsigned, const DrawLogSettings&) {
    switch (dma) {
        case Dma::Off:
            ImGui::TextUnformatted(_("DMA Off"));
            break;
        case Dma::FifoQuery:
            ImGui::TextUnformatted(_("FIFO Query"));
            break;
        case Dma::Read:
            ImGui::TextUnformatted(_("DMA Read"));
            break;
        case Dma::Write:
            ImGui::TextUnformatted(_("DMA Write"));
            break;
    }
}

void PCSX::GPU::CtrlDisplayStart::drawLogNode(unsigned, const DrawLogSettings&) { ImGui::Text("  X: %i, Y: %i", x, y); }
void PCSX::GPU::CtrlHorizontalDisplayRange::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  X0: %i, X1: %i", x0, x1);
}
void PCSX::GPU::CtrlVerticalDisplayRange::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::Text("  Y0: %i, Y1: %i", y0, y1);
}

void PCSX::GPU::CtrlDisplayMode::drawLogNode(unsigned, const DrawLogSettings&) {
    ImGui::TextUnformatted(_("Horizontal resolution:"));
    ImGui::SameLine();
    switch (hres) {
        case HR_256:
            ImGui::TextUnformatted("256 pixels");
            break;
        case HR_320:
            ImGui::TextUnformatted("320 pixels");
            break;
        case HR_512:
            ImGui::TextUnformatted("512 pixels");
            break;
        case HR_640:
            ImGui::TextUnformatted("640 pixels");
            break;
        case HR_368:
            ImGui::TextUnformatted("368 pixels");
            break;
        case HR_384:
            ImGui::TextUnformatted("384 pixels");
            break;
    }
    ImGui::Text(_("Extended width mode: %s"), widthRaw & 1 ? _("Yes") : _("No"));
    ImGui::TextUnformatted(_("Vertical resolution:"));
    ImGui::SameLine();
    switch (vres) {
        case VR_240:
            ImGui::TextUnformatted("240 pixels");
            break;
        case VR_480:
            ImGui::TextUnformatted("480 pixels");
            break;
    }
    ImGui::Text(_("Output mode: %s"), mode == VM_NTSC ? "NTSC" : "PAL");
    ImGui::Text(_("Display depth: %s"), depth == CD_15BITS ? _("15 bits") : _("24 bits"));
    ImGui::Text(_("Interlaced: %s"), interlace ? _("Yes") : _("No"));
}

void PCSX::GPU::CtrlQuery::drawLogNode(unsigned, const DrawLogSettings&) {
    switch (type()) {
        case QueryType::TextureWindow:
            ImGui::TextUnformatted(_("Texture Window"));
            break;
        case QueryType::DrawAreaStart:
            ImGui::TextUnformatted(_("Draw Area Start"));
            break;
        case QueryType::DrawAreaEnd:
            ImGui::TextUnformatted(_("Draw Area End"));
            break;
        case QueryType::DrawOffset:
            ImGui::TextUnformatted(_("Draw Offset"));
            break;
        default:
            ImGui::TextUnformatted(_("Unknown"));
            break;
    }
}

void PCSX::GPU::FastFill::cumulateStats(GPUStats* stats) { stats->pixelWrites += h * w; }
void PCSX::GPU::BlitVramVram::cumulateStats(GPUStats* stats) {
    auto s = h * w;
    stats->pixelWrites += s;
    stats->pixelReads += s;
}
void PCSX::GPU::BlitRamVram::cumulateStats(GPUStats* stats) { stats->pixelWrites += h * w; }
void PCSX::GPU::BlitVramRam::cumulateStats(GPUStats* stats) { stats->pixelReads += h * w; }

void PCSX::GPU::FastFill::getVertices(AddTri&& add, PixelOp op) {
    if (op == PixelOp::READ) return;
    add({int(x), int(y)}, {int(x + w), int(y)}, {int(x + w), int(y + h)});
    add({int(x + w), int(y + h)}, {int(x), int(y + h)}, {int(x), int(y)});
}
void PCSX::GPU::BlitVramVram::getVertices(AddTri&& add, PixelOp op) {
    if (op == PixelOp::READ) {
        add({int(sX), int(sY)}, {int(sX + w), int(sY)}, {int(sX + w), int(sY + h)});
        add({int(sX + w), int(sY + h)}, {int(sX), int(sY + h)}, {int(sX), int(sY)});
    } else if (op == PixelOp::WRITE) {
        add({int(dX), int(dY)}, {int(dX + w), int(dY)}, {int(dX + w), int(dY + h)});
        add({int(dX + w), int(dY + h)}, {int(dX), int(dY + h)}, {int(dX), int(dY)});
    }
}
void PCSX::GPU::BlitRamVram::getVertices(AddTri&& add, PixelOp op) {
    if (op == PixelOp::READ) return;
    add({int(x), int(y)}, {int(x + w), int(y)}, {int(x + w), int(y + h)});
    add({int(x + w), int(y + h)}, {int(x), int(y + h)}, {int(x), int(y)});
}
void PCSX::GPU::BlitVramRam::getVertices(AddTri&& add, PixelOp op) {
    if (op == PixelOp::WRITE) return;
    add({int(x), int(y)}, {int(x + w), int(y)}, {int(x + w), int(y + h)});
    add({int(x + w), int(y + h)}, {int(x), int(y + h)}, {int(x), int(y)});
}

bool PCSX::GPU::FastFill::writeJsonFields(std::ostream& output) const {
    output << ",\n";
    output << "      \"details\": {\n";
    output << "        \"primitive\": \"fast_fill\",\n";
    output << "        \"color\": \"" << colorToHex(color) << "\",\n";
    output << "        \"rect\": {\"x\": " << x << ", \"y\": " << y << ", \"w\": " << w << ", \"h\": " << h
           << "},\n";
    output << "        \"raw\": {\"x\": " << raw.x << ", \"y\": " << raw.y << ", \"w\": " << raw.w
           << ", \"h\": " << raw.h << "},\n";
    output << "        \"clipped\": " << boolString(clipped) << "\n";
    output << "      }";
    return true;
}

bool PCSX::GPU::BlitVramVram::writeJsonFields(std::ostream& output) const {
    output << ",\n";
    output << "      \"details\": {\n";
    output << "        \"primitive\": \"blit_vram_to_vram\",\n";
    output << "        \"source\": {\"x\": " << sX << ", \"y\": " << sY << ", \"w\": " << w << ", \"h\": " << h
           << "},\n";
    output << "        \"destination\": {\"x\": " << dX << ", \"y\": " << dY << ", \"w\": " << w << ", \"h\": " << h
           << "},\n";
    output << "        \"raw\": {\"sX\": " << raw.sX << ", \"sY\": " << raw.sY << ", \"dX\": " << raw.dX
           << ", \"dY\": " << raw.dY << ", \"w\": " << raw.w << ", \"h\": " << raw.h << "},\n";
    output << "        \"clipped\": " << boolString(clipped) << "\n";
    output << "      }";
    return true;
}

bool PCSX::GPU::BlitRamVram::writeJsonFields(std::ostream& output) const {
    output << ",\n";
    output << "      \"details\": {\n";
    output << "        \"primitive\": \"blit_ram_to_vram\",\n";
    output << "        \"destination\": {\"x\": " << x << ", \"y\": " << y << ", \"w\": " << w << ", \"h\": " << h
           << "},\n";
    output << "        \"raw\": {\"x\": " << raw.x << ", \"y\": " << raw.y << ", \"w\": " << raw.w
           << ", \"h\": " << raw.h << "},\n";
    output << "        \"clipped\": " << boolString(clipped) << ",\n";
    output << "        \"dataBytes\": " << data.size() << "\n";
    output << "      }";
    return true;
}

bool PCSX::GPU::BlitVramRam::writeJsonFields(std::ostream& output) const {
    output << ",\n";
    output << "      \"details\": {\n";
    output << "        \"primitive\": \"blit_vram_to_ram\",\n";
    output << "        \"source\": {\"x\": " << x << ", \"y\": " << y << ", \"w\": " << w << ", \"h\": " << h
           << "},\n";
    output << "        \"raw\": {\"x\": " << raw.x << ", \"y\": " << raw.y << ", \"w\": " << raw.w
           << ", \"h\": " << raw.h << "},\n";
    output << "        \"clipped\": " << boolString(clipped) << "\n";
    output << "      }";
    return true;
}

bool PCSX::GPU::TPage::writeJsonFields(std::ostream& output) const {
    output << ",\n";
    output << "      \"details\": {\n";
    output << "        \"primitive\": \"texture_page\",\n";
    output << "        \"raw\": " << raw << ",\n";
    output << "        \"tx\": " << tx << ",\n";
    output << "        \"ty\": " << ty << ",\n";
    output << "        \"blendFunction\": \"" << PCSX::GPU::blendFunctionToString(blendFunction) << "\",\n";
    output << "        \"depth\": \"" << PCSX::GPU::texDepthToString(texDepth) << "\",\n";
    output << "        \"dither\": " << boolString(dither) << ",\n";
    output << "        \"drawToDisplay\": " << boolString(drawToDisplay) << ",\n";
    output << "        \"textureDisable\": " << boolString(texDisable) << ",\n";
    output << "        \"xflip\": " << boolString(xflip) << ",\n";
    output << "        \"yflip\": " << boolString(yflip) << "\n";
    output << "      }";
    return true;
}

bool PCSX::GPU::TWindow::writeJsonFields(std::ostream& output) const {
    output << ",\n";
    output << "      \"details\": {\n";
    output << "        \"primitive\": \"texture_window\",\n";
    output << "        \"raw\": " << raw << ",\n";
    output << "        \"x\": " << x << ",\n";
    output << "        \"y\": " << y << ",\n";
    output << "        \"w\": " << w << ",\n";
    output << "        \"h\": " << h << "\n";
    output << "      }";
    return true;
}

bool PCSX::GPU::DrawingAreaStart::writeJsonFields(std::ostream& output) const {
    output << ",\n";
    output << "      \"details\": {\n";
    output << "        \"primitive\": \"drawing_area_start\",\n";
    output << "        \"raw\": " << raw << ",\n";
    output << "        \"x\": " << x << ",\n";
    output << "        \"y\": " << y << "\n";
    output << "      }";
    return true;
}

bool PCSX::GPU::DrawingAreaEnd::writeJsonFields(std::ostream& output) const {
    output << ",\n";
    output << "      \"details\": {\n";
    output << "        \"primitive\": \"drawing_area_end\",\n";
    output << "        \"raw\": " << raw << ",\n";
    output << "        \"x\": " << x << ",\n";
    output << "        \"y\": " << y << "\n";
    output << "      }";
    return true;
}

bool PCSX::GPU::DrawingOffset::writeJsonFields(std::ostream& output) const {
    output << ",\n";
    output << "      \"details\": {\n";
    output << "        \"primitive\": \"drawing_offset\",\n";
    output << "        \"raw\": " << raw << ",\n";
    output << "        \"x\": " << x << ",\n";
    output << "        \"y\": " << y << "\n";
    output << "      }";
    return true;
}

bool PCSX::GPU::MaskBit::writeJsonFields(std::ostream& output) const {
    output << ",\n";
    output << "      \"details\": {\n";
    output << "        \"primitive\": \"mask_bit\",\n";
    output << "        \"set\": " << boolString(set) << ",\n";
    output << "        \"check\": " << boolString(check) << "\n";
    output << "      }";
    return true;
}

void PCSX::GPU::Logged::addLine(AddTri&& add, int x1, int y1, int x2, int y2) {
    const int32_t dx = x2 - x1;
    const int32_t dy = y2 - y1;

    const auto absDx = std::abs(dx);
    const auto absDy = std::abs(dy);

    // Both vertices coincide, render 1x1 rectangle with the colour and coords of v1
    if (dx == 0 && dy == 0) {
        add({x1, y1}, {x1 + 1, y1}, {x1 + 1, y1 + 1});
        add({x1 + 1, y1 + 1}, {x1, y1 + 1}, {x1, y1});
    } else {
        int xOffset, yOffset;
        if (absDx > absDy) {  // x-major line
            xOffset = 0;
            yOffset = 1;

            // Align line depending on whether dx is positive or not
            dx > 0 ? x2++ : x1++;
        } else {  // y-major line
            xOffset = 1;
            yOffset = 0;

            // Align line depending on whether dy is positive or not
            dy > 0 ? y2++ : y1++;
        }

        add({x1, y1}, {x2, y2}, {x2 + xOffset, y2 + yOffset});
        add({x2 + xOffset, y2 + yOffset}, {x1 + xOffset, y1 + yOffset}, {x1, y1});
    }
}
