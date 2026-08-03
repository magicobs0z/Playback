#include "CameraSampler.h"

#include <algorithm>
#include <cmath>

namespace playback::editor::camera_motion {
namespace {
using editing::model::CameraKeyframe;
using editing::model::CameraPathType;
using editing::model::EasingType;
using editing::model::Vec3;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFullTurnDegrees = 360.0f;
constexpr float kHalfTurnDegrees = 180.0f;

float interpolate(float from, float to, float ratio) { return from + (to - from) * ratio; }
Vec3 interpolate(Vec3 from, Vec3 to, float ratio) { return {interpolate(from.x, to.x, ratio), interpolate(from.y, to.y, ratio), interpolate(from.z, to.z, ratio)}; }
Vec3 add(Vec3 left, Vec3 right) { return {left.x + right.x, left.y + right.y, left.z + right.z}; }
Vec3 scale(Vec3 value, float factor) { return {value.x * factor, value.y * factor, value.z * factor}; }
float shortestAngle(float from, float to, float ratio) { float delta = std::fmod(to - from + kHalfTurnDegrees, kFullTurnDegrees); if (delta < 0.0f) delta += kFullTurnDegrees; return from + (delta - kHalfTurnDegrees) * ratio; }
float cubic(float value, float p1, float p2) { const float inverse = 1.0f - value; return 3.0f * inverse * inverse * value * p1 + 3.0f * inverse * value * value * p2 + value * value * value; }
float cubicDerivative(float value, float p1, float p2) { const float inverse = 1.0f - value; return 3.0f * inverse * inverse * p1 + 6.0f * inverse * value * (p2 - p1) + 3.0f * value * value * (1.0f - p2); }
float ease(const CameraKeyframe& key, float value) {
    switch (key.easingType) {
    case EasingType::EaseIn: return value * value;
    case EasingType::EaseOut: return 1.0f - (1.0f - value) * (1.0f - value);
    case EasingType::EaseInOut: return value < 0.5f ? 2.0f * value * value : 1.0f - std::pow(-2.0f * value + 2.0f, 2.0f) / 2.0f;
    case EasingType::CubicBezier: {
        const float p1 = std::clamp(key.bezierCtrl1.x, 0.0f, 1.0f);
        const float p2 = std::clamp(key.bezierCtrl2.x, 0.0f, 1.0f);
        float parameter = value;
        for (int index = 0; index < 5; ++index) {
            const float derivative = cubicDerivative(parameter, p1, p2);
            if (std::abs(derivative) < 1.0e-5f) break;
            parameter = std::clamp(parameter - (cubic(parameter, p1, p2) - value) / derivative, 0.0f, 1.0f);
        }
        return cubic(parameter, key.bezierCtrl1.y, key.bezierCtrl2.y);
    }
    case EasingType::Linear: return value;
    }
    return value;
}
Vec3 cubicBezier(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float value) {
    const auto a = interpolate(p0, p1, value);
    const auto b = interpolate(p1, p2, value);
    const auto c = interpolate(p2, p3, value);
    return interpolate(interpolate(a, b, value), interpolate(b, c, value), value);
}
Vec3 autoControl(const std::vector<CameraKeyframe>& keys, size_t index, bool outgoing) {
    if (index == 0 || index + 1 >= keys.size()) return {};
    const auto tangent = scale(add(keys[index + 1].position, scale(keys[index - 1].position, -1.0f)), 1.0f / 6.0f);
    return outgoing ? tangent : scale(tangent, -1.0f);
}
}
CameraSample CameraSampler::sampleAt(const editing::model::CameraEntity& camera, int tick) {
    CameraSample sample; sample.source = camera.name;
    if (camera.kind == editing::model::CameraKind::Keyframe) {
        if (camera.keys.empty()) { sample.valid = false; return sample; }
        const auto& keys = camera.keys;
        const auto upper = std::upper_bound(keys.begin(), keys.end(), tick, [](int value, const auto& key) { return value < key.tick; });
        const auto& a = upper == keys.begin() ? keys.front() : *(upper - 1);
        const auto& b = upper == keys.end() ? keys.back() : *upper;
        if (&a == &b) { sample.position = a.position; sample.rotation = {a.yaw, a.pitch}; sample.fov = a.fov; return sample; }
        const float ratio = std::clamp(static_cast<float>(tick - a.tick) / static_cast<float>(b.tick - a.tick), 0.0f, 1.0f);
        const float eased = ease(a, ratio);
        if (a.outgoingMotion.pathType == CameraPathType::CubicBezier) sample.position = cubicBezier(a.position, add(a.position, a.outgoingMotion.outControl), add(b.position, b.outgoingMotion.inControl), b.position, eased);
        else if (a.outgoingMotion.pathType == CameraPathType::AutoSmooth && keys.size() > 2) {
            const size_t aIndex = static_cast<size_t>(std::distance(keys.begin(), upper) - 1);
            const size_t bIndex = aIndex + 1;
            sample.position = cubicBezier(a.position, add(a.position, autoControl(keys, aIndex, true)), add(b.position, autoControl(keys, bIndex, false)), b.position, eased);
        } else sample.position = interpolate(a.position, b.position, eased);
        sample.rotation = {shortestAngle(a.yaw, b.yaw, eased), interpolate(a.pitch, b.pitch, eased)};
        sample.fov = interpolate(a.fov, b.fov, eased) + std::sin(kPi * eased) * a.outgoingMotion.fovPeakOffset;
    }
    else if (camera.kind == editing::model::CameraKind::Path && camera.path) { sample.rotation = camera.path->defaultRotation; sample.fov = camera.path->defaultFov; if (!camera.path->points.empty()) { auto point = std::find_if(camera.path->points.rbegin(), camera.path->points.rend(), [tick](const auto& value) { return value.tick <= tick; }); sample.position = point == camera.path->points.rend() ? camera.path->points.front().position : point->position; } }
    else if (camera.kind == editing::model::CameraKind::Rig && camera.rig) { sample.position = camera.rig->basePosition; sample.rotation = camera.rig->baseRotation; sample.fov = camera.rig->baseFov; }
    else if (camera.kind == editing::model::CameraKind::Preset && camera.preset) { sample.position = camera.preset->offset; sample.rotation = camera.preset->rotation; sample.fov = camera.preset->fov; }
    return sample;
}
}
