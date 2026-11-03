#include "gdx_track_recovery_runtime.h"
#include "gdx_track_recovery.h"
#include "gdx_recovery_file.h"
#include "gdx_recovery_metadata.h"
#include "gdx_content_io.h"
#include "gdx_content_import.h"
#include "gdx_course_state.h"
#include "gdx_course_snapshot_io.h"
#include "gdx_edit_cup.h"
#include "disk_savefile.h"
#include "port_log.h"
#include "imgui.h"

#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <system_error>

extern "C" int GdxContentRecovery_EnqueueDiskOp(void);
extern "C" int gdx_input_in_gameplay(void);
extern "C" void gdx_yield(void);

namespace {
using namespace gdx::recovery;
enum class Command { Recover, Capture, Delete, Restore };
struct Saved {
    Record record;
    std::filesystem::path path;
};
std::vector<Saved> records;
Saved capture;
size_t restoreIndex = 0;
Command command = Command::Recover;
bool queueReady = false;
bool busy = false;
bool inside = false;
void* ownerThread = nullptr;
bool blocked = true;
bool checked = false;
bool hasCapture = false;
bool mutating = false;
bool cancel = false;
bool changed = false;
uint64_t checkedDisk = 0;
std::string status;
std::filesystem::path root;
std::unique_ptr<FileLock> diskLock;
std::filesystem::path lockedPath;
char confirmation[21]{};

template<class F> auto await(F fn) -> decltype(fn()) {
    auto job = std::async(std::launch::async, std::move(fn));
    while (job.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        gdx_yield();
    }
    return job.get();
}

bool active(const Record& r) {
    char id[26]{};
    return gdx_input_in_gameplay() ||
        (gdx_course_state_edit_file_id(id, sizeof(id)) && id == r.name + "." + r.extension);
}

bool identity(const Record& r) {
    return r.disk == gdx_disk_save_identity() && r.directory == gdx_recovery_native_directory();
}

bool restoreReady(const Record& record) {
    const Change* track = nullptr;
    int cupMask = 0;
    for (const auto& change : record.changes) {
        if (change.kind == Kind::Track) {
            track = &change;
        } else if (!change.before.empty() && change.kind == Kind::CentSlot) {
            cupMask |= 1;
        } else if (!change.before.empty() && change.kind == Kind::CenlSlot) {
            cupMask |= 2;
        }
    }
    return track && canRestoreMetadata(record) &&
        gdx_recovery_native_restore_ready(record.name.c_str(), record.extension.c_str(),
                                          track->before.data(), track->before.size(), cupMask);
}

class Native : public Backend {
  public:
    std::filesystem::path path;
    explicit Native(std::filesystem::path p) : path(std::move(p)) {}
    bool read(const Record& r, const Change& c, Bytes& bytes) override {
        if (!identity(r)) {
            return false;
        }
        if (c.kind == Kind::Track) {
            bytes.resize(65536);
            size_t size = 0;
            if (!gdx_recovery_native_read(r.name.c_str(), r.extension.c_str(), bytes.data(), bytes.size(), &size)) {
                return false;
            }
            bytes.resize(size);
            return true;
        }
        if (int(c.kind) < 6) {
            return readMetadata(r, c.kind, bytes);
        }
        std::array<uint8_t, 216> cent{};
        std::array<uint8_t, 508> cenl{};
        if (!gdx_recovery_native_cups(cent.data(), cenl.data())) {
            return false;
        }
        const bool longNames = c.kind == Kind::CenlSlot;
        const size_t width = longNames ? 21 : 9;
        const auto* slot = longNames ? cenl.data() + 4 + c.slot * width : cent.data() + c.slot * width;
        if (*slot == 0) {
            bytes.clear();
        } else {
            bytes.assign(slot, slot + width);
        }
        return true;
    }
    bool write(const Record& r, const Change& c, const Bytes& bytes) override {
        if (!identity(r)) {
            return false;
        }
        if (c.kind == Kind::Track) {
            return gdx_recovery_native_write(r.name.c_str(), r.extension.c_str(), bytes.data(), bytes.size()) != 0;
        }
        if (int(c.kind) < 6) {
            return writeMetadata(r, c.kind, bytes);
        }
        return gdx_recovery_native_cup_slot(c.kind == Kind::CenlSlot, c.slot, bytes.data(), bytes.size()) != 0;
    }
    bool flush(const Change& c) override {
        if (c.kind == Kind::Track || int(c.kind) >= 6) {
            return gdx_disk_save_recovery_flush() != 0;
        }
        return gdx_course_state_recovery_flush(int(c.kind)) == 0;
    }
    bool journal(const Record& r) override {
        if (cancel && !mutating) {
            return false;
        }
        mutating = true;
        if (command == Command::Delete && r.state == State::Deleting) {
            // The journal's before-images must survive a restart even if an earlier edit was only in RAM.
            if (!gdx_disk_save_recovery_flush() || gdx_course_state_recovery_flush(2) != 0 ||
                gdx_course_state_recovery_flush(4) != 0 || gdx_course_state_recovery_flush(5) != 0) {
                return false;
            }
        }
        Bytes bytes;
        return encode(r, bytes) && gdx_recovery_write_await(path.string().c_str(), bytes.data(), bytes.size());
    }
};

bool scan() {
    char path[2048];
    if (!gdx_disk_save_recovery_path(path, sizeof(path))) {
        return false;
    }
    root = path;
    if (!diskLock || lockedPath != root) {
        auto lockPath = root;
        lockPath += ".lock";
        auto acquired = await([lockPath] { return std::make_unique<FileLock>(lockPath); });
        if (!acquired->held()) {
            throw std::runtime_error("Another process owns this disk save, or its recovery lock is unavailable.");
        }
        diskLock = std::move(acquired);
        lockedPath = root;
    }
    auto scanRoot = root;
    auto loaded = await([scanRoot] {
        std::vector<Saved> result;
        std::error_code ec;
        if (!std::filesystem::exists(scanRoot, ec) && !ec) {
            return result;
        }
        if (ec || !prepareDirectory(scanRoot)) {
            throw std::runtime_error("Recovery directory is unavailable or is a link.");
        }
        std::filesystem::directory_iterator it(scanRoot, ec);
        if (ec) {
            throw std::runtime_error("Cannot list recovery records.");
        }
        for (const auto& item : it) {
            if (item.path().extension() != ".gtr") {
                continue;
            }
            if (result.size() >= 64 || !item.is_regular_file() || item.is_symlink()) {
                throw std::runtime_error("Invalid recovery entry or record limit reached.");
            }
            Bytes bytes;
            Record r;
            if (!readFile(item.path(), bytes) || !decode(bytes, r) ||
                item.path().stem().string() != std::to_string(r.id)) {
                throw std::runtime_error("A recovery record is corrupt or unsupported. No deletion is allowed.");
            }
            result.push_back({std::move(r), item.path()});
        }
        return result;
    });
    records = std::move(loaded);
    return true;
}

bool referencesUnchanged(const Record& r, bool allowRemoved);

bool recover() {
    if (!scan()) {
        return false;
    }
    bool mounted = false;
    for (auto& saved : records) {
        if (saved.record.disk != gdx_disk_save_identity()) {
            status = "Recovery record belongs to a different disk.";
            return false;
        }
        if (saved.record.state == State::Deleted || saved.record.state == State::Restored) {
            continue;
        }
        if (!mounted) {
            if (!gdx_recovery_native_prepare() || !gdx_course_state_recovery_ready()) {
                return false;
            }
            mounted = true;
        }
        if (!identity(saved.record) || active(saved.record)) {
            status = "Pending recovery has a different directory or an active track. Writes remain blocked.";
            return false;
        }
        Native backend(saved.path);
        if (saved.record.state == State::Deleting && !referencesUnchanged(saved.record, true)) {
            status = "Cup references changed outside the recovery record. Writes remain blocked.";
            return false;
        }
        if (saved.record.state == State::Restoring && !restoreReady(saved.record)) {
            status = "Recovery data is invalid or storage capacity is insufficient. Writes remain blocked.";
            return false;
        }
        if (resume(saved.record, backend) != Result::Ok) {
            status = "Recovery could not complete. Retry after resolving the I/O error or conflict; writes remain blocked.";
            return false;
        }
        changed = true;
    }
    return true;
}

bool referencesUnchanged(const Record& r, bool allowRemoved) {
    std::array<uint8_t, 216> cent{};
    std::array<uint8_t, 508> cenl{};
    std::array<char, GDX_EDIT_CUP_WIDE_SIZE> wide{};
    if (!gdx_recovery_native_cups(cent.data(), cenl.data())) {
        return false;
    }
    gdx_edit_cup_unpack_cent(cent.data(), wide.data());
    gdx_edit_cup_merge_cenl(cenl.data(), wide.data());
    uint32_t mask = 0;
    for (int slot = 0; slot < 24; ++slot) {
        if (r.name == wide.data() + slot * 21) {
            mask |= uint32_t(1) << slot;
        }
    }
    return matchesCupReferences(r, mask, allowRemoved);
}

bool captureTrack() {
    auto& r = capture.record;
    if (!gdx_recovery_native_prepare() || !gdx_course_state_recovery_ready() || active(r) || records.size() >= 64) {
        return false;
    }
    r.disk = gdx_disk_save_identity();
    r.directory = uint16_t(gdx_recovery_native_directory());
    r.id = uint64_t(std::chrono::system_clock::now().time_since_epoch().count());
    r.state = State::Deleting;
    r.changes.clear();
    Native backend({});
    for (int k = 1; k <= 5; ++k) {
        Change c;
        c.kind = Kind(k);
        if (!backend.read(r, c, c.before)) {
            return false;
        }
        if (k == 1 && (c.before.size() != 16 + GDX_CONTENT_TRACK_PAYLOAD_SIZE ||
            !gdx_snapshot_import_restore(r.directory, r.name.c_str(), r.extension.c_str(), c.before.data() + 16))) {
            return false;
        }
        r.changes.push_back(std::move(c));
    }
    if (r.changes.front().before.size() != 16 + GDX_CONTENT_TRACK_PAYLOAD_SIZE) {
        return false;
    }
    std::array<uint8_t, 216> cent{};
    std::array<uint8_t, 508> cenl{};
    std::array<char, GDX_EDIT_CUP_WIDE_SIZE> wide{};
    if (!gdx_recovery_native_cups(cent.data(), cenl.data())) {
        return false;
    }
    gdx_edit_cup_unpack_cent(cent.data(), wide.data());
    gdx_edit_cup_merge_cenl(cenl.data(), wide.data());
    for (int slot = 0; slot < 24; ++slot) {
        if (r.name != wide.data() + slot * 21) {
            continue;
        }
        for (auto kind : {Kind::CentSlot, Kind::CenlSlot}) {
            Change c;
            c.kind = kind;
            c.slot = uint8_t(slot);
            if (!backend.read(r, c, c.before)) {
                return false;
            }
            r.changes.push_back(std::move(c));
        }
    }
    capture.path = root / (std::to_string(r.id) + ".gtr");
    const auto path = capture.path;
    if (!await([path] {
            std::error_code ec;
            return !std::filesystem::exists(path, ec) && !ec;
        })) {
        return false;
    }
    Bytes check;
    return encode(r, check);
}

void enqueue(Command next) {
    if (!queueReady || busy || gdx_content_mfs_busy() || gdx_content_import_pending() ||
        gdx_content_cup_pending()) {
        status = "Disk or content work is busy. Try again when it finishes.";
        return;
    }
    command = next;
    busy = true;
    mutating = false;
    cancel = false;
    status = "Preparing recovery operation...";
    if (GdxContentRecovery_EnqueueDiskOp() != 0) {
        busy = false;
        status = "Cannot queue recovery operation.";
    }
}
}

