#pragma once

#include <filesystem>
#include <string>

namespace gdx {
bool EnsureUserFolders(const std::filesystem::path& dataRoot, const std::filesystem::path& exportRoot,
                       std::string& errors);
}
