// The window backend of the Android phone build (flat screen, android-phone/).
//
// The desktop SDL backend (tools/gt2game/game_window_sdl.cpp) is the model: one window, the shared Vulkan renderer
// presenting into its swapchain, the keyboard and the controllers. Here the window is the NativeActivity's
// ANativeWindow and the events come from android_main's looper thread (android_host.h, phone_host.h); the game runs on
// its own thread. The Quest backend (tools/gt2game/game_window_android.cpp) is not built into this library.
//
// Android destroys the window while the activity is in the background. Pump() is where the game's thread notices:
// it detaches the renderer, releases the surface, pauses the audio mix and waits until the activity is resumed with a
// new window, then creates the surface again on the same device. Nothing else of the renderer is rebuilt.
#include <vulkan/vulkan.h>

#include "game_window.h"

#include <android/log.h>
#include <android/native_window.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "android_activity_state.h"
#include "android_host.h"
#include "phone_host.h"
#include "game/audio/pause.h"
#include "gt2view/vk_context.h"
#include "platform/input/android_input.h"
#include "platform/os/keys.h"

namespace gt2game {
namespace {

using Clock = std::chrono::steady_clock;
constexpr const char* kTag = "GT2.Phone";

// ---------------------------------------------------------------- the host's side (android_main -> here)

struct HostState : android::ActivityState {
    std::mutex mutex;
    std::condition_variable changed;
    void* vm = nullptr;
    void* activity = nullptr;
    ANativeWindow* window = nullptr; // between INIT_WINDOW and TERM_WINDOW
    bool focused = false;
    bool surfaceAttached = false;    // the backend has a Vulkan surface on `window`
    bool releaseRequested = false;   // TERM_WINDOW is waiting for the backend to drop it
};

HostState& Host() {
    static HostState state;
    return state;
}

// Android key code (android/keycodes.h) -> a key code of platform/os/keys.h, 0 when the screens do not use it.
// A gamepad's buttons reach the game through the pad path (platform/input/android_input.h) and the pad-to-key merge of
// game_window.cpp; the D-pad keys also arrive here as arrows, as on the Quest.
int GameKey(int32_t androidKeyCode) {
    namespace keys = gt2::keys;
    switch (androidKeyCode) {
    case 4: return keys::kEscape;  // AKEYCODE_BACK: the system back gesture / button pauses and leaves menus
    case 19: return keys::kUp;     // AKEYCODE_DPAD_UP
    case 20: return keys::kDown;   // AKEYCODE_DPAD_DOWN
    case 21: return keys::kLeft;   // AKEYCODE_DPAD_LEFT
    case 22: return keys::kRight;  // AKEYCODE_DPAD_RIGHT
    case 23: return keys::kReturn; // AKEYCODE_DPAD_CENTER
    case 62: return keys::kSpace;  // AKEYCODE_SPACE
    case 66: return keys::kReturn; // AKEYCODE_ENTER
    case 67: return keys::kBack;   // AKEYCODE_DEL (backspace)
    case 111: return keys::kEscape;// AKEYCODE_ESCAPE
    case 112: return keys::kDelete;// AKEYCODE_FORWARD_DEL
    case 92: return keys::kPageUp;  // AKEYCODE_PAGE_UP
    case 93: return keys::kPageDown;// AKEYCODE_PAGE_DOWN
    case 122: return keys::kHome;   // AKEYCODE_MOVE_HOME
    case 59: case 60: return keys::kShift; // AKEYCODE_SHIFT_LEFT / _RIGHT
    case 136: return keys::kF5;    // AKEYCODE_F5
    case 141: return keys::kF10;   // AKEYCODE_F10
    default: break;
    }
    if (androidKeyCode >= 29 && androidKeyCode <= 54) return 'A' + (androidKeyCode - 29); // AKEYCODE_A .. AKEYCODE_Z
    if (androidKeyCode >= 7 && androidKeyCode <= 16) return '0' + (androidKeyCode - 7);   // AKEYCODE_0 .. AKEYCODE_9
    return 0;
}

} // namespace

namespace android {

void SetHost(void* javaVm, void* activity) {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    h.vm = javaVm;
    h.activity = activity;
    h.Reset();
    h.window = nullptr;
    h.focused = false;
    h.surfaceAttached = false;
    h.releaseRequested = false;
    gt2::input::android::Reset();
}

void* JavaVm() {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    return h.vm;
}

void* Activity() {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    return h.activity;
}

void SetResumed(bool resumed) {
    HostState& h = Host();
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        h.resumed = resumed;
    }
    h.changed.notify_all();
}

bool Resumed() {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    return h.resumed;
}

void RequestQuit() {
    HostState& h = Host();
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        h.quit = true;
    }
    h.changed.notify_all();
}

