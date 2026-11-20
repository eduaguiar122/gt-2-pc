// In-app disc installation for the phone build (DiscImportActivity.nativeInstall).
//
// The Quest receives its installed disc from the PC installer over adb; a phone has no PC in the loop. The activity
// copies the BIN the player picked (Storage Access Framework) into the app's data folder, then calls this, which does
// what tools/gt2install (extract) and tools/gt2linux/disc_store.h (PublishDisc) do on the desktop: identify the disc
// by its executable, extract the archive into assets/, keep the raw image as disc.raw2352 (XA music and movies are
// read from it) and publish the folder under arcade/, simulation/ or combined/ in one rename.
#include <jni.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "gt2formats/exe_profile.h"
#include "gt2formats/overlay_data.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

namespace fs = std::filesystem;

namespace {

fs::path SafePath(const fs::path& root, const std::string& name) {
    const fs::path relative(name);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || name.find(':') != std::string::npos ||
        name.find('\\') != std::string::npos)
        throw std::runtime_error("unsafe asset path in the disc archive");
    for (const auto& part : relative)
        if (part == ".." || part == ".") throw std::runtime_error("unsafe asset path in the disc archive");
    return root / relative;
}

// Returns the installed folder name. `bin` is moved into the installed folder on success and left in place on failure.
std::string Install(const fs::path& bin, const fs::path& root) {
    const fs::path staging = root / ".import-staging";
    std::string mode;
    {
        gt2::DiscImage disc(bin.string()); // rejects 2048-byte ISOs and truncated images
        const auto& profile = gt2::ProfileOf(disc); // rejects unsupported executables by SHA-1
        mode = gt2::IsCombinedDisc(disc) ? "combined" : profile.arcade ? "arcade" : "simulation";
        if (fs::exists(root / mode / "disc.raw2352"))
            throw std::runtime_error("This disc is already installed (" + mode + "). Existing data was kept.");
        gt2::GtfsVolume volume(disc);
        fs::remove_all(staging);
        for (const auto& e : volume.Files()) (void)SafePath(staging / "assets", e.path); // preflight every name
        fs::create_directories(staging / "assets");
        for (const auto& e : volume.Files()) {
            const fs::path path = SafePath(staging / "assets", e.path);
            fs::create_directories(path.parent_path());
            std::vector<uint8_t> bytes;
            try { bytes = volume.Read(e); }
            catch (const std::exception& ex) { throw std::runtime_error(e.path + ": " + ex.what()); }
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            out.flush();
            if (!out) throw std::runtime_error("cannot write " + e.path + " (is the phone's storage full?)");
        }
    } // the image is closed before it moves
    fs::rename(bin, staging / "disc.raw2352"); // same folder tree: no second 700 MB copy
    const fs::path target = root / mode;
    fs::remove_all(target); // only an incomplete folder can be here (no disc.raw2352)
    fs::rename(staging, target);
    return mode;
}

std::string Utf8(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return out;
}

} // namespace

// "OK:<arcade|simulation|combined>" or "ERR:<reason>".
extern "C" JNIEXPORT jstring JNICALL Java_io_github_gt2pc_phone_DiscImportActivity_nativeInstall(JNIEnv* env, jclass, jstring bin,
                                                                                                 jstring root) {
    std::string result;
    const fs::path rootPath = Utf8(env, root);
    try {
        result = "OK:" + Install(Utf8(env, bin), rootPath);
    } catch (const std::exception& e) {
        std::error_code ignored;
        fs::remove_all(rootPath / ".import-staging", ignored);
        result = std::string("ERR:") + e.what();
    }
    return env->NewStringUTF(result.c_str());
}
