// Copyright (c) Zorkats

#include "gdx_companion_pages.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "imgui.h"

#include "decoders.h"
#include "gdx_companion_loop.h"
#include "gdx_companion_loudness.h"
#include "gdx_companion_music.h"
#include "gdx_companion_packers.h"
#include "gdx_companion_paths.h"
#include "gdx_companion_project.h"
#include "gdx_companion_tracks.h"

namespace gdx_companion {

namespace fs = std::filesystem;

namespace {

constexpr char kNativeDirPrefix[] = "gdx-companion-native-";

struct Colors {
    ImVec4 text;
    ImVec4 secondary;
    ImVec4 error;
    ImVec4 ok;
    ImVec4 warning;
    ImVec4 accent;
    ImVec4 panel;
};

ImVec4 ToVec4(Rgb8 color) {
    return ImVec4(static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
                  static_cast<float>(color.b) / 255.0f, 1.0f);
}

int ResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

// InputText over a std::string, as imgui_stdlib does (that file is not compiled here).
bool InputString(const char* id, std::string& value, ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputText(id, value.data(), value.capacity() + 1, flags, ResizeCallback, &value);
}

void Wrapped(const ImVec4& color, const std::string& text) {
    if (text.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void Problem(const Colors& colors, const std::vector<FieldError>& errors, const char* field) {
    const FieldError* error = FindError(errors, field);
    if (error != nullptr) Wrapped(colors.error, error->message);
}

bool WorkerReady(const AppState& state) { return state.worker.state == WorkerState::Ready; }

bool JobBusy(const AppState& state) { return state.job.state == JobState::Running || state.job_request.has_value(); }

fs::path GameDir(const AppState& state) {
    if (state.installation != InstallState::Validated || state.installation_dir.empty()) return fs::path();
    return PathFromUtf8(state.installation_dir);
}

// The text field and Browse button of a path. Returns true when value changed.
bool PathField(AppState& state, const std::string& target, const char* label, std::string& value, BrowseKind kind,
               const char* filter_name = "", const char* filter_pattern = "") {
    bool changed = false;
    const auto result = state.browse_results.find(target);
    if (result != state.browse_results.end()) {
        value = result->second;
        state.browse_results.erase(result);
        changed = true;
    }
    ImGui::PushID(target.c_str());
    if (label != nullptr && label[0] != '\0') ImGui::TextUnformatted(label);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float browse = state.browse_available
                             ? ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x
                             : 0.0f;
    ImGui::SetNextItemWidth(browse > 0.0f ? -browse : -FLT_MIN);
    if (InputString("##path", value)) changed = true;
    if (state.browse_available) {
        ImGui::SameLine();
        if (ImGui::Button("Browse...") && !state.browse.has_value()) {
            BrowseRequest request;
            request.target = target;
            request.kind = kind;
            request.filter_name = filter_name;
            request.filter_pattern = filter_pattern;
            request.start = !value.empty() ? value : state.installation_dir;
            request.title = label != nullptr ? label : "";
            state.browse = request;
        }
    }
    ImGui::PopID();
    return changed;
}

void RequestJob(AppState& state, const std::string& owner, const JobRequest& request) {
    state.job_request = PageJobRequest{owner, request};
    if (state.job_request_error_owner == owner) {
        state.job_request_error.clear();
        state.job_request_error_owner.clear();
    }
}

// Why a job cannot start now, or empty.
std::string JobBlocker(const AppState& state) {
    if (!WorkerReady(state)) {
        if (state.worker.state == WorkerState::Stopped) return "The worker is stopped. Start it again below.";
        if (state.worker.state == WorkerState::Starting) return "The worker is starting.";
        return "The worker is not available. See Settings > Worker.";
    }
    if (JobBusy(state)) return "Another job is running.";
    return std::string();
}

// The finished job of owner, once.
const JobStatus* TakeFinished(const AppState& state, const std::string& owner, std::string& handled) {
    if (state.job_owner != owner || state.job.job.empty() || state.job.job == handled) return nullptr;
    if (state.job.state == JobState::Running || state.job.state == JobState::Idle) return nullptr;
    handled = state.job.job;
    return &state.job;
}

const char* OpLabel(const std::string& op) {
    if (op == "check") return "Validating";
    if (op == "plan") return "Planning";
    if (op == "build") return "Building";
    if (op == "init") return "Creating the project";
    if (op == "names") return "Loading the list";
    if (op == "inspect") return "Reading the pack";
    if (op == "unpack") return "Unpacking";
    return "Working";
}

void DrawJobPanel(AppState& state, const Colors& colors, const std::string& owner) {
    if (state.job_request_error_owner == owner && !state.job_request_error.empty()) {
        Wrapped(colors.error, "Not started: " + state.job_request_error);
    }
    if (state.job_owner != owner || state.job.state == JobState::Idle) return;
    switch (state.job.state) {
    case JobState::Running: {
        ImGui::TextUnformatted(OpLabel(state.job.op));
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-FLT_MIN, 0.0f), "Working");
        if (ImGui::Button("Cancel##job")) state.job_cancel = true;
        ImGui::SetItemTooltip("Stops the worker. Nothing is installed; a pack being replaced stays as it was.");
        break;
    }
    case JobState::Succeeded:
        Wrapped(colors.ok, state.job.detail);
        break;
    case JobState::Problems:
        Wrapped(colors.warning, state.job.detail);
        break;
    case JobState::Failed:
        Wrapped(colors.error, state.job.detail);
        break;
    case JobState::Cancelled:
        Wrapped(colors.secondary, "Cancelled. Nothing was installed.");
        break;
    case JobState::Idle:
        break;
    }
    if (state.worker.state == WorkerState::Stopped && !state.job_request.has_value()) {
        if (ImGui::Button("Start the worker again##restart")) state.worker_restart = true;
    }
}

void DrawTaskPanel(BackgroundTask& task, const Colors& colors, const char* id) {
    if (!task.Running()) return;
    ImGui::PushID(id);
    ImGui::TextUnformatted(task.Label().c_str());
    ImGui::ProgressBar(task.Progress(), ImVec2(-FLT_MIN, 0.0f));
    if (ImGui::Button("Cancel")) task.Cancel();
    ImGui::PopID();
    (void)colors;
}

std::string Clock(uint64_t frames, uint32_t rate) {
    if (rate == 0) return "0:00.0";
    const double seconds = static_cast<double>(frames) / static_cast<double>(rate);
    const unsigned minutes = static_cast<unsigned>(seconds / 60.0);
    char text[32];
    std::snprintf(text, sizeof(text), "%u:%04.1f", minutes, seconds - minutes * 60.0);
    return text;
}

std::string Kib(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
    return text;
}

// A pack file name from text: letters, digits, '-', '_' and spaces.
std::string FileStem(const std::string& text, const std::string& fallback) {
    std::string stem;
    for (const unsigned char c : text) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                          c == '_' || c == ' ';
        stem += safe ? static_cast<char>(c) : '_';
    }
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    return stem.empty() ? fallback : stem;
}

// Text for where a pack goes and how it is turned on. The Companion never edits the game's settings.
std::string EnableInstructions(const std::string& packer, const fs::path& output, const fs::path& game) {
    const char* toggle = "Enable texture packs";
    if (packer == "model") toggle = "Enable model packs";
    if (packer == "sequence") toggle = "Enable sequence packs";
    if (packer == "soundfont") toggle = "Enable soundfont packs";
    if (packer == "sample") toggle = "Enable sample packs";
    std::string out;
    std::error_code ec;
    const bool in_mods = !game.empty() && fs::equivalent(output.parent_path(), game / "mods", ec);
    if (!in_mods) out += "Copy the pack into the game's mods folder. ";
    out += "In the game, press F1, open Workshop > Mods, tick ";
    out += toggle;
    out += ", then tick the pack. The Companion does not change the game's settings.";
    return out;
}

// ---------------------------------------------------------------------------------------------------
// Packer pages: Textures, Models and Advanced.

struct PackerPage {
    std::string owner;
    std::string title;
    std::vector<std::string> notes;
    std::vector<std::string> packers;
    size_t current = 0;
    std::map<std::string, PackerForm> forms;
    std::map<std::string, std::string> pending_path;  // a typed path for a Paths option, before Add
    std::string project_file;
    std::string project_message;
    bool project_failed = false;
    std::vector<std::string> relink;
    std::map<std::string, std::vector<std::string>> names;
    std::map<std::string, std::string> names_error;
    std::string create_parent;
    std::string create_name;
    std::string handled_job;
    std::string built_message;
};

PackerForm& FormOf(PackerPage& page) {
    const std::string& id = page.packers[page.current];
    PackerForm& form = page.forms[id];
    form.packer = id;
    return form;
}

CompanionProject ProjectFromForm(const PackerPage& page, const PackerForm& form) {
    CompanionProject project;
    project.workspace = page.owner;
    project.text["packer"] = form.packer;
    const PackerSpec* spec = FindPacker(form.packer);
    project.paths["input"] = PathFromUtf8(form.input);
    project.paths["output"] = PathFromUtf8(form.output);
    if (spec != nullptr && spec->has_catalog) project.paths["catalog"] = PathFromUtf8(form.catalog);
    project.flags["overwrite"] = form.overwrite;
    if (spec == nullptr) return project;
    for (const OptionSpec& option : spec->options) {
        switch (option.kind) {
        case FieldKind::Text: {
            const auto it = form.text.find(option.key);
            if (it != form.text.end() && !it->second.empty()) project.text[option.key] = it->second;
            break;
        }
        case FieldKind::Path: {
            const auto it = form.text.find(option.key);
            if (it != form.text.end() && !it->second.empty()) project.paths[option.key] = PathFromUtf8(it->second);
            break;
        }
        case FieldKind::Flag: {
            const auto it = form.flags.find(option.key);
            project.flags[option.key] = it != form.flags.end() && it->second;
            break;
        }
        case FieldKind::Paths: {
            const auto it = form.paths.find(option.key);
            if (it == form.paths.end()) break;
            for (size_t i = 0; i < it->second.size(); ++i) {
                project.paths[std::string(option.key) + "." + std::to_string(i)] = PathFromUtf8(it->second[i]);
            }
            break;
        }
        }
    }
    return project;
}

std::string PathText(const CompanionProject& project, const std::string& key) {
    const auto it = project.paths.find(key);
    return it == project.paths.end() || it->second.empty() ? std::string() : PathToUtf8(it->second);
}

void FormFromProject(PackerPage& page, const CompanionProject& project) {
    const auto packer = project.text.find("packer");
    if (packer == project.text.end()) return;
    for (size_t i = 0; i < page.packers.size(); ++i) {
        if (page.packers[i] == packer->second) page.current = i;
    }
    PackerForm form;
    form.packer = page.packers[page.current];
    const PackerSpec* spec = FindPacker(form.packer);
    form.input = PathText(project, "input");
    form.output = PathText(project, "output");
    form.catalog = PathText(project, "catalog");
    const auto overwrite = project.flags.find("overwrite");
    form.overwrite = overwrite != project.flags.end() && overwrite->second;
    if (spec != nullptr) {
        for (const OptionSpec& option : spec->options) {
            switch (option.kind) {
            case FieldKind::Text: {
                const auto it = project.text.find(option.key);
                if (it != project.text.end()) form.text[option.key] = it->second;
                break;
            }
            case FieldKind::Path:
                form.text[option.key] = PathText(project, option.key);
                break;
            case FieldKind::Flag: {
                const auto it = project.flags.find(option.key);
                form.flags[option.key] = it != project.flags.end() && it->second;
                break;
            }
            case FieldKind::Paths:
                for (size_t i = 0; i < kMaxBases; ++i) {
                    const std::string key = std::string(option.key) + "." + std::to_string(i);
                    if (project.paths.find(key) == project.paths.end()) break;
                    form.paths[option.key].push_back(PathText(project, key));
                }
                break;
            }
        }
    }
    page.forms[form.packer] = form;
}

void SaveProjectTo(PackerPage& page, const std::string& file) {
    std::string target = file;
    if (PathFromUtf8(target).extension().empty()) target += kProjectExtension;
    page.project_file = target;
    std::string error;
    if (SaveProject(PathFromUtf8(target), ProjectFromForm(page, FormOf(page)), error)) {
        page.project_message = "Saved " + target + ".";
        page.project_failed = false;
    } else {
        page.project_message = "Not saved: " + error + ".";
        page.project_failed = true;
    }
}

void OpenProjectFrom(PackerPage& page, const std::string& file) {
    page.project_file = file;
    CompanionProject project;
    std::string error;
    page.relink.clear();
    if (!LoadProject(PathFromUtf8(file), project, page.relink, error)) {
        page.project_message = "Not opened: " + error + ".";
        page.project_failed = true;
        return;
    }
    if (project.workspace != page.owner) {
        page.project_message = "This project belongs to the " + project.workspace + " workspace.";
        page.project_failed = true;
        page.relink.clear();
        return;
    }
    FormFromProject(page, project);
    page.project_failed = false;
    page.project_message = page.relink.empty()
                               ? "Opened " + file + "."
                               : "Opened " + file + ". Some files moved or were deleted; choose them again below.";
}

void DrawProjectRow(AppState& state, const Colors& colors, PackerPage& page) {
    ImGui::SeparatorText("Project");
    Wrapped(colors.secondary, "A project file remembers this form, with paths relative to the project's folder. "
                              "It is written only when you press Save.");
    const std::string open_target = page.owner + ".project_open";
    const std::string save_target = page.owner + ".project_save";
    const auto opened = state.browse_results.find(open_target);
    if (opened != state.browse_results.end()) {
        const std::string file = opened->second;
        state.browse_results.erase(opened);
        OpenProjectFrom(page, file);
    }
    const auto saved = state.browse_results.find(save_target);
    if (saved != state.browse_results.end()) {
        const std::string file = saved->second;
        state.browse_results.erase(saved);
        SaveProjectTo(page, file);
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    InputString("##project_file", page.project_file);
    // With a system dialog: Open... and Save as... pick the file, Save writes the file above. Without
    // one, Open and Save use the typed path.
    const std::string start = page.project_file.empty() ? state.installation_dir : page.project_file;
    if (state.browse_available) {
        if (ImGui::Button("Open...##project")) {
            state.browse = BrowseRequest{open_target, BrowseKind::OpenFile, "Companion projects", "*.gdxproj", start,
                                         "Open a Companion project"};
        }
    } else {
        ImGui::BeginDisabled(page.project_file.empty());
        if (ImGui::Button("Open##project")) OpenProjectFrom(page, page.project_file);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(page.project_file.empty());
    if (ImGui::Button("Save##project")) SaveProjectTo(page, page.project_file);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Writes this form to the project file named above.");
    if (state.browse_available) {
        ImGui::SameLine();
        if (ImGui::Button("Save as...##project")) {
            state.browse = BrowseRequest{save_target, BrowseKind::SaveFile, "Companion projects", "*.gdxproj", start,
                                         "Save the Companion project"};
        }
    }
    if (!page.project_message.empty()) {
        Wrapped(page.project_failed ? colors.error : colors.secondary, page.project_message);
    }
    for (const std::string& key : page.relink) Wrapped(colors.warning, "Missing: " + key);
}

void DrawPathsOption(AppState& state, const Colors& colors, PackerPage& page, PackerForm& form,
                     const OptionSpec& option, const std::vector<FieldError>& errors) {
    std::vector<std::string>& list = form.paths[option.key];
    ImGui::TextUnformatted(option.label);
    Wrapped(colors.secondary, option.help);
    for (size_t i = 0; i < list.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("Remove")) {
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(list[i].c_str());
        ImGui::PopID();
    }
    const std::string target = page.owner + "." + form.packer + "." + option.key + ".add";
    std::string& pending = page.pending_path[target];
    // A pick from the dialog is added at once; a typed path waits for Add.
    const bool picked = state.browse_results.count(target) != 0;
    PathField(state, target, "", pending, BrowseKind::OpenFile, "Game archives", "*.o2r");
    const bool add = ImGui::Button("Add##add_path");
    if (picked || add) {
        if (!pending.empty() && list.size() < kMaxBases) {
            list.push_back(pending);
            pending.clear();
        }
    }
    Problem(colors, errors, option.key);
}

void DrawOption(AppState& state, const Colors& colors, PackerPage& page, PackerForm& form, const OptionSpec& option,
                const std::vector<FieldError>& errors) {
    ImGui::PushID(option.key);
    switch (option.kind) {
    case FieldKind::Text: {
        ImGui::TextUnformatted(option.required ? (std::string(option.label) + " (required)").c_str() : option.label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        InputString("##text", form.text[option.key]);
        Wrapped(colors.secondary, option.help);
        Problem(colors, errors, option.key);
        break;
    }
    case FieldKind::Path: {
        const bool tsv = std::string(option.key) == "native_manifest";
        PathField(state, page.owner + "." + form.packer + "." + option.key, option.label, form.text[option.key],
                  BrowseKind::OpenFile, tsv ? "Manifests" : "Metadata", tsv ? "*.tsv" : "*.json");
        Wrapped(colors.secondary, option.help);
        Problem(colors, errors, option.key);
        break;
    }
    case FieldKind::Flag: {
        bool& value = form.flags[option.key];
        ImGui::Checkbox(option.label, &value);
        ImGui::SetItemTooltip("%s", option.help);
        Problem(colors, errors, option.key);
        break;
    }
    case FieldKind::Paths:
        DrawPathsOption(state, colors, page, form, option, errors);
        break;
    }
    ImGui::PopID();
}

void DrawCreate(AppState& state, const Colors& colors, PackerPage& page, PackerForm& form, const PackerSpec& spec) {
    if (spec.init_choice == nullptr) return;
    if (!ImGui::CollapsingHeader("Create a new project##create")) return;
    ImGui::Indent();
    Wrapped(colors.secondary, "Create starts a project folder from what the game's asset dump captured "
                              "(Workshop > Tools > Asset Dump). Nothing the dump did not capture can be rebuilt.");
    std::vector<std::string>& names = page.names[form.packer];
    const std::string blocker = JobBlocker(state);
    if (names.empty()) {
        ImGui::BeginDisabled(!blocker.empty());
        if (ImGui::Button("Load the list##names")) {
            JobRequest request;
            request.op = "names";
            request.packer = form.packer;
            RequestJob(state, page.owner, request);
        }
        ImGui::EndDisabled();
        if (!page.names_error[form.packer].empty()) Wrapped(colors.error, page.names_error[form.packer]);
    } else {
        ImGui::TextUnformatted(spec.init_choice_label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##choice", form.choice.empty() ? "Choose..." : form.choice.c_str())) {
            for (const std::string& name : names) {
                if (ImGui::Selectable(name.c_str(), name == form.choice)) form.choice = name;
            }
            ImGui::EndCombo();
        }
    }
    PathField(state, page.owner + "." + form.packer + ".create_parent", "Folder to create the project in",
              page.create_parent, BrowseKind::Folder);
    ImGui::TextUnformatted("New project folder name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    InputString("##create_name", page.create_name);
    form.project = page.create_parent.empty() || page.create_name.empty()
                       ? std::string()
                       : PathToUtf8(PathFromUtf8(page.create_parent) / PathFromUtf8(page.create_name));
    if (form.init_catalog.empty()) {
        const fs::path game = GameDir(state);
        std::error_code ec;
        if (!game.empty() && fs::is_directory(game / "dump", ec)) form.init_catalog = PathToUtf8(game / "dump");
    }
    PathField(state, page.owner + "." + form.packer + ".init_catalog", "The game's dump folder", form.init_catalog,
              BrowseKind::Folder);
    for (const OptionSpec& option : spec.init_options) {
        bool& value = form.init_flags[option.key];
        ImGui::Checkbox(option.label, &value);
        ImGui::SetItemTooltip("%s", option.help);
    }
    const std::vector<FieldError> errors = ValidateForm(form, "init");
    Problem(colors, errors, "project");
    Problem(colors, errors, "init_catalog");
    Problem(colors, errors, "choice");
    ImGui::BeginDisabled(!errors.empty() || !blocker.empty());
    if (ImGui::Button("Create##create_button")) {
        JobRequest request;
        std::vector<FieldError> request_errors;
        if (BuildRequest(form, "init", request, request_errors)) RequestJob(state, page.owner, request);
    }
    ImGui::EndDisabled();
    if (!blocker.empty()) Wrapped(colors.secondary, blocker);
    ImGui::Unindent();
}

void HandlePackerJob(AppState& state, PackerPage& page) {
    const JobStatus* job = TakeFinished(state, page.owner, page.handled_job);
    if (job == nullptr) return;
    if (job->op == "names") {
        if (job->state == JobState::Succeeded) {
            page.names[job->packer] = job->result.names;
            page.names_error[job->packer].clear();
            if (job->result.names.empty()) page.names_error[job->packer] = "The worker returned an empty list.";
        } else {
            page.names_error[job->packer] = job->detail;
        }
    } else if (job->op == "init" && job->state == JobState::Succeeded) {
        PackerForm& form = page.forms[job->packer];
        form.input = job->result.project;
        page.create_name.clear();
    } else if (job->op == "build" && job->state == JobState::Succeeded) {
        page.built_message = EnableInstructions(job->packer, PathFromUtf8(job->output), GameDir(state));
    }
}

void DrawPackerPage(AppState& state, const Colors& colors, PackerPage& page) {
    ImGui::PushID(page.owner.c_str());
    HandlePackerJob(state, page);
    for (const std::string& note : page.notes) Wrapped(colors.secondary, note);

    if (page.packers.size() > 1) {
        for (size_t i = 0; i < page.packers.size(); ++i) {
            const PackerSpec* spec = FindPacker(page.packers[i]);
            if (i > 0) ImGui::SameLine();
            if (ImGui::RadioButton(spec->title, page.current == i)) {
                page.current = i;
                page.built_message.clear();
            }
        }
    }
    PackerForm& form = FormOf(page);
    const PackerSpec& spec = *FindPacker(form.packer);
    ImGui::PushID(spec.id);
    Wrapped(colors.text, spec.summary);

    DrawProjectRow(state, colors, page);
    DrawCreate(state, colors, page, form, spec);

    const std::vector<FieldError> build_errors = ValidateForm(form, "build");
    const std::vector<FieldError> check_errors = ValidateForm(form, "check");

    ImGui::SeparatorText("Source");
    PathField(state, page.owner + "." + form.packer + ".input", spec.input_label, form.input,
              spec.input_is_dir ? BrowseKind::Folder : BrowseKind::OpenFile,
              spec.input_is_dir ? "" : "Texture caches", spec.input_is_dir ? "" : spec.input_filter);
    Problem(colors, build_errors, "input");
    if (spec.has_catalog) {
        PathField(state, page.owner + "." + form.packer + ".catalog", spec.catalog_label, form.catalog,
                  BrowseKind::Folder);
        Problem(colors, build_errors, "catalog");
    }

    ImGui::SeparatorText("Options");
    for (const OptionSpec& option : spec.options) DrawOption(state, colors, page, form, option, build_errors);

    ImGui::SeparatorText("Output");
    PathField(state, page.owner + "." + form.packer + ".output", "Pack file (.o2r)", form.output, BrowseKind::SaveFile,
              "Packs", "*.o2r");
    const fs::path game = GameDir(state);
    if (!game.empty()) {
        if (ImGui::Button("Save into the game's mods folder")) {
            std::error_code ec;
            fs::create_directories(game / "mods", ec);
            const auto name = form.text.find("name");
            const std::string stem =
                FileStem(name == form.text.end() ? std::string() : name->second, std::string(spec.id) + "-pack");
            form.output = PathToUtf8(game / "mods" / PathFromUtf8(stem + ".o2r"));
        }
    }
    Problem(colors, build_errors, "output");
    ImGui::Checkbox("Replace an existing pack (the old one is kept as .previous)", &form.overwrite);
    Problem(colors, build_errors, "overwrite");

    ImGui::Separator();
    const std::string blocker = JobBlocker(state);
    ImGui::BeginDisabled(!check_errors.empty() || !blocker.empty());
    if (ImGui::Button("Validate")) {
        JobRequest request;
        std::vector<FieldError> errors;
        if (BuildRequest(form, "check", request, errors)) RequestJob(state, page.owner, request);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Checks the source without writing anything. Changed and unknown targets are listed in the log.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!build_errors.empty() || !blocker.empty());
    if (ImGui::Button("Build")) {
        page.built_message.clear();
        JobRequest request;
        std::vector<FieldError> errors;
        if (BuildRequest(form, "build", request, errors)) RequestJob(state, page.owner, request);
    }
    ImGui::EndDisabled();
    if (!blocker.empty()) {
        Wrapped(colors.secondary, blocker);
    } else if (!build_errors.empty()) {
        Wrapped(colors.secondary, "Build stays off until the problems marked above are fixed.");
    }
    DrawJobPanel(state, colors, page.owner);
    if (!page.built_message.empty() && state.job_owner == page.owner && state.job.state == JobState::Succeeded) {
        Wrapped(colors.text, page.built_message);
    }
    ImGui::PopID();
    ImGui::PopID();
}

// ---------------------------------------------------------------------------------------------------
// Music.

struct MusicPage {
    int target_kind = 0;  // 0: a game slot, 1: a course song
    int slot = 0;
    std::string song_name;
    std::string source;
    std::string inspected;
    AudioInfo info;
    std::string inspect_error;
    int policy = 0;
    uint64_t loop_start = 0;
    uint64_t loop_end = 0;
    bool confirm_replace = false;
    bool match_level = true;
    float trim_db = 0.0f;
    // Loop editor. wave is the mono copy of wave_path; wave_pending receives a read in progress.
    BackgroundTask wave_task;
    std::shared_ptr<LoopAudio> wave_pending;
    std::shared_ptr<const LoopAudio> wave;
    std::string wave_path;
    std::string wave_error;
    double view_from = 0.0;  // the frames the waveform shows
    double view_to = 0.0;
    int dragging = 0;  // 0: none, 1: the start marker, 2: the end marker
    bool snap = true;
    int nudge = 2;
    float seam_ms = 60.0f;
    std::string start_text;
    std::string end_text;
    std::string time_error;
    BackgroundTask find_task;
    std::shared_ptr<std::vector<LoopCandidate>> find_pending;
    const LoopAudio* find_for = nullptr;
    std::vector<LoopCandidate> found;
    std::string find_message;
    BackgroundTask task;
    std::string task_kind;
    std::string message;
    bool message_ok = true;
    // Native.
    bool targets_loaded = false;
    std::vector<NativeTarget> targets;
    std::string targets_error;
    int native_target = 0;
    int bank = 0;  // 0: cartridge, 1: Expansion Kit
    std::string second_source;
    std::string second_inspected;
    AudioInfo second_info;
    std::string second_error;
    bool duplicate = false;
    std::string pack_name = "My music";
    std::string catalog;
    std::string output;
    bool overwrite = false;
    std::string native_dir;
    std::string handled_job;
    std::string native_message;
    bool native_ok = true;

    ~MusicPage() { RemoveNativeDir(); }

    void RemoveNativeDir() {
        if (native_dir.empty()) return;
        const fs::path dir = PathFromUtf8(native_dir);
        std::error_code ec;
        // temp_directory_path ends with a separator on Windows; compare folder to folder.
        const fs::path temp = (fs::temp_directory_path(ec) / "x").parent_path();
        if (!ec && dir.parent_path() == temp &&
            PathToUtf8(dir.filename()).rfind(kNativeDirPrefix, 0) == 0) {
            fs::remove_all(dir, ec);
        }
        native_dir.clear();
    }
};

LoopPolicy PolicyOf(int index) {
    return index == 1 ? LoopPolicy::Whole : (index == 2 ? LoopPolicy::Custom : LoopPolicy::Keep);
}

LoopChoice ChoiceOf(const MusicPage& page) { return LoopChoice{PolicyOf(page.policy), page.loop_start, page.loop_end}; }

void Reinspect(const std::string& path, std::string& inspected, AudioInfo& info, std::string& error) {
    if (path == inspected) return;
    inspected = path;
    info = AudioInfo{};
    error.clear();
    if (path.empty()) return;
    if (!InspectAudio(PathFromUtf8(path), info, error)) info = AudioInfo{};
}

std::string MusicStem(const MusicPage& page) {
    if (page.target_kind == 0) return MusicSlots()[static_cast<size_t>(page.slot)].stem;
    return page.song_name;
}

void DrawPreview(AppState& state, const Colors& colors, MusicPage& page) {
    if (!state.preview.available) {
        Wrapped(colors.secondary, "Preview is off: no audio device opened when the Companion started.");
        return;
    }
    uint64_t start = 0;
    uint64_t end = 0;
    EffectiveLoop(page.info, ChoiceOf(page), start, end);
    const bool usable = page.info.frames > 0 && CheckLoop(page.info, ChoiceOf(page)).empty();
    ImGui::BeginDisabled(!usable);
    if (ImGui::Button("Play")) state.preview_request = PreviewRequest{page.source, 0, start, end, false};
    ImGui::SameLine();
    if (ImGui::Button("Hear the loop seam")) {
        const uint64_t lead = static_cast<uint64_t>(page.info.rate) * 3;
        const uint64_t from = end > start + lead ? end - lead : start;
        state.preview_request = PreviewRequest{page.source, from, start, end, false};
    }
    ImGui::SetItemTooltip("Starts three seconds before the loop ends, so you hear it jump back to the loop start.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.preview.playing);
    if (ImGui::Button("Stop")) state.preview_request = PreviewRequest{std::string(), 0, 0, 0, true};
    ImGui::EndDisabled();
    if (state.preview.playing) {
        ImGui::SameLine();
        ImGui::Text("%s / %s", Clock(state.preview.position, state.preview.rate).c_str(),
                    Clock(page.info.frames, page.info.rate).c_str());
    }
    if (!state.preview.error.empty()) Wrapped(colors.error, "Preview: " + state.preview.error);
}

ImU32 ToU32(const ImVec4& color, float alpha) {
    return ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
}

// Switches the page to a chosen loop, starting from the loop the current policy plays.
void MakeCustom(MusicPage& page) {
    if (page.policy == 2) return;
    uint64_t start = 0;
    uint64_t end = 0;
    EffectiveLoop(page.info, ChoiceOf(page), start, end);
    page.loop_start = start;
    page.loop_end = end;
    page.policy = 2;
}

void ShowAll(MusicPage& page) {
    page.view_from = 0.0;
    page.view_to = page.wave ? static_cast<double>(page.wave->frames) : 0.0;
}

// Shows about two seconds around frame.
void ShowAround(MusicPage& page, uint64_t frame) {
    if (!page.wave) return;
    const double frames = static_cast<double>(page.wave->frames);
    const double span = std::min(frames, 2.0 * page.wave->rate);
    page.view_from = std::clamp(static_cast<double>(frame) - span / 2.0, 0.0, frames - span);
    page.view_to = page.view_from + span;
}

// Starts, cancels or collects the waveform read so wave follows the inspected recording.
void UpdateWave(MusicPage& page) {
    const BackgroundTask::State read = page.wave_task.Poll();
    if (read != BackgroundTask::State::Running && page.wave_pending) {
        if (read == BackgroundTask::State::Succeeded && page.wave_path == page.inspected) {
            page.wave = page.wave_pending;
            ShowAll(page);
        } else if (read == BackgroundTask::State::Failed && page.wave_path == page.inspected) {
            page.wave_error = "No waveform: " + page.wave_task.Message() + ". The loop can still be typed below.";
        }
        page.wave_pending.reset();
    }
    const std::string want = page.info.frames > 0 ? page.inspected : std::string();
    if (page.wave_path == want) return;
    if (page.wave_task.Running()) {
        page.wave_task.Cancel();
        return;
    }
    page.find_task.Cancel();
    page.find_for = nullptr;
    page.wave.reset();
    page.wave_error.clear();
    page.found.clear();
    page.find_message.clear();
    page.wave_path = want;
    if (want.empty()) return;
    if (page.info.frames > kMaxLoopAudioFrames) {
        page.wave_error = "No waveform for recordings this long. The loop can still be typed below.";
        return;
    }
    auto pending = std::make_shared<LoopAudio>();
    page.wave_pending = pending;
    const fs::path path = PathFromUtf8(want);
    page.wave_task.Start("Reading the waveform", [path, pending](const CancelFlag& cancel, ProgressValue& progress,
                                                                 std::string& message) {
        return ReadLoopAudio(path, cancel, progress, *pending, message);
    });
}

void UpdateFind(MusicPage& page) {
    const BackgroundTask::State state = page.find_task.Poll();
    if (state == BackgroundTask::State::Running || !page.find_pending) return;
    if (page.wave && page.find_for == page.wave.get()) {
        if (state == BackgroundTask::State::Succeeded) {
            page.found = *page.find_pending;
            page.find_message.clear();
        } else if (state == BackgroundTask::State::Failed) {
            page.find_message = page.find_task.Message();
        }
    }
    page.find_pending.reset();
}

void StartFind(MusicPage& page) {
    if (!page.wave || page.find_task.Running()) return;
    auto pending = std::make_shared<std::vector<LoopCandidate>>();
    page.find_pending = pending;
    page.find_for = page.wave.get();
    page.found.clear();
    page.find_message.clear();
    const std::shared_ptr<const LoopAudio> wave = page.wave;
    page.find_task.Start("Finding loop points", [wave, pending](const CancelFlag& cancel, ProgressValue& progress,
                                                               std::string& message) {
        return FindLoopPoints(*wave, 5, cancel, progress, *pending, message);
    });
}

std::string MatchText(float match) {
    const int percent = static_cast<int>(std::lround(std::max(0.0f, match) * 100.0f));
    const char* verdict = match >= 0.9f ? "smooth" : (match >= 0.6f ? "a soft bump may be heard" : "likely to click or skip");
    return std::to_string(percent) + "% (" + verdict + ")";
}

void HearSeam(AppState& state, const MusicPage& page, uint64_t start, uint64_t end) {
    const uint64_t lead = static_cast<uint64_t>(page.info.rate) * 3;
    const uint64_t from = end > start + lead ? end - lead : start;
    state.preview_request = PreviewRequest{page.source, from, start, end, false};
}

// The whole recording, or the zoomed part of it, with the loop shaded and its two markers. Drag a marker
// to move it, click elsewhere to play from there, use the wheel to zoom and drag with the right button to
// scroll.
void DrawWaveform(AppState& state, const Colors& colors, MusicPage& page, uint64_t start, uint64_t end) {
    const LoopAudio& wave = *page.wave;
    const double frames = static_cast<double>(wave.frames);
    if (page.view_to <= page.view_from || page.view_to > frames) ShowAll(page);
    const float width = std::max(64.0f, ImGui::GetContentRegionAvail().x);
    const float height = ImGui::GetFontSize() * 6.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wave", ImVec2(width, height),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO& io = ImGui::GetIO();
    double span = page.view_to - page.view_from;
    auto frame_at = [&](float x) {
        const double t = std::clamp(static_cast<double>(x - origin.x) / width, 0.0, 1.0);
        return static_cast<uint64_t>(std::llround(page.view_from + t * span));
    };
    auto x_of = [&](uint64_t frame) {
        return origin.x + static_cast<float>((static_cast<double>(frame) - page.view_from) / span * width);
    };

    if (hovered && io.MouseWheel != 0.0f && !io.KeyShift) {
        const double min_span = std::min(frames, std::max(64.0, static_cast<double>(width) / 4.0));
        const double new_span = std::clamp(span * std::pow(0.8, io.MouseWheel), min_span, frames);
        const double t = static_cast<double>(io.MousePos.x - origin.x) / width;
        const double centre = page.view_from + t * span;
        page.view_from = std::clamp(centre - t * new_span, 0.0, frames - new_span);
        page.view_to = page.view_from + new_span;
    } else if (hovered && (io.MouseWheelH != 0.0f || (io.KeyShift && io.MouseWheel != 0.0f))) {
        const float wheel = io.MouseWheelH != 0.0f ? io.MouseWheelH : io.MouseWheel;
        page.view_from = std::clamp(page.view_from - wheel * span * 0.1, 0.0, frames - span);
        page.view_to = page.view_from + span;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        page.view_from = std::clamp(page.view_from - io.MouseDelta.x / width * span, 0.0, frames - span);
        page.view_to = page.view_from + span;
    }
    span = page.view_to - page.view_from;

    const float grab = ImGui::GetFontSize() * 0.5f;
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const float to_start = std::fabs(io.MousePos.x - x_of(start));
        const float to_end = std::fabs(io.MousePos.x - x_of(end));
        if (std::min(to_start, to_end) <= grab) page.dragging = to_start < to_end ? 1 : 2;
    }
    if (page.dragging != 0 && ImGui::IsItemActive()) {
        MakeCustom(page);
        const uint64_t frame = frame_at(io.MousePos.x);
        if (page.dragging == 1) {
            page.loop_start = std::min(frame, page.loop_end > 0 ? page.loop_end - 1 : 0);
        } else {
            page.loop_end = std::min<uint64_t>(std::max<uint64_t>(frame, page.loop_start + 1), wave.frames);
        }
    }
    if (ImGui::IsItemDeactivated() && page.dragging != 0) {
        if (page.snap) {
            const uint64_t radius = std::max<uint32_t>(1, wave.rate / 100);
            if (page.dragging == 1) {
                const uint64_t snapped = NearestZeroCrossing(wave, page.loop_start, radius);
                if (snapped < page.loop_end) page.loop_start = snapped;
            } else {
                const uint64_t snapped = NearestZeroCrossing(wave, page.loop_end, radius);
                if (snapped > page.loop_start && snapped <= wave.frames) page.loop_end = snapped;
            }
        }
        page.dragging = 0;
    } else if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
               std::fabs(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x) < 3.0f && state.preview.available) {
        const uint64_t from = std::min(frame_at(io.MousePos.x), end > 0 ? end - 1 : 0);
        state.preview_request = PreviewRequest{page.source, from, start, end, false};
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 corner(origin.x + width, origin.y + height);
    draw->AddRectFilled(origin, corner, ToU32(colors.panel, 0.9f));
    draw->PushClipRect(origin, corner, true);
    const float mid = origin.y + height * 0.5f;
    const float scale = height * 0.48f;
    const float loop_left = std::max(origin.x, x_of(start));
    const float loop_right = std::min(corner.x, x_of(end));
    if (loop_right > loop_left) {
        draw->AddRectFilled(ImVec2(loop_left, origin.y), ImVec2(loop_right, corner.y), ToU32(colors.accent, 0.14f));
    }
    const int columns = static_cast<int>(width);
    const double per_column = span / columns;
    for (int x = 0; x < columns; ++x) {
        const uint64_t a = static_cast<uint64_t>(page.view_from + x * per_column);
        const uint64_t b = std::max<uint64_t>(a + 1, static_cast<uint64_t>(page.view_from + (x + 1) * per_column));
        float low = 0.0f;
        float high = 0.0f;
        PeakRange(wave, a, b, low, high);
        const bool inside = a >= start && a < end;
        const float px = origin.x + static_cast<float>(x) + 0.5f;
        draw->AddLine(ImVec2(px, mid - std::clamp(high, -1.0f, 1.0f) * scale),
                      ImVec2(px, mid - std::clamp(low, -1.0f, 1.0f) * scale + 1.0f),
                      ToU32(inside ? colors.text : colors.secondary, inside ? 0.9f : 0.5f));
    }
    draw->AddLine(ImVec2(origin.x, mid), ImVec2(corner.x, mid), ToU32(colors.secondary, 0.25f));
    const char* names[] = {"Start", "End"};
    const uint64_t marks[] = {start, end};
    for (int i = 0; i < 2; ++i) {
        const float x = x_of(marks[i]);
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, corner.y), ToU32(colors.accent, 1.0f), 2.0f);
        draw->AddTriangleFilled(ImVec2(x - grab * 0.6f, origin.y), ImVec2(x + grab * 0.6f, origin.y),
                                ImVec2(x, origin.y + grab), ToU32(colors.accent, 1.0f));
        const ImVec2 size = ImGui::CalcTextSize(names[i]);
        const float label_x = i == 0 ? x + 4.0f : x - size.x - 4.0f;
        draw->AddText(ImVec2(label_x, corner.y - size.y - 2.0f), ToU32(colors.accent, 1.0f), names[i]);
    }
    if (state.preview.playing && state.preview.path == page.source) {
        const float x = x_of(state.preview.position);
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, corner.y), ToU32(colors.text, 1.0f), 1.0f);
    }
    draw->PopClipRect();
    if (hovered && page.dragging == 0) {
        const uint64_t frame = frame_at(io.MousePos.x);
        ImGui::SetItemTooltip("%s (frame %llu)\nDrag a marker to move it. Click to play from here.\nWheel: zoom. "
                              "Shift+wheel or right drag: scroll.",
                              FormatLoopTime(frame, wave.rate).c_str(), static_cast<unsigned long long>(frame));
    }
}

// What the game plays across the seam: the end of the loop on the left, then its start on the right. The
// faint line is what the recording holds there instead, so the closer the two lines, the smoother the jump.
void DrawSeamView(const Colors& colors, MusicPage& page, uint64_t start, uint64_t end) {
    const LoopAudio& wave = *page.wave;
    const float width = std::max(64.0f, ImGui::GetContentRegionAvail().x);
    const float height = ImGui::GetFontSize() * 4.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    const ImVec2 corner(origin.x + width, origin.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, corner, ToU32(colors.panel, 0.9f));
    draw->PushClipRect(origin, corner, true);
    const double half = std::max(8.0, page.seam_ms / 1000.0 * wave.rate);
    const float mid = origin.y + height * 0.5f;
    const float scale = height * 0.46f;
    // heard: the frame the game plays at offset t from the seam; other: what the recording holds there.
    auto sample = [&](double t, bool heard) -> float {
        double frame = 0.0;
        if (t < 0.0) {
            frame = heard ? static_cast<double>(end) + t : static_cast<double>(start) + t;
        } else {
            frame = heard ? static_cast<double>(start) + t : static_cast<double>(end) + t;
        }
        if (frame < 0.0 || frame >= static_cast<double>(wave.frames)) return 0.0f;
        return wave.mono[static_cast<size_t>(frame)];
    };
    const int columns = static_cast<int>(width);
    for (int pass = 0; pass < 2; ++pass) {
        const bool heard = pass == 1;
        const ImU32 color = heard ? ToU32(colors.accent, 1.0f) : ToU32(colors.secondary, 0.45f);
        ImVec2 previous;
        for (int x = 0; x <= columns; ++x) {
            const double t = (static_cast<double>(x) / columns * 2.0 - 1.0) * half;
            const float value = std::clamp(sample(t, heard), -1.0f, 1.0f);
            const ImVec2 point(origin.x + static_cast<float>(x), mid - value * scale);
            if (x > 0) draw->AddLine(previous, point, color, heard ? 1.5f : 1.0f);
            previous = point;
        }
    }
    const float seam_x = origin.x + width * 0.5f;
    draw->AddLine(ImVec2(seam_x, origin.y), ImVec2(seam_x, corner.y), ToU32(colors.text, 0.6f));
    const char* left = "Loop end";
    const char* right = "Loop start";
    draw->AddText(ImVec2(seam_x - ImGui::CalcTextSize(left).x - 4.0f, origin.y + 2.0f), ToU32(colors.secondary, 1.0f),
                  left);
    draw->AddText(ImVec2(seam_x + 4.0f, origin.y + 2.0f), ToU32(colors.secondary, 1.0f), right);
    draw->PopClipRect();
}

// A time field and a frame field for one marker. Returns true when the user changed the value.
bool MarkerFields(const char* label, const char* id, uint64_t& value, std::string& text, uint32_t rate,
                  uint64_t step, std::string& error) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 3.5f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.5f);
    InputString("##time", text);
    const bool editing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        uint64_t frame = 0;
        if (ParseLoopTime(text, rate, frame)) {
            value = frame;
            changed = true;
            error.clear();
        } else {
            error = "Write a time as m:ss.mmm, for example 1:02.500.";
        }
    }
    ImGui::SetItemTooltip("Minutes, seconds and milliseconds: m:ss.mmm. Press Enter to apply.");
    if (!editing) text = FormatLoopTime(value, rate);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
    const ImU64 fast = step * 10;
    if (ImGui::InputScalar("frame##frame", ImGuiDataType_U64, &value, &step, &fast)) changed = true;
    ImGui::SetItemTooltip("The frame number. - and + move it by the nudge step; hold Ctrl for ten steps.");
    ImGui::PopID();
    return changed;
}

