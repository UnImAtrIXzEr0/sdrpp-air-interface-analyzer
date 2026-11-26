#pragma once

#include "utils/event.h"
#include <imgui/imgui.h>
#include <signal_path/signal_path.h>

extern long long lastDrawTimeBackend;
extern long long lastDrawTime;
extern int glSleepTime;

namespace displaymenu {
    void init();
    void checkKeybinds();
    void draw(void* ctx);
    // Selects a waterfall colour map by name (used when a theme brings its own);
    // returns false if no such map is loaded
    bool selectColorMap(const std::string& name);
    extern bool phoneLayout;
    // touch mode for modules (functions are exported from the core DLL, data is not)
    bool touchMode();
#ifdef __ANDROID__
    extern float displayDensity;
#endif
    extern Event<ImGuiContext *> onDisplayDraw;
    extern bool showBattery;
    extern bool showClock;
    extern bool detectSignals;
    extern bool showFFT;
    extern bool showFFTShadows;
    extern bool showMicHistogram;

    // Handler for center frequency changes
    extern EventHandler<double> centerFreqChangedHandler;
    extern std::string currentBatteryLevel;

    extern enum TranscieverLayout {
        TRAL_NONE = 0,
        TRAL_SSB_FIRST = 1
    } transcieverLayout;

}