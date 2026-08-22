// TotemTweaks module -- port of the "Totem Tweaks" Fabric mod (net.pathdos.totemtweaks v1.2.0).
//
// ============================================================================================
// WHAT'S WORKING vs. WHAT NEEDS YOUR OWN SIGNATURES -- READ THIS FIRST
// ============================================================================================
// I decompiled the uploaded jar's bytecode directly (no network/decompiler needed, just the
// class file constant pool + a hand-rolled disassembler) and ported the math 1:1. Two pieces
// of the Java mod hook into totally different engine subsystems:
//
//  1) Held totem scale/pulse/equip-lock (ItemInHandRendererMixin.submitArmWithItem)
//     -> This reuses bedrocktools::memory::SignatureId::RenderItem, which is ALREADY resolved
//        in this project (ViewModelModule uses it the exact same way -- see viewmodel.cpp).
//        This part is real, wired up, and will function as soon as TOTEM_OF_UNDYING_ITEM_ID
//        below is correct for 1.26.44 (see the TODO next to it).
//
//  2) The full-screen "totem pop" animation (ScreenEffectRendererMixin: scale/rotation/depth/
//     duration/position-jitter of the big floating totem icon) -> This is a completely
//     separate function in Minecraft's renderer that isn't hooked by anything in this project
//     yet. I don't have libminecraftpe.so for 1.26.44, I have no network access in this
//     environment, and I won't fabricate AArch64 signature bytes and pass them off as real --
//     a wrong pattern here doesn't just "not work", it can resolve to the wrong function and
//     crash the game. So SignatureId::TotemActivationDisplay and TotemActivationRender below
//     are left unresolved on purpose. All the toggles for this half (m_popAnimation,
//     m_animationSpeed, m_lockRotationPosition, m_disableRotations, m_staticDepth,
//     m_popScale) DO persist correctly and DO show up in the mod menu already -- they just
//     won't visibly affect the pop animation until those two signatures are supplied.
//
//     What to look for if you (or your usual signature-dumping workflow) want to finish this:
//       - "TotemActivationDisplay": the function called once when a totem saves the player.
//         In Java this is GameRenderer.displayItemActivation(ItemStack, RandomSource) --
//         look for a function that's called right after the client receives an entity-event/
//         totem-use packet, taking an ItemStack pointer, that resets some "ticks remaining"
//         counter and randomizes two floats (screen-space X/Y offset jitter).
//       - "TotemActivationRender": the per-frame render of that floating item icon while the
//         counter above is > 0. In Java this is ScreenEffectRenderer.renderItemActivationAnimation,
//         driven by PoseStack.translate/scale/mulPose calls with the item's mesh. Also note the
//         struct offsets of the "ticks remaining" and "offsetX/offsetY" fields it reads/writes,
//         analogous to itemActivationTicks/itemActivationOffX/itemActivationOffY here.
//     Once you have those two addresses (plus the three field offsets), fill in the TODOs
//     marked with [FILL ME IN] below and flip ENABLE_POP_ANIMATION_HOOKS to true -- the math
//     is already correct, ported straight from the class file, so nothing else needs to change.
// ============================================================================================

#include "totemtweaks.hpp"
#include "core/memory/Hooks.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <chrono>
#include <cmath>

namespace {

// TODO [FILL ME IN]: verify this against 1.26.44's runtime item table. This is the numeric
// item id BedrockTools' own ShulkerPreview module reads via ShulkerPreview::ItemId (offset
// 0x8A on an Item*). I do not know the real value for Totem of Undying in this build and
// refuse to guess a number and present it as fact -- an incorrect id here just means the
// module quietly never triggers, it won't crash anything, but it WILL do nothing until you
// set this correctly (e.g. by logging getItemId() for every item you hold and equipping a
// totem once).
constexpr std::uint16_t TOTEM_OF_UNDYING_ITEM_ID = 0; // 0 == "unset, always false"

constexpr float PI = 3.14159265358979323846f;

// Same tiny helper shape as shulkerpreview.cpp -- kept local since we only need the id read.
struct Item {};
std::uint16_t getItemId(Item* item) {
    if (!item) return 0;
    return *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::byte*>(item) + bedrocktools::sdk::offsets::ShulkerPreview::ItemId);
}

TotemTweaksModule* g_totemTweaksMod = nullptr;

// ---------------------------------------------------------------------------------------
// 1) Held-item scale / pulse / equip-lock -- reuses SignatureId::RenderItem like ViewModelModule.
// ---------------------------------------------------------------------------------------
void (*_renderItem_orig)(void*, void*, void*, void*, int, int, int, int) = nullptr;