extern "C" int gdx_recovery_blocks_writes(void) {
    return gdx_snapshot_blocks_writes() ||
        (!(inside && ownerThread && ownerThread == gdx_recovery_native_thread()) && gdx_disk_save_identity() &&
         (!checked || checkedDisk != gdx_disk_save_identity() || blocked || busy));
}

extern "C" int gdx_recovery_persistence_busy(void) {
    return gdx_snapshot_busy() ||
        (gdx_disk_save_identity() && (!checked || checkedDisk != gdx_disk_save_identity() || blocked || busy));
}

extern "C" void gdx_recovery_disk_ready(void) {
    queueReady = true;
}

extern "C" void gdx_recovery_tick(void) {
    if (!gdx_snapshot_busy() && !busy && queueReady && gdx_disk_save_identity() && (!checked || checkedDisk != gdx_disk_save_identity())) {
        enqueue(Command::Recover);
    }
}

extern "C" int gdx_recovery_write_await(const char* path, const uint8_t* data, size_t size) {
    if (!inside || !ownerThread || ownerThread != gdx_recovery_native_thread() || !path || (!data && size)) {
        return 0;
    }
    const std::filesystem::path ownedPath(path);
    Bytes bytes;
    if (size) {
        bytes.assign(data, data + size);
    }
    return await([ownedPath, bytes = std::move(bytes)] { return writeFile(ownedPath, bytes); });
}

