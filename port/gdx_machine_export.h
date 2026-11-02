#pragma once

#include "gdx_machine_capture.h"

#include <atomic>
#include <filesystem>

namespace gdx::machine {

struct ExportResult {
    bool success = false;
    std::filesystem::path directory;
    std::string error;
};

ExportResult Export(const Mesh& mesh, const std::filesystem::path& parent, int character,
                    const std::atomic<bool>& cancel, std::atomic<unsigned>& progress);

}
