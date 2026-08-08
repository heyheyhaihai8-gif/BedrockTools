#include "macewave.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <core/memory/Hooks.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

namespace {
using TessellatorBeginFn = void (*)(void*, void*, int, int, int);
using TessellatorColorFn = void (*)(void*, float, float, float, float);
using TessellatorVertexFn = void (*)(void*, float, float, float);
using RenderMeshFn = void (*)(void*, void*, void*, char*);

struct HashedString {
    uint64_t mStrHash = 0;
    std::string mStr;
    mutable const HashedString* mLastMatch = nullptr;

    explicit HashedString(const char* str) : mStr(str ? str : "") {
        constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t hash = kOffset;
        for (char ch : mStr)
            hash = static_cast<uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        mStrHash = mStr.empty() ? 0 : hash;
    }
};

struct MaterialPtr { void* sharedPtrData[2]; };

static MaceWaveModule* g_module = nullptr;
static TessellatorBeginFn s_tessBegin = nullptr;
static TessellatorColorFn s_tessColor = nullptr;
static TessellatorVertexFn s_tessVertex = nullptr;
static RenderMeshFn s_renderMesh = nullptr;
static uintptr_t s_renderMaterialGroup = 0;
static MaterialPtr* s_selectionMaterial = nullptr;
static void (*s_renderLevelOrig)(void*, void*, void*) = nullptr;

static uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            uintptr_t page = ((uintptr_t)&insns[i] & ~0xFFFULL)
                + ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);

            for (size_t j = i + 1; j < count; ++j) {
                uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }

        if ((insn & 0x9F000000) == 0x10000000) {
            int64_t imm = (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

static MaterialPtr* getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return nullptr;
    HashedString hs(name);
    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[2]) return nullptr;
    using GetMaterialFn = MaterialPtr* (*)(void*, const HashedString*);
    return reinterpret_cast<GetMaterialFn>(vtable[2])((void*)s_renderMaterialGroup, &hs);
}

static void ensureMaterial() {
    if (!s_selectionMaterial && s_renderMaterialGroup)
        s_selectionMaterial = getMaterial("selection_box");
}

static void emitVertex(TessellatorVertexFn fn, void* tess, const bedrocktools::sdk::Vec3& p,
                       float camX, float camY, float camZ) {
    fn(tess, p.x - camX, p.y - camY, p.z - camZ);
}

static void renderWave(const MaceWaveModule::Wave& wave, void* screenContext, void* renderer) {
    if (!g_module || !g_module->enabled || !s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh)
        return;
    if (!screenContext || !renderer) return;

    uintptr_t tessPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessPtr || tessPtr < 0x1000) return;
    void* tess = (void*)tessPtr;

    uintptr_t lrpPtr = *(uintptr_t*)((uintptr_t)renderer + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;

    const float camX = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    const float camY = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    const float camZ = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    ensureMaterial();
    void* material = s_selectionMaterial
        ? (void*)s_selectionMaterial
        : (void*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);
    if (!material) return;

    float progress = std::clamp(wave.age / std::max(0.01f, g_module->duration), 0.0f, 1.0f);
    // Smooth expansion, then a quick fade.
    float eased = 1.0f - std::pow(1.0f - progress, 2.2f);
    float radius = 0.25f + (g_module->maxRadius - 0.25f) * eased;
    float ringWidth = g_module->thickness * (1.0f - progress * 0.35f);

    uint32_t color = g_module->waveColor;
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8) & 0xFF) / 255.0f;
    float b = (color & 0xFF) / 255.0f;
    float baseA = ((color >> 24) & 0xFF) / 255.0f;
    float alpha = baseA * (1.0f - progress) * (1.0f - progress * 0.15f);

    const int segments = std::clamp(g_module->segments, 16, 128);
    s_tessBegin(tess, nullptr, 1, segments * 4, 0);
    s_tessColor(tess, r, g, b, alpha);

    constexpr float twoPi = 6.2831853071795864769f;
    for (int i = 0; i < segments; ++i) {
        float a0 = twoPi * (float)i / (float)segments;
        float a1 = twoPi * (float)(i + 1) / (float)segments;

        float c0 = std::cos(a0), s0 = std::sin(a0);
        float c1 = std::cos(a1), s1 = std::sin(a1);

        bedrocktools::sdk::Vec3 p0{wave.pos.x + c0 * (radius - ringWidth), wave.pos.y + g_module->height, wave.pos.z + s0 * (radius - ringWidth)};
        bedrocktools::sdk::Vec3 p1{wave.pos.x + c1 * (radius - ringWidth), wave.pos.y + g_module->height, wave.pos.z + s1 * (radius - ringWidth)};
        bedrocktools::sdk::Vec3 p2{wave.pos.x + c1 * radius, wave.pos.y + g_module->height, wave.pos.z + s1 * radius};
        bedrocktools::sdk::Vec3 p3{wave.pos.x + c0 * radius, wave.pos.y + g_module->height, wave.pos.z + s0 * radius};

        emitVertex(s_tessVertex, tess, p0, camX, camY, camZ);
        emitVertex(s_tessVertex, tess, p1, camX, camY, camZ);
        emitVertex(s_tessVertex, tess, p2, camX, camY, camZ);
        emitVertex(s_tessVertex, tess, p3, camX, camY, camZ);
    }

    char pad[0x58]{};
    s_renderMesh(screenContext, tess, material, pad);
}

