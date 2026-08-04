#include "CameraRenderOverride.h"

#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRenderPreRenderUpdateParameters.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/minecraft_camera/components/CameraComponent.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>


namespace playback::editor::camera_render {

namespace {

glm::qua<float> toOrientation(float yaw, float pitch) {
    constexpr float degreesToRadians = glm::pi<float>() / 180.0f;
    return glm::normalize(glm::angleAxis(-yaw * degreesToRadians, glm::vec3{0.0f, 1.0f, 0.0f})
                          * glm::angleAxis(-pitch * degreesToRadians, glm::vec3{1.0f, 0.0f, 0.0f}));
}

void applyOverride() {
    auto const state = functions::ReplaySession::getInstance().snapshotEditorCameraOverride();
    if (!state.active) return;

    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto cameraEntity = client->getCameraEntity();
    auto cameraContext = cameraEntity.lock();
    if (!cameraContext) return;
    auto camera = cameraContext->tryGetComponent<MinecraftCamera::CameraComponent>();
    if (!camera) return;

    camera->mPosition = glm::vec3{state.x, state.y, state.z};
    camera->mOrientation = toOrientation(state.yaw, state.pitch);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackCameraPreRenderUpdateHook,
    ll::memory::HookPriority::Normal,
    LevelRenderer,
    &LevelRenderer::preRenderUpdate,
    void,
    ScreenContext& screenContext,
    LevelRenderPreRenderUpdateParameters& parameters
) {
    origin(screenContext, parameters);
    applyOverride();
}

}

bool hookCameraRenderOverride(bool enable) {
    return enable ? PlaybackCameraPreRenderUpdateHook::hook() == 0 : PlaybackCameraPreRenderUpdateHook::unhook();
}

}
