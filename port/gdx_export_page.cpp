// port/gdx_export_page.cpp -- see gdx_export_page.h.
//
// The page lists the install once when it is first drawn and again on Refresh, so an open menu does not walk the
// folders every frame. The zip is written on a worker thread; the page shows its progress and keeps the controls
// disabled until it ends. Signing keys are never picked by default, and picking them shows why that matters.

#include "gdx_export_page.h"

#include "gdx_content_io.h"
#include "gdx_export_all.h"
#include "gdx_export_archive.h"
#include "port_log.h"

#include "libultraship/bridge/consolevariablebridge.h"
#include "ship/Context.h"

#include "imgui.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace {

const ImVec4 kGreen = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
const ImVec4 kRed = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
const ImVec4 kAmber = ImVec4(1.0f, 0.75f, 0.3f, 1.0f);

const unsigned kGroups[] = { GDX_EXPORT_TRACKS, GDX_EXPORT_PLAYER, GDX_EXPORT_SETTINGS, GDX_EXPORT_KEYS };
const char* const kGroupTips[] = {
    "Your Course Edit tracks on the disk save, their recovery copies and snapshots, track settings,\n"
    "forks and creator data, and the .gdxc and .gdxp files in exports/.",
    "Player profiles, the cartridge saves (stock and per ROM hack), achievements, saved ghosts\n"
    "and Continue Cup and Death Race records.",
    "gdiffuser.cfg.json (every menu setting and controller binding), the menu layout, machine palettes\n"
    "and stats, every pack in mods/ (texture, model, audio and Lua packs) and the music/ folder.\n"
    "ROM hack builds in mods/~romhacks/ stay out: they are rebuilt from your own hack ROM.",
    "profile-keys.gpk, the private keys your profiles sign exported tracks with.",
};

struct Job {
    std::mutex lock;
    std::atomic<bool> running{ false };
    std::atomic<double> progress{ 0.0 };
    bool finished = false; // guarded by lock
    bool ok = false;
    std::string message;
};

Job sJob;
gdx::ExportPlan sPlan;
bool sPlanned = false;
bool sPick[4] = { true, true, true, false };
std::string sStatus;
bool sStatusOk = false;

std::filesystem::path exe_folder() {
    char buffer[4096];
    const char* path = gdx_exe_relative_path(buffer, sizeof(buffer), "");
    std::error_code ec;
    return std::filesystem::absolute(std::filesystem::path(path), ec).lexically_normal();
}

void plan_now() {
    sPlan = gdx::PlanExport(std::filesystem::path(Ship::Context::GetAppDirectoryPath()), exe_folder());
    sPlanned = true;
}

unsigned picked_groups() {
    unsigned groups = 0;
    for (int i = 0; i < 4; i++) {
        if (sPick[i]) {
            groups |= kGroups[i];
        }
    }
    return groups;
}

std::string size_text(uint64_t bytes) {
    char text[32];
    if (bytes >= 1024ull * 1024ull) {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(text, sizeof(text), "%u bytes", static_cast<unsigned>(bytes));
    }
    return text;
}

void on_progress(double done, void*) {
    sJob.progress.store(done);
}

void start_export() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char name[64];
    if (gdx_export_file_name(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
                             local.tm_sec, name, sizeof(name)) < 0) {
        sStatus = "Could not name the export from the clock.";
        sStatusOk = false;
        return;
    }
    // Settings changed in the menu are written now, so the zip has them.
    CVarSave();
    plan_now();
    const std::filesystem::path zipPath = exe_folder() / "exports" / name;
    const unsigned groups = picked_groups();

    sJob.progress.store(0.0);
    sJob.running.store(true);
    {
        std::lock_guard<std::mutex> guard(sJob.lock);
        sJob.finished = false;
    }
    sStatus.clear();
    // Detached: quitting mid-write leaves only libzip's temporary file, never a half zip at the export's name.
    std::thread([plan = sPlan, groups, zipPath]() {
        std::string error;
        const bool ok = gdx::WriteExport(plan, groups, zipPath, on_progress, nullptr, error);
        gdx_port_logf("[export] %s: %s\n", zipPath.string().c_str(), ok ? "written" : error.c_str());
        std::lock_guard<std::mutex> guard(sJob.lock);
        sJob.ok = ok;
        sJob.message = ok ? ("Exported " + std::to_string(gdx::ExportCount(plan, groups)) + " files to exports/" +
                             zipPath.filename().string() + ". Unzip it over another G-Diffuser folder to move in.")
                          : error;
        sJob.finished = true;
        sJob.running.store(false);
    }).detach();
}

void collect_job() {
    std::lock_guard<std::mutex> guard(sJob.lock);
    if (!sJob.finished) {
        return;
    }
    sJob.finished = false;
    sStatus = sJob.message;
    sStatusOk = sJob.ok;
    sPlanned = false; // the zip is new in exports/, so list again
}

} // namespace

void GdxExportPage_Draw() {
    collect_job();
    const bool running = sJob.running.load();
    if (!sPlanned && !running) {
        plan_now();
    }

    ImGui::TextWrapped("Write one zip of everything you made and set up here, to back it up or move it to another "
                       "computer. It goes to exports/ next to the executable. ROMs, the game archives built from "
                       "them, ROM hack builds and machine model captures are never included.");

    ImGui::BeginDisabled(running);
    for (int i = 0; i < 4; i++) {
        const int index = i;
        const std::string label = std::string(gdx_export_group_name(kGroups[i])) + "##exportgroup" +
                                  std::to_string(i);
        ImGui::Checkbox(label.c_str(), &sPick[i]);
        ImGui::SetItemTooltip("%s", kGroupTips[i]);
        ImGui::SameLine();
        ImGui::TextDisabled("%u files, %s", static_cast<unsigned>(sPlan.count[index]),
                            size_text(sPlan.bytes[index]).c_str());
    }
    ImGui::EndDisabled();

    if (sPick[3]) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextWrapped("Signing keys: anyone who gets this zip can sign tracks as you, and players will trust "
                           "them as yours. Keep it private, and pick this only for your own backup or your own "
                           "next computer. Never share it.");
        ImGui::PopStyleColor();
    }
    if (!sPlan.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextWrapped("Some files could not be listed: %s", sPlan.error.c_str());
        ImGui::PopStyleColor();
    }

    const unsigned groups = picked_groups();
    const size_t count = gdx::ExportCount(sPlan, groups);
    const bool diskBusy = gdx_content_mfs_busy() != 0;

    ImGui::BeginDisabled(running || count == 0 || diskBusy);
    const std::string button = "Export " + std::to_string(count) + " files (" +
                               size_text(gdx::ExportBytes(sPlan, groups)) + ")##exportall";
    if (ImGui::Button(button.c_str())) {
        start_export();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Refresh##exportall")) {
        plan_now();
    }
    ImGui::EndDisabled();
    if (diskBusy && !running) {
        ImGui::TextDisabled("Waiting for the disk to finish saving.");
    }
    if (running) {
        ImGui::ProgressBar(static_cast<float>(sJob.progress.load()), ImVec2(-1.0f, 0.0f), "Writing the zip");
    }
    if (!sStatus.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, sStatusOk ? kGreen : kRed);
        ImGui::TextWrapped("%s", sStatus.c_str());
        ImGui::PopStyleColor();
    }
}