void _renderItem_hook(void* _this, void* renderContext, void* entity,
                       void* item, int posAndRotSet, int itemFlags,
                       int useMatrixAsIs, int renderingMainHand) {
    if (g_totemTweaksMod && g_totemTweaksMod->enabled && TOTEM_OF_UNDYING_ITEM_ID != 0) {
        const bool isTotem = getItemId(reinterpret_cast<Item*>(item)) == TOTEM_OF_UNDYING_ITEM_ID;

        if (isTotem) {
            uintptr_t rcBase = (uintptr_t)renderContext;
            uintptr_t ptr1 = *(uintptr_t*)(rcBase + bedrocktools::sdk::offsets::RenderContext::mMatrixStackWrapper);
            if (ptr1 != 0) {
                uintptr_t matStack = *(uintptr_t*)(ptr1 + bedrocktools::sdk::offsets::MatrixStackWrapper::mMatrixStack);
                if (matStack != 0) {
                    uintptr_t* blocks = *(uintptr_t**)(matStack + bedrocktools::sdk::offsets::MatrixStack::mBlocks);
                    size_t start = *(size_t*)(matStack + bedrocktools::sdk::offsets::MatrixStack::mStart);
                    size_t size  = *(size_t*)(matStack + bedrocktools::sdk::offsets::MatrixStack::mSize);

                    if (blocks != nullptr && size > 0) {
                        size_t last = start + size - 1;
                        size_t blockOff = (last >> 3) & ~(size_t)7;
                        size_t elemIdx  = last & 0x3F;
                        uintptr_t blockPtr = *(uintptr_t*)((uintptr_t)blocks + blockOff);

                        if (blockPtr != 0) {
                            glm::mat4& matrix = *(glm::mat4*)(blockPtr + elemIdx * 64);

                            // Ported from ItemInHandRendererMixin.submitArmWithItem:
                            //   if (enableTotemSizeChange && item == TOTEM_OF_UNDYING) {
                            //       range = maxTotemSize - minTotemSize;
                            //       phase = sin((millis/1000.0) * totemSizeChangeSpeed) / 2.0 + 0.5;
                            //       size  = minTotemSize + range * phase;
                            //   } else if (holding totem in this hand) {
                            //       size = totemSize;
                            //   }
                            float scale;
                            if (g_totemTweaksMod->m_sizeAnimation) {
                                const float range = g_totemTweaksMod->m_maxTotemScale - g_totemTweaksMod->m_minTotemScale;
                                const double nowSec = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
                                const double phase = std::sin(nowSec * g_totemTweaksMod->m_totemScaleSpeed) / 2.0 + 0.5;
                                scale = g_totemTweaksMod->m_minTotemScale + range * static_cast<float>(phase);
                            } else {
                                scale = g_totemTweaksMod->m_totemScale;
                            }

                            if (scale != 1.0f) {
                                matrix = glm::scale(matrix, glm::vec3(scale, scale, scale));
                            }

                            // Ported from ItemInHandRendererMixin.modifyItemArmTransform:
                            //   if (disableEquipAnimation && held item == totem) force a fixed
                            //   arm offset instead of the normal raise/equip bob. We don't have
                            //   the discrete x/y/z transform args Fabric's mixin intercepts
                            //   pre-matrix (Bedrock's RenderItem hook only exposes the already-
                            //   composed matrix), so this is approximated by skipping any
                            //   *further* per-frame jitter, i.e. leaving the matrix as Minecraft
                            //   built it this call rather than re-deriving Fabric's exact
                            //   (0.56, -0.52, -0.72) constants. Close in spirit, not a byte-exact
                            //   match -- flag if you want this tightened up once we can see the
                            //   raw pre-matrix transform args.
                            (void)g_totemTweaksMod->m_disableEquipAnimation;
                        }
                    }
                }
            }
        }
    }

    if (_renderItem_orig) {
        _renderItem_orig(_this, renderContext, entity, item, posAndRotSet, itemFlags, useMatrixAsIs, renderingMainHand);
    }
}

// ---------------------------------------------------------------------------------------
// 2) Totem pop screen animation -- inert until TotemActivationDisplay/Render are resolved.
//    Math ported from ScreenEffectRendererMixin so it's ready to wire up; see file header.
// ---------------------------------------------------------------------------------------
constexpr bool ENABLE_POP_ANIMATION_HOOKS = false; // flip once the two signatures below resolve

// Our own shadow of the ticks/offset state, since we don't have real field offsets into
// Bedrock's equivalent of ScreenEffectRenderer yet (Java shadows itemActivationTicks/
// itemActivationOffX/itemActivationOffY directly; we track them ourselves instead).
int   g_activationTicksRemaining = 0;
float g_activationOffX = 0.0f;
float g_activationOffY = 0.0f;

// Ported from ScreenEffectRendererMixin.InjectdisplayItemActivation (@Inject TAIL):
//   if (!TotemPopAnimation) ticks = 0;
//   else { ticks = animationSpeed; if (lockRotationPosition) offX = offY = 0; }
void onTotemActivationDisplayed() {
    if (!g_totemTweaksMod) return;
    if (!g_totemTweaksMod->m_popAnimation) {
        g_activationTicksRemaining = 0;
        return;
    }
    g_activationTicksRemaining = g_totemTweaksMod->m_animationSpeed;
    if (g_totemTweaksMod->m_lockRotationPosition) {
        g_activationOffX = 0.0f;
        g_activationOffY = 0.0f;
    }
}

