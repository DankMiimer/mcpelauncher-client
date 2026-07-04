#include "fake_egl.h"
#include "gl_core_patch.h"
#include "settings.h"
#include "imgui_ui.h"
#include <map>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#define __ANDROID__
#include <EGL/egl.h>
#undef __ANDROID__
#include <log.h>
#include <cstring>
#include <game_window.h>
#include <sys/syscall.h>
#include <sched.h>
#include <dirent.h>
#include <thread>
#include <set>
#include <string>
#include <cstdlib>
#include <sys/resource.h>
#include <unistd.h>
#include <mcpelauncher/linker.h>
#ifdef USE_ARMHF_SUPPORT
#include "armhf_support.h"
#endif
namespace fake_egl {

static thread_local EGLSurface currentDrawSurface;
static void *(*hostProcAddrFn)(const char *);
static std::unordered_map<std::string, void *> hostProcOverrides;

struct FrameMetricsState {
    using Clock = std::chrono::steady_clock;

    bool initialized = false;
    FILE *file = nullptr;
    Clock::time_point firstFrame;
    Clock::time_point previousFrame;
    unsigned long long frame = 0;

    void record(Clock::time_point frameStart, Clock::time_point swapEnd) {
        if(!initialized) {
            initialized = true;
            const char *path = std::getenv("MCPE_FRAME_METRICS");
            if(path && *path) {
                file = std::fopen(path, "w");
                if(file) {
                    std::setvbuf(file, nullptr, _IOFBF, 64 * 1024);
                    std::fputs("frame,epoch_us,elapsed_us,frame_us,swap_us,user_us,system_us,minflt,majflt,nvcsw,nivcsw,maxrss_kb\n", file);
                    Log::info("FrameMetrics", "Recording frame times to %s", path);
                } else {
                    Log::error("FrameMetrics", "Unable to open %s", path);
                }
            }
            firstFrame = frameStart;
            previousFrame = frameStart;
        }

        if(!file)
            return;

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            frameStart - firstFrame).count();
        auto frameTime = std::chrono::duration_cast<std::chrono::microseconds>(
            frameStart - previousFrame).count();
        auto swapTime = std::chrono::duration_cast<std::chrono::microseconds>(
            swapEnd - frameStart).count();
        auto epoch = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        auto userTime = static_cast<long long>(usage.ru_utime.tv_sec) * 1000000LL +
                        static_cast<long long>(usage.ru_utime.tv_usec);
        auto systemTime = static_cast<long long>(usage.ru_stime.tv_sec) * 1000000LL +
                          static_cast<long long>(usage.ru_stime.tv_usec);
        std::fprintf(file, "%llu,%lld,%lld,%lld,%lld,%lld,%lld,%ld,%ld,%ld,%ld,%ld\n",
                     frame,
                     static_cast<long long>(epoch),
                     static_cast<long long>(elapsed),
                     static_cast<long long>(frame ? frameTime : 0),
                     static_cast<long long>(swapTime),
                     userTime, systemTime,
                     usage.ru_minflt, usage.ru_majflt,
                     usage.ru_nvcsw, usage.ru_nivcsw,
                     usage.ru_maxrss);
        previousFrame = frameStart;
        frame++;

    }
};

static FrameMetricsState frameMetrics;

EGLBoolean eglInitialize(EGLDisplay display, EGLint *major, EGLint *minor) {
    if(major)
        *major = 1;
    if(minor)
        *minor = 5;
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay display) {
    return EGL_TRUE;
}

EGLint eglGetError() {
    return EGL_SUCCESS;
}

char const *eglQueryString(EGLDisplay display, EGLint name) {
    if(name == EGL_VENDOR)
        return "mcpelauncher";
    if(name == EGL_VERSION)
        return "1.5 mcpelauncher";
    if(name == EGL_EXTENSIONS)
        return "";
    Log::warn("FakeEGL", "eglQueryString %x", name);
    return nullptr;
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType dp) {
    return (EGLDisplay *)1;
}

EGLDisplay eglGetCurrentDisplay() {
    return (EGLDisplay *)1;
}

EGLContext eglGetCurrentContext() {
    return currentDrawSurface ? (EGLContext *)1 : (EGLContext *)0;
}

