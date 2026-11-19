// Production physical runtime vs a full-pad history model with the LUS selection/trim contract.
#include "test_runtime.h"
#include "gdx_physical_input.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardKeyToButtonMapping.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern std::map<std::string, int> values;
extern int checks, lagReads;
void Reset(bool warm);
void Down(int code);
void Up(int code);
unsigned short Apply(int context, unsigned short legacy, int port);
void Legacy(int port, unsigned short bit, std::shared_ptr<Ship::ControllerButtonMapping> mapping);

namespace {
constexpr int Race = GDX_INPUT_CONTEXT_RACE, Menu = GDX_INPUT_CONTEXT_MENU;
constexpr int A = GDX_INPUT_OUT_A;
constexpr unsigned short Abit = 0x8000, Bbit = 0x4000;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #condition); std::exit(1); \
} } while (0)

auto Key(int code) {
    return std::make_shared<Ship::KeyboardKeyToButtonMapping>(0, Abit, static_cast<Ship::KbScancode>(code));
}
void Configure(int lag, int port = 0) {
    values["gSimulatedInputLag"] = lag;
    CHECK(GdxPhysicalInput::AddBinding(port, Race, A, Key(Ship::LUS_KB_E)));
}
unsigned short Poll(unsigned short legacy = 0, int context = Race, int port = 0) {
    return Apply(context, legacy, port);
}
struct Pad {
    unsigned short buttons;
    signed char stickX, stickY;
};
class LegacyHistoryModel {
  public:
    Pad Read(Pad sample, int lag) {
        // Oldest first, unlike production's push_front deque, to keep the oracle independent.
        samples.push_back(sample);
        const auto offset = std::min(samples.size() - 1, static_cast<size_t>(lag));
        const auto selected = samples[samples.size() - 1 - offset];
        while (samples.size() > 6) {
            samples.erase(samples.begin());
        }
        return selected;
    }

