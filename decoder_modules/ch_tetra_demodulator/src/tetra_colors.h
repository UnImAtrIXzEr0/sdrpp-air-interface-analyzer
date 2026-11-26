#pragma once
// Theme-aware text colours for the TETRA plugin: the bright signal colours
// used on dark themes become dark, saturated variants on light themes
// (e.g. "RoSc"), so values stay readable with high contrast.
#include <imgui.h>

namespace tc {
    inline bool light() {
        const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        return (0.299f * bg.x + 0.587f * bg.y + 0.114f * bg.z) > 0.5f;
    }
    inline ImVec4 pick(const ImVec4& dark, const ImVec4& lightCol) { return light() ? lightCol : dark; }

    // Optional value colours of the selected theme (keys "ValueText", "ValueGood",
    // "ValueWarn", "ValueBad"); alpha 0 = not set, the built-in colours apply.
    // Filled once per frame by the plugin from the theme manager (setThemeValues).
    struct ThemeValues { ImVec4 text, good, warn, bad; };
    inline ThemeValues& themeValues() { static ThemeValues t = {}; return t; }
    inline void setThemeValues(const ThemeValues& t) { themeValues() = t; }
    inline ImVec4 themed(const ImVec4& t, const ImVec4& dark, const ImVec4& lightCol) { return t.w > 0.0f ? t : pick(dark, lightCol); }

    inline ImVec4 val()     { return themed(themeValues().text, ImVec4(0.95f, 0.95f, 0.05f, 1.0f), ImVec4(0.00f, 0.25f, 0.55f, 1.0f)); }  // measured value
    inline ImVec4 good()    { return themed(themeValues().good, ImVec4(0.05f, 0.95f, 0.05f, 1.0f), ImVec4(0.02f, 0.45f, 0.10f, 1.0f)); }
    inline ImVec4 warn()    { return themed(themeValues().warn, ImVec4(0.95f, 0.75f, 0.05f, 1.0f), ImVec4(0.54f, 0.30f, 0.00f, 1.0f)); }
    inline ImVec4 bad()     { return themed(themeValues().bad, ImVec4(1.00f, 0.38f, 0.38f, 1.0f), ImVec4(0.74f, 0.06f, 0.10f, 1.0f)); }
    inline ImVec4 orange()  { return pick(ImVec4(0.95f, 0.60f, 0.05f, 1.0f), ImVec4(0.70f, 0.32f, 0.00f, 1.0f)); }
    inline ImVec4 magenta() { return pick(ImVec4(0.95f, 0.05f, 0.95f, 1.0f), ImVec4(0.58f, 0.08f, 0.58f, 1.0f)); }
    inline ImVec4 cyan()    { return pick(ImVec4(0.05f, 0.95f, 0.95f, 1.0f), ImVec4(0.00f, 0.42f, 0.52f, 1.0f)); }
    inline ImVec4 grey()    { return pick(ImVec4(0.70f, 0.70f, 0.70f, 1.0f), ImVec4(0.32f, 0.36f, 0.40f, 1.0f)); }
    inline ImVec4 lgrey()   { return pick(ImVec4(0.80f, 0.80f, 0.80f, 1.0f), ImVec4(0.30f, 0.34f, 0.38f, 1.0f)); }
}