EGLBoolean eglChooseConfig(EGLDisplay display, EGLint const *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    *num_config = 1;
    return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config, EGLint attribute, EGLint *value) {
    if(attribute == EGL_NATIVE_VISUAL_ID) {
        *value = 0;
        return EGL_TRUE;
    }
    if(attribute == EGL_RED_SIZE || attribute == EGL_GREEN_SIZE || attribute == EGL_BLUE_SIZE || attribute == EGL_ALPHA_SIZE || attribute == EGL_DEPTH_SIZE || attribute == EGL_STENCIL_SIZE) {
        *value = 8;
        return EGL_TRUE;
    }
    Log::warn("FakeEGL", "eglGetConfigAttrib %x", attribute);
    return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config, EGLNativeWindowType native_window, EGLint const *attrib_list) {
    return native_window;
}

EGLBoolean eglDestroySurface(EGLDisplay display, EGLSurface surface) {
    return EGL_TRUE;
}

EGLContext eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share_context, EGLint const *attrib_list) {
    return (EGLContext *)1;
}

EGLBoolean eglDestroyContext(EGLDisplay display, EGLContext context) {
    return EGL_TRUE;
}

EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context) {
    if(draw != nullptr) {
        ((GameWindow *)draw)->makeCurrent(true);
#ifdef USE_IMGUI
        ImGuiUIInit((GameWindow*)draw);
#endif
    } else {
        ((GameWindow *)currentDrawSurface)->makeCurrent(false);
    }
    auto hostGlGetString = reinterpret_cast<const unsigned char *(*)(unsigned int)>(
        hostProcAddrFn("glGetString"));
    auto hostVersion = hostGlGetString ? hostGlGetString(0x1F02 /* GL_VERSION */) : nullptr;
    auto hostContext = ::eglGetCurrentContext();
    auto hostError = ::eglGetError();
    Log::trace("FakeEGL",
               "eglMakeCurrent tid=%ld draw=%p context=%p hostContext=%p hostError=0x%x hostGL=%s",
               (long) syscall(SYS_gettid), draw, context, hostContext, hostError,
               hostVersion ? reinterpret_cast<const char *>(hostVersion) : "(null)");
    currentDrawSurface = draw;
    return EGL_TRUE;
}

static void parseCpuList(const char *value, cpu_set_t *set) {
    CPU_ZERO(set);
    const char *p = value;
    while(*p) {
        char *end = nullptr;
        long a = std::strtol(p, &end, 10);
        if(end == p)
            break;
        long b = a;
        p = end;
        if(*p == '-') {
            b = std::strtol(p + 1, &end, 10);
            if(end == p + 1)
                break;
            p = end;
        }
        for(long c = a; c <= b; c++)
            if(c >= 0 && c < CPU_SETSIZE)
                CPU_SET((int)c, set);
        if(*p == ',')
            p++;
    }
}

