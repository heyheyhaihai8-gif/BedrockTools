#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <vector>
#include <cstdint>

class MaceWaveModule : public Module {
public:
    MaceWaveModule();
    ~MaceWaveModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    uint32_t waveColor = 0xFFFFFFFF;
    float duration = 0.60f;
    float maxRadius = 4.0f;
    float thickness = 0.16f;
    int segments = 64;
    float height = 0.035f;
    bool triggerOnAnyAttack = true;

public:
    struct Wave {
        bedrocktools::sdk::Vec3 pos{};
        float age = 0.0f;
    };

    std::vector<Wave> m_waves;

private:
    bool m_patched = false;
    void* m_renderLevelTarget = nullptr;

    void* m_tessBeginAddr = nullptr;
    void* m_tessColorAddr = nullptr;
    void* m_tessVertexAddr = nullptr;
    void* m_renderMeshAddr = nullptr;
    void* m_renderMaterialGroupAddr = nullptr;

    void addWave(const bedrocktools::sdk::Actor* target);
    void applyPatch();
};