static void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (s_renderLevelOrig) s_renderLevelOrig(self, screenContext, a3);
    if (!g_module || !g_module->enabled) return;

    // Draw after the normal world render so the wave sits cleanly on top of the terrain.
    for (const auto& wave : g_module->m_waves)
        renderWave(wave, screenContext, self);
}

} // namespace

MaceWaveModule::MaceWaveModule()
    : Module("MaceWave", "Replaces attack particles with an expanding visual shockwave.") {
    showInMenu = true;
    g_module = this;
}

MaceWaveModule::~MaceWaveModule() {
    if (g_module == this) g_module = nullptr;
}

void MaceWaveModule::onInit() {
    uintptr_t renderLevel = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (renderLevel) m_renderLevelTarget = (void*)renderLevel;

    if (uintptr_t a = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin)) {
        m_tessBeginAddr = (void*)a; s_tessBegin = (TessellatorBeginFn)a;
    }
    if (uintptr_t a = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor)) {
        m_tessColorAddr = (void*)a; s_tessColor = (TessellatorColorFn)a;
    }
    if (uintptr_t a = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex)) {
        m_tessVertexAddr = (void*)a; s_tessVertex = (TessellatorVertexFn)a;
    }

    uintptr_t mesh = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!mesh) mesh = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
    if (mesh) {
        m_renderMeshAddr = (void*)mesh;
        s_renderMesh = (RenderMeshFn)mesh;
    }

    uintptr_t materialGroup = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (materialGroup) {
        m_renderMaterialGroupAddr = (void*)materialGroup;
        uintptr_t groupAddr = resolveADRP(reinterpret_cast<uint32_t*>(materialGroup), 2, 0);
        if (groupAddr)
            s_renderMaterialGroup = groupAddr + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>([](auto& event) {
        if (!g_module || !g_module->enabled || !event.target) return;
        g_module->addWave(event.target);
    });
}

void MaceWaveModule::applyPatch() {
    if (m_patched || !m_renderLevelTarget) return;
    bedrocktools::hooks::install(m_renderLevelTarget, (void*)renderLevelHook, (void**)&s_renderLevelOrig);
    m_patched = true;
}

void MaceWaveModule::onEnable() {
    applyPatch();
    m_waves.clear();
}

void MaceWaveModule::onDisable() {
    m_waves.clear();
}

void MaceWaveModule::onFrame() {
    if (!enabled) return;
    constexpr float dt = 1.0f / 60.0f;
    for (auto& wave : m_waves) wave.age += dt;
    m_waves.erase(std::remove_if(m_waves.begin(), m_waves.end(), [this](const Wave& w) {
        return w.age >= duration;
    }), m_waves.end());
}

void MaceWaveModule::addWave(const bedrocktools::sdk::Actor* target) {
    if (!target) return;

    bedrocktools::sdk::Vec3 p = target->position();
    auto bounds = target->bounds();
    // Actor position is normally at the feet; use the lower AABB when available.
    if (std::isfinite(bounds.min.y) && std::isfinite(bounds.max.y) && bounds.max.y >= bounds.min.y)
        p.y = bounds.min.y;

    m_waves.push_back({p, 0.0f});
    if (m_waves.size() > 16)
        m_waves.erase(m_waves.begin(), m_waves.begin() + (m_waves.size() - 16));
}

void MaceWaveModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    duration = j.value("duration", duration);
    maxRadius = j.value("maxRadius", maxRadius);
    thickness = j.value("thickness", thickness);
    segments = j.value("segments", segments);
    height = j.value("height", height);
    triggerOnAnyAttack = j.value("triggerOnAnyAttack", triggerOnAnyAttack);
    if (j.contains("waveColor")) {
        std::string s = j["waveColor"].get<std::string>();
        if (!s.empty() && s[0] == '#') s.erase(0, 1);
        try { waveColor = static_cast<uint32_t>(std::stoul(s, nullptr, 16)); } catch (...) {}
    }
}

void MaceWaveModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["duration"] = duration;
    j["maxRadius"] = maxRadius;
    j["thickness"] = thickness;
    j["segments"] = segments;
    j["height"] = height;
    j["triggerOnAnyAttack"] = triggerOnAnyAttack;
    char color[12];
    std::snprintf(color, sizeof(color), "#%08X", waveColor);
    j["waveColor"] = std::string(color);
}