// See MCPE_PIN_RENDER_CORE / MCPE_PIN_OTHER_CORES / MCPE_AFFINITY_LOG.
// Called once from the render (swap) thread.
static void applyRenderAffinity() {
    long renderTid = (long)syscall(SYS_gettid);
    const char *renderCore = std::getenv("MCPE_PIN_RENDER_CORE");
    if(renderCore && *renderCore) {
        cpu_set_t set;
        parseCpuList(renderCore, &set);
        if(CPU_COUNT(&set) > 0 &&
           sched_setaffinity(renderTid, sizeof(set), &set) == 0)
            Log::info("Affinity", "Render thread %ld pinned to core(s) %s",
                      renderTid, renderCore);
        else
            Log::warn("Affinity", "Could not pin render thread %ld to %s",
                      renderTid, renderCore);
    }
    const char *otherCoresEnv = std::getenv("MCPE_PIN_OTHER_CORES");
    if(otherCoresEnv && *otherCoresEnv) {
        std::string otherCores = otherCoresEnv;
        const char *logEnv = std::getenv("MCPE_AFFINITY_LOG");
        bool logNames = logEnv && *logEnv == '1';
        const char *mainCoreEnv = std::getenv("MCPE_PIN_MAIN_CORE");
        std::string mainCore = mainCoreEnv ? mainCoreEnv : "";
        std::thread([renderTid, otherCores, mainCore, logNames] {
            cpu_set_t set, mainSet;
            parseCpuList(otherCores.c_str(), &set);
            if(CPU_COUNT(&set) == 0)
                return;
            bool haveMain = !mainCore.empty();
            if(haveMain) {
                parseCpuList(mainCore.c_str(), &mainSet);
                if(CPU_COUNT(&mainSet) == 0)
                    haveMain = false;
            }
            long self = (long)syscall(SYS_gettid);
            // Keep the scanner off the render core (it inherits the render
            // thread's affinity at spawn).
            sched_setaffinity(self, sizeof(set), &set);
            std::set<long> seen;
            while(true) {
                DIR *dir = opendir("/proc/self/task");
                if(dir) {
                    dirent *ent;
                    while((ent = readdir(dir))) {
                        if(ent->d_name[0] < '0' || ent->d_name[0] > '9')
                            continue;
                        long tid = std::strtol(ent->d_name, nullptr, 10);
                        if(tid == renderTid || tid == self)
                            continue;
                        char commPath[64], comm[32] = {0};
                        std::snprintf(commPath, sizeof(commPath),
                                      "/proc/self/task/%ld/comm", tid);
                        if(FILE *f = std::fopen(commPath, "r")) {
                            if(std::fgets(comm, sizeof(comm), f))
                                comm[strcspn(comm, "\n")] = 0;
                            std::fclose(f);
                        }
                        // Mali driver threads submit GPU work on behalf of
                        // the render thread; leave them unconfined so they
                        // never queue behind saturated worker cores.
                        if(strncmp(comm, "mali-", 5) == 0)
                            continue;
                        // The Bedrock main/simulation thread integrates chunk
                        // results and blocks on worker locks; a dedicated core
                        // stops workers from preempting it (kills the lock-wait
                        // freeze). Its comm is "MINECRAFT MAIN".
                        bool isMain = strncmp(comm, "MINECRAFT MAIN", 14) == 0;
                        const cpu_set_t *use = (haveMain && isMain) ? &mainSet : &set;
                        sched_setaffinity(tid, sizeof(*use), use);
                        if(logNames && seen.insert(tid).second)
                            Log::info("Affinity", "Confined tid %ld (%s) to %s",
                                      tid, comm,
                                      (haveMain && isMain) ? mainCore.c_str()
                                                           : otherCores.c_str());
                    }
                    closedir(dir);
                }
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
        }).detach();
        Log::info("Affinity", "Confining non-render threads to core(s) %s",
                  otherCoresEnv);
    }
}

EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
    //    Log::trace("FakeEGL", "eglSwapBuffers");
#ifdef USE_IMGUI
    ImGuiUIDrawFrame((GameWindow*)surface);
#endif
    static bool affinityApplied = false;
    if(!affinityApplied) {
        affinityApplied = true;
        applyRenderAffinity();
    }
    auto frameStart = FrameMetricsState::Clock::now();
    ((GameWindow *)surface)->swapBuffers();
    frameMetrics.record(frameStart, FrameMetricsState::Clock::now());
    return EGL_TRUE;
}

EGLBoolean eglSwapInterval(EGLDisplay display, EGLint interval) {
    static int intervalOverride = -2;
    static bool logged = false;
    if(intervalOverride == -2) {
        intervalOverride = -1;
        const char *value = std::getenv("MCPE_SWAP_INTERVAL_OVERRIDE");
        if(value && *value) {
            char *end = nullptr;
            long parsed = std::strtol(value, &end, 10);
            if(end != value && *end == '\0' && (parsed == 0 || parsed == 1))
                intervalOverride = static_cast<int>(parsed);
            else
                Log::warn("FakeEGL", "Ignoring invalid MCPE_SWAP_INTERVAL_OVERRIDE=%s", value);
        }
    }

    int effectiveInterval = intervalOverride >= 0 ? intervalOverride : interval;
    if(!logged) {
        Log::info("FakeEGL", "Swap interval requested=%d effective=%d", interval,
                  effectiveInterval);
        logged = true;
    }
    ((GameWindow *)currentDrawSurface)->setSwapInterval(effectiveInterval);
    return EGL_TRUE;
}

EGLBoolean eglQuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint *value) {
    if(attribute == EGL_WIDTH || attribute == EGL_HEIGHT) {
        int w, h;
        ((GameWindow *)surface)->getWindowSize(w, h);
        *value = (attribute == EGL_WIDTH ? w : h - Settings::menubarsize);
        return EGL_TRUE;
    }
    Log::warn("FakeEGL", "eglQuerySurface %x", attribute);
    return EGL_TRUE;
}

void *eglGetProcAddress(const char *name) {
    auto it = hostProcOverrides.find(name);
    if(it != hostProcOverrides.end())
        return it->second;
    return hostProcAddrFn(name);
}

}  // namespace fake_egl

