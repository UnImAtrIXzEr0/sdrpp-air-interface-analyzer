#pragma once
#include <thread>
#include <string>
#include <mutex>

namespace LoadingScreen {
    void init();
    void show(std::string msg);
    // Number of show() calls still to come; turns the progress bar from "unknown" into a real fraction.
    void expectSteps(int remaining);
};
