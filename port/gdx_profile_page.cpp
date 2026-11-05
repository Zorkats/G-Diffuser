// port/gdx_profile_page.cpp -- see gdx_profile_page.h.
//
// The active profile is the one being edited, so the page has no second selection to keep in step.
// Every edit is written through gdx_course_state_flush. When a recovery or snapshot write holds the
// state files, the flush is retried once a second while the page stays open. Colour drags are written
// half a second after the last change, so a drag does not rewrite the file every frame.

#include "gdx_profile_page.h"

#include "gdx_course_sign.h"
#include "gdx_course_state.h"
#include "gdx_profile_state.h"

#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <cstring>

extern "C" const char* gMachineNames[];

namespace {

const ImVec4 kGreen = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
const ImVec4 kRed = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
const uint32_t kNewColour = 0x3A7BFFu;

char sStatus[160];
bool sStatusOk = false;
bool sFlushFailed = false;
double sFlushAt = -1.0; // when the next write is due, -1 for none

// "blue falcon" becomes "Blue Falcon".
const char* machine_label(int machine, char* buf, size_t cap) {
    if (machine < 0 || machine >= GDX_PROFILE_ROSTER_COUNT || gMachineNames[machine] == nullptr) {
        return "None";
    }
    std::snprintf(buf, cap, "%s", gMachineNames[machine]);
    bool start = true;
    for (char* c = buf; *c != '\0'; c++) {
        if (start) {
            *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
        }
        start = *c == ' ';
    }
    return buf;
}

void status(bool ok, const char* text) {
    sStatusOk = ok;
    std::snprintf(sStatus, sizeof sStatus, "%s", text);
}

void flush() {
    sFlushFailed = gdx_course_state_flush() < 0;
    sFlushAt = sFlushFailed ? ImGui::GetTime() + 1.0 : -1.0;
}

// Reports an edit's result and saves it when it changed the store.
void finish(int rc, const char* done) {
    if (rc >= 0) {
        status(true, done);
        flush();
        return;
    }
    switch (rc) {
        case GDX_PROFILE_ERR_VALUE:
            status(false, "Use 1 to 16 letters, digits or symbols (no accents), with no space at either end.");
            break;
        case GDX_PROFILE_ERR_FULL:
            status(false, "All 8 profile slots are used. Delete one first.");
            break;
        case GDX_PROFILE_STATE_ERR_RANDOM:
            status(false, "The system gave no random id for the profile. Try again.");
            break;
        default:
            status(false, "The profile could not be changed.");
            break;
    }
}

uint32_t to_rgb(const float c[3]) {
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        const float v = c[i] < 0.0f ? 0.0f : c[i] > 1.0f ? 1.0f : c[i];
        out = (out << 8) | static_cast<uint32_t>(v * 255.0f + 0.5f);
    }
    return out;
}

void from_rgb(uint32_t rgb, float c[3]) {
    c[0] = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    c[1] = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    c[2] = static_cast<float>(rgb & 0xFF) / 255.0f;
}

void draw_list(const GdxProfileStore* store) {
    for (int i = 0; i < store->count; i++) {
        const GdxProfile& p = store->profiles[i];
        float colour[3];
        from_rgb(p.colour, colour);
        ImGui::PushID(i);
        ImGui::ColorButton("##swatch", ImVec4(colour[0], colour[1], colour[2], 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker);
        ImGui::SameLine();
        if (ImGui::RadioButton(p.name, store->active == i) && store->active != i) {
            finish(gdx_profile_state_set_active(i), "Active profile changed.");
        }
        ImGui::PopID();
    }
}

// The fingerprint of the active profile's signing key, "" before it signs its first track. Cached per profile
// and key revision, so the key is not derived every frame.
const char* key_fingerprint(int active) {
    static char sFingerprint[20];
    static int sForProfile = -2;
    static unsigned long sForKeys = ~0ul;
    static unsigned long sForStore = ~0ul;
    if (sForProfile == active && sForKeys == gdx_profile_state_keys_revision() &&
        sForStore == gdx_profile_state_revision()) {
        return sFingerprint;
    }
    sForProfile = active;
    sForKeys = gdx_profile_state_keys_revision();
    sForStore = gdx_profile_state_revision();
    sFingerprint[0] = '\0';
    unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES];
    unsigned char publicKey[GDX_SIGN_PUBLIC_BYTES];
    unsigned char secretKey[GDX_SIGN_SECRET_BYTES];
    if (gdx_profile_state_active_has_seed() && gdx_profile_state_active_seed(seed) == GDX_PROFILE_OK &&
        gdx_sign_key_from_seed(seed, publicKey, secretKey) == 0) {
        gdx_sign_fingerprint(publicKey, sFingerprint);
    }
    gdx_profile_keys_wipe(seed, sizeof seed);
    gdx_profile_keys_wipe(secretKey, sizeof secretKey);
    return sFingerprint;
}

