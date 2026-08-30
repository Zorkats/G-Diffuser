#include "gdx_hackmods_window.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "gdx_extract_launch.h" // GdxExtractBuildHackArchive

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

extern "C" int gdx_input_in_gameplay(void);

// Defined in port/gdx_menu.cpp. Declared here rather than pulled in through gdx_menu_internal.h,
// which is scoped to the menu shell and its registry; this window is neither.
namespace gdxmenu {
void GdxOpenFolder(const std::string& dir);
} // namespace gdxmenu

#ifdef _WIN32
namespace {
// Native open dialog; the same split the other windows make, where other platforms type or paste
// the path into the field instead.
bool GdxPickHackRom(char* outPath, size_t outCap) {
    wchar_t fileName[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"N64 ROM images (*.z64, *.n64, *.v64)\0*.z64;*.n64;*.v64\0"
                      L"All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open a patched ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return false;
    }
    return WideCharToMultiByte(CP_UTF8, 0, fileName, -1, outPath, (int) outCap, nullptr, nullptr) > 0;
}
} // namespace
#endif

void GdxHackModsWindow::Refresh() {
    mEntries = GdxHackModsScan();
    mSelected = GdxHackModsSelectedName();
    mActiveBoot = GdxHackModsActiveName();
    mRefreshed = true;
}

void GdxHackModsWindow::InitElement() {
    mStatus[0] = '\0';
    Refresh();
}

void GdxHackModsWindow::UpdateElement() {
    // The hack directory only changes when the player puts a file in it, so a one-shot refresh at
    // open time is enough; the Rescan button covers the rest.
    if (!mRefreshed) {
        Refresh();
    }
}

void GdxHackModsWindow::DrawBuildSection() {
    ImGui::Separator();
    ImGui::TextUnformatted("Build a hack from a patched ROM (EXPERIMENTAL)");

    const std::string& programDir = GdxHackModsProgramDir();
    if (programDir.empty()) {
        ImGui::TextDisabled("Unavailable this session: the program folder was not resolved.");
        return;
    }

    ImGui::TextWrapped("Point this at a ROM that already has the hack patched into it. G-Diffuser reads "
                       "that ROM with the base game's recipes and writes an .o2r into the folder above. "
                       "Your ROM is only read, never written.");
    ImGui::TextWrapped("Two limits worth knowing before you try it. G-Diffuser runs the decompiled game's "
                       "own code, so only assets and courses come across; whatever the hack changed in "
                       "code will not run. And a hack has no reference archive to be checked against, so "
                       "an asset that reads as valid but is wrong cannot be detected here. Booting it is "
                       "the only way to find out.");

    ImGui::InputTextWithHint("##hackrompath", "Path to a patched .z64 / .n64 / .v64...", mRomInput,
                             sizeof(mRomInput));
#ifdef _WIN32
    ImGui::SameLine();
    if (ImGui::Button("Browse...##hackrom")) {
        GdxPickHackRom(mRomInput, sizeof(mRomInput));
    }
#endif

    ImGui::InputTextWithHint("##hackname", "Name for it (this becomes the file name)...", mNameInput,
                             sizeof(mNameInput));

    const bool ready = mRomInput[0] != '\0' && mNameInput[0] != '\0';
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Build hack archive")) {
        // Synchronous on purpose. It takes a couple of seconds, and a brief freeze is honest; a
        // progress surface here would need its own state machine for no real gain.
        const gdx::HackBuildResult r = gdx::GdxExtractBuildHackArchive(
            mRomInput, mNameInput, GdxHackModsDirectory().c_str(), programDir.c_str());
        mBuildOk = r.ok;
        snprintf(mBuildStatus, sizeof(mBuildStatus), "%s", r.message.c_str());
        if (r.ok) {
            Refresh();
        }
    }
    ImGui::EndDisabled();
    if (!ready) {
        ImGui::SetItemTooltip("Choose a patched ROM and give it a name first.");
    }

    if (mBuildStatus[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, mBuildOk ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                                                      : ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", mBuildStatus);
        ImGui::PopStyleColor();
    }
}

