#pragma once

#include "../Module.hpp"

// Port of the "Totem Tweaks" Fabric mod (net.pathdos.totemtweaks, v1.2.0) to BedrockTools.
//
// Field-for-field mapping back to the Java mod's Gui config class:
//   m_totemScale          <- totemSize            (default 1.0)
//   m_popScale             <- popSize              (default 0.3)
//   m_disableEquipAnimation<- disableEquipAnimation(default false)
//   m_popAnimation          <- TotemPopAnimation    (default true)
//   m_animationSpeed        <- animationSpeed       (default 40)
//   m_lockRotationPosition  <- lockRotationPosition (default false)
//   m_disableRotations      <- disableRotations     (default false)
//   m_staticDepth           <- staticSize           (default false)
//   m_sizeAnimation         <- enableTotemSizeChange(default false)
//   m_totemScaleSpeed       <- totemSizeChangeSpeed (default 1.0)
//   m_minTotemScale         <- minTotemSize         (default 0.5)
//   m_maxTotemScale         <- maxTotemSize         (default 1.0)
//
// NOTE (read before wiring up in ModuleRegistry): the held-item scaling/pulse behaviour
// reuses the SignatureId::RenderItem hook already resolved by ViewModelModule, so that
// part works with this project's existing signature set. The full-screen "totem pop"
// animation (pop scale/rotation/depth/duration/jitter) needs two NEW signatures that do
// not exist yet in Signatures.cpp for 1.26.44 -- see the big comment block at the top of
// totemtweaks.cpp for exactly what to find and why I couldn't generate them myself.
class TotemTweaksModule : public Module {
public:
    TotemTweaksModule();
    ~TotemTweaksModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // -- held totem (main/off hand) --
    bool  m_sizeAnimation        = false; // enableTotemSizeChange
    float m_totemScale           = 1.0f;  // totemSize
    float m_minTotemScale        = 0.5f;  // minTotemSize
    float m_maxTotemScale        = 1.0f;  // maxTotemSize
    float m_totemScaleSpeed      = 1.0f;  // totemSizeChangeSpeed
    bool  m_disableEquipAnimation= false;

    // -- totem pop screen animation --
    bool  m_popAnimation         = true;  // TotemPopAnimation
    int   m_animationSpeed       = 40;    // ticks
    bool  m_lockRotationPosition = false;
    bool  m_disableRotations     = false;
    bool  m_staticDepth          = false; // staticSize
    float m_popScale             = 0.3f;  // popSize

private:
    bool m_renderItemHooked = false;
    bool m_popHooksInstalled = false; // becomes true only once the TODO signatures below resolve
};
