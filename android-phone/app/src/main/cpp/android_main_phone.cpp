// The entry of the Android phone build (flat screen; android-phone/).
//
// The same game libraries as the Quest build (android/app/src/main/cpp/android_main.cpp), with three differences:
// the window backend is game_window_phone.cpp (Vulkan on the activity's ANativeWindow, no OpenXR, no --vr), the
// looper thread hands the window and its loss to that backend (phone_host.h), and a disc is installed inside the app
// (DiscImportActivity, disc_import.cpp) instead of being pushed from a PC.
#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "android_host.h"
#include "disc_launch.h"
#include "game_main.h"
#include "game_window.h"
#include "pc_overlay.h"
#include "phone_host.h"
#include "game/shell/shared_vr_settings.h"
#include "game/shell/title_options.h"
#include "platform/os/paths.h"

namespace {
constexpr const char* kTag = "GT2.Phone";

// A new disc profile on the phone: the desktop's flat defaults, capped at 60 FPS (the physics runs at 30 Hz and the
// presentation is interpolated; 120 Hz panels would otherwise draw twice as many frames for little gain).
inline constexpr const char* kInitialSettings = R"settings(# gt2game settings (written by the title's options; the career save keeps the original options too)
units=kmh
music_volume=240
sfx_volume=192
course_map=1
view_angle=1
chase_view=0
camera_position=0
replay_info=0
vibration=1
trigger_pedals=1
rumble_scale=100
frame_rate=display
frame_cap=60
vsync=1
render_scale=100
render_height=0
msaa=2
texture_filter=smooth
texture_mapping=perspective
scenery_detail=original
draw_distance=all
)settings";
inline constexpr const char* kInitialOverlay = R"settings(# Live overlay preferences; separate from earned progress.
frame_rate=display
frame_cap=60
vsync=1
render_scale=100
render_height=0
msaa=2
texture_filter=smooth
texture_mapping=perspective
scenery_detail=original
draw_distance=all
rumble=100
unlock_courses=0
unlock_cars=0
profiler=0
unlock_sim_events=0
)settings";

void Command(android_app* app, int32_t command) {
    namespace host = gt2game::android;
    switch (command) {
    case APP_CMD_INIT_WINDOW: host::phone::SetWindow(app->window); break;
    case APP_CMD_TERM_WINDOW: host::phone::ReleaseWindow(); break; // returns once the game dropped its surface
    case APP_CMD_GAINED_FOCUS: host::phone::SetFocused(true); break;
    case APP_CMD_LOST_FOCUS: host::phone::SetFocused(false); break;
    case APP_CMD_RESUME: host::SetResumed(true); break;
    case APP_CMD_PAUSE: host::SetResumed(false); break;
    case APP_CMD_DESTROY: host::RequestQuit(); break;
    default: break;
    }
}

float Axis(const AInputEvent* event, int32_t axis) { return AMotionEvent_getAxisValue(event, axis, 0); }

int32_t Input(android_app*, AInputEvent* event) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        int code = AKeyEvent_getKeyCode(event);
        // System keys stay with Android: volume (and its overlay), power, assistant.
        if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN || code == AKEYCODE_VOLUME_MUTE ||
            code == AKEYCODE_POWER || code == AKEYCODE_HOME || code == AKEYCODE_ASSIST)
            return 0;
        if (code == AKEYCODE_BACK) {
            // Many Android pads send BACK for their Select / View button: it is the pad's Select there (so that
            // Start + Select opens the settings). The phone's own back gesture opens and closes the settings menu.
            const int32_t source = AInputEvent_getSource(event);
            const bool pad = (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
                             (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
            code = pad ? AKEYCODE_BUTTON_SELECT : AKEYCODE_F10;
        }
        const int32_t action = AKeyEvent_getAction(event);
        if (action == AKEY_EVENT_ACTION_MULTIPLE) return 1;
        gt2game::android::KeyEvent(code, action == AKEY_EVENT_ACTION_DOWN);
        return 1;
    }
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION &&
        (AInputEvent_getSource(event) & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        // Android pads report the analogue triggers either as LTRIGGER/RTRIGGER or as BRAKE/GAS.
        const float left = std::max(Axis(event, AMOTION_EVENT_AXIS_LTRIGGER), Axis(event, AMOTION_EVENT_AXIS_BRAKE));
        const float right = std::max(Axis(event, AMOTION_EVENT_AXIS_RTRIGGER), Axis(event, AMOTION_EVENT_AXIS_GAS));
        gt2game::android::MotionEvent(Axis(event, AMOTION_EVENT_AXIS_X), Axis(event, AMOTION_EVENT_AXIS_Y), Axis(event, AMOTION_EVENT_AXIS_Z),
                                      Axis(event, AMOTION_EVENT_AXIS_RZ), left, right, Axis(event, AMOTION_EVENT_AXIS_HAT_X),
                                      Axis(event, AMOTION_EVENT_AXIS_HAT_Y));
        return 1;
    }
    return 0; // touch: the system's (no touch controls in this version)
}

