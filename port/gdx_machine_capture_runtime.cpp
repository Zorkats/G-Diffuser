#include "gdx_machine_capture_runtime.h"
#include "gdx_machine_export.h"
#include "port_log.h"
#include "imgui.h"

#include <thread>

extern "C" const char* gMachineNames[];

namespace {
gdx::machine::Cache cache;

struct ExportJob {
    std::thread worker;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{true};
    std::atomic<unsigned> progress{0};
    gdx::machine::ExportResult result;
    bool hasResult = false;
    ~ExportJob() {
        cancel.store(true);
        if (worker.joinable()) {
            worker.join();
        }
    }
};
ExportJob job;
int selectedCharacter = 0;
}

void GdxMachineCapture_Publish(int character, gdx::machine::Result result) {
    cache.Publish(character, std::move(result));
}

void GdxMachineCapture_DrawExports() {
    if (!ImGui::CollapsingHeader("Native machine models (OBJ + glTF)###NativeMachineExport")) {
        return;
    }
    ImGui::TextWrapped("Export the last native preparation of the primary model at LOD 1. "
                       "Enter machine selection/settings first if no capture is available.");
    if (ImGui::BeginCombo("Roster entry###NativeMachineSelection", gMachineNames[selectedCharacter])) {
        for (int i = 0; i < 30; ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(gMachineNames[i], selectedCharacter == i)) {
                selectedCharacter = i;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const auto snapshot = cache.Get(selectedCharacter);
    bool running = !job.done.load(std::memory_order_acquire);
    if (snapshot.mesh) {
        ImGui::Text("Captured: %zu triangles, %zu materials, %zu KiB", snapshot.mesh->faces.size(),
                    snapshot.mesh->materials.size(), snapshot.mesh->RetainedBytes() / 1024);
    } else {
        ImGui::TextWrapped("Unavailable: %s", snapshot.error.empty() ? "not prepared in this session" : snapshot.error.c_str());
    }
    ImGui::BeginDisabled(running || !snapshot.mesh);
    if (ImGui::Button("Export cached native model###NativeMachineExportStart")) {
        if (job.worker.joinable()) {
            job.worker.join();
        }
        char path[4096];
        const std::filesystem::path parent = gdx_exe_relative_path(path, sizeof(path), "exports/machines");
        job.cancel.store(false);
        job.progress.store(0);
        job.hasResult = false;
        job.done.store(false, std::memory_order_release);
        try {
            job.worker = std::thread([mesh = snapshot.mesh, parent, character = selectedCharacter]() {
                job.result = gdx::machine::Export(*mesh, parent, character, job.cancel, job.progress);
                job.done.store(true, std::memory_order_release);
            });
            running = true;
        } catch (const std::exception& e) {
            job.result = {false, {}, e.what()};
            job.hasResult = true;
            job.done.store(true, std::memory_order_release);
            running = false;
        }
    }
    ImGui::EndDisabled();
    if (running) {
        ImGui::ProgressBar(job.progress.load() / 100.0f);
        if (ImGui::Button("Cancel export###NativeMachineExportCancel")) {
            job.cancel.store(true);
        }
    } else if (job.worker.joinable()) {
        job.worker.join();
        job.hasResult = true;
    }
    if (!running && job.hasResult) {
        if (job.result.success) {
            ImGui::TextWrapped("Exported to %s. Import machine.gltf for full material attributes.",
                               job.result.directory.string().c_str());
        } else {
            ImGui::TextWrapped("Export failed: %s", job.result.error.c_str());
            if (!job.result.directory.empty()) {
                ImGui::TextWrapped("Partial files remain in %s", job.result.directory.string().c_str());
            }
        }
    }
    ImGui::TextDisabled("Native capture only; static-body replacement export remains in Asset Dump.");
}
