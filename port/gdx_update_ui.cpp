#include "gdx_update_ui.h"

#include <cstdio>
#include <cstring>

#include <SDL2/SDL.h>
#include <imgui.h>

#include "gdx_update.h"
#include "libultraship/bridge/consolevariablebridge.h" // CVarGetInteger, CVarGetString

namespace {

constexpr const char* kAutoCheckCVar = "gEnhancements.Updates.AutoCheck";
constexpr const char* kLastSeenCVar = "gEnhancements.Updates.LastSeenVersion";

GdxUpdateStatus sStatus;

void PollStatus() {
    gdx_update_poll(&sStatus);
}

const char* StateText(const GdxUpdateStatus& status) {
    switch (status.state) {
        case GDX_UPDATE_IDLE:
            return "Not checked yet";
        case GDX_UPDATE_CHECKING:
            return "Checking...";
        case GDX_UPDATE_UP_TO_DATE:
            return "Up to date";
        case GDX_UPDATE_AVAILABLE:
            return "Update available";
        case GDX_UPDATE_DOWNLOADING:
            return "Downloading...";
        case GDX_UPDATE_VERIFYING:
            return "Verifying...";
        case GDX_UPDATE_READY:
            return "Ready to install";
        case GDX_UPDATE_ERROR:
            return "Update failed";
        case GDX_UPDATE_DISABLED:
            return "Updates are off for this build";
    }
    return "";
}

void DrawProgress(const GdxUpdateStatus& status) {
    if (status.bytesTotal > 0) {
        char overlay[64];
        std::snprintf(overlay, sizeof overlay, "%.1f / %.1f MB", status.bytesDone / 1048576.0,
                      status.bytesTotal / 1048576.0);
        ImGui::ProgressBar(static_cast<float>(static_cast<double>(status.bytesDone) / status.bytesTotal),
                           ImVec2(-1.0f, 0.0f), overlay);
    } else {
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.0f, 0.0f), "");
    }
}

void MarkSeen(const GdxUpdateStatus& status) {
    CVarSetString(kLastSeenCVar, status.latestVersion);
    CVarSave();
}

} // namespace

void GdxUpdateBoot() {
    CVarRegisterInteger(kAutoCheckCVar, 1);
    gdx_update_cleanup_on_boot();
    if (CVarGetInteger(kAutoCheckCVar, 1) != 0) {
        gdx_update_start_check(0);
    }
}

void GdxUpdateDrawAboutSection() {
    PollStatus();
    const GdxUpdateStatus& status = sStatus;

    ImGui::TextDisabled("Updates");
    bool autoCheck = CVarGetInteger(kAutoCheckCVar, 1) != 0;
    if (ImGui::Checkbox("Check for updates at launch", &autoCheck)) {
        CVarSetInteger(kAutoCheckCVar, autoCheck ? 1 : 0);
        CVarSave();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Asks GitHub for the latest release of this build's channel when the game starts.\n"
                          "Nothing is downloaded until you choose Download and install.");
    }

    if (status.latestVersion[0] != '\0') {
        ImGui::Text("%s: %s", StateText(status), status.latestVersion);
    } else {
        ImGui::TextUnformatted(StateText(status));
    }

    const bool busy = status.state == GDX_UPDATE_CHECKING || status.state == GDX_UPDATE_DOWNLOADING ||
                      status.state == GDX_UPDATE_VERIFYING;
    if (status.state == GDX_UPDATE_DOWNLOADING || status.state == GDX_UPDATE_VERIFYING) {
        DrawProgress(status);
    }
    if (status.state == GDX_UPDATE_ERROR && status.errorMessage[0] != '\0') {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", status.errorMessage);
    }

    if (status.state != GDX_UPDATE_DISABLED) {
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Check now")) {
            gdx_update_start_check(1);
        }
        ImGui::EndDisabled();
    }
    if (status.state == GDX_UPDATE_AVAILABLE) {
        ImGui::SameLine();
        if (ImGui::Button("Download and install")) {
            MarkSeen(status);
            gdx_update_start_download();
        }
    }
    if (status.state == GDX_UPDATE_READY) {
        ImGui::SameLine();
        if (ImGui::Button("Restart and update")) {
            gdx_update_apply_and_restart();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Closes the game, installs the update and starts the game again.\n"
                              "Your saves, settings and courses are kept.");
        }
    }
    const char* page = gdx_update_release_page_url();
    if (page != nullptr && page[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::Button("Release page")) {
            SDL_OpenURL(page);
        }
    }

    if ((status.state == GDX_UPDATE_AVAILABLE || status.state == GDX_UPDATE_READY ||
         status.state == GDX_UPDATE_DOWNLOADING || status.state == GDX_UPDATE_VERIFYING) &&
        status.releaseNotes[0] != '\0' && ImGui::CollapsingHeader("Release notes")) {
        ImGui::BeginChild("##GdxUpdateNotes", ImVec2(0.0f, 180.0f), true);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(status.releaseNotes);
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    }
}

GdxUpdateNotice::GdxUpdateNotice() : Ship::GuiWindow("gOpenWindows.UpdateNotice", true, "Update Notice") {
}

void GdxUpdateNotice::InitElement() {
}

void GdxUpdateNotice::UpdateElement() {
}

void GdxUpdateNotice::Draw() {
    PollStatus();
    if (sStatus.state != GDX_UPDATE_AVAILABLE ||
        std::strcmp(CVarGetString(kLastSeenCVar, ""), sStatus.latestVersion) == 0) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 bottomRight(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f,
                             viewport->WorkPos.y + viewport->WorkSize.y - 12.0f);
    ImGui::SetNextWindowPos(bottomRight, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##GdxUpdateNotice", nullptr, flags)) {
        DrawElement();
    }
    ImGui::End();
}

void GdxUpdateNotice::DrawElement() {
    ImGui::Text("G-Diffuser %s is available", sStatus.latestVersion);
    ImGui::TextDisabled("Or later from the menu: Settings, General, About G-Diffuser");
    if (ImGui::Button("Download and install")) {
        MarkSeen(sStatus);
        gdx_update_start_download();
    }
    ImGui::SameLine();
    if (ImGui::Button("Later")) {
        MarkSeen(sStatus);
    }
}
