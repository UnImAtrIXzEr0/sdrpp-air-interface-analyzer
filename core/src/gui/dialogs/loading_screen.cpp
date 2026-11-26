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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

// Start-up screen: dark night-blue background with fixed colours, independent of
// the selected theme (the theme is not applied yet while loading). Behind the
// text a faint spectrum with four channels and a waterfall that moves on with
// every loading step. The progress bar is a row of symbols; the symbol of the
// step being loaded is larger and amber.
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

    static const ImU32 COL_BG_TOP = IM_COL32(15, 30, 46, 255);
    static const ImU32 COL_BG_BOTTOM = IM_COL32(8, 17, 27, 255);
    static const ImU32 COL_TITLE = IM_COL32(238, 243, 248, 255);
    static const ImU32 COL_TEXT = IM_COL32(157, 176, 195, 255);
    static const ImU32 COL_LABEL = IM_COL32(127, 149, 170, 255);
    static const ImU32 COL_TODO = IM_COL32(34, 55, 77, 255);
    static const ImU32 COL_DONE = IM_COL32(90, 174, 234, 255);
    static const ImU32 COL_ACTIVE = IM_COL32(227, 163, 59, 255);
    static const ImU32 COL_RULE = IM_COL32(28, 46, 64, 255);
    static const ImU32 COL_TRACE = IM_COL32(62, 127, 181, 255);

    static ImU32 withAlpha(ImU32 c, int a) {
        return (c & ~IM_COL32_A_MASK) | ((ImU32)(std::min)((std::max)(a, 0), 255) << IM_COL32_A_SHIFT);
    }

    // Deterministic noise 0..1, so the picture is the same for the same step
    static float noise(uint32_t x, uint32_t y) {
        uint32_t h = x * 374761393u + y * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
    }

    // Relative level of the background spectrum at position u (0..1): four channels
    static float channels(float u) {
        static const float centres[4] = { 0.18f, 0.41f, 0.63f, 0.84f };
        float v = 0.0f;
        for (float c : centres) { v += (std::max)(0.0f, 1.0f - std::fabs(u - c) / 0.035f); }
        return (std::min)(1.0f, v * 1.6f);
    }

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

    // Soft glow: stacked translucent discs
    static void glow(ImDrawList* dl, ImVec2 c, float r, ImU32 col, int alphaPerDisc, int discs) {
        for (int i = 0; i < discs; i++) {
            dl->AddCircleFilled(c, r * (1.0f - (float)i / (float)discs), withAlpha(col, alphaPerDisc), 48);
        }
    }

    // Smooth radial gradient: triangle fan from an opaque centre colour to a transparent rim
    static void radialGlow(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
        const int seg = 64;
        ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        ImU32 rim = withAlpha(col, 0);
        dl->PrimReserve(seg * 3, seg + 1);
        ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
        dl->PrimWriteVtx(c, uv, col);
        for (int i = 0; i < seg; i++) {
            float a = (float)i / (float)seg * 6.2831853f;
            dl->PrimWriteVtx(ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), uv, rim);
        }
        for (int i = 0; i < seg; i++) {
            dl->PrimWriteIdx(base);
            dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
            dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % seg));
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
        ImFont* big = style::bigFont;
        ImFont* base = style::baseFont;
        ImFont* tiny = style::tinyFont ? style::tinyFont : style::baseFont;

        // Background: vertical gradient and a soft light cone behind the title
        dl->AddRectFilledMultiColor(p0, p1, COL_BG_TOP, COL_BG_TOP, COL_BG_BOTTOM, COL_BG_BOTTOM);
        radialGlow(dl, ImVec2(p0.x + sz.x * 0.5f, p0.y + sz.y * 0.5f - 60.0f * s), (std::min)(460.0f * s, sz.x * 0.5f), IM_COL32(27, 76, 122, 90));

        // Footer: origin and authors
        const char* foot = "SDR++ by Alexandre Rouma (ON5RYZ)  -  SDR++Brown by sannysanoff  -  Air Interface Analyzer by Dennis, DB2OE";
        ImVec2 fs = textSize(tiny, foot);
        float fy = p1.y - fs.y - 22.0f * s;
        float ruleY = fy - 14.0f * s;

        // Centre block: logo, label, name, tagline
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

        // Symbol row and status line positions
        const int SYMBOLS = 48;
        float barW = (std::min)(720.0f * s, sz.x * 0.8f);
        float bx = p0.x + (sz.x - barW) * 0.5f;
        float by = p0.y + sz.y * 0.5f + 46.0f * s;
        float ly = by + 18.0f * s;
        float statusBottom = ly + textSize(base, "Ag").y;

        // Spectrum and waterfall at the bottom, only where they do not reach the text
        float wfH = (std::min)(150.0f * s, sz.y * 0.19f);
        float wfBottom = ruleY - 4.0f * s;
        float wfTop = wfBottom - wfH;
        float specBase = wfTop - 20.0f * s;
        float specH = 55.0f * s;
        if (specBase - specH > statusBottom + 24.0f * s) {
            float cw = 4.0f * s, rh = 3.0f * s;
            int cols = (int)(sz.x / cw) + 1;
            int rows = (int)(wfH / rh);
            for (int r = 0; r < rows; r++) {
                float y = wfTop + r * rh;
                float fade = (float)r / (float)rows;
                for (int c = 0; c < cols; c++) {
                    float u = (float)c / (float)cols;
                    float v = (std::min)(1.0f, 0.12f + 0.7f * channels(u) + 0.08f * noise(c, r + stepsDone * 3));
                    dl->AddRectFilled(ImVec2(p0.x + c * cw, y), ImVec2(p0.x + c * cw + cw, y + rh), IM_COL32(40, 110, 170, (int)(110.0f * v * fade)));
                }
            }
            std::vector<ImVec2> pts;
            for (float x = 0.0f; x <= sz.x; x += 2.0f * s) {
                float u = x / sz.x;
                float ripple = 4.0f * s * std::sin(x * 0.21f / s) * std::sin(x * 0.05f / s + stepsDone * 0.7f);
                pts.push_back(ImVec2(p0.x + x, specBase - specH * channels(u) - ripple));
            }
            dl->AddPolyline(pts.data(), (int)pts.size(), withAlpha(COL_DONE, 30), ImDrawFlags_None, 7.0f * s);
            dl->AddPolyline(pts.data(), (int)pts.size(), withAlpha(COL_DONE, 60), ImDrawFlags_None, 3.5f * s);
            dl->AddPolyline(pts.data(), (int)pts.size(), COL_TRACE, ImDrawFlags_None, 1.2f * s);
        }

        // Logo and texts
        float tx = c0.x;
        if (haveLogo) {
            dl->AddImage(icons::LOGO, c0, ImVec2(c0.x + imageSize.x, c0.y + imageSize.y));
            tx += imageSize.x + gap;
        }
        float ty = c0.y + (blockH - textH) * 0.5f;
        spacedText(dl, tiny, ImVec2(tx, ty), COL_LABEL, label, 3.0f * s);
        ty += sl.y + 6.0f * s;
        text(dl, big, ImVec2(tx, ty), COL_TITLE, name);
        ty += sn.y + 8.0f * s;
        text(dl, base, ImVec2(tx, ty), COL_TEXT, tag.c_str());

        // Progress as a row of symbols; done and active symbols glow
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
            if (i == active) {
                glow(dl, c, 18.0f * s, COL_ACTIVE, 28, 4);
                dl->AddCircleFilled(c, 8.0f * s, COL_ACTIVE, 24);
            }
            else if (i < active) {
                dl->AddCircleFilled(c, 8.0f * s, withAlpha(COL_DONE, 40), 16);
                dl->AddCircleFilled(c, 4.5f * s, COL_DONE, 16);
            }
            else { dl->AddCircleFilled(c, 4.5f * s, COL_TODO, 16); }
        }
        text(dl, base, ImVec2(bx, ly), COL_TEXT, msg.c_str());
        if (pct >= 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d %%", pct);
            text(dl, base, ImVec2(bx + barW - textSize(base, buf).x, ly), COL_TITLE, buf);
        }

        // Footer
        float margin = (std::min)(80.0f * s, sz.x * 0.05f);
        dl->AddRectFilled(ImVec2(p0.x + margin, ruleY), ImVec2(p1.x - margin, ruleY + 1.0f * s), COL_RULE);
        text(dl, tiny, ImVec2(p0.x + (sz.x - fs.x) * 0.5f, fy), COL_LABEL, foot);

        backend::render(false);
    }
}
