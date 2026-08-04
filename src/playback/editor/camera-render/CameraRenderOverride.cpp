#include "CameraRenderOverride.h"

namespace playback::editor::camera_render {

bool hookCameraRenderOverride(bool enable) {
    return !enable;
}

}
