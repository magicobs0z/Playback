#include "CameraRenderOverride.h"

#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/renderer/game/LevelRenderer.h"
#include "mc/client/renderer/game/LevelRenderPreRenderUpdateParameters.h"
#include "mc/deps/renderer/Camera.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>


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

    auto& renderCamera = client->getCamera();
    auto const orientation = toOrientation(state.yaw, state.pitch);
    renderCamera.mPosition = glm::vec3{state.x, state.y, state.z};
    renderCamera.mRight = orientation * glm::vec3{1.0f, 0.0f, 0.0f};
    renderCamera.mUp = orientation * glm::vec3{0.0f, 1.0f, 0.0f};
    renderCamera.mForward = orientation * glm::vec3{0.0f, 0.0f, 1.0f};
    renderCamera.updateViewMatrixDependencies();
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