bool FakeEGL::enableTexturePatch = false;

void FakeEGL::setProcAddrFunction(void *(*fn)(const char *)) {
    fake_egl::hostProcAddrFn = fn;
}

void FakeEGL::installLibrary() {
    std::unordered_map<std::string, void *> syms;
    syms["eglInitialize"] = (void *)fake_egl::eglInitialize;
    syms["eglTerminate"] = (void *)fake_egl::eglTerminate;
    syms["eglGetError"] = (void *)fake_egl::eglGetError;
    syms["eglQueryString"] = (void *)fake_egl::eglQueryString;
    syms["eglGetDisplay"] = (void *)fake_egl::eglGetDisplay;
    syms["eglGetCurrentDisplay"] = (void *)fake_egl::eglGetCurrentDisplay;
    syms["eglGetCurrentContext"] = (void *)fake_egl::eglGetCurrentContext;
    syms["eglChooseConfig"] = (void *)fake_egl::eglChooseConfig;
    syms["eglGetConfigAttrib"] = (void *)fake_egl::eglGetConfigAttrib;
    syms["eglCreateWindowSurface"] = (void *)fake_egl::eglCreateWindowSurface;
    syms["eglDestroySurface"] = (void *)fake_egl::eglDestroySurface;
    syms["eglCreateContext"] = (void *)fake_egl::eglCreateContext;
    syms["eglDestroyContext"] = (void *)fake_egl::eglDestroyContext;
    syms["eglMakeCurrent"] = (void *)fake_egl::eglMakeCurrent;
    syms["eglSwapBuffers"] = (void *)fake_egl::eglSwapBuffers;
    syms["eglSwapInterval"] = (void *)fake_egl::eglSwapInterval;
    syms["eglQuerySurface"] = (void *)fake_egl::eglQuerySurface;
    syms["eglGetProcAddress"] = (void *)fake_egl::eglGetProcAddress;
    syms["eglWaitClient"] = (void *)+[]() -> EGLBoolean {
        return EGL_TRUE;
    };
    linker::load_library("libEGL.so", syms);
}

