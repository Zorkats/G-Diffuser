// Copyright (c) Zorkats

#include "gdx_companion_packers.h"

#include <filesystem>
#include <system_error>

#include "gdx_companion_json.h"
#include "gdx_companion_paths.h"

namespace gdx_companion {

namespace fs = std::filesystem;

namespace {

std::vector<OptionSpec> Metadata() {
    return {
        {"name", FieldKind::Text, "Pack name", "Shown in Workshop > Mods.", false},
        {"author", FieldKind::Text, "Author", "Shown in Workshop > Mods.", false},
        {"version", FieldKind::Text, "Version", "For example 1.0.", false},
        {"id", FieldKind::Text, "Pack id", "A stable identifier, for example author.packname.", false},
        {"game_version", FieldKind::Text, "Game version", "The target game build. Leave empty for us.rev0.", false},
        {"manifest", FieldKind::Path, "Metadata file",
         "A workshop.json or manifest.json. Leave empty to use the one in the source folder, or the fields above.",
         false},
    };
}

std::vector<PackerSpec> MakePackers() {
    std::vector<PackerSpec> out;

    PackerSpec htc{};
    htc.id = "htc";
    htc.title = "Convert a .htc texture pack";
    htc.summary =
        "Turns a Rice or GlideN64 .htc texture cache into a G-Diffuser pack, by matching each texture against "
        "the game's own archives.";
    htc.input_is_dir = false;
    htc.input_label = "The .htc file";
    htc.input_filter = "*.htc";
    htc.options = {
        {"name", FieldKind::Text, "Pack name", "Shown in Workshop > Mods.", true},
        {"author", FieldKind::Text, "Author", "Shown in Workshop > Mods.", true},
        {"version", FieldKind::Text, "Version", "Leave empty for 0.1.", false},
        {"base", FieldKind::Paths, "Game archives to match against",
         "The game's .o2r archives, for example f3dx.o2r. Later archives win. Up to 8.", true},
        {"alt", FieldKind::Flag, "Alternate assets channel",
         "The textures show only while Use alternate assets is on.", false},
        {"direct_keys", FieldKind::Flag, "Direct keys (advanced)",
         "Each texture shadows the base resource itself. Cannot be combined with the alternate channel.", false},
    };
    out.push_back(htc);

    PackerSpec texture{};
    texture.id = "texture";
    texture.title = "Texture pack";
    texture.summary =
        "Packs a folder of replacement images, laid out like the game's texture dump, with its manifest.tsv.";
    texture.input_is_dir = true;
    texture.input_label = "Replacement images folder";
    texture.options = Metadata();
    texture.options.push_back({"key_scheme_version", FieldKind::Text, "Key scheme version",
                               "Leave empty for the current scheme.", false});
    texture.options.push_back({"native_manifest", FieldKind::Path, "Dump manifest",
                               "The dump's manifest.tsv. Leave empty to use the one in the images folder.", false});
    texture.options.push_back({"direct_keys", FieldKind::Flag, "Direct keys (advanced)",
                               "Each image shadows the base resource itself.", false});
    out.push_back(texture);

    PackerSpec model{};
    model.id = "model";
    model.title = "Machine model pack";
    model.summary =
        "Builds replacement machine bodies from a model project. Only the static body is replaced: wheels, "
        "boosters and effects stay as the game draws them.";
    model.input_is_dir = true;
    model.input_label = "Model project folder";
    model.options = {
        {"name", FieldKind::Text, "Pack name", "Shown in Workshop > Mods.", false},
        {"author", FieldKind::Text, "Author", "Shown in Workshop > Mods.", false},
        {"version", FieldKind::Text, "Version", "For example 1.0.", false},
        {"id", FieldKind::Text, "Pack id", "A stable identifier, for example author.packname.", false},
        {"game_version", FieldKind::Text, "Game version", "Leave empty for us.rev0.", false},
        {"allow_overbudget", FieldKind::Flag, "Allow over-budget detail levels",
         "Reports triangle budget overruns as warnings instead of errors.", false},
    };
    model.init_choice = "machine";
    model.init_choice_label = "Machine";
    out.push_back(model);

    PackerSpec sequence{};
    sequence.id = "sequence";
    sequence.title = "Sequence pack";
    sequence.summary =
        "Packs replacement music sequences in the game's own sequence format. It does not convert MIDI files.";
    sequence.input_is_dir = true;
    sequence.input_label = "Sequences folder";
    sequence.options = Metadata();
    out.push_back(sequence);

    PackerSpec soundfont{};
    soundfont.id = "soundfont";
    soundfont.title = "Soundfont pack";
    soundfont.summary = "Builds replacement instrument banks from a soundfont project.";
    soundfont.input_is_dir = true;
    soundfont.input_label = "Soundfont project folder";
    soundfont.options = Metadata();
    soundfont.init_choice = "font";
    soundfont.init_choice_label = "Soundfont";
    soundfont.init_options = {
        {"skip_samples", FieldKind::Flag, "Skip the stock samples",
         "Writes silent placeholders instead of re-encoding the dumped samples.", false},
    };
    out.push_back(soundfont);

    PackerSpec sample{};
    sample.id = "sample";
    sample.title = "Native sample pack";
    sample.summary =
        "Packs mono WAV files named after native sample keys, each with an optional .loop.json. All installed "
        "sample packs share 1 MiB.";
    sample.input_is_dir = true;
    sample.input_label = "WAV folder";
    sample.has_catalog = true;
    sample.catalog_label = "Dump audio folder";
    sample.options = Metadata();
    out.push_back(sample);

    return out;
}

bool IsSafeText(const std::string& value) {
    if (value.empty() || value.size() > kMaxTextOption || value[0] == '-') return false;
    for (unsigned char c : value) {
        if (c < 32 || c == 127) return false;
    }
    return IsValidUtf8(value);
}

bool EndsWithO2r(const std::string& text) {
    if (text.size() < 4) return false;
    std::string tail = text.substr(text.size() - 4);
    for (char& c : tail) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return tail == ".o2r";
}

void Add(std::vector<FieldError>& errors, const std::string& field, const std::string& message) {
    errors.push_back({field, message});
}

// An absolute path that exists as a folder (want_dir) or a regular file.
bool CheckExisting(std::vector<FieldError>& errors, const std::string& field, const std::string& value,
                   bool want_dir, const char* what) {
    if (value.empty()) {
        Add(errors, field, std::string("Choose ") + what + ".");
        return false;
    }
    const fs::path path = PathFromUtf8(value);
    if (!path.is_absolute()) {
        Add(errors, field, "Use a full path, starting with the drive or the root folder.");
        return false;
    }
    std::error_code ec;
    if (want_dir ? !fs::is_directory(path, ec) : !fs::is_regular_file(path, ec)) {
        Add(errors, field, want_dir ? "No folder at this path." : "No file at this path.");
        return false;
    }
    return true;
}

void CheckOptions(const PackerSpec& spec, const PackerForm& form, std::vector<FieldError>& errors) {
    for (const OptionSpec& option : spec.options) {
        switch (option.kind) {
        case FieldKind::Text: {
            const auto it = form.text.find(option.key);
            const std::string value = it == form.text.end() ? std::string() : it->second;
            if (value.empty()) {
                if (option.required) Add(errors, option.key, std::string(option.label) + " is required.");
            } else if (!IsSafeText(value)) {
                Add(errors, option.key, "Use 1 to 256 printable characters, not starting with '-'.");
            }
            break;
        }
        case FieldKind::Path: {
            const auto it = form.text.find(option.key);
            if (it != form.text.end() && !it->second.empty()) {
                CheckExisting(errors, option.key, it->second, false, "a file");
            } else if (option.required) {
                Add(errors, option.key, std::string(option.label) + " is required.");
            }
            break;
        }
        case FieldKind::Paths: {
            const auto it = form.paths.find(option.key);
            const size_t count = it == form.paths.end() ? 0 : it->second.size();
            if (count == 0) {
                if (option.required) Add(errors, option.key, "Add at least one file.");
            } else if (count > kMaxBases) {
                Add(errors, option.key, "Use at most 8 files.");
            } else {
                for (const std::string& path : it->second) {
                    if (!CheckExisting(errors, option.key, path, false, "a file")) break;
                }
            }
            break;
        }
        case FieldKind::Flag:
            break;
        }
    }
    const auto alt = form.flags.find("alt");
    const auto direct = form.flags.find("direct_keys");
    if (alt != form.flags.end() && alt->second && direct != form.flags.end() && direct->second) {
        Add(errors, "direct_keys", "Turn off the alternate assets channel to use direct keys.");
    }
}

void CheckOutput(const PackerForm& form, const std::string& op, std::vector<FieldError>& errors) {
    if (form.output.empty()) {
        Add(errors, "output", "Choose where to save the .o2r pack.");
        return;
    }
    const fs::path path = PathFromUtf8(form.output);
    if (!path.is_absolute()) {
        Add(errors, "output", "Use a full path, starting with the drive or the root folder.");
        return;
    }
    if (!EndsWithO2r(form.output)) {
        Add(errors, "output", "The pack must end in .o2r.");
        return;
    }
    std::error_code ec;
    if (!fs::is_directory(path.parent_path(), ec)) {
        Add(errors, "output", "The folder for the pack does not exist.");
        return;
    }
    if (fs::is_directory(path, ec)) {
        Add(errors, "output", "This path is a folder. Name a file that ends in .o2r.");
        return;
    }
    if (op == "build" && fs::exists(path, ec) && !form.overwrite) {
        Add(errors, "overwrite", "A pack already exists here. Tick Replace to build over it; the old one is kept as "
                                 ".previous.");
    }
}

void CheckInit(const PackerSpec& spec, const PackerForm& form, std::vector<FieldError>& errors) {
    if (spec.init_choice == nullptr) {
        Add(errors, "project", "This packer has no Create step.");
        return;
    }
    if (form.project.empty()) {
        Add(errors, "project", "Choose a name and place for the new project folder.");
    } else {
        const fs::path path = PathFromUtf8(form.project);
        std::error_code ec;
        if (!path.is_absolute()) {
            Add(errors, "project", "Use a full path, starting with the drive or the root folder.");
        } else if (fs::exists(path, ec)) {
            Add(errors, "project", "Something already exists here. Pick a new folder name.");
        } else if (!fs::is_directory(path.parent_path(), ec)) {
            Add(errors, "project", "The folder that would hold the project does not exist.");
        }
    }
    CheckExisting(errors, "init_catalog", form.init_catalog, true, "the game's dump folder");
    if (form.choice.empty()) {
        Add(errors, "choice", std::string("Choose a ") + spec.init_choice_label + ".");
    } else if (!IsSafeText(form.choice)) {
        Add(errors, "choice", "Pick an entry from the list.");
    }
}

}  // namespace

const std::vector<PackerSpec>& Packers() {
    static const std::vector<PackerSpec> packers = MakePackers();
    return packers;
}

const PackerSpec* FindPacker(std::string_view id) {
    for (const PackerSpec& spec : Packers()) {
        if (id == spec.id) return &spec;
    }
    return nullptr;
}

std::vector<FieldError> ValidateForm(const PackerForm& form, const std::string& op) {
    std::vector<FieldError> errors;
    const PackerSpec* spec = FindPacker(form.packer);
    if (spec == nullptr) {
        Add(errors, "packer", "Unknown packer.");
        return errors;
    }
    if (op == "init") {
        CheckInit(*spec, form, errors);
        return errors;
    }
    if (op != "check" && op != "plan" && op != "build") {
        Add(errors, "op", "Unknown operation.");
        return errors;
    }
    CheckExisting(errors, "input", form.input, spec->input_is_dir,
                  spec->input_is_dir ? "the source folder" : "the source file");
    if (spec->has_catalog) CheckExisting(errors, "catalog", form.catalog, true, "the dump audio folder");
    CheckOptions(*spec, form, errors);
    if (op != "check") CheckOutput(form, op, errors);
    return errors;
}

const FieldError* FindError(const std::vector<FieldError>& errors, std::string_view field) {
    for (const FieldError& error : errors) {
        if (error.field == field) return &error;
    }
    return nullptr;
}

bool BuildRequest(const PackerForm& form, const std::string& op, JobRequest& request,
                  std::vector<FieldError>& errors) {
    errors = ValidateForm(form, op);
    if (!errors.empty()) return false;
    const PackerSpec& spec = *FindPacker(form.packer);
    request = JobRequest{};
    request.op = op;
    request.packer = spec.id;
    if (op == "init") {
        request.output = PathToUtf8(PathFromUtf8(form.project).lexically_normal());
        request.catalog = form.init_catalog;
        JobOption choice;
        choice.key = spec.init_choice;
        choice.kind = JobOption::Kind::Text;
        choice.text = form.choice;
        request.options.push_back(choice);
        for (const OptionSpec& option : spec.init_options) {
            const auto it = form.init_flags.find(option.key);
            if (option.kind == FieldKind::Flag && it != form.init_flags.end() && it->second) {
                JobOption flag;
                flag.key = option.key;
                flag.kind = JobOption::Kind::Flag;
                flag.flag = true;
                request.options.push_back(flag);
            }
        }
        return true;
    }
    request.input = form.input;
    if (spec.has_catalog) request.catalog = form.catalog;
    if (op != "check") {
        request.output = form.output;
        request.overwrite = form.overwrite;
    }
    for (const OptionSpec& option : spec.options) {
        JobOption out;
        out.key = option.key;
        switch (option.kind) {
        case FieldKind::Text:
        case FieldKind::Path: {
            const auto it = form.text.find(option.key);
            if (it == form.text.end() || it->second.empty()) continue;
            out.kind = JobOption::Kind::Text;
            out.text = it->second;
            break;
        }
        case FieldKind::Flag: {
            const auto it = form.flags.find(option.key);
            if (it == form.flags.end() || !it->second) continue;
            out.kind = JobOption::Kind::Flag;
            out.flag = true;
            break;
        }
        case FieldKind::Paths: {
            const auto it = form.paths.find(option.key);
            if (it == form.paths.end() || it->second.empty()) continue;
            out.kind = JobOption::Kind::Paths;
            out.paths = it->second;
            break;
        }
        }
        request.options.push_back(out);
    }
    return true;
}

}  // namespace gdx_companion
