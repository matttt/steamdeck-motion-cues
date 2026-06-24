#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <GLFW/glfw3.h>

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>

static const char *GamescopeOverlayProperty = "GAMESCOPE_EXTERNAL_OVERLAY";

#pragma pack(push, 1)
struct SdHidFrame {
    uint32_t Header;
    uint32_t Increment;
    uint32_t Buttons1;
    uint32_t Buttons2;

    int16_t LeftTrackpadX;
    int16_t LeftTrackpadY;
    int16_t RightTrackpadX;
    int16_t RightTrackpadY;

    int16_t AccelAxisRightToLeft;
    int16_t AccelAxisTopToBottom;
    int16_t AccelAxisFrontToBack;

    int16_t GyroAxisRightToLeft;
    int16_t GyroAxisTopToBottom;
    int16_t GyroAxisFrontToBack;

    int16_t Unknown1;
    int16_t Unknown2;
    int16_t Unknown3;
    int16_t Unknown4;

    int16_t L2Analog;
    int16_t R2Analog;
    int16_t LeftStickX;
    int16_t LeftStickY;
    int16_t RightStickX;
    int16_t RightStickY;

    int16_t LeftTrackpadPushForce;
    int16_t RightTrackpadPushForce;
    int16_t LeftStickTouchCoverage;
    int16_t RightStickTouchCoverage;
};
#pragma pack(pop)
static_assert(sizeof(SdHidFrame) == 64, "SdHidFrame must be exactly 64 bytes");

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

struct Settings {
    int dotAmount = 4;
    float dotSize = 1.00f;  // multiplier on the spacing-derived dot radius
    float opacity = 0.30f;
    float response = 1.00f;
    float colorR = 1.0f;
    float colorG = 1.0f;
    float colorB = 1.0f;
    bool wave = false;
};

// Pixels of dot-field travel for one full wave-motion size cycle.
static constexpr float kWaveLengthPx = 100.0f;

// Wave-motion size envelope. The trough dips below zero so a dot is fully
// invisible for the portion of each cycle where the scale is <= 0.
static constexpr float kWaveScaleMax = 1.25f;   // peak size multiplier
static constexpr float kWaveScaleMin = -0.75f;  // trough; negative => invisible window

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float smoothstep(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

static float readFloatFlag(int argc, const char *argv[], const char *name, float fallback) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) != 0) continue;
        char *end = nullptr;
        float parsed = std::strtof(argv[i + 1], &end);
        if (end != argv[i + 1]) return parsed;
    }
    return fallback;
}

static int readIntFlag(int argc, const char *argv[], const char *name, int fallback) {
    return (int)readFloatFlag(argc, argv, name, (float)fallback);
}

static Settings readSettings(int argc, const char *argv[]) {
    Settings settings;
    settings.dotAmount = std::max(2, std::min(24, readIntFlag(argc, argv, "--dot-amount", settings.dotAmount)));
    settings.dotSize = clampf(readFloatFlag(argc, argv, "--dot-size", settings.dotSize), 0.25f, 3.0f);
    settings.opacity = clampf(readFloatFlag(argc, argv, "--opacity", settings.opacity), 0.05f, 1.0f);
    settings.response = clampf(readFloatFlag(argc, argv, "--response", settings.response), 0.25f, 3.0f);
    settings.colorR = clampf(readFloatFlag(argc, argv, "--color-r", settings.colorR), 0.0f, 1.0f);
    settings.colorG = clampf(readFloatFlag(argc, argv, "--color-g", settings.colorG), 0.0f, 1.0f);
    settings.colorB = clampf(readFloatFlag(argc, argv, "--color-b", settings.colorB), 0.0f, 1.0f);
    settings.wave = readIntFlag(argc, argv, "--wave", settings.wave ? 1 : 0) != 0;
    return settings;
}