  private:
    std::vector<Pad> samples;
};
void TestLagValuesAndSticks() {
    for (int lag : { 0, 1, 3, 6, 99, -1 }) {
        Reset(false);
        Configure(lag);
        LegacyHistoryModel reference;
        for (int frame = 0; frame < 24; ++frame) {
            const bool held = frame % 7 == 1 || frame % 7 == 2;
            if (held) Down(Ship::LUS_KB_E); else Up(Ship::LUS_KB_E);
            const unsigned short raw = (held ? Abit : 0) | (frame % 3 == 0 ? Bbit : 0);
            const Pad sample = { raw, static_cast<signed char>(frame * 6 - 75),
                                static_cast<signed char>(75 - frame * 6) };
            const auto expected = reference.Read(sample, lag);
            auto bridgePad = expected; // The single legacy read has already selected lagged sticks/buttons.
            bridgePad.buttons = Poll(bridgePad.buttons);
            CHECK(bridgePad.buttons == expected.buttons);
            CHECK(bridgePad.stickX == expected.stickX);
            CHECK(bridgePad.stickY == expected.stickY);
        }
    }

    // Literal edge sequences catch startup clamping and off-by-one selection/trim mistakes.
    Reset(false);
    Configure(1);
    CHECK(Poll() == 0);
    Down(Ship::LUS_KB_E); CHECK(Poll() == 0);
    Up(Ship::LUS_KB_E); CHECK(Poll() == Abit);
    CHECK(Poll() == 0);

    Reset(false);
    Configure(3);
    Down(Ship::LUS_KB_E); CHECK(Poll() == Abit); // No artificial zero padding during startup.
    Up(Ship::LUS_KB_E);
    for (int i = 0; i < 3; ++i) CHECK(Poll() == Abit);
    CHECK(Poll() == 0);

    for (int lag : { 6, 99, -1 }) {
        Reset(false);
        Configure(lag);
        Down(Ship::LUS_KB_E); CHECK(Poll() == Abit);
        Up(Ship::LUS_KB_E);
        // LUS selects BEFORE trimming: a full queue temporarily holds seven samples.
        for (int i = 0; i < 6; ++i) CHECK(Poll() == Abit);
        CHECK(Poll() == 0);
    }
}
void TestRuntimeChanges() {
    Reset(false);
    Configure(0);
    CHECK(Poll() == 0);
    Down(Ship::LUS_KB_E); CHECK(Poll() == Abit);
    Up(Ship::LUS_KB_E); CHECK(Poll() == 0);
    values["gSimulatedInputLag"] = 2;
    CHECK(Poll() == Abit); // Lag changes use the existing history immediately, without Invalidate().
    values["gSimulatedInputLag"] = 0;
    CHECK(Poll() == 0);
    Down(Ship::LUS_KB_E); CHECK(Poll() == Abit);
    values["gSimulatedInputLag"] = 99;
    CHECK(Poll() == 0);
    values["gSimulatedInputLag"] = -1;
    Up(Ship::LUS_KB_E); CHECK(Poll() == Abit); // Match LUS's signed-to-size_t conversion, not clamp-to-zero.

    // Record recomposed samples even while every row inherits. First configuration must not
    // start an empty queue, discard old samples, or store a historical replacement mask.
    Reset(false);
    Legacy(0, Abit, Key(Ship::LUS_KB_E));
    GdxPhysicalInput::Invalidate();
    values["gSimulatedInputLag"] = 2;
    Down(Ship::LUS_KB_E); CHECK(Poll(Abit) == Abit);
    Up(Ship::LUS_KB_E); CHECK(Poll() == 0);
    GdxPhysicalInput::ClearBindings(0, Race, A);
    CHECK(Poll() == Abit); // Current empty override replaces the bit, but pending old presses still drain.
    CHECK(Poll() == 0);
    CHECK(GdxPhysicalInput::AddBinding(0, Race, A, Key(Ship::LUS_KB_W)));
    Down(Ship::LUS_KB_W); CHECK(Poll() == 0);
    // Same-count external reload to a different key while lag is active; do not clear the queue.
    values["gEnhancements.Input.Physical.Port1.Race.A.0.Code"] = Ship::LUS_KB_R;
    GdxPhysicalInput::Invalidate();
    Up(Ship::LUS_KB_W); CHECK(Poll() == 0);
    CHECK(Poll() == Abit);
    GdxPhysicalInput::UseLegacyBindings(0, Race, A);
    CHECK(Poll(Bbit) == Bbit); // Current inheritance ignores historical replacement ownership.

    // The inherited context of an output overridden elsewhere uses the same lag, not live input.
    Reset(false);
    values["gSimulatedInputLag"] = 1;
    Legacy(0, Abit, Key(Ship::LUS_KB_Q));
    GdxPhysicalInput::Invalidate();
    GdxPhysicalInput::ClearBindings(0, Race, A);
    CHECK(Poll(0, Menu) == 0);
    Down(Ship::LUS_KB_Q); CHECK(Poll(0, Menu) == 0);
    Up(Ship::LUS_KB_Q); CHECK(Poll(0, Menu) == Abit);
    CHECK(Poll(0, Menu) == 0);
}
void TestBlockerHistory() {
    Reset(false);
    Configure(2);
    auto deck = Ship::Context::current->deck;
    Down(Ship::LUS_KB_E); CHECK(Poll() == Abit);
    Up(Ship::LUS_KB_E); CHECK(Poll() == Abit);
    deck->BlockGameInput(100);
    deck->BlockGameInput(200);
    CHECK(deck->GameInputBlocked());
    const int previousLagReads = lagReads;
    for (int i = 0; i < 4; ++i) CHECK(Poll(0, Menu) == 0);
    CHECK(lagReads == previousLagReads); // Generic blockers skip selection and freeze history.
    deck->UnblockGameInput(100);
    CHECK(Poll(0, Menu) == 0);
    deck->UnblockGameInput(200);
    CHECK(!deck->GameInputBlocked());
    CHECK(Poll(0, Menu) == Abit); // Frozen old press survives exactly as in LUS's queue.
    CHECK(Poll(0, Menu) == 0);

    Reset(false);
    Configure(1);
    deck = Ship::Context::current->deck;
    CHECK(Poll() == 0);
    Down(Ship::LUS_KB_E); CHECK(Poll() == 0);
    deck->keyboardBlocked = true;
    deck->gamepadBlocked = true;
    deck->mouseBlocked = true;
    CHECK(!deck->GameInputBlocked()); // All device checks true does NOT imply a generic blocker.
    CHECK(Poll() == Abit); // Mapping blocks affect the new sample, not the already-queued press.
    CHECK(Poll() == 0);    // Device-blocked zero samples advance the queue, unlike generic blocking.
    Up(Ship::LUS_KB_E);
    CHECK(Poll() == 0);
    deck->keyboardBlocked = false;
    CHECK(Poll(0, Menu) == 0);
    Down(Ship::LUS_KB_E);
    CHECK(Poll(0, Menu) == 0);
    CHECK(Poll(0, Menu) == 0); // Blocked release tracking must not leave a false Race latch.
}
void TestContextLatchesWithLag() {
    Reset(false);
    Configure(1);
    CHECK(GdxPhysicalInput::AddBinding(0, Menu, GDX_INPUT_OUT_B, Key(Ship::LUS_KB_E)));
    CHECK(Poll() == 0);
    Down(Ship::LUS_KB_E); CHECK(Poll() == 0);
    CHECK(Poll(0, Menu) == Abit); // Historical Race press must not become a Menu action.
    Up(Ship::LUS_KB_E); CHECK(Poll(0, Menu) == Abit);
    Down(Ship::LUS_KB_E); CHECK(Poll(0, Menu) == 0);
    CHECK(Poll() == Bbit); // The fresh Menu press retains its context after switching back to Race.
    Up(Ship::LUS_KB_E); CHECK(Poll() == Bbit);
    CHECK(Poll() == 0);
}
void TestPerPortHistory() {
    Reset(false);
    Configure(1, 0);
    Configure(1, 1);
    CHECK(Poll(0, Race, 0) == 0);
    Down(Ship::LUS_KB_E);
    CHECK(Poll(0, Race, 0) == 0);
    CHECK(Poll(0, Race, 1) == Abit); // Port 1's first sample clamps independently of port 0's history.
    Up(Ship::LUS_KB_E);
    CHECK(Poll(0, Race, 0) == Abit);
    CHECK(Poll(0, Race, 1) == Abit);
    CHECK(Poll(0, Race, 0) == 0);
    CHECK(Poll(0, Race, 1) == 0);
}
} // namespace

void TestLagParity() {
    TestLagValuesAndSticks();
    TestRuntimeChanges();
    TestBlockerHistory();
    TestContextLatchesWithLag();
    TestPerPortHistory();
}