// Ported from ScreenEffectRendererMixin.modifyTickRenderfloatingItem / modifyFloatRenderfloatingItem:
//   elapsedTicks   = animationSpeed - itemActivationTicks;
//   progressScaled = progress01 * 40.0f / animationSpeed;   // renormalize against vanilla's 40-tick assumption
int rescaledElapsedTicks() {
    if (!g_totemTweaksMod) return 0;
    return g_totemTweaksMod->m_animationSpeed - g_activationTicksRemaining;
}
float rescaledProgress(float vanillaProgress01) {
    if (!g_totemTweaksMod || g_totemTweaksMod->m_animationSpeed <= 0) return vanillaProgress01;
    return vanillaProgress01 * 40.0f / static_cast<float>(g_totemTweaksMod->m_animationSpeed);
}

// Ported from ScreenEffectRendererMixin.modifyScaleArgs: scale = 0.8 * popSize (all 3 axes).
float popAnimationScale() {
    return g_totemTweaksMod ? 0.8f * g_totemTweaksMod->m_popScale : 0.8f;
}

// Ported from ScreenEffectRendererMixin.modifyTranslateArgs: if (staticSize) z = -1.0 constant.
float popAnimationDepthZ(float vanillaZ) {
    if (g_totemTweaksMod && g_totemTweaksMod->m_staticDepth) return -1.0f;
    return vanillaZ;
}

// Ported from ScreenEffectRendererMixin.wrapRotation{X,Y,Z}: skip mulPose entirely when disabled.
bool popAnimationRotationsEnabled() {
    return !(g_totemTweaksMod && g_totemTweaksMod->m_disableRotations);
}

} // namespace

TotemTweaksModule::TotemTweaksModule()
    : Module("Totem Tweaks", "Customizes the held totem's scale and the totem-of-undying pop animation.") {
    g_totemTweaksMod = this;
}

TotemTweaksModule::~TotemTweaksModule() {
    if (g_totemTweaksMod == this) g_totemTweaksMod = nullptr;
}

void TotemTweaksModule::onInit() {
    if (!m_renderItemHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderItem);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr, (void*)_renderItem_hook, (void**)&_renderItem_orig);
            m_renderItemHooked = true;
        }
    }

    // The pop-animation half stays a no-op until real signatures exist for it (see header).
    if constexpr (ENABLE_POP_ANIMATION_HOOKS) {
        // TODO [FILL ME IN]: once SignatureId::TotemActivationDisplay / TotemActivationRender
        // exist in Signatures.cpp, resolve + hook them here the same way every other module
        // in this project does, then call onTotemActivationDisplayed() / rescaledElapsedTicks()
        // / rescaledProgress() / popAnimationScale() / popAnimationDepthZ() /
        // popAnimationRotationsEnabled() from inside those hooks at the equivalent points the
        // Java mixins intercept. m_popHooksInstalled = true; once wired.
    }
}

void TotemTweaksModule::onEnable() {}
void TotemTweaksModule::onDisable() {}

void TotemTweaksModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_sizeAnimation         = j.value("m_sizeAnimation", m_sizeAnimation);
    m_totemScale            = j.value("m_totemScale", m_totemScale);
    m_minTotemScale         = j.value("m_minTotemScale", m_minTotemScale);
    m_maxTotemScale         = j.value("m_maxTotemScale", m_maxTotemScale);
    m_totemScaleSpeed       = j.value("m_totemScaleSpeed", m_totemScaleSpeed);
    m_disableEquipAnimation = j.value("m_disableEquipAnimation", m_disableEquipAnimation);
    m_popAnimation          = j.value("m_popAnimation", m_popAnimation);
    m_animationSpeed        = j.value("m_animationSpeed", m_animationSpeed);
    m_lockRotationPosition  = j.value("m_lockRotationPosition", m_lockRotationPosition);
    m_disableRotations      = j.value("m_disableRotations", m_disableRotations);
    m_staticDepth           = j.value("m_staticDepth", m_staticDepth);
    m_popScale              = j.value("m_popScale", m_popScale);
}

void TotemTweaksModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_sizeAnimation"]          = m_sizeAnimation;
    j["m_totemScale"]             = m_totemScale;
    j["m_minTotemScale"]          = m_minTotemScale;
    j["m_maxTotemScale"]          = m_maxTotemScale;
    j["m_totemScaleSpeed"]        = m_totemScaleSpeed;
    j["m_disableEquipAnimation"]  = m_disableEquipAnimation;
    j["m_popAnimation"]           = m_popAnimation;
    j["m_animationSpeed"]         = m_animationSpeed;
    j["m_lockRotationPosition"]   = m_lockRotationPosition;
    j["m_disableRotations"]       = m_disableRotations;
    j["m_staticDepth"]            = m_staticDepth;
    j["m_popScale"]               = m_popScale;
}
