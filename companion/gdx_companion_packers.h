// Copyright (c) Zorkats
//
// The packers a workspace page can run, mirrored from PACKERS in companion/worker/gdx_companion_jobs.py,
// and the form a page fills in before it sends a job. The form is checked here first so a page can show
// each problem under its field and keep Build disabled; the worker checks the same request again.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "gdx_companion_jobs.h"

namespace gdx_companion {

inline constexpr size_t kMaxTextOption = 256;  // MAX_TEXT_OPTION
inline constexpr size_t kMaxBases = 8;         // MAX_BASES

enum class FieldKind { Text, Path, Flag, Paths };

struct OptionSpec {
    const char* key;
    FieldKind kind;
    const char* label;
    const char* help;
    bool required;
};

struct PackerSpec {
    const char* id;
    const char* title;
    const char* summary;
    bool input_is_dir;        // false: the input is one file
    const char* input_label;
    const char* input_filter;  // file picker pattern for a file input, for example "*.htc"
    bool has_catalog;          // the build needs a dump folder (sample: --dump-dir)
    const char* catalog_label;
    std::vector<OptionSpec> options;
    const char* init_choice;  // the init choice key (machine, font), or nullptr when init is not offered
    const char* init_choice_label;
    std::vector<OptionSpec> init_options;
};

const std::vector<PackerSpec>& Packers();
const PackerSpec* FindPacker(std::string_view id);

// Field names used in errors: "input", "output", "catalog", "overwrite", "project", "init_catalog",
// "choice", or an option key.
struct FieldError {
    std::string field;
    std::string message;
};

struct PackerForm {
    std::string packer;
    std::string input;    // UTF-8
    std::string output;   // UTF-8, the .o2r
    std::string catalog;  // UTF-8, the dump folder for packers with a catalog
    bool overwrite = false;
    std::map<std::string, std::string> text;  // Text and Path options; an empty value is left out
    std::map<std::string, bool> flags;
    std::map<std::string, std::vector<std::string>> paths;
    // Create (init).
    std::string project;       // the new project folder
    std::string init_catalog;  // the dump folder it starts from
    std::string choice;        // the machine or font
    std::map<std::string, bool> init_flags;
};

// The problems that stop op (check, plan, build or init) for this form. Empty means the request may be
// sent. Paths are checked on disk, so call it again when the page is drawn, not once.
std::vector<FieldError> ValidateForm(const PackerForm& form, const std::string& op);

// The first message for field, or nullptr.
const FieldError* FindError(const std::vector<FieldError>& errors, std::string_view field);

// Fills request for op. Returns false when ValidateForm reports a problem; errors holds them.
bool BuildRequest(const PackerForm& form, const std::string& op, JobRequest& request, std::vector<FieldError>& errors);

}  // namespace gdx_companion
