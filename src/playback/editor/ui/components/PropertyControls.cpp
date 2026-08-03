#include "PropertyControls.h"

#include "imgui.h"

#include <algorithm>
#include <string>

namespace playback::editor::ui::property {

namespace {

float rowHeight() {
    return ImGui::GetFontSize() + 12.0f;
}

}

void beginInspector(std::string_view title, std::string_view objectName) {
    float const height = rowHeight();
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    float const width = ImGui::GetContentRegionAvail().x;
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, IM_COL32(37, 37, 37, 255));
    drawList->AddLine({origin.x, origin.y + height}, {origin.x + width, origin.y + height}, IM_COL32(68, 68, 68, 255));
    ImGui::SetCursorScreenPos({origin.x + 10.0f, origin.y + 6.0f});
    ImGui::TextUnformatted(title.data(), title.data() + title.size());
    ImGui::SetCursorScreenPos({origin.x + 10.0f, origin.y + height + 8.0f});
    ImGui::TextDisabled("%s", objectName.empty() ? "No selection" : std::string(objectName).c_str());
    ImGui::SetCursorScreenPos({origin.x, origin.y + height + ImGui::GetFontSize() + 18.0f});
}

void searchBar(char const* id, char const* hint, char* buffer, size_t bufferSize) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight() * 0.5f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(id, hint, buffer, bufferSize);
    ImGui::PopStyleVar();
    ImGui::Spacing();
}

bool beginSection(char const* label, bool defaultOpen) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(45, 45, 45, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(55, 55, 55, 255));
    bool const open = ImGui::TreeNodeEx(label, flags);
    ImGui::PopStyleColor(2);
    return open;
}

void endSection() {
    ImGui::TreePop();
    ImGui::Spacing();
}

void textRow(char const* label, char const* value) {
    float const width = ImGui::GetContentRegionAvail().x;
    float const labelWidth = std::clamp(width * 0.42f, 110.0f, 180.0f);
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(labelWidth);
    ImGui::TextUnformatted(value);
    separator();
}

void separator() {
    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(58, 58, 58, 255));
    ImGui::Separator();
    ImGui::PopStyleColor();
}

bool actionButton(char const* label, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    bool const clicked = ImGui::Button(label, {-1.0f, 0.0f});
    ImGui::EndDisabled();
    return clicked;
}

}