void FakeEGL::setupGLOverrides() {
#ifdef USE_ARMHF_SUPPORT
    ArmhfSupport::install(fake_egl::hostProcOverrides);
#endif
    // fake_egl::hostProcOverrides["glViewport"] = (void *)+[](int x,
 	// int y,
 	// int width,
 	// int height) {
    //     ((void (*)(int x,
 	// int y,
 	// int width,
 	// int height))(fake_egl::hostProcAddrFn("glViewport")))(x, y, width, height);

    // };
    // MESA 23.1 blackscreen Workaround Start for 1.18.30+, bgfy will disable the extension and the game works
    fake_egl::hostProcOverrides["glDrawElementsInstancedOES"] = nullptr;
    fake_egl::hostProcOverrides["glDrawArraysInstancedOES"] = nullptr;
    fake_egl::hostProcOverrides["glVertexAttribDivisorOES"] = nullptr;
    // MESA 23.1 blackscreen Workaround End
    fake_egl::hostProcOverrides["glInvalidateFramebuffer"] = (void *)+[]() {};  // Stub for a NVIDIA bug
    if(FakeEGL::enableTexturePatch) {
        // Minecraft Intel/Amd Texture Bug 1.16.210-1.17.2 and beyond
        // This patch reduces the visual glitch of blocks, does not work with high resolution textures
        // TODO improve Bugdetection
        fake_egl::hostProcOverrides["glTexSubImage2D"] = (void *)+[](unsigned int target, int level, int xoffset, int yoffset, int width, int height, unsigned int format, unsigned int type, const void *data) {
            if(width == 1024 && height == 1024) {
                size_t z = 0;
                for(long long y = 0; y < height; ++y) {
                    if(*((int32_t *)data + 987 + y * width) == *((int32_t *)data + 988 + y * width) && *((int32_t *)data + 988 + y * width) == *((int32_t *)data + 989 + y * width) && *((int32_t *)data + 989 + y * width) == *((int32_t *)data + 990 + y * width) && *((int32_t *)data + 990 + y * width) != *((int32_t *)data + 991 + y * width)) {
                        z++;
                    }
                }
                if(z >= 64) {
                    for(long long y = 0; y < 32; ++y) {
                        memmove((char *)data + y * width * 4 + 32 * 4, (char *)data + y * width * 4 + 31 * 4, width * 4 - 32 * 4);
                    }
                    for(long long y = height - 2; y >= 31; --y) {
                        memcpy((char *)data + (y + 1) * width * 4 + 32 * 4, (char *)data + y * width * 4 + 31 * 4, width * 4 - 32 * 4);
                        memcpy((char *)data + (y + 1) * width * 4, (char *)data + y * width * 4, 32 * 4);
                    }
                }
            }
            if(width == 2048 && height == 1024) {
                if(*((int32_t *)data + 989 + 1024) == *((int32_t *)data + 990 + 1024) && *((int32_t *)data + 990 + 1024) != *((int32_t *)data + 991 + 1024)) {
                    for(long long y = 0; y < 32; ++y) {
                        memmove((char *)data + y * width * 4 + 32 * 4, (char *)data + y * width * 4 + 31 * 4, width * 4 - 32 * 4);
                    }
                    for(long long y = height - 2; y >= 31; --y) {
                        memcpy((char *)data + (y + 1) * width * 4 + 32 * 4, (char *)data + y * width * 4 + 31 * 4, width * 4 - 32 * 4);
                        memcpy((char *)data + (y + 1) * width * 4, (char *)data + y * width * 4, 32 * 4);
                    }
                }
            }

            if(width == 512 && height == 512) {
                size_t uscore = 0;
                size_t itemscorea = 0, itemscoreb = 0, itemscorec = 0, itemscored = 0;
                for(int y = 0; y < height; ++y) {
                    if(*((uint32_t *)data + y * width + 511 - 14) != 0) {
                        ++itemscorea;
                    }
                    if(*((uint32_t *)data + y * width + 511 - 13) != 0) {
                        ++itemscoreb;
                    }
                    if(*((uint32_t *)data + y * width + 511 - 12) != 0) {
                        ++itemscorec;
                    }
                    if(*((uint32_t *)data + y * width + 511 - 11) == 0) {
                        ++itemscored;
                    }
                }
                for(int x = 0; x < width; ++x) {
                    if(*((uint32_t *)data + 1 * width + x) != 0) {
                        ++uscore;
                    }
                }
                size_t z = 0;
                for(long long y = 0; y < height; ++y) {
                    if(*((int32_t *)data + 511 - 20 + y * width) == *((int32_t *)data + 511 - 19 + y * width) && *((int32_t *)data + 511 - 19 + y * width) == *((int32_t *)data + 511 - 18 + y * width) && *((int32_t *)data + 511 - 18 + y * width) == *((int32_t *)data + 511 - 17 + y * width) && *((int32_t *)data + 511 - 17 + y * width) != *((int32_t *)data + 511 - 16 + y * width)) {
                        z++;
                    }
                }
                if(z >= 64 || (itemscorea > 64 && itemscoreb > 64 && itemscorec > 64 && itemscored > 64)) {
                    if(z >= 64 || uscore < 16) {
                        for(long long y = 0; y < 16; ++y) {
                            memmove((char *)data + y * width * 4 + 16 * 4, (char *)data + y * width * 4 + 15 * 4, width * 4 - 16 * 4);
                        }
                    } else {
                        for(long long y = 15; y >= 0; --y) {
                            memcpy((char *)data + (y + 1) * width * 4 + 16 * 4, (char *)data + y * width * 4 + 15 * 4, width * 4 - 16 * 4);
                        }
                    }
                    if(z >= 64) {
                        for(long long y = height - 2; y >= 16; --y) {
                            memcpy((char *)data + (y + 1) * width * 4 + 16 * 4, (char *)data + y * width * 4 + 15 * 4, width * 4 - 16 * 4);
                            memcpy((char *)data + (y + 1) * width * 4, (char *)data + y * width * 4, 16 * 4);
                        }
                    } else {
                        for(long long y = height - 2; y >= 16; --y) {
                            memcpy((char *)data + (y + 1) * width * 4 + 4, (char *)data + y * width * 4 + 0, width * 4 - 4);
                        }
                    }
                }
            }
            ((void (*)(unsigned int target, int level, int xoffset, int yoffset, int width, int height, unsigned int format, unsigned int type, const void *data))(fake_egl::hostProcAddrFn("glTexSubImage2D")))(target, level, xoffset, yoffset, width, height, format, type, data);
        };
    }
    GLCorePatch::installGL(fake_egl::hostProcOverrides, fake_egl::eglGetProcAddress);
}
