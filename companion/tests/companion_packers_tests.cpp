// Copyright (c) Zorkats
//
// Headless tests for the packer forms: the table mirrors the worker's PACKERS, each problem lands under
// its own field, and BuildRequest sends what EncodeJobRequest expects. Every file is made in a fresh
// temporary folder. Links gdx_companion_host only.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "gdx_companion_packers.h"
#include "gdx_companion_paths.h"

using namespace gdx_companion;
namespace fs = std::filesystem;

namespace {

int gPassed = 0;
int gFailed = 0;

void Check(bool ok, const char* name) {
    if (ok) {
        ++gPassed;
        std::printf("PASS: %s\n", name);
    } else {
        ++gFailed;
        std::printf("FAIL: %s\n", name);
    }
}

void Touch(const fs::path& path) {
    std::ofstream out(path, std::ios::binary);
    out << "x";
}

std::string U(const fs::path& path) { return PathToUtf8(path); }

const JobOption* FindOption(const JobRequest& request, const std::string& key) {
    for (const JobOption& option : request.options) {
        if (option.key == key) return &option;
    }
    return nullptr;
}

void Table() {
    const char* expected[] = {"htc", "texture", "model", "sequence", "soundfont", "sample"};
    bool order = Packers().size() == 6;
    for (size_t i = 0; order && i < 6; ++i) order = std::string(Packers()[i].id) == expected[i];
    Check(order, "the packers are htc, texture, model, sequence, soundfont and sample");
    Check(FindPacker("model") != nullptr && FindPacker("model")->init_choice != nullptr &&
              std::string(FindPacker("model")->init_choice) == "machine",
          "model offers Create with a machine choice");
    Check(FindPacker("soundfont")->init_choice != nullptr && FindPacker("htc")->init_choice == nullptr &&
              FindPacker("texture")->init_choice == nullptr,
          "only model and soundfont offer Create");
    Check(FindPacker("sample")->has_catalog && !FindPacker("texture")->has_catalog, "only sample needs a catalog");
    Check(!FindPacker("htc")->input_is_dir && FindPacker("texture")->input_is_dir, "htc takes a file, texture a folder");
    Check(FindPacker("nope") == nullptr, "an unknown packer is not found");
}

void Htc(const fs::path& base) {
    const fs::path dir = base / "htc";
    fs::create_directories(dir);
    const fs::path htc = dir / "pack.htc";
    const fs::path archive = dir / "f3dx.o2r";
    Touch(htc);
    Touch(archive);

    PackerForm form;
    form.packer = "htc";
    std::vector<FieldError> errors = ValidateForm(form, "build");
    Check(FindError(errors, "input") && FindError(errors, "name") && FindError(errors, "author") &&
              FindError(errors, "base") && FindError(errors, "output"),
          "an empty htc form reports input, name, author, base and output");
    Check(FindError(errors, "version") == nullptr, "an empty optional version is no problem");

    form.input = U(htc);
    form.text["name"] = "HD Pack";
    form.text["author"] = "Someone";
    form.paths["base"] = {U(archive)};
    form.output = U(dir / "out.o2r");
    JobRequest request;
    Check(BuildRequest(form, "build", request, errors) && errors.empty(), "a complete htc form builds a request");
    Check(request.op == "build" && request.packer == "htc" && request.input == U(htc) &&
              request.output == U(dir / "out.o2r") && !request.overwrite,
          "the request carries op, packer, input and output");
    const JobOption* base_option = FindOption(request, "base");
    Check(base_option != nullptr && base_option->kind == JobOption::Kind::Paths && base_option->paths.size() == 1,
          "base is sent as a path list");
    Check(FindOption(request, "version") == nullptr && FindOption(request, "alt") == nullptr,
          "empty text and false flags are left out");

    form.flags["alt"] = true;
    form.flags["direct_keys"] = true;
    errors = ValidateForm(form, "build");
    Check(FindError(errors, "direct_keys") != nullptr, "the alternate channel and direct keys together are refused");
    form.flags["direct_keys"] = false;
    Check(BuildRequest(form, "build", request, errors) && FindOption(request, "alt") != nullptr &&
              FindOption(request, "alt")->flag,
          "a ticked flag is sent");

    form.text["name"] = "-rf";
    Check(FindError(ValidateForm(form, "build"), "name") != nullptr, "text starting with '-' is refused");
    form.text["name"] = std::string(257, 'a');
    Check(FindError(ValidateForm(form, "build"), "name") != nullptr, "text over 256 bytes is refused");
    form.text["name"] = std::string("a\tb");
    Check(FindError(ValidateForm(form, "build"), "name") != nullptr, "a control character is refused");
    form.text["name"] = "HD Pack";

    form.paths["base"] = std::vector<std::string>(9, U(archive));
    Check(FindError(ValidateForm(form, "build"), "base") != nullptr, "more than 8 archives are refused");
    form.paths["base"] = {U(dir / "missing.o2r")};
    Check(FindError(ValidateForm(form, "build"), "base") != nullptr, "a missing archive is refused");
    form.paths["base"] = {U(archive)};

    form.output = U(dir / "out.zip");
    Check(FindError(ValidateForm(form, "build"), "output") != nullptr, "an output that is not .o2r is refused");
    form.output = U(dir / "nowhere" / "out.o2r");
    Check(FindError(ValidateForm(form, "build"), "output") != nullptr, "an output in a missing folder is refused");
    fs::create_directories(dir / "folder.o2r");
    form.output = U(dir / "folder.o2r");
    Check(FindError(ValidateForm(form, "build"), "output") != nullptr, "an output that is a folder is refused");
    form.output = "out.o2r";
    Check(FindError(ValidateForm(form, "build"), "output") != nullptr, "a relative output is refused");

    form.output = U(dir / "existing.o2r");
    Touch(dir / "existing.o2r");
    errors = ValidateForm(form, "build");
    Check(FindError(errors, "overwrite") != nullptr && FindError(errors, "output") == nullptr,
          "building over a pack asks for Replace");
    Check(ValidateForm(form, "plan").empty(), "planning over a pack needs no Replace");
    form.overwrite = true;
    Check(BuildRequest(form, "build", request, errors) && request.overwrite, "Replace is sent as overwrite");

    form.output.clear();
    Check(BuildRequest(form, "check", request, errors) && request.output.empty() && !request.overwrite,
          "check needs no output and sends none");

    form.input = U(dir);
    Check(FindError(ValidateForm(form, "check"), "input") != nullptr, "a folder is refused as the .htc file");
}

void Sample(const fs::path& base) {
    const fs::path dir = base / "sample";
    fs::create_directories(dir / "wavs");
    fs::create_directories(dir / "dump" / "audio");
    PackerForm form;
    form.packer = "sample";
    form.input = U(dir / "wavs");
    form.output = U(dir / "music.o2r");
    std::vector<FieldError> errors = ValidateForm(form, "build");
    Check(FindError(errors, "catalog") != nullptr, "the sample packer requires the dump audio folder");
    form.catalog = U(dir / "dump" / "audio");
    JobRequest request;
    Check(BuildRequest(form, "build", request, errors) && request.catalog == form.catalog,
          "the catalog is sent for the sample packer");
    form.text["manifest"] = U(dir / "missing.json");
    Check(FindError(ValidateForm(form, "build"), "manifest") != nullptr, "a missing metadata file is refused");
}

void Init(const fs::path& base) {
    const fs::path dir = base / "init";
    fs::create_directories(dir / "dump");
    PackerForm form;
    form.packer = "htc";
    Check(FindError(ValidateForm(form, "init"), "project") != nullptr, "Create is refused for htc");

    form.packer = "model";
    std::vector<FieldError> errors = ValidateForm(form, "init");
    Check(FindError(errors, "project") && FindError(errors, "init_catalog") && FindError(errors, "choice"),
          "an empty Create form reports project, dump folder and choice");
    form.project = U(dir / "blue_falcon");
    form.init_catalog = U(dir / "dump");
    form.choice = "blue_falcon";
    JobRequest request;
    Check(BuildRequest(form, "init", request, errors), "a complete Create form builds a request");
    const JobOption* machine = FindOption(request, "machine");
    Check(request.op == "init" && request.output == U(dir / "blue_falcon") && request.catalog == U(dir / "dump") &&
              request.input.empty() && machine != nullptr && machine->text == "blue_falcon",
          "init sends the project as output, the dump as catalog and the choice as an option");
    fs::create_directories(dir / "blue_falcon");
    Check(FindError(ValidateForm(form, "init"), "project") != nullptr, "Create never reuses an existing folder");
    form.project = U(dir / "missing" / "blue_falcon");
    Check(FindError(ValidateForm(form, "init"), "project") != nullptr, "Create needs the parent folder");

    form.packer = "soundfont";
    form.project = U(dir / "font");
    form.choice = "font_3";
    form.init_flags["skip_samples"] = true;
    Check(BuildRequest(form, "init", request, errors) && FindOption(request, "font") != nullptr &&
              FindOption(request, "skip_samples") != nullptr && FindOption(request, "skip_samples")->flag,
          "soundfont Create sends the font and skip_samples");
    form.choice = "-x";
    Check(FindError(ValidateForm(form, "init"), "choice") != nullptr, "an unsafe choice is refused");
}

void Unknown() {
    PackerForm form;
    form.packer = "zip";
    Check(FindError(ValidateForm(form, "build"), "packer") != nullptr, "an unknown packer is refused");
    form.packer = "texture";
    Check(FindError(ValidateForm(form, "names"), "op") != nullptr, "names is not a form operation");
}

}  // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("gdx_companion_packers_tests_" + std::to_string(stamp));
    fs::create_directories(base);

    Table();
    Htc(base);
    Sample(base);
    Init(base);
    Unknown();

    std::error_code ec;
    fs::remove_all(base, ec);
    std::printf("%d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