class HidMotionDriver {
public:
    HidMotionDriver(uint16_t vid = 0x28DE, uint16_t pid = 0x1205, int interfaceNumber = 2)
        : vId(vid), pId(pid), interfaceNumber(interfaceNumber) {}

    bool init() {
        fd = openMatchingHidraw();
        if (fd < 0) {
            std::cerr << "Could not open the Steam Deck hidraw IMU device." << std::endl;
            return false;
        }
        if (!enableGyro())
            std::cerr << "Warning: IMU-enable command write failed." << std::endl;
        std::cout << "Reading IMU from hidraw device." << std::endl;
        return true;
    }

    void shutdown() {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }

    bool sample(double now, Vec3 &linAccel) {
        drainFrames(now);
        if (fd < 0 || now - lastPacketTime > 1.0)
            return false;
        linAccel = lastLinAccel;
        return true;
    }

private:
    static constexpr float kCountsPerG = 16384.0f;
    static constexpr float kG = 9.80665f;
    static constexpr float kCountToMs2 = kG / kCountsPerG;
    static constexpr float kGravityLP = 0.02f;
    static constexpr int kZeroFramesToReenable = 250;

    uint16_t vId;
    uint16_t pId;
    int interfaceNumber;
    int fd = -1;

    double lastPacketTime = -100.0;
    Vec3 gravity;
    Vec3 lastLinAccel;
    bool gravityInit = false;
    int zeroFrames = 0;

    static int readInterfaceNumber(const std::string &name) {
        std::string path = "/sys/class/hidraw/" + name + "/device/../bInterfaceNumber";
        FILE *f = fopen(path.c_str(), "r");
        if (!f) return -1;
        int n = -1;
        if (fscanf(f, "%x", &n) != 1) n = -1;
        fclose(f);
        return n;
    }

    int openMatchingHidraw() {
        DIR *d = opendir("/sys/class/hidraw");
        if (!d) return -1;

        int foundFd = -1;
        dirent *ent;
        while ((ent = readdir(d)) != nullptr) {
            if (std::strncmp(ent->d_name, "hidraw", 6) != 0) continue;

            std::string devPath = std::string("/dev/") + ent->d_name;
            int tryFd = open(devPath.c_str(), O_RDWR | O_NONBLOCK);
            if (tryFd < 0) continue;

            hidraw_devinfo info;
            std::memset(&info, 0, sizeof(info));
            bool match = false;
            if (ioctl(tryFd, HIDIOCGRAWINFO, &info) == 0) {
                match = (uint16_t)info.vendor == vId &&
                        (uint16_t)info.product == pId &&
                        readInterfaceNumber(ent->d_name) == interfaceNumber;
            }

            if (match) {
                foundFd = tryFd;
                break;
            }
            close(tryFd);
        }

        closedir(d);
        return foundFd;
    }

    bool enableGyro() {
        if (fd < 0) return false;
        unsigned char cmd[65] = {
            0x00,
            0x87, 0x0f, 0x30, 0x18, 0x00, 0x07, 0x07, 0x00, 0x08, 0x07, 0x00, 0x31, 0x02, 0x00, 0x18, 0x00
        };
        return write(fd, cmd, sizeof(cmd)) == (ssize_t)sizeof(cmd);
    }

