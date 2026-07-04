#include "fake_window.h"
#include "settings.h"
#include <game_window.h>

void FakeWindow::initHybrisHooks(std::unordered_map<std::string, void*>& syms) {
    syms["ANativeWindow_acquire"] = (void*)+[](void*) {};
    syms["ANativeWindow_release"] = (void*)+[](void*) {};
    syms["ANativeWindow_fromSurface"] = (void*)+[](void*, void*) -> void* {
        return nullptr;
    };
    syms["ANativeWindow_setBuffersGeometry"] = (void*)+[](void*, int32_t, int32_t, int32_t) -> int32_t {
        return 0;
    };
    syms["ANativeWindow_getWidth"] = (void*)+[](void* window) -> int32_t {
        int width, height;
        ((GameWindow*)window)->getWindowSize(width, height);
        return width;
    };
    syms["ANativeWindow_getHeight"] = (void*)+[](void* window) -> int32_t {
        int width, height;
        ((GameWindow*)window)->getWindowSize(width, height);
        return height - Settings::menubarsize;
    };
}