bool QuitRequested() {
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    return h.quit;
}

void KeyEvent(int32_t androidKeyCode, bool down) {
    gt2::input::android::KeyEvent(androidKeyCode, down); // the gamepad half of the same event
    const int key = GameKey(androidKeyCode);
    if (key <= 0 || key >= 256) return;
    HostState& h = Host();
    std::lock_guard<std::mutex> lock(h.mutex);
    if (down && !h.down[size_t(key)]) h.keyDowns.push_back(key);
    h.down[size_t(key)] = down;
}

void MotionEvent(float leftX, float leftY, float rightX, float rightY, float leftTrigger, float rightTrigger, float hatX, float hatY) {
    gt2::input::android::Axes axes;
    axes.leftX = leftX;
    axes.leftY = leftY;
    axes.rightX = rightX;
    axes.rightY = rightY;
    axes.leftTrigger = leftTrigger;
    axes.rightTrigger = rightTrigger;
    axes.hatX = hatX;
    axes.hatY = hatY;
    gt2::input::android::MotionEvent(axes);
}

namespace phone {

void SetWindow(void* nativeWindow) {
    HostState& h = Host();
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        h.window = static_cast<ANativeWindow*>(nativeWindow);
    }
    h.changed.notify_all();
}

void ReleaseWindow() {
    HostState& h = Host();
    std::unique_lock<std::mutex> lock(h.mutex);
    if (h.surfaceAttached) {
        h.releaseRequested = true;
        h.changed.notify_all();
        if (!h.changed.wait_for(lock, std::chrono::seconds(3), [&] { return !h.surfaceAttached; }))
            __android_log_print(ANDROID_LOG_WARN, kTag, "window: the game did not release the surface in time");
    }
    h.window = nullptr;
    // A surface still attached keeps releaseRequested: the next Pump drops it (the renderer tolerates the lost surface).
    for (auto& d : h.down) d = false;
}

void SetFocused(bool focused) {
    HostState& h = Host();
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        h.focused = focused;
        if (!focused) for (auto& d : h.down) d = false;
    }
    h.changed.notify_all();
}

} // namespace phone
} // namespace android

namespace {

// ---------------------------------------------------------------- the backend

class PhoneWindow final : public WindowBackend {
public:
    explicit PhoneWindow(bool render) {
        if (!render) throw std::runtime_error("the phone build has no window without a renderer");
        ANativeWindow* window = WaitForWindow();
        if (!window) throw std::runtime_error("the activity closed before it had a window");
        context_ = std::make_unique<gt2view::VkContext>(nullptr, window);
        renderer_ = std::make_unique<gt2view::VkSceneRenderer>(*context_);
        SetAttached(true);
    }
    ~PhoneWindow() override {
        try { if (renderer_) renderer_->WaitFrame(); }
        catch (const std::exception& e) { std::fprintf(stderr, "graphics: cleanup after failure: %s\n", e.what()); }
        renderer_.reset();
        context_.reset();
        SetAttached(false);
    }

    gt2view::VkSceneRenderer& Renderer() override { return *renderer_; }
    void* NativeHandle() const override { return nullptr; } // the controllers come from the activity's input queue
    void SetTitle(const std::string& title) override { __android_log_print(ANDROID_LOG_INFO, kTag, "screen: %s", title.c_str()); }

