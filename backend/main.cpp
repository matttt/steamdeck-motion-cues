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
    bool simulateMotion = false;
};

// Pixels of dot-field travel for one full wave-motion size cycle.
static constexpr float kWaveLengthPx = 100.0f;

// Wave-motion size envelope. Keep a positive floor so the two checkerboard
// phases overlap instead of leaving a blank handoff.
static constexpr float kWaveScaleMax = 1.25f;  // peak size multiplier
static constexpr float kWaveScaleMin = 0.35f;  // smallest visible size multiplier

// Dot-field physics tuning. Acceleration adds to velocity, drag bleeds it off,
// and the cap prevents sustained acceleration from becoming uncomfortable.
static constexpr float kDotAccelGain = 150.0f;
static constexpr float kDotVelocityDrag = 1.0f;
static constexpr float kDotMaxSpeed = 600.0f;

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float smoothstep(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

static inline float vecLength(Vec3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

static inline float dotVec(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline Vec3 crossVec(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static inline Vec3 scaleVec(Vec3 v, float scale) {
    return {v.x * scale, v.y * scale, v.z * scale};
}

static inline Vec3 lerpVec(Vec3 a, Vec3 b, float t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

static inline Vec3 withMagnitude(Vec3 v, float magnitude, Vec3 fallback) {
    const float len = vecLength(v);
    if (len <= 0.0001f) return fallback;
    return scaleVec(v, magnitude / len);
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
    settings.simulateMotion = readIntFlag(argc, argv, "--simulate-motion", settings.simulateMotion ? 1 : 0) != 0;
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
    static constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    static constexpr float kGyroCountToRad = (2000.0f / 32768.0f) * kDegToRad;
    static constexpr float kGravityAccelCorrection = 0.02f;
    static constexpr float kGyroDeadzoneRad = 0.015f;
    static constexpr float kMaxGyroDt = 0.05f;
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

    static float applyDeadzone(float value, float threshold) {
        return std::fabs(value) < threshold ? 0.0f : value;
    }

    static bool isAllZero(const SdHidFrame &f) {
        return f.AccelAxisRightToLeft == 0 && f.AccelAxisFrontToBack == 0 &&
               f.AccelAxisTopToBottom == 0 && f.GyroAxisRightToLeft == 0 &&
               f.GyroAxisFrontToBack == 0 && f.GyroAxisTopToBottom == 0;
    }

    static Vec3 gyroFromFrame(const SdHidFrame &f) {
        return {
            applyDeadzone(f.GyroAxisRightToLeft * kGyroCountToRad, kGyroDeadzoneRad),
            applyDeadzone(f.GyroAxisTopToBottom * kGyroCountToRad, kGyroDeadzoneRad),
            applyDeadzone(f.GyroAxisFrontToBack * kGyroCountToRad, kGyroDeadzoneRad)
        };
    }

    static Vec3 rotateGravityByGyro(Vec3 currentGravity, Vec3 gyro, float dt) {
        const float omega = vecLength(gyro);
        if (dt <= 0.0f || omega <= 0.0001f) return currentGravity;

        const Vec3 axis = scaleVec(gyro, 1.0f / omega);
        const float angle = omega * dt;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const float axisDotGravity = dotVec(axis, currentGravity);
        const Vec3 gravityCrossAxis = crossVec(currentGravity, axis);

        // Gravity is stored in device coordinates. As the device rotates, that
        // vector moves opposite the measured body angular velocity.
        return {
            currentGravity.x * c + gravityCrossAxis.x * s + axis.x * axisDotGravity * (1.0f - c),
            currentGravity.y * c + gravityCrossAxis.y * s + axis.y * axisDotGravity * (1.0f - c),
            currentGravity.z * c + gravityCrossAxis.z * s + axis.z * axisDotGravity * (1.0f - c)
        };
    }

    void processFrame(const SdHidFrame &f, double now) {
        Vec3 accel{
            f.AccelAxisRightToLeft * kCountToMs2,
            f.AccelAxisTopToBottom * kCountToMs2,
            f.AccelAxisFrontToBack * kCountToMs2
        };
        const float accelMag = vecLength(accel);
        if (accelMag <= 0.0001f) return;

        if (!gravityInit) {
            gravity = withMagnitude(accel, kG, {0.0f, 0.0f, kG});
            gravityInit = true;
        } else {
            const float dt = lastPacketTime > 0.0 ? clampf((float)(now - lastPacketTime), 0.0f, kMaxGyroDt) : 0.0f;
            gravity = rotateGravityByGyro(gravity, gyroFromFrame(f), dt);
            gravity = withMagnitude(gravity, kG, gravity);

            const float accelError = std::fabs(accelMag - kG);
            const float accelTrust = 1.0f - smoothstep(kG * 0.10f, kG * 0.45f, accelError);
            const float correction = kGravityAccelCorrection * accelTrust;
            const Vec3 accelGravity = withMagnitude(accel, kG, gravity);
            gravity = withMagnitude(lerpVec(gravity, accelGravity, correction), kG, gravity);
        }

        lastLinAccel = {accel.x - gravity.x, accel.y - gravity.y, accel.z - gravity.z};
        lastPacketTime = now;
    }

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
        SdHidFrame latestFrame{};
        bool haveLatestFrame = false;
        while ((r = read(fd, buf, sizeof(buf))) > 0) {
            if (r < (ssize_t)sizeof(SdHidFrame)) continue;
            const SdHidFrame *f = reinterpret_cast<const SdHidFrame *>(buf);

            const bool allZero = isAllZero(*f);
            if (allZero) {
                if (++zeroFrames > kZeroFramesToReenable) {
                    enableGyro();
                    zeroFrames = 0;
                }
            } else {
                zeroFrames = 0;
                latestFrame = *f;
                haveLatestFrame = true;
            }
        }

        if (haveLatestFrame) processFrame(latestFrame, now);
    }
};

static float pulseSegment(double t, double start, double end, float value) {
    if (t < start || t > end) return 0.0f;

    const double duration = end - start;
    const float fade = (float)std::min(0.6, duration * 0.45);
    const float fadeIn = smoothstep(0.0f, fade, (float)(t - start));
    const float fadeOut = 1.0f - smoothstep(0.0f, fade, (float)(t - (end - fade)));
    return value * std::min(fadeIn, fadeOut);
}

static Vec3 sampleSimulatedMotion(double elapsed) {
    const double cycle = 24.0;
    double t = std::fmod(elapsed, cycle);
    if (t < 0.0) t += cycle;

    Vec3 accel;

    // Body-space axes match the HID linear acceleration axes: +Y is forward/back,
    // +X is lateral. Coasting intervals intentionally produce zero acceleration.
    accel.y += pulseSegment(t, 0.0, 3.0, 2.1f);     // accelerate forward
    accel.y += pulseSegment(t, 5.2, 7.3, -2.4f);    // brake from forward speed
    accel.y += pulseSegment(t, 9.0, 11.5, -1.7f);   // accelerate backward
    accel.y += pulseSegment(t, 13.3, 15.0, 2.0f);   // brake while backing up
    accel.x += pulseSegment(t, 16.2, 19.1, 1.5f);   // steady-speed right turn
    accel.x += pulseSegment(t, 20.1, 23.0, -1.5f);  // steady-speed left turn

    return accel;
}

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
    if (settings.simulateMotion)
        std::cout << "Using simulated motion input." << std::endl;
    else
        input.init();

    float flowVelX = 0.f;
    float flowVelY = 0.f;
    float flowOffX = 0.f;
    float flowOffY = 0.f;
    float traveled = 0.f;  // accumulated dot-field travel distance, drives wave motion
    const double startTime = glfwGetTime();
    double lastTime = startTime;
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
        const bool ok = settings.simulateMotion || input.sample(now, lin);
        if (settings.simulateMotion)
            lin = sampleSimulatedMotion(now - startTime);
        const float pax = ok ? lin.x : 0.f;
        const float pay = ok ? lin.y : 0.f;
        const float frameDt = clampf(dt, 0.0f, 0.05f);
        const float response = settings.response;
        const float dotAccelGain = kDotAccelGain * response;

        flowVelX += -pax * dotAccelGain * frameDt;
        flowVelY += pay * dotAccelGain * frameDt;

        const float drag = std::exp(-kDotVelocityDrag * frameDt);
        flowVelX *= drag;
        flowVelY *= drag;

        const float flowSpeed = std::sqrt(flowVelX * flowVelX + flowVelY * flowVelY);
        if (flowSpeed > kDotMaxSpeed) {
            const float maxSpeedScale = kDotMaxSpeed / flowSpeed;
            flowVelX *= maxSpeedScale;
            flowVelY *= maxSpeedScale;
        }

        flowOffX += flowVelX * frameDt;
        flowOffY += flowVelY * frameDt;
        traveled += std::sqrt(flowVelX * flowVelX + flowVelY * flowVelY) * frameDt;

        if (debug && (now - lastLog) > 0.2) {
            lastLog = now;
            std::cerr << "[motion-cues] lin=(" << pax << "," << pay << "," << (ok ? lin.z : 0.f)
                      << ") response=" << response
                      << " flow=(" << flowVelX << "," << flowVelY << ")"
                      << (settings.simulateMotion ? " [simulated]" : ok ? "" : " [no device/stale]") << std::endl;
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

        // Wave motion: dots pulse in size by a raised sine wave.
        // Checkerboard neighbors run in opposite phase via a per-dot sign flip.
        // Tracking how many whole cells the field has scrolled (wraps) keeps each
        // physical dot's parity stable, so the checkerboard never snaps on wrap.
        const float wavePhase = 0.5f + 0.5f * std::sin(kPi * traveled / kWaveLengthPx);
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
                    const bool alternatePhase = ((absCol + absRow) & 1L) != 0;
                    const float t = alternatePhase ? 1.0f - wavePhase : wavePhase;
                    const float eased = smoothstep(0.0f, 1.0f, t);
                    scale = kWaveScaleMin + (kWaveScaleMax - kWaveScaleMin) * eased;
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

    if (!settings.simulateMotion)
        input.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