    void drainFrames(double now) {
        if (fd < 0) return;

        unsigned char buf[sizeof(SdHidFrame)];
        ssize_t r;
        while ((r = read(fd, buf, sizeof(buf))) > 0) {
            if (r < (ssize_t)sizeof(SdHidFrame)) continue;
            const SdHidFrame *f = reinterpret_cast<const SdHidFrame *>(buf);

            Vec3 accel{
                f->AccelAxisRightToLeft * kCountToMs2,
                f->AccelAxisTopToBottom * kCountToMs2,
                f->AccelAxisFrontToBack * kCountToMs2
            };

            if (!gravityInit) {
                gravity = accel;
                gravityInit = true;
            } else {
                gravity.x = gravity.x * (1.f - kGravityLP) + accel.x * kGravityLP;
                gravity.y = gravity.y * (1.f - kGravityLP) + accel.y * kGravityLP;
                gravity.z = gravity.z * (1.f - kGravityLP) + accel.z * kGravityLP;
            }

            lastLinAccel = {accel.x - gravity.x, accel.y - gravity.y, accel.z - gravity.z};
            lastPacketTime = now;

            const bool allZero =
                f->AccelAxisRightToLeft == 0 && f->AccelAxisFrontToBack == 0 &&
                f->AccelAxisTopToBottom == 0 && f->GyroAxisRightToLeft == 0 &&
                f->GyroAxisFrontToBack == 0 && f->GyroAxisTopToBottom == 0;
            if (allZero) {
                if (++zeroFrames > kZeroFramesToReenable) {
                    enableGyro();
                    zeroFrames = 0;
                }
            } else {
                zeroFrames = 0;
            }
        }
    }
};

static void markGamescopeOverlay(GLFWwindow *window) {
    Display *x11Display = glfwGetX11Display();
    Window x11Window = glfwGetX11Window(window);
    if (!x11Window || !x11Display) return;

    Atom overlayAtom = XInternAtom(x11Display, GamescopeOverlayProperty, False);
    uint32_t value = 1;
    XChangeProperty(x11Display, x11Window, overlayAtom, XA_CARDINAL, 32, PropertyNewValue, (unsigned char *)&value, 1);
}