    void Pump() override {
        HostState& h = Host();
        bool release = false, foreground = false;
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            release = h.releaseRequested;
            foreground = h.resumed && h.window && !h.quit;
        }
        if (release) DropSurface();
        if (!attached_ || !foreground) WaitForForeground();
    }
    bool TakeTimingReset() override { return std::exchange(timingReset_, false); }
    bool Closed() const override { return closed_ || android::QuitRequested(); }
    void Close() override { closed_ = true; }
    bool Focused() const override {
        HostState& h = Host();
        std::lock_guard<std::mutex> lock(h.mutex);
        return attached_ && h.resumed && h.focused;
    }
    std::vector<int> TakeKeyDowns() override {
        HostState& h = Host();
        std::lock_guard<std::mutex> lock(h.mutex);
        return std::exchange(h.keyDowns, {});
    }
    bool KeyDown(int key) const override {
        if (key <= 0 || key >= 256) return false;
        HostState& h = Host();
        std::lock_guard<std::mutex> lock(h.mutex);
        return h.down[size_t(key)];
    }
    bool TakeDevicesChanged() override { return false; } // the Android pad appears through its first event
    void SleepUntil(Clock::time_point t) override {
        if (t - Clock::now() > std::chrono::milliseconds(1)) std::this_thread::sleep_until(t - std::chrono::microseconds(200));
        while (Clock::now() < t) std::this_thread::yield();
    }
    // No vblank timestamps: the common pacing uses its steady-clock fallback; FIFO presentation paces with vsync on.
    bool VBlankTiming(Clock::time_point&, Clock::duration&) const override { return false; }

private:
    static ANativeWindow* WaitForWindow() {
        HostState& h = Host();
        std::unique_lock<std::mutex> lock(h.mutex);
        h.changed.wait(lock, [&] { return h.quit || (h.window && h.resumed); });
        return h.quit ? nullptr : h.window;
    }
    void SetAttached(bool attached) {
        attached_ = attached;
        HostState& h = Host();
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            h.surfaceAttached = attached;
            if (!attached) h.releaseRequested = false;
        }
        h.changed.notify_all();
    }
    // TERM_WINDOW: no frame may be presented into this window any more.
    void DropSurface() {
        if (attached_) {
            renderer_->DetachSurface();
            context_->ReleaseWindowSurface();
            std::printf("graphics: window surface released (activity left the screen)\n");
        }
        SetAttached(false);
    }
    // Background / paused: the game's clock stops here, the audio mix is held, and the controller's state is cleared
    // so that no button stays held across the pause.
    void WaitForForeground() {
        gt2::audio::ScopedMixPause pause;
        HostState& h = Host();
        for (;;) {
            ANativeWindow* window = nullptr;
            {
                std::unique_lock<std::mutex> lock(h.mutex);
                h.changed.wait(lock, [&] { return h.quit || h.releaseRequested || (h.window && h.resumed); });
                if (h.quit) { closed_ = true; return; }
                if (!h.releaseRequested && attached_) return; // resumed on the same window (pause without TERM_WINDOW)
                if (!h.releaseRequested) window = h.window;
            }
            if (!window) { DropSurface(); continue; }
            try {
                context_->AttachWindowSurface(window);
                renderer_->AttachSurface(context_->Surface());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "graphics: cannot use the new window: %s\n", e.what());
                closed_ = true;
                return;
            }
            SetAttached(true);
            gt2::input::android::Reset();
            timingReset_ = true;
            return;
        }
    }

    std::unique_ptr<gt2view::VkContext> context_;
    std::unique_ptr<gt2view::VkSceneRenderer> renderer_;
    bool attached_ = false;
    bool closed_ = false;
    bool timingReset_ = false;
};

} // namespace

std::unique_ptr<WindowBackend> CreateWindowBackend(const std::string&, int, int) {
    return std::make_unique<PhoneWindow>(true);
}

std::unique_ptr<WindowBackend> CreateInputWindowBackend(const std::string&, int, int) {
    return std::make_unique<PhoneWindow>(false);
}

std::unique_ptr<WindowBackend> CreateXrWindowBackend(const std::string&, int, int) {
    throw std::runtime_error("the phone build plays on the flat screen only (no OpenXR)");
}

} // namespace gt2game