extern "C" void GdxContentRecovery_RunOnDiskThread(void) {
    inside = true;
    ownerThread = gdx_recovery_native_thread();
    try {
        if (command == Command::Recover) {
            blocked = !recover();
            checked = true;
            checkedDisk = gdx_disk_save_identity();
            if (!blocked) {
                status = "Recovery checked. Interrupted operations are complete.";
            }
        } else if (blocked || !checked || checkedDisk != gdx_disk_save_identity() || gdx_input_in_gameplay()) {
            status = "Recovery is blocked or gameplay is active. No operation started.";
        } else if (command == Command::Capture) {
            hasCapture = captureTrack() && !cancel;
            status = hasCapture ? "Review the exact track and affected references before deleting."
                                : "Cannot prepare deletion: active/missing track, unavailable metadata, invalid cup data, or journal limit.";
        } else {
            Saved* selected = command == Command::Delete ? &capture :
                (restoreIndex < records.size() ? &records[restoreIndex] : nullptr);
            if (!selected || (command == Command::Delete && !hasCapture) || cancel ||
                !gdx_recovery_native_prepare() || !gdx_course_state_recovery_ready() ||
                !identity(selected->record) || active(selected->record) ||
                (command == Command::Delete && !referencesUnchanged(selected->record, false))) {
                status = "Operation rejected: active, stale, or unavailable selection.";
            } else {
                const auto directory = root;
                const auto path = selected->path;
                const bool deleting = command == Command::Delete;
                if (!await([directory, path, deleting] {
                        std::error_code ec;
                        if (!prepareDirectory(directory)) {
                            return false;
                        }
                        return !deleting || (!std::filesystem::exists(path, ec) && !ec);
                    })) {
                    throw std::runtime_error("Cannot reserve recovery destination without overwriting a record.");
                }
                Native backend(selected->path);
                const bool ready = deleting || restoreReady(selected->record);
                const auto result = !ready ? Result::Conflict :
                    deleting ? beginDelete(selected->record, backend) : beginRestore(selected->record, backend);
                blocked = result != Result::Ok && mutating;
                changed = mutating;
                hasCapture = false;
                status = !ready ? "Restore blocked: invalid data or insufficient disk/metadata capacity. No transaction started." :
                    result == Result::Ok ? (deleting ? "Track deleted. Its recovery record is retained for Restore."
                                                         : "Track restored without replacing unrelated data.") :
                    result == Result::Conflict ? "Conflict: selected track, metadata, or cup slots changed. Nothing conflicting was overwritten."
                                              : "Operation failed. Retained recovery data must be checked before further writes.";
                if (!scan()) {
                    blocked = true;
                }
            }
        }
    } catch (const std::exception& error) {
        status = error.what();
        blocked = true;
        checked = true;
        checkedDisk = gdx_disk_save_identity();
    }
    if (!status.empty()) {
        gdx_port_logf("[track-recovery] %s\n", status.c_str());
    }
    inside = false;
    ownerThread = nullptr;
    busy = false;
    mutating = false;
}

