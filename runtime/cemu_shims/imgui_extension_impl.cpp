// BotW-Native: minimal replacement for Cemu's imgui_extension.cpp (no embedded fonts/overlay).
#include "imgui/imgui_extension.h"

void ImRotateStart() {}
ImVec2 ImRotationCenter() { return ImVec2(0, 0); }
void ImRotateEnd(float, ImVec2) {}
bool ImGui_BeginPadDistinct(const char* name, bool* p_open, ImGuiWindowFlags flags, bool) {
    return ImGui::Begin(name, p_open, flags);
}
void ImGui_PrecacheFonts() {}
void ImGui_ClearFonts() {}
ImFont* ImGui_GetFont(float) { return nullptr; }
void ImGui_UpdateWindowInformation(bool) {}
