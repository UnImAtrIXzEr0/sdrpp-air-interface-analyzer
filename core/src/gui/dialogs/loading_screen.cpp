#include <version.h>
#include <gui/dialogs/loading_screen.h>
#include <gui/main_window.h>
#include <imgui.h>
#include <gui/icons.h>
#include <gui/style.h>
#include <credits.h>
#include <gui/gui.h>
#include <backend.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>

// Start-up screen: calm light background with fixed colours, independent of the
// selected theme (the theme is not applied yet while loading). The progress bar
// is a row of symbols; the symbol of the step being loaded is larger and amber.
// show() only redraws when a new loading step starts, so the bar shows real
// progress (steps done / steps expected) instead of a time-based animation.
namespace LoadingScreen {
    ImVec2 imageSize(128.0f, 128.0f);
    static int stepsDone = 0;
    static int stepsTotal = 0;   // 0: not known yet

    void init() {
        imageSize = ImVec2(128.0f * style::uiScale, 128.0f * style::uiScale);
    }

    void expectSteps(int remaining) {
        stepsTotal = stepsDone + (std::max)(remaining, 0);
    }

    static const ImU32 COL_BG = IM_COL32(243, 245, 247, 255);
    static const ImU32 COL_NAVY = IM_COL32(14, 47, 77, 255);
    static const ImU32 COL_TEXT = IM_COL32(74, 87, 99, 255);
    static const ImU32 COL_LABEL = IM_COL32(90, 104, 117, 255);
    static const ImU32 COL_SYMBOL = IM_COL32(208, 215, 222, 255);
    static const ImU32 COL_ACTIVE = IM_COL32(227, 163, 59, 255);
    static const ImU32 COL_RULE = IM_COL32(208, 214, 220, 255);

    static void text(ImDrawList* dl, ImFont* f, ImVec2 pos, ImU32 col, const char* s) {
        if (!f) { f = ImGui::GetFont(); }
        dl->AddText(f, f->FontSize, pos, col, s);
    }

    static ImVec2 textSize(ImFont* f, const char* s) {
        if (!f) { f = ImGui::GetFont(); }
        return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, s);
    }

    // Text with extra letter spacing (small caps style label)
    static void spacedText(ImDrawList* dl, ImFont* f, ImVec2 pos, ImU32 col, const char* s, float spacing) {
        if (!f) { f = ImGui::GetFont(); }
        char c[2] = { 0, 0 };
        for (const char* p = s; *p; p++) {
            c[0] = *p;
            dl->AddText(f, f->FontSize, pos, col, c);
            pos.x += textSize(f, c).x + spacing;
        }
    }

    void show(std::string msg) {
        stepsDone++;
        backend::beginFrame();

        const float s = style::uiScale;
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 p0 = vp->Pos;
        ImVec2 sz = vp->Size;
        ImVec2 p1 = ImVec2(p0.x + sz.x, p0.y + sz.y);
        ImDrawList* dl = ImGui::GetBackgroundDrawList();

        // Background and a thin rule at the top
        dl->AddRectFilled(p0, p1, COL_BG);
        dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + 3.0f * s), COL_NAVY);

        // Centre block: logo, label, name, tagline
        ImFont* big = style::bigFont;
        ImFont* base = style::baseFont;
        ImFont* tiny = style::tinyFont ? style::tinyFont : style::baseFont;
        const char* label = "SDR++ EDITION";
        const char* name = "Air Interface Analyzer";
        std::string tag = std::string("Digital radio measurement receiver   \xC2\xB7   Version ") + EDITION_VERSION + "   \xC2\xB7   experimental beta";
        ImVec2 sl = textSize(tiny, label);
        ImVec2 sn = textSize(big, name);
        ImVec2 st = textSize(base, tag.c_str());
        float textW = (std::max)(sn.x, st.x);
        float textH = sl.y + 6.0f * s + sn.y + 8.0f * s + st.y;
        float gap = 32.0f * s;
        bool haveLogo = icons::LOGO != (ImTextureID)0;
        float blockW = (haveLogo ? imageSize.x + gap : 0.0f) + textW;
        float blockH = (std::max)(imageSize.y, textH);
        ImVec2 c0 = ImVec2(p0.x + (sz.x - blockW) * 0.5f, p0.y + sz.y * 0.5f - blockH - 30.0f * s);
        float tx = c0.x;
        if (haveLogo) {
            dl->AddImage(icons::LOGO, c0, ImVec2(c0.x + imageSize.x, c0.y + imageSize.y));
            tx += imageSize.x + gap;
        }
        float ty = c0.y + (blockH - textH) * 0.5f;
        spacedText(dl, tiny, ImVec2(tx, ty), COL_LABEL, label, 3.0f * s);
        ty += sl.y + 6.0f * s;
        text(dl, big, ImVec2(tx, ty), COL_NAVY, name);
        ty += sn.y + 8.0f * s;
        text(dl, base, ImVec2(tx, ty), COL_TEXT, tag.c_str());

        // Progress as a row of symbols
        const int SYMBOLS = 48;
        float barW = (std::min)(720.0f * s, sz.x * 0.8f);
        float bx = p0.x + (sz.x - barW) * 0.5f;
        float by = p0.y + sz.y * 0.5f + 46.0f * s;
        float step = barW / (float)(SYMBOLS - 1);
        int active = -1;
        int pct = -1;
        if (stepsTotal > 0) {
            float frac = (std::min)(1.0f, (float)stepsDone / (float)stepsTotal);
            active = (std::min)(SYMBOLS - 1, (int)(frac * (float)SYMBOLS));
            pct = (int)(frac * 100.0f + 0.5f);
        }
        spacedText(dl, tiny, ImVec2(bx, by - textSize(tiny, "M").y - 20.0f * s), COL_LABEL, "SYMBOLS", 2.0f * s);
        for (int i = 0; i < SYMBOLS; i++) {
            ImVec2 c = ImVec2(bx + i * step, by);
            if (i == active) { dl->AddCircleFilled(c, 8.0f * s, COL_ACTIVE, 24); }
            else { dl->AddCircleFilled(c, 4.5f * s, (i < active) ? COL_NAVY : COL_SYMBOL, 16); }
        }
        float ly = by + 18.0f * s;
        text(dl, base, ImVec2(bx, ly), COL_TEXT, msg.c_str());
        if (pct >= 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d %%", pct);
            text(dl, base, ImVec2(bx + barW - textSize(base, buf).x, ly), COL_NAVY, buf);
        }

        // Footer: origin and authors
        const char* foot = "SDR++ by Alexandre Rouma (ON5RYZ)  -  SDR++Brown by sannysanoff  -  Air Interface Analyzer by Dennis, DB2OE";
        ImVec2 fs = textSize(tiny, foot);
        float fy = p1.y - fs.y - 22.0f * s;
        float margin = (std::min)(80.0f * s, sz.x * 0.05f);
        dl->AddRectFilled(ImVec2(p0.x + margin, fy - 14.0f * s), ImVec2(p1.x - margin, fy - 13.0f * s), COL_RULE);
        text(dl, tiny, ImVec2(p0.x + (sz.x - fs.x) * 0.5f, fy), COL_LABEL, foot);

        backend::render(false);
    }
}