void GdxHackModsWindow::DrawElement() {
    ImGui::TextWrapped("ROM hacks replace the game's own content, so they live apart from texture packs and only "
                       "one can be on at a time. Put a hack archive (.o2r) in mods/%s and pick it here.",
                       GDX_HACKMODS_DIR);

    if (GdxHackModsDirectory().empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "No mods/%s folder was found next to the game.", GDX_HACKMODS_DIR);
        ImGui::TextWrapped("Create it, drop a hack archive in, then restart. The folder is only looked for at "
                           "startup.");
        return;
    }

    ImGui::TextDisabled("Folder: %s", GdxHackModsDirectory().c_str());

    if (ImGui::Button("Rescan")) {
        Refresh();
        snprintf(mStatus, sizeof(mStatus), "Found %d hack archive%s.", (int) mEntries.size(),
                 mEntries.size() == 1 ? "" : "s");
    }
    ImGui::SameLine();
    // Installing a hack someone else built is entirely a file-copy job, so the folder should be one
    // click away rather than a path to select out of the disabled label above.
    if (ImGui::Button("Open folder")) {
        gdxmenu::GdxOpenFolder(GdxHackModsDirectory());
    }

    // Say plainly what is running right now, because the selection below is about the NEXT boot.
    ImGui::Separator();
    if (mActiveBoot.empty()) {
        ImGui::TextUnformatted("Running now: stock game (no hack).");
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Running now: %s", mActiveBoot.c_str());
        ImGui::TextDisabled("Progress is saved to saves/%s", gdx_hackmod_active_save_basename());
    }
    if (mSelected != mActiveBoot) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Restart to apply your new selection.");
    }

    if (mEntries.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("No .o2r archives in that folder yet.");
        DrawBuildSection();
        return;
    }

    ImGui::Separator();
    if (!ImGui::BeginTable("GdxHackModsTable", 3,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_SizingFixedFit,
                           ImVec2(0.0f, 200.0f))) {
        return;
    }
    ImGui::TableSetupColumn("Hack");
    ImGui::TableSetupColumn("Save file");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();

    // "Stock" is a real row rather than a Clear button, so turning a hack off is the same gesture
    // as turning one on.
    ImGui::PushID(-1);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("(none — stock game)");
    ImGui::TableNextColumn();
    ImGui::TextDisabled("saves/%s", GDX_HACKMOD_SAVE_STOCK);
    ImGui::TableNextColumn();
    if (mSelected.empty()) {
        ImGui::TextDisabled("selected");
    } else if (ImGui::SmallButton("Select")) {
        GdxHackModsSetSelected(std::string());
        Refresh();
        snprintf(mStatus, sizeof(mStatus), "Stock game selected. Restart to apply.");
    }
    ImGui::PopID();

    for (size_t i = 0; i < mEntries.size(); i++) {
        const GdxHackModEntry& row = mEntries[i];
        char save[GDX_HACKMOD_SAVE_MAX];
        const bool nameUsable = gdx_hackmod_save_basename(row.basename.c_str(), save, sizeof(save)) != 0;

        ImGui::PushID((int) i);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.basename.c_str());

        ImGui::TableNextColumn();
        if (nameUsable) {
            ImGui::TextDisabled("saves/%s", save);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "unusable name");
        }

        ImGui::TableNextColumn();
        if (!nameUsable) {
            // Refusing here rather than at boot: a hack whose name cannot produce its own save
            // file would otherwise have to share the stock one, which is exactly what the
            // isolation exists to prevent.
            ImGui::TextDisabled("cannot select");
            ImGui::SetItemTooltip("This archive's filename has no characters usable in a save-file name. "
                                  "Rename it to letters, digits, dots, dashes or underscores.");
        } else if (mSelected == row.basename) {
            ImGui::TextDisabled("selected");
        } else if (ImGui::SmallButton("Select")) {
            GdxHackModsSetSelected(row.basename);
            Refresh();
            snprintf(mStatus, sizeof(mStatus), "'%s' selected. Restart to apply.", row.basename.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndTable();

    if (mStatus[0] != '\0') {
        ImGui::Separator();
        ImGui::TextWrapped("%s", mStatus);
    }

    DrawBuildSection();
}