bool GdxRecovery_Draw(const GdxContentEntry* selected) {
    const bool refresh = changed && !busy;
    if (refresh) {
        changed = false;
    }
    if (hasCapture && !busy && (!selected || selected->contentType != GDX_CONTENT_TYPE_TRACK ||
        capture.record.name != selected->name || capture.record.extension != selected->extension)) {
        hasCapture = false;
        confirmation[0] = 0;
        status = "Selection changed. Prepare deletion again for the selected track.";
    }
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Track deletion and recovery")) {
        ImGui::TextWrapped("Deletion keeps a recovery record. Shared music files are never removed. The last edited track cannot be deleted while its editor identity is active.");
        ImGui::BeginDisabled(busy || blocked || gdx_content_mfs_busy() || !selected ||
                             selected->contentType != GDX_CONTENT_TYPE_TRACK || gdx_input_in_gameplay());
        if (ImGui::Button("Prepare deletion of selected track")) {
            confirmation[0] = 0;
            capture = {};
            capture.record.name = selected->name;
            capture.record.extension = selected->extension;
            hasCapture = false;
            enqueue(Command::Capture);
        }
        ImGui::EndDisabled();
        if (hasCapture && !busy) {
            ImGui::Text("Delete %s.%s", capture.record.name.c_str(), capture.record.extension.c_str());
            ImGui::Text("Affected cup slots: %zu", (capture.record.changes.size() - 5) / 2);
            ImGui::TextUnformatted("Type the exact track name to confirm:");
            ImGui::InputText("##DeleteTrackConfirmation", confirmation, sizeof(confirmation));
            ImGui::BeginDisabled(capture.record.name != confirmation);
            if (ImGui::Button("Confirm deletion")) {
                confirmation[0] = 0;
                enqueue(Command::Delete);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel deletion")) {
                hasCapture = false;
                confirmation[0] = 0;
            }
        }
        if (busy) {
            ImGui::TextUnformatted(mutating ? "Persisting transaction. Do not close the application." : "Preparing...");
            if (command != Command::Recover && !mutating && ImGui::Button("Cancel before mutation")) {
                cancel = true;
            }
        }
        ImGui::BeginDisabled(busy || gdx_input_in_gameplay());
        if (ImGui::Button("Check / retry recovery")) {
            enqueue(Command::Recover);
        }
        ImGui::EndDisabled();
        if (!status.empty()) {
            ImGui::TextWrapped("%s", status.c_str());
        }
        if (!root.empty()) {
            ImGui::TextWrapped("Recovery directory: %s", root.string().c_str());
        }
        for (size_t i = 0; i < records.size(); ++i) {
            const auto& r = records[i].record;
            ImGui::PushID(int(i));
            ImGui::Text("%s.%s (%s)", r.name.c_str(), r.extension.c_str(),
                        r.state == State::Deleted ? "deleted" : r.state == State::Restored ? "restored" : "interrupted");
            ImGui::BeginDisabled(busy || blocked || r.state != State::Deleted || gdx_input_in_gameplay());
            if (ImGui::Button("Restore...")) {
                restoreIndex = i;
                ImGui::OpenPopup("Confirm restore");
            }
            ImGui::EndDisabled();
            if (ImGui::BeginPopupModal("Confirm restore", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Restore %s.%s?", r.name.c_str(), r.extension.c_str());
                ImGui::TextUnformatted("Reused names, metadata, or cup slots cause a conflict, not an overwrite.");
                if (ImGui::Button("Confirm restore")) {
                    ImGui::CloseCurrentPopup();
                    enqueue(Command::Restore);
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    return refresh;
}
