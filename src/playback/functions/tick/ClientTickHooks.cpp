#include "ClientTickHooks.h"

#include "playback/Playback.h"
#include "playback/editor/ReplayUI.h"
#include "playback/functions/record/ChunkMutationBarrier.h"
#include "playback/functions/record/Recorder.h"
#include "playback/functions/replay/ReplaySession.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/SceneType.h"
#include "mc/client/multiplayer/MultiPlayerLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/FrameUpdateContextBase.h"

namespace playback::functions {

namespace {

void tickPlayback() {
    switch (playback::Playback::getInstance().getMode()) {
    case playback::PlaybackMode::Record:
        Recorder::getInstance().endTick(false);
        break;
    case playback::PlaybackMode::Replay:
        ReplaySession::getInstance().tick();
        break;
    case playback::PlaybackMode::Unknown:
    default:
        break;
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    PlaybackClientUpdateHook,
    ll::memory::HookPriority::Normal,
    ClientInstance,
    &ClientInstance::$update,
    bool,
    bool isInitFinished
) {
    auto  result     = origin(isInitFinished);
    auto& replay     = ReplaySession::getInstance();
    replay.updateControlPlane();
    bool  hudVisible = false;
    if (isInitFinished && replay.isActive()) {
        auto const topScene     = static_cast<unsigned int>(getTopSceneType());
        auto const hudScene     = static_cast<unsigned int>(ui::SceneType::HudScene);
        bool const replayReady  = replay.hasJoinedReplayWorld();
        bool const sceneVisible = (topScene & hudScene) != 0 || replayReady;
        hudVisible = sceneVisible && isInWorldAndNotShowingAnyMenuScreens() && !isShowingLoadingScreen()
                  && !isShowingProgressScreen();
    }
    editor::tickReplayUI(hudVisible);
    replay.tryFinalizeWorldCleanup();
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackLocalPlayerFrameUpdateHook,
    ll::memory::HookPriority::Low,
    LocalPlayer,
    &LocalPlayer::$frameUpdate,
    void,
    FrameUpdateContextBase& context
) {
    // #region debug-point B:frame-update
    ReplaySession::getInstance().debugReportCameraState("LocalPlayer::frameUpdate:before", *this);
    // #endregion
    origin(context);
    auto& replay = ReplaySession::getInstance();
    if (replay.isEditorCameraControlling(*this)) replay.applyEditorCameraOverride();
    // #region debug-point B:frame-update
    replay.debugReportCameraState("LocalPlayer::frameUpdate:after", *this);
    // #endregion
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackLocalPlayerNormalTickHook,
    ll::memory::HookPriority::High,
    LocalPlayer,
    &LocalPlayer::$normalTick,
    void
) {
    // #region debug-point B:normal-tick
    ReplaySession::getInstance().debugReportCameraState("LocalPlayer::normalTick:before", *this);
    // #endregion
    origin();
    // #region debug-point B:normal-tick
    ReplaySession::getInstance().debugReportCameraState("LocalPlayer::normalTick:after", *this);
    // #endregion
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackLocalPlayerTurnHook,
    ll::memory::HookPriority::High,
    LocalPlayer,
    &LocalPlayer::localPlayerTurn,
    void,
    Vec2 const& deltaRot
) {
    if (ReplaySession::getInstance().isEditorCameraControlling(*this)) return;
    origin(deltaRot);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackLocalPlayerApplyTurnDeltaHook,
    ll::memory::HookPriority::High,
    LocalPlayer,
    &LocalPlayer::_applyTurnDelta,
    void,
    Vec2 const& turnOffset
) {
    if (ReplaySession::getInstance().isEditorCameraControlling(*this)) return;
    origin(turnOffset);
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackClientLevelTickHook,
    ll::memory::HookPriority::High,
    MultiPlayerLevel,
    &MultiPlayerLevel::$_subTick,
    void
) {
    ChunkMutationBarrier::setActiveLevel(this);
    origin();
    [[maybe_unused]] auto tickBoundary = ChunkMutationBarrier::enterTickBoundary(*this);
    tickPlayback();
    editor::applyReplayCameraPreview();
}

bool hookClientTick(bool enable) {
    struct HookState {
        bool update{};
        bool levelTick{};
        bool localPlayerFrameUpdate{};
        bool localPlayerNormalTick{};
        bool localPlayerTurn{};
        bool localPlayerApplyTurnDelta{};
    };
    static HookState state;

    auto allInstalled  = [&] {
        return state.update && state.levelTick && state.localPlayerFrameUpdate && state.localPlayerNormalTick && state.localPlayerTurn
            && state.localPlayerApplyTurnDelta;
    };
    auto noneInstalled = [&] {
        return !state.update && !state.levelTick && !state.localPlayerFrameUpdate && !state.localPlayerNormalTick && !state.localPlayerTurn
            && !state.localPlayerApplyTurnDelta;
    };
    auto installAll    = [&] {
        if (!state.update) state.update = PlaybackClientUpdateHook::hook() == 0;
        if (!state.update) return false;
        if (!state.levelTick) state.levelTick = PlaybackClientLevelTickHook::hook() == 0;
        if (!state.levelTick) return false;
        if (!state.localPlayerFrameUpdate) state.localPlayerFrameUpdate = PlaybackLocalPlayerFrameUpdateHook::hook() == 0;
        if (!state.localPlayerFrameUpdate) return false;
        if (!state.localPlayerNormalTick) state.localPlayerNormalTick = PlaybackLocalPlayerNormalTickHook::hook() == 0;
        if (!state.localPlayerNormalTick) return false;
        if (!state.localPlayerTurn) state.localPlayerTurn = PlaybackLocalPlayerTurnHook::hook() == 0;
        if (!state.localPlayerTurn) return false;
        if (!state.localPlayerApplyTurnDelta) {
            state.localPlayerApplyTurnDelta = PlaybackLocalPlayerApplyTurnDeltaHook::hook() == 0;
        }
        return state.localPlayerApplyTurnDelta;
    };
    auto removeAll = [&] {
        if (state.localPlayerApplyTurnDelta && PlaybackLocalPlayerApplyTurnDeltaHook::unhook()) {
            state.localPlayerApplyTurnDelta = false;
        }
        if (state.localPlayerTurn && PlaybackLocalPlayerTurnHook::unhook()) state.localPlayerTurn = false;
        if (state.localPlayerNormalTick && PlaybackLocalPlayerNormalTickHook::unhook()) state.localPlayerNormalTick = false;
        if (state.localPlayerFrameUpdate && PlaybackLocalPlayerFrameUpdateHook::unhook()) {
            state.localPlayerFrameUpdate = false;
        }
        if (state.levelTick && PlaybackClientLevelTickHook::unhook()) state.levelTick = false;
        if (state.update && PlaybackClientUpdateHook::unhook()) state.update = false;
        return noneInstalled();
    };

    if (enable) {
        if (allInstalled()) return true;
        if (installAll()) return true;

        bool removed = removeAll();
        Playback::getInstance().getSelf().getLogger().error(
            "Unable to install client tick hooks (update={}, levelTick={}, frameUpdate={}, normalTick={}, turn={}, applyTurn={}, rollback={})",
            state.update,
            state.levelTick,
            state.localPlayerFrameUpdate,
            state.localPlayerNormalTick,
            state.localPlayerTurn,
            state.localPlayerApplyTurnDelta,
            removed
        );
        return false;
    }

    if (noneInstalled()) return true;
    if (removeAll()) return true;

    bool restored = installAll();
    Playback::getInstance().getSelf().getLogger().error(
        "Unable to remove client tick hooks (update={}, levelTick={}, frameUpdate={}, normalTick={}, turn={}, applyTurn={}, restoration={})",
        state.update,
        state.levelTick,
        state.localPlayerFrameUpdate,
        state.localPlayerNormalTick,
        state.localPlayerTurn,
        state.localPlayerApplyTurnDelta,
        restored
    );
    return false;
}

} // namespace playback::functions
