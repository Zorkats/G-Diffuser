#include "gdx_user_folders.h"

namespace gdx {
bool EnsureUserFolders(const std::filesystem::path& dataRoot, const std::filesystem::path& exportRoot,
                       std::string& errors) {
    errors.clear();
    if (dataRoot.empty() || !dataRoot.is_absolute() || exportRoot.empty() || !exportRoot.is_absolute()) {
        errors = "The data and export directories must be absolute paths.";
        return false;
    }
    // Music lookup stays beside the executable even when the game-data root is redirected.
    // Downloaded tracks are a cache, never a save, so they sit beside mods/ rather than under a save path.
    const std::filesystem::path folders[] = {
        dataRoot / "mods/~romhacks", exportRoot.parent_path() / "music", exportRoot / "machines",
        dataRoot / "online/cache/tracks"
    };
    for (const auto& folder : folders) {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec) {
            if (!errors.empty()) {
                errors += '\n';
            }
            errors += folder.string() + ": " + ec.message();
        }
    }
    return errors.empty();
}
}
