#include "OreUiSelectionHooks.h"

#include "playback/Playback.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/ScreenTechStackSelector.h"
#include "mc/client/gui/TechStack.h"
#include "mc/client/gui/oreui/SceneProvider.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace playback::functions {

namespace {

struct HookState {
    bool techStackSelector{};
    bool sceneProvider{};
};

HookState& hookState() {
    static HookState state;
    return state;
}

std::unordered_map<std::string, ui::TechStack>& observedTechStacks() {
    static std::unordered_map<std::string, ui::TechStack> values;
    return values;
}

std::unordered_map<std::string, bool>& observedScenes() {
    static std::unordered_map<std::string, bool> values;
    return values;
}

char const* techStackName(ui::TechStack stack) {
    switch (stack) {
    case ui::TechStack::JsonUI:
        return "JsonUI";
    case ui::TechStack::OreUI:
        return "OreUI";
    default:
        return "Unknown";
    }
}

void logTechStack(std::string const& screenName, ui::TechStack stack) {
    auto& observed = observedTechStacks();
    auto  it       = observed.find(screenName);
    if (it != observed.end() && it->second == stack) return;

    observed.insert_or_assign(screenName, stack);
    Playback::getInstance().getSelf().getLogger().debug(
        "OreUI selection telemetry: screen='{}', techStack={}",
        screenName,
        techStackName(stack)
    );
}

void logScene(
    std::string const& url,
    OreUI::RouteMode   mode,
    OreUI::FacetRegistryLocation location,
    bool               created
) {
    auto key = url + "|" + std::to_string(static_cast<int>(mode)) + "|" + std::to_string(static_cast<int>(location));
    auto& observed = observedScenes();
    auto  it       = observed.find(key);
    if (it != observed.end() && it->second == created) return;

    observed.insert_or_assign(std::move(key), created);
    Playback::getInstance().getSelf().getLogger().debug(
        "OreUI scene telemetry: url='{}', routeMode={}, location={}, created={}",
        url,
        static_cast<int>(mode),
        static_cast<int>(location),
        created
    );
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackScreenTechStackSelectorHook,
    ll::memory::HookPriority::Normal,
    ui::ScreenTechStackSelector,
    &ui::ScreenTechStackSelector::getTechStackForScreen,
    ui::TechStack,
    std::string const& screenName
) {
    auto result = origin(screenName);
    logTechStack(screenName, result);
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackOreUiSceneProviderHook,
    ll::memory::HookPriority::Normal,
    OreUI::SceneProvider,
    &OreUI::SceneProvider::createScene,
    std::shared_ptr<AbstractScene>,
    std::string const&                                url,
    OreUI::Router&                                    router,
    Bedrock::NotNullNonOwnerPtr<ISceneStack> const&   sceneStack,
    OreUI::RouteMode                                  mode,
    OreUI::FacetRegistryLocation                      location
) {
    auto result = origin(url, router, sceneStack, mode, location);
    logScene(url, mode, location, static_cast<bool>(result));
    return result;
}

bool allInstalled(HookState const& state) { return state.techStackSelector && state.sceneProvider; }

bool noneInstalled(HookState const& state) { return !state.techStackSelector && !state.sceneProvider; }

bool installAll(HookState& state) {
    if (!state.techStackSelector) state.techStackSelector = PlaybackScreenTechStackSelectorHook::hook() == 0;
    if (!state.techStackSelector) return false;
    if (!state.sceneProvider) state.sceneProvider = PlaybackOreUiSceneProviderHook::hook() == 0;
    return state.sceneProvider;
}

bool removeAll(HookState& state) {
    if (state.sceneProvider && PlaybackOreUiSceneProviderHook::unhook()) state.sceneProvider = false;
    if (state.techStackSelector && PlaybackScreenTechStackSelectorHook::unhook()) state.techStackSelector = false;
    return noneInstalled(state);
}

void resetObservedValues() {
    observedTechStacks().clear();
    observedScenes().clear();
}

} // namespace

bool hookOreUiSelectionTelemetry(bool enable) {
    auto& state = hookState();
    auto& logger = Playback::getInstance().getSelf().getLogger();

    if (enable) {
        if (allInstalled(state)) return true;
        if (installAll(state)) {
            logger.debug("OreUI selection telemetry hooks installed");
            return true;
        }

        bool removed = removeAll(state);
        logger.warn(
            "OreUI selection telemetry unavailable (techStackSelector={}, sceneProvider={}, rollback={})",
            state.techStackSelector,
            state.sceneProvider,
            removed
        );
        return false;
    }

    if (noneInstalled(state)) return true;
    if (removeAll(state)) {
        resetObservedValues();
        logger.debug("OreUI selection telemetry hooks removed");
        return true;
    }

    bool restored = installAll(state);
    logger.error(
        "Unable to remove OreUI selection telemetry hooks (techStackSelector={}, sceneProvider={}, restoration={})",
        state.techStackSelector,
        state.sceneProvider,
        restored
    );
    return false;
}

} // namespace playback::functions
