#include "OreUiSelectionHooks.h"

#include "playback/Playback.h"
#include "playback/utils/PathUtils.h"

#include "ll/api/memory/Hook.h"

#include "mc/client/gui/ScreenTechStackSelector.h"
#include "mc/client/gui/TechStack.h"
#include "mc/client/gui/oreui/SceneProvider.h"
#include "mc/client/gui/oreui/routing/Router.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace playback::functions {

namespace {

struct HookState {
    bool techStackSelector{};
    bool sceneProvider{};
    bool routerChange{};
    bool routerPush{};
    bool routerReplace{};
    bool routerBack{};
};

HookState& hookState() {
    static HookState state;
    return state;
}

std::mutex& telemetryFileMutex() {
    static std::mutex mutex;
    return mutex;
}

bool& telemetryFileWriteFailureLogged() {
    static bool value{};
    return value;
}

bool appendTelemetryLine(std::string_view line) {
    std::lock_guard lock(telemetryFileMutex());

    std::error_code ec;
    auto const      directory = utils::PathUtils::getTelemetryDir();
    std::filesystem::create_directories(directory, ec);
    if (ec) return false;

    std::ofstream output(directory / "oreui-selection.txt", std::ios::out | std::ios::app | std::ios::binary);
    if (!output.is_open()) return false;

    output << line << '\n';
    output.flush();
    return static_cast<bool>(output);
}

void writeTelemetryLine(std::string line) {
    if (appendTelemetryLine(line)) return;
    if (telemetryFileWriteFailureLogged()) return;

    telemetryFileWriteFailureLogged() = true;
    Playback::getInstance().getSelf().getLogger().warn("Unable to append OreUI selection telemetry file");
}

std::unordered_map<std::string, ui::TechStack>& observedTechStacks() {
    static std::unordered_map<std::string, ui::TechStack> values;
    return values;
}

std::unordered_map<std::string, bool>& observedScenes() {
    static std::unordered_map<std::string, bool> values;
    return values;
}

std::unordered_map<std::string, bool>& observedRoutes() {
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
    writeTelemetryLine("tech_stack\tscreen=" + screenName + "\tstack=" + techStackName(stack));
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
    writeTelemetryLine(
        "scene\turl=" + url + "\troute_mode=" + std::to_string(static_cast<int>(mode)) + "\tlocation="
        + std::to_string(static_cast<int>(location)) + "\tcreated=" + (created ? "true" : "false")
    );
}

std::string describeLocation(std::optional<OreUI::RouterLocation> const& location) {
    if (!location) return "none";

    return "path=" + location->getPath() + "\tquery=" + location->getQuery() + "\tfragment="
         + location->getFragment();
}

void logRoute(std::string line) {
    auto& observed = observedRoutes();
    if (observed.contains(line)) return;

    observed.insert_or_assign(line, true);
    Playback::getInstance().getSelf().getLogger().debug("OreUI router telemetry: {}", line);
    writeTelemetryLine(std::move(line));
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

LL_TYPE_INSTANCE_HOOK(
    PlaybackOreUiRouterChangeHook,
    ll::memory::HookPriority::Normal,
    OreUI::Router,
    &OreUI::Router::_onChange,
    void,
    std::optional<OreUI::RouterLocation> const& oldLocation,
    std::optional<OreUI::RouterLocation> const& currentLocation
) {
    origin(oldLocation, currentLocation);
    logRoute(
        "route_change\told_" + describeLocation(oldLocation) + "\tcurrent_" + describeLocation(currentLocation)
    );
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackOreUiRouterPushHook,
    ll::memory::HookPriority::Normal,
    OreUI::Router,
    &OreUI::Router::_pushRoute,
    bool,
    std::string const& route,
    OreUI::Router::RouterPushMode mode
) {
    auto result = origin(route, mode);
    logRoute(
        "route_push\troute=" + route + "\tpush_mode=" + std::to_string(static_cast<int>(mode)) + "\tsuccess="
        + (result ? "true" : "false")
    );
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackOreUiRouterReplaceHook,
    ll::memory::HookPriority::Normal,
    OreUI::Router,
    &OreUI::Router::replaceRoute,
    bool,
    std::string const& route
) {
    auto result = origin(route);
    logRoute("route_replace\troute=" + route + "\tsuccess=" + (result ? "true" : "false"));
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    PlaybackOreUiRouterBackHook,
    ll::memory::HookPriority::Normal,
    OreUI::Router,
    &OreUI::Router::goBack,
    void
) {
    origin();
    logRoute("route_back");
}

bool allInstalled(HookState const& state) {
    return state.techStackSelector && state.sceneProvider && state.routerChange && state.routerPush
        && state.routerReplace && state.routerBack;
}

bool noneInstalled(HookState const& state) {
    return !state.techStackSelector && !state.sceneProvider && !state.routerChange && !state.routerPush
        && !state.routerReplace && !state.routerBack;
}

bool installAll(HookState& state) {
    if (!state.techStackSelector) state.techStackSelector = PlaybackScreenTechStackSelectorHook::hook() == 0;
    if (!state.techStackSelector) return false;
    if (!state.sceneProvider) state.sceneProvider = PlaybackOreUiSceneProviderHook::hook() == 0;
    if (!state.sceneProvider) return false;
    if (!state.routerChange) state.routerChange = PlaybackOreUiRouterChangeHook::hook() == 0;
    if (!state.routerChange) return false;
    if (!state.routerPush) state.routerPush = PlaybackOreUiRouterPushHook::hook() == 0;
    if (!state.routerPush) return false;
    if (!state.routerReplace) state.routerReplace = PlaybackOreUiRouterReplaceHook::hook() == 0;
    if (!state.routerReplace) return false;
    if (!state.routerBack) state.routerBack = PlaybackOreUiRouterBackHook::hook() == 0;
    return state.routerBack;
}

bool removeAll(HookState& state) {
    if (state.routerBack && PlaybackOreUiRouterBackHook::unhook()) state.routerBack = false;
    if (state.routerReplace && PlaybackOreUiRouterReplaceHook::unhook()) state.routerReplace = false;
    if (state.routerPush && PlaybackOreUiRouterPushHook::unhook()) state.routerPush = false;
    if (state.routerChange && PlaybackOreUiRouterChangeHook::unhook()) state.routerChange = false;
    if (state.sceneProvider && PlaybackOreUiSceneProviderHook::unhook()) state.sceneProvider = false;
    if (state.techStackSelector && PlaybackScreenTechStackSelectorHook::unhook()) state.techStackSelector = false;
    return noneInstalled(state);
}

void resetObservedValues() {
    observedTechStacks().clear();
    observedScenes().clear();
    observedRoutes().clear();
}

} // namespace

bool hookOreUiSelectionTelemetry(bool enable) {
    auto& state = hookState();
    auto& logger = Playback::getInstance().getSelf().getLogger();

    if (enable) {
        if (allInstalled(state)) return true;
        if (installAll(state)) {
            logger.debug("OreUI selection and router telemetry hooks installed");
            writeTelemetryLine("status\thooks=installed");
            return true;
        }

        bool removed = removeAll(state);
        logger.warn(
            "OreUI selection and router telemetry unavailable (techStackSelector={}, sceneProvider={}, routerChange={}, routerPush={}, routerReplace={}, routerBack={}, rollback={})",
            state.techStackSelector,
            state.sceneProvider,
            state.routerChange,
            state.routerPush,
            state.routerReplace,
            state.routerBack,
            removed
        );
        writeTelemetryLine(
            "status\thooks=unavailable\ttech_stack_selector=" + std::to_string(state.techStackSelector)
            + "\tscene_provider=" + std::to_string(state.sceneProvider) + "\trouter_change="
            + std::to_string(state.routerChange) + "\trouter_push=" + std::to_string(state.routerPush)
            + "\trouter_replace=" + std::to_string(state.routerReplace) + "\trouter_back="
            + std::to_string(state.routerBack) + "\trollback=" + std::to_string(removed)
        );
        return false;
    }

    if (noneInstalled(state)) return true;
    if (removeAll(state)) {
        resetObservedValues();
        logger.debug("OreUI selection and router telemetry hooks removed");
        writeTelemetryLine("status\thooks=removed");
        return true;
    }

    bool restored = installAll(state);
    logger.error(
        "Unable to remove OreUI selection and router telemetry hooks (techStackSelector={}, sceneProvider={}, routerChange={}, routerPush={}, routerReplace={}, routerBack={}, restoration={})",
        state.techStackSelector,
        state.sceneProvider,
        state.routerChange,
        state.routerPush,
        state.routerReplace,
        state.routerBack,
        restored
    );
    return false;
}

} // namespace playback::functions