void DrawLoop(AppState& state, const Colors& colors, MusicPage& page) {
    ImGui::SeparatorText("Loop");
    UpdateWave(page);
    UpdateFind(page);
    const char* labels[] = {"Keep the file's loop", "Repeat the whole recording", "Choose the loop"};
    for (int i = 0; i < 3; ++i) {
        const bool offered = LoopPolicyOffered(page.info.format, PolicyOf(i));
        if (i > 0) ImGui::SameLine();
        ImGui::BeginDisabled(!offered);
        if (ImGui::RadioButton(labels[i], page.policy == i)) {
            if (i == 2) {
                MakeCustom(page);
            } else {
                page.policy = i;
            }
        }
        ImGui::EndDisabled();
    }
    if (page.policy == 0) {
        Wrapped(colors.secondary, page.info.has_loop ? "Loop from the file: " + Clock(page.info.loop_start, page.info.rate) +
                                                           " to " + Clock(page.info.loop_end, page.info.rate) + "."
                                                     : "The file has no loop tags, so the whole recording repeats.");
    }
    uint64_t start = 0;
    uint64_t end = 0;
    EffectiveLoop(page.info, ChoiceOf(page), start, end);

    if (page.wave_task.Running()) {
        ImGui::TextUnformatted("Reading the waveform...");
        ImGui::ProgressBar(page.wave_task.Progress(), ImVec2(-FLT_MIN, 0.0f));
    }
    Wrapped(colors.secondary, page.wave_error);
    if (page.wave) {
        DrawWaveform(state, colors, page, start, end);
        if (ImGui::SmallButton("Show all")) ShowAll(page);
        ImGui::SameLine();
        if (ImGui::SmallButton("Show the start")) ShowAround(page, start);
        ImGui::SameLine();
        if (ImGui::SmallButton("Show the end")) ShowAround(page, end);
        // The drag may have moved the loop this frame.
        EffectiveLoop(page.info, ChoiceOf(page), start, end);
    }

    if (page.policy == 2) {
        static const char* kSteps[] = {"1 frame", "1 ms", "10 ms", "100 ms", "1 s"};
        static const double kStepSeconds[] = {0.0, 0.001, 0.01, 0.1, 1.0};
        page.nudge = std::clamp(page.nudge, 0, 4);
        const uint64_t step = page.nudge == 0 ? 1
                                              : std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(
                                                                          kStepSeconds[page.nudge] * page.info.rate)));
        MarkerFields("Start", "loop_start", page.loop_start, page.start_text, page.info.rate, step, page.time_error);
        MarkerFields("End", "loop_end", page.loop_end, page.end_text, page.info.rate, step, page.time_error);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        ImGui::Combo("Nudge step##nudge", &page.nudge, kSteps, 5);
        ImGui::SameLine();
        ImGui::BeginDisabled(!page.wave);
        ImGui::Checkbox("Snap dragged markers to zero crossings##snap", &page.snap);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("A marker let go on a point where the wave crosses zero joins without a click.");
        Wrapped(colors.error, page.time_error);
        Wrapped(colors.secondary, "The end is the first frame after the loop. The recording has " +
                                      std::to_string(page.info.frames) + " frames at " + std::to_string(page.info.rate) +
                                      " Hz.");
        EffectiveLoop(page.info, ChoiceOf(page), start, end);
    }

    if (page.wave && start < end && end <= page.wave->frames) {
        float match = 0.0f;
        if (SeamMatch(*page.wave, start, end, match)) {
            Wrapped(match >= 0.9f ? colors.ok : (match >= 0.6f ? colors.warning : colors.error),
                    "Seam match: " + MatchText(match));
        } else {
            Wrapped(colors.secondary, "The whole recording repeats, so the seam joins its end to its start.");
        }
        DrawSeamView(colors, page, start, end);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        ImGui::SliderFloat("Seam view (ms each side)##seam_ms", &page.seam_ms, 2.0f, 1000.0f, "%.0f ms",
                           ImGuiSliderFlags_Logarithmic);
        if (ImGui::Button("Match the end to the start")) {
            MakeCustom(page);
            page.loop_end = AlignLoopEnd(*page.wave, page.loop_start, page.loop_end,
                                         std::max<uint64_t>(1, page.info.rate / 20));
        }
        ImGui::SetItemTooltip("Moves the end up to 50 ms either way, to where the music lines up best with the start.");
    }

    if (page.wave) {
        ImGui::BeginDisabled(page.find_task.Running());
        if (ImGui::Button("Find loop points")) StartFind(page);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Compares every stretch of the recording with every earlier one and suggests loops "
                              "where the music repeats. Listen to each before you install it.");
        if (page.find_task.Running()) {
            ImGui::SameLine();
            ImGui::ProgressBar(page.find_task.Progress(), ImVec2(ImGui::GetFontSize() * 12.0f, 0.0f));
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##find_cancel")) page.find_task.Cancel();
        }
        Wrapped(colors.error, page.find_message);
        if (!page.found.empty() &&
            ImGui::BeginTable("##found", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Start");
            ImGui::TableSetupColumn("End");
            ImGui::TableSetupColumn("Length");
            ImGui::TableSetupColumn("Seam match", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < page.found.size(); ++i) {
                const LoopCandidate& candidate = page.found[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FormatLoopTime(candidate.start, page.info.rate).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FormatLoopTime(candidate.end, page.info.rate).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FormatLoopTime(candidate.end - candidate.start, page.info.rate).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(MatchText(candidate.match).c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Use")) {
                    page.policy = 2;
                    page.loop_start = candidate.start;
                    page.loop_end = candidate.end;
                    ShowAround(page, candidate.end);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!state.preview.available);
                if (ImGui::SmallButton("Hear")) HearSeam(state, page, candidate.start, candidate.end);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    Wrapped(colors.error, CheckLoop(page.info, ChoiceOf(page)));
}

// A decibel figure with one decimal: "12.0".
std::string Decibels(double db) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", db);
    return text;
}

// The change, in dB, that the game applies to a recording tagged with track gain gain_db.
double GameLevelDb(double gain_db, bool has_peak, float peak) {
    GdxMusicInfo tag{};
    tag.hasGain = 1;
    tag.gainDb = static_cast<float>(gain_db);
    tag.hasPeak = has_peak ? 1 : 0;
    tag.peak = peak;
    return 20.0 * std::log10(static_cast<double>(gdx_music_level_gain(&tag)));
}

// "turns it down 12.0 dB", "turns it up 2.0 dB" or "leaves its level".
std::string LevelChange(double db) {
    if (std::fabs(db) < 0.05) return "leaves its level";
    return std::string(db < 0.0 ? "turns it down " : "turns it up ") + Decibels(std::fabs(db)) + " dB";
}

// "at the level of the game's music", or how far louder or quieter than it.
std::string AgainstStock(double db) {
    if (std::fabs(db) < 0.05) return "at the level of the game's music";
    return Decibels(std::fabs(db)) + " dB " + (db < 0.0 ? "quieter" : "louder") + " than the game's music";
}

void DrawLevel(const Colors& colors, MusicPage& page) {
    ImGui::SeparatorText("Level");
    ImGui::Checkbox("Match the game's music level##match_level", &page.match_level);
    ImGui::SetItemTooltip("Measures the recording's loudness on install and writes it into the file as a level tag, "
                          "so the game plays it as loud as its own music. The audio is not changed. In the game, "
                          "Settings > Audio > Match custom music loudness must be on, as it is by default.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!page.match_level);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::SliderFloat("Adjust##trim_db", &page.trim_db, -12.0f, 12.0f, "%+.1f dB");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Plays the song this much louder, or quieter, than the game's music. 0 dB matches it. "
                          "Ctrl+click to type a value.");
    const LoopAudio* wave = page.wave.get();
    const bool measured = wave != nullptr && wave->has_loudness;
    const std::string facts =
        measured ? "Measured " + Decibels(wave->lufs) + " LUFS, peak " +
                       (wave->peak > 0.0f ? Decibels(20.0 * std::log10(static_cast<double>(wave->peak))) + " dBFS. "
                                          : std::string("silent. "))
                 : std::string();
    if (page.match_level) {
        if (wave == nullptr) {
            Wrapped(colors.secondary, "The loudness is measured when you install.");
        } else if (!wave->has_loudness) {
            Wrapped(colors.error, "The recording is silent, so its level cannot be matched. Turn matching off to install it.");
        } else {
            const double wanted = static_cast<double>(GDX_MUSIC_STOCK_LUFS) - wave->lufs + page.trim_db;
            const double played = GameLevelDb(ReplayGainFor(wave->lufs) + page.trim_db, true, wave->peak);
            std::string text = facts + "The game " + LevelChange(played) + ", so it plays " +
                               AgainstStock(wave->lufs + played - static_cast<double>(GDX_MUSIC_STOCK_LUFS));
            if (played < wanted - 0.05) text += ": more would push its peak past full scale";
            Wrapped(colors.text, text + ".");
        }
    } else if (page.info.has_gain) {
        const double played = GameLevelDb(page.info.gain_db, page.info.has_peak, page.info.peak);
        Wrapped(colors.secondary, facts + "The file's own level tag stays: the game " + LevelChange(played) + ".");
    } else if (measured) {
        Wrapped(colors.warning, facts + "The game plays it as decoded, " +
                                    AgainstStock(wave->lufs - static_cast<double>(GDX_MUSIC_STOCK_LUFS)) + ".");
    } else {
        Wrapped(colors.secondary, "The game plays it as decoded.");
    }
}

void StartInstall(AppState& state, MusicPage& page, const fs::path& game, const std::string& stem) {
    state.preview_request = PreviewRequest{std::string(), 0, 0, 0, true};
    const fs::path source = PathFromUtf8(page.source);
    const AudioInfo info = page.info;
    const LoopChoice choice = ChoiceOf(page);
    const LevelRequest level{page.match_level, static_cast<double>(page.trim_db)};
    const bool slot = page.target_kind == 0;
    page.task_kind = "install";
    page.message.clear();
    page.task.Start("Installing", [game, stem, source, info, choice, level, slot](const CancelFlag& cancel,
                                                                                     ProgressValue& progress,
                                                                                     std::string& message) {
        std::string error;
        const InstallPlan plan = PlanMusicInstall(game, stem, info.format);
        const fs::path staging = MakeMusicStaging(plan.music_dir, error);
        if (staging.empty()) {
            message = error;
            return false;
        }
        const fs::path staged = staging / plan.destination.filename();
        MusicLevel written;
        const bool ok = StageMusic(source, info, choice, staged, cancel, progress, error, level, &written) &&
                        CommitMusicInstall(plan, staged, error);
        RemoveMusicStaging(staging, plan.music_dir);
        if (!ok) {
            message = error;
            return false;
        }
        message = "Installed as music/" + PathToUtf8(plan.destination.filename()) + ". ";
        if (written.set) {
            message += "The game " + LevelChange(GameLevelDb(written.gain_db, written.peak > 0.0f, written.peak)) + ". ";
        }
        message += slot ? "Restart the game to hear it." : "Restart the game, then pick the song for a track in Course Edit.";
        if (!plan.displaced.empty()) message += " Replaced files are kept as .previous; Restore brings them back.";
        return true;
    });
}

void DrawInstall(AppState& state, const Colors& colors, MusicPage& page) {
    ImGui::SeparatorText("Install into the game's music folder");
    const fs::path game = GameDir(state);
    const std::string stem = MusicStem(page);
    std::string blocker;
    if (game.empty()) blocker = "Choose the game installation in Settings first.";
    if (blocker.empty() && page.target_kind == 0 && !MusicSlots()[static_cast<size_t>(page.slot)].streams) {
        blocker = "The game plays this slot through its sound effect player and ignores music/ for it.";
    }
    if (blocker.empty() && page.target_kind == 1) blocker = CheckCourseSongName(stem);
    if (blocker.empty() && page.info.frames == 0) blocker = "Choose a recording the game can play.";
    if (blocker.empty()) blocker = CheckLoop(page.info, ChoiceOf(page));
    if (!blocker.empty()) {
        Wrapped(colors.secondary, blocker);
        return;
    }
    const InstallPlan plan = PlanMusicInstall(game, stem, page.info.format);
    Wrapped(colors.text, "Mode: streamed from music/. It uses no native sample memory.");
    Wrapped(colors.text, "Destination: " + PathToUtf8(plan.destination));
    for (const fs::path& path : plan.displaced) {
        Wrapped(colors.warning, "Replaces " + PathToUtf8(path.filename()) + ", kept as " +
                                    PathToUtf8(path.filename()) + ".previous");
    }
    for (const fs::path& path : plan.replaced_backups) {
        Wrapped(colors.warning, "The older backup " + PathToUtf8(path.filename()) + " is deleted.");
    }
    for (const fs::path& path : plan.legacy) {
        Wrapped(colors.secondary, "music/ outranks " + PathToUtf8(path) + ", which stays where it is.");
    }
    if (!plan.displaced.empty()) ImGui::Checkbox("Replace these files##confirm_replace", &page.confirm_replace);
    const bool ready = plan.displaced.empty() || page.confirm_replace;
    ImGui::BeginDisabled(!ready || page.task.Running());
    if (ImGui::Button("Install##music_install")) StartInstall(state, page, game, stem);
    ImGui::EndDisabled();
    const RestorePlan restore = PlanMusicRestore(game, stem);
    if (!restore.previous.empty()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(page.task.Running());
        if (ImGui::Button("Restore the earlier version##music_restore")) {
            state.preview_request = PreviewRequest{std::string(), 0, 0, 0, true};
            std::string error;
            page.message_ok = RestoreMusic(restore, error);
            page.message = page.message_ok ? "Restored. The version it replaced is kept as .previous." : error;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Swaps the installed file with its .previous backup.");
    }
}

void LoadTargets(AppState& state, MusicPage& page) {
    if (page.targets_loaded) return;
    page.targets_loaded = true;
    const fs::path file = PathFromUtf8(state.exe_dir) / "music_targets.json";
    if (!LoadNativeTargets(file, page.targets, page.targets_error)) {
        page.targets_error = "Native targets not available: " + page.targets_error;
    }
}

const std::vector<NativeResource>* BankOf(const NativeTarget& target, int bank) {
    if (bank == 0 && !target.cartridge.empty()) return &target.cartridge;
    if (bank == 1 && !target.expansion_kit.empty()) return &target.expansion_kit;
    return nullptr;
}

void StartNativeConvert(AppState& state, MusicPage& page, const std::vector<NativeResource>& resources) {
    struct Part {
        fs::path source;
        AudioInfo info;
        NativeResource resource;
    };
    std::vector<Part> parts;
    for (const NativeResource& resource : resources) {
        const bool second = resource.channel == "part_1" && !page.duplicate;
        parts.push_back({PathFromUtf8(second ? page.second_source : page.source), second ? page.second_info : page.info,
                         resource});
    }
    page.RemoveNativeDir();
    const LoopChoice choice = ChoiceOf(page);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / (std::string(kNativeDirPrefix) + std::to_string(stamp));
    page.task_kind = "native";
    page.native_message.clear();
    page.task.Start("Converting for native playback", [parts, choice, dir](const CancelFlag& cancel,
                                                                           ProgressValue& progress,
                                                                           std::string& message) {
        std::error_code error_code;
        fs::create_directories(dir, error_code);
        if (error_code) {
            message = "cannot create " + PathToUtf8(dir) + ": " + error_code.message();
            return false;
        }
        for (const Part& part : parts) {
            std::string error;
            if (!WriteNativeWav(part.source, part.info, part.resource.rate_hz, choice,
                                dir / PathFromUtf8(part.resource.key + ".wav"),
                                dir / PathFromUtf8(part.resource.key + ".loop.json"), cancel, progress, error)) {
                fs::remove_all(dir, error_code);
                message = error;
                return false;
            }
        }
        message = PathToUtf8(dir);
        return true;
    });
    (void)state;
}

PackerForm NativeForm(const MusicPage& page, const std::string& input) {
    PackerForm form;
    form.packer = "sample";
    form.input = input;
    form.output = page.output;
    form.catalog = page.catalog;
    form.overwrite = page.overwrite;
    if (!page.pack_name.empty()) form.text["name"] = page.pack_name;
    return form;
}

void DrawNative(AppState& state, const Colors& colors, MusicPage& page) {
    if (!ImGui::CollapsingHeader("Advanced: native samples##native")) return;
    ImGui::Indent();
    Wrapped(colors.secondary,
            "Instead of streaming, the game can play a recording through its own sample player, as the original "
            "music does. Only the title, selection and (Expansion Kit) Big Blue songs are offered. All native "
            "sample packs share 1 MiB, so long recordings do not fit.");
    LoadTargets(state, page);
    if (!page.targets_error.empty()) {
        Wrapped(colors.error, page.targets_error);
        ImGui::Unindent();
        return;
    }
    if (page.targets.empty()) {
        ImGui::Unindent();
        return;
    }
    page.native_target = std::clamp(page.native_target, 0, static_cast<int>(page.targets.size()) - 1);
    for (size_t i = 0; i < page.targets.size(); ++i) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::RadioButton((page.targets[i].label + "##native_target").c_str(),
                               page.native_target == static_cast<int>(i))) {
            page.native_target = static_cast<int>(i);
        }
    }
    const NativeTarget& target = page.targets[static_cast<size_t>(page.native_target)];
    if (BankOf(target, page.bank) == nullptr) page.bank = target.cartridge.empty() ? 1 : 0;
    if (!target.cartridge.empty() && !target.expansion_kit.empty()) {
        ImGui::RadioButton("Cartridge (playing without the Expansion Kit)##bank", &page.bank, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Expansion Kit##bank", &page.bank, 1);
    } else {
        Wrapped(colors.text, target.cartridge.empty() ? "Mode: Expansion Kit only." : "Mode: cartridge only.");
    }
    const std::vector<NativeResource>& resources = *BankOf(target, page.bank);
    bool multipart = false;
    for (const NativeResource& resource : resources) multipart = multipart || resource.channel == "part_1";

    ImGui::TextUnformatted("Resources this replaces:");
    for (const NativeResource& resource : resources) {
        ImGui::BulletText("%s, %u Hz, %s", resource.key.c_str(), resource.rate_hz, resource.channel.c_str());
    }

    std::string blocker;
    if (page.info.frames == 0) blocker = "Choose a recording above first.";
    if (multipart) {
        Wrapped(colors.secondary, "This song plays in two parts. The recording above is part 0. Choose part 1, or "
                                  "use the same recording for both on purpose.");
        ImGui::Checkbox("Use the same recording for both parts##duplicate", &page.duplicate);
        if (!page.duplicate) {
            PathField(state, "music.native.second", "Part 1 recording", page.second_source, BrowseKind::OpenFile,
                      "Recordings", "*.ogg;*.flac;*.wav");
            Reinspect(page.second_source, page.second_inspected, page.second_info, page.second_error);
            Wrapped(colors.error, page.second_error);
            if (blocker.empty() && page.second_info.frames == 0) blocker = "Choose the part 1 recording.";
            if (blocker.empty() && PolicyOf(page.policy) == LoopPolicy::Custom &&
                (page.second_info.frames != page.info.frames || page.second_info.rate != page.info.rate)) {
                blocker = "A chosen loop needs both parts to have the same length and rate.";
            }
        }
    }
    uint64_t bytes = 0;
    for (const NativeResource& resource : resources) {
        const bool second = resource.channel == "part_1" && !page.duplicate;
        const AudioInfo& info = second ? page.second_info : page.info;
        bytes += NativeBytes(ResampledFrames(info.frames, info.rate, resource.rate_hz));
    }
    if (page.info.frames > 0) {
        Wrapped(bytes > kNativeBudgetBytes ? colors.error : colors.text,
                "Native memory: " + Kib(bytes) + " of the " + Kib(kNativeBudgetBytes) +
                    " all installed sample packs share.");
        if (blocker.empty() && bytes > kNativeBudgetBytes) blocker = "The recording is too long for native samples.";
    }

    ImGui::TextUnformatted("Pack name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    InputString("##native_name", page.pack_name);
    const fs::path game = GameDir(state);
    std::error_code ec;
    if (page.catalog.empty() && !game.empty() && fs::is_directory(game / "dump" / "audio", ec)) {
        page.catalog = PathToUtf8(game / "dump" / "audio");
    }
    PathField(state, "music.native.catalog", "The game's dump audio folder (Workshop > Tools > Asset Dump)",
              page.catalog, BrowseKind::Folder);
    if (page.output.empty() && !game.empty()) {
        page.output = PathToUtf8(game / "mods" / PathFromUtf8(FileStem(page.pack_name, "music") + ".o2r"));
    }
    PathField(state, "music.native.output", "Pack file (.o2r)", page.output, BrowseKind::SaveFile, "Packs", "*.o2r");
    ImGui::Checkbox("Replace an existing pack (the old one is kept as .previous)##native_overwrite", &page.overwrite);

    std::vector<FieldError> errors = ValidateForm(NativeForm(page, PathToUtf8(fs::temp_directory_path(ec))), "build");
    for (const char* field : {"name", "catalog", "output", "overwrite"}) Problem(colors, errors, field);
    errors.erase(std::remove_if(errors.begin(), errors.end(), [](const FieldError& e) { return e.field == "input"; }),
                 errors.end());
    const std::string job_blocker = JobBlocker(state);
    if (blocker.empty() && !job_blocker.empty()) blocker = job_blocker;
    if (blocker.empty() && page.task.Running()) blocker = "A conversion is running.";
    ImGui::BeginDisabled(!blocker.empty() || !errors.empty());
    if (ImGui::Button("Build native pack##native_build")) StartNativeConvert(state, page, resources);
    ImGui::EndDisabled();
    Wrapped(colors.secondary, blocker);
    if (!page.native_message.empty()) Wrapped(page.native_ok ? colors.ok : colors.error, page.native_message);
    DrawJobPanel(state, colors, "music");
    ImGui::Unindent();
}

void PollMusic(AppState& state, MusicPage& page) {
    const BackgroundTask::State before = page.task.Running() ? BackgroundTask::State::Running : BackgroundTask::State::Idle;
    const BackgroundTask::State now = page.task.Poll();
    if (before == BackgroundTask::State::Running && now != BackgroundTask::State::Running) {
        const bool ok = now == BackgroundTask::State::Succeeded;
        const std::string text = now == BackgroundTask::State::Cancelled ? "Cancelled. Nothing was changed."
                                                                         : page.task.Message();
        if (page.task_kind == "native") {
            if (ok) {
                page.native_dir = text;
                JobRequest request;
                std::vector<FieldError> errors;
                if (BuildRequest(NativeForm(page, page.native_dir), "build", request, errors)) {
                    RequestJob(state, "music", request);
                    page.native_message.clear();
                } else {
                    page.native_ok = false;
                    page.native_message = errors.front().message;
                    page.RemoveNativeDir();
                }
            } else {
                page.native_ok = false;
                page.native_message = text;
            }
        } else {
            page.message_ok = ok;
            page.message = text;
        }
        if (now == BackgroundTask::State::Succeeded || now == BackgroundTask::State::Failed ||
            now == BackgroundTask::State::Cancelled) {
            state.log.Append(ok ? LogLevel::Info : LogLevel::Warning, "music: " + text);
        }
    }
    const JobStatus* job = TakeFinished(state, "music", page.handled_job);
    if (job != nullptr) {
        page.RemoveNativeDir();
        if (job->state == JobState::Succeeded) {
            page.native_ok = true;
            page.native_message = EnableInstructions("sample", PathFromUtf8(job->output), GameDir(state));
        }
    }
}

void DrawMusicPage(AppState& state, const Colors& colors, MusicPage& page) {
    ImGui::PushID("music");
    PollMusic(state, page);
    Wrapped(colors.secondary, "Replace a game song or add a song for Course Edit tracks. The game streams Ogg Vorbis, "
                              "FLAC and WAV files from its music folder; MIDI files are not converted.");

    ImGui::SeparatorText("Target");
    ImGui::RadioButton("Replace a game song##kind", &page.target_kind, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Add a course song##kind", &page.target_kind, 1);
    if (page.target_kind == 0) {
        const auto& slots = MusicSlots();
        const MusicSlot& current = slots[static_cast<size_t>(page.slot)];
        ImGui::SetNextItemWidth(-FLT_MIN);
        const std::string preview = std::string(current.where) + " (" + current.stem + ")";
        if (ImGui::BeginCombo("##slot", preview.c_str())) {
            for (const MusicSlot& slot : slots) {
                const std::string label = std::string(slot.where) + " (" + slot.stem + ")" +
                                          (slot.streams ? "" : ", not replaceable");
                if (ImGui::Selectable(label.c_str(), slot.id == page.slot)) page.slot = slot.id;
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextUnformatted("Song name (the file name in music/)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        InputString("##song_name", page.song_name);
        Wrapped(colors.error, page.song_name.empty() ? std::string() : CheckCourseSongName(page.song_name));
        Wrapped(colors.secondary, "Course songs are picked per track in Course Edit; the Companion does not assign "
                                  "them.");
    }

    ImGui::SeparatorText("Recording");
    PathField(state, "music.source", "Recording (.ogg, .flac or .wav)", page.source, BrowseKind::OpenFile, "Recordings",
              "*.ogg;*.flac;*.wav");
    const std::string before = page.inspected;
    Reinspect(page.source, page.inspected, page.info, page.inspect_error);
    if (page.inspected != before && page.info.frames > 0) {
        page.loop_start = page.info.has_loop ? page.info.loop_start : 0;
        page.loop_end = page.info.has_loop ? page.info.loop_end : page.info.frames;
        if (!LoopPolicyOffered(page.info.format, PolicyOf(page.policy))) page.policy = 0;
    }
    Wrapped(colors.error, page.inspect_error);
    if (page.info.frames > 0) {
        Wrapped(colors.text, std::string(FormatName(page.info.format)) + ", " + std::to_string(page.info.rate) + " Hz, " +
                                 (page.info.channels == 1 ? "mono" : "stereo") + ", " +
                                 Clock(page.info.frames, page.info.rate) + ".");
        DrawPreview(state, colors, page);
        DrawLoop(state, colors, page);
        DrawLevel(colors, page);
    } else {
        UpdateWave(page);
    }

    DrawInstall(state, colors, page);
    DrawTaskPanel(page.task, colors, "music_task");
    if (!page.message.empty()) Wrapped(page.message_ok ? colors.ok : colors.error, page.message);
    DrawNative(state, colors, page);
    ImGui::PopID();
}

// ---------------------------------------------------------------------------------------------------
// Tracks.

struct TracksPage {
    std::string image;
    BackgroundTask task;
    std::shared_ptr<ImageListing> pending;
    ImageListing listing;
    bool loaded = false;
    std::string error;
    int selected = -1;
    std::string export_path;
    bool overwrite = false;
    std::string message;
    bool message_ok = true;
};

const char* FitLabel(TrackFit fit) {
    switch (fit) {
    case TrackFit::Stock:
        return "Fits";
    case TrackFit::NeedsExtended:
        return "Needs Extended course height";
    case TrackFit::Refused:
        return "Refused";
    }
    return "Refused";
}

void SelectTrack(const AppState& state, TracksPage& page, int index) {
    page.selected = index;
    page.message.clear();
    const fs::path game = GameDir(state);
    page.export_path = game.empty() || index < 0
                           ? std::string()
                           : PathToUtf8(DefaultExportPath(game, page.listing.tracks[static_cast<size_t>(index)]));
}

void DrawTracksPage(AppState& state, const Colors& colors, TracksPage& page) {
    ImGui::PushID("tracks");
    Wrapped(colors.secondary, "Look at the Course Edit tracks on a 64DD disk image from another player or emulator, "
                              "with the game's own checks, and export one as a .gdxc the game imports. Reading never "
                              "changes the disk image.");
    if (page.task.Running() == false && page.pending) {
        const BackgroundTask::State done = page.task.Poll();
        if (done == BackgroundTask::State::Succeeded) {
            page.listing = std::move(*page.pending);
            page.loaded = true;
            page.error.clear();
            SelectTrack(state, page, page.listing.tracks.empty() ? -1 : 0);
        } else {
            page.error = done == BackgroundTask::State::Cancelled ? "Cancelled." : page.task.Message();
        }
        page.pending.reset();
    } else {
        page.task.Poll();
    }

    PathField(state, "tracks.image", "Disk image (.ndd, .disk or .ram)", page.image, BrowseKind::OpenFile,
              "64DD disk images", "*.ndd;*.disk;*.ram");
    ImGui::BeginDisabled(page.image.empty() || page.task.Running());
    if (ImGui::Button("Read the disk##read")) {
        page.loaded = false;
        page.selected = -1;
        auto result = std::make_shared<ImageListing>();
        page.pending = result;
        const fs::path path = PathFromUtf8(page.image);
        page.task.Start("Reading the disk", [result, path](const CancelFlag&, ProgressValue& progress,
                                                           std::string& message) {
            std::string error;
            if (!ReadDiskImage(path, *result, error)) {
                message = error;
                return false;
            }
            progress.store(1.0f);
            return true;
        });
    }
    ImGui::EndDisabled();
    DrawTaskPanel(page.task, colors, "tracks_task");
    Wrapped(colors.error, page.error);
    if (!page.loaded) {
        ImGui::PopID();
        return;
    }
    if (page.listing.tracks.empty()) Wrapped(colors.secondary, "The disk holds no Course Edit tracks.");

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    const float height = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min<size_t>(
                                                                     page.listing.tracks.size() + 1, 9));
    if (!page.listing.tracks.empty() && ImGui::BeginTable("##track_table", 4, flags, ImVec2(0.0f, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Track");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Result");
        ImGui::TableSetupColumn("Points");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < page.listing.tracks.size(); ++i) {
            const ImageTrack& track = page.listing.tracks[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(track.display_name.c_str(), page.selected == static_cast<int>(i),
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                SelectTrack(state, page, static_cast<int>(i));
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(track.extension.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(track.read_error.empty() ? FitLabel(track.inspection.fit) : "Unreadable");
            ImGui::TableNextColumn();
            ImGui::Text("%d", track.inspection.control_points);
        }
        ImGui::EndTable();
    }
    if (!page.listing.machines.empty()) {
        std::string names;
        for (const ImageMachine& machine : page.listing.machines) {
            if (!names.empty()) names += ", ";
            names += machine.display_name;
        }
        Wrapped(colors.secondary, "Machines on this disk (listed only; the Companion does not export machines): " + names);
    }

    if (page.selected >= 0 && static_cast<size_t>(page.selected) < page.listing.tracks.size()) {
        const ImageTrack& track = page.listing.tracks[static_cast<size_t>(page.selected)];
        ImGui::SeparatorText(("Track " + track.display_name).c_str());
        Wrapped(colors.error, track.read_error);
        const TrackInspection& inspection = track.inspection;
        if (track.read_error.empty()) {
            if (inspection.fit == TrackFit::Stock) {
                Wrapped(colors.ok, "Passes the game's checks.");
            } else if (inspection.fit == TrackFit::NeedsExtended) {
                Wrapped(colors.warning, "Passes only with Enhancements > Course Edit & Machine > Extended course "
                                        "height (experimental) turned on in the game.");
            } else {
                for (const std::string& line : DescribeTrackErrors(inspection.extended_errors)) {
                    Wrapped(colors.error, line);
                }
            }
            if (inspection.diagnostic.sanitizedPits > 0) {
                Wrapped(colors.secondary, std::to_string(inspection.diagnostic.sanitizedPits) +
                                              " pit values outside the editor's range are cleared on export, as the "
                                              "game's import does.");
            }
        }
        if (inspection.fit != TrackFit::Refused) {
            PathField(state, "tracks.export", "Export to (.gdxc)", page.export_path, BrowseKind::SaveFile, "Track exports",
                      "*.gdxc");
            ImGui::Checkbox("Replace an existing file##track_overwrite", &page.overwrite);
            ImGui::BeginDisabled(page.export_path.empty());
            if (ImGui::Button("Export##track_export")) {
                const fs::path output = PathFromUtf8(page.export_path);
                const fs::path game = GameDir(state);
                std::error_code ec;
                if (!game.empty() && output.parent_path() == game / "exports") fs::create_directories(output.parent_path(), ec);
                std::string error;
                page.message_ok = ExportTrack(track, output, page.overwrite, error);
                page.message = page.message_ok ? "Exported. In the game, open Workshop > Content Library to import it." +
                                                     std::string(game.empty() || output.parent_path() == game / "exports"
                                                                     ? ""
                                                                     : " Put the file in the game's exports folder first.")
                                               : error;
                state.log.Append(page.message_ok ? LogLevel::Info : LogLevel::Warning, "tracks: " + page.message);
            }
            ImGui::EndDisabled();
            if (page.export_path.empty()) Wrapped(colors.secondary, "Choose where to export, or set the game folder in Settings.");
        }
        if (!page.message.empty()) Wrapped(page.message_ok ? colors.ok : colors.error, page.message);
    }
    ImGui::PopID();
}

struct InspectPage {
    std::string archive;     // UTF-8, the pack to read
    std::string dump;        // UTF-8, the game's texture dump, or empty
    bool dump_defaulted = false;
    std::string output;      // UTF-8, the new folder to unpack into
    bool output_auto = true;  // output follows the archive until the user changes it
    std::string handled;     // the last job id read
    bool loaded = false;
    Event listing;           // the last inspect or unpack result
    std::string listed;      // the archive that listing describes
    std::string filter;
};

constexpr char kInspectOwner[] = "inspect";

// <folder>/<stem>-unpacked, or the first <stem>-unpacked-<n> that does not exist yet.
std::string DefaultUnpackPath(const std::string& archive) {
    if (archive.empty()) return std::string();
    const fs::path path = PathFromUtf8(archive);
    const std::string stem = FileStem(PathToUtf8(path.stem()), "pack") + "-unpacked";
    std::error_code ec;
    for (int n = 1; n < 100; ++n) {
        const fs::path candidate = path.parent_path() / PathFromUtf8(n == 1 ? stem : stem + "-" + std::to_string(n));
        if (!fs::exists(candidate, ec)) return PathToUtf8(candidate);
    }
    return PathToUtf8(path.parent_path() / PathFromUtf8(stem));
}

JobRequest ReaderRequest(const InspectPage& page, const std::string& op) {
    JobRequest request;
    request.op = op;
    request.packer = "unpack";
    request.input = page.archive;
    request.catalog = page.dump;
    if (op == "unpack") request.output = page.output;
    return request;
}

// Field, value: a read-only line that can be selected and copied.
void CopyableLine(const char* label, const std::string& value) {
    if (value.empty()) return;
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    std::string copy = value;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    InputString("##value", copy, ImGuiInputTextFlags_ReadOnly);
    ImGui::PopID();
}

// Splits "path\tkind\tdetail".
void SplitEntry(const std::string& row, std::string& path, std::string& kind, std::string& detail) {
    const size_t a = row.find('\t');
    const size_t b = a == std::string::npos ? std::string::npos : row.find('\t', a + 1);
    path = row.substr(0, a);
    kind = a == std::string::npos ? std::string() : row.substr(a + 1, b == std::string::npos ? b : b - a - 1);
    detail = b == std::string::npos ? std::string() : row.substr(b + 1);
}

bool ContainsFolded(const std::string& text, const std::string& needle) {
    if (needle.empty()) return true;
    const auto lower = [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); };
    std::string a(text.size(), ' ');
    std::string b(needle.size(), ' ');
    std::transform(text.begin(), text.end(), a.begin(), lower);
    std::transform(needle.begin(), needle.end(), b.begin(), lower);
    return a.find(b) != std::string::npos;
}

void DrawListing(AppState& state, const Colors& colors, InspectPage& page) {
    const Event& listing = page.listing;
    ImGui::SeparatorText("What the pack holds");
    CopyableLine("Pack:", page.listed);
    CopyableLine("Title:", listing.title);
    CopyableLine("Author:", listing.author);
    CopyableLine("Version:", listing.pack_version);
    CopyableLine("Pack id:", listing.pack_id);
    Wrapped(colors.text, listing.description);
    for (const std::string& line : listing.summary) Wrapped(colors.secondary, line);

    if (listing.op == "unpack" && !listing.folder.empty()) {
        ImGui::SeparatorText("Unpacked");
        CopyableLine("Folder:", listing.folder);
        if (ImGui::Button("Open folder##inspect_open")) state.open_folder = listing.folder;
        Wrapped(colors.secondary,
                "textures/ is laid out like the game's texture dump, with its own manifest.tsv: point the texture "
                "packer on the Textures page at it to build the pack again. alt-textures/ holds the alternate "
                "channel and palette-variants/ the per-palette textures. files/ holds everything else as stored "
                "(scripts, JSON, audio, models), and contents.tsv lists every entry with what was done with it.");
    }

    ImGui::SeparatorText("Entries");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    InputString("Filter##inspect_filter", page.filter);
    if (static_cast<int64_t>(listing.entries.size()) < listing.total) {
        Wrapped(colors.secondary, "Showing the first " + std::to_string(listing.entries.size()) + " of " +
                                      std::to_string(listing.total) +
                                      " entries. Unpack the pack to get contents.tsv, which lists all of them.");
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
    const float height = ImGui::GetTextLineHeightWithSpacing() * 16.0f;
    if (!listing.entries.empty() && ImGui::BeginTable("##inspect_table", 3, flags, ImVec2(0.0f, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        std::string path, kind, detail;
        for (const std::string& row : listing.entries) {
            if (!ContainsFolded(row, page.filter)) continue;
            SplitEntry(row, path, kind, detail);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(path.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kind.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(detail.c_str());
            if (!detail.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", detail.c_str());
        }
        ImGui::EndTable();
    }
}

void DrawInspectPage(AppState& state, const Colors& colors, InspectPage& page) {
    ImGui::PushID(kInspectOwner);
    Wrapped(colors.secondary,
            "Look inside a pack someone else made: an .o2r texture, model or audio pack, or a .gdxp track package. "
            "Read lists what it holds. Unpack copies it into a new folder you can study, with textures turned into "
            "PNG images. Neither one changes the pack.");

    if (const JobStatus* done = TakeFinished(state, kInspectOwner, page.handled)) {
        if (done->state == JobState::Succeeded && ReadsPack(done->result.op)) {
            page.listing = done->result;
            page.listed = page.archive;
            page.loaded = true;
            if (done->result.op == "unpack" && page.output_auto) page.output = DefaultUnpackPath(page.archive);
        }
    }

    const fs::path game = GameDir(state);
    if (!page.dump_defaulted && page.dump.empty() && !game.empty()) {
        std::error_code ec;
        if (fs::is_directory(game / "dump", ec)) page.dump = PathToUtf8(game / "dump");
        page.dump_defaulted = true;
    }

    if (PathField(state, "inspect.archive", "Pack (.o2r or .gdxp)", page.archive, BrowseKind::OpenFile, "Packs",
                  "*.o2r;*.O2R;*.gdxp;*.GDXP")) {
        if (page.output_auto) page.output = DefaultUnpackPath(page.archive);
    }
    PathField(state, "inspect.dump", "Texture dump folder (optional)", page.dump, BrowseKind::Folder);
    Wrapped(colors.secondary,
            "The game's own texture dump (Workshop > Tools > Asset Dump). With it, paletted textures show their real "
            "colors, and unpacked HD textures keep the dump's native size and format so they pack again exactly. "
            "Without it they still unpack, and paletted ones show their color indices in grey.");

    const std::string blocker = JobBlocker(state);
    ImGui::BeginDisabled(page.archive.empty() || !blocker.empty());
    if (ImGui::Button("Read the pack##inspect_read")) RequestJob(state, kInspectOwner, ReaderRequest(page, "inspect"));
    ImGui::EndDisabled();

    ImGui::SeparatorText("Unpack");
    // A folder pick is where the new folder goes, so the pack's own name is kept.
    const auto picked = state.browse_results.find("inspect.output");
    if (picked != state.browse_results.end()) {
        const std::string stem = FileStem(PathToUtf8(PathFromUtf8(page.archive).stem()), "pack") + "-unpacked";
        page.output = PathToUtf8(PathFromUtf8(picked->second) / PathFromUtf8(stem));
        page.output_auto = false;
        state.browse_results.erase(picked);
    }
    if (PathField(state, "inspect.output", "Unpack into (a new folder)", page.output, BrowseKind::Folder)) {
        page.output_auto = false;
    }
    std::error_code ec;
    const bool output_exists = !page.output.empty() && fs::exists(PathFromUtf8(page.output), ec);
    if (output_exists) Wrapped(colors.warning, "That folder already exists. Unpack only makes a new folder.");
    ImGui::BeginDisabled(page.archive.empty() || page.output.empty() || output_exists || !blocker.empty());
    if (ImGui::Button("Unpack##inspect_unpack")) RequestJob(state, kInspectOwner, ReaderRequest(page, "unpack"));
    ImGui::EndDisabled();
    if (!blocker.empty() && !page.archive.empty()) Wrapped(colors.secondary, blocker);

    DrawJobPanel(state, colors, kInspectOwner);
    if (page.loaded) DrawListing(state, colors, page);
    ImGui::PopID();
}

}  // namespace

// ---------------------------------------------------------------------------------------------------

struct WorkspacePages::Impl {
    Colors colors{};
    PackerPage textures;
    PackerPage models;
    PackerPage advanced;
    MusicPage music;
    TracksPage tracks;
    InspectPage inspect;
};

WorkspacePages::WorkspacePages() : impl_(std::make_unique<Impl>()) {
    PackerPage& textures = impl_->textures;
    textures.owner = "textures";
    textures.title = "Textures";
    textures.packers = {"texture", "htc"};
    textures.notes = {
        "Start from the game's texture dump (Workshop > Tools > Asset Dump): edit the dumped images, keep their "
        "folders and the dump's manifest.tsv, and pack the folder. Validate lists the textures you changed and any "
        "file that matches no game texture.",
    };
    PackerPage& models = impl_->models;
    models.owner = "models";
    models.title = "Models";
    models.packers = {"model"};
    models.notes = {
        "A model pack replaces the static body of a machine. Wheels, boosters and effects stay as the game draws "
        "them. Machines the game's dump did not capture cannot be rebuilt.",
    };
    PackerPage& advanced = impl_->advanced;
    advanced.owner = "advanced";
    advanced.title = "Advanced";
    advanced.packers = {"sequence", "soundfont", "sample"};
    advanced.notes = {
        "These packers work in the game's own audio formats. A sequence is note data in the N64 format; MIDI files "
        "are not converted. To replace a song with a recording, use Music instead.",
    };
}

WorkspacePages::~WorkspacePages() = default;

void WorkspacePages::SetTheme(const ThemeTokens& theme) {
    impl_->colors.text = ToVec4(theme.text);
    impl_->colors.secondary = ToVec4(theme.text_secondary);
    impl_->colors.error = ToVec4(theme.status_error);
    impl_->colors.ok = ToVec4(theme.status_ok);
    impl_->colors.warning = ToVec4(theme.status_warning);
    impl_->colors.accent = ToVec4(theme.accent);
    impl_->colors.panel = ToVec4(theme.panel);
}

void WorkspacePages::Draw(Section section, AppState& state) {
    const Colors& colors = impl_->colors;
    switch (section) {
    case Section::Music:
        DrawMusicPage(state, colors, impl_->music);
        break;
    case Section::Textures:
        DrawPackerPage(state, colors, impl_->textures);
        break;
    case Section::Models:
        DrawPackerPage(state, colors, impl_->models);
        break;
    case Section::Tracks:
        DrawTracksPage(state, colors, impl_->tracks);
        break;
    case Section::Inspect:
        DrawInspectPage(state, colors, impl_->inspect);
        break;
    case Section::Advanced:
        DrawPackerPage(state, colors, impl_->advanced);
        break;
    case Section::Settings:
        break;
    }
}

}  // namespace gdx_companion