int main(int argc, const char *argv[]) {
    const Settings settings = readSettings(argc, argv);
    const bool debug = std::getenv("MOTION_CUES_DEBUG") != nullptr;

    if (!glfwInit()) {
        std::cerr << "glfwInit failed." << std::endl;
        return 1;
    }

    const char *glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
#ifdef GLFW_MOUSE_PASSTHROUGH
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
#endif

    GLFWwindow *window = glfwCreateWindow(1280, 800, "Steam Deck Motion Cues", nullptr, nullptr);
    if (!window) {
        std::cerr << "glfwCreateWindow failed." << std::endl;
        glfwTerminate();
        return 1;
    }

    markGamescopeOverlay(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
#ifdef GLFW_MOUSE_PASSTHROUGH
    glfwSetWindowAttrib(window, GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
#endif

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, false);
    ImGui_ImplOpenGL3_Init(glslVersion);

    HidMotionDriver input;
    input.init();

    float flowVelX = 0.f;
    float flowVelY = 0.f;
    float flowOffX = 0.f;
    float flowOffY = 0.f;
    float traveled = 0.f;  // accumulated dot-field travel distance, drives wave motion
    double lastTime = glfwGetTime();
    double lastLog = 0.0;

    constexpr float kPi = 3.14159265358979323846f;

    while (!glfwWindowShouldClose(window)) {
        int wx, wy, W, H;
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        glfwGetMonitorWorkarea(monitor, &wx, &wy, &W, &H);
        glfwSetWindowPos(window, wx, wy);
        glfwSetWindowSize(window, W, H);

        const double now = glfwGetTime();
        const float dt = (float)(now - lastTime);
        lastTime = now;

        glfwPollEvents();

        Vec3 lin;
        const bool ok = input.sample(now, lin);
        const float pax = ok ? lin.x : 0.f;
        const float pay = ok ? lin.y : 0.f;
        // const float planeMag = std::sqrt(pax * pax + pay * pay);
        const float response = settings.response;
        // const float intensity = smoothstep(0.0f, 1.0f, planeMag * response);

        const float flowGain = 150.0f * response;
        constexpr float flowSmooth = 0.85f;
        flowVelX = flowVelX * flowSmooth + (-pax * flowGain) * (1.f - flowSmooth);
        flowVelY = flowVelY * flowSmooth + (pay * flowGain) * (1.f - flowSmooth);
        flowOffX += flowVelX * dt;
        flowOffY += flowVelY * dt;
        traveled += std::sqrt(flowVelX * flowVelX + flowVelY * flowVelY) * dt;

        if (debug && (now - lastLog) > 0.2) {
            lastLog = now;
            std::cerr << "[motion-cues] lin=(" << pax << "," << pay << "," << (ok ? lin.z : 0.f)
                      << ") response=" << response
                      << " flow=(" << flowVelX << "," << flowVelY << ")"
                      << (ok ? "" : " [no device/stale]") << std::endl;
        }

        ImGui_ImplGlfw_NewFrame();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        ImDrawList *draw = ImGui::GetForegroundDrawList();
        const float cx = W * 0.5f;
        const float cy = H * 0.5f;
        const float shortest = (float)std::min(W, H);
        const float spacing = clampf(shortest / ((float)settings.dotAmount + 1.0f), 28.0f, 220.0f);
        const float baseRadius = clampf(spacing * 0.08f, 2.0f, 10.0f);
        const float radius = baseRadius * (0.75f + 0.50f) * settings.dotSize;
        const int alpha = (int)clampf(settings.opacity * (0.65f + 0.35f) * 255.0f, 0.0f, 255.0f);
        const int red = (int)clampf(settings.colorR * 255.0f, 0.0f, 255.0f);
        const int green = (int)clampf(settings.colorG * 255.0f, 0.0f, 255.0f);
        const int blue = (int)clampf(settings.colorB * 255.0f, 0.0f, 255.0f);

        constexpr float eyeWFrac = 0.75f;
        constexpr float eyeHFrac = 0.55f;
        constexpr float feather = 1.5f;
        const float eyeHalfW = eyeWFrac * W * 0.5f;
        const float eyeHalfH = eyeHFrac * H * 0.5f;

        float offX = std::fmod(flowOffX, spacing);
        if (offX < 0) offX += spacing;
        float offY = std::fmod(flowOffY, spacing);
        if (offY < 0) offY += spacing;

        // Wave motion: dots pulse in size by sin(2*pi * traveled / wavelength).
        // Checkerboard neighbors run in opposite phase via a per-dot sign flip.
        // Tracking how many whole cells the field has scrolled (wraps) keeps each
        // physical dot's parity stable, so the checkerboard never snaps on wrap.
        const float globalSin = std::sin(kPi * traveled / kWaveLengthPx);
        const long wrapsX = (long)std::floor(flowOffX / spacing);
        const long wrapsY = (long)std::floor(flowOffY / spacing);

        for (float gx = -spacing; gx <= W + spacing; gx += spacing) {
            const long absCol = (long)std::lround(gx / spacing) - wrapsX;
            for (float gy = -spacing; gy <= H + spacing; gy += spacing) {
                const long absRow = (long)std::lround(gy / spacing) - wrapsY;
                const float px = gx + offX;
                const float py = gy + offY;

                const float u = (px - cx) / eyeHalfW;
                const float w = (py - cy) / eyeHalfH;
                const float s = u * u + std::fabs(w) - 1.f;
                const float mask = smoothstep(0.f, feather, s);
                if (mask <= 0.001f) continue;

                const int maskedAlpha = (int)(alpha * mask);
                if (maskedAlpha <= 0) continue;

                float scale = 1.0f;
                if (settings.wave) {
                    const float sign = ((absCol + absRow) & 1L) ? -1.0f : 1.0f;
                    const float t = 0.5f + 0.5f * globalSin * sign;  // [-1,1] sine -> [0,1]
                    scale = kWaveScaleMin + (kWaveScaleMax - kWaveScaleMin) * t;  // -0.75x .. 1.25x
                    if (scale <= 0.0f) continue;  // dip below zero: dot is invisible
                }

                draw->AddCircleFilled({px, py}, radius * mask * scale, IM_COL32(red, green, blue, maskedAlpha), 16);
            }
        }

        ImGui::Render();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    input.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