void draw_active(const GdxProfileStore* store) {
    static char sName[GDX_PROFILE_NAME_MAX + 1];
    static unsigned long sShownRevision = ~0ul;
    static bool sConfirmDelete = false;
    const int active = store->active;
    const GdxProfile& p = store->profiles[active];

    // Reload the fields whenever the store changed under them (another edit, or a restore).
    if (sShownRevision != gdx_profile_state_revision()) {
        std::snprintf(sName, sizeof sName, "%s", p.name);
        sShownRevision = gdx_profile_state_revision();
        sConfirmDelete = false;
    }

    ImGui::SeparatorText("Active profile");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    ImGui::InputText("##profilename", sName, sizeof sName);
    ImGui::SameLine();
    ImGui::BeginDisabled(std::strcmp(sName, p.name) == 0);
    if (ImGui::Button("Rename")) {
        finish(gdx_profile_state_rename(active, sName), "Profile renamed.");
    }
    ImGui::EndDisabled();

    float colour[3];
    from_rgb(p.colour, colour);
    if (ImGui::ColorEdit3("Colour##profilecolour", colour, ImGuiColorEditFlags_NoInputs) &&
        to_rgb(colour) != p.colour) {
        if (gdx_profile_state_set_colour(active, to_rgb(colour)) == GDX_PROFILE_OK) {
            sFlushAt = ImGui::GetTime() + 0.5;
            sStatus[0] = '\0';
        }
        sShownRevision = gdx_profile_state_revision(); // keep a name being typed
    }

    char label[32];
    char option[32];
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::BeginCombo("Machine##profilemachine", machine_label(p.preferredMachine, label, sizeof label))) {
        for (int m = -1; m < GDX_PROFILE_ROSTER_COUNT; m++) {
            if (ImGui::Selectable(machine_label(m, option, sizeof option), p.preferredMachine == m) &&
                p.preferredMachine != m) {
                finish(gdx_profile_state_set_machine(active, m), "Machine saved.");
                sShownRevision = gdx_profile_state_revision();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Your usual machine. Online rooms will show it next to your name.");

    const char* fingerprint = key_fingerprint(active);
    if (fingerprint[0] != '\0') {
        ImGui::TextDisabled("Signing key %s", fingerprint);
    } else {
        ImGui::TextDisabled("Signing key: made when you first export a track you saved");
    }
    ImGui::SetItemTooltip("Exported tracks you saved in Course Edit carry a signature from this key. Share the\n"
                          "key with players, so they can tell your tracks from copies that use your name.\n"
                          "The key itself stays in profile-keys.gpk: never share that file.");

    if (!sConfirmDelete) {
        if (ImGui::Button("Delete profile")) {
            sConfirmDelete = true;
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextWrapped("Delete %s? Tracks keep the name already saved in them. Its signing key is deleted "
                           "too, so tracks it signed can no longer be re-signed after an edit.", p.name);
        ImGui::PopStyleColor();
        if (ImGui::Button("Delete")) {
            finish(gdx_profile_state_remove(active), "Profile deleted.");
            sConfirmDelete = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep")) {
            sConfirmDelete = false;
        }
    }
}

void draw_create(const GdxProfileStore* store) {
    static char sNewName[GDX_PROFILE_NAME_MAX + 1];

    ImGui::SeparatorText("New profile");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    ImGui::InputTextWithHint("##newprofile", "Name", sNewName, sizeof sNewName);
    ImGui::SameLine();
    ImGui::BeginDisabled(sNewName[0] == '\0' || store->count >= GDX_PROFILE_CAPACITY);
    if (ImGui::Button("Create")) {
        const int rc = gdx_profile_state_create(sNewName, kNewColour, -1);
        finish(rc, store->count == 1 ? "Profile created. It is the active profile." : "Profile created.");
        if (rc >= 0) {
            sNewName[0] = '\0';
        }
    }
    ImGui::EndDisabled();
}

} // namespace

void GdxProfilePage_Draw() {
    gdx_course_boot(); // loads profiles.gps the first time
    const GdxProfileStore* store = gdx_profile_state_store();

    if (sFlushAt >= 0.0 && ImGui::GetTime() >= sFlushAt) {
        flush();
    }

    ImGui::TextWrapped("The active profile's name is the Author of tracks you save in Course Edit. When another "
                       "profile saves the same track, its name is shown as Edited by.");
    if (store->count == 0) {
        ImGui::TextDisabled("No profile yet. Tracks you save have no author.");
    } else {
        draw_list(store);
        draw_active(store);
    }
    draw_create(store);

    if (sStatus[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, sStatusOk ? kGreen : kRed);
        ImGui::TextWrapped("%s", sStatus);
        ImGui::PopStyleColor();
    }
    if (sFlushFailed) {
        ImGui::TextDisabled("Waiting for a track save to finish before writing profiles.gps.");
    }
}