// The settings' "Import another game disc..." returns here: the Java activity opens the disc picker of Android.
void OpenDiscImport(android_app* app, JNIEnv* env) {
    if (!env) return;
    jobject activity = app->activity->clazz;
    jclass type = env->GetObjectClass(activity);
    jmethodID method = type ? env->GetMethodID(type, "openDiscImport", "()V") : nullptr;
    if (method) env->CallVoidMethod(activity, method);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (type) env->DeleteLocalRef(type);
}

bool Installed(const std::filesystem::path& root, const std::string& key) {
    return std::filesystem::exists(root / gt2game::DescribeDiscSelection(key).folder / "disc.raw2352");
}

void RunGame(android_app* app, JNIEnv* env) {
    using namespace gt2game;
    const auto root = gt2::os::DataRoot();
    const auto saves = gt2::os::SavesDir();
    SetDiscImportAvailable(true);
    std::string preferred = "arcade";
    std::ifstream(saves / "last-disc.txt") >> preferred;
    if (!ValidDiscSelection(preferred)) preferred = "arcade";
    gt2::shell::InitializeSharedVrSettings(saves, kInitialSettings, kInitialOverlay);
    LoadOverlaySettings((saves / DescribeDiscSelection(preferred).saveMode / "settings.txt").string());
    bool firstLaunch = true, directLaunch = false;
    for (;;) {
        std::string mode;
        if (directLaunch && Installed(root, preferred)) mode = preferred; // combined title <-> Arcade menus
        else mode = SelectGameDisc(root.string(), preferred, false, false, {}, {}, true, firstLaunch);
        directLaunch = false;
        if (mode.empty()) break;
        firstLaunch = false;
        preferred = mode;
        gt2::shell::WritePreferences(saves / "last-disc.txt", mode + "\n");
        const auto modeSaves = saves / DescribeDiscSelection(mode).saveMode;
        std::filesystem::create_directories(modeSaves);
        // Seed only a new disc profile; updates keep the player's saved preferences.
        if (!std::filesystem::exists(modeSaves / "settings.txt") && !std::filesystem::exists(modeSaves / "settings.txt.overlay")) {
            for (const auto& [name, text] : {std::pair{"settings.txt", kInitialSettings}, std::pair{"settings.txt.overlay", kInitialOverlay}}) {
                std::ofstream out(modeSaves / name);
                out << text;
                out.flush();
                if (!out) throw std::runtime_error("Cannot write the initial settings");
            }
        }
        auto args = InstalledDiscArguments("gt2game", root, saves, mode);
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        const int code = GameMain(int(argv.size()), argv.data());
        __android_log_print(ANDROID_LOG_INFO, kTag, "GameMain returned %d", code);
        if (code != 0 || !TakeGameChangeRequest()) break;
        if (const std::string next = TakeGameChangePreference(); ValidDiscSelection(next)) preferred = next;
        directLaunch = TakeGameChangeDirect();
        SetFakePadScript({});
        SetFakePad2Script({});
    }
    ReleaseRetainedWindow();
    if (TakeDiscImportRequest()) OpenDiscImport(app, env);
}

} // namespace

void android_main(android_app* app) {
    app->onAppCmd = Command;
    app->onInputEvent = Input;
    gt2game::android::SetHost(app->activity->vm, app->activity->clazz);
    gt2::os::SetAndroidDirs(app->activity->externalDataPath, app->activity->internalDataPath);
    std::freopen(gt2::os::LogPath().string().c_str(), "w", stdout);
    std::freopen((gt2::os::SavesDir().parent_path() / "gt2game-error.log").string().c_str(), "w", stderr);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("android: phone activity started\n");
    std::atomic<bool> finished{false};
    std::thread game;
    bool started = false;
    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int result = ALooper_pollOnce(20, nullptr, &events, reinterpret_cast<void**>(&source));
        if (result >= 0 && source) source->process(app, source);
        if (!started && app->window && gt2game::android::Resumed()) {
            started = true;
            game = std::thread([&] {
                JNIEnv* env = nullptr;
                const bool attached = app->activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
                try {
                    RunGame(app, attached ? env : nullptr);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "Startup failed: %s\n", e.what());
                    __android_log_print(ANDROID_LOG_ERROR, kTag, "Startup failed: %s", e.what());
                }
                gt2game::ReleaseRetainedWindow();
                if (attached) app->activity->vm->DetachCurrentThread();
                finished = true;
                ALooper_wake(app->looper);
            });
        }
        if (finished) {
            ANativeActivity_finish(app->activity);
            finished = false; // finish once; the loop ends with destroyRequested
        }
    }
    gt2game::android::RequestQuit();
    if (game.joinable()) game.join();
}
