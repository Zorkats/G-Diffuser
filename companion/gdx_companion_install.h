// Copyright (c) Zorkats
//
// Finds the game installation the Companion works with. The order is: the folder the user chose in
// Settings, then the Companion's own folder, then the folder above it (an install with the Companion in
// a companion subfolder). The choice is kept in companion-settings.json beside the Companion. Nothing
// here depends on SDL or ImGui.
#pragma once

#include <filesystem>
#include <string>

namespace gdx_companion {

enum class InstallState { Unknown, Validated, Unavailable };

enum class InstallSource { None, Chosen, Beside, Parent };

struct InstallCheck {
    InstallState state = InstallState::Unknown;
    InstallSource source = InstallSource::None;
    std::filesystem::path game_dir;  // the folder that holds the game executable; empty when Unavailable
    std::string detail;              // one or two sentences for Settings and the log
};

// companion-settings.json beside the Companion.
std::filesystem::path InstallSettingsPath(const std::filesystem::path& exe_dir);

// Reads the chosen folder. A missing file, or a file without the key, gives an empty dir and returns true.
// A file that cannot be read or parsed returns false with error set and dir empty.
bool ReadSavedInstallation(const std::filesystem::path& settings, std::filesystem::path& dir, std::string& error);

// Writes the chosen folder through a temporary file. An empty dir removes the setting. Returns false with
// error set when the file cannot be written.
bool WriteSavedInstallation(const std::filesystem::path& settings, const std::filesystem::path& dir,
                            std::string& error);

// Turns what the user picked (the game executable, or the folder that holds it) into the installation
// folder. Returns false with error set when the pick is neither.
bool InstallationFromChoice(const std::filesystem::path& picked, std::filesystem::path& dir, std::string& error);

// Looks for the game in the order above. saved_dir is the chosen folder, or empty.
InstallCheck CheckInstallation(const std::filesystem::path& exe_dir, const std::filesystem::path& saved_dir);

}  // namespace gdx_companion
