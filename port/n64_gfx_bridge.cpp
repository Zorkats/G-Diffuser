// C/C++ boundary for GFX task submission, called from port/n64_sched.c via extern "C".
// Uses GetInterpreterWeak() + Interpreter::Run() directly, NOT DrawAndRunGraphicsCommands,
// which would double-wrap StartFrame/EndFrame.
// Must build into the G-Diffuser executable target only, never the gdiffuser_game OBJECT library.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
/* POSIX memory-probe backend: /proc/self/maps snapshot helpers plus the #else branches of
 * GetMainModuleRange / ReadableByteLimit / ReadableCommandLimit / IsReadableAddress. */
#include <atomic>
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "ship/Context.h"
#include "fast/Fast3dWindow.h"
#include "fast/lus_gbi.h"
#include "gdx_perf.h" // no-op when GDX_PERF is disabled
#include "gdx_dev_gates.h" // gates every GDX_DIAG_* / behavior switch below
#include "port_log.h"
#include "rom_buffer.h"
#include "gdx_segment_source.h"
#include "n64_rdram.h"
#include "n64_gfx_bridge.h"
#include "n64_gfx_convert.h"
#include "gdx_vi_convert.h"
#include "gdx_interp.h"
#include "gdx_camera_pose.h"
extern "C" {
#include "mio0.h"
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int gGdxRaceActive;
extern "C" int gGameMode;
extern "C" void gdx_ck(const char* s);
extern "C" void gdx_cki(const char* s, int v);

// ---------------------------------------------------------------------------------------------
// Segment-reload seqlock (mode-transition TOCTOU guard).
//
// Mode transitions reload asset segments on the GAME thread: gdx_load_mode_segments() decodes
// fresh bytes into the segment-4/7/9 carves, rewrites them in place, and swaps the gSegments[]
// bases. The GRAPHICS thread reads gSegments[] and the buffer CONTENTS concurrently with no
// other synchronization, so a torn read can hand the interpreter a bogus opcode/pointer pair
// -- e.g. an OTR-filepath opcode (0x25) with a bare 32-bit token where a host string pointer
// is expected, which faults inside strlen on the LUS side.
//
// The game thread brackets every segment mutation with begin()/end(); the counter is ODD while
// a mutation is in flight. The graphics thread snapshots before a segment-backed resolution and
// GdxSegmentEpochStable() rejects the result on an odd or changed snapshot. The graphics thread
// must NEVER block here: an unstable snapshot takes the same graceful hard-skip as a failed
// resolution, dropping one texture for one reload frame. Only atomic loads, no locks, no spins.
static std::atomic<uint32_t> gGdxSegmentEpoch{0};

// acq_rel keeps each increment from being reordered past the segment writes it fences:
// begin()'s odd publish stays BEFORE the buffer/base writes, end()'s even publish AFTER them,
// so a graphics-thread acquire-load seeing an even, unchanged epoch has observed fully-settled
// segment state.
extern "C" void gdx_segment_epoch_begin(void) {
    gGdxSegmentEpoch.fetch_add(1u, std::memory_order_acq_rel);
}
extern "C" void gdx_segment_epoch_end(void) {
    gGdxSegmentEpoch.fetch_add(1u, std::memory_order_acq_rel);
}

// Graphics-thread seqlock read side.
static inline uint32_t GdxSegmentEpochSnapshot() {
    return gGdxSegmentEpoch.load(std::memory_order_acquire);
}
// The acquire fence orders the caller's segment reads (taken after `snap`) before this second
// load, so a mutation that raced those reads is reliably detected as a changed epoch.
static inline bool GdxSegmentEpochStable(uint32_t snap) {
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t now = gGdxSegmentEpoch.load(std::memory_order_acquire);
    return ((snap & 1u) == 0u) && (snap == now);
}
// Diagnostic, strip later. racer.c raises this when the countdown draw runs so the raw vtx/mtx
// trace covers a few frames instead of the whole race (gGdxRaceActive stays 1 all race and
// overflowed the fixed-size trace before the countdown appeared).
extern "C" int gGdxCountdownProbeArm = 0;
// Diagnostic, strip later. The coarse arm above latches for the rest of the process, so racer.c
// also tags the digit quad's own vertex pointer here -- still a raw N64 low32 at translate time,
// directly comparable against in.w1 below. On a matching G_VTX the bridge publishes the RESOLVED
// host pointer so interpreter.cpp's GfxSpVertex can narrow the render-state probe to that draw.
extern "C" unsigned int gGdxCountdownProbeVtxLow32 = 0;
extern "C" uintptr_t gGdxCountdownProbeResolvedVtx = 0;
#include <cstdio>
#include <cstring>
#include <deque> // stable-address scratch-slot arena (deque never invalidates element pointers)
#include <memory>
#include <unordered_map>
#include <vector>
#include <chrono>

extern "C" unsigned long long gSegments[16];

extern "C" uint8_t D_3000000[];
extern "C" uint8_t D_3000028[];
extern "C" uint8_t D_3000050[];
extern "C" uint8_t D_3000088[];
extern "C" uint8_t D_30000C0[];
extern "C" uint8_t D_3000100[];
extern "C" uint8_t D_3000138[];
extern "C" uint8_t D_3000170[];
extern "C" uint8_t D_30001A8[];
extern "C" uint8_t D_3000270[];
extern "C" uint8_t D_30002E0[];
extern "C" uint8_t D_3000338[];
extern "C" uint8_t D_3000400[];
extern "C" uint8_t D_3000438[];
extern "C" uint8_t D_3000470[];
extern "C" uint8_t D_30004A8[];
extern "C" uint8_t D_30004E0[];
extern "C" uint8_t D_3000510[];
extern "C" uint8_t D_3000540[];
extern "C" uint8_t D_3000590[];
extern "C" uint8_t D_30005D8[];
extern "C" uint8_t D_3000688[];
extern "C" uint8_t D_30006D0[];
/* Course material setup DLs (segment 8 +0x14040 / +0x14078), read by the GDX_DIAG_SETUPDL probe. */
extern "C" uint8_t D_8014040[];
extern "C" uint8_t D_8014078[];
/* Tall-building texture window (segment 8 +0x14A20), overwritten per venue by
   gdx_load_venue_building_texture. */
extern "C" uint16_t D_8014A20[];
/* course_track_gfx base (segment 8). Warmed at boot because its MIO0 decode measured 133.95ms
   in a single hit -- the largest asset stall in the port. */
extern "C" uint8_t D_8000000[];
extern "C" uint8_t aVpFullScreen[];
extern "C" uint16_t D_A000000_235130[];
extern "C" uint16_t D_A000000_239A80[];
extern "C" uint16_t D_A000000_23EC50[];
extern "C" uint16_t D_A000000_243D90[];
extern "C" uint16_t D_A000000_24A270[];
extern "C" uint16_t D_A000000_2507F0[];
extern "C" uint16_t D_A000000_255100[];
extern "C" uint16_t D_A000000_259600[];
extern "C" uint16_t D_A000000_25F360[];
extern "C" uint16_t D_A000000_266C20[];
extern "C" uint16_t D_A000000_26D780[];
extern "C" uint8_t D_2000000[];
extern "C" uint8_t D_80225800[];
extern "C" uint8_t D_1000000[];
/* The two live GfxPools (decomp: GfxPool D_8024DCE0[2], unk_gfx_segment.c:194), addressed as
   raw bytes. IsGfxPoolHostRange() needs this so the persistent texture-copy cache knows the
   range is REWRITTEN EVERY FRAME rather than ROM-stable. */
extern "C" uint8_t D_8024DCE0[];
/* Unsuffixed venue texture bank symbols (segment 0x0A, 0x1000-byte banks). course.c's road
   material table references them directly (ROAD_1..WALLED_ROAD), but they are 1-byte LinkStubs
   — per-venue data loads via the suffixed symbols (D_A000000_235130 etc.) into gSegments[0x0A]. */
extern "C" uint8_t D_A000000[];
extern "C" uint8_t D_A001000[];
extern "C" uint8_t D_A002000[];
extern "C" uint8_t D_A003000[];
extern "C" uint8_t D_A004000[];
extern "C" uint8_t D_A005000[];
extern "C" uint8_t D_A006000[];
extern "C" uint8_t D_A007000[];
extern "C" uint8_t D_A008000[];
// Banks 9-10 (decomp fzx_segmentA.h:15-17) have no current reference, so a log-driven regen of
// LinkStubs.c could never add them; they are listed in gen_link_stubs.py's EXTRA_DATA_SYMS to
// force an always-linked stub and keep the bank table symmetric.
extern "C" uint8_t D_A009000[];
extern "C" uint8_t D_A00A000[];
#ifdef EXPANSION_KIT
// D_A00B000..D_A00BD80 are the Road-Type panel's per-venue preview icons (RGBA16 24x12 at
// segment 0x0A +0xB000..+0xBD80, immediately past the 11 main banks), referenced by
// expansion_kit/A3AE0.c:533-568. That overlay directory is excluded from non-EK builds and the
// stubs live only in port/gen/EkLinkStubs.c, so the symbols do not exist to link against
// outside an EXPANSION_KIT build.
extern "C" uint8_t D_A00B000[];
extern "C" uint8_t D_A00B240[];
extern "C" uint8_t D_A00B480[];
extern "C" uint8_t D_A00B6C0[];
extern "C" uint8_t D_A00B900[];
extern "C" uint8_t D_A00BB40[];
extern "C" uint8_t D_A00BD80[];
#endif  // EXPANSION_KIT
extern "C" uint8_t gspF3DEX2_fifoTextStart[];
extern "C" uint8_t gspF3DFLX2_Rej_fifoTextStart[];
extern "C" uint8_t gspF3DLX2_Rej_fifoTextStart[];
extern "C" unsigned long long gspF3DEX2_Rej_fifoTextStart[];
extern "C" unsigned long long gspL3DEX2_fifoTextStart[];

extern "C" int gdx_lookup_asset_segment(unsigned int sym_low32,
                                         unsigned char* segment,
                                         unsigned int* rom_base,
                                         unsigned char* compressed,
                                         unsigned int* offset,
                                         unsigned int* image_size);
extern "C" int gdx_lookup_asset_segment_interior(unsigned int sym_low32,
                                                  unsigned char* segment,
                                                  unsigned int* rom_base,
                                                  unsigned char* compressed,
                                                  unsigned int* offset,
                                                  unsigned int* image_size);
extern "C" void gdx_fixup_asset_segment_image(unsigned char segment,
                                               unsigned int rom_base,
                                               unsigned char* data,
                                               unsigned int size);
extern "C" void gdx_register_asset_segment_command_ranges(unsigned char segment,
                                                            unsigned int rom_base,
                                                            unsigned char* data,
                                                            unsigned int size);
extern "C" int gdx_resolve_mode_segment9(unsigned int raw, size_t requiredBytes,
                                           uintptr_t* outAddress);
// True when segment `seg` (4, 7 or 9) is currently owned by a live mode carve; false otherwise.
extern "C" int gdx_mode_owns_segment(unsigned int seg);
// True when `rom_base` matches the ROM family currently resident for mode-owned segment `seg`.
// Gates the live-carve redirect below so a stale-family AssetBindings.c row -- different content
// sharing the same segment number -- is not served the wrong carve's bytes.
extern "C" int gdx_mode_segment_content_matches(unsigned int seg, unsigned int rom_base);
extern "C" const char* GDiffuser_LookupLoadedAssetKey(const void* buffer, size_t minSize, int requireUnmodified);
extern "C" const char* GDiffuser_LookupLoadedAssetKeyAndSize(const void* buffer, size_t* outSize);
extern "C" const char* gdx_lookup_asset_segment_o2r_key(unsigned int sym_low32);
// Workshop texture packs (port/gdx_workshop.cpp).
extern "C" int gdx_workshop_texture_packs_enabled(void);
extern "C" const char* GdxWorkshopLookupOverridePath(const char* key);
extern "C" const char* GdxWorkshopLookupOverridePathMinSize(const char* key, size_t minBytes);

namespace {

// The decomp builds N64 display-list packets as two 32-bit words. libultraship's Fast3D
// interpreter reads packets as two uintptr_t words on 64-bit hosts. Never cast the decomp
// command stream directly to LUS Gfx*; expand it first.
struct N64Gfx {
    uint32_t w0;
    uint32_t w1;
};

static_assert(sizeof(N64Gfx) == 8, "N64 display-list packets must stay 8 bytes");

constexpr size_t kN64GfxStride = sizeof(N64Gfx);
// Host-built decomp Gfx packets can be wider than the original 8-byte N64
// packet because some union members carry host pointers on 64-bit builds.
// Read host-built lists with the host stride; RDRAM/ROM decoded lists stay 8-byte.
constexpr size_t kHostBuiltGfxStride = (sizeof(uintptr_t) > 4) ? 16 : kN64GfxStride;

static inline uint32_t Byteswap32(uint32_t x);

N64Gfx ReadRawCommand(const N64Gfx* source, size_t index, size_t stride) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(source) + (index * stride);
    N64Gfx command = {};
    std::memcpy(&command, bytes, sizeof(command));
    return command;
}

N64Gfx ReadCommand(const N64Gfx* source, size_t index, size_t stride, bool isBig) {
    N64Gfx command = ReadRawCommand(source, index, stride);
    if (isBig) {
        command.w0 = Byteswap32(command.w0);
        command.w1 = Byteswap32(command.w1);
    }
    return command;
}

static inline uint32_t Byteswap32(uint32_t x) {
    return ((x & 0xFF000000) >> 24) |
           ((x & 0x00FF0000) >> 8)  |
           ((x & 0x0000FF00) << 8)  |
           ((x & 0x000000FF) << 24);
}

static inline uint16_t Byteswap16(uint16_t x) {
    return ((x & 0xFF00) >> 8) | ((x & 0x00FF) << 8);
}

bool IsLikelyDisplayListOpcode(uint8_t op); // defined below

static inline bool IsLikelyBigEndianDisplayList(const N64Gfx* source, size_t readableLimit) {
    if (readableLimit == 0) return false;
    uint32_t w0 = source[0].w0;
    uint8_t opL = w0 >> 24;
    uint8_t opB = w0 & 0xFF;
    if (opL == 0 && opB != 0) return true;
    if ((opB >= 0xB0 || opB == 0x01 || opB == 0x04) && opL < 0x20) return true;
    /* The first word alone is ambiguous when the BE command carries nonzero operand bytes:
       gSPGeometryMode D9 FD FF FF reads as LE 0xFFFFFDD9 whose top byte 0xFF is also a
       plausible opcode, which classified every EK disk-filled UI list as LE. Walk the list as
       big-endian instead: a genuine BE list yields a valid opcode chain reaching G_ENDDL
       inside the readable window. */
    {
        const size_t scan = (readableLimit < 64) ? readableLimit : 64;
        for (size_t i = 0; i < scan; i++) {
            const uint8_t op = static_cast<uint8_t>(source[i].w0 & 0xFFu);
            if (!IsLikelyDisplayListOpcode(op)) return false;
            if (op == 0xDF || op == 0xB8) return true; // G_ENDDL (EX2 / F3D)
        }
    }
    return false;
}

constexpr uint8_t kGfxSegmentCount = 16;
constexpr uint32_t kSegmentOffsetLimit = 0x01000000;
constexpr size_t kMaxUnboundedDisplayListCommands = 1 << 20;

constexpr uint8_t kOpVtx = 0x01;
constexpr uint8_t kOpBranchZ = 0x04;
// G_BRANCH_Z in original F3D (used by F-Zero X race DLs): G_IMMFIRST=0xE5, so 0xE5-15 = 0xD6.
// F3DEX2 encodes the same command as 0x04 (kOpBranchZ above).
constexpr uint8_t kOpBranchZF3D = 0xD6;
constexpr uint8_t kOpEndDl = 0xDF;
constexpr uint8_t kOpDl = 0xDE;
constexpr uint8_t kOpMovemem = 0xDC;
constexpr uint8_t kOpMoveword = 0xDB;
constexpr uint8_t kOpMtx = 0xDA;
constexpr uint8_t kOpRdpHalf1 = 0xE1;
constexpr uint8_t kOpLoadTlut = 0xF0;
constexpr uint8_t kOpSetTileSize = 0xF2;
constexpr uint8_t kOpLoadBlock = 0xF3;
constexpr uint8_t kOpLoadTile = 0xF4;
constexpr uint8_t kOpSetTile = 0xF5;
constexpr uint8_t kOpSetColorImage = 0xFF;
constexpr uint8_t kOpSetDepthImage = 0xFE;
constexpr uint8_t kOpSetTextureImage = 0xFD;
constexpr uint8_t kOpSetTextureImageOtrFilepath = 0x25;
constexpr uint8_t kOpDlOtrFilepath = 0x27;

constexpr uint8_t kMovewordSegmentIndex = 0x06;
constexpr size_t kDisplayListValidationCommandLimit = 1 << 16;
constexpr size_t kTextureLoadScanCommandLimit = 2048;
constexpr size_t kMinRawTextureCopyBytes = 8;
constexpr size_t kMaxRawTextureCopyBytes = 1 << 20;
constexpr uint32_t kTextureImageFrac = 2;
constexpr uintptr_t kSetupGfxRomOffset = 0x17B1E0;
constexpr size_t kSetupGfxSize = 0x778;

uint8_t Opcode(uint32_t w0) {
    return static_cast<uint8_t>(w0 >> 24);
}

uint8_t WordParam(uint32_t w0) {
    return static_cast<uint8_t>((w0 >> 16) & 0xFF);
}

bool IsLikelyDisplayListOpcode(uint8_t op) {
    if (op <= 0x09) return true;
    if ((op >= 0x20) && (op <= 0x49)) return true;
    if ((op >= 0xB0) && (op <= 0xBF)) return true;  // F3D range: G_TRI1, G_ENDDL, G_TEXTURE, ...
    if ((op >= 0xC8) && (op <= 0xCF)) return true;
    if ((op >= 0xD3) && (op <= 0xE3)) return true;
    if ((op >= 0xE4) && (op <= 0xFF)) return true;
    return false;
}

Fast::F3DGfx MakeLusGfx(uintptr_t w0, uintptr_t w1) {
    Fast::F3DGfx gfx = {};
    gfx.words.w0 = w0;
    gfx.words.w1 = w1;
    return gfx;
}

struct ResolvedAddress {
    uintptr_t full = 0;
    uint8_t segment = 0;
    uint32_t offset = 0;
    bool segmented = false;
};

// [venueload] diagnostic, strip later: ticks still owed a post-venue-load translation-cost line.
static int gGdxVenueWatchTicks = 0;

// Scheduler yield counter (n64_sched.c). Each yield returns to the host fiber, which pumps a whole
// frame before re-dispatching, so one yield inside a load costs a full ~16.7ms tick of wall clock
// irrespective of the load's own work. Used to tell real decode cost apart from yield latency.
extern "C" unsigned long gdx_yield_count;

struct ConversionStats {
    std::array<size_t, 256> opCounts{};
    size_t convertedLists = 0;
    size_t noopDisplayLists = 0;
    size_t fallbackDataCommands = 0;
    size_t skippedDataCommands = 0;
    size_t skippedTextures = 0;
    /* Resolutions rejected because a game-thread segment reload (gGdxSegmentEpoch) raced them.
       Separate from skippedTextures/skippedDataCommands so a mode-transition reload storm stays
       distinguishable from genuine resolution failures. */
    size_t skippedEpochRetries = 0;
    size_t textureCopies = 0;
    size_t textureCopyBytes = 0;
    size_t commandsOut = 0;
    size_t f3dLists = 0;
    size_t ucodeSwitches = 0;
    size_t unknownUcodeSwitches = 0;
    uint32_t firstUnknownUcodeRaw = 0;
    /* Deliberate L3DEX2 (line ucode) skips, counted separately from unknownUcodeSwitches so the
       benign per-menu-frame skip stays distinguishable from a genuinely unmatched G_LOAD_UCODE
       in the [gfxdiag] line. */
    size_t l3dexUcodeSkips = 0;
    uint32_t firstL3dexUcodeRaw = 0;
    uint8_t firstFallbackDataOp = 0;
    uint32_t firstFallbackDataRaw = 0;
    uint32_t firstFallbackDataW0 = 0;
    uintptr_t firstFallbackDataSource = 0;
    size_t firstFallbackDataIndex = 0;
    uint8_t firstSkippedDataOp = 0;
    uint32_t firstSkippedDataRaw = 0;
    uint32_t firstSkippedDataW0 = 0;
    uint32_t firstNoopDlRaw = 0;
    size_t missingDisplayLists = 0;
    size_t badDisplayLists = 0;
    uint32_t firstMissingDlRaw = 0;
    uint32_t firstBadDlRaw = 0;
    uintptr_t firstMissingParent = 0;
    size_t firstMissingParentIndex = 0;
    size_t firstMissingParentStride = 0;
    bool firstMissingParentBigEndian = false;
    bool firstMissingParentF3D = false;
    uint32_t firstMissingParentRawW0 = 0;
    uint32_t firstMissingParentRawW1 = 0;
    uint32_t firstMissingParentDecodedW0 = 0;
    uint32_t firstMissingParentDecodedW1 = 0;
    uintptr_t firstBadDlTarget = 0;
    size_t firstBadDlLimit = 0;
    size_t firstBadDlStride = 0;
    bool firstBadDlBigEndian = false;
    bool firstBadDlF3D = false;
    uint32_t firstBadDlFirstW0 = 0;
    uint32_t firstBadDlFirstW1 = 0;
    size_t firstBadDlFailureIndex = 0;
    uint8_t firstBadDlFailureOpcode = 0;
    uint8_t firstBadDlFailureReason = 0; // 1=zero limit, 2=invalid opcode, 3=no terminator

    /* Every distinct resolved G_SETCOLORIMAGE host address in this task's list (deduped,
       capped). A task frequently redirects CIMG to an offscreen framebuffer mid-task (the
       SETCIMG "canvas" idiom in texture_utils.c's func_8007AB88/func_8007ABA4, and
       OBJECT_FRAMEBUFFER) and restores it before the task ends, so mirroring only the FINAL
       color image leaves every mid-task-only target permanently invalid. */
    std::array<uintptr_t, 8> colorImageTargets{};
    size_t colorImageTargetCount = 0;
};

struct HostRange {
    uintptr_t begin = 0;
    size_t size = 0;
};

struct N64AddressRange {
    uint32_t n64Begin = 0;
    uintptr_t hostBegin = 0;
    size_t size = 0;
};

struct PersistentRawTextureCopy {
    uintptr_t source = 0;
    size_t size = 0;
    std::unique_ptr<uint8_t[]> bytes;
    uint64_t dmaGenAtCopy = UINT64_MAX;
};

struct N64FramebufferInfo {
    uintptr_t address = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid = false;
};

std::vector<HostRange> gHostRanges;
std::vector<HostRange> gRawN64Ranges;
std::vector<HostRange> gHostN64CommandRanges;
std::vector<HostRange> gHostWideCommandRanges;
std::vector<N64AddressRange> gN64AddressRanges;
// Host-pointer identity registration for compiled-in host arrays whose address SETTIMG can
// carry directly (EK disk assets from EkAssetBindings.c, plus base-game arrays registered from
// decomp_port.c). HostRange is reused with begin = the array's own HOST address, not an N64
// address; ResolveHostPointerStub's interior delta<size match is otherwise identical in shape
// to the other host-range tables here.
std::vector<HostRange> gHostPointerStubs;
std::vector<HostRange> gF3DAssetRanges;
std::vector<HostRange> gNativeRgba16Ranges;
std::vector<uintptr_t> gPendingNativeRgba16RangeClears;
std::vector<uint8_t> gSetupGfxSegment;
std::vector<PersistentRawTextureCopy> gRawTextureCopies;
std::vector<uintptr_t> gPendingTextureCacheDeletes;
std::vector<std::unique_ptr<uint8_t[]>> gPersistentAllocations;
std::vector<N64FramebufferInfo> gN64Framebuffers;
uintptr_t gViCurrentFramebuffer = 0;
uintptr_t gViNextFramebuffer = 0;
uintptr_t gLastRenderedFramebuffer = 0;

/* Title->menu wipe-band probe, strip later. Zero size means no active transition capture, so
 * the probe cannot fire outside a transition. */
uintptr_t gDiagTransitionCaptureBegin = 0;
size_t gDiagTransitionCaptureSize = 0;

// Bumped whenever an asset/ROM-backed segment image is (re)decoded, to invalidate converted
// lists built against the old image. Declared ahead of EnsureAssetSegmentImage; the rest of the
// converter state lives below, after the resolver helpers it needs.
uint32_t gConvertEpoch = 1;

// Set by gdx_gfx_run() whenever a real GFX task renders into the current host
// frame; checked + cleared once per frame by gdx_vi_present_fallback(). When it
// is false at present time, no task produced this frame (boot-logo phase or any
// other CPU-drawn screen) and the fallback must scan out the VI framebuffer.
bool gHostFrameGfxTaskRan = false;

// Once a real GFX task has ever presented, a taskless host frame must HOLD the last GPU image
// (as the N64 VI re-scans the already-rendered RDRAM buffer) rather than blit the CPU-side VI
// framebuffer, which is empty for GPU-rendered screens: Cup Select's slide-up rendered 1 present
// in 3 and the other 2 flashed full-screen black.
static bool sGpuContentLive = false;
// Diagnostic only (GdxDiagHoldTick): whether the frame mirror got fresh content since the last
// hold tick. It does NOT gate drawing — the hold path composites every tick regardless.
static bool sGpuHoldPixelsStale = true;

struct AssetSegmentLookup {
    uint8_t segment = 0;
    uint32_t romBase = 0;
    bool compressed = false;
    uint32_t offset = 0;
    uint32_t imageSize = 0;
};

struct LoadedAssetSegment {
    uint8_t segment = 0;
    uint32_t romBase = 0;
    bool compressed = false;
    std::vector<uint8_t> bytes;
};

std::vector<LoadedAssetSegment> gLoadedAssetSegments;

/* Segments whose display-list dialect is KNOWN, so DLs inside their decompressed images can
 * skip the per-DL opcode scan (which misclassifies F3DEX2 setup DLs whose byte stream happens
 * to contain 0xB8 before 0xDF). Unlisted segments keep the heuristic path.
 *
 * Segment 0x03 (machine_custom_gfx) must stay absent: it is not a single dialect. ROM ground
 * truth (baserom.us.rev0.z64, decompressed segment-3 image, offset 0x6D0) has a genuine F3DEX2
 * sub-list — G_VTX(0x01) + 3x G_TRI2(0x06) terminated by 0xDF — a few bytes before named
 * legacy-F3D machine-part DLs (e.g. D_3000780). Tagging the whole segment F3D pushes every
 * G_TRI2 there through the F3D-only "0x06 = G_DL" remap (the isF3DSource-gated case 0x06
 * below), feeding vertex-index pairs to TranslateDisplayListPointer as sub-DL pointers and
 * producing garbage wedges on custom machine bodies. */
enum class GdxSegmentUcode : uint8_t { Unknown = 0, F3D, F3DEX2 };

static GdxSegmentUcode GdxSegmentDialect(uint8_t segment) {
    /* Segment 0x08 (course_track_gfx) is F3DEX2 by word-level decode of D_80172A0: 0xD7
     * G_TEXTURE, 0x01 G_VTX, 0x05 G_TRI1 runs, 0xDF terminator. Do not re-tag it F3D — the
     * decoration artifacts that suggest F3D come from the fixup-region vertex-block swap
     * instead (see the sAssetFixups split in AssetBindings.c). */
    switch (segment) {
        case 0x08:
            return GdxSegmentUcode::F3DEX2;
        default:
            return GdxSegmentUcode::Unknown;
    }
}

GdxSegmentUcode GdxAssetPointerDialect(uintptr_t addr) {
    for (const LoadedAssetSegment& seg : gLoadedAssetSegments) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(seg.bytes.data());
        if ((base != 0) && (addr >= base) && (addr < base + seg.bytes.size())) {
            return GdxSegmentDialect(seg.segment);
        }
    }
    return GdxSegmentUcode::Unknown;
}

// Incremented on every DMA chunk loaded into RDRAM. RDRAM texture copies record
// the generation at copy time; a mismatch on the next frame triggers a re-upload.
// This avoids expensive per-frame memcmp against large raw texture copies.
static uint64_t gDmaGeneration = 0;
struct DmaDirtyRange {
    uintptr_t begin;
    uintptr_t end;
    uint64_t generation;
};
static std::vector<DmaDirtyRange> gDmaDirtyRanges;

void RecordHostWrite(uintptr_t begin, size_t size) {
    ++gDmaGeneration;
    if (begin == 0 || size == 0 || size > UINTPTR_MAX - begin) {
        return;
    }
    gDmaDirtyRanges.push_back({ begin, begin + size, gDmaGeneration });
    if (gDmaDirtyRanges.size() > 4096) {
        gDmaDirtyRanges.erase(gDmaDirtyRanges.begin(), gDmaDirtyRanges.begin() + 2048);
    }
}

} // namespace (temporarily close to define extern "C")

extern "C" void gdx_record_dma_load(uint32_t rdram_phys, uint32_t rom_offset, uint32_t size) {
    (void)rom_offset;
    ++gDmaGeneration;
    if (gdx_rdram != nullptr && size != 0 && rdram_phys < GDX_RDRAM_SIZE) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(gdx_rdram) + rdram_phys;
        const uintptr_t end = begin + std::min<size_t>(size, GDX_RDRAM_SIZE - rdram_phys);
        gDmaDirtyRanges.push_back({begin, end, gDmaGeneration});
        if (gDmaDirtyRanges.size() > 4096) {
            gDmaDirtyRanges.erase(gDmaDirtyRanges.begin(), gDmaDirtyRanges.begin() + 2048);
        }
    }
}

namespace {

bool HostRangeChanged(uintptr_t source, size_t size, uint64_t sinceGeneration) {
    if (sinceGeneration == gDmaGeneration) {
        return false;
    }
    if (!gDmaDirtyRanges.empty() && sinceGeneration < gDmaDirtyRanges.front().generation) {
        return true;
    }

    const uintptr_t end = source + size;
    for (auto it = gDmaDirtyRanges.rbegin(); it != gDmaDirtyRanges.rend(); ++it) {
        if (it->generation <= sinceGeneration) {
            break;
        }
        if (source < it->end && end > it->begin) {
            return true;
        }
    }
    return false;
}

bool IsN64FramebufferRange(uintptr_t source, size_t size) {
    if (size > UINTPTR_MAX - source) {
        return false;
    }
    const uintptr_t end = source + size;
    for (const N64FramebufferInfo& framebuffer : gN64Framebuffers) {
        const size_t framebufferSize =
            static_cast<size_t>(framebuffer.width) * framebuffer.height * sizeof(uint16_t);
        if (framebufferSize <= UINTPTR_MAX - framebuffer.address &&
            source >= framebuffer.address && end <= framebuffer.address + framebufferSize) {
            return true;
        }
    }
    return false;
}

bool IsNativeRgba16Range(uintptr_t source, size_t size) {
    for (const HostRange& range : gNativeRgba16Ranges) {
        if (source >= range.begin && source + size <= range.begin + range.size) {
            return true;
        }
    }
    return false;
}

// Bytes from `source` to the end of the native-RGBA16 range containing it, 0 if none. A load-size
// estimate that rounds up past the registered image (the WIPE transition's single wide LOADBLOCK
// overshoots WIDTH*HEIGHT*2 by a row) would otherwise disable the byte swap for the whole copy;
// clamping to this extent keeps CopyRawTextureBytes swapping.
size_t NativeRgba16RangeRemaining(uintptr_t source) {
    size_t best = 0;
    for (const HostRange& range : gNativeRgba16Ranges) {
        if (source >= range.begin && source < range.begin + range.size) {
            const size_t remaining = (range.begin + range.size) - source;
            if (remaining > best) {
                best = remaining;
            }
        }
    }
    return best;
}

bool NativeRgba16CopyMatches(const uint8_t* copy, uintptr_t source, size_t size) {
    if (copy == nullptr || source == 0) {
        return false;
    }

    const auto* input = reinterpret_cast<const uint8_t*>(source);
    size_t i = 0;
    for (; i + 1 < size; i += 2) {
        if (copy[i] != input[i + 1] || copy[i + 1] != input[i]) {
            return false;
        }
    }
    return i >= size || copy[i] == input[i];
}

void CopyRawTextureBytes(uint8_t* destination, uintptr_t source, size_t size) {
    const auto* input = reinterpret_cast<const uint8_t*>(source);
    if (!IsNativeRgba16Range(source, size)) {
        std::memcpy(destination, input, size);
        return;
    }

    size_t i = 0;
    for (; i + 1 < size; i += 2) {
        destination[i] = input[i + 1];
        destination[i + 1] = input[i];
    }
    if (i < size) {
        destination[i] = input[i];
    }
}

// ---------------------------------------------------------------------------
// Quarantine for the pointer-GUESSING resolver branches.
//
// Game-emitted lists carry real 64-bit host pointers and the narrow->wide converter resolves
// every binary N64 list once deterministically (segment table + RDRAM-arena physical strip
// only), so the branches below -- low32-window match, KSEG0/physical/source-window high-32
// reconstruction, ambiguous cross-segment fallback, raw-buffer substitutions -- should only fire
// for stragglers: GDX_G2_CONVERT=0, a conversion miss, or a legacy F3D path the converter skips.
//
// The gate DEFAULTS OFF; a full boot->race->close session recorded zero hits on every branch.
// GDX_LEGACY_RESOLVE (or gDevTools.Behavior.LegacyResolve) restores it without a rebuild. The
// per-branch counters and capped [legacy-resolve] lines exist to keep that claim falsifiable.
//
// If these are ever deleted, do NOT delete with them: segment-table lookups (explicit-segment
// and encodedSegment paths), ResolvePortBssAlias / ResolveVenueBankAlias /
// ResolveGeneratedAssetStub / ResolveSetupGfxStub (exact symbol matches, not guesses), the
// D_1000000 special case, the EK gN64AddressRanges reverse scan, SETTIMG/SETCIMG/SETZIMG image
// op handling, BRANCH_Z/DMA_IO/RDPHALF_1, or the converter/cache/GDX_G2_CONVERT switch. The
// raw & 0x1FFFFFFF RDRAM strip inside the KSEG0 branch is deterministic, not a guess.
// ---------------------------------------------------------------------------
constexpr bool kGdxLegacyResolveDefaultEnabled = false;
enum class LegacyResolveBranch : uint8_t {
    kRegisteredHostLow32 = 0, // ResolveRegisteredHostPointer: low32-window match
    kKseg0High32,             // out-of-RDRAM KSEG0/KSEG1 high-32 reconstruction
    kPhysicalWindow,          // tryPhysicalWindow/tryAllPhysicalWindows
    kSourceWindow,            // trySourceWindow (referencing-DL-window guess)
    kCrossSegmentFallback,    // ambiguous cross-segment SegCandidate sort
    kModuleHigh32,            // mModuleBegin high-32 reconstruction
    kRawHigh32Scan,           // raw>=0x10000000 highCandidates scan
    kFallbackBuffer,          // FallbackDataPointer identity/viewport/vtx buffers
    kDirectCast,              // TranslateDataPointer raw-as-pointer last resort
    kCount,
};

inline const char* LegacyResolveBranchName(LegacyResolveBranch branch) {
    switch (branch) {
        case LegacyResolveBranch::kRegisteredHostLow32: return "reghost_low32";
        case LegacyResolveBranch::kKseg0High32: return "kseg0_high32";
        case LegacyResolveBranch::kPhysicalWindow: return "phys_window";
        case LegacyResolveBranch::kSourceWindow: return "src_window";
        case LegacyResolveBranch::kCrossSegmentFallback: return "cross_seg_fallback";
        case LegacyResolveBranch::kModuleHigh32: return "module_high32";
        case LegacyResolveBranch::kRawHigh32Scan: return "raw_high32_scan";
        case LegacyResolveBranch::kFallbackBuffer: return "fallback_buffer";
        case LegacyResolveBranch::kDirectCast: return "direct_cast";
        default: return "unknown";
    }
}

inline uint64_t (&LegacyResolveHitCounters())[static_cast<size_t>(LegacyResolveBranch::kCount)] {
    static uint64_t hits[static_cast<size_t>(LegacyResolveBranch::kCount)] = {};
    return hits;
}

// Set by ProcessList before each command so both member and free-function guessing branches can
// tag their [legacy-resolve] lines with the triggering opcode without threading a parameter
// through every call site. 0xFF = a guess fired outside the per-command loop.
uint8_t gLegacyResolveCurrentOp = 0xFFu;

// The gate is normalized so 0 == stock (guessing OFF), because Dev Tools compiles Bucket B gates
// out of a Release build by hard-wiring them to 0. If kGdxLegacyResolveDefaultEnabled is ever
// flipped back to true this short-circuits and the gate can no longer turn guessing OFF —
// revisit it together with that flip.
inline bool LegacyResolveEnabled() {
    return kGdxLegacyResolveDefaultEnabled || gdx_dev_gate(GDX_GATE_LEGACY_RESOLVE) != 0;
}

// Called only from a guessing branch's SUCCESS path, so a branch with hits==0 across a full run
// genuinely never contributed a resolution.
inline void RecordLegacyResolveHit(LegacyResolveBranch branch, uint32_t raw, uint8_t op) {
    static bool sAnySeen = false;
    uint64_t& hits = LegacyResolveHitCounters()[static_cast<size_t>(branch)];
    ++hits;
    if (!sAnySeen) {
        sAnySeen = true;
        gdx_port_logf("[legacy-resolve] SUMMARY: first legacy-resolve hit this run (branch=%s) -- "
                      "soak is NOT clean yet. GDX_LEGACY_RESOLVE=%d\n",
                      LegacyResolveBranchName(branch), LegacyResolveEnabled() ? 1 : 0);
    }
    if (hits <= 8) {
        gdx_port_logf("[legacy-resolve] branch=%s hits=%llu raw=%08X op=%02X\n",
                      LegacyResolveBranchName(branch), static_cast<unsigned long long>(hits), raw, op);
    }
}

alignas(8) const int32_t kFallbackIdentityMtx[16] = {
    0x00010000, 0x00000000,
    0x00000001, 0x00000000,
    0x00000000, 0x00010000,
    0x00000000, 0x00000001,
    0, 0, 0, 0, 0, 0, 0, 0,
};

alignas(8) const int16_t kFallbackViewport[8] = {
    640, 480, 0x03FF, 0,
    640, 480, 0, 0,
};

/* Zeroed stand-in substituted for an unreadable vertex pointer (G_VTX crash failsafe in
 * ProcessList). It MUST stay sized for the full 8-bit F3DEX2 vertex count: the failsafe swaps
 * only the POINTER, not the COUNT the interpreter re-reads from the command word (C0(12,8)),
 * so a desynced command with count=0xF0 walks 3840 bytes and faults inside GfxSpVertex if this
 * buffer is smaller. 256 entries * sizeof(F3DVtx)(16B). */
alignas(8) const uint8_t kFallbackVertices[256 * 16] = {};

uint32_t Low32(uintptr_t value) {
    return static_cast<uint32_t>(value);
}

/* PE preferred image base. ASLR randomizes the runtime module base every launch, so a logged
   runtime pointer cannot be looked up in G-Diffuser.map directly; emitting
   (kPreferredImageBaseVA + moduleOffset) makes any diagnostic pointer map-resolvable regardless
   of the run's base. uint64_t so the arithmetic stays valid on a 32-bit host build. */
constexpr uint64_t kPreferredImageBaseVA = 0x140000000ULL;

uint32_t ReadBE32(const uint8_t* src) {
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

bool LookupAssetSegment(uint32_t raw, AssetSegmentLookup& out) {
    unsigned char segment = 0;
    unsigned int romBase = 0;
    unsigned char compressed = 0;
    unsigned int offset = 0;
    unsigned int imageSize = 0;

    if (gdx_lookup_asset_segment(raw, &segment, &romBase, &compressed, &offset, &imageSize) == 0 &&
        gdx_lookup_asset_segment_interior(raw, &segment, &romBase, &compressed, &offset, &imageSize) == 0) {
        return false;
    }

    out.segment = segment;
    out.romBase = romBase;
    out.compressed = compressed != 0;
    out.offset = offset;
    out.imageSize = imageSize;
    return true;
}

/* True when a pointer's low32 is a generated ASSET PLACEHOLDER symbol. These 1-byte BSS stubs
 * (e.g. setup_gfx's D_3000050) stand in for data loaded into a runtime segment; the real
 * DL/vertex bytes live in the loaded segment image, NOT at the stub's own address, so such a
 * pointer must go through TryResolveAddress -> ResolveGeneratedAssetStub and never be used
 * verbatim. ProcessList uses this so a wide packet carrying a placeholder is not mistaken for a
 * real host pointer just because its high32 is set. */
bool IsAssetPlaceholderPointer(uint32_t low32) {
    AssetSegmentLookup scratch = {};
    return LookupAssetSegment(low32, scratch);
}

uintptr_t EnsureAssetSegmentImage(const AssetSegmentLookup& lookup) {
    for (LoadedAssetSegment& loaded : gLoadedAssetSegments) {
        if ((loaded.segment == lookup.segment) &&
            (loaded.romBase == lookup.romBase) &&
            (loaded.compressed == lookup.compressed) &&
            !loaded.bytes.empty()) {
            /* Claim the slot only when unowned. Reassigning on every cache hit lets a stray
               pointer matching another venue's texture symbol hijack segment 0x0A mid-race, and
               the road then renders with the wrong venue's texture; gdx_load_venue_texture_segment
               stays the authority for 0x0A and assigns it unconditionally.

               Deliberately NOT epoch-bracketed. EnsureAssetSegmentImage runs on BOTH the game
               thread and the graphics thread, but gdx_segment_epoch_begin/end is a SINGLE-WRITER
               seqlock owned by the game thread -- a graphics-thread bracket would race the
               fetch_add and corrupt the odd/even parity for every reader. Safe unbracketed
               because the ==0 test makes this a one-way 0 -> valid immutable pointer transition,
               the store is a single aligned uintptr_t, and the buffer was fully built first: a
               racing reader sees either 0 (unresolved -> graceful hard-skip) or the published
               pointer. The authoritative 0x0A rewrite in gdx_load_venue_texture_segment IS
               bracketed. */
            if (gSegments[lookup.segment] == 0) {
                gSegments[lookup.segment] = reinterpret_cast<uintptr_t>(loaded.bytes.data());
            }
            return reinterpret_cast<uintptr_t>(loaded.bytes.data());
        }
    }

    // Byte content comes from the shim only; gdx_rom_buffer/gdx_rom_size appear in this function
    // for bounds arithmetic, never as a content read. Gating on gdx_rom_buffer != NULL before the
    // shim runs would kill every asset segment on an archive-only boot (verified by deleting the
    // ROM after setup) even though the segment blobs fully cover them: the ROM is required only
    // when NO archive blob contains this family.
    uint32_t blobSpan = 0;
    const bool haveBlobSpan = GdxSegmentSourceContainingSpan(lookup.romBase, &blobSpan) != 0;
    const bool romUsable = (gdx_rom_buffer != nullptr) && (lookup.romBase < gdx_rom_size);
    if (!haveBlobSpan && !romUsable) {
        return 0;
    }

    LoadedAssetSegment loaded = {};
    loaded.segment = lookup.segment;
    loaded.romBase = lookup.romBase;
    loaded.compressed = lookup.compressed;

    // MIO0_HEADER_LENGTH covers both the magic sniff and the BE32 decoded-size field. A short
    // read (family with fewer than 16 source bytes, or no source) leaves havePeek false and the
    // plain path below handles it.
    uint8_t peek[MIO0_HEADER_LENGTH];
    const bool havePeek = GdxSegmentSourceRead(lookup.romBase, MIO0_HEADER_LENGTH, peek) != 0;
    const bool isMio0 = havePeek && (std::memcmp(peek, "MIO0", 4) == 0);

    // Stage the whole MIO0 stream through the shim, then decode from the staged copy. The
    // compressed length is not in the header, so size the stage to the family's archive-blob span;
    // with no blob, bound it by the MIO0 header (the uncompressed-section end is the last input
    // the decoder reads) capped to the ROM image. The stage must stay a per-call heap buffer, not
    // a shared static: this function runs on both the game and graphics threads.
    // [venueload] diagnostic, strip later: read/decode window, sampled alongside the scheduler
    // yield counter so a full ~16.7ms-per-yield stall is not misread as decode cost.
    const auto gdxDecodeT0 = std::chrono::steady_clock::now();
    const unsigned long gdxYieldsBefore = gdx_yield_count;

    auto stageAndDecodeMio0 = [&]() -> bool {
        uint32_t span = 0;
        size_t stageSize;
        if (GdxSegmentSourceContainingSpan(lookup.romBase, &span) &&
            span >= MIO0_HEADER_LENGTH) {
            stageSize = span; // exact archived family span -> stays contained
        } else {
            const uint32_t decSize = ReadBE32(peek + 4);   // dest_size
            const uint32_t uncompOff = ReadBE32(peek + 12); // uncomp_offset
            const size_t bound = static_cast<size_t>(uncompOff) + static_cast<size_t>(decSize);
            // No containing blob (or a degenerate one below header size): the raw
            // ROM is the only source left. Archive-only with no usable ROM yields
            // avail=0 -> stageSize < header -> clean failure below.
            const size_t avail = romUsable ? (gdx_rom_size - lookup.romBase) : 0;
            stageSize = std::min<size_t>(bound, avail);
        }
        if (stageSize < MIO0_HEADER_LENGTH) {
            return false;
        }
        std::vector<uint8_t> stage(stageSize);
        if (!GdxSegmentSourceRead(lookup.romBase, static_cast<uint32_t>(stageSize), stage.data())) {
            return false;
        }
        const uint32_t decodedSize = ReadBE32(stage.data() + 4);
        const size_t outputSize = std::max<size_t>(decodedSize, lookup.imageSize);
        if (outputSize == 0) {
            return false;
        }
        loaded.bytes.resize(outputSize);
        std::memset(loaded.bytes.data(), 0, loaded.bytes.size());
        const int decoded = mio0_decode(stage.data(), loaded.bytes.data(), nullptr);
        return decoded > 0;
    };

    if (lookup.compressed) {
        if (!isMio0 || !stageAndDecodeMio0()) {
            return 0;
        }
    } else if (isMio0) {
        gdx_port_logf("[segload] MIO0-autodetect seg=%u romBase=%08X (binding said uncompressed)\n",
                      lookup.segment, lookup.romBase);
        // Some segments (notably per-venue texture segments like Mute City's D_A000000_235130)
        // are MIO0-compressed in ROM while the asset bindings mark them uncompressed; copying the
        // raw stream as texture data is what renders the "track stripes". Trust the magic over the
        // flag. loaded.compressed keeps lookup.compressed so the cache key still matches future
        // lookups — the cached bytes are already decompressed.
        if (!stageAndDecodeMio0()) {
            return 0;
        }
    } else {
        // Sizing bound: the ROM tail when a ROM is present, else the containing blob's span. The
        // gate above guarantees at least one of the two exists.
        const size_t available = romUsable ? (gdx_rom_size - lookup.romBase)
                                           : static_cast<size_t>(blobSpan);
        // The generator guarantees every blob covers its segments' declared image sizes, but
        // nothing at runtime enforces it, and downstream consumers trust imageSize as the logical
        // extent. Log loudly if a regenerated table breaks that; min() below still bounds.
        if (!romUsable && lookup.imageSize != 0 && available < lookup.imageSize) {
            gdx_port_logf("[segload] WARNING: blob span 0x%zX < declared imageSize 0x%zX for "
                          "seg=%u romBase=%08X (archive-only) -- generated blob table may be "
                          "out of sync with the segment map\n",
                          available, static_cast<size_t>(lookup.imageSize),
                          lookup.segment, lookup.romBase);
        }
        const size_t allocSize = lookup.imageSize != 0
            ? std::min<size_t>(lookup.imageSize, available)
            : std::min<size_t>(available, 8 * 1024 * 1024);
        if (allocSize == 0) {
            return 0;
        }

        loaded.bytes.resize(allocSize);
        std::memset(loaded.bytes.data(), 0, loaded.bytes.size());
        // Uncompressed families read straight into the destination; no staging needed.
        if (!GdxSegmentSourceRead(lookup.romBase, static_cast<uint32_t>(allocSize),
                                  loaded.bytes.data())) {
            return 0;
        }
    }

    // [venueload] diagnostic, strip later: fixup and range-registration timed apart, since
    // boot-preloading the archive blobs left venue loads at 20-26 ms and the cost is below here.
    const auto gdxFixT0 = std::chrono::steady_clock::now();
    double gdxFixupMs = 0.0;
    double gdxRangesMs = 0.0;
    if (!loaded.bytes.empty()) {
        gdx_fixup_asset_segment_image(lookup.segment,
                                      lookup.romBase,
                                      loaded.bytes.data(),
                                      static_cast<unsigned int>(std::min<size_t>(loaded.bytes.size(), UINT32_MAX)));
        const auto gdxFixT1 = std::chrono::steady_clock::now();
        gdxFixupMs = std::chrono::duration<double, std::milli>(gdxFixT1 - gdxFixT0).count();
        gdx_register_asset_segment_command_ranges(
            lookup.segment,
            lookup.romBase,
            loaded.bytes.data(),
            static_cast<unsigned int>(std::min<size_t>(loaded.bytes.size(), UINT32_MAX)));
        gdxRangesMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gdxFixT1).count();
    }
    const double gdxDecodeMs = std::chrono::duration<double, std::milli>(gdxFixT0 - gdxDecodeT0).count();
    if ((gdxDecodeMs + gdxFixupMs + gdxRangesMs) > 1.0) {
        gdx_port_logf("[venueload] seg=%u bytes=%zu decode=%.2fms yields=%lu fixup=%.2fms ranges=%.2fms "
                      "hostRanges=%zu\n",
                      (unsigned) lookup.segment, loaded.bytes.size(), gdxDecodeMs,
                      gdx_yield_count - gdxYieldsBefore, gdxFixupMs, gdxRangesMs, gHostRanges.size());
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(loaded.bytes.data());
    /* First-claim store, deliberately NOT epoch-bracketed -- same argument as the cache-hit
       claim above: bracketing here would corrupt the game-thread-only seqlock parity when the
       graphics thread reaches it. */
    if (gSegments[lookup.segment] == 0) {
        gSegments[lookup.segment] = base;
    }
    gHostRanges.push_back({ base, loaded.bytes.size() });
    gRawN64Ranges.push_back({ base, loaded.bytes.size() });
    gLoadedAssetSegments.emplace_back(std::move(loaded));
    // A freshly decoded image may rebind a segment or move the targets converted lists resolved
    // against, so every cached wide conversion in the asset/ROM stamp space must be rebuilt.
    ++gConvertEpoch;
    return base;
}

uintptr_t EnsureAssetSegmentForSymbol(uint32_t symbolLow32, uint32_t* outOffset = nullptr) {
    AssetSegmentLookup lookup = {};
    if (!LookupAssetSegment(symbolLow32, lookup)) {
        return 0;
    }

    const uintptr_t base = EnsureAssetSegmentImage(lookup);
    if (base == 0) {
        return 0;
    }

    if (outOffset != nullptr) {
        *outOffset = lookup.offset;
    }
    return base;
}

uintptr_t FallbackDataPointer(uint8_t op, uint32_t raw = 0) {
    // Last-resort guess: static identity matrix / default viewport / zeroed vertices when nothing
    // resolved. Quarantined behind GDX_LEGACY_RESOLVE like every other guess.
    if (!LegacyResolveEnabled()) {
        return 0;
    }
    uintptr_t result = 0;
    switch (op) {
        case kOpMtx:
            result = reinterpret_cast<uintptr_t>(kFallbackIdentityMtx);
            break;
        case kOpMovemem:
            result = reinterpret_cast<uintptr_t>(kFallbackViewport);
            break;
        case kOpVtx:
            result = reinterpret_cast<uintptr_t>(kFallbackVertices);
            break;
        default:
            return 0;
    }
    RecordLegacyResolveHit(LegacyResolveBranch::kFallbackBuffer, raw, op);
    return result;
}

bool IsReadableAddress(uintptr_t address);

uintptr_t NormalizeLusDirectPointer(uintptr_t pointer) {
    /* libultraship's SegAddr() treats bit 0 as a segmented-address sentinel.
       Host allocations can legitimately have odd low32 values after N64 pointer
       reconstruction, but the actual data these commands read is aligned. Clear
       the sentinel bit when the aligned address is still readable. */
    if ((pointer & 1u) == 0) {
        return pointer;
    }

    const uintptr_t aligned = pointer & ~static_cast<uintptr_t>(1);
    return IsReadableAddress(aligned) ? aligned : pointer;
}

size_t RegisteredHostRemaining(uintptr_t full_addr) {
    for (const HostRange& range : gHostRanges) {
        if ((range.begin == 0) || (range.size == 0)) {
            continue;
        }
        if ((full_addr >= range.begin) && (full_addr < range.begin + range.size)) {
            return range.size - static_cast<size_t>(full_addr - range.begin);
        }
    }
    return 0;
}

bool IsRdramHostPointer(uintptr_t full_addr) {
    if (gdx_rdram == nullptr) {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(gdx_rdram);
    return (full_addr >= base) && (full_addr < base + GDX_RDRAM_SIZE);
}

/* True when a pointer's low32 falls on a PORT BSS ALIAS: a dummy host definition standing in for
 * N64 storage whose LIVE bytes are elsewhere. Two exist:
 *  - D_1000000 (GfxPool dummy in decomp_port.c). Game code WRITES matrices through gGfxPool->
 *    (the live double-buffered pool segment 1 points at) but emits DL references as
 *    &D_1000000.member (racer.c gSPMatrix modelviews, camera.c:3415 projection). A wide packet
 *    carries the dummy's real host address, so taking it verbatim reads the never-written dummy:
 *    zero projection/modelview, every 3D triangle collapses while 2D texrects survive.
 *  - D_2000000 (segment-2 BSS base, 1-byte LinkStubs token). Live storage is D_80225800 via
 *    ResolvePortBssAlias; verbatim it is also misaligned, so kOpMtx zeroed it and every frame
 *    fell back to the identity matrix.
 * Both must go back through the low32 resolver, like the asset placeholders above. */
bool IsPortBssAliasPointer(uint32_t low32) {
    if (low32 == Low32(reinterpret_cast<uintptr_t>(D_2000000))) {
        return true;
    }
    const uintptr_t d1Base = reinterpret_cast<uintptr_t>(D_1000000);
    const uint32_t d1Low = Low32(d1Base);
    const HostRange* r = gHostRanges.data();
    const size_t n = gHostRanges.size();
    for (size_t i = 0; i < n; i++) {
        if (r[i].begin == d1Base) {
            return (low32 >= d1Low) && (static_cast<size_t>(low32 - d1Low) < r[i].size);
        }
    }
    return false;
}

/* Runs for nearly every translated pointer over hundreds of ranges. Iterate via data()/size():
   MSVC Debug iterator checking on range-for made this a measurable per-frame cost once
   menus/gameplay started resolving EK asset pointers. */
static inline bool HostRangeListContains(const std::vector<HostRange>& list, uintptr_t full_addr) {
    const HostRange* r = list.data();
    const size_t n = list.size();
    for (size_t i = 0; i < n; i++) {
        if ((r[i].begin != 0) && (r[i].size != 0) &&
            (full_addr >= r[i].begin) && (full_addr < r[i].begin + r[i].size)) {
            return true;
        }
    }
    return false;
}

bool IsRawN64HostPointer(uintptr_t full_addr) {
    if (IsRdramHostPointer(full_addr)) {
        return true;
    }
    return HostRangeListContains(gRawN64Ranges, full_addr);
}

bool IsHostN64CommandPointer(uintptr_t full_addr) {
    return HostRangeListContains(gHostN64CommandRanges, full_addr);
}

bool IsHostWideCommandPointer(uintptr_t full_addr) {
    return HostRangeListContains(gHostWideCommandRanges, full_addr);
}

bool IsF3DAssetPointer(uintptr_t full_addr) {
    for (const HostRange& range : gF3DAssetRanges) {
        if ((range.begin == 0) || (range.size == 0)) {
            continue;
        }
        if ((full_addr >= range.begin) && (full_addr < range.begin + range.size)) {
            return true;
        }
    }
    return false;
}

bool DisplayListUsesF3D(const N64Gfx* source, size_t limit, size_t stride, bool isBig) {
    if (source == nullptr || limit == 0) {
        return false;
    }

    const size_t scanLimit = std::min(limit, kDisplayListValidationCommandLimit);
    for (size_t i = 0; i < scanLimit; ++i) {
        const uint8_t op = Opcode(ReadCommand(source, i, stride, isBig).w0);
        if (op == 0xB8u) {
            return true; // F3D G_ENDDL
        }
        if (op == kOpEndDl) {
            return false; // F3DEX2 G_ENDDL
        }
    }
    return false;
}

size_t ReadableByteLimit(uintptr_t address); // defined below

bool ResolveRdramLow32(uint32_t raw, size_t requiredBytes, uintptr_t* outHost) {
    if (gdx_rdram == nullptr || outHost == nullptr) {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(gdx_rdram);
    const uint32_t offset = raw - Low32(base);
    if (offset >= static_cast<uint32_t>(GDX_RDRAM_SIZE) ||
        requiredBytes > static_cast<size_t>(GDX_RDRAM_SIZE) - offset) {
        return false;
    }

    const uintptr_t full = base + offset;
    if (ReadableByteLimit(full) < requiredBytes) {
        return false;
    }

    *outHost = full;
    return true;
}

bool ResolveRegisteredHostPointer(uint32_t raw, ResolvedAddress& out, size_t requiredBytes = 1) {
    // The registered-host low32-window guess, which the converter deliberately does not reproduce
    // (G2ResolvePhysical does only the deterministic RDRAM-arena subset).
    if (!LegacyResolveEnabled()) {
        return false;
    }
    for (const HostRange& range : gHostRanges) {
        if ((range.begin == 0) || (range.size == 0)) {
            continue;
        }

        const uint32_t baseLow = Low32(range.begin);
        const uint32_t offset = raw - baseLow;
        /* gHostRanges low32 windows can overlap by chance for an unrelated raw value, so a match
           on the start byte alone is not enough: validate the declared size AND the real mapped
           pages, and keep scanning on failure rather than committing to the first hit in
           registration order. Accepting the first containing range produced wrong-but-readable
           resolutions and garbage vertex/matrix loads. */
        if (offset < range.size && requiredBytes <= range.size - offset) {
            const uintptr_t full = range.begin + offset;
            if (ReadableByteLimit(full) >= requiredBytes) {
                out.full = full;
                out.segmented = false;
                RecordLegacyResolveHit(LegacyResolveBranch::kRegisteredHostLow32, raw, gLegacyResolveCurrentOp);
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Binary N64 (8-byte) -> wide 16-byte boundary converter + cache.
//
// A narrow N64-format list (EK disk asset, ROM blob, or RDRAM-decoded segment) is converted ONCE
// to the wide layout the fast path consumes and cached across frames, so the per-frame path reads
// a resolver-free stride-16 source instead of re-parsing and guessing pointers.
//
// Wiring is LAZY: the redirect lives in EnqueueList, so every narrow list the draw-time walk
// reaches is converted on first encounter and sub-DL recursion falls out of the existing walk --
// each sub-DL is itself an EnqueueList hitting the same hook. See n64_gfx_convert.{h,cpp}.
// ---------------------------------------------------------------------------
gdx::GfxWideCache gWideCache;
// Runtime kill switch: GDX_G2_CONVERT=0 restores the pure narrow path without a rebuild.
bool gG2ConvertEnabled = true;
bool gG2ConvertInit = false;
// Dialect of each converted wide buffer, keyed by its exact data pointer. The converted buffer
// loses its source segment's dialect tag and the opcode-scan fallback misclassifies F3DEX2 setup
// DLs containing 0xB8 before 0xDF, so ProcessList must consult this rather than re-derive from
// the wide stream. Recorded before enqueue, so a live buffer's value is always current.
std::unordered_map<const void*, bool> gConvertedWideIsF3d;

/* Segment-9 fallback probe, strip later: G2ResolvePhysical variant of GdxSeg9FallbackDiag below,
 * adapted to its `uintptr_t* out_host` signature. GDX_LOG-gated, first 24 misses per process.
 * Declared here so it is visible at G2ResolvePhysical's call site, which precedes
 * N64DisplayListAdapter in this translation unit. */
class GdxSeg9FallbackDiagRaw {
  public:
    GdxSeg9FallbackDiagRaw(bool armed, uint32_t raw, size_t requiredBytes, const uintptr_t* outHost)
        : mArmed(armed), mRaw(raw), mRequiredBytes(requiredBytes), mOutHost(outHost) {}
    ~GdxSeg9FallbackDiagRaw() {
        if (!mArmed) {
            return;
        }
        static int sLogs = 0;
        if (sLogs >= 24) {
            return;
        }
        ++sLogs;
        if (*mOutHost != 0) {
            gdx_port_logf("[seg9diag] G2ResolvePhysical fallback served seg9 token raw=%08X req=%zu -> host=%p\n",
                          mRaw, mRequiredBytes, reinterpret_cast<void*>(*mOutHost));
        } else {
            gdx_port_logf("[seg9diag] G2ResolvePhysical seg9 token raw=%08X req=%zu UNRESOLVED (mode resolver + "
                          "all fallbacks missed)\n",
                          mRaw, mRequiredBytes);
        }
    }
    GdxSeg9FallbackDiagRaw(const GdxSeg9FallbackDiagRaw&) = delete;
    GdxSeg9FallbackDiagRaw& operator=(const GdxSeg9FallbackDiagRaw&) = delete;

  private:
    bool mArmed;
    uint32_t mRaw;
    size_t mRequiredBytes;
    const uintptr_t* mOutHost;
};

bool G2ResolvePhysical(void* /*user*/, uint32_t raw, size_t required_bytes, uintptr_t* out_host) {
    /* Registered overlay tokens must resolve BEFORE KSEG values are treated as RDRAM: EK display
       lists use original overlay VRAM addresses (0x80137528 for light structures), and reading
       those as physical RDRAM silently feeds zeroed memory to the renderer. These are
       authoritative token-to-host mappings, not low32 guesses, and the order matches
       TryResolveAddress's precedence. */
    {
        uintptr_t modeAddress = 0;
        if (gdx_resolve_mode_segment9(raw, required_bytes, &modeAddress) != 0 &&
            ReadableByteLimit(modeAddress) >= required_bytes) {
            *out_host = modeAddress;
            return true;
        }
    }

    // Armed only for segment-9 tokens that just missed the authoritative mode resolver; the
    // destructor fires on whichever return below serves (or fails to serve) the token. Gates are
    // read live, not latched in a static, so arming from Dev Tools takes effect without a restart.
    const bool seg9DiagEnabled = gdx_dev_gate(GDX_GATE_LOG_FILE) || gdx_dev_gate(GDX_GATE_DIAG_NODEINFO);
    GdxSeg9FallbackDiagRaw seg9Diag(seg9DiagEnabled && (raw >> 24) == 9u, raw, required_bytes, out_host);

    {
        const N64AddressRange* ranges = gN64AddressRanges.data();
        for (size_t ri = gN64AddressRanges.size(); ri > 0; ri--) {
            const N64AddressRange& range = ranges[ri - 1];
            if (raw < range.n64Begin) {
                continue;
            }
            const size_t offset = static_cast<size_t>(raw - range.n64Begin);
            if (offset <= range.size && required_bytes <= range.size - offset) {
                const uintptr_t host = range.hostBegin + offset;
                if (ReadableByteLimit(host) >= required_bytes) {
                    *out_host = host;
                    return true;
                }
            }
        }
    }

    if (gdx_rdram == nullptr) {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(gdx_rdram);
    if (raw >= 0x80000000u && raw <= 0xBFFFFFFFu) {
        const uint32_t phys = raw & 0x1FFFFFFFu;
        if (phys < static_cast<uint32_t>(GDX_RDRAM_SIZE)) {
            const uintptr_t full = base + phys;
            if (ReadableByteLimit(full) >= required_bytes) {
                *out_host = full;
                return true;
            }
        }
        return false;
    }
    if ((raw >= static_cast<uint32_t>(GDX_RDRAM_GFXPOOL_OFFSET)) &&
        (raw < static_cast<uint32_t>(GDX_RDRAM_SIZE))) {
        const uintptr_t full = base + raw;
        if (ReadableByteLimit(full) >= required_bytes) {
            *out_host = full;
            return true;
        }
    }
    return ResolveRdramLow32(raw, required_bytes, out_host);
}

void EnsureG2ConvertInit() {
    if (gG2ConvertInit) {
        return;
    }
    gG2ConvertInit = true;
    // Read ONCE: the converter's cache/context are wired here, so flipping the switch afterwards
    // would leave already-converted lists behind. Dev Tools labels the control "applies on
    // restart" for that reason.
    gG2ConvertEnabled = !gdx_dev_gate(GDX_GATE_NO_G2_CONVERT);
    // Plain-getenv override of the same switch, needed because GDX_GATE_NO_G2_CONVERT is Bucket B
    // and hard-wired to 0 in a Release build -- the binary where the interpolation flicker is
    // reproduced. Diagnostic only: it forces the slower narrow path for the whole session.
    if (const char* e = getenv("GDX_DIAG_NO_G2_CONVERT")) {
        if (e[0] != 0 && strcmp(e, "0") != 0) {
            gG2ConvertEnabled = false;
            gdx_port_logf("[g2] converter DISABLED by GDX_DIAG_NO_G2_CONVERT (narrow path)\n");
        }
    }
    gdx::ConvertContext ctx;
    ctx.resolve_physical = &G2ResolvePhysical;
    ctx.user = nullptr;
    gWideCache.SetContext(ctx);
}

uint64_t G2StampFor(const N64Gfx* src) {
    // Two disjoint stamp namespaces so a DMA generation never collides with an asset epoch.
    // RDRAM-backed lists are mutable (course loads overwrite the arena), so key them on the DMA
    // generation; asset/ROM lists are immutable once decoded (a reload lands at a fresh heap
    // address = new key), so key them on the asset epoch with the high bit set.
    if (IsRdramHostPointer(reinterpret_cast<uintptr_t>(src))) {
        return gDmaGeneration;
    }
    return 0x8000000000000000ull | static_cast<uint64_t>(gConvertEpoch);
}

bool ResolveGeneratedAssetStub(uint32_t raw, ResolvedAddress& out, size_t requiredBytes = 1) {
    AssetSegmentLookup lookup = {};
    if (!LookupAssetSegment(raw, lookup)) {
        return false;
    }

    /* Reject ONLY on an out-of-bounds offset, deliberately NOT on requiredBytes overshooting the
     * declared image: block-rounded copy estimates legitimately overshoot by a row (see
     * NativeRgba16RangeRemaining and its WIPE-transition LOADBLOCK case) and TranslateTexturePointer
     * clamps the actual copy to what is readable, so an inflated estimate cannot over-read. The
     * case a stricter check was meant to catch -- a token that really belongs to another source --
     * is covered by the gdx_mode_owns_segment gate below. */
    if (lookup.offset >= lookup.imageSize) {
        return false;
    }

    /* A mode-owned segment's live carve is authoritative; a ROM-backed AssetBindings.c row for the
     * same segment number is stale context. Redirect into the carve rather than reject: hud_gfx
     * and machine_global_gfx stay mode-owned for the ENTIRE race, so hard-rejecting starves every
     * compiled-symbol reference into segments 4/7 (HUD digits, flag, energy bar, racer DLs) for
     * the whole race. The gdx_mode_segment_content_matches gate keeps a row from a ROM family
     * that is NOT the resident one (stale machine_models during Course Edit; the never-loaded
     * seg-4 course_edit_textures_beta) falling through to a genuine reject.
     *
     * ReadableByteLimit is the only bound check, matching the 0x0A pattern below: segments 4/7
     * have no per-segment active-size accessor, and segment 9's stricter sGdxSeg9ActiveSize gate
     * already ran in gdx_resolve_mode_segment9 with first refusal in TryResolveAddress -- this is
     * only the permissive net behind it. ResolveWideAssetStubPointer reaches this function
     * directly, so the check has to live here too. */
    if (gdx_mode_owns_segment(lookup.segment) != 0) {
        const uintptr_t live = gSegments[lookup.segment];
        if (live != 0 && gdx_mode_segment_content_matches(lookup.segment, lookup.romBase) != 0 &&
            ReadableByteLimit(live + lookup.offset) >= requiredBytes) {
            out.full = live + lookup.offset;
            out.segment = lookup.segment;
            out.offset = lookup.offset;
            out.segmented = true;
            return true;
        }
        {
            static int sE2RejectLogs = 0;
            if (sE2RejectLogs < 16) {
                ++sE2RejectLogs;
                gdx_port_logf("[e2-reject] seg=%u off=%X req=%zu mode-owned, live fallback unavailable\n",
                              static_cast<unsigned>(lookup.segment), lookup.offset, requiredBytes);
            }
        }
        return false;
    }

    /* Live-carve preference, segment 0x0A ONLY. Interior venue-bank pointers low32-match whichever
     * venue's suffixed stub range happens to contain them (the stubs alias in low32), so the
     * per-symbol heap image can be a DIFFERENT venue's texture bank -- the wrong-venue floor on
     * the first frames of a race. The 0x0A carve is rotated by gdx_load_venue_texture_segment and
     * every venue shares the same bank*0x1000 layout, so resolving live is venue-correct by
     * construction.
     *
     * Do NOT generalize to other segments: routing seg-4/7 placeholder textures to the rdram
     * carve strips their o2r eligibility (the SETTIMG path's !IsRdramHostPointer gate) and puts
     * them on the raw-copy path with per-frame staleness refreshes -- a regression run garbled
     * the whole HUD and vehicles at unplayable FPS. The rank digits gain nothing either:
     * gSegments[4]+0x13DE0 is zero at race time, since the console's runtime fill has no port
     * equivalent yet. 0x0A is never mode-owned, hence its own case here. */
    if (lookup.segment == 0x0Au) {
        const uintptr_t live = gSegments[0x0A];
        if (live != 0 && ReadableByteLimit(live + lookup.offset) >= 1) {
            out.full = live + lookup.offset;
            out.segment = lookup.segment;
            out.offset = lookup.offset;
            out.segmented = true;
            return true;
        }
    }

    const uintptr_t base = EnsureAssetSegmentImage(lookup);
    if (base == 0) {
        return false;
    }
    out.full = base + lookup.offset;
    out.segment = lookup.segment;
    out.offset = lookup.offset;
    out.segmented = true;
    return true;
}

bool ResolvePortBssAlias(uint32_t raw, ResolvedAddress& out) {
    /*
     * D_2000000 is the original segment-2 BSS base. LinkStubs can only provide
     * a one-byte symbol token for it, while the active host storage begins at
     * D_80225800. Host-built display lists carry the token directly, so they
     * bypass normal segmented-address resolution and need the same alias here.
     * Do not use D_80225800_2: that duplicate overlap object is never initialized
     * by Game_ThreadEntry, so it contains a zero modelview matrix.
     */
    if (raw != Low32(reinterpret_cast<uintptr_t>(D_2000000))) {
        return false;
    }

    out.full = reinterpret_cast<uintptr_t>(D_80225800);
    out.segment = 2;
    out.offset = 0;
    out.segmented = true;
    return true;
}

/* Registry of REAL, full-size host arrays whose own address SETTIMG can carry directly -- as
 * opposed to a generated 1-byte LinkStubs placeholder -- so ResolveWideAssetStubPointer
 * RECOGNIZES them instead of miscounting them as unbound stubs. Populated from
 * gdx_ek_assets_fill()'s sEkAssetFills[] table and, at init, from decomp_port.c for base-game
 * compiled-in arrays with the same false positive (ending fireworks sprites,
 * sCourseMinimapPalette). This table affects RECOGNITION only: the resolved pointer is the delta
 * added back onto the array's own address, never different bytes. Pair a registration with
 * gdx_set_native_rgba16_texture_range when the compiled bytes also need the host-endian byteswap.
 *
 * The match is delta<size with unsigned wraparound, not exact-base, because interior offsets are
 * real: course_edit/191080.c's node-info number strip indexes aCourseEditNumberSheetTex at
 * +0x120/+0x240 for later digit-glyph bands. Same rule gdx_lookup_asset_segment_interior uses.
 *
 * gdx_register_host_pointer_stub is defined further down, outside this anonymous namespace,
 * with the other extern "C" gdx_register_* functions. */
bool ResolveHostPointerStub(uint32_t raw, ResolvedAddress& out) {
    for (const HostRange& entry : gHostPointerStubs) {
        const uint32_t base = Low32(entry.begin);
        const uint32_t delta = raw - base;
        if (delta < entry.size) {
            out.full = entry.begin + delta;
            out.segment = 0;
            out.offset = delta;
            out.segmented = false;
            return true;
        }
    }
    return false;
}

bool ResolveVenueBankAlias(uint32_t raw, ResolvedAddress& out); // fwd decl (defined below)
/* Game-BUILT wide DLs carry the REAL host address of a generated 1-byte asset stub whenever a
   compile-time table stores asset symbols -- course.c:101-112 stores the venue banks
   D_A000000..D_A008000 directly. Taken verbatim by the wide host-pointer fast path (the EXE
   module is a registered host range) the track floor, walls and tunnel sample EXE data-section
   bytes: black where the stub neighborhood is zeroed, striped garbage otherwise, and different
   per BUILD as the module layout moves. The low32 resolvers already know these identities but
   wide pointers never reach them, hence this hop.

   Matching on low32 is sound here rather than a guess: within one process each stub symbol has
   exactly one host address, so low32 -> address is injective over the stub set, and the value
   being compared IS that address truncated -- a genuine data pointer cannot false-match a stub
   it does not equal. Returns 0 when `full` is not a known stub. */
uintptr_t ResolveWideAssetStubPointer(uintptr_t full, uintptr_t moduleBegin, uintptr_t moduleEnd,
                                       size_t requiredBytes = 1) {
    if (full == 0) {
        return 0;
    }
    /* Exact-symbol matchers MUST run before the module-range gate below. On Linux PIE,
       GetMainModuleRange under-covers the anonymous .bss tail holding the venue-bank stubs, so
       gating exact matching behind the range check drops every venue bank and the track
       floor/walls/pipes sample the raw zero stub byte (solid black). Exact resolution is safe
       outside the range because the stub's own address is what matched. */
    const uint32_t low = Low32(full);
    ResolvedAddress out = {};
    if (ResolveVenueBankAlias(low, out)) {
        return out.full;
    }
    if (ResolveGeneratedAssetStub(low, out, requiredBytes)) {
        return out.full;
    }
    if (ResolveHostPointerStub(low, out)) {
        return out.full;
    }
    /* Kept to guard any future range-scoped resolution: a `full` outside the EXE module is not a
       generated stub and must not be reinterpreted. Nothing range-scoped exists below yet. */
    if (moduleBegin == 0 || full < moduleBegin || full >= moduleEnd) {
        return 0;
    }
    return 0;
}

/* course.c's road material table (ROAD_1..WALLED_ROAD) stores the unsuffixed venue bank symbols
 * D_A000000..D_A008000 directly, so road-pass SETTIMGs carry those stubs' truncated addresses.
 * The stubs are 1-byte placeholders with no binding; without this alias they false-match
 * zero-filled memory and the road samples an all-black texture. The real data is the per-venue
 * image gdx_load_venue_texture_segment puts in gSegments[0x0A]. Exact-base match only: the stubs
 * are packed 1 byte apart, so an interior span would collide with the next symbol. */
bool ResolveVenueBankAlias(uint32_t raw, ResolvedAddress& out) {
    if (gSegments[0x0A] == 0) {
        return false;
    }
    // {stub symbol, byte offset within the venue segment image}. Offsets are listed rather than
    // derived from an index because only the first 11 are the uniform 0x1000-byte banks; the EK
    // Road-Type icons after them are 576-byte and not bank-aligned.
    struct BankEntry {
        uint32_t low32;
        uint32_t offset;
    };
    static const BankEntry kBanks[] = {
        { Low32(reinterpret_cast<uintptr_t>(D_A000000)), 0x0000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A001000)), 0x1000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A002000)), 0x2000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A003000)), 0x3000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A004000)), 0x4000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A005000)), 0x5000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A006000)), 0x6000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A007000)), 0x7000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A008000)), 0x8000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A009000)), 0x9000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00A000)), 0xA000u },
#ifdef EXPANSION_KIT
        { Low32(reinterpret_cast<uintptr_t>(D_A00B000)), 0xB000u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00B240)), 0xB240u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00B480)), 0xB480u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00B6C0)), 0xB6C0u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00B900)), 0xB900u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00BB40)), 0xBB40u },
        { Low32(reinterpret_cast<uintptr_t>(D_A00BD80)), 0xBD80u },
#endif
    };
    for (const BankEntry& bank : kBanks) {
        if (raw == bank.low32) {
            out.full = gSegments[0x0A] + bank.offset;
            out.segment = 0x0A;
            out.offset = bank.offset;
            out.segmented = true;
            return true;
        }
    }
    return false;
}

uintptr_t EnsureSetupGfxSegment() {
    uint32_t offset = 0;
    const uintptr_t base = EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(D_3000000)), &offset);
    if (base != 0) {
        return base;
    }

    if (!gSetupGfxSegment.empty()) {
        return reinterpret_cast<uintptr_t>(gSetupGfxSegment.data());
    }

    // Same shim-aware gate as EnsureAssetSegmentImage: the ROM is required only when no archive
    // blob contains this range. The shim read below fails cleanly either way; this only avoids a
    // pointless allocation.
    uint32_t setupSpan = 0;
    if (GdxSegmentSourceContainingSpan(static_cast<uint32_t>(kSetupGfxRomOffset), &setupSpan) == 0 &&
        ((gdx_rom_buffer == nullptr) || (gdx_rom_size < kSetupGfxRomOffset + kSetupGfxSize))) {
        return 0;
    }

    gSetupGfxSegment.resize(kSetupGfxSize);
    // Hardcoded 0x17B1E0/0x778 fallback for a missing-binding regression only: D_3000000 is in
    // sAssetSegmentMap, so the primary path above always resolves and this is dead in normal
    // operation.
    std::vector<uint8_t> raw(kSetupGfxSize);
    if (!GdxSegmentSourceRead(static_cast<uint32_t>(kSetupGfxRomOffset),
                              static_cast<uint32_t>(kSetupGfxSize), raw.data())) {
        return 0;
    }
    for (size_t i = 0; i < kSetupGfxSize; i += sizeof(uint32_t)) {
        const uint32_t word = ReadBE32(raw.data() + i);
        std::memcpy(gSetupGfxSegment.data() + i, &word, sizeof(word));
    }

    {
        const uintptr_t segBase = reinterpret_cast<uintptr_t>(gSetupGfxSegment.data());
        // Registered so RegisteredHostRemaining() treats this as ROM-backed and G_MOVEWORD cannot
        // overwrite gSegments[3] with a garbage arena-buffer address.
        gHostRanges.push_back({ segBase, gSetupGfxSegment.size() });
        gHostN64CommandRanges.push_back({ segBase, gSetupGfxSegment.size() });
        if (gSegments[3] == 0) {
            gSegments[3] = segBase;
        }
        return segBase;
    }
}

uintptr_t MakeFramebufferToken(uint32_t raw) {
#if UINTPTR_MAX > UINT32_MAX
    constexpr uintptr_t kFramebufferTokenBase = 0x0000000300000000ull;
#else
    constexpr uintptr_t kFramebufferTokenBase = 0x30000000u;
#endif
    // CIMG and ZIMG commands that reference the same N64 address must retain
    // pointer identity. Fast3D uses that identity to distinguish a depth clear
    // from a visible color fill.
    return kFramebufferTokenBase | (static_cast<uintptr_t>(raw) & 0xFFFFFFFEu);
}

bool ResolveSetupGfxStub(uint32_t raw, ResolvedAddress& out) {
    struct SetupSymbol {
        const uint8_t* symbol;
        uint32_t offset;
    };

    static const SetupSymbol kSetupSymbols[] = {
        { D_3000000, 0x000 }, { D_3000028, 0x028 }, { D_3000050, 0x050 }, { D_3000088, 0x088 },
        { D_30000C0, 0x0C0 }, { D_3000100, 0x100 }, { D_3000138, 0x138 }, { D_3000170, 0x170 },
        { D_30001A8, 0x1A8 }, { D_3000270, 0x270 }, { D_30002E0, 0x2E0 }, { D_3000338, 0x338 },
        { D_3000400, 0x400 }, { D_3000438, 0x438 }, { D_3000470, 0x470 }, { D_30004A8, 0x4A8 },
        { D_30004E0, 0x4E0 }, { D_3000510, 0x510 }, { D_3000540, 0x540 }, { D_3000590, 0x590 },
        { D_30005D8, 0x5D8 }, { D_3000688, 0x688 }, { D_30006D0, 0x6D0 },
    };

    for (const SetupSymbol& entry : kSetupSymbols) {
        if (raw == Low32(reinterpret_cast<uintptr_t>(entry.symbol))) {
            const uintptr_t base = EnsureSetupGfxSegment();
            if (base == 0) {
                return false;
            }
            out.full = base + entry.offset;
            out.segment = 3;
            out.offset = entry.offset;
            out.segmented = true;
            return true;
        }
    }
    return false;
}

#if defined(_WIN32)
bool IsReadablePageProtect(uint32_t protect);

struct WindowsMemoryRegion {
    uintptr_t begin;
    uintptr_t end;
    bool readable;
};

static std::vector<WindowsMemoryRegion> sWindowsMemoryRegions;

static const WindowsMemoryRegion* FindWindowsMemoryRegion(uintptr_t address) {
    size_t low = 0;
    size_t high = sWindowsMemoryRegions.size();
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const WindowsMemoryRegion& region = sWindowsMemoryRegions[middle];
        if (address < region.begin) {
            high = middle;
        } else if (address >= region.end) {
            low = middle + 1;
        } else {
            return &region;
        }
    }
    return nullptr;
}

static bool WindowsMemoryRegionFor(uintptr_t address, WindowsMemoryRegion& out) {
    const WindowsMemoryRegion* cached = FindWindowsMemoryRegion(address);
    if (cached != nullptr) {
        out = *cached;
        return true;
    }

    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0) {
        return false;
    }

    const uintptr_t begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
    if (info.RegionSize == 0 || info.RegionSize > UINTPTR_MAX - begin) {
        return false;
    }

    WindowsMemoryRegion region = {
        begin,
        begin + info.RegionSize,
        info.State == MEM_COMMIT && IsReadablePageProtect(info.Protect),
    };
    if (!region.readable) {
        out = region;
        return address >= out.begin && address < out.end;
    }
    auto insertAt = std::lower_bound(
        sWindowsMemoryRegions.begin(), sWindowsMemoryRegions.end(), region.begin,
        [](const WindowsMemoryRegion& existing, uintptr_t value) {
            return existing.begin < value;
        });
    insertAt = sWindowsMemoryRegions.insert(insertAt, region);
    out = *insertAt;
    return address >= out.begin && address < out.end;
}

static void ResetWindowsMemoryRegionCache() {
    sWindowsMemoryRegions.clear();
    if (sWindowsMemoryRegions.capacity() == 0) {
        sWindowsMemoryRegions.reserve(64);
    }
}
#else
// ---------------------------------------------------------------------------------------------
// POSIX memory-probe backend: a snapshot of /proc/self/maps with miss-triggered refresh.
//
// VirtualQuery answers "readable, and how far does the region extend?" per call; /proc/self/maps
// is the Linux equivalent but this bridge probes thousands of times per frame, so re-parsing per
// call is out. Snapshot once, re-parse exactly once on a miss, then re-query -- that covers
// regions mmap'd after boot (RDRAM calloc, fiber stacks, late texture arenas) without a watcher
// thread. Readable regions are coalesced at parse time so a "rest of the block" answer is
// comparable to VirtualQuery's region-spanning result.
//
// These probes run only on the graphics thread; the atomic generation counter is otherwise unused.
// ---------------------------------------------------------------------------------------------
struct MapsRegion {
    uintptr_t begin;
    uintptr_t end;
    bool      readable;
};
static std::vector<MapsRegion> sMaps;               // sorted by begin, readable runs coalesced
static std::atomic<uint32_t>   sMapsGeneration{0};

static void ParseProcMaps() {
    std::vector<MapsRegion> parsed;
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (f != nullptr) {
        char line[512];
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            unsigned long long b = 0, e = 0;
            char perms[8] = {0};
            // Line shape: "begin-end perms offset dev inode pathname".
            if (std::sscanf(line, "%llx-%llx %7s", &b, &e, perms) == 3) {
                MapsRegion r;
                r.begin = static_cast<uintptr_t>(b);
                r.end = static_cast<uintptr_t>(e);
                r.readable = (perms[0] == 'r');
                parsed.push_back(r);
            }
        }
        std::fclose(f);
    }

    std::sort(parsed.begin(), parsed.end(),
              [](const MapsRegion& a, const MapsRegion& b) { return a.begin < b.begin; });

    // Coalesce touching readable regions so a limit query spans the whole run, like VirtualQuery.
    std::vector<MapsRegion> coalesced;
    for (const MapsRegion& r : parsed) {
        if (!coalesced.empty() && coalesced.back().readable && r.readable &&
            coalesced.back().end == r.begin) {
            coalesced.back().end = r.end;
        } else {
            coalesced.push_back(r);
        }
    }

    sMaps.swap(coalesced);
    sMapsGeneration.fetch_add(1, std::memory_order_relaxed);
}

static const MapsRegion* FindMapsRegion(uintptr_t addr) {
    size_t lo = 0;
    size_t hi = sMaps.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (addr < sMaps[mid].begin) {
            hi = mid;
        } else if (addr >= sMaps[mid].end) {
            lo = mid + 1;
        } else {
            return &sMaps[mid];
        }
    }
    return nullptr;
}

// Copies the region out by value: a subsequent probe can re-parse and reallocate sMaps.
static bool PosixRegionFor(uintptr_t addr, MapsRegion& out) {
    if (sMaps.empty()) {
        ParseProcMaps();
    }
    const MapsRegion* r = FindMapsRegion(addr);
    if (r == nullptr) {
        ParseProcMaps(); // one re-parse then re-query
        r = FindMapsRegion(addr);
    }
    if (r == nullptr) {
        return false;
    }
    out = *r;
    return true;
}
#endif

void GetMainModuleRange(uintptr_t& moduleBegin, uintptr_t& moduleEnd) {
    moduleBegin = 0;
    moduleEnd = 0;

#ifdef _WIN32
    HMODULE module = GetModuleHandleA(nullptr);
    if (module == nullptr) {
        return;
    }

    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }

    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }

    moduleBegin = reinterpret_cast<uintptr_t>(module);
    moduleEnd = moduleBegin + ntHeaders->OptionalHeader.SizeOfImage;
#else
    // Mirror "module base + SizeOfImage": take the contiguous run of /proc/self/maps entries whose
    // pathname is the main executable, using begin-of-first .. end-of-last. dladdr on a local
    // function gives the load base as a cross-check; the executable path comes from readlink of
    // /proc/self/exe (dli_fname is a fallback, since it can be a relative/short name).
    Dl_info info;
    std::memset(&info, 0, sizeof(info));
    const bool haveDl = dladdr(reinterpret_cast<void*>(&GetMainModuleRange), &info) != 0;

    char exePath[4096];
    ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    const char* wantPath = nullptr;
    if (n > 0) {
        exePath[n] = '\0';
        wantPath = exePath;
    } else if (haveDl && info.dli_fname != nullptr && info.dli_fname[0] != '\0') {
        wantPath = info.dli_fname;
    }
    if (wantPath == nullptr) {
        return;
    }

    FILE* f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) {
        return;
    }
    char line[4608];
    uintptr_t lo = 0, hi = 0;
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned long long b = 0, e = 0;
        char perms[8] = {0};
        char path[4096] = {0};
        // begin-end perms offset dev inode <spaces> pathname
        int matched = std::sscanf(line, "%llx-%llx %7s %*x %*s %*u %4095[^\n]", &b, &e, perms, path);
        if (matched < 3) {
            continue;
        }
        char* p = path;
        while (*p == ' ') {
            ++p;
        }
        if (matched == 4 && std::strcmp(p, wantPath) == 0) {
            if (!found) {
                lo = static_cast<uintptr_t>(b);
                found = true;
            }
            hi = static_cast<uintptr_t>(e);
        }
    }
    std::fclose(f);

    if (found) {
        moduleBegin = lo;
        moduleEnd = hi;
    } else if (haveDl && info.dli_fbase != nullptr) {
        // Pathname match failed (unusual): fall back to the dladdr load base alone. Without an end
        // we cannot bound the module, so leave moduleEnd at 0 -- callers treat {base,0} the same
        // as {0,0} (an empty range), i.e. no worse than the Windows failure path.
        moduleBegin = reinterpret_cast<uintptr_t>(info.dli_fbase);
    }
#endif
}

bool IsReadablePageProtect(uint32_t protect) {
#ifdef _WIN32
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    protect &= 0xFF;
    return protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
#else
    (void)protect;
    return true;
#endif
}

size_t ReadableCommandLimit(const void* source, size_t stride = kN64GfxStride) {
#ifdef _WIN32
    const uintptr_t address = reinterpret_cast<uintptr_t>(source);
    WindowsMemoryRegion region;
    if (!WindowsMemoryRegionFor(address, region) || !region.readable) {
        return 0;
    }

    if (address >= region.end) {
        return 0;
    }

    return static_cast<size_t>((region.end - address) / stride);
#else
    const uintptr_t addr = reinterpret_cast<uintptr_t>(source);
    MapsRegion r;
    if (!PosixRegionFor(addr, r) || !r.readable) {
        return 0;
    }
    if (addr >= r.end) {
        return 0;
    }
    return static_cast<size_t>((r.end - addr) / stride);
#endif
}

size_t ReadableByteLimit(uintptr_t address) {
#ifdef _WIN32
    WindowsMemoryRegion region;
    if (!WindowsMemoryRegionFor(address, region) || !region.readable) {
        return 0;
    }

    if (address >= region.end) {
        return 0;
    }

    return static_cast<size_t>(region.end - address);
#else
    MapsRegion r;
    if (!PosixRegionFor(address, r) || !r.readable) {
        return 0;
    }
    if (address >= r.end) {
        return 0;
    }
    return static_cast<size_t>(r.end - address);
#endif
}

bool IsReadableAddress(uintptr_t address) {
#ifdef _WIN32
    WindowsMemoryRegion region;
    if (!WindowsMemoryRegionFor(address, region)) {
        return false;
    }
    return region.readable;
#else
    MapsRegion r;
    if (!PosixRegionFor(address, r)) {
        return false;
    }
    return r.readable;
#endif
}

uintptr_t MakePersistentVtxCopy(uintptr_t source, size_t count) {
    if (source == 0 || count == 0) {
        return 0;
    }
    size_t requiredBytes = count * 16;

    /* Defense in depth: callers are expected to have validated requiredBytes at `source` via
       TranslateDataPointer, but an under-validated resolution (only 1 byte proven readable) lets
       this loop walk past the vertex buffer into unrelated host memory, and one garbage vertex is
       a visible stretched-polygon spike. Clamp and zero the tail; log so a remaining spike still
       has a visible cause. */
    const size_t readable = ReadableByteLimit(source);
    size_t safeCount = count;
    if (readable < requiredBytes) {
        safeCount = readable / 16;
        static int sVtxClampLogs = 0;
        if (sVtxClampLogs < 40) {
            ++sVtxClampLogs;
            gdx_port_logf("[vtx-clamp] source=%p requested=%zu(%zuB) readable=%zuB clampedCount=%zu\n",
                          reinterpret_cast<void*>(source), count, requiredBytes, readable, safeCount);
        }
    }

    auto alloc = std::make_unique<uint8_t[]>(requiredBytes);
    uint8_t* out = alloc.get();
    std::memset(out, 0, requiredBytes);
    gPersistentAllocations.push_back(std::move(alloc));

    const uint8_t* in = reinterpret_cast<const uint8_t*>(source);
    for (size_t i = 0; i < safeCount; i++) {
        uint16_t* out_s = reinterpret_cast<uint16_t*>(out + i * 16);
        const uint16_t* in_s = reinterpret_cast<const uint16_t*>(in + i * 16);
        out_s[0] = Byteswap16(in_s[0]);
        out_s[1] = Byteswap16(in_s[1]);
        out_s[2] = Byteswap16(in_s[2]);
        out_s[3] = Byteswap16(in_s[3]);
        out_s[4] = Byteswap16(in_s[4]);
        out_s[5] = Byteswap16(in_s[5]);
        out[i * 16 + 12] = in[i * 16 + 12];
        out[i * 16 + 13] = in[i * 16 + 13];
        out[i * 16 + 14] = in[i * 16 + 14];
        out[i * 16 + 15] = in[i * 16 + 15];
    }
    return reinterpret_cast<uintptr_t>(out);
}

/* Big-endian static Mtx data (asset/heap sources) word-swapped for the
   interpreter, which reads matrices as host-order u32 words. Host-built
   matrices (Matrix_ToMtx's j^1 layout) must NOT pass through here. */
uintptr_t MakePersistentMtxCopy(uintptr_t source) {
    if (source == 0) {
        return 0;
    }
    auto alloc = std::make_unique<uint8_t[]>(64);
    uint8_t* out = alloc.get();
    std::memset(out, 0, 64);
    gPersistentAllocations.push_back(std::move(alloc));

    /* Same clamp idiom as MakePersistentVtxCopy, applied internally so ANY caller -- including
       the kOpVtx F3D-remapped-to-Mtx branch -- cannot turn an under-validated resolution into an
       out-of-bounds read. The buffer is zeroed above, so a fully-unreadable `source` yields a
       degenerate all-zero matrix rather than a null return. That deliberately differs from the
       primary kOpMtx call site, which drops the whole command on the same condition. */
    const size_t readable = ReadableByteLimit(source);
    const size_t safeWords = std::min<size_t>(16, readable / 4);
    if (readable < 64) {
        static int sMtxClampLogs = 0;
        if (sMtxClampLogs < 40) {
            ++sMtxClampLogs;
            gdx_port_logf("[mtx-clamp] source=%p requested=64B readable=%zuB clampedWords=%zu\n",
                          reinterpret_cast<void*>(source), readable, safeWords);
        }
    }

    const uint32_t* in_w = reinterpret_cast<const uint32_t*>(source);
    uint32_t* out_w = reinterpret_cast<uint32_t*>(out);
    for (size_t i = 0; i < safeWords; i++) {
        out_w[i] = Byteswap32(in_w[i]);
    }
    return reinterpret_cast<uintptr_t>(out);
}

/* True when a texture/TLUT source lands inside the two live GfxPools (D_8024DCE0[2]).
 *
 * MakePersistentRawTextureCopy treats any registered host range as IMMUTABLE and skips the
 * memcmp. That holds for asset carves, the ROM buffer and the audio heap, but the GfxPools are
 * registered too (decomp_port.c:116) and are RAM scratch the game rewrites EVERY FRAME, so the
 * first copy minted at a pool address would be served for the rest of the process.
 *
 * Night-course background sprites are where that shows: their CI4 TLUTs are staged into the pool
 * each frame (background.c:1376) and bound from it via the segment-1 alias (background.c:1438),
 * with slot indices assigned from 0 in first-seen order per course (background.c:1278-1306). So
 * every night course reuses the same two host addresses, and racing Mute City 2 before Silence
 * leaves Silence's moon decoding the skyline's palette -- bright pink. Cold-booting into Silence
 * looks correct, which is the tell.
 *
 * Excluding the pools puts them back on the `changed` path, which re-copies AND queues a
 * TextureCacheDelete so LUS re-imports the same frame. Cost is trivial: pool-backed texture
 * sources are only ever 32-byte TLUTs; pool vertices and matrices go through
 * MakePersistentVtxCopy/MakePersistentMtxCopy instead.
 *
 * Deliberately NOT extended to D_1000000: that is the never-written BSS alias, so a memcmp
 * against it could never observe a change. */
extern "C" size_t gdx_gfxpool_sizeof(void);
bool IsGfxPoolHostRange(uintptr_t source) {
    const size_t poolSize = gdx_gfxpool_sizeof();
    if (poolSize == 0) {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(&D_8024DCE0[0]);
    const size_t span = poolSize * 2; // GfxPool D_8024DCE0[2]
    return (source >= base) && (source < base + span);
}

/* The 0x800-byte window at segment 8 + 0x14A20 (D_8014A20, tall-building texture) is the one
 * region of a decoded asset-segment image the game REWRITES: func_800747EC DMAs a per-venue slice
 * of super_textures over it at every course load. Decoded images are registered host ranges, so
 * without this carve-out the immutable fast path serves whichever venue's bytes were minted first
 * — the same stale-copy class as the GfxPool TLUTs above. Written once by
 * gdx_load_venue_building_texture and never changes after, since gLoadedAssetSegments never
 * evicts. */
static std::atomic<uintptr_t> sVenueBuildingTexBase{ 0 };

static bool IsVenueBuildingTextureRange(uintptr_t source) {
    const uintptr_t base = sVenueBuildingTexBase.load(std::memory_order_relaxed);
    return (base != 0) && (source >= base) && (source < base + 0x800);
}

uintptr_t MakePersistentRawTextureCopy(uintptr_t source, size_t requiredBytes, bool* outRefreshed) {
    if (outRefreshed != nullptr) {
        *outRefreshed = false;
    }
    if ((source == 0) || (requiredBytes == 0)) {
        return 0;
    }

    size_t readable = ReadableByteLimit(source);
    if (readable == 0) {
        readable = RegisteredHostRemaining(source);
    }
    if (readable == 0) {
        return 0;
    }

    const size_t copyBytes = std::min(requiredBytes, readable);

    for (PersistentRawTextureCopy& copy : gRawTextureCopies) {
        if (copy.source != source) {
            continue;
        }

        const bool needsResize = (copy.bytes == nullptr) || (copy.size < requiredBytes);
        bool changed = needsResize;
        if (!needsResize) {
            if (IsNativeRgba16Range(source, copyBytes)) {
                /* Transition captures live in the back arena.  That address is
                   deliberately reused for the next transition, so treating a
                   registered host range as immutable leaves the persistent
                   texture copy containing the previous screen.  Compare in
                   the byte order stored by CopyRawTextureBytes instead. */
                changed = !NativeRgba16CopyMatches(copy.bytes.get(), source, copyBytes);
            } else if (IsRdramHostPointer(source) || IsN64FramebufferRange(source, copy.size)) {
                changed = HostRangeChanged(source, copy.size, copy.dmaGenAtCopy);
            } else {
                // ROM-backed textures are stable after the segment is loaded; skip memcmp.
                // The GfxPools are registered host ranges too but are per-frame RAM scratch,
                // so they must stay on the compare path -- see IsGfxPoolHostRange above.
                // Likewise the venue building-texture window, rewritten per course load.
                const bool stableSource = RegisteredHostRemaining(source) > 0 && !IsGfxPoolHostRange(source) &&
                                          !IsVenueBuildingTextureRange(source);
                if (!stableSource) {
                    changed = (std::memcmp(copy.bytes.get(), reinterpret_cast<const void*>(source), copyBytes) != 0);
                }
            }
        }

        /* GDX_DIAG_POOL_TEX=1: dumps pool-backed TLUT sources and whether the compare saw a
           change. Entering a night course, the first line for a source must show chg=1. */
        static const bool sDiagPoolTex = std::getenv("GDX_DIAG_POOL_TEX") != nullptr;
        if (sDiagPoolTex && copyBytes >= 8 && IsGfxPoolHostRange(source)) {
            static int sPoolTexLogs = 0;
            if (sPoolTexLogs < 200) {
                ++sPoolTexLogs;
                const uint8_t* s = reinterpret_cast<const uint8_t*>(source);
                gdx_port_logf("[pool-tex] src=%p bytes=%zu chg=%d %02X%02X %02X%02X %02X%02X %02X%02X\n",
                              reinterpret_cast<void*>(source), copyBytes, changed ? 1 : 0,
                              s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
            }
        }

        if (changed) {
            if (outRefreshed != nullptr) {
                *outRefreshed = true;
            }
            if (!needsResize) {
                gPendingTextureCacheDeletes.push_back(reinterpret_cast<uintptr_t>(copy.bytes.get()));
                std::memset(copy.bytes.get(), 0, copy.size);
                CopyRawTextureBytes(copy.bytes.get(), source, copyBytes);
            } else {
                if (copy.bytes != nullptr) {
                    gPendingTextureCacheDeletes.push_back(reinterpret_cast<uintptr_t>(copy.bytes.get()));
                    gPersistentAllocations.push_back(std::move(copy.bytes));
                }
                auto refreshed = std::make_unique<uint8_t[]>(requiredBytes);
                std::memset(refreshed.get(), 0, requiredBytes);
                CopyRawTextureBytes(refreshed.get(), source, copyBytes);
                copy.bytes = std::move(refreshed);
                copy.size = requiredBytes;
            }
            copy.dmaGenAtCopy = gDmaGeneration;
        }
        return reinterpret_cast<uintptr_t>(copy.bytes.get());
    }

    PersistentRawTextureCopy copy = {};
    copy.source = source;
    copy.size = requiredBytes;
    copy.bytes = std::make_unique<uint8_t[]>(requiredBytes);
    std::memset(copy.bytes.get(), 0, requiredBytes);
    CopyRawTextureBytes(copy.bytes.get(), source, copyBytes);
    copy.dmaGenAtCopy = gDmaGeneration;

    const uintptr_t out = reinterpret_cast<uintptr_t>(copy.bytes.get());
    gRawTextureCopies.emplace_back(std::move(copy));
    if (outRefreshed != nullptr) {
        *outRefreshed = true;
    }
    return out;
}

// =============================================================================================
// Matrix-interpolation retention + scratch-slot indirection (default-OFF, GDX_INTERP_P0)
// =============================================================================================
// Every hook is a strict no-op with zero allocation on the normal path.
//
// RETENTION INVARIANT: the resolved command buffer and every input the interpreter dereferences
// must stay valid and re-executable within one tick. Those inputs are `converted` (owned by the
// local N64DisplayListAdapter, valid until gdx_gfx_run returns); interp->mSegmentPointers[0..15]
// (a gSegments snapshot taken just before ConvertRoot); the bytes those commands point at --
// GfxPool matrices in segment-1 RDRAM, persistent copies in gPersistentAllocations (freed only
// AFTER Run, so a replay must happen BEFORE that free), vertex staging, texture sources; and the
// latched ucode variant.
//
// SCRATCH-SLOT TRANSPARENCY: at G_MTX translation, pool-span matrices are copied into a stable
// per-tick scratch slot and the command's pointer rewritten to it, recording (origPtr,
// scratchPtr). At t=1 the scratch is a byte copy of the pool matrix, so interpreter output is
// identical -- checked by memcmp via GdxP0TransparencyViolations(). Non-pool matrices resolve
// outside the pool span and pass through untouched.
//
// The FNV-1a cmdhash logged before each pass is the evidence that the retained buffer is stable
// and the interpreter does not mutate it in place; a differing cmdhash would mean replay has to
// snapshot/restore the buffer per pass.
//
// A second interp->Run() replaying the same buffer is safe on the DX11 backend because Run()
// clears+draws+MSAA-resolves but does NOT present -- present is interp->EndFrame()/SwapBuffers,
// called once by the host.

struct GdxP0Mtx {
    uint64_t w[8];
}; // 64 bytes; alignof 8 satisfies the interpreter's (ptr & 7) == 0 matrix-alignment check.
static_assert(sizeof(GdxP0Mtx) == 64, "N64 Mtx is 64 bytes");

// Bucket B (it changes what is rendered), so without GDX_DEV_TOOLS the gate is a compile-time 0
// and the whole path is dead. Sample once per gfx task (mP0Enabled in the caller), never per
// command: a mid-task flip would desync the two-pass scratch bookkeeping.
static bool GdxInterpP0Enabled() {
    return gdx_dev_gate(GDX_GATE_INTERP_P0) != 0;
}

// ===== Host-driven decoupled-loop configuration (set by port/main.cpp per iteration) =====
// The host configures each tick's sub-frame schedule BEFORE gdx_dispatch; gdx_gfx_run consumes it
// from inside dispatch. Graphics/main thread only, hence no locking. Inert unless
// gdx_interp::P2HostActive() and the host set active=1 for the tick.
namespace {
struct GdxInterpHostCfg {
    bool active = false;       // host enabled the decoupled present loop for this tick
    double tickStart = 0.0;    // now-fn timestamp at the top of this host iteration
    double tickDuration = 0.0; // one 60 Hz logic-tick budget in now-fn units (~1.001/60 s)
    int maxSubframes = 4;      // VSync-off cap: presents don't block, so bound the loop
};
GdxInterpHostCfg gGdxInterpHostCfg;
GdxInterpNowFn gGdxInterpNowFn = nullptr;
bool gGdxInterpPresentedLastTick = false;
int gGdxInterpLastSubframes = 0;
double gGdxInterpLastT = 1.0;
// The presents/sec meter counts EVERY sub-frame present over a ~0.5 s wall-clock window, so the
// menu shows true presents/sec rather than logic ticks.
/* [interp-idem] cumulative ticks whose replays bound DIFFERENT textures than pass 0, i.e. ticks
   where re-executing the display list was NOT idempotent. */
extern "C" void gdx_gfx_texbind_hash_reset(void);
extern "C" unsigned long long gdx_gfx_texbind_hash(void);
static unsigned long long sGdxIdemPass0Hash = 0;
static bool sGdxIdemTickCounted = false;
/* [interp-shot] >=0 while a sub-frame render should be captured; the pass index becomes the file
   suffix. Set by the sub-frame loop, consumed by gdx_gfx_post_run_capture below. */
int gGdxShotArmedPass = -1;
size_t gGdxIdemDivergentTicks = 0;
size_t gGdxIdemMultiPassTicks = 0;

size_t gGdxInterpLastLerped = 0;
/* [interp-pair] These ACCUMULATE across ticks: the telemetry line prints one tick in 120, so a
   per-tick snapshot makes a low-rate mispairing statistically invisible. Totals plus a window max
   are what make the reading decisive. */
float gGdxInterpPairMaxDelta = 0.0f;   // max delta since the last read (reader resets)
size_t gGdxInterpPairSuspect = 0;      // cumulative suspicious pairings since boot
size_t gGdxInterpPairLerped = 0;       // cumulative paired slots -- the denominator
size_t gGdxInterpLastSnapped = 0;
// Sub-frames the swapchain limiter refused this tick. Counted apart from presented ones, or a
// heavy tick reads as healthy in the log while dropping frames on screen.
int gGdxInterpLastDropped = 0;
double gGdxInterpPresentsPerSec = 0.0;
int gGdxInterpPresentWindowCount = 0;
double gGdxInterpPresentWindowStart = -1.0;
// Read by the adapter ctor instead of re-reading the CVar mid-tick, so the "is P2 host mode on"
// decision matches the branch main.cpp already committed to before dispatch.
inline bool GdxP2HostConfigured() { return gGdxInterpHostCfg.active; }

// Tick-boundary latch for the referenced-offset set.
//
// gdx_gfx_run -- and therefore GdxInterpBeginTick -- executes ONCE PER GFX TASK, and the game
// submits 2-6 tasks per 60 Hz tick (measured directly: the [interp-geo] census emits 4, 5 and 6
// line groups under a single tick id). Rolling the referenced-offset set inside gdx_gfx_run
// therefore answered "was this offset referenced last tick?" against the PREVIOUS TASK's set:
//
//   task 1  clear -> note {A,B} -> commit          prev={A,B}
//   task 2  clear -> note {C,D} vs prev={A,B}      not present -> SNAP
//   task 3  clear -> note {E}   vs prev={C,D}      not present -> SNAP
//   next tick task 1: note {A,B} vs prev={E}       not present -> SNAP
//
// Slots that should lerp snapped instead, and when every slot in a task snapped the `degenerate`
// check in the sub-frame loop forced t=1 on all M passes -- interpolation rendered M identical
// frames while still paying for them. The host sets this flag at the real tick boundary
// (gdx_gfx_interp_tick_config, once per iteration before dispatch) and the tick's FIRST task
// consumes it. gdx_interp.h's claim that BeginTick runs "EXACTLY ONCE per rendered tick" is what
// this restores; it was not true as written.
bool gGdxInterpNewTick = false;
int gGdxInterpTasksThisTick = 0;
int gGdxInterpLastTasks = 0;
// The cut epoch latched for the WHOLE tick. CutPendingForThisTick() is a consume-once edge, so
// calling it per task lets the first task eat the cut and every later task see false -- half the
// frame snaps and the other half lerps straight across the cut, destroying the whole-frame
// semantic the cut exists for. Latch once at the tick boundary; every task in the tick reads this.
bool gGdxInterpCutThisTick = false;
} // namespace

// In-race pause flag (game.c), read directly the same way gSegments/D_8024DCE0 are. Pause must be
// handled on the interpolation branch: a paused tick forces one crisp t=1 present here while the
// host's logic-deadline pacer holds 60 Hz. Routing it through main.cpp's default path hands pacing
// to gdx_frame_pacer_tick(), a no-op while FrameInterpolation is on, and free-runs the present on
// a VSync-off panel. Read-only.
extern "C" { extern signed char gGamePaused; }

// Implemented in libultraship/src/fast/Fast3dWindow.cpp. Overrides the DXGI software rate limiter's
// verdict while the interpolation sub-frame loop is driving presents; the swapchain's waitable
// object provides the actual pacing. See the block comment at the definition.
extern "C" void gdx_fast3d_set_subframe_present(int on);

// A pending transition background-capture must read a CANONICAL t=1 frame, never a tween.
// TRANSITION_FLAG_SET_BACKGROUND_BUFFER is set in Transition_Update (sys_gfx.c:204) and cleared
// only inside Transition_SetBackgroundBuffer (transition.c:802), which reads our frame mirror at
// sys_gfx.c:219 -- later in the SAME tick than this gdx_gfx_run.
//
// The ordering that makes the read safe (sys_gfx.c:202-219, n64_sched.c:915): the whole game frame
// runs inside one gdx_vi_tick, in program order Transition_Update (SETS flag) -> Transition_Draw
// -> Gfx_FullSync -> the fiber blocks on osRecvMesg(&D_800DCAC8), where the port synchronously
// runs osSpTaskStartGo -> gdx_gfx_run (GdxInterpBeginTick reads the flag here, still set) -> the
// sub-frame loop presents and refreshes the mirror -> DP-done wakes the fiber ->
// Transition_SetBackgroundBuffer clears the flag and reads the just-refreshed mirror.
//
// OR-ing this into mForceCutSnap snaps every scratch slot to t=1, so the sub-frame loop goes
// degenerate and renders every pass at t=1 while pass COUNT stays at M for a constant present
// cadence -- the mirror the capture samples is then the un-interpolated tick.
//
// Layout: flag bit is (1<<0); `flags` is a u16 at struct offset 0x12 (activeType s32, queuedType
// s32, state s32, timer s16, argument s16, appearType u16, flags u16 at natural alignment, per
// transition.h:34-45).
extern "C" { extern unsigned char sTransition[]; }
static inline bool GdxTransitionCapturePendingThisTick() {
    const unsigned short flags =
        *reinterpret_cast<const unsigned short*>(&sTransition[0x12]);
    return (flags & 0x1u /* TRANSITION_FLAG_SET_BACKGROUND_BUFFER */) != 0;
}

// Determinism canary. The game's two LCG states (math.c:185-188) are advanced ONLY by game logic,
// never by the render path -- interpolation reads GfxPools and writes only scratch. So the
// per-tick RNG fingerprint must be identical with interpolation ON and OFF given identical input,
// and the first tick whose fingerprint differs localizes a sub-frame value leaking back into
// logic. Read-only; see GdxInterpDeterminismTick below.
extern "C" {
extern int gRandSeed1;
extern unsigned int gRandMask1;
extern int gRandSeed2;
extern unsigned int gRandMask2;
}

// GfxPool span from the segment-1 base. Gfx_InitBuffer does Segment_SetPhysicalAddress(1, gGfxPool)
// every frame, so gSegments[1] holds the CURRENT pool's host base and a pool matrix resolves to
// gSegments[1] + member-offset.
//
// The span MUST come from gdx_gfxpool_sizeof() (decomp_port.c, the TU with the real GfxPool type),
// never the N64 struct-comment size 0x36730. sizeof(Gfx) doubles on a 64-bit host, inflating
// gfxBuffer[13313] by 0x1A008 to a real pool of 0x50738, and the modelview matrices live past
// 0x36730 — with the stale bound GdxP0MtxInPoolSpan rejects essentially every modelview matrix and
// interpolation has nothing to tween.
extern "C" size_t gdx_gfxpool_sizeof(void);
static inline bool GdxP0MtxInPoolSpan(uintptr_t p) {
    static const size_t kGdxP0GfxPoolSpanBytes = gdx_gfxpool_sizeof();
    const uintptr_t base = static_cast<uintptr_t>(gSegments[1]);
    return base != 0 && p >= base && p < base + kGdxP0GfxPoolSpanBytes;
}

// Same ground-truth rule as the pool span: offsetof from decomp_port.c, never the N64
// struct-comment constant 0x2A308. The host offset is 0x1A008 higher because sizeof(Gfx) doubles,
// and the stale constant aims this test into courseVtxBuffer.
extern "C" size_t gdx_gfxpool_effects_vtx_offset(void);
extern "C" size_t gdx_gfxpool_effects_vtx_bytes(void);

// Per-racer matrix layout (ground truth from decomp_port.c). Lets a rerouted pool offset be mapped
// back to (which of the three per-racer matrix arrays, which racer id) -- see GdxRacerMtxClassify.
extern "C" void gdx_gfxpool_racer_mtx_layout(size_t* outBody, size_t* outSecond, size_t* outHighlight,
                                             size_t* outStride, size_t* outCount);

// --- Camera projection*view rebuild -----------------------------------------------------------
// The one pool matrix that must NOT be lerped element-wise. GfxPool::unk_20208 is the COMBINED
// projection*view (camera.c:1278) and its translation row is -eye*R -- the eye already rotated by
// R, not a position. Lerping it lerps a product of two quantities that both change across the
// tick, so the implied camera position bows off the straight line between the two true eye
// positions by roughly (delta-rotation x |eye|). eye is a raw world coordinate thousands of units
// out, so that lever arm turns a small rotation error into a large mid-tick WORLD translation:
// the scene shifts and snaps back every tick boundary. Interpolate the camera's INPUTS and re-run
// the game's own build instead. Derivation in gdx_interp.h CameraInterpActive; rebuild contract
// in port/gdx_camera_pose.h.
extern "C" void gdx_gfxpool_camera_mtx_layout(size_t* outProjView, size_t* outStride, size_t* outCount);

// One-shot offsetof caching, same as the racer layout below: the N64 struct-comment offsets are
// 0x1A008 low on the host because sizeof(Gfx) doubles under PORT.
struct GdxCameraMtxLayout {
    size_t projView = 0;
    size_t stride = 0;
    size_t count = 0;
    GdxCameraMtxLayout() {
        gdx_gfxpool_camera_mtx_layout(&projView, &stride, &count);
    }
};

static const GdxCameraMtxLayout& GdxCameraMtxLayoutRef() {
    static const GdxCameraMtxLayout k;
    return k;
}

// Pool slot index for a camera projection*view matrix, or -1 for any other pool offset.
static int GdxCameraMtxSlot(uint32_t poolOffset) {
    const GdxCameraMtxLayout& k = GdxCameraMtxLayoutRef();
    if (k.stride == 0 || k.count == 0) {
        return -1;
    }
    if (poolOffset < k.projView || poolOffset >= k.projView + (k.stride * k.count)) {
        return -1;
    }
    const size_t rel = poolOffset - k.projView;
    if ((rel % k.stride) != 0) {
        return -1;
    }
    return static_cast<int>(rel / k.stride);
}

// File scope, NOT adapter members: the adapter is constructed once per GFX TASK and the game
// submits several per 60 Hz tick, so an adapter-owned history would reset mid-tick and never hold
// a previous keyframe. Rolled once per tick from GdxInterpBeginTick's new-tick block.
static GdxCameraPose gGdxCamPoseCur[GDX_CAMERA_POSE_MAX] = {};
static GdxCameraPose gGdxCamPosePrev[GDX_CAMERA_POSE_MAX] = {};
// Per-slot verdict from the t=1 identity check: a pose is usable as an interpolation endpoint ONLY
// if rebuilding from it reproduced the game's own pool matrix byte-for-byte on its capture tick.
// That check is what covers every case where a gCameras snapshot does not describe the matrix
// actually in the pool -- photo mode restores camera->eye/at/fov after the build
// (camera.c:1279-1288), func_i3_8012EE90 clobbers unk_20008/unk_20108 mid-frame (C2160.c:15-18) --
// so a misread surfaces as a mismatch instead of a wrong camera on screen.
static bool gGdxCamPoseCurOk[GDX_CAMERA_POSE_MAX] = {};
static bool gGdxCamPosePrevOk[GDX_CAMERA_POSE_MAX] = {};
static size_t gGdxCamRebuilds = 0;   // per-tick rebuild calls that produced a matrix
static size_t gGdxCamRejects = 0;    // per-tick slots the t=1 identity check refused
// Why a camera slot was NOT rebuilt. Every refusal reason needs its own counter: gGdxCamRejects
// alone counts the memcmp mismatch, which left a third of ticks silently on the element-wise lerp
// while the telemetry read clean.
enum GdxCamWhy {
    kCamWhyPoseRead = 0, // gdx_camera_pose_read refused, or layout/pool base unavailable
    kCamWhyId,           // camera->id disagreed with the slot index
    kCamWhyUnreadable,   // the pool matrix for this slot was not readable
    kCamWhyBuild,        // gdx_camera_build_projview refused (degenerate lookat input)
    kCamWhyMismatch,     // built fine, but did not reproduce the pool matrix at t=1
    kCamWhyPrevPose,     // this tick verified, but the PREVIOUS tick's pose had not
    kCamWhySnap,         // slot already snapping (cut/pause/absent keyframe)
    kCamWhyCount
};
static size_t gGdxCamWhy[kCamWhyCount] = {};
static float gGdxCamMaxEyeDelta = 0.0f; // largest per-tick |eye_cur - eye_prev| among usable pairs

// Camera cut detection must run in POSE space, not matrix space.
//
// A projection matrix's row 3 is P*(-eye*R), a VIEW-SPACE term rather than a world position
// (camera.c:968-973), and eye is a raw world coordinate thousands of units out -- the game's own
// EK branch gates on ABS(projectionViewMtx.m[3][1]) > 30000.0f (camera.c:1260-1262). One degree of
// per-tick yaw moves row 3 by roughly |eye| * dTheta, about 525 units at |eye| ~= 20000, which
// clears kTeleportThreshold (300) and snaps the WHOLE CAMERA to t=1 while model matrices keep
// lerping: new camera, old model poses, the machine sliding across a frozen world and popping back
// at the tick boundary. Steering-correlated, because steering is what rotates the camera.
//
// The EYE POSITION is a world position, so the 300-unit scale means what it says there. Returns
// false for an unverified pair, leaving the slot on the element-wise lerp.
static bool GdxCameraPoseTeleport(uint32_t poolOffset) {
    const int slot = GdxCameraMtxSlot(poolOffset);
    if (slot < 0 || slot >= GDX_CAMERA_POSE_MAX) {
        return false; // not a camera matrix; caller uses the world-space test
    }
    if (!gGdxCamPoseCurOk[slot] || !gGdxCamPosePrevOk[slot]) {
        return false;
    }
    const GdxCameraPose& c = gGdxCamPoseCur[slot];
    const GdxCameraPose& p = gGdxCamPosePrev[slot];
    const float dx = c.eyeX - p.eyeX;
    const float dy = c.eyeY - p.eyeY;
    const float dz = c.eyeZ - p.eyeZ;
    return ((dx * dx) + (dy * dy) + (dz * dz)) >
           (gdx_interp::kTeleportThreshold * gdx_interp::kTeleportThreshold);
}

// Process-lifetime gate for the rebuild path, cached like the other env gates here since an env
// override is a test hook and must not be re-read per call.
//
// Default ON. [screen-probe] projected the player's machine per sub-frame against the
// un-interpolated 60 Hz trajectory; cornering reverses the machine's screen direction ~30% of
// ticks by itself, so what matters is how many reversals interpolation ADDS:
//
//     element-wise lerp   sx reversals 57.9% vs 31.8% natural  ->  +26.1 points
//     pose rebuild        sx reversals 29.3% vs 28.2% natural  ->   +1.1 points
//
// World position is linear across the same ticks (0 reversals in ~2000, step ratio 1.00), so the
// excess came from the camera. Set to "0" to force the element-wise path back for comparison.
static bool GdxCameraRebuildConfigured() {
    static const bool on = [] {
        const char* v = std::getenv("GDX_INTERP_CAMERA_POSE");
        if (v == nullptr || v[0] == '\0') {
            return true;
        }
        return !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

// Every field is a plain scalar in world/degree space, so a linear blend is correct for all of
// them -- including up, because Matrix_SetLookAt re-derives a true orthonormal basis from
// (eye, at, up) via side = up x forward then trueUp = forward x side (math.c:1022-1048), so a
// blended up that is no longer perpendicular is corrected by the builder. No quaternion slerp
// needed. numPlayers/id come from the CURRENT pose: they gate control flow, not geometry, and the
// caller already refuses a pair that disagrees on them.
static void GdxCameraPoseLerp(const GdxCameraPose& a, const GdxCameraPose& b, float t, GdxCameraPose* out) {
    const float u = 1.0f - t;
    out->eyeX = (a.eyeX * u) + (b.eyeX * t);
    out->eyeY = (a.eyeY * u) + (b.eyeY * t);
    out->eyeZ = (a.eyeZ * u) + (b.eyeZ * t);
    out->atX = (a.atX * u) + (b.atX * t);
    out->atY = (a.atY * u) + (b.atY * t);
    out->atZ = (a.atZ * u) + (b.atZ * t);
    out->upX = (a.upX * u) + (b.upX * t);
    out->upY = (a.upY * u) + (b.upY * t);
    out->upZ = (a.upZ * u) + (b.upZ * t);
    out->fov = (a.fov * u) + (b.fov * t);
    out->nearZ = (a.nearZ * u) + (b.nearZ * t);
    out->farZ = (a.farZ * u) + (b.farZ * t);
    out->fovScaleX = (a.fovScaleX * u) + (b.fovScaleX * t);
    out->fovScaleY = (a.fovScaleY * u) + (b.fovScaleY * t);
    out->frustrumCenterX = (a.frustrumCenterX * u) + (b.frustrumCenterX * t);
    out->frustrumCenterY = (a.frustrumCenterY * u) + (b.frustrumCenterY * t);
    // Interpolate the RESOLVED fov, not the raw one, and keep the resolved flag set so the builder
    // skips the threshold. Both endpoints were resolved at their own tick, so the value moves
    // smoothly across the tick instead of the branch snapping on and off inside it.
    out->resolvedFov = (a.resolvedFov * u) + (b.resolvedFov * t);
    out->fovIsResolved = (a.fovIsResolved != 0 && b.fovIsResolved != 0) ? 1 : 0;
    out->numPlayers = b.numPlayers;
    out->id = b.id;
    out->valid = 1;
}

// --- Side-attack model-basis discontinuity ----------------------------------------------------
// racer.c:4556 hard-assigns modelBasis.x = trueBasis.x for the duration of a SIDE attack,
// racer.c:4621-4638 re-derives the rest of the basis from it, racer.c:5985 builds the body matrix.
// So the body matrix's ROTATION jumps in one tick, twice per attack, and lerping across that jump
// draws the machine at an orientation it never held -- the side-attack afterimage. Spin attacks
// enter and leave continuously (COS(0)=1, SIN(0)=0) and have no jump, matching the side-yes/spin-no
// split seen in play. Predicate: gdx_racer_side_attack_active in decomp_port.c.
extern "C" int gdx_racer_side_attack_count(void);
extern "C" int gdx_racer_side_attack_active(int index);

static constexpr uint32_t kGdxSideAttackSlots = 30; // == kGdxRacerMtxSlots (TOTAL_RACER_COUNT)
static uint8_t gGdxSideAttackCur[kGdxSideAttackSlots] = {};
static uint8_t gGdxSideAttackPrev[kGdxSideAttackSlots] = {};
// Set when the predicate went 1 -> 0 last tick. The EXIT discontinuity is off by one: on the final
// attack tick racer.c:4555 still sees unk_27C != 0 and performs the hard assignment, and only
// afterwards does racer.c:4569-4574 clear unk_27C/attackState -- so the snapshot reads 0 on a tick
// whose matrix is still the attacked one, and the basis reverts on the FOLLOWING tick. Entry has
// no such skew, which is why only this direction is deferred.
static uint8_t gGdxSideAttackExitPending[kGdxSideAttackSlots] = {};
// This tick's verdict: the racer's model basis is discontinuous between the previous pool's matrix
// and the current one, so the previous keyframe's ROTATION is meaningless for it.
static uint8_t gGdxSideAttackJump[kGdxSideAttackSlots] = {};
static size_t gGdxBasisJumpFixed = 0;
// [basis-probe] ticks still to report after an attack ends, so the deferred exit jump is captured.
static int gGdxBasisProbeTail = 0;

// Restore rigidity to an element-wise-lerped per-racer model matrix.
//
// lerp(R0, R1, t) is not a rotation: the lerp of two unit vectors is SHORTER than unit, so basis
// rows collapse mid-tick and recover at t=1 -- up to an 18% shrink with 18% row divergence at
// t=0.5, against 0.00000 spread at t=1. The machine squashes for a sub-frame and pops back,
// invisible at the model origin and growing across the silhouette.
//
// Rescale each lerped row to the interpolation of the two endpoint row LENGTHS, not to unit, so
// genuine per-tick scale animation (attack highlight growth, machineLod switches) survives.
// Cross-product re-derivation would also fix the residual shear, but the shrink is the dominant
// term and this keeps the operation to what the measurement justifies.
static void GdxRenormalizeLerpedBasis(const void* prevMtx, const void* curMtx, float t, GdxP0Mtx* scratch) {
    float prv[4][4];
    float cur[4][4];
    float out[4][4];
    gdx_interp::MtxToF(prevMtx, prv);
    gdx_interp::MtxToF(curMtx, cur);
    gdx_interp::MtxToF(scratch, out);
    for (int r = 0; r < 3; ++r) {
        const float lp = std::sqrt((prv[r][0] * prv[r][0]) + (prv[r][1] * prv[r][1]) + (prv[r][2] * prv[r][2]));
        const float lc = std::sqrt((cur[r][0] * cur[r][0]) + (cur[r][1] * cur[r][1]) + (cur[r][2] * cur[r][2]));
        const float have = std::sqrt((out[r][0] * out[r][0]) + (out[r][1] * out[r][1]) + (out[r][2] * out[r][2]));
        if (have <= 1e-6f) {
            continue; // degenerate row; leave it rather than divide by ~0
        }
        const float want = (lp * (1.0f - t)) + (lc * t);
        const float k = want / have;
        out[r][0] *= k;
        out[r][1] *= k;
        out[r][2] *= k;
    }
    gdx_interp::MtxFromF(out, scratch);
}

// Read the A/B gate for deciding the EK fov threshold once per tick. Default ON (the 20px vertical
// snap it removes was measured before and after), but switchable so a regression can be attributed.
static bool GdxFovResolveConfigured() {
    static const bool on = [] {
        const char* v = std::getenv("GDX_INTERP_FOV_RESOLVE");
        if (v == nullptr || v[0] == '\0') {
            return true;
        }
        return !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

// A/B gates for the lerped-basis renormalisation and the basis-jump fixup. Resolved in
// gdx_interp.cpp (RigidBasisActive/BasisJumpFixActive) where CVarGetInteger links: the env var is
// a force-on pin, the registered CVar carries the shipping default. Reading getenv alone here
// would make both fixes unreachable to users.
static bool GdxRotFixConfigured() {
    return gdx_interp::RigidBasisActive();
}

static bool GdxBasisJumpFixConfigured() {
    return gdx_interp::BasisJumpFixActive();
}

// Which per-racer matrix a pool offset belongs to. kNone for anything else in the pool (camera,
// course, HUD, menu matrices) -- those are not per-racer and have no co-moving sibling.
enum class GdxRacerMtxField { kNone, kBody, kSecond, kHighlight };

struct GdxRacerMtxId {
    GdxRacerMtxField field = GdxRacerMtxField::kNone;
    uint32_t racer = 0;
};

static GdxRacerMtxId GdxRacerMtxClassify(uint32_t poolOffset) {
    struct Layout {
        size_t body, second, highlight, stride, count;
        Layout() {
            gdx_gfxpool_racer_mtx_layout(&body, &second, &highlight, &stride, &count);
        }
    };
    static const Layout k;
    GdxRacerMtxId out;
    if (k.stride == 0) {
        return out;
    }
    const size_t span = k.stride * k.count;
    const struct {
        size_t base;
        GdxRacerMtxField field;
    } arrays[] = { { k.body, GdxRacerMtxField::kBody },
                   { k.second, GdxRacerMtxField::kSecond },
                   { k.highlight, GdxRacerMtxField::kHighlight } };
    for (const auto& a : arrays) {
        if (poolOffset >= a.base && poolOffset < a.base + span) {
            const size_t rel = poolOffset - a.base;
            if ((rel % k.stride) == 0) {
                out.field = a.field;
                out.racer = static_cast<uint32_t>(rel / k.stride);
            }
            return out;
        }
    }
    return out;
}
static inline bool GdxEffectsVtxInSpan(uintptr_t p, size_t bytes) {
    static const size_t kOffset = gdx_gfxpool_effects_vtx_offset();
    static const size_t kBytes = gdx_gfxpool_effects_vtx_bytes();
    const uintptr_t base = static_cast<uintptr_t>(gSegments[1]);
    if (base == 0) {
        return false;
    }
    const uintptr_t lo = base + kOffset;
    return p >= lo && (p + bytes) <= (lo + kBytes);
}

// Course-select carousel viewports (course_view.c: Vp D_i5_80118FF0[2][6], first index is the
// D_800DCCFC parity). Overlays are statically linked, so naming the symbol directly is fine.
// Declared as raw s16 lanes rather than the decomp Vp union to keep this TU out of the decomp
// include tree; sizeof(Vp)==16, layout vscale[4] then vtrans[4], asserted below.
extern "C" int16_t D_i5_80118FF0[2][6][8];
static_assert(sizeof(D_i5_80118FF0) == 2 * 6 * 16, "carousel viewport array shape");

static inline void GdxP0FnvAccum(uint64_t& h, uint64_t word) {
    h ^= word;
    h *= 0x100000001B3ull;
}

/* Segment-9 fallback probe, strip later. A seg9 token that misses the authoritative
 * gdx_resolve_mode_segment9 falls through ~10 generic resolver branches, any of which can serve it
 * by accident -- nearby seg-9 addresses landing in different host buffers is the [nodeinfo]
 * scatter. Declared once after the mode miss, this reads whatever `out` holds on ANY exit path via
 * the destructor, so no log line has to be added to a hot-path branch. GDX_LOG-gated, first 24
 * misses per process. */
class GdxSeg9FallbackDiag {
  public:
    GdxSeg9FallbackDiag(bool armed, uint32_t raw, size_t requiredBytes, const ResolvedAddress* out)
        : mArmed(armed), mRaw(raw), mRequiredBytes(requiredBytes), mOut(out) {}
    ~GdxSeg9FallbackDiag() {
        if (!mArmed) {
            return;
        }
        static int sLogs = 0;
        if (sLogs >= 24) {
            return;
        }
        ++sLogs;
        if (mOut->full != 0) {
            gdx_port_logf("[seg9diag] fallback served seg9 token raw=%08X req=%zu -> full=%p segment=%u "
                          "offset=%08X segmented=%d\n",
                          mRaw, mRequiredBytes, reinterpret_cast<void*>(mOut->full),
                          static_cast<unsigned>(mOut->segment), mOut->offset, static_cast<int>(mOut->segmented));
        } else {
            gdx_port_logf("[seg9diag] seg9 token raw=%08X req=%zu UNRESOLVED (mode resolver + all fallbacks "
                          "missed)\n",
                          mRaw, mRequiredBytes);
        }
    }
    GdxSeg9FallbackDiag(const GdxSeg9FallbackDiag&) = delete;
    GdxSeg9FallbackDiag& operator=(const GdxSeg9FallbackDiag&) = delete;

  private:
    bool mArmed;
    uint32_t mRaw;
    size_t mRequiredBytes;
    const ResolvedAddress* mOut;
};

class N64DisplayListAdapter {
  public:
    struct ConvertedList {
        std::vector<Fast::F3DGfx> commands;
    };

    struct QueueItem {
        const N64Gfx* source;
        size_t limit;
        ConvertedList* listPtr;
    };

    N64DisplayListAdapter(const void* root, size_t rootSizeBytes, bool isBig, ConversionStats* stats = nullptr)
        : mRootBegin(static_cast<const N64Gfx*>(root)),
          mRootByteEnd(reinterpret_cast<uintptr_t>(root) + rootSizeBytes),
          mIsBig(isBig),
          mStats(stats) {
        GetMainModuleRange(mModuleBegin, mModuleEnd);
    }

    uintptr_t EnqueueList(const N64Gfx* source, size_t explicitLimit) {
        // Lazy conversion boundary: a narrow N64-format source becomes a cached wide buffer so
        // ProcessList takes the resolver-free fast path. No-op for already-wide sources.
        source = GetOrBuildConvertedWide(source, explicitLimit);

        auto cached = mLists.find(source);
        if (cached != mLists.end()) return reinterpret_cast<uintptr_t>(cached->second->commands.data());

        auto list = std::make_unique<ConvertedList>();
        ConvertedList* listPtr = list.get();
        mLists.emplace(source, std::move(list));
        if (mStats != nullptr) mStats->convertedLists++;

        const size_t limit = EffectiveLimit(source, explicitLimit);
        listPtr->commands.reserve(limit + 1);

        mWorkQueue.push_back({source, limit, listPtr});

        return reinterpret_cast<uintptr_t>(listPtr->commands.data());
    }

    // --- P0/P1 scratch-slot indirection + evidence API (no-ops unless mInterpEnabled) ---

    bool GdxP0Enabled() const { return mP0Enabled; }
    bool GdxP1Enabled() const { return mP1Enabled; }
    // True when the host-driven sub-frame present loop owns this tick (reuses P1 lerp).
    bool GdxP2HostActive() const { return mP2Host; }
    // t used for the presented (2nd) replay pass in P1 (0.5 for "mid"/"half").
    float GdxInterpPresentT() const { return gdx_interp::P1().presentT; }

    // Snap-event counters (per tick), surfaced by the [interp-p1] evidence line.
    size_t GdxP1Lerped() const { return mP1Lerped; }
    size_t GdxP1SnappedAbsent() const { return mP1SnappedAbsent; }
    size_t GdxP1SnappedTeleport() const { return mP1SnappedTeleport; }
    size_t GdxP1SnappedCut() const { return mP1SnappedCut; } // P3: whole-frame cut/pause snaps
    float GdxP1PairMaxDelta() const { return mP1PairMaxDelta; }
    size_t GdxP1PairSuspect() const { return mP1PairSuspect; }
    size_t GdxP1PoolBaseMisses() const { return mP1PoolBaseMisses; }
    // True iff this tick's whole-frame snap was armed by a pending transition capture.
    bool GdxCaptureSnapThisTick() const { return mCaptureSnapThisTick; }

    // Begin dual-pool tracking for this tick: latch the current/previous GfxPool bases from
    // gSegments[1] and reset the referenced-offset set.
    // Called once per gdx_gfx_run, before ConvertRoot drains the reroutes. No-op unless P1.
    void GdxInterpBeginTick() {
        mCurPoolBase = 0;
        mPrevPoolBase = 0;
        mP1Lerped = 0;
        mP1SnappedAbsent = 0;
        mP1SnappedTeleport = 0;
        mP1SnappedCut = 0;
        mP1PairMaxDelta = 0.0f;
        mP1PairSuspect = 0;
        mP1PoolBaseMisses = 0;
        mForceCutSnap = false;
        mCaptureSnapThisTick = false;

        // ---- PER-TICK work, performed by the tick's FIRST task ----
        // Must run BEFORE the mP1Enabled bail-out so the cut epoch is consumed exactly once per
        // tick even when lerping is inactive; otherwise an epoch bump on a non-interp tick is
        // double-counted against the next interp tick. The referenced-offset set rolls here for a
        // related reason: committing at the START of the next tick (rather than after the final
        // task) means nothing has to identify which task is last -- every task accumulates into one
        // set and tests against a complete previous tick.
        ++gGdxInterpTasksThisTick;
        // Latched because the camera pose roll below needs "is this the tick's first task?" AFTER
        // the pool bases exist, and the flag itself is consumed here.
        bool newTick = false;
        if (gGdxInterpNewTick) {
            newTick = true;
            gGdxInterpNewTick = false;
            gdx_interp::CommitTick();
            gdx_interp::BeginTick();
            gGdxInterpCutThisTick = gdx_interp::CutPendingForThisTick();
        }

        // Also before the mP1Enabled bail-out: this history is an edge detector and must advance
        // exactly once per tick whether or not lerping is active. Rolling it after the early return
        // burns `newTick` without advancing, leaving prev/cur comparing non-adjacent ticks and
        // silently dropping transitions. Reads only gRacers, never the pool, so it is safe here.
        if (newTick) {
            GdxSideAttackRoll();
        }

        if (!mP1Enabled) {
            return;
        }
        mCurPoolBase = static_cast<uintptr_t>(gSegments[1]);
        mPrevPoolBase = gdx_interp::PrevPoolBase(mCurPoolBase); // 0 if pool layout mismatch
        // A cut, a pause, or a pending transition capture each makes the previous keyframe
        // meaningless for the WHOLE frame, so any of them forces every slot to snap to t=1: a cut
        // invalidates all prev keyframes, a pause has nothing new to tween, and the capture's
        // mirror must be un-interpolated (see GdxTransitionCapturePendingThisTick's ordering).
        mCaptureSnapThisTick = GdxTransitionCapturePendingThisTick();
        mForceCutSnap = gGdxInterpCutThisTick || (gGamePaused != 0) || mCaptureSnapThisTick;
        if (newTick) {
            GdxCameraPoseRoll();
        }
    }

    // Roll the side-attack predicate one tick and decide, per racer, whether the model basis is
    // discontinuous across THIS pool pair. Runs once per tick from the tick's first task.
    //
    // Entry and exit are not symmetric. On the first attack tick the predicate and the hard
    // assignment turn on together, so a 0 -> 1 edge marks the jump directly. On the LAST attack
    // tick the assignment still runs (racer.c:4555 sees unk_27C != 0) and racer.c:4569-4574 clears
    // the state only afterwards, so the 1 -> 0 edge lands on a tick whose matrix is still the
    // attacked one and the basis reverts on the following tick -- where the deferred flag fires.
    //
    // NOT gated on mBasisJumpFix: this is an edge detector AND the source of the attack flag the
    // probes report, so it must advance every tick regardless of whether the repair is on. Gating
    // it made [screen-probe] read atk=0 for a whole session. The repair itself stays gated, in
    // GdxFixupBasisJumpMatrices.
    void GdxSideAttackRoll() {
        gGdxBasisJumpFixed = 0;

        int count = gdx_racer_side_attack_count();
        if (count > static_cast<int>(kGdxSideAttackSlots)) {
            count = static_cast<int>(kGdxSideAttackSlots);
        }
        for (int i = 0; i < count; ++i) {
            gGdxSideAttackPrev[i] = gGdxSideAttackCur[i];
            gGdxSideAttackCur[i] = (gdx_racer_side_attack_active(i) != 0) ? 1u : 0u;

            const bool entry = (gGdxSideAttackPrev[i] == 0 && gGdxSideAttackCur[i] == 1);
            const bool exitNow = (gGdxSideAttackExitPending[i] != 0);
            gGdxSideAttackExitPending[i] =
                (gGdxSideAttackPrev[i] == 1 && gGdxSideAttackCur[i] == 0) ? 1u : 0u;
            gGdxSideAttackJump[i] = (entry || exitNow) ? 1u : 0u;
        }
    }

    // [basis-probe] GDX_DIAG_BASIS=1, strip later. Measures the player's body matrix (unk_20308[0])
    // per tick through every side attack, BEFORE any fixup rewrites r.prev. Rotation and
    // translation are reported separately because racer.c:4546-4548 makes modelPos depend on
    // modelBasis.y, so the basis jump moves the machine as well as turning it, and the current
    // fixup only addresses rotation.
    void GdxBasisProbeTick() {
        static const bool sBasisProbe = std::getenv("GDX_DIAG_BASIS") != nullptr;
        if (!sBasisProbe || !mP1Enabled || mCurPoolBase == 0) {
            return;
        }
        const bool active = (gGdxSideAttackCur[0] != 0);
        const bool jump = (gGdxSideAttackJump[0] != 0);
        if (!active && !jump && gGdxBasisProbeTail == 0) {
            return; // quiet outside an attack and its trailing window
        }
        if (active || jump) {
            gGdxBasisProbeTail = 4; // keep logging past the exit so the deferred jump is visible
        } else {
            --gGdxBasisProbeTail;
        }

        // Per-racer-0 matrices, indexed body / second(shadow) / highlight.
        float rot[3] = { -1.0f, -1.0f, -1.0f };
        float tr[3] = { -1.0f, -1.0f, -1.0f };
        // Worst paired translation delta anywhere in the pool this tick. The machine travels ~33
        // units per tick, so anything far above that is a slot whose two keyframes are not the same
        // object -- byte-offset identity pairing unrelated draws, which renders one object at
        // another's previous position. The offset and classification say WHAT is ghosting.
        float worstDelta = 0.0f;
        uint32_t worstOffset = 0;
        int worstField = -1;
        uint32_t worstRacer = 0;

        for (const GdxP0Record& r : mP0Records) {
            if (r.prev == 0 || ReadableByteLimit(r.prev) < 64u || ReadableByteLimit(r.orig) < 64u) {
                continue;
            }
            const uint32_t off = static_cast<uint32_t>(r.orig - mCurPoolBase);
            float cur[4][4];
            float prv[4][4];
            gdx_interp::MtxToF(reinterpret_cast<const void*>(r.orig), cur);
            gdx_interp::MtxToF(reinterpret_cast<const void*>(r.prev), prv);

            const float dx = cur[3][0] - prv[3][0];
            const float dy = cur[3][1] - prv[3][1];
            const float dz = cur[3][2] - prv[3][2];
            const float delta = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));

            const GdxRacerMtxId id = GdxRacerMtxClassify(off);
            // Projection matrices are excluded: their row 3 is a view-space term, not a world
            // position, so a large "delta" there means nothing. Same reason GdxP0Record carries proj.
            if (!r.proj && delta > worstDelta) {
                worstDelta = delta;
                worstOffset = off;
                worstField = static_cast<int>(id.field);
                worstRacer = id.racer;
            }

            if (id.field == GdxRacerMtxField::kNone || id.racer != 0) {
                continue;
            }
            const int col = (id.field == GdxRacerMtxField::kBody)     ? 0
                            : (id.field == GdxRacerMtxField::kSecond) ? 1
                                                                      : 2;
            float rotMax = 0.0f;
            for (int row = 0; row < 3; ++row) {
                for (int c = 0; c < 3; ++c) {
                    const float d = std::fabs(cur[row][c] - prv[row][c]);
                    if (d > rotMax) {
                        rotMax = d;
                    }
                }
            }
            rot[col] = rotMax;
            tr[col] = delta;
        }

        // rot is the largest absolute change in any 3x3 basis element -- not an angle, a spike
        // detector. field: 0 none, 1 body, 2 second/shadow, 3 highlight.
        gdx_port_logf("[basis-probe] active=%d jump=%d "
                      "body=%.4f/%.2f shadow=%.4f/%.2f hl=%.4f/%.2f "
                      "worst=%.2f@%05X field=%d racer=%u\n",
                      active ? 1 : 0, jump ? 1 : 0,
                      rot[0], tr[0], rot[1], tr[1], rot[2], tr[2],
                      worstDelta, worstOffset, worstField, worstRacer);
    }

    // [screen-probe] GDX_DIAG_SCREEN=1, strip later. Projects the player's machine through the
    // camera once per sub-frame, in pixels. Screen position is the product of two independently
    // interpolated things -- the body matrix and the camera's projection*view -- either of which
    // can be smooth alone while the product is not. Across one tick's sub-frames the machine must
    // sweep monotonically in roughly equal steps; a zig-zag or an outsized step is the artifact.
    // Called after each sub-frame's refill, so it reads exactly the matrices about to be drawn.
    void GdxScreenProbe(float t) {
        static const bool sScreenProbe = std::getenv("GDX_DIAG_SCREEN") != nullptr;
        if (!sScreenProbe || !mP1Enabled || mCurPoolBase == 0) {
            return;
        }
        const GdxP0Mtx* body = nullptr;
        const GdxP0Mtx* cam = nullptr;
        for (const GdxP0Record& r : mP0Records) {
            const uint32_t off = static_cast<uint32_t>(r.orig - mCurPoolBase);
            if (body == nullptr) {
                const GdxRacerMtxId id = GdxRacerMtxClassify(off);
                if (id.field == GdxRacerMtxField::kBody && id.racer == 0) {
                    body = r.scratch;
                }
            }
            if (cam == nullptr && r.proj && GdxCameraMtxSlot(off) == 0) {
                cam = r.scratch;
            }
            if (body != nullptr && cam != nullptr) {
                break;
            }
        }
        if (body == nullptr || cam == nullptr) {
            return;
        }
        float b[4][4];
        float pv[4][4];
        gdx_interp::MtxToF(body, b);
        gdx_interp::MtxToF(cam, pv);
        // Row 3 of the modelview is the machine's world position; row-vector convention, so the
        // clip-space point is worldPos * projectionView (same order Camera_CalculateProjectionViewMtx
        // builds for, camera.c:949).
        // Also project a point 100 units out along the model's own forward axis. The origin alone
        // cannot see a ROTATION error: lerping a rotation element-wise yields a non-orthonormal
        // matrix whose error is exactly zero at the origin and grows with distance from it. The
        // machine's visible extent is what the player sees, so an offset point is the probe that
        // can detect a squashed or sheared model while the centre sits perfectly still. Steering is
        // when the model's rotation changes fastest, which is when this should light up.
        float tipClip[4];
        const float tipLocal[3] = { 100.0f, 0.0f, 0.0f };
        for (int c = 0; c < 4; ++c) {
            const float wxp = (tipLocal[0] * b[0][0]) + (tipLocal[1] * b[1][0]) + (tipLocal[2] * b[2][0]) + b[3][0];
            const float wyp = (tipLocal[0] * b[0][1]) + (tipLocal[1] * b[1][1]) + (tipLocal[2] * b[2][1]) + b[3][1];
            const float wzp = (tipLocal[0] * b[0][2]) + (tipLocal[1] * b[1][2]) + (tipLocal[2] * b[2][2]) + b[3][2];
            tipClip[c] = (wxp * pv[0][c]) + (wyp * pv[1][c]) + (wzp * pv[2][c]) + pv[3][c];
        }
        // Orthonormality of the lerped model basis: |row| should be the model scale on every row,
        // and equal across rows. Element-wise lerp shrinks rows mid-tick; report the spread.
        float rowLen[3];
        for (int r = 0; r < 3; ++r) {
            rowLen[r] = std::sqrt((b[r][0] * b[r][0]) + (b[r][1] * b[r][1]) + (b[r][2] * b[r][2]));
        }
        const float lenMin = std::min(rowLen[0], std::min(rowLen[1], rowLen[2]));
        const float lenMax = std::max(rowLen[0], std::max(rowLen[1], rowLen[2]));
        float clip[4];
        for (int c = 0; c < 4; ++c) {
            clip[c] = (b[3][0] * pv[0][c]) + (b[3][1] * pv[1][c]) + (b[3][2] * pv[2][c]) + pv[3][c];
        }
        if (clip[3] <= 0.0001f && clip[3] >= -0.0001f) {
            return; // behind/at the eye plane; the divide would be meaningless
        }
        const float sx = (clip[0] / clip[3]) * 160.0f + 160.0f; // N64 320x240 screen space
        const float sy = (clip[1] / clip[3]) * -120.0f + 120.0f;
        // Camera eye at this same sub-frame, from the pose history. With both the machine's world
        // position and the camera's eye logged beside the resulting screen position, a marked
        // instant says which of the two moved -- no inference required.
        float ex = 0.0f, ey = 0.0f, ez = 0.0f;
        int camOk = 0;
        if (gGdxCamPoseCurOk[0] && gGdxCamPosePrevOk[0]) {
            GdxCameraPose mid;
            GdxCameraPoseLerp(gGdxCamPosePrev[0], gGdxCamPoseCur[0], t, &mid);
            ex = mid.eyeX; ey = mid.eyeY; ez = mid.eyeZ;
            camOk = 1;
        }
        float tx = -1.0f, ty = -1.0f;
        if (tipClip[3] > 0.0001f || tipClip[3] < -0.0001f) {
            tx = (tipClip[0] / tipClip[3]) * 160.0f + 160.0f;
            ty = (tipClip[1] / tipClip[3]) * -120.0f + 120.0f;
        }
        gdx_port_logf("[screen-probe] t=%.3f sx=%.2f sy=%.2f tip=(%.2f,%.2f) "
                      "rowlen=%.4f/%.4f world=(%.1f,%.1f,%.1f) "
                      "eye=(%.1f,%.1f,%.1f) camok=%d atk=%d\n",
                      static_cast<double>(t), static_cast<double>(sx), static_cast<double>(sy),
                      static_cast<double>(tx), static_cast<double>(ty),
                      static_cast<double>(lenMin), static_cast<double>(lenMax),
                      static_cast<double>(b[3][0]), static_cast<double>(b[3][1]),
                      static_cast<double>(b[3][2]),
                      static_cast<double>(ex), static_cast<double>(ey), static_cast<double>(ez),
                      camOk, (gGdxSideAttackCur[0] != 0) ? 1 : 0);
    }

    // Freeze the ROTATION and keep lerping the TRANSLATION for every per-racer matrix whose basis
    // jumped this tick. Synthesise a previous keyframe that is the current matrix with the previous
    // matrix's translation row: the machine still travels smoothly between the two tick positions,
    // but stops sweeping through an orientation it never occupied.
    //
    // Deliberately NOT a snap: snapping these slots freezes the machine for a whole tick at both
    // transitions (a visible hitch) and also catches spin attacks, which have no jump to hide.
    // Same shape as GdxFixupSpawnedRacerMatrices, generalised from "no previous keyframe" to
    // "previous keyframe's rotation is meaningless".
    void GdxFixupBasisJumpMatrices() {
        if (!mBasisJumpFix || !mP1Enabled || mCurPoolBase == 0 || mPrevPoolBase == 0 || mForceCutSnap) {
            return;
        }
        for (GdxP0Record& r : mP0Records) {
            if (r.snap || r.prev == 0) {
                continue; // nothing paired to correct
            }
            const GdxRacerMtxId id = GdxRacerMtxClassify(static_cast<uint32_t>(r.orig - mCurPoolBase));
            if (id.field == GdxRacerMtxField::kNone || id.racer >= kGdxSideAttackSlots) {
                continue;
            }
            if (gGdxSideAttackJump[id.racer] == 0) {
                continue;
            }
            if (ReadableByteLimit(r.orig) < 64u || ReadableByteLimit(r.prev) < 64u) {
                continue;
            }
            float cur[4][4];
            float prv[4][4];
            gdx_interp::MtxToF(reinterpret_cast<const void*>(r.orig), cur);
            gdx_interp::MtxToF(reinterpret_cast<const void*>(r.prev), prv);
            // Row 3 is the translation (same convention GdxFixupSpawnedRacerMatrices relies on).
            // Take the previous position, keep the current orientation.
            for (int axis = 0; axis < 3; ++axis) {
                cur[3][axis] = prv[3][axis];
            }
            mSynthPrev.emplace_back();
            GdxP0Mtx* synth = &mSynthPrev.back();
            gdx_interp::MtxFromF(cur, synth);
            r.prev = reinterpret_cast<uintptr_t>(synth);
            ++gGdxBasisJumpFixed;
        }
    }

    // Roll the camera pose history one tick and re-verify the fresh poses. Runs once per tick, from
    // the tick's first task, AFTER the pool bases are latched (the verification reads the pool).
    //
    // The verification is the whole safety argument. gCameras is snapshotted one step removed from
    // Camera_UpdateProjectionViewMtx, so nothing guarantees a priori that rebuilding from the
    // snapshot reproduces the matrix the game left in the pool. Rather than enumerate the ways it
    // could disagree: rebuild at the captured pose and memcmp against the pool -- same inputs, same
    // builder, same process, so a match is bit-exact and a mismatch disqualifies the slot for the
    // tick, falling back to the element-wise lerp.
    void GdxCameraPoseRoll() {
        if (!mInterpCameraRebuild) {
            return;
        }
        gGdxCamRebuilds = 0;
        gGdxCamRejects = 0;
        gGdxCamMaxEyeDelta = 0.0f;
        for (size_t w = 0; w < kCamWhyCount; ++w) {
            gGdxCamWhy[w] = 0;
        }

        const GdxCameraMtxLayout& layout = GdxCameraMtxLayoutRef();
        int count = gdx_camera_pose_count();
        if (count > GDX_CAMERA_POSE_MAX) {
            count = GDX_CAMERA_POSE_MAX;
        }
        for (int i = 0; i < count; ++i) {
            gGdxCamPosePrev[i] = gGdxCamPoseCur[i];
            gGdxCamPosePrevOk[i] = gGdxCamPoseCurOk[i];
            gGdxCamPoseCurOk[i] = false;

            if (gdx_camera_pose_read(i, &gGdxCamPoseCur[i]) == 0 || layout.stride == 0 ||
                mCurPoolBase == 0) {
                ++gGdxCamWhy[kCamWhyPoseRead];
                continue;
            }
            // gCameras[N].id == N is the game's own invariant (sole assignment camera.c:2177), and
            // the id is what indexes the pool slot, so disagreement means this pose cannot be
            // matched back to a matrix and must not be used.
            // Resolve the EK fov threshold once per tick, before anything interpolates: from here
            // on only the RESULT moves and the 30000 cutoff is never re-tested inside a tick.
            // Gated so a suspected regression can be attributed.
            if (GdxFovResolveConfigured()) {
                gdx_camera_resolve_fov(&gGdxCamPoseCur[i]);
            } else {
                gGdxCamPoseCur[i].fovIsResolved = 0;
                gGdxCamPoseCur[i].resolvedFov = gGdxCamPoseCur[i].fov;
            }
            const int id = gGdxCamPoseCur[i].id;
            if (id != i || id < 0 || static_cast<size_t>(id) >= layout.count) {
                ++gGdxCamWhy[kCamWhyId];
                continue;
            }
            const uintptr_t poolPtr = mCurPoolBase + layout.projView + (static_cast<size_t>(id) * layout.stride);
            if (ReadableByteLimit(poolPtr) < 64u) {
                ++gGdxCamWhy[kCamWhyUnreadable];
                continue;
            }
            GdxP0Mtx rebuilt = {};
            if (gdx_camera_build_projview(&gGdxCamPoseCur[i], &rebuilt) == 0) {
                ++gGdxCamWhy[kCamWhyBuild];
                continue; // degenerate lookat input; the builder refused rather than half-write
            }
            if (std::memcmp(&rebuilt, reinterpret_cast<const void*>(poolPtr), 64) == 0) {
                gGdxCamPoseCurOk[i] = true;
                if (gGdxCamPosePrevOk[i]) {
                    const float dx = gGdxCamPoseCur[i].eyeX - gGdxCamPosePrev[i].eyeX;
                    const float dy = gGdxCamPoseCur[i].eyeY - gGdxCamPosePrev[i].eyeY;
                    const float dz = gGdxCamPoseCur[i].eyeZ - gGdxCamPosePrev[i].eyeZ;
                    const float delta = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
                    if (delta > gGdxCamMaxEyeDelta) {
                        gGdxCamMaxEyeDelta = delta;
                    }
                }
            } else {
                ++gGdxCamRejects;
                ++gGdxCamWhy[kCamWhyMismatch];
            }
        }
    }

    // Rebuild one camera matrix at sub-frame time t. Returns false to fall through to the
    // element-wise path; every refusal below is a case where the rebuild is not provably better
    // than the lerp. Takes the record's fields rather than the record because GdxP0Record is
    // declared further down the class and is not nameable in a member signature this early.
    bool GdxCameraRebuildSlot(uintptr_t orig, uintptr_t recPrev, bool recSnap, GdxP0Mtx* scratch, float t) {
        const int slot = GdxCameraMtxSlot(static_cast<uint32_t>(orig - mCurPoolBase));
        if (slot < 0 || slot >= GDX_CAMERA_POSE_MAX) {
            return false;
        }
        // Both endpoints must have passed their own tick's identity check. Interpolating from an
        // unverified keyframe would reintroduce exactly the class of error this replaces.
        if (!gGdxCamPoseCurOk[slot] || !gGdxCamPosePrevOk[slot]) {
            ++gGdxCamWhy[kCamWhyPrevPose];
            return false;
        }
        // A slot the existing machinery already decided to snap (cut epoch, pause, transition
        // capture, absent keyframe) has no meaningful previous pose either.
        if (recSnap || recPrev == 0 || mForceCutSnap) {
            ++gGdxCamWhy[kCamWhySnap];
            return false;
        }
        const GdxCameraPose& cur = gGdxCamPoseCur[slot];
        const GdxCameraPose& prev = gGdxCamPosePrev[slot];
        if (cur.id != slot || prev.id != slot) {
            return false;
        }
        // numPlayers gates the EK fov-widening branch. A change across the pair means the two
        // endpoints were built by different control flow, so blending them is not defined.
        if (cur.numPlayers != prev.numPlayers) {
            return false;
        }
        GdxCameraPose mid;
        GdxCameraPoseLerp(prev, cur, t, &mid);
        if (gdx_camera_build_projview(&mid, scratch) == 0) {
            return false;
        }
        ++gGdxCamRebuilds;
        return true;
    }

    // Copy a resolved 64-byte pool matrix into a stable per-tick scratch slot and return its
    // address so the caller can rewrite the command's w1. The scratch arena MUST be a deque: it
    // never invalidates element addresses on push_back, so a scratch pointer baked into an earlier
    // command stays valid as more matrices are rerouted. P0 records (0, cur, scratch, false) and
    // refills by identity copy; P1 derives the sibling-pool prev pointer and the snap decision.
    uintptr_t GdxP0RerouteMtx(uintptr_t origPtr, bool isProj) {
        if (!mInterpEnabled || origPtr == 0 || ReadableByteLimit(origPtr) < 64u) {
            return origPtr; // defensive: leave the pointer untouched, bit-exact stock path
        }
        mP0Scratch.emplace_back();
        GdxP0Mtx* slot = &mP0Scratch.back();
        std::memcpy(slot, reinterpret_cast<const void*>(origPtr), 64);

        uintptr_t prevPtr = 0;
        bool snap = false;
        if (mP1Enabled) {
            // A slot whose offset was NOT referenced last tick has no usable prev
            // keyframe (spawn/despawn) -> snap. Note it either way so it's in this tick's set.
            const uint32_t offset = static_cast<uint32_t>(origPtr - mCurPoolBase);
            const bool prevPresent = gdx_interp::NoteReferencedOffset(offset);
            if (mForceCutSnap) {
                // A cut/teleport epoch bump or an active pause snaps every slot to cur regardless
                // of a usable prev keyframe. Still noted above so the referenced-set stays correct
                // for the NEXT tick's spawn/despawn decision.
                snap = true;
                ++mP1SnappedCut;
            } else if (mPrevPoolBase != 0 && mCurPoolBase != 0 && prevPresent) {
                prevPtr = mPrevPoolBase + offset;
                if (ReadableByteLimit(prevPtr) < 64u) {
                    prevPtr = 0;
                    snap = true; // sibling not readable -> snap to cur
                    ++mP1SnappedAbsent;
                } else if (isProj ? GdxCameraPoseTeleport(offset)
                                  : (gdx_interp::TranslationTeleport(reinterpret_cast<const void*>(prevPtr),
                                                                     reinterpret_cast<const void*>(origPtr)) ||
                                     // Perfection-plan Slice 3: two objects closer than the
                                     // teleport threshold can still be a silent mispairing --
                                     // unrelated orientations give it away. Default-off until
                                     // owner-validated in a race (gdx_interp.h RotationSnapActive).
                                     (gdx_interp::RotationSnapActive() &&
                                      gdx_interp::RotationTeleport(reinterpret_cast<const void*>(prevPtr),
                                                                   reinterpret_cast<const void*>(origPtr))))) {
                    snap = true; // teleport/cut heuristic (belt-and-suspenders)
                    ++mP1SnappedTeleport;
                } else {
                    ++mP1Lerped;
                    // [interp-pair] Pairing-quality sample; see gdx_interp.h TranslationDelta for
                    // why byte-offset identity can silently pair two different objects. Sample only
                    // slots whose row 3 IS a world position: including projection matrices makes
                    // pair_max/pair_susp report the camera's view-space terms, which reads as a fat
                    // tail whenever the view rotates.
                    const float delta = isProj ? 0.0f
                                               : gdx_interp::TranslationDelta(
                                                     reinterpret_cast<const void*>(prevPtr),
                                                     reinterpret_cast<const void*>(origPtr));
                    if (delta > mP1PairMaxDelta) {
                        mP1PairMaxDelta = delta;
                    }
                    // Suspicious, not wrong: real per-tick motion is a few tens of units
                    // (gdx_interp.cpp:201-204), so a paired slot moving further is either genuinely
                    // fast or a mispairing.
                    if (delta > 200.0f) {
                        ++mP1PairSuspect;
                    }
                }
            } else {
                snap = true; // absent prev keyframe or pool-base mismatch -> snap to cur
                if (mPrevPoolBase == 0 || mCurPoolBase == 0) {
                    ++mP1PoolBaseMisses;
                } else {
                    ++mP1SnappedAbsent;
                }
            }
        }

        // [attack-hl] Per-racer verdict for the attack-highlight probe: unk_21208 is written only
        // while the attack is live (racer.c:5876), so on the attack's first tick its offset was not
        // referenced last tick and this ladder snaps it to t=1 while the body matrix lerps to t<1 --
        // same machine, two instants.
        if (mP1Enabled && mCurPoolBase != 0) {
            const GdxRacerMtxId id = GdxRacerMtxClassify(static_cast<uint32_t>(origPtr - mCurPoolBase));
            if (id.field != GdxRacerMtxField::kNone && id.racer < kGdxRacerMtxSlots) {
                const int col = (id.field == GdxRacerMtxField::kBody)     ? 0
                                : (id.field == GdxRacerMtxField::kSecond) ? 1
                                                                          : 2;
                mRacerMtxVerdict[id.racer][col] = snap ? 2 : 1; // 0 absent, 1 lerped, 2 snapped
            }
        }

        mP0Records.push_back({ origPtr, prevPtr, slot, snap, isProj });
        return reinterpret_cast<uintptr_t>(slot);
    }

    size_t GdxP0ScratchSlots() const { return mP0Records.size(); }

    // Give a per-racer matrix that SPAWNED this tick the keyframe it lacks, borrowed from the same
    // racer's body matrix, so it tweens in lockstep with the machine it is drawn on instead of
    // snapping to t=1 alone.
    //
    // The artifact: a side attack draws a 1.075x scaled red copy of the machine from
    // gGfxPool->unk_21208[racer->id] (racer.c:5876-5896), written ONLY while the attack is live.
    // On the attack's first tick that slot has no previous keyframe and snaps to t=1 while the body
    // (unk_20308, written every drawn tick) lerps to t<1. At only 7.5% larger the highlight should
    // read as a red rim; separated by a tick of motion, the body slides out from inside it.
    //
    // Borrowing is sound because ownership is STRUCTURAL, not a proximity heuristic like the
    // effects-vertex anchor: unk_20308/unk_20A88/unk_21208 are parallel Mtx[30] arrays indexed by
    // the same racer->id. Translation only -- copying the body's rotation would be wrong since the
    // highlight carries its own scale, and the residual rotational mismatch on hard-turning frames
    // is orders of magnitude below the artifact.
    void GdxFixupSpawnedRacerMatrices() {
        if (!mP1Enabled || mCurPoolBase == 0 || mPrevPoolBase == 0 || mForceCutSnap) {
            return;
        }

        // Index the racers whose body matrix has a usable pair this tick.
        size_t bodyIdx[kGdxRacerMtxSlots];
        for (uint32_t i = 0; i < kGdxRacerMtxSlots; ++i) {
            bodyIdx[i] = SIZE_MAX;
        }
        for (size_t i = 0; i < mP0Records.size(); ++i) {
            const GdxP0Record& r = mP0Records[i];
            if (r.snap || r.prev == 0) {
                continue;
            }
            const GdxRacerMtxId id = GdxRacerMtxClassify(static_cast<uint32_t>(r.orig - mCurPoolBase));
            if (id.field == GdxRacerMtxField::kBody && id.racer < kGdxRacerMtxSlots) {
                bodyIdx[id.racer] = i;
            }
        }
        for (size_t i = 0; i < mP0Records.size(); ++i) {
            GdxP0Record& r = mP0Records[i];
            if (!r.snap || r.prev != 0) {
                continue; // already paired, or snapped for a reason other than a missing keyframe
            }
            const GdxRacerMtxId id = GdxRacerMtxClassify(static_cast<uint32_t>(r.orig - mCurPoolBase));
            if ((id.field != GdxRacerMtxField::kHighlight && id.field != GdxRacerMtxField::kSecond) ||
                id.racer >= kGdxRacerMtxSlots || bodyIdx[id.racer] == SIZE_MAX) {
                continue;
            }
            const GdxP0Record& body = mP0Records[bodyIdx[id.racer]];
            if (ReadableByteLimit(body.orig) < 64u || ReadableByteLimit(body.prev) < 64u ||
                ReadableByteLimit(r.orig) < 64u) {
                continue;
            }
            float bodyCur[4][4];
            float bodyPrev[4][4];
            float mine[4][4];
            gdx_interp::MtxToF(reinterpret_cast<const void*>(body.orig), bodyCur);
            gdx_interp::MtxToF(reinterpret_cast<const void*>(body.prev), bodyPrev);
            gdx_interp::MtxToF(reinterpret_cast<const void*>(r.orig), mine);
            for (int axis = 0; axis < 3; ++axis) {
                mine[3][axis] += bodyPrev[3][axis] - bodyCur[3][axis];
            }
            mSynthPrev.emplace_back();
            GdxP0Mtx* synth = &mSynthPrev.back();
            gdx_interp::MtxFromF(mine, synth);
            r.prev = reinterpret_cast<uintptr_t>(synth);
            r.snap = false;
            ++mP1BorrowedKeyframes;
            // Keep the probe's verdict table honest: this slot now lerps.
            const int col = (id.field == GdxRacerMtxField::kHighlight) ? 2 : 1;
            mRacerMtxVerdict[id.racer][col] = 1;
        }
    }

    size_t GdxP1BorrowedKeyframes() const { return mP1BorrowedKeyframes; }

    // [attack-hl] This tick's verdict for one per-racer matrix: 0 not referenced, 1 lerped,
    // 2 snapped. col 0 = body (unk_20308), 1 = second (unk_20A88), 2 = attack highlight (unk_21208).
    uint8_t GdxRacerMtxVerdict(uint32_t racer, int col) const {
        if (racer >= kGdxRacerMtxSlots || col < 0 || col > 2) {
            return 0;
        }
        return mRacerMtxVerdict[racer][col];
    }

    // Refill every scratch slot for the next replay pass. P0 (and any snapped P1 slot) copies the
    // current-pool matrix verbatim (== lerp at t=1). A live P1 slot writes lerp(prev, cur, t) in
    // float space (SoH interpolate_mtxf). At t=1 both paths are byte-identical to the pool matrix.
    void GdxP0RefillScratch(float t) {
        for (const GdxP0Record& r : mP0Records) {
            if (ReadableByteLimit(r.orig) < 64u) {
                continue;
            }
            // Camera projection*view rebuilds from an interpolated pose instead of lerping the
            // finished matrix. r.proj is tested first so slot classification costs only the handful
            // of projection loads per tick, not every matrix in the list.
            //
            // t == 1 deliberately falls through to the verbatim copy below: the transparency
            // invariant is that the last sub-frame IS a byte copy of the pool matrix, and the
            // rebuild must not be the thing that establishes the invariant it is checked against.
            if (r.proj && mInterpCameraRebuild && t < 1.0f && mCurPoolBase != 0 &&
                GdxCameraRebuildSlot(r.orig, r.prev, r.snap, r.scratch, t)) {
                continue;
            }
            if (mP1Enabled && r.prev != 0 && !r.snap && t < 1.0f &&
                ReadableByteLimit(r.prev) >= 64u) {
                gdx_interp::LerpMtx(reinterpret_cast<const void*>(r.prev),
                                    reinterpret_cast<const void*>(r.orig), t, r.scratch);
                // Restore rigidity to per-racer model matrices only. Applied to the racer arrays
                // rather than every pool matrix because those are known to be scale-times-rotation
                // (Matrix_ScaleFrom3DMatrix, racer.c:5985/5883); an arbitrary pool matrix carries no
                // such guarantee and rescaling its rows would be a change, not a correction.
                if (mRotFix && mCurPoolBase != 0) {
                    const GdxRacerMtxId rid = GdxRacerMtxClassify(static_cast<uint32_t>(r.orig - mCurPoolBase));
                    if (rid.field != GdxRacerMtxField::kNone) {
                        GdxRenormalizeLerpedBasis(reinterpret_cast<const void*>(r.prev),
                                                  reinterpret_cast<const void*>(r.orig), t, r.scratch);
                    }
                }
            } else {
                std::memcpy(r.scratch, reinterpret_cast<const void*>(r.orig), 64);
            }
        }
    }

    // --- Carousel viewport interpolation ----------------------------------------------------------
    //
    // The course-select carousel slides by rewriting viewport vtrans[0] per tick (up to 192 vtrans
    // units = 48 screen px), and no matrix carries that motion, so the pool-matrix lerp cannot
    // smooth it. The viewports live in D_i5_80118FF0[2][6], parity-indexed by the SAME D_800DCCFC
    // toggle as the GfxPool, so the previous keyframe already exists at the sibling parity row.
    //
    // Deliberately NOT reusing the matrix machinery: the array is overlay BSS outside the pool
    // span, the prev keyframe is orig +/- 6*16 bytes rather than prevPoolBase + offset, and
    // NoteReferencedOffset's set is keyed on pool offsets a viewport pointer would collide with.
    // Identity here is the slot index, which is perfectly stable -- stronger than the byte-offset
    // identity the matrices live with.
    struct alignas(8) GdxVpSlot {
        int16_t v[8]; // vscale[4], vtrans[4] — layout of the libultra Vp_t, host-native
    };
    struct GdxVpRecord {
        uintptr_t orig;    // &D_i5_80118FF0[parity][i], the game's live viewport for this tick
        uintptr_t prev;    // sibling parity row, same slot (previous tick's values)
        GdxVpSlot* scratch;
        bool snap;
    };
    std::deque<GdxVpSlot> mVpScratch; // deque: element addresses must survive later push_backs
    std::vector<GdxVpRecord> mVpRecords;
    size_t mVpLerped = 0;
    size_t mVpSnapped = 0;

    uintptr_t GdxVpReroute(uintptr_t origPtr) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(&D_i5_80118FF0[0][0][0]);
        if (!mInterpEnabled || origPtr < base || origPtr >= base + sizeof(D_i5_80118FF0) ||
            ((origPtr - base) % sizeof(GdxVpSlot)) != 0) {
            return origPtr;
        }

        const size_t flat = (origPtr - base) / sizeof(GdxVpSlot); // 0..11
        const size_t slot = flat % 6;
        const size_t parity = flat / 6;
        const uintptr_t prevPtr = base + ((parity ^ 1u) * 6 + slot) * sizeof(GdxVpSlot);

        mVpScratch.emplace_back();
        GdxVpSlot* scratch = &mVpScratch.back();
        std::memcpy(scratch, reinterpret_cast<const void*>(origPtr), sizeof(GdxVpSlot));

        // Snap on the whole-frame cut, and on any vtrans[0] delta beyond the game's own per-tick
        // clamp: legit carousel motion never exceeds 192 units, so anything larger is the
        // GAMEMODE_FLX_GP_RACE_NEXT_COURSE instant warp or a tick where the writer was skipped and
        // the sibling row is two ticks stale. One test covers both.
        bool snap = mForceCutSnap || !mP1Enabled;
        if (!snap) {
            const int16_t* cur = reinterpret_cast<const int16_t*>(origPtr);
            const int16_t* prv = reinterpret_cast<const int16_t*>(prevPtr);
            const int delta = static_cast<int>(cur[4]) - static_cast<int>(prv[4]); // vtrans[0]
            if (delta > 192 || delta < -192) {
                snap = true;
            }
        }
        if (snap) {
            ++mVpSnapped;
        } else {
            ++mVpLerped;
        }
        mVpRecords.push_back({ origPtr, prevPtr, scratch, snap });
        return reinterpret_cast<uintptr_t>(scratch);
    }

    void GdxVpRefillScratch(float t) {
        for (const GdxVpRecord& r : mVpRecords) {
            // t>=1 must be a byte-exact copy (same transparency contract as the matrices); the lerp
            // rounds to nearest so the carousel cannot bias a sub-pixel toward the stale keyframe
            // on every pass.
            if (r.snap || t >= 1.0f) {
                std::memcpy(r.scratch, reinterpret_cast<const void*>(r.orig), sizeof(GdxVpSlot));
                continue;
            }
            const int16_t* cur = reinterpret_cast<const int16_t*>(r.orig);
            const int16_t* prv = reinterpret_cast<const int16_t*>(r.prev);
            for (int i = 0; i < 8; ++i) {
                const float v = static_cast<float>(prv[i]) +
                                (static_cast<float>(cur[i]) - static_cast<float>(prv[i])) * t;
                r.scratch->v[i] = static_cast<int16_t>(std::lround(v));
            }
        }
    }

    // --- Effects vertex interpolation (booster flames / side-attack quads) ------------------------
    //
    // racer.c computes effect vertex positions on the CPU per tick from racer->modelMatrix and bumps
    // them into gGfxPool->effectsVtxBuffer with no gSPMatrix anywhere in that path, so at sub-frame
    // t the body renders at the lerped pose while its flame stays baked at the tick-end pose --
    // the afterimage. The pool is double-buffered, so the previous tick's vertices sit at
    // prevPoolBase + the same offset and the parity latch's quiescence argument covers vertex bytes
    // exactly as it covers matrices.
    struct GdxVtxRecord {
        uintptr_t orig;
        uint8_t* scratch; // contiguous batch buffer (count*16 bytes)
        uint32_t count;
        bool snap;
        // Anchor: the owning racer's interpolated motion, captured as the model matrix's
        // translation at both keyframes. The batch is shifted by the anchor's tween each pass; no
        // previous-tick vertex bytes are consulted at all.
        float aPrev[3];
        float aCur[3];
    };
    std::deque<std::vector<uint8_t>> mVtxScratch; // deque of per-batch buffers: stable addresses
    std::vector<GdxVtxRecord> mVtxRecords;
    size_t mVtxLerped = 0;
    size_t mVtxSnapped = 0;

    uintptr_t GdxVtxReroute(uintptr_t origPtr, uint32_t count) {
        const size_t bytes = static_cast<size_t>(count) * 16u;
        if (!mInterpEnabled || count == 0 || ReadableByteLimit(origPtr) < bytes) {
            return origPtr;
        }
        mVtxScratch.emplace_back(bytes);
        uint8_t* scratch = mVtxScratch.back().data();
        std::memcpy(scratch, reinterpret_cast<const void*>(origPtr), bytes);

        // ANCHORED interpolation. Do not replace this with vertex-byte pairing: byte identity
        // cannot name an effect (stale bump-buffer bytes are STABLE, so the side-attack quads'
        // unwritten tc lanes match the sibling pool's identical ancient garbage and lerp into a red
        // vehicle-shaped smear), and in a bunched pack no distance threshold separates "same flame,
        // one tick of motion" from "the next machine's flame" -- both live within tens of units.
        //
        // What CAN name an effect is its owner: every effect in racer.c is computed from its
        // racer's modelMatrix, and that matrix (gGfxPool->unk_20308[id], racer.c:6070) is already
        // rerouted with both keyframes in hand. Find the nearest non-projection matrix record to
        // the batch centroid and shift the whole batch by that anchor's interpolated translation
        // delta. Previous vertex bytes are never read, so there is nothing to mispair; the residual
        // cost is that per-tick shape animation renders at the current keyframe.
        //
        // 3-vertex batches (debris shards, sparks) stay permanently snapped: single-tick particles,
        // several of which genuinely have no owner after a crash.
        bool snap = true;
        float aPrev[3] = { 0.0f, 0.0f, 0.0f };
        float aCur[3] = { 0.0f, 0.0f, 0.0f };
        if (count != 3 && !mForceCutSnap && mP1Enabled && mPrevPoolBase != 0 && mCurPoolBase != 0) {
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            const int16_t* cur = reinterpret_cast<const int16_t*>(origPtr);
            for (uint32_t v = 0; v < count; ++v) {
                const size_t lane = v * 8;
                cx += cur[lane + 0];
                cy += cur[lane + 1];
                cz += cur[lane + 2];
            }
            const float inv = 1.0f / static_cast<float>(count);
            cx *= inv; cy *= inv; cz *= inv;

            // 60 world units: generous for "this machine's own effect" (boosters sit within ~20
            // units of the model origin), tight enough that a neighbouring machine must be
            // physically overlapping to steal an anchor -- and if machines overlap, their motions
            // are near-identical anyway, so a stolen anchor degrades to a correct answer.
            float bestD2 = 60.0f * 60.0f;
            const GdxP0Record* best = nullptr;
            for (const GdxP0Record& m : mP0Records) {
                if (m.proj || m.snap || m.prev == 0) {
                    continue;
                }
                float mf[4][4];
                gdx_interp::MtxToF(reinterpret_cast<const void*>(m.orig), mf);
                const float dx = mf[3][0] - cx;
                const float dy = mf[3][1] - cy;
                const float dz = mf[3][2] - cz;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestD2) {
                    bestD2 = d2;
                    best = &m;
                }
            }
            if (best != nullptr) {
                float cf[4][4];
                float pf[4][4];
                gdx_interp::MtxToF(reinterpret_cast<const void*>(best->orig), cf);
                gdx_interp::MtxToF(reinterpret_cast<const void*>(best->prev), pf);
                for (int i = 0; i < 3; ++i) {
                    aCur[i] = cf[3][i];
                    aPrev[i] = pf[3][i];
                }
                snap = false;
            }
        }
        if (snap) {
            ++mVtxSnapped;
        } else {
            ++mVtxLerped;
        }
        mVtxRecords.push_back({ origPtr, scratch, count, snap,
                                { aPrev[0], aPrev[1], aPrev[2] },
                                { aCur[0], aCur[1], aCur[2] } });
        return reinterpret_cast<uintptr_t>(scratch);
    }

    void GdxVtxRefillScratch(float t) {
        for (const GdxVtxRecord& r : mVtxRecords) {
            const size_t bytes = static_cast<size_t>(r.count) * 16u;
            if (ReadableByteLimit(r.orig) < bytes) {
                continue;
            }
            // Whole-batch copy keeps flag/tc/cn and the current shape; the anchor shift then moves
            // the batch back along its owner's motion. At t=1 the shift is exactly zero, so the
            // copy alone is byte-exact (transparency contract).
            std::memcpy(r.scratch, reinterpret_cast<const void*>(r.orig), bytes);
            if (r.snap || t >= 1.0f) {
                continue;
            }
            float shift[3];
            bool any = false;
            for (int i = 0; i < 3; ++i) {
                // lerp(prev,cur,t) - cur == (prev-cur)*(1-t)
                shift[i] = (r.aPrev[i] - r.aCur[i]) * (1.0f - t);
                if (shift[i] != 0.0f) {
                    any = true;
                }
            }
            if (!any) {
                continue;
            }
            int16_t* out = reinterpret_cast<int16_t*>(r.scratch);
            for (uint32_t v = 0; v < r.count; ++v) {
                const size_t lane = v * 8;
                for (int ax = 0; ax < 3; ++ax) {
                    const float f = static_cast<float>(out[lane + ax]) + shift[ax];
                    out[lane + ax] = static_cast<int16_t>(std::lround(f));
                }
            }
        }
    }

    size_t GdxVpLerped() const { return mVpLerped; }
    size_t GdxVpSnapped() const { return mVpSnapped; }
    size_t GdxVtxLerped() const { return mVtxLerped; }
    size_t GdxVtxSnapped() const { return mVtxSnapped; }

    // memcmp every scratch vs its origin; at t=1 they must match (the transparency invariant).
    size_t GdxP0TransparencyViolations() const {
        size_t bad = 0;
        for (const GdxP0Record& r : mP0Records) {
            if (ReadableByteLimit(r.orig) < 64u) {
                continue;
            }
            if (std::memcmp(r.scratch, reinterpret_cast<const void*>(r.orig), 64) != 0) {
                ++bad;
            }
        }
        return bad;
    }

    // FNV-1a over every resolved command word in every converted list. mLists is not mutated
    // between passes, so its (unordered) iteration order is identical across calls in one tick,
    // making the hash directly comparable pass-to-pass.
    uint64_t GdxP0HashCommands() const {
        uint64_t h = 0xCBF29CE484222325ull;
        for (const auto& kv : mLists) {
            const ConvertedList* l = kv.second.get();
            for (const Fast::F3DGfx& g : l->commands) {
                GdxP0FnvAccum(h, static_cast<uint64_t>(g.words.w0));
                GdxP0FnvAccum(h, static_cast<uint64_t>(g.words.w1));
            }
        }
        return h;
    }

    // Snapshot all command words (same iteration order as the hash) for mutation counting.
    void GdxP0SnapshotCommands(std::vector<uint64_t>& out) const {
        out.clear();
        for (const auto& kv : mLists) {
            const ConvertedList* l = kv.second.get();
            for (const Fast::F3DGfx& g : l->commands) {
                out.push_back(static_cast<uint64_t>(g.words.w0));
                out.push_back(static_cast<uint64_t>(g.words.w1));
            }
        }
    }

    // Count operands that changed vs a prior snapshot (detects in-place interpreter mutation).
    size_t GdxP0CountMutations(const std::vector<uint64_t>& snap) const {
        size_t idx = 0, muts = 0;
        for (const auto& kv : mLists) {
            const ConvertedList* l = kv.second.get();
            for (const Fast::F3DGfx& g : l->commands) {
                if (idx + 1 < snap.size()) {
                    if (snap[idx] != static_cast<uint64_t>(g.words.w0)) {
                        ++muts;
                    }
                    if (snap[idx + 1] != static_cast<uint64_t>(g.words.w1)) {
                        ++muts;
                    }
                }
                idx += 2;
            }
        }
        return muts;
    }

  private:

    const N64Gfx* mRootBegin = nullptr;
    uintptr_t mRootByteEnd = 0;
    uintptr_t mModuleBegin = 0;
    uintptr_t mModuleEnd = 0;
    bool mIsBig = false;
    ConversionStats* mStats = nullptr;
    std::unordered_map<const N64Gfx*, std::unique_ptr<ConvertedList>> mLists;
    std::vector<QueueItem> mWorkQueue;
    std::vector<Fast::F3DGfx> mNoopList{ MakeLusGfx(static_cast<uintptr_t>(kOpEndDl) << 24, 0) };

    // Scratch-slot indirection state, populated only when mInterpEnabled. The arena is per-adapter
    // (per gdx_gfx_run, per tick) and deque<> guarantees element-address stability. The
    // host-driven decoupled loop reuses ALL of the P1 dual-pool lerp machinery, so P2 activation is
    // OR-ed into the P1 enable; mP2Host must be declared first because members initialise in
    // declaration order and mP1Enabled reads it.
    const bool mP2Host = GdxP2HostConfigured();
    const bool mP0Enabled = GdxInterpP0Enabled();
    const bool mP1Enabled = gdx_interp::P1().enabled || mP2Host;
    const bool mInterpEnabled = mP0Enabled || mP1Enabled;
    // Latched once per adapter rather than read per matrix command: CameraInterpActive() hashes a
    // CVar name and this condition is evaluated for EVERY G_MTX in the display list. The Bucket B
    // dev gate is OR-ed in so a dev build can force camera interpolation on for A/B without
    // touching the shipped CVar.
    const bool mInterpCamera =
        gdx_interp::CameraInterpActive() || gdx_dev_gate(GDX_GATE_INTERP_CAMERA) != 0;
    // Rebuild the camera's projection*view from an interpolated POSE instead of lerping the
    // finished matrix element-wise. Requires mInterpCamera: with camera interpolation off the
    // matrix is not rerouted at all and there is nothing to rebuild. ON by default; see
    // GdxCameraRebuildConfigured for the measurement, and set GDX_INTERP_CAMERA_POSE=0 to A/B it.
    const bool mInterpCameraRebuild = mInterpCamera && GdxCameraRebuildConfigured();
    // Kill the rotation smear across a side attack's two model-basis discontinuities. Independent of
    // the camera work: this one is a racer-matrix defect and needs no projection involvement.
    const bool mBasisJumpFix = GdxBasisJumpFixConfigured();
    // Rescale element-wise-lerped per-racer basis rows back to rigid. See GdxRenormalizeLerpedBasis.
    const bool mRotFix = GdxRotFixConfigured();
    struct GdxP0Record {
        uintptr_t orig;    // resolved CURRENT-pool matrix host pointer (curPoolPtr)
        uintptr_t prev;    // sibling(PREVIOUS)-pool matrix host pointer (0 in P0 / snapped slot)
        GdxP0Mtx* scratch; // stable slot the command now points at
        bool snap;         // P1: force t=1 (absent prev keyframe or teleport) for this slot
        bool proj;         // G_MTX_PROJECTION load: excluded from effect-anchor search (its row 3
                           // is a view-space term, not a world position)
    };
    std::deque<GdxP0Mtx> mP0Scratch;
    std::vector<GdxP0Record> mP0Records;
    // [attack-hl] Per-racer verdicts for the three per-racer matrix arrays this tick:
    // 0 = not referenced, 1 = lerped, 2 = snapped. Columns are body / second / highlight.
    static constexpr uint32_t kGdxRacerMtxSlots = 30;
    uint8_t mRacerMtxVerdict[kGdxRacerMtxSlots][3] = {};
    // Synthetic previous keyframes built by GdxFixupSpawnedRacerMatrices. deque: a record's prev
    // pointer must stay valid as later ones are appended.
    std::deque<GdxP0Mtx> mSynthPrev;
    size_t mP1BorrowedKeyframes = 0;
    // Dual-pool bases (latched per tick by GdxInterpBeginTick) + snap-event counters.
    uintptr_t mCurPoolBase = 0;
    uintptr_t mPrevPoolBase = 0;
    size_t mP1Lerped = 0;
    size_t mP1SnappedAbsent = 0;
    size_t mP1SnappedTeleport = 0;
    size_t mP1SnappedCut = 0;      // P3: slots snapped by a cut-epoch bump or an active pause
    // [interp-pair] pairing-quality, reset per tick with the counters above
    float mP1PairMaxDelta = 0.0f;  // largest prev->cur translation delta among PAIRED slots
    size_t mP1PairSuspect = 0;     // paired slots that moved further than a tick plausibly can
    size_t mP1PoolBaseMisses = 0;
    bool mForceCutSnap = false;    // P3: this whole tick snaps (cut/teleport epoch changed, or paused)
    bool mCaptureSnapThisTick = false; // P4: the whole-frame snap this tick is a transition capture

    size_t CommandStrideForSource(const N64Gfx* source) const {
        const uintptr_t ptr = reinterpret_cast<uintptr_t>(source);
        /* Some game-built lists live inside the emulated RDRAM arena but use the
         * PORT Gfx ABI (16-byte packets with a pointer-width w1). They must win
         * over the general "RDRAM is raw N64" classification. */
        if (IsHostWideCommandPointer(ptr)) {
            return kHostBuiltGfxStride;
        }
        return (IsRawN64HostPointer(ptr) || IsHostN64CommandPointer(ptr)) ? kN64GfxStride : kHostBuiltGfxStride;
    }

    // Wide 16-byte version of `source`, converted and cached on first encounter; `ioLimit` becomes
    // the wide command count. Returns `source` and leaves ioLimit alone when conversion is
    // disabled, the source is already wide, or its extent is unknown -- the caller then falls back
    // to the narrow machinery.
    const N64Gfx* GetOrBuildConvertedWide(const N64Gfx* source, size_t& ioLimit) {
        EnsureG2ConvertInit();
        if (!gG2ConvertEnabled || source == nullptr) {
            return source;
        }
        const size_t stride = CommandStrideForSource(source);
        if (stride != kN64GfxStride) {
            return source;  // already wide (game-emitted, or a converted buffer)
        }

        // Bound the walk exactly as the narrow path would.
        size_t narrowLimit = (ioLimit != 0) ? ioLimit : KnownCommandLimit(source);
        const size_t knownLimit = KnownCommandLimit(source);
        if (knownLimit != 0) {
            narrowLimit = (narrowLimit != 0) ? std::min(narrowLimit, knownLimit) : knownLimit;
        }
        if (narrowLimit == 0) {
            return source;  // unknown extent -> leave to the narrow path
        }

        // Endianness and dialect must be decided the SAME way ProcessList would, or the converted
        // value path diverges and the pointer classification stops matching. The dialect is
        // recorded so ProcessList reuses it instead of re-deriving it from the tagless wide buffer.
        const bool isBig = CommandSourceIsBigEndian(source, stride);
        const GdxSegmentUcode dialect = GdxAssetPointerDialect(reinterpret_cast<uintptr_t>(source));
        const bool isF3d =
            (dialect == GdxSegmentUcode::F3D)      ? true
            : (dialect == GdxSegmentUcode::F3DEX2) ? false
            : (IsF3DAssetPointer(reinterpret_cast<uintptr_t>(source)) ||
               DisplayListUsesF3D(source, narrowLimit, stride, isBig));

        const std::vector<gdx::WideGfx>& wide =
            gWideCache.GetOrBuild(source, narrowLimit, isBig, isF3d, G2StampFor(source));
        if (wide.empty()) {
            return source;
        }

        const N64Gfx* wideSrc = reinterpret_cast<const N64Gfx*>(wide.data());
        /* [dl-census] diagnostic, strip later: one line per unique narrow list converted this
           session, with an ASLR-stable identity when the source is module-resident. */
        {
            static uintptr_t sDlCensusSeen[48] = {};
            static int sDlCensusCount = 0;
            const uintptr_t srcAddr = reinterpret_cast<uintptr_t>(source);
            bool dlDup = false;
            for (int dc = 0; dc < sDlCensusCount; dc++) {
                if (sDlCensusSeen[dc] == srcAddr) {
                    dlDup = true;
                    break;
                }
            }
            // [dl-census] high-frequency per-draw diagnostic: silent unless GDX_DIAG_VERBOSE=1.
            if (gdx_diag_verbose() && !dlDup && sDlCensusCount < 48) {
                sDlCensusSeen[sDlCensusCount++] = srcAddr;
                uint64_t prefVA = 0;
                if (mModuleBegin != 0 && srcAddr >= mModuleBegin && srcAddr < mModuleEnd) {
                    prefVA = kPreferredImageBaseVA + (srcAddr - mModuleBegin);
                }
                const char* cls = IsRdramHostPointer(srcAddr) ? "rdram"
                                  : (prefVA != 0)             ? "module"
                                                              : "other";
                gdx_port_logf("[dl-census] narrow src=%p cls=%s prefVA=%011llX limit=%zu isBig=%d f3d=%d\n",
                              reinterpret_cast<const void*>(source), cls,
                              static_cast<unsigned long long>(prefVA), narrowLimit,
                              isBig ? 1 : 0, isF3d ? 1 : 0);
            }
        }
        // Clearing is safe because the dialect is re-recorded below before any ProcessList read of
        // this exact buffer, and a rebuilt buffer's old address is never read again. Bounds the
        // map's growth from in-place rebuilds that relocate a cached list's storage.
        if (gConvertedWideIsF3d.size() > 8192) {
            gConvertedWideIsF3d.clear();
        }
        gConvertedWideIsF3d[wideSrc] = isF3d;
        ioLimit = wide.size();
        return wideSrc;
    }

    bool CommandSourceIsBigEndian(const N64Gfx* source, size_t stride) const {
        if ((source == nullptr) || (stride != kN64GfxStride)) {
            return false;
        }

        const uintptr_t ptr = reinterpret_cast<uintptr_t>(source);
        if (!IsRawN64HostPointer(ptr) || IsHostN64CommandPointer(ptr)) {
            return false;
        }

        return IsLikelyBigEndianDisplayList(source, ReadableCommandLimit(source, stride));
    }

    size_t RootCommandLimit(const N64Gfx* source) const {
        if ((source != nullptr) && (mRootBegin != nullptr)) {
            const uintptr_t ptr = reinterpret_cast<uintptr_t>(source);
            const uintptr_t begin = reinterpret_cast<uintptr_t>(mRootBegin);
            if ((ptr >= begin) && (ptr < mRootByteEnd)) {
                return static_cast<size_t>((mRootByteEnd - ptr) / CommandStrideForSource(source));
            }
        }
        return 0;
    }

    size_t KnownCommandLimit(const N64Gfx* source) const {
        if (source == nullptr) {
            return 0;
        }

        const size_t stride = CommandStrideForSource(source);
        const size_t readableLimit = ReadableCommandLimit(source, stride);
        if (readableLimit == 0) {
            return 0;
        }

        size_t limit = 0;
        const auto applyLimit = [&limit](size_t candidate) {
            if (candidate == 0) {
                return;
            }
            limit = (limit == 0) ? candidate : std::min(limit, candidate);
        };

        applyLimit(RootCommandLimit(source));

        const uintptr_t ptr = reinterpret_cast<uintptr_t>(source);
        const size_t registeredRemaining = RegisteredHostRemaining(ptr);
        if (registeredRemaining >= stride) {
            applyLimit(registeredRemaining / stride);
        }

        if ((mModuleBegin != 0) && (ptr >= mModuleBegin) && (ptr < mModuleEnd)) {
            applyLimit(static_cast<size_t>((mModuleEnd - ptr) / stride));
        }

        for (uint8_t segment = 0; segment < kGfxSegmentCount; segment++) {
            const uintptr_t base = gSegments[segment];
            if ((base != 0) && (ptr >= base) && (ptr < base + kSegmentOffsetLimit)) {
                applyLimit(static_cast<size_t>(((base + kSegmentOffsetLimit) - ptr) / stride));
            }
        }

        applyLimit(readableLimit);
        return limit;
    }

    bool TryResolveAddress(uint32_t raw, ResolvedAddress& out, size_t requiredBytes = 1, bool preferPhysical = false,
                           uintptr_t sourceHint = 0) const {
        if (raw == 0) {
            return false;
        }

        /* Segment 9 is not a single global namespace in Expansion Kit builds:
           Create Machine/machine-settings use the cartridge machine_models
           image, while Course Edit uses a different disk-resident image at the
           same 0x09xxxxxx addresses. The port's mode loader owns that switch;
           consult it before the generated EK address ranges so stale/overlapping
           registrations cannot select the other mode's bytes. */
        {
            uintptr_t modeAddress = 0;
            if (gdx_resolve_mode_segment9(raw, requiredBytes, &modeAddress) != 0 &&
                ReadableByteLimit(modeAddress) >= requiredBytes) {
                out.full = modeAddress;
                out.segment = 9u;
                out.offset = raw & 0x00FFFFFFu;
                out.segmented = true;
                return true;
            }
        }

        // Armed only for segment-9 tokens that just missed the authoritative mode resolver; the
        // destructor fires on whichever return below serves (or fails to serve) the token. Gates
        // read live, as on the raw-resolver twin above.
        const bool seg9DiagEnabled =
            gdx_dev_gate(GDX_GATE_LOG_FILE) || gdx_dev_gate(GDX_GATE_DIAG_NODEINFO);
        GdxSeg9FallbackDiag seg9Diag(seg9DiagEnabled && (raw >> 24) == 9u, raw, requiredBytes, &out);

        /* Disk-resident EK overlays keep their original N64 virtual/segmented pointers inside
           display lists while their payloads live in generated host arrays, so these explicit
           token ranges must resolve before the generic KSEG/segment heuristics. Reverse order lets
           the most recently registered overlay win when original overlay VRAM ranges overlap.
           Raw-pointer iteration: this runs per translated data pointer over ~600 EK ranges and
           MSVC Debug checked iterators made it a real per-frame cost. */
        {
            const N64AddressRange* ranges = gN64AddressRanges.data();
            for (size_t ri = gN64AddressRanges.size(); ri > 0; ri--) {
                const N64AddressRange& r = ranges[ri - 1];
                if (raw < r.n64Begin) {
                    continue;
                }
                const size_t offset = static_cast<size_t>(raw - r.n64Begin);
                if (offset <= r.size && requiredBytes <= r.size - offset) {
                    out.full = r.hostBegin + offset;
                    out.segmented = (r.n64Begin >> 24) < kGfxSegmentCount;
                    if (out.segmented) {
                        out.segment = static_cast<uint8_t>(r.n64Begin >> 24);
                        out.offset = raw & 0x00FFFFFFu;
                    }
                    return true;
                }
            }
        }

        if (ResolvePortBssAlias(raw, out)) {
            return true;
        }

        if (ResolveVenueBankAlias(raw, out)) {
            return true;
        }

        // requiredBytes is threaded through so a caller-side over-estimate cannot accept an
        // interior match that overruns the row's declared image size.
        if (ResolveGeneratedAssetStub(raw, out, requiredBytes)) {
            return true;
        }

        if (ResolveSetupGfxStub(raw, out)) {
            return true;
        }

        const uint32_t d1000000_low = Low32(reinterpret_cast<uintptr_t>(D_1000000));
        for (const HostRange& range : gHostRanges) {
            if (range.begin == reinterpret_cast<uintptr_t>(D_1000000)) {
                /* requiredBytes must fit before the END of the range, not just contain its start.
                   D_1000000 also backs per-player/machine structures (e.g.
                   &D_1000000.unk_21988[playerIndex] as a G_MTX source), so a matrix or vertex load
                   near the tail would otherwise walk into unrelated memory. */
                const size_t offset = static_cast<size_t>(raw - d1000000_low);
                if (raw >= d1000000_low && offset <= range.size && requiredBytes <= range.size - offset) {
                    out.full = static_cast<uintptr_t>(gSegments[1]) + offset;
                    out.segmented = false;
                    return true;
                }
                break;
            }
        }

        /* Explicit N64 segment addresses (top byte = segment index 1..15, e.g. the 0x08xxxxxx /
           0x0Axxxxxx course/venue texture pointers) resolve through the segment table. This must
           run BEFORE the low-32 host-range heuristic, which can false-match a segment address
           against an unrelated host allocation whose low 32 bits happen to cover it -- that is
           what left the track textures reading garbage. */
        {
            const uint8_t seg = static_cast<uint8_t>(raw >> 24);
            const uint32_t segOffset = raw & 0x00FFFFFFu;
            if (seg >= 1 && seg < kGfxSegmentCount && gSegments[seg] != 0 &&
                segOffset < kSegmentOffsetLimit) {
                const uintptr_t full = gSegments[seg] + segOffset;
                /* A segment match is authoritative only when the result is actually readable.
                   K0_TO_PHYS truncates module data pointers to 29 bits, so e.g.
                   0x7FF702142C10 & 0x1FFFFFFF = 0x02142C10 masquerades as a segment-2 offset far
                   past the real segment-2 buffer; accepting it yields an unreadable pointer that
                   TranslateDataPointer nulls and the texture silently vanishes. Fall through so
                   the physical-window paths can reconstruct the original module pointer. */
                if (ReadableByteLimit(full) >= requiredBytes) {
                    out.full = full;
                    out.segment = seg;
                    out.offset = segOffset;
                    out.segmented = true;
                    return true;
                }
            }
        }

        /* Host-built PORT commands still carry N64-sized pointer words, so a pointer into the
           emulated RDRAM arena can arrive as its low 32 bits. Unlike the quarantined
           registered-range reconstruction below this is one exact, session-owned 8 MiB allocation
           and unsigned subtraction handles a low32 wrap. Must stay after explicit segment tokens
           so 0x01xxxxxx..0x0Fxxxxxx keep their N64 meaning. */
        {
            uintptr_t rdramAddress = 0;
            if (ResolveRdramLow32(raw, requiredBytes, &rdramAddress)) {
                out.full = rdramAddress;
                out.segmented = false;
                return true;
            }
        }

        if (ResolveRegisteredHostPointer(raw, out, requiredBytes)) {
            return true;
        }

        /* KSEG0 (0x80000000–0x9FFFFFFF) and KSEG1 (0xA0000000–0xBFFFFFFF):
           strip segment bits to obtain the physical RDRAM offset.
           Ordered AFTER all asset-stub resolvers so ROM-backed stubs always win. */
        if (raw >= 0x80000000u && raw <= 0xBFFFFFFFu) {
            const uint32_t phys = raw & 0x1FFFFFFFu;
            if (phys < static_cast<uint32_t>(GDX_RDRAM_SIZE) && gdx_rdram != nullptr) {
                out.full = reinterpret_cast<uintptr_t>(gdx_rdram) + phys;
                out.segmented = false;
                return true;
            }
            /* Out-of-RDRAM KSEG0 is not necessarily MMIO/cart: a truncated 64-bit host pointer
               whose low32 lands in 0x80-0x9F (e.g. 0x933AEF70 from heap 0x20A933AEF70) looks
               identical. Reconstruct against known high-32 windows before giving up, or texture
               loads feed garbage into TMEM. Quarantined -- this is a guess; the deterministic
               RDRAM strip above covers the non-guessing case. */
            if (LegacyResolveEnabled()) {
                const uintptr_t highCandidates[] = {
                    reinterpret_cast<uintptr_t>(mRootBegin) & 0xFFFFFFFF00000000ULL,
                    mModuleBegin & 0xFFFFFFFF00000000ULL,
                };
                for (uintptr_t high : highCandidates) {
                    if (high == 0) {
                        continue;
                    }
                    const uintptr_t full = high | static_cast<uintptr_t>(raw);
                    if (ReadableByteLimit(full) >= requiredBytes) {
                        out.full = full;
                        out.segmented = false;
                        RecordLegacyResolveHit(LegacyResolveBranch::kKseg0High32, raw, gLegacyResolveCurrentOp);
                        return true;
                    }
                }
                for (const HostRange& range : gHostRanges) {
                    const uintptr_t high = range.begin & 0xFFFFFFFF00000000ULL;
                    if (high == 0) {
                        continue;
                    }
                    const uintptr_t full = high | static_cast<uintptr_t>(raw);
                    if (ReadableByteLimit(full) >= requiredBytes) {
                        out.full = full;
                        out.segmented = false;
                        RecordLegacyResolveHit(LegacyResolveBranch::kKseg0High32, raw, gLegacyResolveCurrentOp);
                        return true;
                    }
                }
            }
            return false; /* genuinely unresolvable KSEG0 (MMIO/cart range) */
        }

        /* Some PORT paths pass bare physical RDRAM offsets through display-list words after
           osVirtualToPhysical() truncates host pointers, so mirror the decomp-side resolver or
           sub-DL/data pointers like 0x0013C700 become no-op display lists. The lower bound sits at
           the graphics-pool/arena area so tiny immediates such as 0x400 are not read as pointers. */
        if ((raw >= static_cast<uint32_t>(GDX_RDRAM_GFXPOOL_OFFSET)) &&
            (raw < static_cast<uint32_t>(GDX_RDRAM_SIZE)) &&
            (gdx_rdram != nullptr)) {
            out.full = reinterpret_cast<uintptr_t>(gdx_rdram) + raw;
            out.segmented = false;
            return true;
        }

        const uint8_t encodedSegment = static_cast<uint8_t>(raw >> 24);
        const uint32_t encodedOffset = raw & 0x00FFFFFE;

        /* On PORT, K0_TO_PHYS()/OS_K0_TO_PHYSICAL() are full passthroughs -- (u32)(uintptr_t)(x) --
           so `raw` is the complete unmasked low32 of the real host pointer, not the low 29 bits.
           The window mask must match: take the high 32 bits from a known range and OR in the full
           low32, the same convention gdx_resolve_module_host_address() and the mModuleBegin block
           below use. The helper is extracted here so it can run either before or after the segment
           table depending on the caller; bounds and readability checks stop small immediates from
           matching. */
        constexpr uintptr_t kPhysicalAddressMask = 0xFFFFFFFFu;
        // Quarantined guess: substituting a known range's high32 onto `raw`'s low32. The gate
        // lives in tryAllPhysicalWindows so one check covers both call sites below.
        const auto tryPhysicalWindow = [&](uintptr_t begin, uintptr_t end) -> bool {
            if ((begin == 0) || (end <= begin)) {
                return false;
            }
            uintptr_t full = (begin & ~kPhysicalAddressMask) | static_cast<uintptr_t>(raw);
            /* A host range can straddle a 4 GB low32 window, so the token may belong to the NEXT
               window even though the range begins in the previous one. */
            if (full < begin) {
                constexpr uintptr_t kPhysicalAddressWindow = kPhysicalAddressMask + 1u;
                if (full > UINTPTR_MAX - kPhysicalAddressWindow) {
                    return false;
                }
                full += kPhysicalAddressWindow;
            }
            if ((full < begin) || (full >= end) ||
                (requiredBytes > static_cast<size_t>(end - full)) ||
                (ReadableByteLimit(full) < requiredBytes)) {
                return false;
            }
            out.full = full;
            out.segmented = false;
            RecordLegacyResolveHit(LegacyResolveBranch::kPhysicalWindow, raw, gLegacyResolveCurrentOp);
            return true;
        };
        const auto tryAllPhysicalWindows = [&]() -> bool {
            if (!LegacyResolveEnabled()) {
                return false;
            }
            if (tryPhysicalWindow(mModuleBegin, mModuleEnd)) return true;
            for (const HostRange& range : gHostRanges) {
                if ((range.begin == 0) || (range.size == 0) ||
                    (range.size > UINTPTR_MAX - range.begin)) {
                    continue;
                }
                if (tryPhysicalWindow(range.begin, range.begin + range.size)) return true;
            }
            return false;
        };

        /* Quarantined last-resort guess. A host-built display list and the matrices/vertices it
         * references are almost always allocated in the same 4 GB low32 window (the GfxPool / task
         * DL arena); when that arena is not a registered host range the reconstructions above miss
         * and the pointer degrades to an identity matrix. Accept only a readable result within one
         * allocation region of the source, so it cannot false-match unrelated memory elsewhere in
         * the window. GDX_DIAG_NO_SRCWIN=1 disables it for bisection without a rebuild. */
        const auto trySourceWindow = [&]() -> bool {
            if (!LegacyResolveEnabled() || gdx_dev_gate(GDX_GATE_NO_SRCWIN) || (sourceHint == 0))
                return false;
            uintptr_t full = (sourceHint & ~kPhysicalAddressMask) | static_cast<uintptr_t>(raw);
            const uintptr_t lo = (full < sourceHint) ? full : sourceHint;
            const uintptr_t hi = (full < sourceHint) ? sourceHint : full;
            constexpr uintptr_t kSourceWindowSpan = 0x04000000u; // 64 MB around the DL
            if ((hi - lo) > kSourceWindowSpan) return false;
            if (ReadableByteLimit(full) < requiredBytes) return false;
            out.full = full;
            out.segmented = false;
            RecordLegacyResolveHit(LegacyResolveBranch::kSourceWindow, raw, gLegacyResolveCurrentOp);
            return true;
        };

        /* When the caller knows `raw` came from K0_TO_PHYS on a host pointer (e.g. a G_MTX from
           host-built F3DEX2 code), the low32 physical window must be tried BEFORE the segment
           table, or a raw whose top byte matches an active segment index -- 0x0805DAA0 against
           segment 8 -- gets misrouted. */
        if (preferPhysical && (tryAllPhysicalWindows() || trySourceWindow())) {
            return true;
        }

        if ((encodedSegment < kGfxSegmentCount) &&
            ((gSegments[encodedSegment] != 0) || (encodedSegment == 0)) &&
            ((raw & 0x00FFFFFF) < kSegmentOffsetLimit)) {
            const uintptr_t full = gSegments[encodedSegment] + encodedOffset;
            /* Same readability gate as the explicit-segment path above: reject
               truncated module pointers masquerading as segment offsets so the
               physical-window reconstruction below gets a chance. */
            if (ReadableByteLimit(full) >= requiredBytes) {
                out.full = full;
                out.segment = encodedSegment;
                out.offset = encodedOffset;
                out.segmented = true;
                return true;
            }
        }

        if (!preferPhysical && (tryAllPhysicalWindows() || trySourceWindow())) {
            return true;
        }

        /* Quarantined ambiguous cross-segment fallback. When `raw`'s own top-byte segment is stale
           or unregistered this frame, an unrelated segment can win purely by low32 coincidence, so
           picking the numerically closest offset outright yields wrong-but-readable vertex/matrix
           loads (a requiredBytes check only guards SHORT reads, not wrong-but-long-enough ones).
           Gather every in-range segment, sort by offset ascending, and accept the first that
           actually proves readable -- falling through instead of trusting the closest. */
        if (LegacyResolveEnabled()) {
            struct SegCandidate {
                uint8_t segment;
                uint32_t offset;
                uintptr_t full;
            };
            SegCandidate candidates[kGfxSegmentCount];
            int candidateCount = 0;

            for (uint8_t segment = 0; segment < kGfxSegmentCount; segment++) {
                const uintptr_t base = gSegments[segment];
                if (base == 0) {
                    continue;
                }

                const uint32_t baseLow = static_cast<uint32_t>(base);
                const uint32_t offset = raw - baseLow;
                if (offset < kSegmentOffsetLimit && candidateCount < kGfxSegmentCount) {
                    candidates[candidateCount++] = SegCandidate{ segment, offset, base + offset };
                }
            }

            std::sort(candidates, candidates + candidateCount,
                      [](const SegCandidate& a, const SegCandidate& b) { return a.offset < b.offset; });

            for (int i = 0; i < candidateCount; i++) {
                if (ReadableByteLimit(candidates[i].full) >= requiredBytes) {
                    out.full = candidates[i].full;
                    out.segment = candidates[i].segment;
                    out.offset = candidates[i].offset;
                    out.segmented = true;
                    RecordLegacyResolveHit(LegacyResolveBranch::kCrossSegmentFallback, raw, gLegacyResolveCurrentOp);
                    return true;
                }
            }
        }

        // Quarantined mModuleBegin high-32 reconstruction guess.
        if (LegacyResolveEnabled() && (mModuleBegin != 0)) {
            uintptr_t full = (mModuleBegin & 0xFFFFFFFF00000000ULL) | static_cast<uintptr_t>(raw);
            if (full < mModuleBegin) {
                full += 0x100000000ULL;
            }

            /* Requiring the full payload readable, not just `full` inside the module range: a
               candidate landing a few bytes before an unmapped page otherwise "succeeds" and a
               58-vertex (928-byte) MakePersistentVtxCopy walks into unrelated memory -- the
               stretched-polygon vertex spike. */
            if ((full >= mModuleBegin) && (full < mModuleEnd) &&
                (ReadableByteLimit(full) >= requiredBytes)) {
                out.full = full;
                out.segmented = false;
                RecordLegacyResolveHit(LegacyResolveBranch::kModuleHigh32, raw, gLegacyResolveCurrentOp);
                return true;
            }
        }

        // Quarantined raw>=0x10000000 high-32 candidate scan.
        if (LegacyResolveEnabled() && (raw >= 0x10000000)) {
            const uintptr_t highCandidates[] = {
                reinterpret_cast<uintptr_t>(mRootBegin) & 0xFFFFFFFF00000000ULL,
                mModuleBegin & 0xFFFFFFFF00000000ULL,
                gSegments[0] & 0xFFFFFFFF00000000ULL,
                gSegments[1] & 0xFFFFFFFF00000000ULL,
                gSegments[2] & 0xFFFFFFFF00000000ULL,
                gSegments[3] & 0xFFFFFFFF00000000ULL,
                gSegments[4] & 0xFFFFFFFF00000000ULL,
                gSegments[5] & 0xFFFFFFFF00000000ULL,
                gSegments[6] & 0xFFFFFFFF00000000ULL,
                gSegments[7] & 0xFFFFFFFF00000000ULL,
                gSegments[8] & 0xFFFFFFFF00000000ULL,
                gSegments[9] & 0xFFFFFFFF00000000ULL,
                gSegments[10] & 0xFFFFFFFF00000000ULL,
                gSegments[11] & 0xFFFFFFFF00000000ULL,
                gSegments[12] & 0xFFFFFFFF00000000ULL,
                gSegments[13] & 0xFFFFFFFF00000000ULL,
                gSegments[14] & 0xFFFFFFFF00000000ULL,
                gSegments[15] & 0xFFFFFFFF00000000ULL,
            };

            for (uintptr_t high : highCandidates) {
                if (high == 0) {
                    continue;
                }

                // Full-payload readability, same reason as the mModuleBegin block above: a 1-byte
                // check passes a candidate sitting just before the end of its region.
                const uintptr_t full = high | static_cast<uintptr_t>(raw);
                if (ReadableByteLimit(full) >= requiredBytes) {
                    out.full = full;
                    out.segmented = false;
                    RecordLegacyResolveHit(LegacyResolveBranch::kRawHigh32Scan, raw, gLegacyResolveCurrentOp);
                    return true;
                }
            }

            /* Also try high32 from every registered host range (covers heap / fiber-stack
               allocations that share a VirtualAlloc region with gdx_rdram but whose full
               address isn't yet captured in gSegments or the module range). */
            for (const auto& range : gHostRanges) {
                if (range.begin == 0) {
                    continue;
                }
                const uintptr_t high = range.begin & 0xFFFFFFFF00000000ULL;
                if (high == 0) {
                    continue;
                }
                const uintptr_t full = high | static_cast<uintptr_t>(raw);
                if (ReadableByteLimit(full) >= requiredBytes) {
                    out.full = full;
                    out.segmented = false;
                    RecordLegacyResolveHit(LegacyResolveBranch::kRawHigh32Scan, raw, gLegacyResolveCurrentOp);
                    return true;
                }
            }
        }

        {
            static int sResolveFails = 0;
            if (sResolveFails < 200) {
                ++sResolveFails;
                const uintptr_t rootHigh = mRootBegin
                    ? (reinterpret_cast<uintptr_t>(mRootBegin) & 0xFFFFFFFF00000000ULL)
                    : 0ULL;
                const uintptr_t modHigh  = mModuleBegin & 0xFFFFFFFF00000000ULL;
                const uintptr_t rootCand = rootHigh | static_cast<uintptr_t>(raw);
                const uintptr_t modCand  = modHigh  | static_cast<uintptr_t>(raw);
                gdx_port_logf("[resolve-fail] raw=%08X "
                              "mModule=[%016llX,%016llX) "
                              "rootCand=%016llX rootReadable=%d "
                              "modCand=%016llX modInRange=%d modReadable=%d\n",
                              raw,
                              static_cast<unsigned long long>(mModuleBegin),
                              static_cast<unsigned long long>(mModuleEnd),
                              static_cast<unsigned long long>(rootCand),
                              IsReadableAddress(rootCand) ? 1 : 0,
                              static_cast<unsigned long long>(modCand),
                              (modCand >= mModuleBegin && modCand < mModuleEnd) ? 1 : 0,
                              IsReadableAddress(modCand) ? 1 : 0);
            }
        }
        return false;
    }

    uintptr_t TranslateDataPointer(uint32_t raw, size_t requiredBytes = 1, bool preferPhysical = false,
                                   uintptr_t sourceHint = 0) const {
        if (raw == 0) {
            return 0;
        }

        ResolvedAddress resolved = {};
        if (TryResolveAddress(raw, resolved, requiredBytes, preferPhysical, sourceHint)) {
            return IsReadableAddress(resolved.full) ? resolved.full : 0;
        }

        // Quarantined: treats the bare low32 as a literal host pointer, which only works if the
        // allocation happens to sit under 4 GB -- not guaranteed on any 64-bit target.
        if (!LegacyResolveEnabled()) {
            return 0;
        }
        const uintptr_t direct = static_cast<uintptr_t>(raw);
        if (IsReadableAddress(direct)) {
            RecordLegacyResolveHit(LegacyResolveBranch::kDirectCast, raw, gLegacyResolveCurrentOp);
            return direct;
        }
        return 0;
    }

    static uint64_t TextureBytesForPixels(uint64_t pixels, uint32_t size) {
        switch (size) {
            case 0: return (pixels + 1) / 2;
            case 1: return pixels;
            case 2: return pixels * 2;
            case 3: return pixels * 4;
            default: return 0;
        }
    }

    static uint64_t LoadBlockCopyBytes(const N64Gfx& command, uint32_t size, uint32_t imageWidth) {
        /* G_LOADBLOCK's uls is a raw source texel offset (bits 23:12 of w0),
         * unlike G_LOADTILE's fixed-point texture coordinates. The bridge must copy from
         * settimg_ptr all the way through the end of this load (uls + lrs + 1 texels)
         * so libultraship can index into the buffer at the correct source offset. */
        const uint32_t uls_texels = (command.w0 >> 12) & 0xFFF;
        const uint32_t ult_rows = command.w0 & 0xFFF;
        const uint32_t lrs = (command.w1 >> 12) & 0xFFF;
        const uint64_t startTexel = static_cast<uint64_t>(ult_rows) * imageWidth + uls_texels;
        return TextureBytesForPixels(startTexel + lrs + 1, size);
    }

    static uint64_t LoadTileCopyBytes(const N64Gfx& command, uint32_t size, uint32_t imageWidth) {
        const uint32_t uls = (command.w0 >> 12) & 0xFFF;
        const uint32_t ult = command.w0 & 0xFFF;
        const uint32_t lrs = (command.w1 >> 12) & 0xFFF;
        const uint32_t lrt = command.w1 & 0xFFF;
        if ((lrs < uls) || (lrt < ult)) {
            return 0;
        }

        const uint64_t offsetX = uls >> kTextureImageFrac;
        const uint64_t offsetY = ult >> kTextureImageFrac;
        const uint64_t width = ((lrs - uls) >> kTextureImageFrac) + 1;
        const uint64_t height = ((lrt - ult) >> kTextureImageFrac) + 1;

        uint64_t bytesPerLine = TextureBytesForPixels(imageWidth, size);
        uint64_t offsetBytes = TextureBytesForPixels(offsetX, size);
        if (size == 0) {
            offsetBytes = offsetX / 2;
        }

        const uint64_t tileLineBytes = TextureBytesForPixels(width, size);
        return (offsetY * bytesPerLine) + offsetBytes + ((height - 1) * bytesPerLine) + tileLineBytes;
    }

    static uint64_t LoadTlutCopyBytes(const N64Gfx& command) {
        const uint32_t highIndex = (command.w1 >> 14) & 0x3FF;
        return (static_cast<uint64_t>(highIndex) + 1) * 2;
    }

    size_t EstimateRawTextureCopyBytes(const N64Gfx* source, size_t index, size_t limit, size_t stride, bool isBig) const {
        /* ReadCommand is an 8-byte reader (w1 at +4), but a wide 16-byte packet stores w1 at +8
           with zero padding at +4. Without this compensation every w1-derived load extent
           (LOADBLOCK lrs, LOADTILE lrt/lrs, TLUT count) reads as 0 on wide lists, the estimate
           collapses to kMinRawTextureCopyBytes, and LUS decodes a full tile out of an 8-byte copy.
           w0 sits at offset 0 in both layouts and needs no correction. */
        const bool sourceIsWide = (stride == kHostBuiltGfxStride);
        const auto readScanCommand = [&](size_t i) {
            N64Gfx command = ReadCommand(source, i, stride, isBig);
            if (sourceIsWide) {
                uintptr_t w1full = 0;
                std::memcpy(&w1full, reinterpret_cast<const uint8_t*>(source) + i * stride + 8,
                            sizeof(w1full));
                command.w1 = static_cast<uint32_t>(w1full);
            }
            return command;
        };
        const N64Gfx setImg = readScanCommand(index);
        const uint32_t size = (setImg.w0 >> 19) & 0x3;
        const uint32_t imageWidth = (setImg.w0 & 0xFFF) + 1;
        uint64_t required = kMinRawTextureCopyBytes;

        const size_t scanEnd = std::min(limit, index + 1 + kTextureLoadScanCommandLimit);
        for (size_t i = index + 1; i < scanEnd; i++) {
            const N64Gfx command = readScanCommand(i);
            switch (Opcode(command.w0)) {
                case kOpSetTextureImage:
                case kOpEndDl:
                    i = scanEnd;
                    break;
                case kOpLoadBlock:
                    required = std::max(required, LoadBlockCopyBytes(command, size, imageWidth));
                    break;
                case kOpLoadTile:
                    required = std::max(required, LoadTileCopyBytes(command, size, imageWidth));
                    break;
                case kOpLoadTlut:
                    required = std::max(required, LoadTlutCopyBytes(command));
                    break;
                default:
                    break;
            }
        }

        if ((required == 0) || (required > kMaxRawTextureCopyBytes)) {
            return 0;
        }
        return static_cast<size_t>(required);
    }

    uintptr_t TranslateTexturePointer(uint32_t raw, const N64Gfx* source, size_t index, size_t limit, bool isBig,
                                      size_t stride) {
        /* Resolve with the actual upcoming load size so TryResolveAddress's readability gates can
           reject short false matches -- e.g. a segment base plus a truncated-module-pointer offset
           readable for only a few bytes -- instead of feeding a partial buffer to the copy. */
        const size_t estimatedBytes = EstimateRawTextureCopyBytes(source, index, limit, stride, isBig);
        const uintptr_t translated = TranslateDataPointer(raw, std::max<size_t>(estimatedBytes, 1));
        if (translated == 0) {
            static int sMissingTexturePointerPrints = 0;
            if (sMissingTexturePointerPrints < 200) {
                gdx_port_logf("[texdiag] unresolved G_SETTIMG pointer raw=%08X\n", raw);
                sMissingTexturePointerPrints++;
            }
            return 0;
        }

        /* [digit-carve] one-shot diagnostic, strip later: the rank digits (aPositionDigitTexs,
           seg4+0x13DE0) are zero-filled in ROM and composed at runtime on console, so this dump
           says whether the port's carve ever receives that content. Mode-gated rather than
           gGdxRaceActive-gated because Course Edit sets that latch too and spends the one-shot.
           0x01 is GAMEMODE_GP_RACE. */
        if ((gGameMode & 0x1F) == 0x01 && gSegments[4] != 0) {
            static bool sDigitCarveLogged = false;
            if (!sDigitCarveLogged) {
                sDigitCarveLogged = true;
                const uint8_t* d = reinterpret_cast<const uint8_t*>(gSegments[4]) + 0x13DE0;
                gdx_port_logf("[digit-carve] gSegments[4]+13DE0: "
                              "%02X%02X%02X%02X%02X%02X%02X%02X %02X%02X%02X%02X%02X%02X%02X%02X\n",
                              d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                              d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
            }
        }
        // Readable extent at `translated`. gHostRanges is consulted first because the page-level
        // limit is coarse: the 8 MB RDRAM calloc is one VirtualAlloc block, so ReadableByteLimit
        // would report 7+ MB remaining for a pointer anywhere inside it.
        const size_t registeredRemaining = RegisteredHostRemaining(translated);
        size_t readable = registeredRemaining;
        if (readable == 0) readable = ReadableByteLimit(translated);

        size_t required = estimatedBytes;
        if (required == 0) {
            static int sBadTextureEstimatePrints = 0;
            if (sBadTextureEstimatePrints < 200) {
                gdx_port_logf("[texdiag] zero/oversized texture estimate raw=%08X translated=%p\n",
                              raw, reinterpret_cast<const void*>(translated));
                sBadTextureEstimatePrints++;
            }
            required = std::min(readable, kMaxRawTextureCopyBytes);
        }
        
        // Always clamp to what is actually readable to avoid page faults
        required = std::min(required, readable);

        bool textureCopyRefreshed = false;
        uintptr_t outPtr = MakePersistentRawTextureCopy(translated, required, &textureCopyRefreshed);
        if (mStats != nullptr && textureCopyRefreshed) {
            mStats->textureCopyBytes += required;
        }
        if (outPtr == 0) {
            static int sTextureCopyFailPrints = 0;
            if (sTextureCopyFailPrints < 32) {
                gdx_port_logf("[texdiag] raw texture copy failed raw=%08X host=%p required=%zu vq=%zu reg=%zu\n",
                              raw, reinterpret_cast<const void*>(translated), required,
                              ReadableByteLimit(translated), RegisteredHostRemaining(translated));
                sTextureCopyFailPrints++;
            }
        }
        (void)textureCopyRefreshed;
        return outPtr;
    }

    const N64Gfx* ResolveDisplayListSource(uint32_t raw) const {
        ResolvedAddress resolved = {};
        if (TryResolveAddress(raw, resolved)) {
            if (!IsReadableAddress(resolved.full)) {
                return nullptr;
            }
            return reinterpret_cast<const N64Gfx*>(resolved.full);
        }
        return nullptr;
    }

    bool IsResolvableDisplayList(uint32_t raw, const N64Gfx** outTarget = nullptr) const {
        const N64Gfx* target = ResolveDisplayListSource(raw);
        if (target == nullptr) {
            return false;
        }

        const size_t limit = KnownCommandLimit(target);
        if ((limit == 0) || !LooksLikeDisplayList(target, limit)) {
            return false;
        }

        if (outTarget != nullptr) {
            *outTarget = target;
        }
        return true;
    }

    uintptr_t TranslateDisplayListPointer(uint32_t raw, const N64Gfx* parentSource = nullptr, size_t parentIndex = 0,
                                          const N64Gfx* directTarget = nullptr) {
        /* Resolve the EXACT token first: generated asset symbols are one-byte host stubs and are
         * not necessarily 8-byte aligned, so masking first collapses distinct symbols into an
         * unrelated texture. Genuine N64 DL addresses still get the hardware-compatible alignment
         * fallback, but only after the exact candidate proves absent or invalid.
         *
         * A wide (host-built) parent already carries the real host pointer in directTarget; use it
         * verbatim, but still validate it and route it through EnqueueList, since the interpreter
         * cannot consume a raw wide-decomp Gfx list directly. */
        const N64Gfx* target = (directTarget != nullptr) ? directTarget : ResolveDisplayListSource(raw);
        const auto isValidTarget = [this](const N64Gfx* candidate) {
            if (candidate == nullptr) {
                return false;
            }
            const size_t candidateLimit = KnownCommandLimit(candidate);
            return (candidateLimit != 0) && LooksLikeDisplayList(candidate, candidateLimit);
        };

        if (directTarget == nullptr && !isValidTarget(target)) {
            const uint32_t alignedRaw = raw & ~static_cast<uint32_t>(7u);
            if (alignedRaw != raw) {
                const N64Gfx* alignedTarget = ResolveDisplayListSource(alignedRaw);
                if (isValidTarget(alignedTarget)) {
                    raw = alignedRaw;
                    target = alignedTarget;
                }
            }
        }

        if (target == nullptr) {
            /* Budget split by race phase: one global budget is exhausted by menu/transition frames
               long before a race starts, so a missing race-time DL never gets logged. Always-on
               error family, but bounded in both phases so it cannot flood boot menus. */
            static int sMissingDlPrintsMenu = 0;
            static int sMissingDlPrintsRace = 0;
            int& missingBudget = (gGdxRaceActive != 0) ? sMissingDlPrintsRace : sMissingDlPrintsMenu;
            const int missingCap = (gGdxRaceActive != 0) ? 400 : 40;
            if (missingBudget < missingCap) {
                ++missingBudget;
                const uintptr_t parent = reinterpret_cast<uintptr_t>(parentSource);
                const size_t parentStride = parentSource ? CommandStrideForSource(parentSource) : kN64GfxStride;
                const N64Gfx parentCmd = parentSource ? ReadRawCommand(parentSource, parentIndex, parentStride) : N64Gfx{};
                gdx_port_logf("[gdl-miss] race=%d raw=%08X parent=%p index=%zu w0=%08X "
                              "seg0=%p seg1=%p seg2=%p seg3=%p seg8=%p\n",
                              gGdxRaceActive,
                              raw,
                              reinterpret_cast<const void*>(parent),
                              parentIndex,
                              parentCmd.w0,
                              reinterpret_cast<void*>(gSegments[0]),
                              reinterpret_cast<void*>(gSegments[1]),
                              reinterpret_cast<void*>(gSegments[2]),
                              reinterpret_cast<void*>(gSegments[3]),
                              reinterpret_cast<void*>(gSegments[8]));
            }
            if (mStats != nullptr) {
                mStats->noopDisplayLists++;
                if (mStats->firstNoopDlRaw == 0) mStats->firstNoopDlRaw = raw;
                mStats->missingDisplayLists++;
                if (mStats->firstMissingDlRaw == 0) {
                    mStats->firstMissingDlRaw = raw;
                    mStats->firstMissingParent = reinterpret_cast<uintptr_t>(parentSource);
                    mStats->firstMissingParentIndex = parentIndex;
                    if (parentSource != nullptr) {
                        const size_t stride = CommandStrideForSource(parentSource);
                        const bool isBig = CommandSourceIsBigEndian(parentSource, stride);
                        const N64Gfx rawParent = ReadRawCommand(parentSource, parentIndex, stride);
                        const N64Gfx decodedParent = ReadCommand(parentSource, parentIndex, stride, isBig);
                        mStats->firstMissingParentStride = stride;
                        mStats->firstMissingParentBigEndian = isBig;
                        mStats->firstMissingParentF3D =
                            IsF3DAssetPointer(reinterpret_cast<uintptr_t>(parentSource));
                        mStats->firstMissingParentRawW0 = rawParent.w0;
                        mStats->firstMissingParentRawW1 = rawParent.w1;
                        mStats->firstMissingParentDecodedW0 = decodedParent.w0;
                        mStats->firstMissingParentDecodedW1 = decodedParent.w1;
                    }
                }
            }
            return reinterpret_cast<uintptr_t>(mNoopList.data());
        }

        const size_t limit = KnownCommandLimit(target);
        if ((limit == 0) || !LooksLikeDisplayList(target, limit)) {
            /* Race-phase-split budget, same reasoning as [gdl-miss] above. */
            static int sBadDlPrintsMenu = 0;
            static int sBadDlPrintsRace = 0;
            int& badBudget = (gGdxRaceActive != 0) ? sBadDlPrintsRace : sBadDlPrintsMenu;
            const int badCap = (gGdxRaceActive != 0) ? 400 : 40;
            if (badBudget < badCap) {
                ++badBudget;
                const uint32_t alignedRaw = raw & ~7u;
                const N64Gfx* alignedTarget = (alignedRaw != raw) ? ResolveDisplayListSource(alignedRaw) : nullptr;
                const size_t targetStride = CommandStrideForSource(target);
                const N64Gfx first = (limit > 0) ? ReadCommand(target, 0, targetStride, CommandSourceIsBigEndian(target, targetStride)) : N64Gfx{};
                const size_t parentStride = parentSource ? CommandStrideForSource(parentSource) : kN64GfxStride;
                const N64Gfx parentCmd = parentSource ? ReadRawCommand(parentSource, parentIndex, parentStride) : N64Gfx{};
                gdx_port_logf("[gdl-bad] race=%d raw=%08X target=%p limit=%zu first=%08X "
                              "alignedRaw=%08X aligned=%p alignedFirst=%08X parent=%p index=%zu w0=%08X\n",
                              gGdxRaceActive,
                              raw,
                              reinterpret_cast<const void*>(target),
                              limit,
                              first.w0,
                              alignedRaw,
                              reinterpret_cast<const void*>(alignedTarget),
                              alignedTarget ? ReadRawCommand(alignedTarget, 0, CommandStrideForSource(alignedTarget)).w0 : 0,
                              reinterpret_cast<const void*>(parentSource),
                              parentIndex,
                              parentCmd.w0);
            }
            if (mStats != nullptr) {
                mStats->noopDisplayLists++;
                if (mStats->firstNoopDlRaw == 0) mStats->firstNoopDlRaw = raw;
                mStats->badDisplayLists++;
                if (mStats->firstBadDlRaw == 0) {
                    const size_t stride = CommandStrideForSource(target);
                    const bool isBig = CommandSourceIsBigEndian(target, stride);
                    const size_t scanLimit = std::min(limit, kDisplayListValidationCommandLimit);
                    mStats->firstBadDlRaw = raw;
                    mStats->firstBadDlTarget = reinterpret_cast<uintptr_t>(target);
                    mStats->firstBadDlLimit = limit;
                    mStats->firstBadDlStride = stride;
                    mStats->firstBadDlBigEndian = isBig;
                    mStats->firstBadDlF3D = IsF3DAssetPointer(reinterpret_cast<uintptr_t>(target));
                    if (limit == 0) {
                        mStats->firstBadDlFailureReason = 1;
                    } else {
                        const N64Gfx first = ReadCommand(target, 0, stride, isBig);
                        mStats->firstBadDlFirstW0 = first.w0;
                        mStats->firstBadDlFirstW1 = first.w1;
                        mStats->firstBadDlFailureReason = 3;
                        mStats->firstBadDlFailureIndex = scanLimit;
                        for (size_t i = 0; i < scanLimit; ++i) {
                            const N64Gfx command = ReadCommand(target, i, stride, isBig);
                            const uint8_t op = Opcode(command.w0);
                            if (!IsLikelyDisplayListOpcode(op)) {
                                mStats->firstBadDlFailureReason = 2;
                                mStats->firstBadDlFailureIndex = i;
                                mStats->firstBadDlFailureOpcode = op;
                                break;
                            }
                            if ((op == kOpEndDl) || (op == 0xB8u)) {
                                break;
                            }
                        }
                    }
                }
            }
            return reinterpret_cast<uintptr_t>(mNoopList.data());
        }

        return EnqueueList(target, limit);
    }

    size_t EffectiveLimit(const N64Gfx* source, size_t explicitLimit) const {
        const size_t knownLimit = KnownCommandLimit(source);
        if ((explicitLimit != 0) && (knownLimit != 0)) return std::min(explicitLimit, knownLimit);
        if (explicitLimit != 0) return explicitLimit;
        if (knownLimit != 0) return knownLimit;
        return kMaxUnboundedDisplayListCommands;
    }

    bool LooksLikeDisplayList(const N64Gfx* source, size_t limit) const {
        const size_t scanLimit = std::min(limit, kDisplayListValidationCommandLimit);
        const size_t stride = CommandStrideForSource(source);
        const bool isBig = CommandSourceIsBigEndian(source, stride);
        for (size_t i = 0; i < scanLimit; i++) {
            const N64Gfx command = ReadCommand(source, i, stride, isBig);
            const uint8_t op = Opcode(command.w0);
            if (!IsLikelyDisplayListOpcode(op)) return false;
            if (op == kOpEndDl || op == 0xB8u) return true;  // 0xB8 = F3D G_ENDDL
        }
        return false;
    }

    // --- Segment-reload seqlock guards -------------------------------------
    // Each guard snapshots the epoch before a resolution and re-checks it after; a resolution that
    // raced a mode-transition reload (torn base or torn bytes) is reported unstable and the caller
    // substitutes an opcode-specific fallback. Wide packets carrying a real host pointer never read
    // gSegments[], so they bypass the guard. Wait-free: two acquire loads per guarded command, no
    // locks, no allocation.

    // Rate-limited skip notice shared by every guarded site.
    void NoteEpochSkip() {
        if (mStats != nullptr) mStats->skippedEpochRetries++;
        static int sEpochSkipLogs = 0;
        if (sEpochSkipLogs < 40) {
            ++sEpochSkipLogs;
            gdx_port_logf("[seg-epoch] segment reload raced a translate; command "
                          "dropped/fell back this frame (n=%d)\n", sEpochSkipLogs);
        }
    }

    // Guarded TranslateDataPointer. Writes the resolved pointer to `out` and
    // returns true if stable; on a raced reload it counts+logs the skip and
    // returns false (caller applies its fallback). `out` is written in every
    // case, but the caller must overwrite it on a false return.
    bool ResolveGuarded(uint32_t raw, bool hostPtr, uintptr_t hostFull, uintptr_t& out,
                        size_t requiredBytes = 1, bool preferPhysical = false,
                        uintptr_t sourceHint = 0) {
        if (hostPtr) {
            out = hostFull;
            return true;
        }
        const uint32_t epoch = GdxSegmentEpochSnapshot();
        out = TranslateDataPointer(raw, requiredBytes, preferPhysical, sourceHint);
        if (GdxSegmentEpochStable(epoch)) return true;
        NoteEpochSkip();
        return false;
    }

    // Guarded TranslateDisplayListPointer. On a raced reload returns the noop DL
    // (mNoopList) -- the same fallback TranslateDisplayListPointer itself uses
    // for an unresolved target -- rather than a possibly-torn sub-list pointer.
    uintptr_t ResolveDisplayListGuarded(uint32_t raw, const N64Gfx* parentSource,
                                        size_t parentIndex, bool hostPtr, uintptr_t hostFull) {
        // Host-pointer packets never read gSegments[], so skip the epoch snapshot
        // entirely for them (mirrors the hostPtr fast path in ResolveGuarded above).
        if (hostPtr) {
            return TranslateDisplayListPointer(raw, parentSource, parentIndex,
                                                reinterpret_cast<const N64Gfx*>(hostFull));
        }
        const uint32_t epoch = GdxSegmentEpochSnapshot();
        // Pre-check because TranslateDisplayListPointer unconditionally bumps the miss/bad counters
        // and spends [gdl-miss]/[gdl-bad] log budget; an already-unstable snapshot would pollute
        // both with a result that gets discarded anyway.
        if (!GdxSegmentEpochStable(epoch)) {
            NoteEpochSkip();
            return reinterpret_cast<uintptr_t>(mNoopList.data());
        }
        const uintptr_t resolved = TranslateDisplayListPointer(raw, parentSource, parentIndex, nullptr);
        // A race beginning during the call still pollutes at most one call's counters, unlike the
        // always-hit case the pre-check eliminates.
        if (GdxSegmentEpochStable(epoch)) return resolved;
        NoteEpochSkip();
        return reinterpret_cast<uintptr_t>(mNoopList.data());
    }

  public:
    Fast::F3DGfx* ConvertRoot() {
        if (mRootBegin == nullptr) return nullptr;
        const size_t rootLimit = RootCommandLimit(mRootBegin);
        /* CRASH FAILSAFE: a ROOT display list that does not validate must render NOTHING. Feeding
           it to the interpreter executes arbitrary host memory as GBI commands. Sub-DLs already
           route to mNoopList on a LooksLikeDisplayList failure, but the ROOT is enqueued directly
           and bypasses that guard. The N64 task DL is always terminated by gSPEndDisplayList
           (Gfx_FullSync), so a well-formed root validates well within the scan limit; a
           truncated/garbage/zeroed one does not, and the whole frame is skipped. */
        if (rootLimit == 0 ||
            ReadableByteLimit(reinterpret_cast<uintptr_t>(mRootBegin)) <
                CommandStrideForSource(mRootBegin) ||
            !LooksLikeDisplayList(mRootBegin, rootLimit)) {
            static int sRootRejectLogs = 0;
            if (sRootRejectLogs < 16) {
                ++sRootRejectLogs;
                gdx_port_logf("[gfxfail] ROOT rejected: ptr=%p limit=%zu readable=%zuB "
                              "-- rendering nothing this frame\n",
                              reinterpret_cast<const void*>(mRootBegin), rootLimit,
                              ReadableByteLimit(reinterpret_cast<uintptr_t>(mRootBegin)));
            }
            return nullptr;
        }
        EnqueueList(mRootBegin, rootLimit);
        while (!mWorkQueue.empty()) {
            QueueItem item = mWorkQueue.back();
            mWorkQueue.pop_back();
            ProcessList(item);
        }
        return mLists[mRootBegin]->commands.data();
    }

    void ProcessList(QueueItem item) {
        if (mStats != nullptr) mStats->convertedLists++;
        item.listPtr->commands.reserve(item.limit);

        /* [seg4] probe, strip later: hud_gfx (countdown faces, start arc) has real backing yet
           does not draw. Logs whether those display lists reach the bridge at all. */
        {
            static int sSeg4Probes = 0;
            const uintptr_t seg4Base = gSegments[4];
            const uintptr_t src = reinterpret_cast<uintptr_t>(item.source);
            if (seg4Base != 0 && src >= seg4Base && src < seg4Base + 0x29EA0 && sSeg4Probes < 12) {
                ++sSeg4Probes;
                gdx_port_logf("[seg4] DL translated from seg4+0x%X limit=%zu\n",
                              static_cast<unsigned>(src - seg4Base), item.limit);
            }
        }

        /* RDRAM/ROM-loaded asset pointers use 8-byte N64 command slots. Some loaded
         * GFX ranges are already fixup-swapped to host endian, while untagged ROM GFX
         * ranges remain big-endian; detect endian from the command stream itself.
         * BSS/module/GfxPool pointers hold host-built data (little-endian).
         * Use the physical location rather than a heuristic to avoid false positives. */
        const size_t stride = CommandStrideForSource(item.source);
        const bool isBig = CommandSourceIsBigEndian(item.source, stride);
        // DLs inside a tagged asset segment have a known dialect and must never fall back to the
        // opcode scan. A converted wide buffer has lost its source segment's tag, so the dialect
        // the converter recorded is consulted FIRST -- re-deriving it from the wide stream risks
        // the exact F3DEX2/F3D misclassification the tag table exists to prevent.
        const GdxSegmentUcode sourceDialect =
            GdxAssetPointerDialect(reinterpret_cast<uintptr_t>(item.source));
        const auto convertedDialect = gConvertedWideIsF3d.find(item.source);
        const bool isF3DSource =
            (convertedDialect != gConvertedWideIsF3d.end()) ? convertedDialect->second
            : (sourceDialect == GdxSegmentUcode::F3D)    ? true
            : (sourceDialect == GdxSegmentUcode::F3DEX2) ? false
            : (IsF3DAssetPointer(reinterpret_cast<uintptr_t>(item.source)) ||
               DisplayListUsesF3D(item.source, item.limit, stride, isBig));
        if (isF3DSource && mStats != nullptr) {
            mStats->f3dLists++;
        }

        // [setupdl] diagnostic, strip later: how the course material setup DLs (segment 8 +0x14040
        // / +0x14078) classify and convert. Their SETTILEs program tiles 1-7 for every track draw,
        // so a misconversion here breaks all course tiling.
        bool diagThisList = false;
        if (gdx_dev_gate(GDX_GATE_DIAG_SETUPDL)) {
            /* Both DLs are BSS placeholders redirected into the bridge's own segment-8 image, so
               item.source is never gSegments[8] + offset; anchor on the generated asset rows.
               gLoadedAssetSegments is only ever appended to, so resolved addresses are cacheable. */
            static uintptr_t sSetupDlA = 0;
            static uintptr_t sSetupDlB = 0;
            if (sSetupDlA == 0 || sSetupDlB == 0) {
                uint32_t offA = 0;
                uint32_t offB = 0;
                const uintptr_t baseA =
                    EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(D_8014040)), &offA);
                const uintptr_t baseB =
                    EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(D_8014078)), &offB);
                if (baseA != 0) sSetupDlA = baseA + offA;
                if (baseB != 0) sSetupDlB = baseB + offB;
            }
            const uintptr_t src = reinterpret_cast<uintptr_t>(item.source);
            if (src != 0 && (src == sSetupDlA || src == sSetupDlB)) {
                static int sSetupDlDumps = 0;
                if (sSetupDlDumps < 40) {
                    ++sSetupDlDumps;
                    diagThisList = (sSetupDlDumps <= 2);
                    gdx_port_logf("[setupdl] source=%p sym=%s stride=%zu big=%d f3d=%d limit=%zu race=%d\n",
                                  item.source, (src == sSetupDlA) ? "D_8014040" : "D_8014078", stride,
                                  (int)isBig, (int)isF3DSource, item.limit, gGdxRaceActive);
                }
            }
        }

        /* Set while an unsupported microcode is active: its SP-side commands (op < 0xE0) have
           semantics the F3DEX2 interpreter does not implement, so running them produces garbage.
           Drop them until the list loads a supported microcode again. */
        bool skipUnsupportedUcode = false;
        /* True while an L3DEX2 (line microcode) section is being converted: its non-line
           commands are F3DEX2-compatible and run as Standard, and each G_LINE3D (0x08) is
           rewritten into OTR_G_LINE3D_GDX for the interpreter's screen-space quad expansion.
           Cleared by the next recognized microcode load. */
        bool l3dexLineSection = false;

        // Host-built lists carry a FULL pointer-width w1: the decomp Gfx type under PORT is
        // { u32 w0; <pad>; uintptr_t w1; } = 16 bytes, so the pointer word is at offset 8, not 4.
        // ReadCommand only recovers 8 bytes (w0 @0, padding @4 on wide packets), so wide sources
        // pull the real 64-bit word here and set in.w1 to its low 32 bits -- the operand parsing
        // below still expects a 32-bit token. Narrow N64/ROM/RDRAM lists are unaffected.
        const bool sourceIsWide =
            (stride == kHostBuiltGfxStride) && (kHostBuiltGfxStride > kN64GfxStride);

        for (size_t i = 0; i < item.limit; i++) {
            N64Gfx in = ReadCommand(item.source, i, stride, isBig);
            uintptr_t w1full;
            if (sourceIsWide) {
                std::memcpy(&w1full,
                            reinterpret_cast<const uint8_t*>(item.source) + i * stride + 8,
                            sizeof(w1full));
                in.w1 = static_cast<uint32_t>(w1full);
            } else {
                w1full = in.w1;
            }
            /* A wide packet whose pointer word has any high bits set is a REAL host pointer the
             * game already resolved: use it verbatim, no low32 reconstruction. High bits zero
             * means a 32-bit VALUE or a segmented address, which still flows through the segment
             * table / value path. Narrow sources never set high bits. */
            bool w1IsHostPointer =
                sourceIsWide && ((static_cast<uint64_t>(w1full) >> 32) != 0);
            /* EXCEPTION to that rule: the game references runtime-loaded segmented assets
             * (setup_gfx render-mode DLs via gSPDisplayList(&D_3000050), vertex data via
             * gSPVertex(&D_3000xxx)) through 1-byte BSS PLACEHOLDER symbols. The wide packet
             * carries the full host address of the placeholder, so high32 is set and it looks like
             * a real pointer -- but the object is 1 byte and the real bytes live in the loaded
             * segment image. Taken verbatim, the interpreter branches into the stub and reads
             * adjacent BSS as commands (odd branch targets, a G_GEOMETRYMODE word misread as a
             * vertex pointer, a count=0xF0 G_VTX). Route these back through the low32 resolver.
             * Genuine runtime host pointers are never in the asset map and are unaffected. */
            if (w1IsHostPointer &&
                (IsAssetPlaceholderPointer(in.w1) || IsPortBssAliasPointer(in.w1))) {
                w1IsHostPointer = false;
            }
            /* Second EXCEPTION: high32 == 0xFFFFFFFF is a SIGN-EXTENDED 32-bit value, not a host
             * pointer -- user space never has an all-ones top half. Decomp code widens a
             * bit31-set token through a signed cast into the wide list, and taking it verbatim had
             * LUS memcpy from kernel space (GfxDpLoadBlock from 0xFFFFFFFFF8694130 on Course Edit
             * entry). Strip to low32 and route through the resolver like a narrow command. */
            if (w1IsHostPointer && (static_cast<uint64_t>(w1full) >> 32) == 0xFFFFFFFFull) {
                static int sSignExtLogs = 0;
                if (sSignExtLogs < 8) {
                    sSignExtLogs++;
                    gdx_port_logf("[signext] wide w1=%016llX op=%02X routed to low32 resolver\n",
                                  static_cast<unsigned long long>(w1full),
                                  static_cast<unsigned>(Opcode(in.w0)));
                }
                w1IsHostPointer = false;
            }
            const uint8_t op = Opcode(in.w0);
            // Tags any guessing branch that fires while resolving this command with the opcode
            // that triggered it; free functions read this global instead of a threaded param.
            gLegacyResolveCurrentOp = op;
            /* Three things survive the skip. 0xDD, because a ucode reload ends it. ENDDL
               (0xDF EX2 / 0xB8 F3D), because dropping the terminator leaves the translated list
               unterminated and the interpreter runs off its end. And every RDP command (op >=
               0xE0), because the RDP executes those, not the RSP microcode -- on hardware they
               behave identically whichever ucode is loaded, so dropping them leaves color
               image / scissor / prim color / combine stale for whatever draws right after the
               section (course_edit/191080.c reloads F3DEX2 and keeps drawing into the same
               viewport). Deliberately NOT counted in skippedDataCommands: that counter drives the
               [datafail] line and an intentional skip would spam it every Course Edit frame. */
            if (skipUnsupportedUcode && op != 0xDD && op != 0xDF && op != 0xB8 && op < 0xE0) {
                continue;
            }
            if (diagThisList && i < 24) {
                gdx_port_logf("[setupdl]   #%02zu %08X %08X op=%02X\n", i, in.w0, in.w1, op);
            }
            /* [trect] probe, strip later. GDX_DIAG_TRECT=1, zero cost otherwise. The budget resets
             * per TRANSITION INSTANCE (activeTransitionType is sTransition's first s32) rather
             * than per process: a fade emits ~74 strip texrects, so a process-lifetime cap
             * silences everything after the first instance and makes "only N reach the bridge"
             * an artifact of the cap. A summary line fires when the next instance begins, giving a
             * definitive total for the one that just ended. */
            if (op == 0xE4 || op == 0xE5) {
                if (gdx_dev_gate(GDX_GATE_DIAG_TRECT)) {
                    static int32_t sTRectLastType = 0; // TRANSITION_TYPE_NONE
                    static int sTRectIndex = 0;        // running index within the current instance
                    static int sTRectTotal = 0;        // total TEXRECTs seen this instance
                    static uint32_t sTRectLastUlx = 0, sTRectLastUly = 0, sTRectLastLrx = 0, sTRectLastLry = 0;
                    static uint32_t sTRectLastTile = 0;
                    const int32_t curType = *reinterpret_cast<const int32_t*>(&sTransition[0]);
                    if (curType != sTRectLastType) {
                        if (sTRectLastType != 0 && sTRectTotal != 0) {
                            gdx_port_logf("[trect] transition type=%d ENDED: %d texrects reached the bridge "
                                          "(last ul=(%u,%u) lr=(%u,%u) tile=%u)\n",
                                          sTRectLastType, sTRectTotal, sTRectLastUlx, sTRectLastUly, sTRectLastLrx,
                                          sTRectLastLry, sTRectLastTile);
                        }
                        sTRectLastType = curType;
                        sTRectIndex = 0;
                        sTRectTotal = 0;
                    }
                    const uint32_t lrx = (in.w0 >> 12) & 0xFFF;
                    const uint32_t lry = in.w0 & 0xFFF;
                    const uint32_t tile = (in.w1 >> 24) & 0x7;
                    const uint32_t ulx = (in.w1 >> 12) & 0xFFF;
                    const uint32_t uly = in.w1 & 0xFFF;
                    sTRectLastUlx = ulx;
                    sTRectLastUly = uly;
                    sTRectLastLrx = lrx;
                    sTRectLastLry = lry;
                    sTRectLastTile = tile;
                    ++sTRectTotal;
                    if (sTRectIndex < 16 || (sTRectIndex % 16) == 0) {
                        gdx_port_logf("[trect] type=%d #%d op=%02X ul=(%u,%u) lr=(%u,%u) tile=%u stride=%zu "
                                      "f3d=%d big=%d\n",
                                      curType, sTRectIndex, op, ulx, uly, lrx, lry, tile, stride, (int)isF3DSource,
                                      (int)isBig);
                    }
                    ++sTRectIndex;
                }
            }
            uintptr_t outW0 = static_cast<uintptr_t>(in.w0);
            uintptr_t outW1 = static_cast<uintptr_t>(in.w1);
            if (mStats != nullptr) mStats->opCounts[op]++;

            /* Countdown raw-value trace, strip later: dumps every vtx/mtx w0/w1 so the command
               stream can be diffed against decomp-side low32 values with no count filter, which
               could itself hide the match (e.g. wrong endianness changing the parsed count). */
            {
                const bool sDiagCountdownRaw = gdx_dev_gate(GDX_GATE_DIAG_COUNTDOWN) != 0;
                // Gated on the arm flag, not gGdxRaceActive: that stays 1 for the whole race and
                // the fixed-size trace fills long before the countdown appears.
                if (sDiagCountdownRaw && gGdxCountdownProbeArm != 0 && (op == kOpVtx || op == kOpMtx)) {
                    static int sRawTraceCount = 0;
                    static FILE* sRawTraceFile = nullptr;
                    if (sRawTraceCount == 0) {
                        sRawTraceFile = fopen("vtx-mtx-trace.txt", "w");
                    }
                    if (sRawTraceFile != nullptr && sRawTraceCount < 400000) {
                        ++sRawTraceCount;
                        fprintf(sRawTraceFile, "op=%02X w0=%08X w1=%08X f3d=%d big=%d src=%p i=%zu\n",
                                op, in.w0, in.w1, (int)isF3DSource, (int)isBig, item.source, i);
                        if ((sRawTraceCount % 500) == 0) {
                            fflush(sRawTraceFile);
                        }
                    }
                }
            }

            switch (op) {
                case kOpVtx:
                    // F3D uses opcode 0x01 for G_MTX (not G_VTX). Remap to kOpMtx so Fast3D
                    // doesn't try to load a 64-byte matrix struct as a vertex buffer. The
                    // parameter flags also differ: legacy F3D stores them in w0[23:16],
                    // while F3DEX2 stores its XOR-with-PUSH form in w0[7:0].
                    if (isF3DSource) {
                        const uint8_t legacy = static_cast<uint8_t>((in.w0 >> 16) & 0xFFu);
                        uint8_t parameters = 0;
                        if ((legacy & 0x01u) != 0) parameters |= 0x04u; // projection
                        if ((legacy & 0x02u) != 0) parameters |= 0x02u; // load
                        if ((legacy & 0x04u) != 0) parameters |= 0x01u; // push
                        const uint8_t encoded = parameters ^ 0x01u;
                        outW0 = (static_cast<uintptr_t>(kOpMtx) << 24) |
                                static_cast<uintptr_t>(encoded);
                    }
                    {
                        /* The resolver must prove the FULL vertex payload readable, not just the
                           first byte: MakePersistentVtxCopy reads numVtx*16 unconditionally, and an
                           ambiguous resolution (segment-offset guess, physical-window
                           reconstruction) can pass a 1-byte gate yet be wrong past byte 0 -- one
                           garbage vertex is a visible stretched-polygon spike. */
                        const uint32_t vtxCount = (!isF3DSource) ? ((in.w0 >> 12) & 0xFFu) : 0u;
                        const size_t vtxRequiredBytes =
                            (vtxCount != 0) ? (static_cast<size_t>(vtxCount) * 16u) : 1u;
                        /* Mirrors the G_MTX case's preferPhysical/sourceHint arguments so vertex
                           buffers get trySourceWindow's last-resort reconstruction too -- it was
                           written for exactly this case, a host-built list and the vertices it
                           references sharing one 4 GB low32 window. */
                        if (!ResolveGuarded(in.w1, w1IsHostPointer, w1full, outW1, vtxRequiredBytes,
                                            /*preferPhysical=*/!isF3DSource,
                                            reinterpret_cast<uintptr_t>(item.source))) {
                            /* A raced vertex resolution must NEVER be skipped silently: later
                               triangles index the vertex buffer and would render garbage.
                               Substitute the readability failsafe's fallback, which also
                               neutralizes the possibly-torn pointer before any downstream deref. */
                            outW1 = isF3DSource ? 0u
                                                : reinterpret_cast<uintptr_t>(kFallbackVertices);
                        }
                        /* [vtx-census] diagnostic, strip later: one line per unique vertex source,
                           with a CONTENT fingerprint at +0x10 -- the failure family it targets is
                           readable-but-zero/aliased data, invisible to the failsafe probes below. */
                        {
                            static uint32_t sVtxCensusSeen[48] = {};
                            static int sVtxCensusCount = 0;
                            bool vcDup = false;
                            for (int vc = 0; vc < sVtxCensusCount; vc++) {
                                if (sVtxCensusSeen[vc] == in.w1) {
                                    vcDup = true;
                                    break;
                                }
                            }
                            /* Segment-8 raws only: general sources burn the budget before race
                               time, and the decoration family is the open investigation. */
                            if (!vcDup && sVtxCensusCount < 48 && outW1 != 0 &&
                                ((in.w1 >> 24) & 0xFFu) == 0x08u) {
                                sVtxCensusSeen[sVtxCensusCount++] = in.w1;
                                const char* vcls = IsRdramHostPointer(outW1) ? "rdram"
                                                   : (mModuleBegin != 0 && outW1 >= mModuleBegin &&
                                                      outW1 < mModuleEnd)   ? "module"
                                                                             : "heap";
                                uint8_t vfp[8] = {};
                                if (ReadableByteLimit(outW1 + 0x10) >= sizeof(vfp)) {
                                    std::memcpy(vfp, reinterpret_cast<const void*>(outW1 + 0x10), sizeof(vfp));
                                }
                                gdx_port_logf("[vtx-census] raw=%08X host=%d out=%p cls=%s n=%u "
                                              "fp8=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                              in.w1, w1IsHostPointer ? 1 : 0,
                                              reinterpret_cast<void*>(outW1), vcls, vtxCount,
                                              vfp[0], vfp[1], vfp[2], vfp[3], vfp[4], vfp[5], vfp[6], vfp[7]);
                            }
                        }
                        // Deliberately always on, not env-gated: a run without the opt-in captured
                        // zero [vtx-spike]/[vtx-dropped] evidence while the spikes kept happening.
                        // Costs one branch plus a capped set of race-time log lines.
                        if (gGdxRaceActive != 0 && vtxCount != 0) {
                            if (outW1 != 0) {
                                const size_t readable = ReadableByteLimit(outW1);
                                static int sVtxSpikeLogs = 0;
                                if (readable < vtxRequiredBytes && sVtxSpikeLogs < 40) {
                                    ++sVtxSpikeLogs;
                                    gdx_port_logf("[vtx-spike] raw=%08X resolved=%p need=%zuB readable=%zuB count=%u src=%p\n",
                                                  in.w1, reinterpret_cast<void*>(outW1), vtxRequiredBytes, readable,
                                                  vtxCount, item.source);
                                }
                            } else {
                                /* The strict gate rejected every candidate. Log what a
                                   requiredBytes=1 rule would have accepted, so a spike that this
                                   turned into a dropped-vertex pop is still attributable. */
                                const uintptr_t loose = TranslateDataPointer(in.w1, 1);
                                static int sVtxDroppedLogs = 0;
                                if (sVtxDroppedLogs < 40) {
                                    ++sVtxDroppedLogs;
                                    gdx_port_logf("[vtx-dropped] raw=%08X count=%u need=%zuB "
                                                  "looseResolve=%p looseReadable=%zuB src=%p\n",
                                                  in.w1, vtxCount, vtxRequiredBytes,
                                                  reinterpret_cast<void*>(loose),
                                                  loose ? ReadableByteLimit(loose) : 0u, item.source);
                                }
                            }
                        }
                    }
                    if (outW1 != 0 && isBig && !isF3DSource) {
                        outW1 = MakePersistentVtxCopy(outW1, (outW0 >> 12) & 0xFF);
                    } else if (outW1 != 0 && isBig && isF3DSource) {
                        /* This command was just remapped to G_MTX: big-endian
                           static Mtx data needs the word-swapped copy. */
                        outW1 = MakePersistentMtxCopy(outW1);
                    }
                    if (outW1 != 0) {
                        outW1 = NormalizeLusDirectPointer(outW1);
                    }
                    /* CRASH FAILSAFE: host-built
                       little-endian display lists (the GfxPool root and its
                       sub-DLs) skip MakePersistentVtxCopy -- which is the ONLY
                       vertex path that clamps the read to the readable region --
                       so an under-validated / truncated / legacy-guessed vertex
                       pointer reached the interpreter verbatim and was
                       dereferenced in GfxSpVertex (interpreter.cpp), faulting on
                       the first real frame after the boot-logo hold. The
                       [vtx-spike] probe above only LOGGED this; it never stopped
                       the garbage pointer. Never hand the interpreter a vertex
                       pointer whose full payload is not readable: substitute the
                       zeroed, fully-readable kFallbackVertices buffer (a benign
                       degenerate load) for a real F3DEX2 vertex load, or drop a
                       remapped F3D matrix pointer to 0 (the interpreter tolerates
                       a null matrix, same as the [mtx-dropped] path). */
                    if (outW1 != 0) {
                        const size_t vtxNeedBytes =
                            isF3DSource ? 64u
                                        : static_cast<size_t>((in.w0 >> 12) & 0xFFu) * 16u;
                        if (vtxNeedBytes != 0 && ReadableByteLimit(outW1) < vtxNeedBytes) {
                            static int sVtxFailsafeLogs = 0;
                            if (sVtxFailsafeLogs < 40) {
                                ++sVtxFailsafeLogs;
                                gdx_port_logf("[vtx-failsafe] raw=%08X resolved=%p need=%zuB "
                                              "readable=%zuB f3d=%d src=%p -> %s\n",
                                              in.w1, reinterpret_cast<void*>(outW1), vtxNeedBytes,
                                              ReadableByteLimit(outW1), (int)isF3DSource, item.source,
                                              isF3DSource ? "dropped" : "fallback-vertices");
                            }
                            outW1 = isF3DSource
                                        ? 0u
                                        : reinterpret_cast<uintptr_t>(kFallbackVertices);
                        }
                    }
                    // Effects vertex lerp (booster flames / side-attack quads). Must sit AFTER the
                    // readability failsafe so a kFallbackVertices substitute is never rerouted. The
                    // count cap of 20 matches racer.c's largest effect batch; course track vertices
                    // can legally spill into the effects span in the EK layout (course.c:4514), and
                    // the cap is what keeps their batches out rather than mispairing static geometry.
                    {
                        // [vtx-interp] diagnostic, strip later: the first few pool-interior,
                        // effect-sized batches, which is the activation proof for this tier. Not
                        // race-gated -- a scripted race never boosts, so it cannot draw the effects
                        // this targets at all.
                        static int sGdxVtxAnyLogs = 0;
                        const uint32_t gdxVtxSeenN = (in.w0 >> 12) & 0xFFu;
                        if (sGdxVtxAnyLogs < 8 && mInterpEnabled && gdxVtxSeenN != 0 &&
                            gdxVtxSeenN <= 20u && GdxP0MtxInPoolSpan(outW1)) {
                            ++sGdxVtxAnyLogs;
                            gdx_port_logf("[vtx-interp] pool batch: op=%p poolOff=0x%zX n=%u "
                                          "(effects span 0x%zX..0x%zX)\n",
                                          reinterpret_cast<void*>(outW1),
                                          static_cast<size_t>(outW1 - static_cast<uintptr_t>(gSegments[1])),
                                          gdxVtxSeenN, gdx_gfxpool_effects_vtx_offset(),
                                          gdx_gfxpool_effects_vtx_offset() + gdx_gfxpool_effects_vtx_bytes());
                        }
                    }
                    if (mInterpEnabled && outW1 != 0 && !isBig && !isF3DSource) {
                        const uint32_t gdxVtxN = (in.w0 >> 12) & 0xFFu;
                        if (gdxVtxN != 0 && gdxVtxN <= 20u &&
                            GdxEffectsVtxInSpan(outW1, static_cast<size_t>(gdxVtxN) * 16u)) {
                            outW1 = GdxVtxReroute(outW1, gdxVtxN);
                        } else if (GdxP0MtxInPoolSpan(outW1)) {
                            // [vtx-interp] diagnostic, strip later: pool-interior vertex operands
                            // that fail the effects-span test, with the numbers needed to say
                            // whether the span or the count gate is what excluded them.
                            static int sGdxVtxMissLogs = 0;
                            if (sGdxVtxMissLogs < 6) {
                                ++sGdxVtxMissLogs;
                                gdx_port_logf("[vtx-interp] in-pool miss: op=%p poolOff=0x%zX n=%u "
                                              "(effects span 0x%zX..0x%zX)\n",
                                              reinterpret_cast<void*>(outW1),
                                              static_cast<size_t>(outW1 - static_cast<uintptr_t>(gSegments[1])),
                                              gdxVtxN, gdx_gfxpool_effects_vtx_offset(),
                                              gdx_gfxpool_effects_vtx_offset() + gdx_gfxpool_effects_vtx_bytes());
                            }
                        }
                    }
                    /* Diagnostic, strip later: publishes the resolved host pointer when this
                       command's raw low32 matches the digit quad tagged in racer.c, so
                       interpreter.cpp's GfxSpVertex can recognize that exact draw rather than
                       whatever triangle the coarse arm flag catches first. */
                    if (gGdxCountdownProbeArm != 0 && gGdxCountdownProbeVtxLow32 != 0 &&
                        in.w1 == gGdxCountdownProbeVtxLow32 && outW1 != 0) {
                        gGdxCountdownProbeResolvedVtx = outW1;
                    }
                    /* [rect] probe, strip later: the "3,2,1,GO" HUD quad is a 4-vertex textured
                       rect inside a 3D billboard. Dumps ob/tc for every 4-vertex load so the
                       countdown's object-space rect can be checked for degeneracy. */
                    {
                        const bool sDiagCountdown = gdx_dev_gate(GDX_GATE_DIAG_COUNTDOWN) != 0;
                        // Gated on the arm flag, not gGdxRaceActive: a blanket race gate lets ~60
                        // unrelated 4-vertex HUD quads exhaust the cap before the countdown runs.
                        if (sDiagCountdown && gGdxCountdownProbeArm != 0 && !isF3DSource &&
                            (((in.w0 >> 12) & 0xFFu) == 4u) && outW1 != 0) {
                            static int sRectLogs = 0;
                            if (sRectLogs < 200) {
                                ++sRectLogs;
                                // Read the raw N64 Vtx_t layout by hand (ob[3] s16, flag u16,
                                // tc[2] s16, cn[4] u8 = 16 bytes/vertex) -- same convention
                                // MakePersistentVtxCopy already uses -- rather than depending
                                // on a Vtx_t type possibly not visible/ODR-safe in this TU.
                                const uint8_t* base = reinterpret_cast<const uint8_t*>(outW1);
                                const auto ob = [&](int v, int c) {
                                    int16_t x;
                                    std::memcpy(&x, base + v * 16 + c * 2, sizeof(x));
                                    return x;
                                };
                                const auto tc = [&](int v, int c) {
                                    int16_t x;
                                    std::memcpy(&x, base + v * 16 + 8 + c * 2, sizeof(x));
                                    return x;
                                };
                                gdx_port_logf("[rect] raw=%08X resolved=%p "
                                              "v0=(%d,%d,%d tc=%d,%d) v1=(%d,%d,%d tc=%d,%d) "
                                              "v2=(%d,%d,%d tc=%d,%d) v3=(%d,%d,%d tc=%d,%d)\n",
                                              in.w1, reinterpret_cast<void*>(outW1),
                                              ob(0,0), ob(0,1), ob(0,2), tc(0,0), tc(0,1),
                                              ob(1,0), ob(1,1), ob(1,2), tc(1,0), tc(1,1),
                                              ob(2,0), ob(2,1), ob(2,2), tc(2,0), tc(2,1),
                                              ob(3,0), ob(3,1), ob(3,2), tc(3,0), tc(3,1));
                            }
                        }
                    }
                    break;

                case kOpMtx:
                    if (!ResolveGuarded(in.w1, w1IsHostPointer, w1full, outW1, 64,
                                        /*preferPhysical=*/!isF3DSource,
                                        reinterpret_cast<uintptr_t>(item.source))) {
                        /* Raced reload: 0 is the sentinel, never a null pointer reaching the
                           interpreter -- the shared post-switch check further down always
                           intercepts it, substituting FallbackDataPointer's result under
                           GDX_LEGACY_RESOLVE or dropping the command entirely otherwise. */
                        outW1 = 0;
                    }
                    if ((outW1 & 7u) != 0) {
                        outW1 = 0;
                    }
                    {
                        // Unlike the vertex paths, this call already requires the full 64-byte Mtx
                        // readable, so a DROPPED resolution rather than a garbage read is the
                        // expected failure mode -- logged so dropped machine matrices can be
                        // correlated with observed z-fighting. Always on, same reasoning as
                        // [vtx-spike] above.
                        if (gGdxRaceActive != 0 && outW1 == 0 && in.w1 != 0) {
                            static int sMtxDroppedLogs = 0;
                            if (sMtxDroppedLogs < 40) {
                                ++sMtxDroppedLogs;
                                gdx_port_logf("[mtx-dropped] raw=%08X f3d=%d big=%d src=%p\n",
                                              in.w1, (int)isF3DSource, (int)isBig, item.source);
                            }
                        }
                    }
                    /* CRASH FAILSAFE. The w1IsHostPointer fast path takes w1full verbatim with no
                       readability check, and the resolver can return an under-validated candidate
                       (ResolveGeneratedAssetStub's bounds check is offset-only, so a
                       near-end-of-image interior match yields a valid-start/short-tail pointer).
                       A matrix load dereferences 64 bytes, so drop any pointer whose full 64 are
                       not readable; 0 is the sentinel the shared post-switch failsafe consumes.
                       MUST run BEFORE MakePersistentMtxCopy: that helper always returns a fresh,
                       fully-readable allocation, so checking after the copy validates the wrong
                       pointer and never protects the copy's own raw 64-byte read. */
                    if (outW1 != 0 && ReadableByteLimit(outW1) < 64u) {
                        static int sMtxFailsafeLogs = 0;
                        if (sMtxFailsafeLogs < 40) {
                            ++sMtxFailsafeLogs;
                            gdx_port_logf("[mtx-failsafe] raw=%08X resolved=%p readable=%zuB "
                                          "src=%p -> dropped\n",
                                          in.w1, reinterpret_cast<void*>(outW1),
                                          ReadableByteLimit(outW1), item.source);
                        }
                        outW1 = 0u;
                    }
                    if (outW1 != 0) {
                        /* Same byte-order proxy as the G_VTX paths: matrices referenced from
                           big-endian DLs are static asset Mtx data needing word-swapping for the
                           interpreter's host-order reads, while host-built DLs reference
                           Matrix_ToMtx output, already host-order. */
                        if (isBig) {
                            outW1 = MakePersistentMtxCopy(outW1);
                        }
                        outW1 = NormalizeLusDirectPointer(outW1);
                    }
                    // [mtx-content] probe, strip later: dumps the resolved 64-byte Mtx as hex
                    // words, armed on the same flag as [rect], to check the countdown billboard's
                    // modelview for degenerate values rather than just a resolved pointer.
                    {
                        const bool sDiagCountdownMtx = gdx_dev_gate(GDX_GATE_DIAG_COUNTDOWN) != 0;
                        if (sDiagCountdownMtx && gGdxCountdownProbeArm != 0 && outW1 != 0) {
                            static int sMtxContentLogs = 0;
                            if (sMtxContentLogs < 200) {
                                ++sMtxContentLogs;
                                const uint32_t* w = reinterpret_cast<const uint32_t*>(outW1);
                                gdx_port_logf("[mtx-content] raw=%08X resolved=%p "
                                              "%08X %08X %08X %08X %08X %08X %08X %08X "
                                              "%08X %08X %08X %08X %08X %08X %08X %08X\n",
                                              in.w1, reinterpret_cast<void*>(outW1),
                                              w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                                              w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
                            }
                        }
                    }
                    // Scratch-slot indirection for pool matrices: a fully-resolved 64-byte matrix
                    // inside the current GfxPool segment-1 span is copied into a stable scratch
                    // slot and the command rewritten to point there. Static/persistent asset
                    // matrices took the isBig -> MakePersistentMtxCopy path above and resolve
                    // OUTSIDE the pool span, so they pass through untouched -- HUD/2D ortho draws
                    // never load segment-1 pool matrices and are structurally excluded.
                    //
                    // G_MTX_PROJECTION (0x04) loads are included whenever mInterpCamera is set.
                    // race.c:250 loads gfxPool->unk_20208[] -- the combined projection*view -- with
                    // that flag and course.c emits no gSPMatrix at all, so excluding projection
                    // freezes BOTH the camera and the whole track at 60 Hz while racer matrices
                    // keep interpolating: objects smoothed against a static world, which is what
                    // tore the CPU-baked booster flames off the machines. See gdx_interp.h
                    // CameraInterpActive for why lerping a combined projection*view is not
                    // sufficient. F3DEX2's `p^G_MTX_PUSH` encoding never touches bit 0x04, so the
                    // raw low byte can be tested without un-XOR'ing.
                    //
                    // [mtx-census] GDX_DIAG_MTX=1, strip later: every distinct G_MTX operand that
                    // MISSES the reroute, with the top row of the matrix it points at. Answers
                    // whether the booster flames really draw under an identity matrix resolving
                    // outside the pool span, by measurement rather than by reading.
                    {
                        static const bool sDiagMtx = std::getenv("GDX_DIAG_MTX") != nullptr;
                        if (sDiagMtx && mInterpEnabled && outW1 != 0 && !GdxP0MtxInPoolSpan(outW1) &&
                            ReadableByteLimit(outW1) >= 64u) {
                            // Report each distinct operand once. A flat array rather than a set:
                            // the cap is 64 and this runs inside the display-list walk.
                            static uintptr_t sSeen[64];
                            static size_t sSeenCount = 0;
                            bool known = false;
                            for (size_t i = 0; i < sSeenCount; ++i) {
                                if (sSeen[i] == outW1) {
                                    known = true;
                                    break;
                                }
                            }
                            if (!known && sSeenCount < 64u) {
                                sSeen[sSeenCount++] = outW1;
                                const int32_t* w = reinterpret_cast<const int32_t*>(outW1);
                                gdx_port_logf("[mtx-census] non-pool G_MTX raw=%08X resolved=%p big=%d "
                                              "params=%02X row0=%08X %08X %08X %08X\n",
                                              in.w1, reinterpret_cast<void*>(outW1), isBig ? 1 : 0,
                                              (unsigned) (in.w0 & 0xFFu), w[0], w[1], w[2], w[3]);
                            }
                        }
                    }
                    if (mInterpEnabled && outW1 != 0 && !isBig &&
                        (((in.w0 & 0x04u) == 0u) || mInterpCamera) &&
                        GdxP0MtxInPoolSpan(outW1)) {
                        outW1 = GdxP0RerouteMtx(outW1, (in.w0 & 0x04u) != 0u);
                    }
                    break;

                case kOpMovemem:
                    if (!ResolveGuarded(in.w1, w1IsHostPointer, w1full, outW1)) {
                        continue;  // raced reload: skip the command this frame
                    }
                    if (outW1 != 0) {
                        outW1 = NormalizeLusDirectPointer(outW1);
                    }
                    // Carousel viewport lerp. Index byte 8 == G_MV_VIEWPORT; GdxVpReroute rejects
                    // any pointer outside D_i5_80118FF0, so every other viewport passes untouched.
                    if (mInterpEnabled && outW1 != 0 && (in.w0 & 0xFFu) == 8u) {
                        outW1 = GdxVpReroute(outW1);
                    }
                    break;

                case kOpSetColorImage:
                case kOpSetDepthImage:
                    if (!ResolveGuarded(in.w1, w1IsHostPointer, w1full, outW1)) {
                        continue;  // raced reload: skip the command this frame
                    }
                    if (outW1 == 0) outW1 = MakeFramebufferToken(in.w1);
                    if (op == kOpSetColorImage && outW1 != 0 && mStats != nullptr) {
                        bool alreadySeen = false;
                        for (size_t si = 0; si < mStats->colorImageTargetCount; si++) {
                            if (mStats->colorImageTargets[si] == outW1) {
                                alreadySeen = true;
                                break;
                            }
                        }
                        if (!alreadySeen && mStats->colorImageTargetCount < mStats->colorImageTargets.size()) {
                            mStats->colorImageTargets[mStats->colorImageTargetCount++] = outW1;
                        }
                    }
                    break;

                case kOpSetTextureImage:
                    {
                        /* Snapshot the segment-reload epoch BEFORE touching any segment-backed
                           state; without it a mode transition reloading segments 4/7/9 mid-read
                           produced an AV inside strlen on Create Machine entry. */
                        const uint32_t settimgEpoch = GdxSegmentEpochSnapshot();
                        const uintptr_t translated =
                            w1IsHostPointer ? w1full : TranslateDataPointer(in.w1);
                        /* A raced reload can leave both the translated pointer and the bytes it
                           addresses torn. Host-pointer textures never read gSegments[] and are
                           exempt. Drop the texture for one frame -- the previous binding persists
                           and the skip is invisible during a transition -- rather than sampling or
                           stringifying half-written state. */
                        const bool segmentReloadRaced =
                            !w1IsHostPointer && !GdxSegmentEpochStable(settimgEpoch);
                        if (segmentReloadRaced) {
                            if (mStats != nullptr) mStats->skippedTextures++;
                            continue;
                        }
                        /* Gates every branch below that dereferences the resolved bytes as memory
                           or as a string -- the O2R/pack filepath emit and the diagnostics. A
                           failed resolution returns 0 and must hard-skip all of them; the normal
                           unresolved-fallback emit further down is unaffected. */
                        const bool resolutionUsable = (translated != 0);
                        // Emit the O2R filepath opcode ONLY for BSS-stub textures (asset-segment
                        // symbols with a 1:1 O2R resource). RDRAM-backed textures are contiguous
                        // multi-tile buffers the game loads with many G_LOADBLOCKs at increasing
                        // ULS offsets; the O2R resource covers only the first tile, so later bands
                        // read out of bounds. Those must take the raw-copy path.
                        //
                        // The venue building-texture window is excluded for a different reason and
                        // MUST STAY excluded: its content is runtime-mutable and venue-dependent
                        // (func_800747EC DMAs a per-venue slice of super_textures over it each
                        // course load), so no build-time archive snapshot can be right --
                        // course_track_gfx/D_8014A20 in the o2r is in fact ALL ZEROS, because on
                        // the ROM that region is scratch. Serving it by name draws flat white slab
                        // buildings on every course placing BUILDING_TALL (Death Race, Mute City 2,
                        // the Ending). The raw-copy path reads the decoded image, which
                        // gdx_load_venue_building_texture keeps venue-correct.
                        const char* o2rKey = (!w1IsHostPointer && resolutionUsable && !IsRdramHostPointer(translated) &&
                                              !IsVenueBuildingTextureRange(translated))
                                                 ? gdx_lookup_asset_segment_o2r_key(in.w1)
                                                 : nullptr;
                        const char* texCensusPath = "rawcopy"; /* [tex-census] delivery classification */
                        /* Texture-pack override. `translated` is the unified source pointer for
                           BOTH delivery paths, so one lookup here covers common assets (fonts,
                           portraits, title art) whichever branch would otherwise take them. A pack
                           hit rewrites the load to the OTR-filepath opcode, the same mechanism as
                           the o2rKey emit. Existence is cached per key per pack epoch; with the
                           CVar off no lookup runs at all. */
                        const char* packPath = nullptr;
                        bool packViaRdram = false;
                        /* Same multi-tile exclusion as the o2rKey emit above: an RDRAM-backed
                           buffer is a contiguous atlas sampled at many ULS offsets and a
                           single-OTEX override covers only the first band, which garbles every
                           later one (title-screen text corruption with a font pack active). Atlas
                           replacement is per-tile keyed ("atlas/<baseKey>/o<byteOffset>/<FMT>/<W>x<H>")
                           and happens interpreter-side at the LoadBlock/LoadTile loaded.addr
                           assignment sites (devdocs/MODDING_ATLAS_DESIGN.md, mechanism corrected by
                           devdocs/POST_1_0_SCOPING.md) -- the bridge deliberately does NO opcode
                           rewrite for atlases; these buffers keep the raw-copy path so TMEM stays
                           populated for bands no pack overrides.

                           Exception (issue #27): RDRAM-backed WHOLE-image buffers (portraits,
                           title art) are registered by the common-asset fast path with their full
                           size, and were unreachable here -- packs could never override them. The
                           override is served only when the pack payload covers the whole
                           registered buffer; a band-sized payload is single-glyph atlas business
                           and keeps the raw-copy path. */
                        if (!o2rKey && resolutionUsable && gdx_workshop_texture_packs_enabled()) {
                            if (!IsRdramHostPointer(translated)) {
                                const char* assetKey = GDiffuser_LookupLoadedAssetKey(
                                    reinterpret_cast<const void*>(translated), 0, 0);
                                if (assetKey != nullptr) {
                                    packPath = GdxWorkshopLookupOverridePath(assetKey);
                                }
                            } else if (!IsVenueBuildingTextureRange(translated)) {
                                size_t bufSize = 0;
                                const char* assetKey = GDiffuser_LookupLoadedAssetKeyAndSize(
                                    reinterpret_cast<const void*>(translated), &bufSize);
                                if (assetKey != nullptr && bufSize != 0) {
                                    packPath = GdxWorkshopLookupOverridePathMinSize(assetKey, bufSize);
                                    packViaRdram = (packPath != nullptr);
                                }
                                /* [pack] diagnostic, strip later: issue #27 decision trace. A whole-image
                                   RDRAM buffer with a registered key is the portrait/title-art path; we
                                   need to see hit/miss/rejection at runtime. */
                                if (assetKey != nullptr) {
                                    static int sPackRdramSettimgLogs = 0;
                                    if (sPackRdramSettimgLogs < 8) {
                                        sPackRdramSettimgLogs++;
                                        gdx_port_logf("[pack] SETTIMG rdram src=%p key=%s bufSize=%zu "
                                                      "override=%s\n",
                                                      reinterpret_cast<void*>(translated), assetKey,
                                                      bufSize, packPath ? packPath : "(none)");
                                    }
                                }
                            }
                        }
                        if (o2rKey) {
                            texCensusPath = "o2r";
                            outW0 = (outW0 & 0x00FFFFFFu) | (static_cast<uintptr_t>(kOpSetTextureImageOtrFilepath) << 24);
                            outW1 = reinterpret_cast<uintptr_t>(o2rKey);
                        } else if (packPath) {
                            texCensusPath = packViaRdram ? "pack-o2r-rdram" : "pack-o2r";
                            outW0 = (outW0 & 0x00FFFFFFu) |
                                    (static_cast<uintptr_t>(kOpSetTextureImageOtrFilepath) << 24);
                            outW1 = reinterpret_cast<uintptr_t>(packPath);
                        } else if (w1IsHostPointer) {
                            // Real host pointer to texel data — use directly,
                            // UNLESS it is a generated asset stub (see
                            // ResolveWideAssetStubPointer): stubs must be re-routed to
                            // the decoded asset image or the sampler reads EXE data.
                            // The upcoming load-size estimate is computed here (rather
                            // than left at the resolver's default requiredBytes=1) so
                            // E1's bounds check in ResolveGeneratedAssetStub validates
                            // against the actual copy size instead of silently
                            // accepting a 1-byte-wide match; reused below for the
                            // native-RGBA16 copy so it is computed only once.
                            const size_t estimatedBytes =
                                EstimateRawTextureCopyBytes(item.source, i, item.limit, stride, isBig);
                            const uintptr_t stubResolved = ResolveWideAssetStubPointer(
                                w1full, mModuleBegin, mModuleEnd, std::max<size_t>(estimatedBytes, 1));
                            const uintptr_t hostTextureSource =
                                (stubResolved != 0) ? stubResolved : w1full;
                            // [stub-miss] diagnostic, strip later: a module-range texture pointer
                            // both stub resolvers miss is almost certainly an unbound stub
                            // (LinkStubs symbol with no AssetBindings entry, or a venue bank past
                            // the alias table) -- the next visual-garbage candidate.
                            if (stubResolved == 0 && w1full >= mModuleBegin && w1full < mModuleEnd) {
                                // One line per UNIQUE address: legitimate module-resident arrays
                                // like sCourseMinimapPalette are sampled every frame and would
                                // drown the budget.
                                static uintptr_t sStubMissSeen[24] = {};
                                static int sStubMissCount = 0;
                                bool alreadySeen = false;
                                for (int s = 0; s < sStubMissCount; s++) {
                                    if (sStubMissSeen[s] == w1full) {
                                        alreadySeen = true;
                                        break;
                                    }
                                }
                                if (!alreadySeen && sStubMissCount < 24) {
                                    sStubMissSeen[sStubMissCount++] = w1full;
                                    /* The low32 alone is useless against the .map because the base
                                       is randomized per run; prefVA (imageBase + moduleOffset)
                                       resolves to the exact symbol deterministically. */
                                    const uintptr_t modOff = w1full - mModuleBegin;
                                    gdx_port_logf("[stub-miss] SETTIMG module ptr %p (low32=%08X) "
                                                  "modOff=%08llX prefVA=%011llX unresolved -- taken verbatim\n",
                                                  reinterpret_cast<void*>(w1full), Low32(w1full),
                                                  static_cast<unsigned long long>(modOff),
                                                  static_cast<unsigned long long>(kPreferredImageBaseVA + modOff));
                                }
                            }
                            /* [transition-cap] probe, strip later: whether the byteswap-applying
                               native-RGBA16 range covers the transition capture buffer. native=1
                               means the host-endian capture IS swapped for the big-endian RGBA5551
                               reader; native=0 means it is read raw, giving the red/blue swap and
                               bit-shift that is the title->menu noise band. */
                            if (gDiagTransitionCaptureSize != 0 &&
                                w1full >= gDiagTransitionCaptureBegin &&
                                w1full < gDiagTransitionCaptureBegin + gDiagTransitionCaptureSize) {
                                static uintptr_t sTransCapSeen[8] = {};
                                static int sTransCapSeenCount = 0;
                                bool capSeen = false;
                                for (int s = 0; s < sTransCapSeenCount; s++) {
                                    if (sTransCapSeen[s] == w1full) {
                                        capSeen = true;
                                        break;
                                    }
                                }
                                if (!capSeen && sTransCapSeenCount < 8) {
                                    sTransCapSeen[sTransCapSeenCount++] = w1full;
                                    const bool nativeApplied = IsNativeRgba16Range(w1full, 2);
                                    gdx_port_logf("[transition-cap] SETTIMG src=%p native=%d "
                                                  "(1=byteswapped/correct, 0=raw/red-blue-swap band)\n",
                                                  reinterpret_cast<void*>(w1full), nativeApplied ? 1 : 0);
                                }
                            }
                            /* CPU framebuffer readback and transition capture buffers hold
                               HOST-order RGBA5551 words. Most host pointers refer to generated
                               texture bytes already in N64 byte order and must stay direct, but
                               ranges registered through gdx_set_native_rgba16_texture_range need
                               their two bytes swapped before Fast3D's N64 texture reader consumes
                               them. Wide Gfx packets have to apply the same policy the narrow path
                               gets from MakePersistentRawTextureCopy, or phased strips sample
                               little-endian words as big-endian and render as multicolour noise
                               bands. Copy only the exact load extent; every non-native host
                               texture keeps the direct path. */
                            if (IsNativeRgba16Range(hostTextureSource, 2)) {
                                size_t readable = RegisteredHostRemaining(hostTextureSource);
                                if (readable == 0) {
                                    readable = ReadableByteLimit(hostTextureSource);
                                }
                                /* Clamp the copy to the registered native-RGBA16 extent. The
                                   load-size estimate can round up past the registered image (the
                                   WIPE transition's single wide LOADBLOCK overshoots WIDTH*HEIGHT*2
                                   by a row), and CopyRawTextureBytes' IsNativeRgba16Range check is
                                   all-or-nothing: a copy spilling past the range is memcpy'd
                                   WITHOUT the swap, so the un-revealed wipe region renders as
                                   rainbow noise. The estimate's tail bytes lie beyond the last
                                   real load anyway. */
                                const size_t nativeRemaining = NativeRgba16RangeRemaining(hostTextureSource);
                                if (nativeRemaining != 0 && nativeRemaining < readable) {
                                    readable = nativeRemaining;
                                }
                                size_t required = estimatedBytes;
                                if (required == 0) {
                                    required = std::min(readable, kMaxRawTextureCopyBytes);
                                }
                                required = std::min(required, readable);

                                bool textureCopyRefreshed = false;
                                outW1 = MakePersistentRawTextureCopy(hostTextureSource, required,
                                                                     &textureCopyRefreshed);
                                if (mStats != nullptr && textureCopyRefreshed) {
                                    mStats->textureCopyBytes += required;
                                }
                                texCensusPath = "host-native-rgba16";
                            } else {
                                texCensusPath = (stubResolved != 0) ? "widestub" : "hostptr";
                                outW1 = hostTextureSource;
                            }
                            if (outW1 != 0) {
                                outW1 = NormalizeLusDirectPointer(outW1);
                            }
                        } else {
                            outW1 = TranslateTexturePointer(in.w1, item.source, i, item.limit, isBig, stride);
                            if (outW1 != 0) {
                                outW1 = NormalizeLusDirectPointer(outW1);
                            }
                        }
                        /* Close the seqlock bracket: the early check passed, but the resolution,
                           the O2R/pack lookups and MakePersistentRawTextureCopy have all READ
                           segment-backed state since. Re-check the SAME snapshot here -- after
                           every content copy, before the bytes reach the census or the
                           interpreter -- and drop the texture for this frame if a reload began
                           anywhere in that window. */
                        if (!w1IsHostPointer && !GdxSegmentEpochStable(settimgEpoch)) {
                            if (mStats != nullptr) mStats->skippedTextures++;
                            NoteEpochSkip();
                            continue;
                        }
                        /* [tex-census] diagnostic, strip later: one line per unique SETTIMG source
                           for the whole session, pairing each texture with its delivery path and
                           the first bytes LUS consumes. Fingerprint taken at +0x20 to skip
                           transparent glyph corners when the buffer is large enough. */
                        {
                            /* CI-format sources get a RESERVED pool: a single shared budget burns
                               out on RGBA/I textures during boot and the CI ones never log. */
                            static uint32_t sTexCensusSeen[512] = {};
                            static int sTexCensusCount = 0;
                            static uint32_t sTexCensusCiSeen[64] = {};
                            static int sTexCensusCiCount = 0;
                            const uint32_t cw0 = static_cast<uint32_t>(in.w0);
                            const uint32_t cFmt = (cw0 >> 21) & 0x7;
                            bool censusDup = false;
                            for (int tc = 0; tc < sTexCensusCount; tc++) {
                                if (sTexCensusSeen[tc] == in.w1) {
                                    censusDup = true;
                                    break;
                                }
                            }
                            for (int tc = 0; !censusDup && tc < sTexCensusCiCount; tc++) {
                                if (sTexCensusCiSeen[tc] == in.w1) {
                                    censusDup = true;
                                    break;
                                }
                            }
                            const bool censusHasBudget =
                                (sTexCensusCount < 512) ||
                                (cFmt == 2u && sTexCensusCiCount < 64);
                            // [tex-census]/[ci-dump] high-frequency per-draw census:
                            // silent unless GDX_DIAG_VERBOSE=1.
                            if (gdx_diag_verbose() && !censusDup && censusHasBudget) {
                                if (sTexCensusCount < 512) {
                                    sTexCensusSeen[sTexCensusCount++] = in.w1;
                                } else if (cFmt == 2u && sTexCensusCiCount < 64) {
                                    sTexCensusCiSeen[sTexCensusCiCount++] = in.w1;
                                }
                                const uint32_t cSiz = (cw0 >> 19) & 0x3;
                                const uint32_t cWidth = (cw0 & 0xFFF) + 1;
                                if (o2rKey) {
                                    gdx_port_logf("[tex-census] raw=%08X path=o2r fmt=%u siz=%u w=%u key=%s\n",
                                                  in.w1, cFmt, cSiz, cWidth, o2rKey);
                                } else {
                                    uint8_t cfp[8] = {};
                                    uintptr_t fpAt = outW1;
                                    if (fpAt != 0 && ReadableByteLimit(fpAt) >= 0x28) {
                                        fpAt += 0x20;
                                    }
                                    if (fpAt != 0 && ReadableByteLimit(fpAt) >= sizeof(cfp)) {
                                        std::memcpy(cfp, reinterpret_cast<const void*>(fpAt), sizeof(cfp));
                                    }
                                    gdx_port_logf("[tex-census] raw=%08X path=%s fmt=%u siz=%u w=%u out=%p "
                                                  "fp8=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                                  in.w1, texCensusPath, cFmt, cSiz, cWidth,
                                                  reinterpret_cast<void*>(outW1),
                                                  cfp[0], cfp[1], cfp[2], cfp[3], cfp[4], cfp[5], cfp[6], cfp[7]);
                                    /* [ci-dump], strip later: for CI sources the discriminator
                                       between "delivered wrong" and "interpreted wrong" is the raw
                                       tile bytes LUS will consume. 32 bytes from offset 0, not the
                                       +0x20 fingerprint -- CI text tiles are small enough that the
                                       skip would overshoot. */
                                    if (cFmt == 2u && outW1 != 0 && ReadableByteLimit(outW1) >= 32) {
                                        const uint8_t* cd = reinterpret_cast<const uint8_t*>(outW1);
                                        gdx_port_logf("[ci-dump] raw=%08X b0=%02X%02X%02X%02X%02X%02X%02X%02X"
                                                      "%02X%02X%02X%02X%02X%02X%02X%02X "
                                                      "b16=%02X%02X%02X%02X%02X%02X%02X%02X"
                                                      "%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                                      in.w1,
                                                      cd[0], cd[1], cd[2], cd[3], cd[4], cd[5], cd[6], cd[7],
                                                      cd[8], cd[9], cd[10], cd[11], cd[12], cd[13], cd[14], cd[15],
                                                      cd[16], cd[17], cd[18], cd[19], cd[20], cd[21], cd[22], cd[23],
                                                      cd[24], cd[25], cd[26], cd[27], cd[28], cd[29], cd[30], cd[31]);
                                    }
                                }
                            }
                        }
                        /* [GDX-DBG cm] probe, strip later: mode-gated census for Create Machine
                           (GET_MODE == 0x10), needed because the session-wide [tex-census] budget
                           burns out during boot. Logs the raw source, resolved pointer, format and
                           first 16 bytes, to compare against the known-good disk decode
                           (0x00C8A270 bg strip, 0x00C8CE60 OK button). */
                        if ((gGameMode & 0x1F) == 0x10) {
                            static uint32_t sCmSeen[512] = {};
                            static int sCmCount = 0;
                            bool cmDup = false;
                            for (int s = 0; s < sCmCount; s++) {
                                if (sCmSeen[s] == in.w1) { cmDup = true; break; }
                            }
                            if (!cmDup && sCmCount < 512) {
                                sCmSeen[sCmCount++] = in.w1;
                                const uint32_t cw0 = static_cast<uint32_t>(in.w0);
                                const uint32_t cFmt = (cw0 >> 21) & 0x7;
                                const uint32_t cSiz = (cw0 >> 19) & 0x3;
                                const uint32_t cWidth = (cw0 & 0xFFF) + 1;
                                uint8_t cb[16] = {};
                                if (outW1 != 0 && ReadableByteLimit(outW1) >= sizeof(cb)) {
                                    std::memcpy(cb, reinterpret_cast<const void*>(outW1), sizeof(cb));
                                }
                                gdx_port_logf("[GDX-DBG cm] raw=%08X path=%s fmt=%u siz=%u w=%u out=%p "
                                              "b0..15=%02X%02X%02X%02X%02X%02X%02X%02X"
                                              "%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                              in.w1, texCensusPath, cFmt, cSiz, cWidth,
                                              reinterpret_cast<void*>(outW1),
                                              cb[0], cb[1], cb[2], cb[3], cb[4], cb[5], cb[6], cb[7],
                                              cb[8], cb[9], cb[10], cb[11], cb[12], cb[13], cb[14], cb[15]);
                            }
                        }
                        /* [cm-seg3] probe, strip later: segment-3 SETTIMG census for the untextured
                           Create Machine preview. Separates a host-pointer path bypassing the
                           placeholder redirect (isPlaceholder=0, zero bytes) from a healthy
                           resolve. */
                        if ((gGameMode & 0x1F) == 0x10) {
                            static uint32_t sCmTexSeen[512] = {};
                            static int sCmTexCount = 0;
                            bool cmDup = false;
                            for (int s = 0; s < sCmTexCount; s++) {
                                if (sCmTexSeen[s] == in.w1) { cmDup = true; break; }
                            }
                            if (!cmDup && sCmTexCount < 512) {
                                sCmTexSeen[sCmTexCount++] = in.w1;
                                uint8_t cmb[8] = {};
                                if (outW1 != 0 && ReadableByteLimit(outW1) >= sizeof(cmb)) {
                                    std::memcpy(cmb, reinterpret_cast<const void*>(outW1), sizeof(cmb));
                                }
                                gdx_port_logf("[cm-seg3] raw=%08X path=%s out=%p isPlaceholder=%d "
                                              "b0..7=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                              in.w1, texCensusPath, reinterpret_cast<void*>(outW1),
                                              IsAssetPlaceholderPointer(in.w1) ? 1 : 0,
                                              cmb[0], cmb[1], cmb[2], cmb[3], cmb[4], cmb[5], cmb[6], cmb[7]);
                            }
                        }
                        // [settimg-trace] probe, strip later. Classifies the RESOLVED SOURCE, not
                        // the persistent-copy output: the copy is always an unregistered heap
                        // buffer, so classifying outW1 reports only which delivery path ran, never
                        // whether the data is correct. Flags MIO0 streams reaching the sampler.
                        if (gdx_dev_gate(GDX_GATE_DIAG_SETTIMG) && gGdxRaceActive != 0) {
                            static int sSettimgCount = 0;
                            // Per-course budget: gGdxRaceActive is a latch that never clears, so a
                            // process-lifetime cap could only ever describe a GP's first course.
                            static int sSettimgMode = -1;
                            const int settimgMode = gGameMode & 0x1F;
                            if (settimgMode != sSettimgMode) {
                                sSettimgMode = settimgMode;
                                sSettimgCount = 0;
                            }
                            if (sSettimgCount < 6000) {
                                ++sSettimgCount;
                                const auto classify = [this](uintptr_t p) -> const char* {
                                    if (p == 0) {
                                        return "NULL";
                                    }
                                    if (gdx_rdram != nullptr && p >= reinterpret_cast<uintptr_t>(gdx_rdram) &&
                                        p < reinterpret_cast<uintptr_t>(gdx_rdram) + GDX_RDRAM_SIZE) {
                                        return "rdram";
                                    }
                                    for (const LoadedAssetSegment& segImg : gLoadedAssetSegments) {
                                        const uintptr_t base = reinterpret_cast<uintptr_t>(segImg.bytes.data());
                                        if (base != 0 && p >= base && p < base + segImg.bytes.size()) {
                                            return "assetseg";
                                        }
                                    }
                                    if (p >= mModuleBegin && p < mModuleEnd) {
                                        return "module";
                                    }
                                    for (const HostRange& range : gHostRanges) {
                                        if (range.begin != 0 && p >= range.begin &&
                                            p < range.begin + range.size) {
                                            return "hostrange";
                                        }
                                    }
                                    return "OTHER";
                                };
                                const uint32_t fmt = (in.w0 >> 21) & 0x7;
                                const uint32_t siz = (in.w0 >> 19) & 0x3;
                                const uint32_t width = (in.w0 & 0xFFF) + 1;
                                uint8_t head[8] = {};
                                uint32_t sum = 0;
                                int mio0 = 0;
                                const uintptr_t fpSrc = (outW1 != 0) ? outW1 : translated;
                                const size_t fpAvail = (fpSrc != 0) ? ReadableByteLimit(fpSrc) : 0;
                                if (fpAvail >= sizeof(head)) {
                                    std::memcpy(head, reinterpret_cast<const void*>(fpSrc), sizeof(head));
                                    const size_t span = std::min<size_t>(fpAvail, 256);
                                    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(fpSrc);
                                    for (size_t b = 0; b < span; b++) {
                                        sum = sum * 31u + bytes[b];
                                    }
                                    mio0 = (std::memcmp(head, "MIO0", 4) == 0) ? 1 : 0;
                                }
                                // One file per process, opened once and held: the old fixed name
                                // truncated the previous session's trace on every launch.
                                static FILE* sSettimgFile = nullptr;
                                if (sSettimgFile == nullptr) {
#ifdef _WIN32
                                    const unsigned long tracePid =
                                        static_cast<unsigned long>(GetCurrentProcessId());
#else
                                    const unsigned long tracePid = static_cast<unsigned long>(getpid());
#endif
                                    char tracePath[64];
                                    std::snprintf(tracePath, sizeof(tracePath), "settimg-trace-%lu.txt",
                                                  tracePid);
                                    sSettimgFile = fopen(tracePath, "w");
                                }
                                if (sSettimgFile != nullptr) {
                                    fprintf(sSettimgFile,
                                            "T raw=%08X src=%p scls=%s out=%p fmt=%u siz=%u w=%u "
                                            "fp=%02X%02X%02X%02X%02X%02X%02X%02X sum=%08X mio0=%d dl=%p\n",
                                            in.w1, reinterpret_cast<void*>(translated), classify(translated),
                                            reinterpret_cast<void*>(outW1), fmt, siz, width,
                                            head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7],
                                            sum, mio0, item.source);
                                    // Flushed per line so a crash keeps the trace, as the old
                                    // open/close-per-line pattern did.
                                    fflush(sSettimgFile);
                                }
                            }
                        }
                    }
                    // [nodeinfo] probe, strip later. GDX_DIAG_NODEINFO, deliberately NOT race-gated
                    // so it is live in the editor. Fires on two raw-source windows: the seg-9
                    // node-info textures, and the seg-7 I4 glyph sheets (0x07009080 / 0x07009C80 /
                    // 0x0700A880, 0xC00 each) that func_xk1_8002924C blits the info panels from.
                    //
                    // The live question is RESOLUTION, not the assets. seg-7 is also populated from
                    // cartridge ROM (expansion_kit_textures_beta), and only TryResolveAddress
                    // scanning gN64AddressRanges ahead of the segment table keeps the disk copy
                    // winning; a requiredBytes over-estimate drops the range match and falls through
                    // to that cart data, which is structureless noise. The budget is exact -- 3072
                    // of 3072 -- with no slack to absorb one.
                    //
                    // Reading it: no lines means the draw never runs (an editor-state gate); out=NULL
                    // or a short avail means the resolve is failing and the cart fallback is what
                    // reaches the screen.
                    //
                    // Already ruled out: 0x7031F0 is an ovl_i3 verbatim HUD array, not an editor
                    // asset; all four seg-9 symbols are present and correctly sized in
                    // EkAssetBindings.c and are RGBA16/I4, not CI8, so the two-half CI8 cache cannot
                    // apply; and the glyph sheets hold legible data on the translated disk
                    // (1625/1401/1566 nonzero bytes, counted over the FULL buffer -- all three open
                    // with a zero run for the blank-space glyph, so a fixed-prefix checksum lies).
                    {
                        const bool inNodeInfoWindow = in.w1 >= 0x09000C48u && in.w1 < 0x09003808u;
                        const bool inSetupFontWindow = in.w1 >= 0x07009080u && in.w1 < 0x0700B480u;

                        if (gdx_dev_gate(GDX_GATE_DIAG_NODEINFO) && !w1IsHostPointer &&
                            (inNodeInfoWindow || inSetupFontWindow)) {
                            static int sNodeInfoLogs = 0;
                            if (sNodeInfoLogs < 400) {
                                ++sNodeInfoLogs;
                                // outW1 is the final resolved host pointer LUS samples this
                                // command; out=NULL / avail<8 means the seg-9 source did not
                                // resolve to live pixels.
                                const size_t avail = (outW1 != 0) ? ReadableByteLimit(outW1) : 0;
                                uint8_t nib[8] = {};
                                if (avail >= sizeof(nib)) {
                                    std::memcpy(nib, reinterpret_cast<const void*>(outW1), sizeof(nib));
                                }
                                // Label by the window that matched: the seg-9 ladder alone reports
                                // every seg-7 hit as NumberSheet, since 0x0700xxxx < 0x09001788.
                                const char* asset =
                                    inSetupFontWindow
                                        ? ((in.w1 < 0x07009C80u)   ? "SetupFontSheet0"
                                           : (in.w1 < 0x0700A880u) ? "SetupFontSheet1"
                                                                   : "SetupFontSheet2")
                                    : (in.w1 < 0x09001788u)   ? "NumberSheet"
                                    : (in.w1 < 0x09002F88u)   ? "NumberTex"
                                    : (in.w1 < 0x09003408u)   ? "InfoBackground"
                                                              : "InfoFontSheet";
                                gdx_port_logf("[nodeinfo] %s raw=%08X out=%p avail=%zu "
                                              "b0..7=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                                              asset, in.w1, reinterpret_cast<void*>(outW1), avail,
                                              nib[0], nib[1], nib[2], nib[3], nib[4], nib[5], nib[6], nib[7]);
                            }
                        }
                    }
                    break;

                case kOpDl:
                    outW1 = ResolveDisplayListGuarded(in.w1, item.source, i, w1IsHostPointer, w1full);
                    break;

                case kOpDlOtrFilepath:
                    /* Module-built trampolines (gdx_model_packs) carry the pack key as a real
                       host string pointer; forward it verbatim for the interpreter's filepath
                       handler. The OTR-filepath readability guard below validates outW1. */
                    if (w1IsHostPointer) {
                        outW1 = w1full;
                    }
                    break;

                case kOpMoveword:
                    if (WordParam(in.w0) == kMovewordSegmentIndex) {
                        const uint8_t segIdx = static_cast<uint8_t>((in.w0 & 0xFFFF) / 4);
                        /* gSPSegment(seg, base) packs the segment BASE through the widened gDma1p
                           macro, so a game-emitted wide list carries the FULL host pointer in
                           w1full. The segment table is the central base for ALL segmented
                           addressing, so reading the truncated low32 and running it through the
                           legacy resolver truncates every segment base and cascades into zero-byte
                           texture copies, garbage vertex/matrix bases and noop'd DL roots. Take the
                           real host pointer verbatim, like the other w1IsHostPointer opcodes.

                           This is a segment-table WRITE, so the epoch guard matters more than on
                           the read paths: if a reload raced it, do NOT publish a base derived from
                           a torn gSegments[] -- skip the write, keep the stale base for one frame
                           (the DL re-emits the segment set next frame) and emit that. */
                        const uint32_t mwEpoch =
                            w1IsHostPointer ? 0u : GdxSegmentEpochSnapshot();
                        uintptr_t translated;
                        if (w1IsHostPointer) {
                            translated = w1full;
                        } else {
                            translated = TranslateDataPointer(in.w1);
                            if (translated == 0 && in.w1 == 0 && gdx_rdram != nullptr) {
                                translated = reinterpret_cast<uintptr_t>(gdx_rdram);
                            }
                        }
                        if (!w1IsHostPointer && !GdxSegmentEpochStable(mwEpoch)) {
                            NoteEpochSkip();
                            outW1 = (segIdx < kGfxSegmentCount)
                                        ? gSegments[segIdx]
                                        : static_cast<uintptr_t>(in.w1);
                            break;
                        }
                        if (translated != 0 && segIdx < kGfxSegmentCount) {
                            const uintptr_t normalized = NormalizeLusDirectPointer(translated);
                            /* Only the CPU-side setters print [seg] lines, so in-DL repoints were
                               invisible and "draws through a stale segment" undiagnosable. */
                            if (gSegments[segIdx] != normalized) {
                                static int sSegRepointLogs[kGfxSegmentCount] = {};
                                // [seg-dl] successful repoint is informational per-draw:
                                // silent unless GDX_DIAG_VERBOSE=1 (the FAILED variant below
                                // is an error family and stays always-on).
                                if (gdx_diag_verbose() && sSegRepointLogs[segIdx] < 6) {
                                    ++sSegRepointLogs[segIdx];
                                    gdx_port_logf("[seg-dl] moveword seg=%X raw=%08X %p -> %p\n", segIdx, in.w1,
                                                  reinterpret_cast<void*>(gSegments[segIdx]),
                                                  reinterpret_cast<void*>(normalized));
                                }
                                /* [seg-dl-race] probe, strip later. Course-select's carousel alone
                                   burns the 6-slot budget above for segment 0x0A before a race is
                                   entered, so an 0x0A repoint during the first race frames is
                                   invisible -- and 0x0A is what ResolveVenueBankAlias resolves
                                   course.c's road/wall stubs through. If it is transiently wrong
                                   there, MakePersistentRawTextureCopy mints a cache entry from the
                                   WRONG source and a later correct entry takes over under a
                                   different key, which looks like self-healing but is not staleness
                                   detection. Keyed independently so it survives course-select. */
                                if (segIdx == 0x0A && gGdxRaceActive != 0) {
                                    static int sSegARaceRepointLogs = 0;
                                    // [seg-dl-race] informational probe: silent unless GDX_DIAG_VERBOSE=1.
                                    if (gdx_diag_verbose() && sSegARaceRepointLogs < 32) {
                                        ++sSegARaceRepointLogs;
                                        gdx_port_logf(
                                            "[seg-dl-race] seg=A raw=%08X %p -> %p (race-scoped, unbudgeted by "
                                            "course-select)\n",
                                            in.w1, reinterpret_cast<void*>(gSegments[segIdx]),
                                            reinterpret_cast<void*>(normalized));
                                    }
                                }
                            }
                            gSegments[segIdx] = normalized;
                        } else if (in.w1 != 0 && segIdx < kGfxSegmentCount) {
                            /* A segment repoint we could NOT translate: the segment keeps
                               its stale base and everything drawn through it afterwards is
                               missing or garbage. This must never be silent. */
                            static int sSegFailLogs = 0;
                            if (sSegFailLogs < 24) {
                                ++sSegFailLogs;
                                gdx_port_logf("[seg-dl] moveword FAILED seg=%X raw=%08X (stale base %p kept)\n",
                                              segIdx, in.w1, reinterpret_cast<void*>(gSegments[segIdx]));
                            }
                        }
                        outW1 = (segIdx < kGfxSegmentCount) ? gSegments[segIdx] : static_cast<uintptr_t>(in.w1);
                    }
                    break;

                case kOpBranchZF3D:
                    if (isF3DSource) {
                        // Legacy F3D uses 0xD6 for G_BRANCH_Z. Its target/condition
                        // encoding is not compatible with F3DEX2 G_DMA_IO.
                        continue;
                    }
                    // F3DEX2 uses 0xD6 for G_DMA_IO. F3DFLX loads its per-vertex
                    // reflection-alpha lookup table through this command.
                    if (!ResolveGuarded(in.w1, w1IsHostPointer, w1full, outW1)) {
                        continue;  // raced reload: skip the command this frame
                    }
                    if (outW1 == 0) {
                        if (mStats != nullptr) {
                            mStats->skippedDataCommands++;
                        }
                        continue;
                    }
                    outW1 = NormalizeLusDirectPointer(outW1);
                    break;

                case kOpRdpHalf1:
                    /* G_RDPHALF_1 is also used as raw RDP payload for commands like
                       G_TEXRECT.  Do not blindly treat its w1 as a G_BRANCH_Z display-list
                       pointer just because the following command byte is 0x04; in older F3D
                       GBIs 0x04 is G_VTX, and texture-rectangle payloads can otherwise be
                       corrupted into no-op display-list pointers. */
                    if ((i + 1 < item.limit) &&
                        (Opcode(ReadCommand(item.source, i + 1, stride, isBig).w0) == kOpBranchZ) &&
                        (w1IsHostPointer || IsResolvableDisplayList(in.w1))) {
                        outW1 = ResolveDisplayListGuarded(in.w1, item.source, i, w1IsHostPointer, w1full);
                    }
                    break;

                case 0xDD: {
                    /* Physical G_LOAD_UCODE packets encode a data size in w0 and a truncated host
                       symbol in w1. Convert known F3DEX2-family loads into a semantic variant
                       switch Libultraship can consume without reading the size as a UcodeHandlers
                       enum.

                       The game emits this pointer through different VA->PA transforms (identity,
                       & 0x1FFFFFFF, - 0x80000000) depending on call site, and the symbols' truncated
                       host addresses move every build, so recognition compares within the 512 MB
                       physical window -- an exact-low32 match broke per build and machine select
                       lost every model. A WIDE packet carrying a full host pointer is matched
                       exactly instead; the low29 window is only for narrow/truncated sources. */
                    const auto matchesUcodeText = [raw = in.w1, w1IsHostPointer,
                                                   w1full](const void* symbol) {
                        const uintptr_t symbolAddr = reinterpret_cast<uintptr_t>(symbol);
                        if (w1IsHostPointer) {
                            return w1full == symbolAddr;
                        }
                        const uint32_t symbolLow = Low32(symbolAddr);
                        return ((raw ^ symbolLow) & 0x1FFFFFFFu) == 0;
                    };

                    /* Any recognized microcode load ends a pending L3DEX2 line section;
                       the L3DEX2 arm below re-arms it. */
                    l3dexLineSection = false;

                    Fast::F3dex2Variant variant;
                    if (matchesUcodeText(gspF3DEX2_fifoTextStart)) {
                        variant = Fast::F3dex2Variant::Standard;
                    } else if (matchesUcodeText(gspF3DLX2_Rej_fifoTextStart)) {
                        variant = Fast::F3dex2Variant::Reject;
                    } else if (matchesUcodeText(gspF3DEX2_Rej_fifoTextStart)) {
                        /* EK menus load plain F3DEX2.Rej, distinct from F3DLX2.Rej but with the
                           same reject-box semantics. Without this arm the load is dropped and
                           reject screening never engages (ucode_unknown=1 every menu frame). */
                        variant = Fast::F3dex2Variant::Reject;
                    } else if (matchesUcodeText(gspF3DFLX2_Rej_fifoTextStart)) {
                        variant = Fast::F3dex2Variant::FZeroFlxReject;
                    } else if (matchesUcodeText(gspL3DEX2_fifoTextStart)) {
                        /* L3DEX2 line microcode (Course Edit track lines, menu track previews).
                           Fast3D has no line primitive, but the section's non-line commands are
                           F3DEX2-compatible, so run it as Standard and rewrite each G_LINE3D into
                           OTR_G_LINE3D_GDX (case 0x08 below), which the interpreter expands into a
                           screen-space quad. Skipping the section wholesale avoids garbage frames
                           but never renders the course spline or control-point connectors.
                           l3dexUcodeSkips counts translated sections, not skips. */
                        variant = Fast::F3dex2Variant::Standard;
                        l3dexLineSection = true;
                        if (mStats != nullptr) {
                            mStats->l3dexUcodeSkips++;
                            if (mStats->firstL3dexUcodeRaw == 0) {
                                mStats->firstL3dexUcodeRaw = in.w1;
                            }
                        }
                    } else {
                        /* Unrecognized load: drop only the load itself, never engage the section
                           skip -- a false positive there silently eats the rest of the frame. */
                        if (mStats != nullptr) {
                            mStats->unknownUcodeSwitches++;
                            if (mStats->firstUnknownUcodeRaw == 0) {
                                mStats->firstUnknownUcodeRaw = in.w1;
                            }
                        }
                        continue;
                    }
                    skipUnsupportedUcode = false;

                    if (mStats != nullptr) {
                        mStats->ucodeSwitches++;
                    }
                    outW0 = (static_cast<uintptr_t>(0xDDu) << 24) |
                            static_cast<uintptr_t>(ucode_f3dex2);
                    outW1 = Fast::F3DEX2_VARIANT_SWITCH_MARKER |
                            static_cast<uintptr_t>(variant);
                    break;
                }

                // L3DEX2 G_LINE3D, meaningful only inside a Course Edit line section.
                //
                // This build defines F3DEX_GBI_2=1, where G_LINE3D == 0x08 (gbi.h:121, inside the
                // "#ifdef F3DEX_GBI_2" block opened at line 90). The (G_IMMFIRST-10) == 0xB5
                // definition at line 148 belongs to the LEGACY branch after the "#else" at line 122
                // -- that #else's trailing block comment only NAMES the condition being closed,
                // Nintendo style, and does not open the GBI_2 block. Labelling this case 0xB5 lets
                // real 0x08 line commands reach the interpreter verbatim and crash ("Unhandled OP
                // code: 0x8, for loaded ucode: 4") as soon as 4+ control points make lines draw.
                //
                // gSPLineW3D packs into w0 alone: w0[31:24]=0x08, w0[23:16]=v0*2, w0[15:8]=v1*2,
                // w0[7:0]=wd, w1=0. The rewrite preserves that byte layout; the interpreter expands
                // it into a screen-space quad (indices arrive *2, handler divides by 2). Outside a
                // line section the opcode is dropped: 0x08 is G_RESERVED3 in plain F3DEX2 and never
                // legitimately emitted.
                case 0x08:
                    if (!l3dexLineSection) {
                        continue;
                    }
                    outW0 = (static_cast<uintptr_t>(0x41u) << 24) | (in.w0 & 0x00FFFFFFu);
                    outW1 = 0;
                    break;

                // F3D G_ENDDL (0xB8): stop list processing, emit F3DEX2 ENDDL so Fast3D sees a clean end.
                case 0xB8:
                    outW0 = static_cast<uintptr_t>(kOpEndDl) << 24;
                    outW1 = 0;
                    item.listPtr->commands.push_back(MakeLusGfx(outW0, outW1));
                    if (mStats != nullptr) mStats->commandsOut++;
                    return;

                // F3D G_DL (0x06): sub-display-list call — same layout as F3DEX2 G_DL (0xDE).
                // GUARD: F3DEX2 uses 0x06 for G_TRI2; only remap in F3D asset DLs.
                case 0x06:
                    if (isF3DSource) {
                        outW0 = (outW0 & 0x00FFFFFFu) | (static_cast<uintptr_t>(kOpDl) << 24);
                        outW1 = ResolveDisplayListGuarded(in.w1, item.source, i, /*hostPtr=*/false, 0);
                    }
                    break;

                // F3D G_TRI1 (0xBF): w1 stores three vertex indices multiplied by 10.
                // F3DEX2 G_TRI1 stores the indices multiplied by 2 in w0[23:0].
                // GUARD: F3DEX2 uses 0xBF for G_CULLDL; only convert in F3D asset DLs.
                case 0xBF:
                    if (isF3DSource) {
                        const uint8_t v0 = static_cast<uint8_t>((in.w1 >> 16) & 0xFFu) / 10u;
                        const uint8_t v1 = static_cast<uint8_t>((in.w1 >> 8) & 0xFFu) / 10u;
                        const uint8_t v2 = static_cast<uint8_t>(in.w1 & 0xFFu) / 10u;
                        outW0 = (static_cast<uintptr_t>(0x05u) << 24) |
                                (static_cast<uintptr_t>(v0 * 2u) << 16) |
                                (static_cast<uintptr_t>(v1 * 2u) << 8) |
                                static_cast<uintptr_t>(v2 * 2u);
                        outW1 = 0;
                    }
                    break;

                /* F3D G_MOVEMEM (0x03): in F3DEX2, 0x03 = G_CULLDL — cannot pass through.
                   Legacy gDma1p stores the target in w0[23:16] and byte count in w0[15:0].
                   Translate viewport, look-at, and light slots to the F3DEX2 G_MV_LIGHT
                   index/offset layout used by Libultraship. */
                case 0x03:
                    if (isF3DSource) {
                        const uint8_t legacyIndex = static_cast<uint8_t>((in.w0 >> 16) & 0xFFu);
                        const size_t xferSize = static_cast<size_t>(in.w0 & 0xFFFFu);
                        uint8_t index = 0;
                        uint8_t offset = 0;

                        if (legacyIndex == 0x80u) {
                            index = 8u; // G_MV_VIEWPORT
                        } else if (legacyIndex == 0x84u) {
                            index = 10u; // G_MV_LIGHT / G_MVO_LOOKATX
                            offset = 0u;
                        } else if (legacyIndex == 0x82u) {
                            index = 10u; // G_MV_LIGHT / G_MVO_LOOKATY
                            offset = 24u;
                        } else if (legacyIndex >= 0x86u && legacyIndex <= 0x94u &&
                                   ((legacyIndex - 0x86u) & 1u) == 0) {
                            index = 10u; // G_MV_LIGHT
                            offset = static_cast<uint8_t>(
                                48u + ((legacyIndex - 0x86u) / 2u) * 24u);
                        } else {
                            continue;
                        }

                        uintptr_t addr;
                        if (!ResolveGuarded(in.w1, /*hostPtr=*/false, 0, addr,
                                            std::max<size_t>(xferSize, 1u))) {
                            continue;  // raced reload: skip the command this frame
                        }
                        if (addr == 0) {
                            addr = FallbackDataPointer(kOpMovemem, in.w1);
                        }
                        if (addr == 0) {
                            if (mStats != nullptr) mStats->skippedDataCommands++;
                            continue;
                        }
                        addr = NormalizeLusDirectPointer(addr);

                        /* The F3DEX2 handler consumes index from low 8 bits and
                           offset in units of eight bytes from bits 15:8. */
                        outW0 = (static_cast<uintptr_t>(kOpMovemem) << 24) |
                                (static_cast<uintptr_t>(offset / 8u) << 8) |
                                static_cast<uintptr_t>(index);
                        outW1 = addr;
                    }
                    break;

                /* F3D G_POPMTX (0xBD) → F3DEX2 G_POPMTX (0xD8).
                   F3DEX2 encodes the pop count as n*sizeof(Mtx) in w1; F3D uses 0. */
                case 0xBD:
                    if (isF3DSource) {
                        outW0 = static_cast<uintptr_t>(0xD8u) << 24;
                        outW1 = 64u; /* sizeof(Mtx) — pop 1 matrix */
                    }
                    break;

                /* F3D G_MOVEWORD (0xBC) → F3DEX2 G_MOVEWORD (0xDB).
                   Same word layout (index in w0[23:16], offset in w0[15:0], data in w1).
                   Handle segment-table writes identically to case kOpMoveword above. */
                case 0xBC:
                    if (isF3DSource) {
                        outW0 = (outW0 & 0x00FFFFFFu) | (static_cast<uintptr_t>(kOpMoveword) << 24);
                        if (WordParam(in.w0) == kMovewordSegmentIndex) {
                            const uint8_t segIdx = static_cast<uint8_t>((in.w0 & 0xFFFF) / 4);
                            /* Same full-width segment base as kOpMoveword above: a converted
                               wide F3D list may commit a genuine >4GB host base to
                               w1full, so honor w1IsHostPointer before falling back to the
                               narrow-token resolver. */
                            /* Same segment-table WRITE race policy as kOpMoveword:
                               snapshot before resolving and skip the write on a raced
                               reload, retaining the stale base for this frame instead
                               of publishing a torn one. */
                            const uint32_t mwEpoch =
                                w1IsHostPointer ? 0u : GdxSegmentEpochSnapshot();
                            uintptr_t translated;
                            if (w1IsHostPointer) {
                                translated = w1full;
                            } else {
                                translated = TranslateDataPointer(in.w1);
                                if (translated == 0 && in.w1 == 0 && gdx_rdram != nullptr) {
                                    translated = reinterpret_cast<uintptr_t>(gdx_rdram);
                                }
                            }
                            if (!w1IsHostPointer && !GdxSegmentEpochStable(mwEpoch)) {
                                NoteEpochSkip();
                                outW1 = (segIdx < kGfxSegmentCount)
                                            ? gSegments[segIdx]
                                            : static_cast<uintptr_t>(in.w1);
                                break;
                            }
                            if (translated != 0 && segIdx < kGfxSegmentCount) {
                                gSegments[segIdx] = NormalizeLusDirectPointer(translated);
                            }
                            outW1 = (segIdx < kGfxSegmentCount) ? gSegments[segIdx]
                                                                 : static_cast<uintptr_t>(in.w1);
                        }
                    }
                    break;

                /* F3D G_TEXTURE (0xBB) → F3DEX2 G_TEXTURE (0xD7).  Same word layout. */
                case 0xBB:
                    if (isF3DSource) {
                        outW0 = (outW0 & 0x00FFFFFFu) | (static_cast<uintptr_t>(0xD7u) << 24);
                    }
                    break;

                /* F3D G_SETOTHERMODE_H (0xBA) → F3DEX2 G_SETOTHERMODE_H (0xE3).
                   F3D stores shift/length directly, while F3DEX2 stores
                   (32 - shift - length)/(length - 1). Re-encode both fields. */
                case 0xBA:
                    if (isF3DSource) {
                        const uint32_t shift = (in.w0 >> 8) & 0xFFu;
                        const uint32_t length = in.w0 & 0xFFu;
                        if ((length == 0u) || (shift + length > 32u)) {
                            continue;
                        }
                        outW0 = (static_cast<uintptr_t>(0xE3u) << 24) |
                                (static_cast<uintptr_t>(32u - shift - length) << 8) |
                                static_cast<uintptr_t>(length - 1u);
                    }
                    break;

                /* F3D G_SETOTHERMODE_L (0xB9) → F3DEX2 G_SETOTHERMODE_L (0xE2).
                   Re-encode the legacy direct shift/length fields for F3DEX2. */
                case 0xB9:
                    if (isF3DSource) {
                        const uint32_t shift = (in.w0 >> 8) & 0xFFu;
                        const uint32_t length = in.w0 & 0xFFu;
                        if ((length == 0u) || (shift + length > 32u)) {
                            continue;
                        }
                        outW0 = (static_cast<uintptr_t>(0xE2u) << 24) |
                                (static_cast<uintptr_t>(32u - shift - length) << 8) |
                                static_cast<uintptr_t>(length - 1u);
                    }
                    break;

                /* F3D G_SETGEOMETRYMODE (0xB7): OR flags into geometry mode.
                   F3DEX2 G_GEOMETRYMODE (0xD9): w0[23:0]=keep-mask, w1=set-bits.
                   Keep-mask 0xFFFFFF means keep all existing bits, then OR in w1. */
                case 0xB7:
                    if (isF3DSource) {
                        outW0 = (static_cast<uintptr_t>(0xD9u) << 24) | 0x00FFFFFFu;
                        outW1 = static_cast<uintptr_t>(in.w1);
                    }
                    break;

                /* F3D G_CLEARGEOMETRYMODE (0xB6): AND-clear flags from geometry mode.
                   F3DEX2 G_GEOMETRYMODE: keep-mask=~flags (clear exactly those bits), set=0. */
                case 0xB6:
                    if (isF3DSource) {
                        outW0 = (static_cast<uintptr_t>(0xD9u) << 24) | (~in.w1 & 0x00FFFFFFu);
                        outW1 = 0;
                    }
                    break;

                /* F3D G_CULLDL (0xBE): sub-DL conditional cull.  No F3DEX2 equivalent at 0xBE.
                   NOP — culling is an optimization, not a correctness requirement. */
                case 0xBE:
                    if (isF3DSource) {
                        continue;
                    }
                    break;

                // F3D G_VTX (0x04): only in ROM asset DLs; GfxPool F3DEX2 DLs use 0x04 for G_BRANCH_Z.
                case 0x04:
                    if (isF3DSource) {
                        // F3D: w0 = [04, par=(n-1)<<4|v0, len=sizeof(Vtx)*n]; w1 = vtx addr
                        // F3DEX2: w0 = [01, n<<12 | (v0+n)*2]; w1 = vtx addr
                        const uint16_t len = static_cast<uint16_t>(in.w0 & 0xFFFFu);
                        const uint8_t n = (len >= 16u) ? static_cast<uint8_t>(len / 16u) : 1u;
                        const uint8_t par = static_cast<uint8_t>((in.w0 >> 16) & 0xFFu);
                        const uint8_t v0 = par & 0x0Fu;
                        /* Vertex-spike root cause (#3): this path is the one the
                           original author already tagged as the source of "the
                           machine-part and decoration vertex-spike geometry" below,
                           but TranslateDataPointer was still only asked to prove 1
                           byte readable while MakePersistentVtxCopy unconditionally
                           reads n*16 bytes. Require the full payload up front so an
                           ambiguous/near-miss resolution can't slip a garbage vertex
                           into a machine model. */
                        const size_t vtxRequiredBytes = static_cast<size_t>(n) * 16u;
                        /* Same trySourceWindow() asymmetry fix as the F3DEX2 G_VTX case
                           above: pass sourceHint so a legacy-F3D vertex load can still be
                           reconstructed from its own referencing DL's window. isF3DSource
                           is always true on this path, so preferPhysical stays false
                           (unchanged) -- only sourceHint is new here. */
                        if (!ResolveGuarded(in.w1, /*hostPtr=*/false, 0, outW1, vtxRequiredBytes,
                                            /*preferPhysical=*/!isF3DSource,
                                            reinterpret_cast<uintptr_t>(item.source))) {
                            /* Raced reload: substitute the fallback vertices -- never
                               skip a vertex load silently (later triangles index the
                               buffer). Neutralizes the torn pointer before the copy. */
                            outW1 = reinterpret_cast<uintptr_t>(kFallbackVertices);
                        }
                        // Always on: see the [vtx-spike]/[vtx-dropped] note
                        // in the F3DEX2 G_VTX case above -- this legacy F3D path is the
                        // one identified as the source of machine-part
                        // and decoration vertex-spike geometry, so it especially cannot
                        // be left opt-in behind an env var the user never set.
                        if (gGdxRaceActive != 0) {
                            if (outW1 == 0) {
                                const uintptr_t loose = TranslateDataPointer(in.w1, 1);
                                static int sVtxF3DDroppedLogs = 0;
                                if (sVtxF3DDroppedLogs < 40) {
                                    ++sVtxF3DDroppedLogs;
                                    gdx_port_logf("[vtx-dropped-f3d] raw=%08X n=%u need=%zuB "
                                                  "looseResolve=%p looseReadable=%zuB src=%p\n",
                                                  in.w1, n, vtxRequiredBytes,
                                                  reinterpret_cast<void*>(loose),
                                                  loose ? ReadableByteLimit(loose) : 0u, item.source);
                                }
                            }
                        }
                        if (outW1 != 0) {
                            /* F3D vertex payloads share the DL's byte order, same
                               as the EX2 path above: big-endian sources need the
                               swapped persistent copy or the interpreter reads
                               byte-swapped s16 coordinates (the machine-part and
                               decoration vertex-spike geometry). */
                            if (isBig) {
                                outW1 = MakePersistentVtxCopy(outW1, n);
                            }
                            outW1 = NormalizeLusDirectPointer(outW1);
                        } else {
                            outW1 = FallbackDataPointer(kOpVtx, in.w1);
                            if (outW1 == 0) {
                                if (mStats != nullptr) mStats->skippedDataCommands++;
                                continue;
                            }
                        }
                        outW0 = (static_cast<uintptr_t>(kOpVtx) << 24) |
                                (static_cast<uintptr_t>(n) << 12) |
                                static_cast<uintptr_t>((v0 + n) * 2u);
                    }
                    break;

                default:
                    break;
            }

            if (outW1 == 0 && (op == kOpVtx || op == kOpMtx || op == kOpMovemem || op == kOpSetTextureImage)) {
                outW1 = FallbackDataPointer(op, in.w1);
                if (outW1 != 0) {
                    if (mStats != nullptr) {
                        if (mStats->fallbackDataCommands == 0) {
                            mStats->firstFallbackDataOp = op;
                            mStats->firstFallbackDataRaw = in.w1;
                            mStats->firstFallbackDataW0 = in.w0;
                            mStats->firstFallbackDataSource = reinterpret_cast<uintptr_t>(item.source);
                            mStats->firstFallbackDataIndex = i;
                        }
                        mStats->fallbackDataCommands++;
                    }
                } else {
                    if (mStats != nullptr) {
                        if (mStats->skippedDataCommands == 0) {
                            mStats->firstSkippedDataOp = op;
                            mStats->firstSkippedDataRaw = in.w1;
                            mStats->firstSkippedDataW0 = in.w0;
                        }
                        mStats->skippedDataCommands++;
                        mStats->skippedTextures++;
                    }
                    continue;
                }
            }

            /* OTR-filepath strlen backstop (Create Machine entry, thread_id=3 AV
               in strlen). The LUS OTR-filepath handlers (interpreter.cpp
               gfx_set_timg_otr_filepath_handler_custom et al.) treat w1 as a
               `const char*` and hand it to LoadResourceProcess -> strlen. LUS
               already rejects w1 < 0x10000 and > 0x0000FFFFFFFFFFFF, but a BARE
               32-bit token zero-extended into the mid range (e.g. 0x25820F60 --
               note the 0x25 top byte is itself the SETTIMG-OTR opcode of a torn
               command word) sails through that filter straight into strlen.
               Such a value reaches here two ways: (a) a segment buffer read torn
               mid-reload whose opcode byte happens to land on an OTR-filepath
               opcode, routed through `default:` with outW1 = the raw low32 token;
               (b) any future emit path that sets an OTR opcode without a real host
               string pointer. A legitimate O2R/pack emit always stores a heap or
               module string pointer, which on this 64-bit target has non-zero
               high bits and is readable -- so this guard is byte-identical for
               every real filepath command and only drops the poisoned ones. */
            {
                const uint8_t outOp = static_cast<uint8_t>((outW0 >> 24) & 0xFFu);
                /* Literal opcode bytes (fast/lus_gbi.h): 0x24 OTR_G_VTX_OTR_FILEPATH,
                   0x25 OTR_G_SETTIMG_OTR_FILEPATH, 0x27 OTR_G_DL_OTR_FILEPATH,
                   0x28 OTR_G_PUSHCD, 0x29 OTR_G_MTX_OTR_FILEPATH. Byte values are part
                   of the LUS OTR command format and stable across header revisions. */
                const bool outIsOtrFilepath = (outOp == 0x24u) || (outOp == 0x25u) ||
                                              (outOp == 0x27u) || (outOp == 0x28u) ||
                                              (outOp == 0x29u);
                if (outIsOtrFilepath &&
                    ((static_cast<uint64_t>(outW1) >> 32) == 0 || !IsReadableAddress(outW1))) {
                    if (mStats != nullptr) mStats->skippedDataCommands++;
                    continue;
                }
            }

            item.listPtr->commands.push_back(MakeLusGfx(outW0, outW1));
            if (mStats != nullptr) {
                mStats->commandsOut++;
                if (op == kOpSetTextureImage) mStats->textureCopies++;
            }

            /* Both terminators must stop translation. Only stopping on the EX2
               terminator let lists ending in F3D 0xB8 (notably lone-0xB8 "empty
               part" DLs in segment 3) run past their end into adjacent EX2
               sub-lists while still classified F3D, converting G_TRI2 (0x06)
               commands into bogus G_DL branches (the raw=0x00000406-family
               [gdl-bad] class). */
            if (op == kOpEndDl || op == 0xB8u) return;
        }

        item.listPtr->commands.push_back(MakeLusGfx(static_cast<uintptr_t>(kOpEndDl) << 24, 0));
    }
};

} // namespace

extern "C" void gdx_register_host_range(void* ptr, size_t size) {
    if ((ptr == nullptr) || (size == 0)) return;
    gHostRanges.push_back({ reinterpret_cast<uintptr_t>(ptr), size });
}

extern "C" void gdx_register_host_n64_command_range(void* ptr, size_t size) {
    if ((ptr == nullptr) || (size == 0)) return;
    gHostN64CommandRanges.push_back({ reinterpret_cast<uintptr_t>(ptr), size });
}

extern "C" void gdx_register_host_wide_command_range(void* ptr, size_t size) {
    if ((ptr == nullptr) || (size == 0)) return;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    const auto duplicate = std::find_if(
        gHostWideCommandRanges.begin(), gHostWideCommandRanges.end(),
        [begin, size](const HostRange& range) {
            return range.begin == begin && range.size == size;
        });
    if (duplicate == gHostWideCommandRanges.end()) {
        gHostWideCommandRanges.push_back({ begin, size });
    }
}

extern "C" void gdx_register_host_raw_n64_range(void* ptr, size_t size) {
    if ((ptr == nullptr) || (size == 0)) return;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    const auto duplicate = std::find_if(
        gRawN64Ranges.begin(), gRawN64Ranges.end(),
        [begin, size](const HostRange& range) {
            return range.begin == begin && range.size == size;
        });
    if (duplicate == gRawN64Ranges.end()) {
        gRawN64Ranges.push_back({ begin, size });
    }
}

// E3/A1/A2: registers a known-good compiled-in host array's own address/size so
// ResolveHostPointerStub (above, inside the anonymous namespace) can recognize
// wide SETTIMG pointers that carry the array's address directly instead of
// miscounting them as unbound stubs. Called once per entry from
// gdx_ek_assets_fill()'s fill loop (port/gen/EkAssetBindings.c, EXPANSION_KIT
// only) and once at RDRAM init from port/decomp_port.c for base-game arrays
// (gdx_rdram_init: fireworks sprites, sCourseMinimapPalette) -- not EK-gated,
// since the latter callers exist in every build.
extern "C" void gdx_register_host_pointer_stub(void* dest, size_t size) {
    if ((dest == nullptr) || (size == 0)) return;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(dest);
    const auto duplicate = std::find_if(
        gHostPointerStubs.begin(), gHostPointerStubs.end(),
        [begin, size](const HostRange& range) {
            return range.begin == begin && range.size == size;
        });
    if (duplicate == gHostPointerStubs.end()) {
        gHostPointerStubs.push_back({ begin, size });
    }
}

extern "C" void gdx_register_n64_address_range(unsigned int n64Begin, void* hostBegin, size_t size) {
    if ((n64Begin == 0) || (hostBegin == nullptr) || (size == 0)) return;
    const uintptr_t host = reinterpret_cast<uintptr_t>(hostBegin);
    const auto duplicate = std::find_if(
        gN64AddressRanges.begin(), gN64AddressRanges.end(),
        [n64Begin, host, size](const N64AddressRange& range) {
            return range.n64Begin == n64Begin && range.hostBegin == host && range.size == size;
        });
    if (duplicate == gN64AddressRanges.end()) {
        gN64AddressRanges.push_back({ n64Begin, host, size });
        /* A newly registered exact token can change pointer widening for an
           already-seen binary list. Force the G2 cache to rebuild on next use. */
        ++gConvertEpoch;
    }
}

extern "C" void gdx_register_n64_framebuffer(void* cpuAddr, unsigned int width, unsigned int height) {
    if (cpuAddr == nullptr || width == 0 || height == 0) {
        return;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(cpuAddr);
    auto existing = std::find_if(gN64Framebuffers.begin(), gN64Framebuffers.end(),
                                 [address](const N64FramebufferInfo& info) {
                                     return info.address == address;
                                 });
    if (existing == gN64Framebuffers.end()) {
        gN64Framebuffers.push_back({ address, width, height, false });
    } else {
        existing->width = width;
        existing->height = height;
    }

    const size_t byteCount = static_cast<size_t>(width) * height * sizeof(uint16_t);
    const auto nativeRange = std::find_if(gNativeRgba16Ranges.begin(), gNativeRgba16Ranges.end(),
                                         [address](const HostRange& range) {
                                             return range.begin == address;
                                         });
    if (nativeRange == gNativeRgba16Ranges.end()) {
        gNativeRgba16Ranges.push_back({ address, byteCount });
    }
}

/* Shared RGBA5551 -> 24bpp BMP writer for one-shot diagnostic dumps
   (transition capture, VI-fallback frame). Pixels are host-order u16, top-down. */
static void DumpRgba16Bmp(const char* filename, const uint16_t* pixels, unsigned int width,
                          unsigned int height) {
    std::FILE* f = std::fopen(filename, "wb");
    if (f == nullptr) {
        gdx_port_logf("[dump] BMP write failed: %s\n", filename);
        return;
    }
    const unsigned int rowBytes = (width * 3u + 3u) & ~3u;
    const unsigned int imageBytes = rowBytes * height;
    const unsigned int fileBytes = 14u + 40u + imageBytes;
    unsigned char header[54] = { 0 };
    header[0] = 'B'; header[1] = 'M';
    header[2] = static_cast<unsigned char>(fileBytes);
    header[3] = static_cast<unsigned char>(fileBytes >> 8);
    header[4] = static_cast<unsigned char>(fileBytes >> 16);
    header[5] = static_cast<unsigned char>(fileBytes >> 24);
    header[10] = 54;
    header[14] = 40;
    header[18] = static_cast<unsigned char>(width);
    header[19] = static_cast<unsigned char>(width >> 8);
    header[20] = static_cast<unsigned char>(width >> 16);
    header[21] = static_cast<unsigned char>(width >> 24);
    header[22] = static_cast<unsigned char>(height);
    header[23] = static_cast<unsigned char>(height >> 8);
    header[24] = static_cast<unsigned char>(height >> 16);
    header[25] = static_cast<unsigned char>(height >> 24);
    header[26] = 1;
    header[28] = 24;
    std::fwrite(header, 1, sizeof(header), f);
    std::vector<unsigned char> row(rowBytes, 0);
    for (unsigned int y = 0; y < height; y++) {
        const uint16_t* srcRow = pixels + static_cast<size_t>(height - 1 - y) * width;
        for (unsigned int x = 0; x < width; x++) {
            const uint16_t p = srcRow[x];
            const unsigned char r5 = (p >> 11) & 0x1F;
            const unsigned char g5 = (p >> 6) & 0x1F;
            const unsigned char b5 = (p >> 1) & 0x1F;
            row[x * 3 + 2] = static_cast<unsigned char>((r5 << 3) | (r5 >> 2));
            row[x * 3 + 1] = static_cast<unsigned char>((g5 << 3) | (g5 >> 2));
            row[x * 3 + 0] = static_cast<unsigned char>((b5 << 3) | (b5 >> 2));
        }
        std::fwrite(row.data(), 1, rowBytes, f);
    }
    std::fclose(f);
}

extern "C" void gdx_vi_set_next_framebuffer(void* cpuAddr) {
    gViNextFramebuffer = reinterpret_cast<uintptr_t>(cpuAddr);
}

extern "C" void gdx_vi_set_current_framebuffer(void* cpuAddr) {
    gViCurrentFramebuffer = reinterpret_cast<uintptr_t>(cpuAddr);
}

/* Shared RGBA5551 fullscreen compositor (framebuffer coherence).
 *
 * Uploads a 320x240 RGBA5551 CPU framebuffer as one host RGBA32 texture and draws
 * a single copy-mode rectangle over the whole screen. The caller MUST have already
 * established the frame (rapi StartFrame + StartDrawToFramebuffer + ClearFramebuffer).
 * Used by BOTH the VI-scanout fallback below (whole-frame present when no task ran)
 * and the boot-logo seed hook (background drawn under a task's content). Setting
 * loaded_texture directly (as GfxDpImageRectangle does for backgrounds) uploads all
 * 320x240 in one UploadTexture with no TMEM strip loop. */
static void SeedFramebufferQuad(Fast::Interpreter* interp, const uint16_t* srcPixels) {
    constexpr uint32_t kFbW = 320; // SCREEN_WIDTH  (decomp/include/macros.h)
    constexpr uint32_t kFbH = 240; // SCREEN_HEIGHT
    constexpr uint32_t kPixels = kFbW * kFbH;

    static uint8_t sConverted[kPixels * 4];
    gdx_convert_rgba5551_to_rgba8888(srcPixels, sConverted, kPixels);

    // Source-content probe: the seed/VI quad can only show what the CPU put in
    // the framebuffer. "Boot logo invisible with seeding enabled" needs this
    // one number to split loader-side (count==0: the game never blitted the
    // logo — port feature gap in the IPL/logo path) from renderer-side
    // (count>0: the quad draw itself is broken).
    {
        static int sSeedContentLogs = 0;
        // [seed] quad content probe: high-frequency diagnostic, silent unless GDX_DIAG_VERBOSE=1.
        if (gdx_diag_verbose() && sSeedContentLogs < 10) {
            ++sSeedContentLogs;
            size_t nonZero = 0;
            size_t nonBackground = 0; // pixels != 0x0001 (the RGBA5551 cleared-black
                                      // value): distinguishes real blitted content
                                      // (boot logo) from a bare clear — "all nonzero"
                                      // alone cannot (0x0001 counts as nonzero).
            for (size_t k = 0; k < kPixels; k++) {
                if (srcPixels[k] != 0) {
                    ++nonZero;
                }
                if (srcPixels[k] != 0 && srcPixels[k] != 0x0001) {
                    ++nonBackground;
                }
            }
            gdx_port_logf("[seed] quad source %p nonzero=%zu nonbg=%zu/%u\n",
                          reinterpret_cast<const void*>(srcPixels), nonZero, nonBackground, kPixels);
        }
    }

    interp->mRdp->viewport_or_scissor_changed = true;
    interp->mRenderingState.viewport = {};
    interp->mRenderingState.scissor = {};

    // Boot-phase frames run before the game ever sets an RDP scissor, leaving
    // it 0x0 ([gpustate] sc=0.0,0.0 0.0x0.0) — the interpreter clips every
    // rectangle against it, so the quad was drawn and fully scissored away
    // (a prior one-shot capture was solid black while the source FB held the logo).
    // Establish the full-screen scissor this draw needs; coords are 10.2 fixed.
    interp->GfxDpSetScissor(0 /*G_SC_NON_INTERLACE*/, 0, 0, kFbW << 2, kFbH << 2);

    constexpr int kTile = 0;
    auto& tile = interp->mRdp->texture_tile[kTile];
    tile.fmt = G_IM_FMT_RGBA;
    tile.siz = G_IM_SIZ_32b;
    tile.cms = G_TX_CLAMP;
    tile.cmt = G_TX_CLAMP;
    tile.masks = G_TX_NOMASK;
    tile.maskt = G_TX_NOMASK;
    tile.shifts = 0;
    tile.shiftt = 0;
    tile.uls = 0.0f;
    tile.ult = 0.0f;
    tile.lrs = static_cast<float>((kFbW - 1) * 4);
    tile.lrt = static_cast<float>((kFbH - 1) * 4);
    tile.tmem = 0;
    tile.tmem_index = 0;
    tile.palette = 0;
    // Nonzero to pass ImportTexture's zero-line guard; the draw derives width/height
    // from loaded_texture's line/size below.
    tile.line_size_bytes = kFbW * 2;

    Fast::LoadedTexture& loaded = interp->mRdp->loaded_texture[0];
    loaded = Fast::LoadedTexture{};
    loaded.addr = sConverted;
    loaded.orig_size_bytes = kPixels * 4;
    loaded.size_bytes = kPixels * 4;
    loaded.full_image_line_size_bytes = kFbW * 4;
    loaded.line_size_bytes = kFbW * 4;
    loaded.tex_flags = 0;
    loaded.masked = false;
    loaded.blended = false;

    interp->mRdp->first_tile_index = kTile;
    interp->mRdp->textures_changed[0] = true;
    interp->mRdp->textures_changed[1] = true;

    // The converted buffer lives at a fixed address, so evict any cache entry from a
    // previous present — otherwise ImportTexture would serve a stale upload and a
    // CPU-animated screen would freeze.
    interp->TextureCacheDelete(sConverted);

    // Copy cycle: GfxDpTextureRectangle auto-applies a TEXEL0 passthrough combine and
    // point filtering (dsdx=0x0400 = 1 texel/pixel, so 320 texels -> 320px).
    interp->GfxSpSetOtherMode(G_MDSFT_CYCLETYPE + 32, 2, static_cast<uint64_t>(G_CYC_COPY) << 32);

    // The CPU framebuffer is top-down, which D3D11 samples correctly. OpenGL samples the same
    // upload bottom-up, so the logo would appear upside-down on the GL path that bypasses the
    // post-pass V flip. Start from the top texel (S10.5) and sample downward on OpenGL only.
    int16_t texT = 0;
    int16_t dtdy = 0x0400;
    if (Fast::GfxRenderingAPI* rapi = interp->GetCurrentRenderingAPI()) {
        if (std::strcmp(rapi->GetName(), "OpenGL") == 0) {
            texT = static_cast<int16_t>((kFbH - 1) << 5);
            dtdy = -0x0400;
        }
    }

    interp->GfxDpTextureRectangle(0, 0, (kFbW - 1) << G_TEXTURE_IMAGE_FRAC, (kFbH - 1) << G_TEXTURE_IMAGE_FRAC,
                                  kTile, 0, texT, 0x0400, dtdy, false);
    interp->Flush();
}

extern "C" int gGameMode; // decomp global; GET_MODE = (gGameMode & 0x1F), GAMEMODE_TITLE == 0

// Persistent framebuffer holding a copy of the most recently completed game
// frame. Written at the end of every gdx_gfx_run task AND at the end of the
// VI-scanout fallback presenter (gdx_vi_present_fallback), so a transition
// snapshot taken during/after a boot-phase VI-fallback frame still sees a
// fresh mirror instead of a stale/empty one. Read by
// gdx_read_current_framebuffer (the game's transition snapshot). Declared
// this early (rather than immediately above gdx_gfx_run) so the VI-fallback
// presenter, which runs first in file order, can also write it.
static int gFrameMirrorFb = -1;
static bool gFrameMirrorValid = false;

// Shared epilogue for gdx_gfx_run and gdx_vi_present_fallback: refresh the persistent
// GPU-side frame mirror that transition snapshots read from (gdx_read_current_framebuffer).
// Both callers reach this point after producing a complete frame through the interpreter —
// one via a real GFX task, the other via the VI-scanout fallback quad — and the mirror
// update itself was byte-for-byte identical in both, so it now lives in one place instead
// of two copies that could silently drift. GPU->GPU copy, no CPU stall.
// Bounded, env-gated per-frame capture facility.
// GDX_CAPTURE_FRAMES=<startFrame>:<count> dumps <count> consecutive presented
// frames (numbered from process start, counted at every GdxUpdateFrameMirror
// call, i.e. once per presented host frame regardless of real-task vs VI-fallback
// path) to gdxcap_NNNNN.bmp in the working directory. Zero cost when the env var
// is unset (single cached bool check). Kept in-tree as a debugging tool.
static void GdxCaptureFrameIfRequested(const std::shared_ptr<Fast::Interpreter>& interp) {
    static int sCapState = -1;   // -1 = unparsed, 0 = disabled, 1 = enabled
    static int sCapStart = 0;    // frames (or matching-mode frames) to skip
    static int sCapCount = 0;    // frames to dump
    static int sCapMode = -1;    // -1 = any mode; else GET_MODE gate (0x1F mask)
    static long sCapFrame = 0;   // global presented-frame counter (for filenames)
    static int sCapSkipped = 0;  // mode-matching frames skipped so far
    static int sCapDumped = 0;   // frames dumped so far
    if (sCapState == -1) {
        sCapState = 0; // parse exactly once
        const char* env = std::getenv("GDX_CAPTURE_FRAMES");
        if (env != nullptr && *env != '\0') {
            int start = 0, count = 0;
            if (std::sscanf(env, "%d:%d", &start, &count) == 2 && count > 0) {
                sCapStart = start;
                sCapCount = count;
                sCapState = 1;
                const char* menv = std::getenv("GDX_CAPTURE_MODE");
                if (menv != nullptr && *menv != '\0') {
                    sCapMode = static_cast<int>(std::strtol(menv, nullptr, 0)) & 0x1F;
                }
                gdx_port_logf("[gdxcap] enabled: start=%d count=%d modeGate=%d\n", start, count, sCapMode);
            }
        }
    }
    const long frame = sCapFrame++;
    if (sCapState != 1 || sCapDumped >= sCapCount) {
        return;
    }
    if (sCapMode >= 0) {
        // Mode-gated: only count/dump frames while GET_MODE == modeGate.
        if ((gGameMode & 0x1F) != sCapMode) {
            return;
        }
        if (sCapSkipped < sCapStart) {
            ++sCapSkipped;
            return;
        }
    } else {
        // Frame-number-gated: dump the window [start, start+count).
        if (frame < sCapStart) {
            return;
        }
    }
    if (gFrameMirrorFb < 0) {
        return;
    }
    static uint16_t sCapPixels[320 * 240];
    interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(gFrameMirrorFb, 320, 240, sCapPixels);
    char name[64];
    std::snprintf(name, sizeof(name), "gdxcap_%05ld.bmp", frame);
    DumpRgba16Bmp(name, sCapPixels, 320, 240);
    ++sCapDumped;
    gdx_port_logf("[gdxcap] dumped frame %ld -> %s (gameMode=0x%X)\n", frame, name, (gGameMode & 0x1F));
}

// GDX_INPUT_SCRIPT (dev-only) SHOT hook: one-shot named framebuffer dump requested by
// gdx_input_script.c. Arms a label; the next GdxUpdateFrameMirror call (i.e. the next presented
// frame) reuses the exact same read-back + BMP encode path as GdxCaptureFrameIfRequested above
// (ReadFramebufferToCPU + DumpRgba16Bmp) to write "autotest/<label>.bmp". A plain global instead
// of a queue: SHOT is a single-script, one-in-flight-at-a-time dev feature, and
// gdx_input_script_override() only ever issues the next SHOT after the current poll's pad state
// has been consumed, so two requests can never race.
static bool gPendingNamedDumpArmed = false;
static char gPendingNamedDumpLabel[128];

extern "C" void gdx_request_frame_dump(const char* label) {
    if (label == nullptr || label[0] == '\0') {
        return;
    }
    std::snprintf(gPendingNamedDumpLabel, sizeof(gPendingNamedDumpLabel), "%s", label);
    gPendingNamedDumpArmed = true;
}

static void GdxDumpNamedFrameIfRequested(const std::shared_ptr<Fast::Interpreter>& interp) {
    if (!gPendingNamedDumpArmed || gFrameMirrorFb < 0) {
        return;
    }
    gPendingNamedDumpArmed = false;

#ifdef _WIN32
    CreateDirectoryA("autotest", nullptr); // ERROR_ALREADY_EXISTS is fine (see disk_savefile.cpp)
#endif

    static uint16_t sNamedDumpPixels[320 * 240];
    interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(gFrameMirrorFb, 320, 240, sNamedDumpPixels);
    char path[192];
    std::snprintf(path, sizeof(path), "autotest/%s.bmp", gPendingNamedDumpLabel);
    DumpRgba16Bmp(path, sNamedDumpPixels, 320, 240);
    gdx_port_logf("[autotest] SHOT dumped -> %s\n", path);
}

/* [interp-shot] Capture one sub-frame's rendered image. Called from Fast3dWindow (libultraship)
   immediately after Interpreter::Run and BEFORE gui->EndDraw / EndFrame -- the only point where the
   image exists and nothing has been blitted or presented. Capturing after the present compares
   FLIP_DISCARD back buffers whose contents are undefined, which is how a previous attempt produced
   a meaningless "44% of pixels differ" between two passes fed identical matrices.

   Source depends on where the game actually drew: mRendersToFb selects an intermediate game
   framebuffer (widescreen pillarbox, MSAA, or a viewport/resolution mismatch), otherwise the draw
   went straight to framebuffer 0. Reading the wrong one yields a stale or blank image. */
extern "C" void gdx_gfx_post_run_capture(void) {
    if (gGdxShotArmedPass < 0 || gFrameMirrorFb < 0) {
        return;
    }
    const int pass = gGdxShotArmedPass;
    gGdxShotArmedPass = -1; // one shot per arming, regardless of what happens below

    auto wnd = Ship::Context::GetInstance() != nullptr ? Ship::Context::GetInstance()->GetWindow() : nullptr;
    if (wnd == nullptr) {
        return;
    }
    auto* fw = dynamic_cast<Fast::Fast3dWindow*>(wnd.get());
    if (fw == nullptr) {
        return;
    }
    auto interp = fw->GetInterpreterWeak().lock();
    if (interp == nullptr) {
        return;
    }
    const int src = interp->mRendersToFb ? interp->mGameFb : 0;
    interp->CopyFrameBuffer(gFrameMirrorFb, src, false, nullptr);
    static uint16_t sShotPixels[320 * 240];
    interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(gFrameMirrorFb, 320, 240, sShotPixels);
#ifdef _WIN32
    CreateDirectoryA("autotest", nullptr);
#endif
    char shotPath[192];
    std::snprintf(shotPath, sizeof(shotPath), "autotest/interp_pass%d.bmp", pass);
    DumpRgba16Bmp(shotPath, sShotPixels, 320, 240);
    gdx_port_logf("[interp-shot] pass=%d src=%s(%d) -> %s\n", pass,
                  interp->mRendersToFb ? "gameFb" : "fb0", src, shotPath);
}

static void GdxUpdateFrameMirror(const std::shared_ptr<Fast::Interpreter>& interp) {
    if (gFrameMirrorFb < 0) {
        gFrameMirrorFb = interp->CreateFrameBuffer(320, 240, 320, 240, 1, false);
    }
    if (gFrameMirrorFb >= 0) {
        interp->CopyFrameBuffer(gFrameMirrorFb, 0, false, nullptr);
        gFrameMirrorValid = true;
    }
    GdxCaptureFrameIfRequested(interp);
    GdxDumpNamedFrameIfRequested(interp);
}

/* PORT boot-logo seed (framebuffer coherence).
 *
 * Registered as the interpreter's after-clear hook (Interpreter::SetPortAfterClearHook)
 * ONLY when GDX_SEED_BOOT_LOGO is enabled (see gdx_gfx_run). It runs on the
 * freshly-cleared canvas, BEFORE the frame's task commands, so the CPU-written VI
 * framebuffer (the boot logo blitted by func_806F33D0 / func_80069F5C, which no RDP
 * task renders) shows as a background UNDER the task's overlay content instead of a
 * black canvas. Gated to the boot/title phase (GAMEMODE_TITLE) so it can never
 * affect gameplay/menus. This complements the already-present GPU->CPU readback for
 * transitions (gdx_read_current_framebuffer). Left opt-in because it cannot be
 * runtime-validated without a launch. */
/* The hook is registered UNCONDITIONALLY (which removes any registration-order /
   env-timing question); the opt-in gate lives here, per call, on a cached env check.
   Set by gdx_gfx_run's first-frame env probe. */
static bool gSeedBootLogoEnabled = false;

static void SeedBootLogoAfterClear(Fast::Interpreter* interp) {
    if (!gSeedBootLogoEnabled) {
        return; // Opt-in via GDX_SEED_BOOT_LOGO (see gdx_gfx_run).
    }
    if ((gGameMode & 0x1F) != 0) {
        return; // Boot/title phase only.
    }
    const uintptr_t fbAddr = gViCurrentFramebuffer;
    if (fbAddr == 0) {
        return; // No framebuffer presented yet.
    }
    SeedFramebufferQuad(interp, reinterpret_cast<const uint16_t*>(fbAddr));
}

/* Present-path telemetry (Course-Edit whole-window + ImGui strobe).
 *
 * Env-gated on GDX_PRESENT_PATH_TRACE. In the default (single-present) frame
 * loop gdx_vi_present_fallback runs once per host frame and can classify which
 * path actually produced the present: a real gfx task (task-render), the
 * taskless full-res hold re-composite (hold-recomposite), or a taskless
 * VI-scanout draw (vifb-*). Identical consecutive frames are coalesced into
 * run-length lines so a 60 fps soak yields a compact alternation trace instead
 * of 60 lines/second. A pure strobe (task-render x1 / hold-recomposite x1 /
 * task-render x1 / ...) is the exact signature of the defect, and it is measurable
 * at boot/title wherever rendersToFb is taskless, without entering the editor.
 * Zero cost unless the env var is set.
 *
 * All callers pass a string LITERAL, so the pointer identity comparison below
 * is a valid "same path" test within this translation unit. */
static void GdxPresentPathTrace(const char* path) {
    if (!gdx_dev_gate(GDX_GATE_PRESENT_PATH_TRACE)) {
        return;
    }
    static const char* sLastPath = nullptr;
    static unsigned sRunLen = 0;
    static unsigned long long sTotalLines = 0;
    if (path == sLastPath) {
        ++sRunLen;
        return;
    }
    if (sLastPath != nullptr && sTotalLines < 4000) {
        ++sTotalLines;
        gdx_port_logf("[present-path] %s x%u\n", sLastPath, sRunLen);
    }
    sLastPath = path;
    sRunLen = 1;
}

/* Hold-recomposite readback probe (40fps-on-menus regression, interp ON).
 *
 * Env-gated on GDX_DIAG_HOLD=1. Confirms in one soak that the taskless hold path
 * (gdx_vi_present_fallback's holdGpuFrame branch below) no longer pays a
 * synchronous CPU readback: it now blits the persistent frame mirror straight
 * into the current draw target on the GPU (Interpreter::CopyFrameBuffer, the
 * same primitive GdxUpdateFrameMirror already uses in the other direction),
 * instead of ReadFramebufferToCPU (D3D11 Map + a full-frame CPU box-filter
 * downscale) into a CPU buffer that SeedFramebufferQuad then re-uploads as a
 * texture. `contentChangedSincePrevHold` is true when a real GFX task rendered
 * since the previous hold tick (i.e. the OLD code would have paid a readback
 * here) -- logged so the previously-every-other-frame cadence is visible
 * alongside proof that every single hold tick, changed or not, is now GPU-only. */
static void GdxDiagHoldTick(bool contentChangedSincePrevHold) {
    if (!gdx_dev_gate(GDX_GATE_DIAG_HOLD)) {
        return;
    }
    static unsigned long long sHoldTicks = 0;
    static unsigned long long sChangedTicks = 0;
    ++sHoldTicks;
    if (contentChangedSincePrevHold) {
        ++sChangedTicks;
    }
    if (sHoldTicks <= 16 || (sHoldTicks % 60) == 0) {
        gdx_port_logf("[hold-diag] tick=%llu changedSincePrevHold=%d(total=%llu) mode=gpu-copy readback=0\n",
                      sHoldTicks, contentChangedSincePrevHold ? 1 : 0, sChangedTicks);
    }
}

/* Present-target invariant: leave framebuffer 0 bound on EVERY exit from
 * gdx_vi_present_fallback.
 *
 * The host composites ImGui immediately after that function returns
 * (main.cpp:1273 -> 1274 on the default single-present path, 1307 -> 1308 on the
 * interpolation path), and neither ImGui backend selects a render target of its
 * own for the main viewport: imgui_impl_opengl3.cpp issues no glBindFramebuffer
 * anywhere, and imgui_impl_dx11.cpp's only OMSetRenderTargets lives in
 * ImGui_ImplDX11_RenderWindow, which serves secondary platform viewports. The
 * game image and the whole enhancement menu therefore land on whatever target
 * happens to be bound — the composite has always INHERITED its render target
 * rather than asserted one.
 *
 * Why that inheritance is fragile here and not upstream: this port's frame loop
 * is inverted. The entire game frame, interp->Run() included, executes inside
 * gdx_vi_tick() at main.cpp:1232 — posting the VI retrace message dispatches the
 * game fiber synchronously, so the gfx task is submitted and run right there —
 * which means Run() finishes BEFORE the host opens the frame with
 * gui->StartDraw() / w->StartFrame() at main.cpp:1252-1253. Upstream, Run() is
 * the last thing to touch the rendering API before the ImGui composite. Here
 * Interpreter::StartFrame (interpreter.cpp:7243) runs after it, and whenever
 * mRendersToFb is true it unconditionally re-runs
 * UpdateFramebufferParameters(mGameFb, ...) (interpreter.cpp:7288-7298).
 *
 * That is exactly where the two backends diverge.
 * GfxRenderingAPIDX11::UpdateFramebufferParameters never touches the output
 * merger, so the framebuffer-0 binding Run() left behind (interpreter.cpp:7482
 * on the mRendersToFb path; on the !mRendersToFb path its prologue at
 * interpreter.cpp:7431 targeted fb 0 directly) survives StartFrame and the
 * inherited composite happens to be correct.
 * GfxRenderingAPIOGL::UpdateFramebufferParameters glBindFramebuffer()s the
 * framebuffer it is about to reconfigure and never restores the previous one,
 * so on GL that same StartFrame leaves mGameFb's FBO bound. The Expansion Kit
 * editors force mRendersToFb (whole-frame fixed-aspect pillarbox), so the game
 * image and the entire menu were composited into the offscreen texture while the
 * window presented nothing but the bare black clear from Run()'s prologue:
 * Create Machine invisible, Course Edit strobing, both correct on D3D11.
 *
 * So state the invariant instead of assuming it. This is a re-bind, not a frame
 * setup, and the two omissions are load-bearing:
 *  - noiseScale is 0.0f deliberately. Both backends skip the dither-noise
 *    uniform update when it is zero (the `if (noise_scale != 0.0f)` guard in
 *    GfxRenderingAPIOGL::StartDrawToFramebuffer and in
 *    GfxRenderingAPIDX11::StartDrawToFramebuffer), so whatever a real frame
 *    latched is preserved rather than overwritten with 1/0.
 *  - It does NOT clear. On a !mRendersToFb frame Run() rendered the game
 *    straight into framebuffer 0, so a clear here would erase the very frame
 *    about to be presented.
 * On D3D11 the call is consequently inert: it rebinds an already-bound RTV,
 * assigns mRenderTargetHeight the value it already holds, skips the noise
 * update, and re-uploads a byte-identical PerFrameCB. D3D11 behaviour is
 * unchanged.
 *
 * The hold re-composite and VI-scanout branches below select their own draw
 * targets after this runs and are unaffected. The exits this exists for are the
 * task-render fast path (by far the common one), the no-VI-framebuffer return,
 * and every acquisition failure in between.
 *
 * The rendering API pointer is cached the same way gdx_gfx_run caches its window
 * pointer, for the same reason: Interpreter::Init assigns mRapi once for the
 * process (interpreter.cpp:7156), while re-reading the Context's window
 * shared_ptr every frame is the per-frame refcount touch that crashed in
 * _Ptr_base<Window>::_Incref during rapid mode transitions. Any step that yields
 * null leaves the cache empty and returns; the next frame retries.
 */
static void GdxBindWindowFramebuffer() {
    static Fast::GfxRenderingAPI* sPresentRapi = nullptr;
    if (sPresentRapi == nullptr) {
        auto ctx = Ship::Context::GetInstance();
        if (ctx == nullptr) {
            return;
        }
        auto wnd = ctx->GetWindow();
        Fast::Fast3dWindow* fw = static_cast<Fast::Fast3dWindow*>(wnd.get());
        if (fw == nullptr) {
            return;
        }
        auto interp = fw->GetInterpreterWeak().lock();
        if (!interp) {
            return;
        }
        sPresentRapi = interp->GetCurrentRenderingAPI();
        if (sPresentRapi == nullptr) {
            return;
        }
    }
    // noiseScale 0 = "leave the noise uniform alone"; no clear, see above.
    sPresentRapi->StartDrawToFramebuffer(0, 0.0f);
}

/* VI-scanout fallback (boot-logo black screen fix, host-side).
 *
 * On the N64 the VI chip scans out whatever u16 pixels sit in the framebuffer
 * RDRAM, regardless of who wrote them. F-Zero X's boot N64/64DD logo is drawn
 * that way: sys_main.c's func_806F33D0 / func_80069F5C CPU-blit pixels straight
 * into a gFrameBuffers[] FrameBuffer (see decomp/include/gfx.h: a union whose
 * `u16 array[240][320]` view is RGBA5551) with NO RDP graphics task ever
 * submitted, then the boot code osViSwapBuffer()s that buffer and holds it on
 * screen for ~3.8s (sys_gfx.c Game_ThreadEntry) while nothing is drawn.
 *
 * This port's presentation is task-driven — everything visible comes from
 * interp->Run() rendering a parsed F3D display list — so a framebuffer nothing
 * draws through that pipeline shows only the (black) GL/D3D render target. This
 * function is the missing "VI reads raw memory" fallback.
 *
 * Design (learned from a prior failed attempt — do not regress):
 *  - It runs on the HOST/render side (called from main.cpp's frame loop after
 *    gdx_dispatch()), inside the window's already-open StartFrame/EndFrame
 *    bracket — NOT injected from a game fiber, which had no valid frame context
 *    and rendered nothing.
 *  - It is cheap: a single 320x240 texture upload + one fullscreen quad, no
 *    per-call GPU sync (no ReadFramebuffer, no extra EndFrame — the host loop's
 *    EndFrame presents), so it never stalls the audio fiber.
 *  - When a real GFX task rendered this frame (gHostFrameGfxTaskRan), it is a
 *    single boolean check and immediate return — zero cost during gameplay.
 *
 * The pixels are converted RGBA5551 -> RGBA8888 on the CPU
 * (gdx_convert_rgba5551_to_rgba8888) and uploaded as one RGBA32 texture; the
 * interpreter's own texture-rectangle path (GfxDpTextureRectangle) then draws
 * the quad, reusing its tested VBO/shader/viewport machinery. Uploading the
 * full frame in one shot (rather than the old 4 KB-TMEM strip blit) is why the
 * tile's loaded_texture fields are set directly here instead of via
 * GfxDpLoadTile — the emulated TMEM only bounds LoadTile, not import.
 */
extern "C" void gdx_vi_present_fallback(void) {
    // Framebuffer 0 is the draw target the host's ImGui composite (main.cpp:1274)
    // requires, and every exit below must satisfy it -- including the ones that
    // return before any of this function's own work happens. See
    // GdxBindWindowFramebuffer above for the full chain.
    GdxBindWindowFramebuffer();

    // A real GFX task already produced this host frame: nothing to do. Clear the
    // flag for the next frame. This is the hot path once gameplay is rendering.
    if (gHostFrameGfxTaskRan) {
        gHostFrameGfxTaskRan = false;
        GdxPresentPathTrace("task-render");
        return;
    }

    const uintptr_t fbAddr = gViCurrentFramebuffer;
    if (fbAddr == 0) {
        // Traced: this was the one exit with no telemetry at all, which made it
        // invisible in a present-path soak (every other exit reports a path).
        GdxPresentPathTrace("no-vi-fb");
        return; // No framebuffer has been presented yet.
    }

    // Cache the window pointer once (same rationale as gdx_gfx_run): avoid a
    // per-frame refcount touch on the Context's window shared_ptr.
    static Fast::Fast3dWindow* sFallbackWindow = nullptr;
    if (sFallbackWindow == nullptr) {
        auto ctx = Ship::Context::GetInstance();
        if (ctx == nullptr) {
            return;
        }
        auto wnd = ctx->GetWindow();
        sFallbackWindow = static_cast<Fast::Fast3dWindow*>(wnd.get());
    }
    Fast::Fast3dWindow* fw = sFallbackWindow;
    if (fw == nullptr) {
        return;
    }
    auto interp = fw->GetInterpreterWeak().lock();
    if (!interp) {
        return;
    }
    Fast::GfxRenderingAPI* rapi = interp->GetCurrentRenderingAPI();
    if (rapi == nullptr) {
        return;
    }

    // Full-resolution hold path: when the game renders to an offscreen framebuffer
    // (menus, pillarboxed modes such as the Expansion Kit editors) and this tick submits
    // NO gfx task, the last frame's texture is STILL in mGameFb and interp->mGfxFrameBuffer
    // still points at it (Interpreter::StartFrame does not reset it), so Fast3dGui::DrawGame
    // can re-composite the retained frame.
    //
    // ROOT CAUSE (Course-Edit whole-window + ImGui strobe): the previous
    // implementation early-returned here doing NOTHING, on the assumption that "a taskless
    // present needs no work — returning re-presents the previous frame". That assumption is
    // false in this port's split frame loop. main.cpp unconditionally runs, AFTER this
    // function returns, gui->EndDraw() (which builds a FRESH ImGui frame — the held game
    // image via DrawGame plus the enhancement menu — and renders it) and interp->EndFrame()
    // (SwapBuffers). That composite targets the window backbuffer (framebuffer 0). On the
    // DXGI flip-model swap chain the backbuffer must be re-acquired, re-bound and cleared
    // every presented frame — exactly what Interpreter::Run()/RunGuiOnly() do in their
    // prologue (UpdateFramebufferParameters(0) -> StartFrame -> StartDrawToFramebuffer(0) ->
    // ClearFramebuffer). The early-return skipped ALL of it, so a hold frame's ImGui composite
    // landed on an unprepared/stale backbuffer. Task frames prepared the backbuffer; hold
    // frames did not, so as the two frame kinds alternated the ENTIRE window — enhancement
    // menu included — strobed. ImGui is composited by the host into the same backbuffer as the
    // game (Fast3dGui::DrawGame draws the game FB as an ImGui image inside the "Main Game"
    // window), which is why the menu flickered in lockstep with the game content. The bug is
    // invisible outside pillarboxed taskless modes because a
    // non-mRendersToFb taskless frame falls through to the VI-scanout path below, which already
    // runs the full fb-0 prologue.
    //
    // Fix: run the same fb-0 prologue a real frame runs, but do NOT touch mGameFb (it holds the
    // frame we are re-presenting) or mGfxFrameBuffer (it still references that frame's texture).
    // gui->EndDraw() then composites the retained frame into a freshly prepared, cleared
    // backbuffer — pixel-identical pipeline to a task frame, so the strobe disappears.
    if (sGpuContentLive && interp->mRendersToFb && interp->mGfxFrameBuffer != 0) {
        interp->SpReset();
        rapi->UpdateFramebufferParameters(0, interp->mGfxCurrentWindowDimensions.width,
                                          interp->mGfxCurrentWindowDimensions.height, 1, false, true, true,
                                          !interp->mRendersToFb);
        rapi->StartFrame();
        // noiseScale=1, not mCurDimensions.height/mNativeDimensions.height: that
        // ratio (used by Interpreter::StartFrame's callers when the draw target is
        // mGameFb) feeds N64->internal coordinate scaling for the internal-resolution
        // game surface -- StartDrawToFramebuffer's second parameter is actually the
        // dither-noise uniform (noise_scale in the backends), unrelated to that
        // ratio. This call's target is unconditionally framebuffer 0 (the window
        // backbuffer, already at window resolution) -- exactly the same shape as
        // Interpreter::Run()/RunGuiOnly()'s OWN unconditional-fb-0 calls (their
        // MSAA-resolve epilogue and failsafe-reset paths), which also pass a literal
        // 1. Verified against interpreter.cpp's StartFrame/Run/RunGuiOnly.
        rapi->StartDrawToFramebuffer(0, 1);
        rapi->ClearFramebuffer(true, true);
        // mGfxFrameBuffer intentionally left unchanged: mGameFb still holds last frame (no gfx
        // task cleared it this tick), so DrawGame re-composites it. The host loop's EndFrame
        // presents. Mirror update is deliberately skipped (as before): the held content is
        // already the mirror, so re-capturing it would only add a needless blit.
        GdxPresentPathTrace("hold-recomposite");
        return;
    }

    // --- Frame prologue: exactly what Interpreter::Run() establishes before a
    //     task's commands. The host's w->StartFrame() only ran interp->StartFrame()
    //     (which set mRendersToFb / framebuffer params); the rapi frame + draw
    //     target + clear are done by Run(), which is not called this frame. ---
    interp->SpReset();
    rapi->UpdateFramebufferParameters(0, interp->mGfxCurrentWindowDimensions.width,
                                      interp->mGfxCurrentWindowDimensions.height, 1, false, true, true,
                                      !interp->mRendersToFb);
    rapi->StartFrame();
    rapi->StartDrawToFramebuffer(interp->mRendersToFb ? interp->mGameFb : 0,
                                 interp->mNativeDimensions.height != 0
                                     ? static_cast<float>(interp->mCurDimensions.height) / interp->mNativeDimensions.height
                                     : 1.0f);
    rapi->ClearFramebuffer(true, true);

    // Source selection. Once GPU content is live, a taskless present re-presents
    // the last GPU frame — the CPU VI framebuffer holds no pixels for
    // GPU-rendered screens and blitting it flashed the whole screen black.
    // Before the first GFX task (boot logo and other genuinely CPU-drawn
    // phases) the VI framebuffer is the real image source, exactly as before.
    //
    // 40fps-on-menus fix (interp ON): 2D menus alternate task/no-task host
    // ticks close to 1:1, so a real GFX task refreshes the persistent frame
    // mirror on almost every OTHER tick — sGpuHoldPixelsStale (set at the tail
    // of every task tick, see gdx_gfx_run) therefore also flips true almost
    // every other tick. That is legitimate staleness (the mirror really did
    // just get new content), so no amount of smarter invalidation shrinks the
    // number of hold ticks that need fresh pixels — the ONLY way to fix a
    // near-1:1 alternation is to make refreshing a hold tick cheap. The old
    // code paid for that refresh with rapi->ReadFramebufferToCPU: a D3D11
    // Map(D3D11_MAP_READ) that blocks on the GPU plus an O(w*h) CPU box-filter
    // downscale (gfx_direct3d11.cpp, added for Issue C's fade dash-band fix) —
    // a synchronous stall on the hot path, which is what halved menu fps.
    //
    // Fix: composite the mirror with a GPU->GPU copy instead of a CPU
    // roundtrip. gFrameMirrorFb is a real entry in the interpreter's
    // mFrameBuffers map (created via CreateFrameBuffer(..., resize=true)), so
    // Interpreter::StartFrame — which the host's per-frame w->StartFrame() call
    // always runs, hold tick or not — keeps it resized in lockstep with every
    // other framebuffer (mGameFb, fb 0) whenever the window/native dimensions
    // change (interpreter.cpp:StartFrame, the mFrameBuffers resize loop). That
    // means gFrameMirrorFb and the current draw target are always the same
    // pixel size, so Interpreter::CopyFrameBuffer's DX11 backend takes its
    // fast same-size CopyResource path (no scaling shader, no readback) — the
    // exact primitive GdxUpdateFrameMirror already uses in the other direction
    // every task tick. This also drops the previous round-trip's quality loss
    // (320x240 box-filter downsample immediately followed by a 1:1-texel
    // upload and a nearest-filter re-stretch back to full resolution).
    //
    // Invariants preserved: a hold tick still requires sGpuContentLive (so it
    // can never fire before the first real task has rendered — the "1 of every
    // 3 presents flashed black" fix above is untouched) and the vifb-scanout
    // (non-GPU-content-live) path below is byte-for-byte unchanged. Branch A
    // (the mRendersToFb re-composite above) is untouched.
    const bool holdGpuFrame = sGpuContentLive && gFrameMirrorValid && gFrameMirrorFb >= 0;
    GdxPresentPathTrace(holdGpuFrame ? "vifb-held-gpu-mirror" : "vifb-vi-scanout");
    if (holdGpuFrame) {
        // Diag only: true iff a real task refreshed the mirror since the previous
        // hold tick — i.e. the case that used to force a CPU readback here. The
        // GPU copy below runs unconditionally either way; sGpuHoldPixelsStale no
        // longer gates anything, it is read-and-cleared purely for this probe.
        const bool contentChangedSincePrevHold = sGpuHoldPixelsStale;
        sGpuHoldPixelsStale = false;
        interp->CopyFrameBuffer(interp->mRendersToFb ? interp->mGameFb : 0, gFrameMirrorFb, false, nullptr);
        GdxDiagHoldTick(contentChangedSincePrevHold);
    } else {
        // Convert + upload the VI framebuffer's CPU-written RGBA5551 pixels and draw
        // them as one fullscreen copy-mode rectangle. The pointer tracked at
        // osViSwapBuffer time is a real host pointer, read directly as u16.
        SeedFramebufferQuad(interp.get(), reinterpret_cast<const uint16_t*>(fbAddr));
    }

    // --- Frame epilogue: same as Run(). When rendering to an offscreen game FB
    //     (resolution multiplier / MSAA), publish it for the GUI compositor. ---
    interp->mGfxFrameBuffer = 0;
    if (interp->mRendersToFb) {
        // Mirror Interpreter::Run()'s MSAA-resolve decision. This taskless path runs only when NO
        // gfx task executed this frame, so Interpreter::Run() (and its mid-frame fixed-aspect
        // re-latch) did NOT run; the caches here are exactly the ones interp->StartFrame() latched
        // and sized mGameFbMsaaResolved from. That makes the resolved target guaranteed-allocated
        // whenever this branch selects it (no divergence to guard against, unlike Run()'s epilogue).
        const bool widescreenPillarbox = !interp->mWidescreenEnabledCache || interp->mForceFixedAspectCache;
        // noiseScale=1 is correct here, not the mCurDimensions/mNativeDimensions ratio:
        // that ratio is only meaningful for a draw target that can be mGameFb (it feeds
        // N64->internal coordinate scaling elsewhere), while StartDrawToFramebuffer's
        // second parameter is the dither-noise uniform (noise_scale in the backends).
        // This target is unconditionally framebuffer 0, matching interpreter.cpp's OWN
        // MSAA-resolve epilogue (Run()/RunGuiOnly(), same "if (mRendersToFb) {
        // StartDrawToFramebuffer(0, 1); ..." shape this block mirrors).
        rapi->StartDrawToFramebuffer(0, 1);
        rapi->ClearFramebuffer(true, true);
        if (interp->mMsaaLevel > 1) {
            if (interp->ViewportMatchesRendererResolution() && !widescreenPillarbox) {
                // Normal path: resolve straight to the window; mGfxFrameBuffer stays 0.
                rapi->ResolveMSAAColorBuffer(0, interp->mGameFb);
            } else {
                // Viewport differs OR a whole-frame pillarbox is required: resolve into the offscreen
                // target and publish it so Fast3dGui::DrawGame composites the centred 4:3 sub-region.
                // This is what keeps the fallback from leaving mGfxFrameBuffer == 0 when a pillarbox
                // is required (which would stretch 4:3 content across the whole window).
                rapi->ResolveMSAAColorBuffer(interp->mGameFbMsaaResolved, interp->mGameFb);
                interp->mGfxFrameBuffer =
                    reinterpret_cast<uintptr_t>(rapi->GetFramebufferTextureId(interp->mGameFbMsaaResolved));
            }
        } else {
            interp->mGfxFrameBuffer = reinterpret_cast<uintptr_t>(rapi->GetFramebufferTextureId(interp->mGameFb));
        }
    }
    // The host loop's w->EndFrame() presents this frame — do NOT EndFrame here.

    static int sFallbackLogs = 0;
    if (sFallbackLogs < 8) {
        ++sFallbackLogs;
        gdx_port_logf("[vifallback] presented %s fb=%p (%ux%u, rendersToFb=%d)\n",
                      holdGpuFrame ? "held GPU frame (mirror)" : "VI framebuffer",
                      reinterpret_cast<void*>(fbAddr), 320u, 240u, static_cast<int>(interp->mRendersToFb));
    }

    // Transition snapshot mirror: identical to the update done at the tail of
    // gdx_gfx_run. Boot-phase frames are often presented entirely through this
    // VI-scanout fallback (no GFX task runs), so without this the mirror stays
    // stale/empty until the first real task, and any transition snapshot taken
    // during/after boot reads garbage. Skipped on hold frames: the composed
    // content IS the mirror, and re-capturing it would only add a needless blit.
    if (!holdGpuFrame) {
        GdxUpdateFrameMirror(interp);
    }

}

extern "C" void gdx_set_native_rgba16_texture_range(void* ptr, size_t size, int enabled) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    gNativeRgba16Ranges.erase(
        std::remove_if(gNativeRgba16Ranges.begin(), gNativeRgba16Ranges.end(),
                       [begin](const HostRange& range) { return range.begin == begin; }),
        gNativeRgba16Ranges.end());
    if (enabled && ptr != nullptr && size != 0) {
        gNativeRgba16Ranges.push_back({begin, size});
    }
    if (ptr != nullptr && size != 0 && IsRdramHostPointer(begin)) {
        gdx_record_dma_load(static_cast<uint32_t>(begin - reinterpret_cast<uintptr_t>(gdx_rdram)), 0,
                            static_cast<uint32_t>(std::min<size_t>(size, UINT32_MAX)));
    }
}

extern "C" void gdx_defer_native_rgba16_texture_range_clear(void* ptr) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    if (begin != 0) {
        gPendingNativeRgba16RangeClears.push_back(begin);
    }
}

/* Minimap staleness fix: sCourseMinimapTex (minimap.c) is a CI8 texture that is
 * Arena_Allocate'd per race under EXPANSION_KIT. Fast3D's texture cache keys CI8
 * by address with no content hash, and the deterministic arena rewind hands the
 * same address to the next race, so a cache HIT serves the previous race's decoded
 * outline. Evicting the exact buffer address after it has been re-rasterized forces
 * the next upload to re-decode the current course. Scoped to this one producer on
 * purpose -- a general CI content-hash caused a race regression (see the CI-hash
 * note in libultraship interpreter.cpp). Mirrors the TextureCacheDelete usage in
 * SeedFramebufferQuad and is safe before the interpreter is up (early-out on null),
 * so callers can invoke it unconditionally. */
extern "C" void gdx_invalidate_texture_address(const void* addr) {
    if (addr == nullptr) {
        return;
    }
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return;
    }
    auto wnd = ctx->GetWindow();
    auto* fw = static_cast<Fast::Fast3dWindow*>(wnd.get());
    if (fw == nullptr) {
        return;
    }
    auto interp = fw->GetInterpreterWeak().lock();
    if (!interp) {
        return;
    }
    interp->TextureCacheDelete(reinterpret_cast<const uint8_t*>(addr));
}

/* Transition-capture probe hook (see gDiagTransitionCaptureBegin). Called by
 * Transition_SetBackgroundBuffer immediately after it registers the captured
 * background buffer as a native-RGBA16 range, so the SETTIMG probe can report
 * whether that same buffer is byteswapped when the wipe/phased-strips draw reads
 * it. Pure diagnostic bookkeeping: it records an address span, never alters
 * rendering. Pass size 0 to clear the scope. */
extern "C" void gdx_diag_note_transition_capture(void* ptr, size_t size) {
    gDiagTransitionCaptureBegin = reinterpret_cast<uintptr_t>(ptr);
    gDiagTransitionCaptureSize = (ptr != nullptr) ? size : 0;
}

extern "C" void gdx_register_main_module_range(void) {
    uintptr_t moduleBegin = 0;
    uintptr_t moduleEnd = 0;
    GetMainModuleRange(moduleBegin, moduleEnd);
    if ((moduleBegin == 0) || (moduleEnd <= moduleBegin)) {
        return;
    }

    for (const HostRange& range : gHostRanges) {
        if ((range.begin == moduleBegin) && (range.size == (moduleEnd - moduleBegin))) {
            return;
        }
    }

    gHostRanges.push_back({ moduleBegin, moduleEnd - moduleBegin });
    gdx_port_logf("[bridge-init] registered EXE module range: base=%p low32=%08X size=0x%zx\n",
                  reinterpret_cast<void*>(moduleBegin),
                  static_cast<unsigned>(moduleBegin & 0xFFFFFFFFu),
                  static_cast<size_t>(moduleEnd - moduleBegin));
}

extern "C" void* gdx_ensure_asset_segment_for_symbol(unsigned int symLow32, unsigned int* outOffset) {
    uint32_t offset = 0;
    const uintptr_t base = EnsureAssetSegmentForSymbol(symLow32, &offset);
    if (outOffset != nullptr) {
        *outOffset = offset;
    }
    return reinterpret_cast<void*>(base);
}

/* CPU-side analogue of ResolveWideAssetStubPointer (this file, ~:1832), for
 * Gdx_ResolvePortAddress's wide-pointer branch. A wide (>32-bit) pointer inside
 * the EXE module range whose low32 matches an asset-segment map row IS that
 * placeholder symbol's own address: the module spans < 2^32 bytes so low32 is a
 * bijection within it, and every map row's low32 derives from this same
 * module's live symbol address — a match is an identity, never a coincidence.
 * Such a pointer must resolve to the decoded, fixed-up segment image; the stub's
 * own storage is zero BSS by construction and nothing legitimately reads it.
 * Runtime-confirmed victim: Camera_InitViewport's &aVp* reads came back all
 * zero ([vpinit] vscale=0/0 vtrans=0/0, vp inside the module), zeroing
 * camera->currentVp{Scale,Trans}{X,Y} and silently killing the 1ST/2ND/3RD
 * position markers, the rival marker, the ending fireworks and the background
 * stars — four symptoms, one read.
 * The module-range gate runs FIRST (cached statics: GetMainModuleRange scans
 * /proc/self/maps on Linux and must not run per pointer); the binary search
 * only prices pointers that already live in the module. Known Linux caveat,
 * inherited from ResolveWideAssetStubPointer's notes: under PIE the range can
 * under-cover the .bss tail, which degrades to a false NEGATIVE (today's
 * behavior) — the "wide-asset" gdx_addr_log line missing on Linux while
 * present on Windows is the signature to watch for. */
/* A/B kill switch for the wide-asset redirect below. Default ON; GDX_WIDE_ASSET_RESOLVE=0
   restores the pre-fix verbatim wide-pointer return so a regression can be attributed to
   this hook vs. the fixed image base that landed in the same build, without a rebuild. */
extern "C" int gdx_wide_asset_resolve_enabled(void) {
    static int sState = -1;
    if (sState < 0) {
        const char* v = std::getenv("GDX_WIDE_ASSET_RESOLVE");
        sState = (v != nullptr && v[0] == '0' && v[1] == '\0') ? 0 : 1;
    }
    return sState;
}

extern "C" void* gdx_resolve_wide_asset_pointer(const void* full) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(full);
    if (v == 0) {
        return nullptr;
    }
    static uintptr_t sBegin = 0;
    static uintptr_t sEnd = 0;
    static bool sInit = false;
    if (!sInit) {
        sInit = true;
        GetMainModuleRange(sBegin, sEnd);
    }
    if (sBegin == 0 || v < sBegin || v >= sEnd) {
        return nullptr;
    }
    uint32_t offset = 0;
    const uintptr_t base = EnsureAssetSegmentForSymbol(Low32(v), &offset);
    if (base == 0) {
        return nullptr;
    }
    return reinterpret_cast<void*>(base + offset);
}

// Venue texture-bank symbols, indexed by venue id. File scope because two callers must agree on
// this list exactly: the runtime binder below, and gdx_boot_warm_asset_segments, which decodes the
// same banks at boot so the runtime call is a cache hit. A second copy of this table would be a
// place for them to silently diverge.
static const void* const kGdxVenueSegmentSymbols[] = {
    D_A000000_235130, // Mute City
    D_A000000_239A80, // Port Town
    D_A000000_23EC50, // Big Blue
    D_A000000_243D90, // Sand Ocean
    D_A000000_24A270, // Devil's Forest
    D_A000000_2507F0, // White Land
    D_A000000_255100, // Sector
    D_A000000_259600, // Red Canyon
    D_A000000_25F360, // Fire Field
    D_A000000_266C20, // Silence
    D_A000000_26D780, // Ending
};

// Decode the runtime-expensive asset segments at BOOT, so first use during play is a cache hit.
//
// WHY: the asset stalls were attributed by measurement, killing four theories in sequence.
// Preloading the COMPRESSED blobs changed nothing (11/11 warmed in 1.6ms; Cup Select loads
// unmoved). Fixups and command-range registration measured 0.00-0.01ms. The ++gConvertEpoch
// cache invalidation measured flat (xlate 0.12-0.16ms for 8 ticks after each load). And the MIO0
// decode itself -- the last suspect standing, and the one this function was first written to
// avoid -- turned out to cost 2.2ms for ALL TWELVE segments when run here on the host thread.
// The runtime figures (133.95ms for course_track_gfx in one hit, 6-26ms per venue bank at first
// Cup Select entry, each overrunning the 16.68ms tick that carried it) were dominated by
// gdx_yield ROUND-TRIPS: the load window measured yields=2..9, and per-yield cost varies with
// where in the host frame the yield lands (seg 8: 9 yields ~= 134ms). Which call inside
// EnsureAssetSegmentImage yields on the game fiber remains unattributed -- moving the work here
// makes the question moot, since the host context cannot yield at all.
// Decoded images live in gLoadedAssetSegments, which NEVER evicts, so warming here removes those
// stalls for the whole session.
//
// WHY THE SNAPSHOT/RESTORE: EnsureAssetSegmentImage claims gSegments[seg] when the slot is still 0.
// Left alone, this loop would boot the game with segment 0x0A bound to whichever venue decoded
// last -- a binding the game never asked for. Restoring the snapshot returns every slot to its
// pre-warm state; the claim is re-applied harmlessly by the first runtime cache hit (the hit path
// has the same ==0 claim), and the venue binder above re-asserts 0x0A unconditionally anyway.
// The gConvertEpoch bumps this loop causes are left alone: nothing is converted yet at boot, so
// there is nothing to invalidate.
//
// CALLED: from main.cpp, after gdx_rdram_init and the blob preloads, BEFORE bootproc -- host
// thread only, no fibers running, so none of the seqlock/threading constraints in this file apply
// yet. gdx_yield inside the load path is a no-op in host context (__osRunningThread == NULL).
extern "C" void gdx_boot_warm_asset_segments(void) {
    const void* const kWarmSymbols[] = {
        D_8000000, // course_track_gfx (seg 8): the 133.95ms single hit
        kGdxVenueSegmentSymbols[0], kGdxVenueSegmentSymbols[1], kGdxVenueSegmentSymbols[2],
        kGdxVenueSegmentSymbols[3], kGdxVenueSegmentSymbols[4], kGdxVenueSegmentSymbols[5],
        kGdxVenueSegmentSymbols[6], kGdxVenueSegmentSymbols[7], kGdxVenueSegmentSymbols[8],
        kGdxVenueSegmentSymbols[9], kGdxVenueSegmentSymbols[10],
    };

    unsigned long long savedSegments[16];
    static_assert(sizeof(savedSegments) == sizeof(gSegments), "gSegments snapshot size");
    std::memcpy(savedSegments, gSegments, sizeof(savedSegments));

    const auto warmT0 = std::chrono::steady_clock::now();
    int warmed = 0;
    for (const void* sym : kWarmSymbols) {
        uint32_t offset = 0;
        if (EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(sym)), &offset) != 0) {
            ++warmed;
        }
    }

    std::memcpy(gSegments, savedSegments, sizeof(savedSegments));

    /* Pin the venue building-texture range now rather than waiting for the first
       course load: the SETTIMG o2r-delivery exclusion keys on it, and a building
       drawn before gdx_load_venue_building_texture ever ran (course previews)
       would otherwise be served the all-zero archive snapshot. */
    {
        uint32_t offset = 0;
        const uintptr_t base =
            EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(D_8014A20)), &offset);
        if (base != 0) {
            sVenueBuildingTexBase.store(base + offset, std::memory_order_relaxed);
        }
    }

    gdx_port_logf("[boot-warm] decoded %d/%zu asset segments in %.1fms (segments table restored)\n",
                  warmed, std::size(kWarmSymbols),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - warmT0).count());
}

extern "C" int gdx_load_venue_texture_segment(int venue) {
    if (venue < 0 || static_cast<size_t>(venue) >= std::size(kGdxVenueSegmentSymbols)) {
        gdx_port_logf("[segment] invalid venue texture segment %d\n", venue);
        return 0;
    }

    const uint32_t symbol = Low32(reinterpret_cast<uintptr_t>(kGdxVenueSegmentSymbols[venue]));
    uint32_t offset = 0;
    // [venueload] Attribute the Cup Select stall. This path is invisible to the existing
    // [transition] timers, which only bracket mode-change ticks -- and Cup Select's course preview
    // loading is NOT a mode change. It runs one course per game tick from
    // course_model.c:35-39, so the ~350ms the owner sees is really ~6 consecutive ticks.
    //
    // Two candidate causes, and they need opposite fixes, which is why this measures rather than
    // assumes: either the LOAD itself is expensive (archive read + MIO0 decode + fixups), in which
    // case preloading the blobs at boot removes it; or the load is cheap and the cost is the
    // ++gConvertEpoch below invalidating every cached display-list conversion, in which case
    // preloading changes NOTHING and the fix is scoped epoch invalidation. The epoch counter is
    // logged alongside so the following ticks' xlate can be correlated.
    const double gdxVenueT0 = (gGdxInterpNowFn != nullptr) ? gGdxInterpNowFn() : 0.0;
    const uint32_t gdxEpochBefore = gConvertEpoch;
    const uintptr_t base = EnsureAssetSegmentForSymbol(symbol, &offset);
    if (base == 0) {
        gdx_port_logf("[segment] failed to load venue=%d symbol=%08X\n", venue, symbol);
        return 0;
    }
    if (gGdxInterpNowFn != nullptr) {
        gdx_port_logf("[venueload] venue=%d symbol=%08X load=%.2fms epoch %u->%u\n", venue, symbol,
                      (gGdxInterpNowFn() - gdxVenueT0) * 1000.0, (unsigned) gdxEpochBefore,
                      (unsigned) gConvertEpoch);
        gGdxVenueWatchTicks = 8; // log the next 8 ticks' translation cost (see gdx_gfx_run)
    }

    // Force segment 0x0A to point at the (decompressed) venue texture image.
    // EnsureAssetSegmentImage only claims a segment slot when it is still 0, but
    // the game sets segment 0x0A via gsSPSegment before this loads, so the slot
    // was already non-zero and kept a stale/raw pointer — the track then sampled
    // raw ROM/compressed bytes (the "stripes"). This loader is the authority for
    // the venue texture segment, so it still re-asserts unconditionally whenever
    // the value actually changes (e.g. the game's own DL rewrote it via moveword).
    // Bracket the write with the seqlock epoch: an unbracketed store here was
    // invisible to the graphics-thread's GdxSegmentEpochStable() guard, so a
    // racing translate could observe a torn/stale base with no skip counted.
    // gdx_load_venue_texture_segment is game-thread-only (called from
    // decomp_port.c Segment_LoadAssets), which is a requirement for calling
    // gdx_segment_epoch_begin/end -- never call these from the graphics thread.
    if (gSegments[0x0A] != base) {
        gdx_segment_epoch_begin();
        gSegments[0x0A] = base;
        gdx_segment_epoch_end();
    }

    // gGdxRaceActive is set by the caller for race modes (decomp_port.c
    // Segment_LoadAssets) — this loader also runs for the course-select
    // preview now, and menus must not flip the race-diagnostics gate.

    // Segment_LoadAssets calls this every frame; log only on change so Debug
    // builds don't pay a file write + flush per frame.
    {
        static int sLastVenue = -1;
        static uintptr_t sLastBase = 0;
        if (venue != sLastVenue || base != sLastBase) {
            sLastVenue = venue;
            sLastBase = base;
            gdx_port_logf("[segment] loaded venue=%d segment=10 base=%p symbol=%08X offset=%08X\n",
                          venue, reinterpret_cast<void*>(base), symbol, offset);
        }
    }
    return 1;
}

/* PORT replacement for func_800747EC's building-texture DMA (course_gadgets.c).
 * On console that DMA lands a per-venue 0x800 slice of super_textures on top of
 * course_track_gfx at D_8014A20; under PORT the destination
 * Segment_SegmentedToVirtual(D_8014A20) masks a 64-bit host pointer to 24 bits
 * and the copy lands nowhere, so every venue drew the ROM-baked scratch bytes
 * (the "white buildings"). Write into the decoded segment-8 image instead --
 * that is the storage the display-list resolver actually reads for D_8014A20.
 * A verbatim ROM copy is correct here: no sAssetFixups row overlaps
 * [0x14A20, 0x15220) (the last segment-8 row ends exactly at 0x14A20), and the
 * decoded image stores N64-order bytes just like the ROM.
 * Game-thread only (course load), same context as the venue segment loader
 * above. Cache refresh on venue change is handled by the compare path in
 * MakePersistentRawTextureCopy via the IsVenueBuildingTextureRange carve-out,
 * keyed off the base recorded here. */
extern "C" void gdx_load_venue_building_texture(unsigned int romOffset) {
    uint32_t offset = 0;
    const uintptr_t base =
        EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(D_8014A20)), &offset);
    if (base == 0) {
        gdx_port_logf("[segment] building texture: D_8014A20 did not resolve\n");
        return;
    }

    /* Same address strip as Dma_PortRomOffset (decomp/src/sys/dma.c), so this
       reads the exact bytes the console DMA would have. */
    const uint32_t romPhys = romOffset & 0x1FFFFFFFu;
    const uint32_t romBase = (romPhys >= 0x10000000u) ? (romPhys - 0x10000000u) : romPhys;

    uint8_t staged[0x800];
    if (!GdxSegmentSourceRead(romBase, static_cast<uint32_t>(sizeof(staged)), staged)) {
        gdx_port_logf("[segment] building texture: ROM read failed at %08X\n", romBase);
        return;
    }

    uint8_t* const dst = reinterpret_cast<uint8_t*>(base + offset);
    sVenueBuildingTexBase.store(base + offset, std::memory_order_relaxed);
    if (std::memcmp(dst, staged, sizeof(staged)) != 0) {
        std::memcpy(dst, staged, sizeof(staged));
        gdx_port_logf("[segment] building texture: wrote venue slice rom=%08X -> seg8+%05X\n",
                      romOffset, offset);
    }
}

/* Writable extent of the registered host range containing `host`, or 0 when the pointer is not
   inside any registered range. Exposed to decomp TUs because Dma_RomCopy resolves a 32-bit N64
   pointer to a host address and then writes `size` bytes into it, and nothing in that path
   otherwise knows how large the destination actually is. */
extern "C" size_t gdx_registered_host_capacity(const void* host) {
    return RegisteredHostRemaining(reinterpret_cast<uintptr_t>(host));
}

#ifdef _WIN32
/* Name a Microsoft C++ exception (code 0xE06D7363) from its raw EXCEPTION_RECORD, for the
 * crash handler in n64_sched.c (a C TU that cannot touch C++ RTTI). Two crashes on 2026-08-07
 * logged only "code=0xE06D7363 pc=<KERNELBASE!RaiseException>" -- the throw site and the message
 * were both invisible, which made the reports undiagnosable. The record's parameters are the
 * MSVC throw ABI: [0] magic, [1] exception object, [2] ThrowInfo, [3] module base (x64). The
 * ThrowInfo walk reads the mangled type name; if it names a std::exception lineage, what() is
 * called through the object's real vtable. Every dereference is SEH-guarded: this runs while the
 * process is already dying, so a garbage record must degrade to "no detail", never to a second
 * fault escaping the handler. Buffers are static because the caller writes them straight into
 * the crash report; the handler is serialized by its own sInHandler latch. */
extern "C" const char* gdx_cxx_exception_name(const EXCEPTION_RECORD* rec) {
    static char sName[192];
    if (rec == NULL || rec->ExceptionCode != 0xE06D7363u || rec->NumberParameters < 4) {
        return NULL;
    }
    __try {
        const uint8_t* imageBase = reinterpret_cast<const uint8_t*>(rec->ExceptionInformation[3]);
        const uint32_t* throwInfo = reinterpret_cast<const uint32_t*>(rec->ExceptionInformation[2]);
        if (imageBase == NULL || throwInfo == NULL) {
            return NULL;
        }
        // ThrowInfo: {attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray} -- RVAs on x64.
        const uint32_t* cta = reinterpret_cast<const uint32_t*>(imageBase + throwInfo[3]);
        if (cta[0] < 1) { // {nCatchableTypes, rva[]}
            return NULL;
        }
        // CatchableType: {properties, pType(TypeDescriptor RVA), ...}.
        const uint32_t* ct = reinterpret_cast<const uint32_t*>(imageBase + cta[1]);
        // TypeDescriptor: {vftable, spare, char name[]} -- name like ".?AVexception@std@@".
        const char* name = reinterpret_cast<const char*>(imageBase + ct[1]) + 2 * sizeof(void*);
        size_t i = 0;
        for (; i < sizeof(sName) - 1 && name[i] != '\0'; ++i) {
            sName[i] = name[i];
        }
        sName[i] = '\0';
        return sName;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

extern "C" const char* gdx_cxx_exception_what(const EXCEPTION_RECORD* rec) {
    static char sWhat[256];
    if (rec == NULL || rec->ExceptionCode != 0xE06D7363u || rec->NumberParameters < 2) {
        return NULL;
    }
    __try {
        const std::exception* e =
            reinterpret_cast<const std::exception*>(rec->ExceptionInformation[1]);
        if (e == NULL) {
            return NULL;
        }
        const char* w = e->what(); // virtual call through the live object's vtable
        if (w == NULL) {
            return NULL;
        }
        size_t i = 0;
        for (; i < sizeof(sWhat) - 1 && w[i] != '\0'; ++i) {
            sWhat[i] = w[i];
        }
        sWhat[i] = '\0';
        return sWhat;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}
#endif

/* Report a DMA whose destination is too small for the requested transfer. Rate-limited: a load
   loop that trips this once normally trips it on every block, and Dma_LoadAssets issues one call
   per 1 KB. */
extern "C" void gdx_dma_report_short_dest(const void* dst, unsigned int size, size_t capacity,
                                          unsigned int romOffset) {
    static int sReports = 0;
    if (sReports >= 16) {
        return;
    }
    ++sReports;
    gdx_port_logf("[dma] REFUSED copy: dst=%p needs %u bytes, only %zu writable (romOffset=%08X) "
                  "-- this would have written past the end of the destination buffer\n",
                  dst, size, capacity, romOffset);
}

extern "C" void* gdx_resolve_registered_host_address(unsigned int addr) {
    /* Two candidates per lookup: the raw value, then the value with bit 31 restored.
       Decomp code routinely converts pointers with the KSEG0->physical idiom
       (osVirtualToPhysical / K0_TO_PHYS strips bit 31) before storing them in
       32-bit fields (audio acmd lists, DMA descriptors). On Windows the module
       and heap sit below 2 GB so low32 never has bit 31 set and the strip is a
       no-op; on Linux PIE/mmap the low32 of a host pointer frequently has bit 31
       set, and the stripped form matched nothing — the audio HLE's
       LOADBUFF/SAVEBUFF ops resolved NULL and were skipped, producing the
       all-zero (silent) sample output. The exact-match pass always runs first,
       so this cannot shadow a legitimate raw match. */
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t candidate = (pass == 0) ? addr : (addr | 0x80000000u);
        if (pass == 1 && candidate == addr) {
            break; /* bit 31 already set: nothing new to try */
        }
        for (const HostRange& range : gHostRanges) {
            if ((range.begin == 0) || (range.size == 0)) {
                continue;
            }

            const uint32_t baseLow = Low32(range.begin);
            const uint32_t offset = candidate - baseLow;
            if (offset < range.size) {
                static int sRegisteredResolveLogs = 0;
                if (sRegisteredResolveLogs < 8) {
                    ++sRegisteredResolveLogs;
                    gdx_port_logf("[registered-resolve] raw=%08X cand=%08X base=%p baseLow=%08X size=0x%zx -> %p\n",
                                  addr, candidate, reinterpret_cast<void*>(range.begin), baseLow, range.size,
                                  reinterpret_cast<void*>(range.begin + offset));
                }
                return reinterpret_cast<void*>(range.begin + offset);
            }
        }
    }
    return nullptr;
}

extern "C" void* gdx_resolve_module_host_address(unsigned int addr) {
    uintptr_t moduleBegin = 0;
    uintptr_t moduleEnd = 0;
    GetMainModuleRange(moduleBegin, moduleEnd);
    if ((moduleBegin == 0) || (moduleEnd <= moduleBegin)) {
        return nullptr;
    }

    /* Same two-candidate rule as gdx_resolve_registered_host_address above: the raw
       low32, then low32 with bit 31 restored (KSEG0->physical stripping — see the
       comment there). On Linux PIE the module's BSS low32 range regularly crosses
       0x80000000, so stripped pointers reconstructed below moduleBegin and the +4GB
       correction overshot moduleEnd -> NULL (silent audio: skipped LOADBUFF/SAVEBUFF). */
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t candidate = (pass == 0) ? addr : (addr | 0x80000000u);
        if (pass == 1 && candidate == addr) {
            break;
        }

        uintptr_t full = (moduleBegin & 0xFFFFFFFF00000000ULL) | static_cast<uintptr_t>(candidate);
        if (full < moduleBegin) {
            full += 0x100000000ULL;
        }

        /* Match the bridge's module reconstruction rule: linker/BSS segment symbols can
           point at section boundaries that are not themselves readable, while offsets
           from that base can still land on valid display-list/data bytes. */
        if ((full >= moduleBegin) && (full < moduleEnd)) {
            static int sModuleResolveLogs = 0;
            if (sModuleResolveLogs < 8) {
                ++sModuleResolveLogs;
                gdx_port_logf("[module-resolve] raw=%08X cand=%08X -> %p module=[%p,%p)\n",
                              addr, candidate, reinterpret_cast<void*>(full),
                              reinterpret_cast<void*>(moduleBegin), reinterpret_cast<void*>(moduleEnd));
            }
            return reinterpret_cast<void*>(full);
        }
    }
    return nullptr;
}

// gFrameMirrorFb / gFrameMirrorValid are declared near the top of this file
// (just after the gGameMode extern) so gdx_vi_present_fallback can also
// write them; see the comment there.

// ===== Host API (see port/n64_gfx_bridge.h) — thin accessors over the module state above. =====
extern "C" void gdx_gfx_interp_set_now_fn(GdxInterpNowFn fn) {
    gGdxInterpNowFn = fn;
}

extern "C" int gdx_gfx_interp_host_active(void) {
    return gdx_interp::P2HostActive() ? 1 : 0;
}

// port/gdx_frame_pacer.c used to read the RAW FrameInterpolation CVar to
// decide "am I mutually excluded this tick", but main.cpp's per-tick interpOn additionally forces
// the classic single-present path off (interpOn = host_active && !interpEditorActive) while an EK
// editor (Course Edit / Create Machine) is active. With BOTH FrameInterpolation and FramePacing on,
// an editor tick took the classic branch (which DOES call gdx_frame_pacer_tick()) but the raw CVar
// was still 1, so the pacer self-unarmed and neither pacing mechanism ran for that tick (free-run).
// This accessor exposes the per-tick truth main.cpp already committed via
// gdx_gfx_interp_tick_config (gGdxInterpHostCfg.active) instead of the always-on raw CVar, so the
// pacer's mutual-exclusion check reflects what THIS tick actually did, not the menu toggle.
extern "C" int gdx_gfx_interp_tick_active(void) {
    return gGdxInterpHostCfg.active ? 1 : 0;
}

extern "C" void gdx_gfx_interp_tick_config(int active, double tickStart, double tickDuration,
                                           int maxSubframes) {
    gGdxInterpHostCfg.active = (active != 0);
    gGdxInterpHostCfg.tickStart = tickStart;
    gGdxInterpHostCfg.tickDuration = tickDuration;
    gGdxInterpHostCfg.maxSubframes = (maxSubframes > 0) ? maxSubframes : 1;
    // Reset per tick; gdx_gfx_run sets it back to true only if it actually presents (a gfx task
    // ran AND p2Host held). On a taskless tick it stays false and the host presents once.
    gGdxInterpPresentedLastTick = false;
    // THE tick boundary. This is the only place in the bridge that runs exactly once per 60 Hz
    // logic tick (the host calls it per iteration, before dispatch), so it is where the
    // referenced-offset set is armed to roll. gdx_gfx_run cannot do it: it runs per GFX task.
    gGdxInterpNewTick = true;
    gGdxInterpLastTasks = gGdxInterpTasksThisTick;
    gGdxInterpTasksThisTick = 0;
}

// Tasks (gdx_gfx_run calls) the previous tick submitted. Surfaced so the [interp-p2] line can show
// the number the per-task/per-tick distinction turns on, instead of it being folklore.
extern "C" int gdx_gfx_interp_last_tasks(void) {
    return gGdxInterpLastTasks;
}

extern "C" int gdx_gfx_interp_presented_last_tick(void) {
    return gGdxInterpPresentedLastTick ? 1 : 0;
}

extern "C" int gdx_gfx_interp_last_subframes(void) {
    return gGdxInterpLastSubframes;
}

extern "C" double gdx_gfx_interp_last_t(void) {
    return gGdxInterpLastT;
}

// Real-FPS visibility. Declared locally (extern "C") by gdx_menu.cpp's Stats
// page and by the FPS overlay — same minimal-include idiom as gdx_gfx_interp_last_subframes above,
// so no n64_gfx_bridge.h change is needed. presents_per_sec is a rolling ~0.5 s meter of true
// sub-frame presents; last_lerped/last_snapped are the previous tick's per-slot tween/snap counts.
extern "C" double gdx_gfx_interp_presents_per_sec(void) {
    return gGdxInterpPresentsPerSec;
}

/* Sim-schedule slip exchange between the host loop and the sub-frame burst.
 *
 * Slip is how far the tick finished PAST its running logic deadline (main.cpp
 * sNextLogicDeadline), sampled after gdx_host_pace_logic_until: a healthy tick
 * ends at (or sleeping until) the deadline and publishes ~0; a tick that could
 * not afford its work publishes the shortfall. Only the host loop can measure
 * this -- the deadline schedule lives there -- so it owns the sample and
 * publishes it here for the burst pre-sizer (the AIMD controller) to steer on.
 * See the controller's block comment in gdx_gfx_run for why slip, not per-pass
 * cost, is the steering signal. */
static double gGdxInterpSimSlipSec = 0.0;

extern "C" void gdx_gfx_interp_set_sim_slip(double seconds) {
    // A breakpoint / alt-tab stall can make one sample arbitrarily large; the running schedule
    // re-anchors past 4 ticks of stall anyway, so cap what one sample can claim. Negative means
    // the pacer slept to the deadline -- schedule healthy, slip zero.
    if (seconds < 0.0) {
        gGdxInterpSimSlipSec = 0.0;
    } else if (seconds < 1.0) {
        gGdxInterpSimSlipSec = seconds;
    }
}
/* [interp-pair] Pairing-quality readout. Largest prev->cur translation delta among slots that
   actually PAIRED this tick, and how many of those exceeded a plausible per-tick motion. See
   gdx_interp.h TranslationDelta: byte-offset slot identity can pair two different objects when the
   pool layout shifts, and the 2000-unit teleport guard is far too coarse to notice. Latched into
   file globals at the same site as the other P1 counters, since the adapter is tick-scoped. */
extern "C" float gdx_gfx_interp_pair_max_delta(void) {
    // Read-and-reset: each printed value is the worst pairing seen since the previous line, not an
    // all-time high that would saturate on the first bad tick and never move again.
    const float v = gGdxInterpPairMaxDelta;
    gGdxInterpPairMaxDelta = 0.0f;
    return v;
}

extern "C" int gdx_gfx_interp_pair_suspect(void) {
    return static_cast<int>(gGdxInterpPairSuspect);
}

extern "C" int gdx_gfx_interp_idem_divergent(void) {
    return static_cast<int>(gGdxIdemDivergentTicks);
}

extern "C" int gdx_gfx_interp_idem_multipass(void) {
    return static_cast<int>(gGdxIdemMultiPassTicks);
}

extern "C" int gdx_gfx_interp_pair_lerped_total(void) {
    return static_cast<int>(gGdxInterpPairLerped);
}

extern "C" int gdx_gfx_interp_last_lerped(void) {
    return static_cast<int>(gGdxInterpLastLerped);
}
extern "C" int gdx_gfx_interp_last_snapped(void) {
    return static_cast<int>(gGdxInterpLastSnapped);
}
extern "C" int gdx_gfx_interp_last_dropped(void) {
    return gGdxInterpLastDropped;
}

// Tier 2/3 coverage counters: viewport and effects-vertex batches classified this tick. Read by
// main.cpp's [interp-p2] line. The lerped/snapped split is the acceptance instrument for the
// effects-vertex work in particular: its byte-offset identity churns far harder than the matrices'
// (batch sizes flip 4<->5 with boost state, spawns shift every downstream offset), and a snapped
// batch renders exactly like the pre-Tier-3 build — so a high snap ratio means "shipped but inert",
// which must be visible in one glance at the log, not discovered by eyeballing flames.
// Per-racer matrices that spawned this tick and borrowed a keyframe from their racer's body
// (GdxFixupSpawnedRacerMatrices). Non-zero during side/spin attacks is the fix working.
static size_t gGdxInterpLastBorrowed = 0;
extern "C" int gdx_gfx_interp_last_borrowed(void) {
    return static_cast<int>(gGdxInterpLastBorrowed);
}
// Camera projection*view rebuilds (task #14). rebuilt = sub-frame matrices produced from an
// interpolated pose; rejected = camera slots whose t=1 identity check failed this tick and fell back
// to the element-wise lerp. A steady rejected>0 means the rebuild is shipped but inert for that
// slot, which must be one glance in the log rather than a guess from how the game looks.
// eyedelta is the largest per-tick |eye_cur - eye_prev| among verified pairs: the measurement a
// cut-detection threshold would have to be derived from. No such threshold is applied yet -- the
// existing cut/pause/absent-keyframe snap rules already disqualify the slot, and picking a number
// before measuring one is how the last camera hypothesis went wrong.
// Per-racer matrices whose previous keyframe had its rotation frozen because the racer crossed a
// side-attack model-basis discontinuity this tick (GdxFixupBasisJumpMatrices). Expect brief
// non-zero bursts twice per side attack and NEVER during a spin attack -- a non-zero reading
// during a spin would mean the predicate is wrong, not that the fix is working harder.
static size_t gGdxInterpLastBasisFixed = 0;
extern "C" int gdx_gfx_interp_last_basis_fixed(void) {
    return static_cast<int>(gGdxInterpLastBasisFixed);
}
static size_t gGdxInterpLastCamRebuilt = 0;
static size_t gGdxInterpLastCamRejected = 0;
static float gGdxInterpLastCamEyeDelta = 0.0f;
extern "C" int gdx_gfx_interp_last_cam_rebuilt(void) {
    return static_cast<int>(gGdxInterpLastCamRebuilt);
}
extern "C" int gdx_gfx_interp_last_cam_rejected(void) {
    return static_cast<int>(gGdxInterpLastCamRejected);
}
extern "C" double gdx_gfx_interp_last_cam_eye_delta(void) {
    return static_cast<double>(gGdxInterpLastCamEyeDelta);
}
// Why the camera rebuild did not run, per tick. Order matches GdxCamWhy:
// poseread, id, unreadable, build, mismatch, prevpose, snap.
static size_t gGdxInterpLastCamWhy[kCamWhyCount] = {};
extern "C" int gdx_gfx_interp_last_cam_why(int which) {
    if (which < 0 || which >= static_cast<int>(kCamWhyCount)) {
        return -1;
    }
    return static_cast<int>(gGdxInterpLastCamWhy[which]);
}
static size_t gGdxInterpLastVpLerped = 0;
static size_t gGdxInterpLastVpSnapped = 0;
static size_t gGdxInterpLastVtxLerped = 0;
static size_t gGdxInterpLastVtxSnapped = 0;
extern "C" int gdx_gfx_interp_last_vp_lerped(void) {
    return static_cast<int>(gGdxInterpLastVpLerped);
}
extern "C" int gdx_gfx_interp_last_vp_snapped(void) {
    return static_cast<int>(gGdxInterpLastVpSnapped);
}
extern "C" int gdx_gfx_interp_last_vtx_lerped(void) {
    return static_cast<int>(gGdxInterpLastVtxLerped);
}
extern "C" int gdx_gfx_interp_last_vtx_snapped(void) {
    return static_cast<int>(gGdxInterpLastVtxSnapped);
}

// P4 determinism gate: per-tick logic-state fingerprint. Called ONCE per rendered tick from
// gdx_gfx_run, on BOTH the interpolation-ON and interpolation-OFF paths (gdx_gfx_run is reached
// identically either way, and the tick counter advances only on ticks that produce a gfx task --
// the same ticks on both paths, so the sequences stay index-aligned). It reads ONLY game-logic RNG
// state that the render path never touches (interpolation reads GfxPools and writes only scratch
// -- the prime directive), so with identical input the fingerprint sequence is byte-identical ON
// vs OFF, and the FIRST divergent tick localizes any leak of a sub-frame value back into logic.
// No-op unless GDX_INTERP_DETERMINISM is set (parsed once).
//
// To use it: record a ghost with FrameInterpolation OFF, replay it twice with
// GDX_INTERP_DETERMINISM=1 (once interp OFF, once ON), and diff the [interp-determinism] lines.
// Byte-identical tick-for-tick means interpolation is provably render-only. The RNG fingerprint is
// only the canary -- the real gate is the ghost byte-streams and finishing times themselves; this
// makes a divergence cheap to localize.
static void GdxInterpDeterminismTick() {
    if (!gdx_dev_gate(GDX_GATE_INTERP_DETERMINISM)) {
        return;
    }
    static uint64_t sTick = 0;
    const uint32_t words[4] = {
        static_cast<uint32_t>(gRandSeed1), gRandMask1,
        static_cast<uint32_t>(gRandSeed2), gRandMask2,
    };
    uint64_t h = 0xCBF29CE484222325ull; // FNV-1a
    for (uint32_t w : words) {
        h ^= w;
        h *= 0x100000001B3ull;
    }
    gdx_port_logf("[interp-determinism] tick=%llu rng=%08X/%08X/%08X/%08X hash=%016llX\n",
                  static_cast<unsigned long long>(sTick++),
                  static_cast<unsigned>(words[0]), static_cast<unsigned>(words[1]),
                  static_cast<unsigned>(words[2]), static_cast<unsigned>(words[3]),
                  static_cast<unsigned long long>(h));
}

extern "C" void gdx_gfx_run(void* dl, size_t dl_size, GdxTaskUcode taskUcode) {
    // Time the WHOLE bridge call, so the perf summary's "logic" figure stops absorbing work that
    // is not game logic. logic is derived as gametick - (xlate + run + mirror), and only ConvertRoot
    // was ever timed, so every other thing this function does was being reported as decomp time.
    //
    // Scope guard rather than a begin/end pair: this function has several early returns (no window,
    // no interpreter, bad display list), and a leaked open timer would silently corrupt every
    // subsequent sample rather than fail loudly.
    // The POST half is opened after the sub-frame burst, but this function has several exits after
    // that point, so the guard closes it on whichever one is taken rather than requiring every
    // return site to remember.
    bool gdxPostTimerOpen = false;
    struct GdxGfxRunTimer {
        bool* postOpen;
        explicit GdxGfxRunTimer(bool* p) : postOpen(p) {
            gdx_perf_sub_begin(GDX_PERF_SUB_GFXRUN);
        }
        ~GdxGfxRunTimer() {
            if (*postOpen) {
                gdx_perf_sub_end(GDX_PERF_SUB_POST);
            }
            gdx_perf_sub_end(GDX_PERF_SUB_GFXRUN);
        }
    } gdxGfxRunTimer(&gdxPostTimerOpen);
    gdx_perf_sub_begin(GDX_PERF_SUB_SETUP);

    // The Fast3dWindow is created once at startup and lives for the whole
    // program. Fetch it once and cache the raw pointer instead of copying the
    // Context's window shared_ptr every frame: that per-frame refcount touch
    // crashed in _Ptr_base<Window>::_Incref when the Context's window member
    // was transiently unreadable during rapid mode transitions (e.g. machine
    // select -> settings). The cache is populated at startup when state is
    // clean, so later frames never re-read that member.
    static Fast::Fast3dWindow* sCachedWindow = nullptr;
    if (sCachedWindow == nullptr) {
        auto ctx = Ship::Context::GetInstance();
        if (ctx == nullptr) { return; }
        auto wnd = ctx->GetWindow();
        sCachedWindow = static_cast<Fast::Fast3dWindow*>(wnd.get());
    }
    Fast::Fast3dWindow* fw = sCachedWindow;
    if (fw == nullptr) { return; }

    // Advance the wide-conversion cache's frame counter once
    // per real GFX task and let it sweep stale entries when it has grown past
    // its watermark (see GfxWideCache::BeginFrame). gWideCache stays self-
    // contained (no logging dependency of its own, so it still builds/unit-
    // tests standalone); the bridge does the one-line log here instead.
    {
        const size_t evicted = gWideCache.BeginFrame();
        if (evicted != 0) {
            gdx_port_logf("[g2] evicted %zu stale conversions\n", evicted);
        }
    }

    // Register the boot-logo seed hook ALWAYS and gate the
    // behavior per call inside SeedBootLogoAfterClear (gSeedBootLogoEnabled).
    // The env state is logged UNCONDITIONALLY so a soak log always shows what
    // the process actually saw. GetEnvironmentVariableA is used instead of
    // std::getenv: getenv reads the CRT's startup snapshot of the environment,
    // which can miss variables in edge cases (env changed after CRT init, or a
    // launcher passing a custom environment block); the Win32 call reads the
    // live process environment directly.
    static bool sSeedHookChecked = false;
    if (!sSeedHookChecked) {
        sSeedHookChecked = true;
        char seedValue[32] = { 0 };
        bool seedPresent = false;
#ifdef _WIN32
        const DWORD seedLen =
            GetEnvironmentVariableA("GDX_SEED_BOOT_LOGO", seedValue, sizeof(seedValue));
        seedPresent = (seedLen > 0 && seedLen < sizeof(seedValue));
#else
        if (const char* seedEnv = std::getenv("GDX_SEED_BOOT_LOGO")) {
            std::snprintf(seedValue, sizeof(seedValue), "%s", seedEnv);
            seedPresent = true;
        }
#endif
        gSeedBootLogoEnabled = seedPresent && seedValue[0] != '0';
        // Shell-proof fallback: the env var route failed silently in user soak
        // runs (PowerShell `set` does not export; double-click launches carry
        // no shell env at all). A command-line switch survives every launch
        // method.
        bool seedFromArg = false;
#ifdef _WIN32
        {
            const char* cmd = GetCommandLineA();
            if (cmd != nullptr) {
                if (!gSeedBootLogoEnabled && std::strstr(cmd, "--seed-boot-logo") != nullptr) {
                    gSeedBootLogoEnabled = true;
                    seedFromArg = true;
                }
                // Forward the TEXEL1 A/B bisect switch to the interpreter's
                // getenv probe (interpreter.cpp, GDX_DIAG_TEXEL1_FROM_BASE).
                // This runs before the interpreter's first material import,
                // so the CRT env update is seen by its one-time static check.
                if (std::strstr(cmd, "--diag-texel1-base") != nullptr) {
                    _putenv("GDX_DIAG_TEXEL1_FROM_BASE=1");
                    gdx_port_logf("[seed] --diag-texel1-base: TEXEL1 forced to base tile for this run\n");
                }
                // Forward the SETTIMG race-trace probe (classifies + fingerprints
                // every resolved texture source during a race into
                // settimg-trace.txt) so it works from any launch method.
                if (std::strstr(cmd, "--diag-settimg") != nullptr) {
                    // The _putenv stays until interpreter.cpp is migrated off getenv (its probe
                    // still samples the CRT environment); gdx_dev_gate_force arms the same probe
                    // on the bridge side, which has already sampled the environment by now.
                    _putenv("GDX_DIAG_SETTIMG=1");
                    gdx_dev_gate_force(GDX_GATE_DIAG_SETTIMG, 1);
                    gdx_port_logf("[seed] --diag-settimg: SETTIMG race trace enabled\n");
                }
            }
        }
#endif
        gdx_port_logf("[seed] GDX_SEED_BOOT_LOGO=%s%s (seeding %s)\n",
                      seedPresent ? seedValue : "<unset>",
                      seedFromArg ? " arg=--seed-boot-logo" : "",
                      gSeedBootLogoEnabled ? "ENABLED" : "disabled");
        Fast::Interpreter::SetPortAfterClearHook(&SeedBootLogoAfterClear);
    }

    // All three task variants share F3DEX2 command encoding. Their semantic
    // differences are carried separately so the base opcode table stays valid.
    fw->SetRendererUCode(ucode_f3dex2);
    auto interp = fw->GetInterpreterWeak().lock();
    if (!interp) { return; }
    // The task ucode is this display list's ENTRY state, not a property of the whole frame, so it
    // is kept in a local and re-armed before every sub-frame replay (see the pass loop below).
    // A mid-list G_LOAD_UCODE variant-switch marker mutates mF3dex2Variant during the walk
    // (interpreter.cpp:7159) and SpReset does not restore it. Set once per task, pass 0 therefore
    // walks the list from the task variant while every replay walks it from whatever the previous
    // pass ended on -- and under Reject/FZeroFlxReject that arms the 2x-viewport reject box
    // (interpreter.cpp:3007) over triangles pass 0 rendered normally. Measured: ~39 extra clip
    // rejections per task with bit-identical vertex and matrix hashes, converging after the first
    // replay because the list's end state is a fixed point. Visible as the floor and clouds
    // dropping out on replays while pass 0 stays correct.
    Fast::F3dex2Variant gdxTaskVariant = Fast::F3dex2Variant::Standard;
    switch (taskUcode) {
        case GDX_TASK_UCODE_F3DLX2_REJ:
            gdxTaskVariant = Fast::F3dex2Variant::Reject;
            break;
        case GDX_TASK_UCODE_F3DFLX2_REJ:
            gdxTaskVariant = Fast::F3dex2Variant::FZeroFlxReject;
            break;
        case GDX_TASK_UCODE_F3DEX2:
        default:
            gdxTaskVariant = Fast::F3dex2Variant::Standard;
            break;
    }
    interp->SetF3dex2Variant(gdxTaskVariant);

    for (int i = 0; i < 16; i++) {
        interp->mSegmentPointers[i] = gSegments[i];
    }

    EnsureSetupGfxSegment();
    EnsureAssetSegmentForSymbol(Low32(reinterpret_cast<uintptr_t>(aVpFullScreen)));
    for (int i = 0; i < 16; i++) {
        interp->mSegmentPointers[i] = gSegments[i];
    }

    bool isBigEndian = IsLikelyBigEndianDisplayList(static_cast<const N64Gfx*>(dl), dl_size / sizeof(N64Gfx));

    // Sub-phase: per-command DL translation (the adapter's ConvertRoot walk). See gdx_perf.h.
#ifdef _WIN32
    ResetWindowsMemoryRegionCache();
#endif
    gdx_perf_sub_end(GDX_PERF_SUB_SETUP);
    const double gdxXlateT0 = (gGdxVenueWatchTicks > 0 && gGdxInterpNowFn != nullptr) ? gGdxInterpNowFn() : 0.0;
    gdx_perf_sub_begin(GDX_PERF_SUB_XLATE);
    ConversionStats stats = {};
    N64DisplayListAdapter adapter(dl, dl_size, isBigEndian, &stats);
    // Latch the current/previous GfxPool bases and reset the referenced-offset set BEFORE
    // ConvertRoot drains the G_MTX reroutes (which populate this tick's lerp list). No-op unless P1.
    adapter.GdxInterpBeginTick();
    Fast::F3DGfx* converted = adapter.ConvertRoot();
    // The referenced-offset set is NOT promoted here. This function runs once per GFX TASK and the
    // game submits several per tick, so the promotion happens at the real tick boundary inside
    // GdxInterpBeginTick (see gGdxInterpNewTick) where a complete tick's set is available.
    // Emit this tick's determinism fingerprint (no-op unless GDX_INTERP_DETERMINISM set).
    // Placed on the common path so it runs identically whether interpolation is ON or OFF this tick.
    // Repair spawned per-racer matrices BEFORE any refill runs, so the first sub-frame of an attack
    // already tweens in lockstep with its machine. Must follow ConvertRoot (every matrix decision
    // for the tick must exist) and precede GdxP0RefillScratch.
    adapter.GdxFixupSpawnedRacerMatrices();
    // Must follow the spawn fixup: a highlight matrix that spawned THIS tick has just been given a
    // synthetic prev, and if its racer also crossed a basis discontinuity that prev needs the same
    // rotation freeze. Running before would leave those slots unpaired and skip them.
    // Measure BEFORE the fixup rewrites r.prev, otherwise the probe reports the repaired pair and
    // the discontinuity it exists to size becomes invisible.
    adapter.GdxBasisProbeTick();
    adapter.GdxFixupBasisJumpMatrices();
    GdxInterpDeterminismTick();
    // [attack-hl] Falsifiable emit: log ONLY the disagreement this probe exists to find -- a racer
    // whose attack-highlight matrix snapped while its own body matrix lerped (or vice versa). Both
    // draw the same machine at the same pose, so any disagreement puts them at different instants
    // on screen. Silence across a run of side attacks refutes the hypothesis and sends the hunt
    // back to the content/flicker-blend class; lines here confirm it and name the racer.
    // Unconditional but self-limiting (24 lines a session): a disagreement is rare by construction,
    // so there is nothing to gate against, and a gate would just be one more thing to forget to arm.
    {
        // POSITIVE CONTROL FIRST. A probe that reports zero without proving its subject occurred is
        // worthless -- the first run of this probe returned a confident zero from a script that
        // never reached a race. So count the ticks on which ANY highlight matrix was referenced at
        // all: that number is the attack-tick count, and it must be non-zero before a zero
        // disagreement count means anything.
        static int sAttackHlLogs = 0;
        static uint32_t sHighlightTicks = 0;
        static uint32_t sDisagreeTicks = 0;
        bool anyHighlight = false;
        for (uint32_t r = 0; r < 30; ++r) {
            const uint8_t body = adapter.GdxRacerMtxVerdict(r, 0);
            const uint8_t hl = adapter.GdxRacerMtxVerdict(r, 2);
            if (hl != 0) {
                anyHighlight = true;
            }
            if (body != 0 && hl != 0 && body != hl && sAttackHlLogs < 24) {
                ++sAttackHlLogs;
                gdx_port_logf("[attack-hl] racer=%u body=%s highlight=%s  <-- same machine, "
                              "different instants\n",
                              r, body == 1 ? "LERPED" : "SNAPPED", hl == 1 ? "LERPED" : "SNAPPED");
            }
        }
        if (anyHighlight) {
            ++sHighlightTicks;
            bool disagreed = false;
            for (uint32_t r = 0; r < 30; ++r) {
                const uint8_t body = adapter.GdxRacerMtxVerdict(r, 0);
                const uint8_t hl = adapter.GdxRacerMtxVerdict(r, 2);
                if (body != 0 && hl != 0 && body != hl) {
                    disagreed = true;
                }
            }
            if (disagreed) {
                ++sDisagreeTicks;
            }
            // One summary line per 60 highlight ticks keeps this readable while still proving the
            // subject occurred. attackTicks=0 in a whole run means the script never attacked.
            if ((sHighlightTicks % 60u) == 0u) {
                gdx_port_logf("[attack-hl] summary: attackTicks=%u disagreeTicks=%u\n", sHighlightTicks,
                              sDisagreeTicks);
            }
        }
    }
    gdx_perf_sub_end(GDX_PERF_SUB_XLATE);
    // [venueload] The discriminating measurement. If translation stays flat across these ticks the
    // cost is the load itself and a boot-time preload fixes it. If it spikes, the ++gConvertEpoch
    // in the load path invalidated every cached conversion and we are paying full re-translation
    // for several consecutive ticks -- which no amount of preloading would avoid.
    if (gGdxVenueWatchTicks > 0 && gGdxInterpNowFn != nullptr) {
        --gGdxVenueWatchTicks;
        gdx_port_logf("[venueload] post tick=%d xlate=%.2fms lists=%zu cmds_out=%zu epoch=%u\n",
                      8 - gGdxVenueWatchTicks, (gGdxInterpNowFn() - gdxXlateT0) * 1000.0,
                      stats.convertedLists, stats.commandsOut, (unsigned) gConvertEpoch);
    }
    if (converted == nullptr) return;

    static bool sBridgeInitDiag = false;
    if (!sBridgeInitDiag) {
        sBridgeInitDiag = true;
        uintptr_t mb = 0, me = 0;
        GetMainModuleRange(mb, me);
        gdx_port_logf("[bridge-init] EXE module: base=%p end=%p size=0x%zx\n",
                      reinterpret_cast<void*>(mb), reinterpret_cast<void*>(me), static_cast<size_t>(me - mb));
        for (size_t ri = 0; ri < gHostRanges.size(); ++ri) {
            gdx_port_logf("[bridge-init] hostrange[%zu]: begin=%p low32=%08X size=0x%zx\n",
                          ri, reinterpret_cast<void*>(gHostRanges[ri].begin),
                          static_cast<unsigned>(gHostRanges[ri].begin & 0xFFFFFFFFu),
                          gHostRanges[ri].size);
        }
        gdx_port_logf("[bridge-init] DL root: ptr=%p size=%zu isBig=%d taskUcode=%d\n",
                      dl, dl_size, static_cast<int>(isBigEndian), static_cast<int>(taskUcode));
        /* One-shot dump of every ucode stub symbol's low32 so
           any [gfxdiag] ucode_raw / ucode_l3d_raw value in this log is
           attributable to a symbol without a PDB lookup. */
        gdx_port_logf("[bridge-init] ucode stubs: F3DEX2=%08X F3DLX2_Rej=%08X "
                      "F3DEX2_Rej=%08X F3DFLX2_Rej=%08X L3DEX2=%08X\n",
                      Low32(reinterpret_cast<uintptr_t>(gspF3DEX2_fifoTextStart)),
                      Low32(reinterpret_cast<uintptr_t>(gspF3DLX2_Rej_fifoTextStart)),
                      Low32(reinterpret_cast<uintptr_t>(gspF3DEX2_Rej_fifoTextStart)),
                      Low32(reinterpret_cast<uintptr_t>(gspF3DFLX2_Rej_fifoTextStart)),
                      Low32(reinterpret_cast<uintptr_t>(gspL3DEX2_fifoTextStart)));
    }

    static uint64_t sDiagFrames = 0;
    const bool shouldLogDiagnostics =
        sDiagFrames < 8 || (sDiagFrames % 120) == 0 ||
        stats.noopDisplayLists != 0 || stats.fallbackDataCommands != 0 ||
        stats.skippedDataCommands != 0 || stats.textureCopyBytes != 0 ||
        stats.ucodeSwitches != 0 || stats.unknownUcodeSwitches != 0 ||
        stats.l3dexUcodeSkips != 0 || stats.skippedEpochRetries != 0;
    if (shouldLogDiagnostics) {
        if (gdx_diag_verbose()) {
            const unsigned int romFallbackTotal = gdx_segment_source_fallback_total();
            gdx_port_logf("[gfxdiag] lists=%zu f3d_lists=%zu cmds=%zu noop_dl=%zu noop_raw=%08X "
                          "miss_dl=%zu miss_raw=%08X bad_dl=%zu bad_raw=%08X "
                          "fallback_data=%zu skip_data=%zu skip_tex=%zu skip_epoch=%zu "
                          "tex_copy_bytes=%zu vtx=%zu mtx=%zu dl=%zu teximg=%zu settile=%zu "
                          "tlut=%zu loadblk=%zu loadtile=%zu tilesize=%zu texrect=%zu fillrect=%zu "
                          "setcimg=%zu setzimg=%zu tris=%zu end=%zu "
                          "ucode_switch=%zu ucode_unknown=%zu ucode_raw=%08X "
                          "ucode_l3d_skip=%zu ucode_l3d_raw=%08X size=%zu romfb=%u\n",
                          stats.convertedLists, stats.f3dLists, stats.commandsOut,
                          stats.noopDisplayLists, stats.firstNoopDlRaw,
                          stats.missingDisplayLists, stats.firstMissingDlRaw,
                          stats.badDisplayLists, stats.firstBadDlRaw,
                          stats.fallbackDataCommands,
                          stats.skippedDataCommands, stats.skippedTextures, stats.skippedEpochRetries,
                          stats.textureCopyBytes, stats.opCounts[kOpVtx],
                          stats.opCounts[kOpMtx], stats.opCounts[kOpDl], stats.opCounts[kOpSetTextureImage], stats.opCounts[kOpSetTile],
                          stats.opCounts[kOpLoadTlut], stats.opCounts[kOpLoadBlock], stats.opCounts[kOpLoadTile], stats.opCounts[kOpSetTileSize],
                          stats.opCounts[0xE4] + stats.opCounts[0xE5], stats.opCounts[0xF6], stats.opCounts[kOpSetColorImage],
                          stats.opCounts[kOpSetDepthImage],
                          stats.opCounts[0x05] + stats.opCounts[0x06] + stats.opCounts[0x07] + stats.opCounts[0xBF],
                          stats.opCounts[kOpEndDl],
                          stats.ucodeSwitches, stats.unknownUcodeSwitches,
                          stats.firstUnknownUcodeRaw,
                          stats.l3dexUcodeSkips, stats.firstL3dexUcodeRaw, dl_size, romFallbackTotal);
            /* When any raw-ROM fallback has happened, emit one
               extra line listing the nonzero families, rate-limited to the periodic
               gfxdiag cadence so a per-frame stats trigger cannot spam it. */
            if (romFallbackTotal != 0 && (sDiagFrames < 8 || (sDiagFrames % 120) == 0)) {
                char famLine[512];
                size_t famOff = 0;
                const char* famKey = nullptr;
                unsigned int famFb = 0;
                for (unsigned int fi = 0; GdxSegmentSourceFamilyStats(fi, &famKey, &famFb); ++fi) {
                    if (famFb != 0 && famKey != nullptr && famOff + 1 < sizeof(famLine)) {
                        int n = std::snprintf(famLine + famOff, sizeof(famLine) - famOff,
                                              " %s=%u", famKey, famFb);
                        if (n > 0) {
                            famOff += static_cast<size_t>(n);
                        }
                        if (famOff >= sizeof(famLine)) {
                            famOff = sizeof(famLine) - 1;
                            break;
                        }
                    }
                }
                famLine[famOff] = '\0';
                gdx_port_logf("[gfxdiag] romfb families:%s\n", famLine);
            }
        }
        // [gfxfail]/[datafail] per-frame aggregate diagnostics: silent unless
        // GDX_DIAG_VERBOSE=1. The per-occurrence, bounded [gdl-miss]/[gdl-bad] lines
        // (and the bounded [gfxfail] ROOT-rejected error above) stay always-on.
        if (gdx_diag_verbose() && stats.noopDisplayLists != 0) {
            gdx_port_logf("[gfxfail] "
                          "miss=%zu raw=%08X parent=%p pidx=%zu pstride=%zu pbig=%d pf3d=%d "
                          "praw=%08X/%08X pdecoded=%08X/%08X "
                          "bad=%zu raw=%08X target=%p limit=%zu stride=%zu big=%d f3d=%d "
                          "first=%08X/%08X reason=%u fail_idx=%zu fail_op=%02X\n",
                          stats.missingDisplayLists,
                          stats.firstMissingDlRaw,
                          reinterpret_cast<void*>(stats.firstMissingParent),
                          stats.firstMissingParentIndex,
                          stats.firstMissingParentStride,
                          static_cast<int>(stats.firstMissingParentBigEndian),
                          static_cast<int>(stats.firstMissingParentF3D),
                          stats.firstMissingParentRawW0,
                          stats.firstMissingParentRawW1,
                          stats.firstMissingParentDecodedW0,
                          stats.firstMissingParentDecodedW1,
                          stats.badDisplayLists,
                          stats.firstBadDlRaw,
                          reinterpret_cast<void*>(stats.firstBadDlTarget),
                          stats.firstBadDlLimit,
                          stats.firstBadDlStride,
                          static_cast<int>(stats.firstBadDlBigEndian),
                          static_cast<int>(stats.firstBadDlF3D),
                          stats.firstBadDlFirstW0,
                          stats.firstBadDlFirstW1,
                          static_cast<unsigned>(stats.firstBadDlFailureReason),
                          stats.firstBadDlFailureIndex,
                          static_cast<unsigned>(stats.firstBadDlFailureOpcode));
        }
        if (gdx_diag_verbose() && (stats.fallbackDataCommands != 0 || stats.skippedDataCommands != 0)) {
            gdx_port_logf("[datafail] fallback=%zu op=%02X raw=%08X w0=%08X source=%p idx=%zu "
                          "skipped=%zu op=%02X raw=%08X w0=%08X\n",
                          stats.fallbackDataCommands,
                          static_cast<unsigned>(stats.firstFallbackDataOp),
                          stats.firstFallbackDataRaw,
                          stats.firstFallbackDataW0,
                          reinterpret_cast<void*>(stats.firstFallbackDataSource),
                          stats.firstFallbackDataIndex,
                          stats.skippedDataCommands,
                          static_cast<unsigned>(stats.firstSkippedDataOp),
                          stats.firstSkippedDataRaw,
                          stats.firstSkippedDataW0);
        }
    }
    sDiagFrames++;

    /* Cache eviction stays BEFORE Run so an in-place content refresh takes
       effect on the frame that produced it (LUS re-imports from the updated
       copy). The retired BUFFERS are freed after Run instead — see below. */
    if (!gPendingTextureCacheDeletes.empty()) {
        std::sort(gPendingTextureCacheDeletes.begin(), gPendingTextureCacheDeletes.end());
        gPendingTextureCacheDeletes.erase(
            std::unique(gPendingTextureCacheDeletes.begin(), gPendingTextureCacheDeletes.end()),
            gPendingTextureCacheDeletes.end());
        for (uintptr_t ptr : gPendingTextureCacheDeletes) {
            interp->TextureCacheDelete(reinterpret_cast<const uint8_t*>(ptr));
        }
        gPendingTextureCacheDeletes.clear();
    }

    interp->ResetGeometryDiagnostics();

    // ===== P0/P1 retention + M=2 replay (DEBUG-ONLY, GDX_INTERP_P0 / GDX_INTERP_P1) =====
    // Pools are quiescent across this whole window: D_800DCCFC ^= 1 toggles only in the NEXT
    // tick's Gfx_InitBuffer (decomp/src/sys/sys_gfx.c:115-125), so both GfxPools — and every byte
    // this buffer dereferences — are stable while we replay.
    // This block sits BEFORE the post-Run buffer frees below (gPersistentAllocations.clear, native
    // RGBA16 range retirement), so pass 1's Run() dereferences the same live inputs as pass 0.
    //
    // P0 (evidence-only, both passes t=1) and P1 (lerp: pass 1 renders the tween) are mutually
    // exclusive: P1 breaks P0's t=1 transparency/hash invariants, so when GDX_INTERP_P1 is set the
    // P0 evidence path is suppressed and P1 owns the replay. PASS ORDERING (verified against this
    // block + interpreter.cpp): each interp->Run() clears+redraws mGameFb WITHOUT presenting
    // (present is interp->EndFrame()/SwapBuffers, host-called ONCE), so the LAST Run's output is
    // what EndFrame presents. Pass 0 renders at t=1 first; pass 1 renders the presented frame at
    // presentT (0.5 for "mid") second — so the host presents the interpolated midpoint.
    // ===== P2 branch: host-driven main-loop render/logic decoupling =====
    // When the host owns pacing this tick (gEnhancements.Graphics.FrameInterpolation / GDX_INTERP_P2,
    // committed by main.cpp via gdx_gfx_interp_tick_config before dispatch), the retained buffer is
    // replayed AND PRESENTED M times right here — the only place it is alive (this sits before the
    // post-Run frees; the pool stays quiescent until the next tick's Gfx_InitBuffer). Each
    // sub-frame is a COMPLETE present via fw->DrawAndRunGraphicsCommands (StartDraw -> StartFrame ->
    // Run -> EndDraw -> EndFrame), so composite / ImGui / MSAA-resolve are all correct per sub-frame.
    // The host does NOT open its own present bracket on interp ticks (would nest the ImGui frame);
    // it hands ownership here and only paces the LOGIC deadline (the frame pacer is mutually excluded).
    // The env-gated P0/P1 in-bridge diagnostics keep their own single-present M=2 path in the else.
    const bool p2Host = adapter.GdxP2HostActive() && gGdxInterpHostCfg.active;
    if (p2Host) {
        // Game logic already ran exactly once (ConvertRoot above); no sub-frame re-enters it.
        // Sub-frames read the two GfxPools and write only scratch — render-only (prime directive).
        // DETERMINISTIC sub-frame schedule (SoH interpolate_frame).
        // main.cpp derives `count` (delivered as maxSubframes) from the target rate via a rational
        // remainder accumulator (running remainder, NO clock reads), so it is stable per tick and
        // averages target/60 sub-frames per tick across ticks. Here we simply present exactly `count`
        // evenly-spaced sub-frames at t = (k+1)/count. The OLD wall-clock accumulator (t = (now -
        // tickStart)/tickDur sampled inside the loop) coupled t to game-logic wall time, so t clustered
        // unpredictably and the present count oscillated with VSync jitter into an unstable
        // framerate. Even spacing with the LAST pass at t = count/count = 1.0 presents the newest pose
        // (byte-identical to stock, minimizing latency) with uniform tweens leading up to it.
        const int count = (gGdxInterpHostCfg.maxSubframes > 0) ? gGdxInterpHostCfg.maxSubframes : 1;
        const size_t lerpSlots = adapter.GdxP0ScratchSlots();
        // Discontinuity safety: if this tick referenced no pool
        // matrices, or every referenced slot snapped (empty lerp list, or PrevPoolBase mismatch so all
        // prev keyframes are unusable), there is nothing to tween — every pass renders at t=1
        // (content identical to the disabled path for this tick).
        // The old guard also DROPPED to a
        // single present on these ticks, so the presented rate FLAPPED between the target and 60
        // whenever a menu/transition/cut tick had zero lerpable slots (telemetry: 120 -> 91 -> 60 ->
        // 120 across one menu visit). A flapping rate is far more jarring than the redundant
        // re-presents are costly, so keep presenting `count` passes — all at t=1 — for a CONSTANT
        // present cadence. Content per pass is byte-identical to the old single pass, so the
        // transition-capture contract (GdxTransitionCapturePendingThisTick block above: the mirror
        // must sample the un-interpolated tick) is preserved exactly.
        const bool degenerate = (lerpSlots == 0) || (adapter.GdxP1Lerped() == 0);
        // Mutable: the budget pre-sizer below may shrink this BEFORE the loop, so the t denominator
        // shrinks with it and the last presented pose stays t=1. See the pre-sizer comment.
        int passes = count;
        const int gdxPlannedPasses = count;

        // [interp-pace] probe: when each pass was ATTEMPTED and whether the limiter took it. Kept
        // after the pacing experiment it was built to test (see the note in the loop) because it
        // settled that question in a single run where frame-rate averages had argued in circles for
        // hours. `window` is how much of the tick remained when the present loop was entered --
        // measured at ~15.86ms of a 16.68ms tick, i.e. the loop starts almost at the tick boundary,
        // NOT after a long game-logic phase as the perf phase breakdown had suggested.
        const double gdxLoopStart = (gGdxInterpNowFn != nullptr) ? gGdxInterpNowFn() : 0.0;
        double gdxPaceWindow = 0.0;
        if (gGdxInterpNowFn != nullptr && gGdxInterpHostCfg.tickDuration > 0.0) {
            gdxPaceWindow = (gGdxInterpHostCfg.tickStart + gGdxInterpHostCfg.tickDuration) - gdxLoopStart;
            if (gdxPaceWindow <= 0.0 || gdxPaceWindow > gGdxInterpHostCfg.tickDuration) {
                gdxPaceWindow = 0.0;
            }
        }
        double gdxAttemptAt[8] = {};
        bool gdxAttemptOk[8] = {};

        // Take ownership of pacing for the sub-frame burst; the swapchain's waitable object does the
        // real pacing. See the block comment on gdx_fast3d_set_subframe_present in
        // libultraship/src/fast/Fast3dWindow.cpp for why the software limiter cannot pace a burst.
        // GDX_INTERP_LIMITER=1 restores the old behaviour for A/B without a rebuild.
        static const bool sHonourLimiter = [] {
            const char* e = getenv("GDX_INTERP_LIMITER");
            return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
        }();
        gdx_fast3d_set_subframe_present(sHonourLimiter ? 0 : 1);

        // Budget guard for the loop below. Declared out here, not in the loop body, so its
        // thread-safe-static guard variable is not re-checked on every pass.
        //
        // WHY THIS EXISTS. Bypassing the software limiter above removed the only mechanism in the
        // system that could shed load: IsFrameReady used to return false and skip a present cheaply
        // when the schedule was behind. Nothing replaced it, so the tick had to pay for all M
        // presents no matter how long they took. That is not merely a frame-rate problem, because
        // gdx_vi_tick advances the simulation exactly ONCE per host-loop iteration and there is no
        // catch-up anywhere: every millisecond this loop overruns is a millisecond the 60 Hz sim
        // never gets back. Measured consequence before this guard, at a 144 Hz target: sim rate
        // median 57 Hz, mean 52 Hz, worst 8.6 Hz -- the game visibly in slow motion, the
        // machine-select model spinning slow and the 3-2-1-GO countdown stretching in real seconds.
        //
        // THE TRADE, stated plainly: when the tick budget is gone, drop the remaining sub-frames.
        // A frame-rate dip is a smoothness cost; a slow game clock is a correctness fault. Smoothness
        // is the thing that yields.
        //
        // HOW IT DECIDES: an AIMD controller on the replay count, steered by the sim-schedule slip
        // the host publishes each tick -- see the block comment on sAimdReplayCeiling below for
        // why that signal and not a per-pass cost estimate (two predictive-cost versions of this
        // guard were measured causing the very shortfall they existed to prevent).
        //
        // GDX_NO_INTERP_BUDGET=1 disables the guard for A/B without a rebuild. A suppressed guard
        // must reproduce the slow sim to earn the claim that it is what fixed it.
        static const bool sNoBudgetGuard = [] {
            const char* e = getenv("GDX_NO_INTERP_BUDGET");
            return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
        }();
        // CLOSED-LOOP BURST SIZING (AIMD). This replaces a predictive cost model, and the reasons
        // it was replaced are load-bearing, so they are recorded here.
        //
        // The old guard priced a pass at its measured wall time and refused passes that "did not
        // fit" the tick. That measurement is unusable under VSync: a present ends in
        // WaitForSingleObject on the swapchain waitable (gfx_dxgi.cpp), so with a saturated
        // swapchain the sample converges to one refresh interval -- 6.94 ms at 144 Hz --
        // REGARDLESS of render work (gdx_perf.h: "includes any vsync/latency block"). Priced that
        // way, a second sub-frame is affordable only while all non-burst work stays under ~2.8 ms,
        // a property of the target rate rather than of the scene, and the one-sided estimator
        // oscillated (refusal decays it 6%/tick; only a pass that ran can raise it). Measured
        // result: a 61-82 presents/s band, mean 67.9, against a 144 Hz target. Run C with the
        // guard bypassed: 144.0-145.9 presents/s, zero drops, sim_hz 59.2-60.0 -- every counted
        // drop had been the guard's own decision.
        //
        // Per-tick wall time cannot steer this loop either: at 144 Hz the rational accumulator
        // alternates 2- and 3-pass ticks (M averages 2.4), so tick length legitimately alternates
        // ~13.9 / ~20.8 ms around the 16.68 ms tick while the host's RUNNING logic schedule
        // absorbs the difference (short ticks recover what long ticks overran). A "did this tick
        // overrun" test fires on every 3-pass tick by construction.
        //
        // So steer on the quantity the guard exists to protect: the sim schedule itself. main.cpp
        // publishes, after the logic pacer, how far the tick finished past its running deadline
        // (gdx_gfx_interp_set_sim_slip). Slip behaves as a queue: healthy operation holds it near
        // zero with transient bumps that short ticks drain; a machine that cannot afford the burst
        // grows it without bound -- the slow-motion failure observed directly rather than
        // predicted. The trade is unchanged from the old guard: when slip says the sim is losing
        // time, sub-frames are what yield, because a frame-rate dip is a smoothness cost while a
        // slow game clock is a correctness fault.
        //
        // AIMD on the replay ceiling: halve on sustained slip (multiplicative decrease, behind a
        // cooldown so one decision can reach the signal before the next is taken), grow by one
        // after a run of healthy ticks (additive increase). The ceiling starts at the cap: the
        // optimistic start mis-sizes at most a cooldown's worth of early ticks, while a
        // pessimistic start would spend every session's first seconds interpolating nothing.
        // Unlike the old replay-cost EMA, neither direction can deadlock -- the increase is driven
        // by healthy ticks, not by replays that must first be permitted to run.
        static int sAimdReplayCeiling = 7; // replays beyond pass 0; pass telemetry arrays hold 8
        static int sAimdHealthyTicks = 0;
        static int sAimdCooldownTicks = 0;
        int budgetDropped = 0;

        // PRE-SIZE THE BURST; never clip it mid-loop. The t values are fixed at (k+1)/passes, so
        // clipping the third pass of a 3-pass tick would present t=1/3 and t=2/3 and NEVER PRESENT
        // t=1 -- the motion stream skips the tick's newest pose and takes a double-width step into
        // the next tick (0.33, 0.67, then 1.33): a judder spike riding exactly on the ticks that
        // were already struggling. Sizing BEFORE the loop lets the t denominator shrink with the
        // decision: a tick sized to 2 passes presents t=1/2 and t=1 -- evenly spaced, terminal
        // pose correct, no skip.
        //
        // Pass 0 always runs (this loop owns presentation; see the pass-0 rule in the loop), so
        // the ceiling only ever bounds REPLAYS. A mid-burst stall (shader compile, texture upload)
        // is answered one tick later through the slip signal, not by aborting the burst it
        // happened in -- that abort is what produced the skipped-terminal-pose judder.
        if (!sNoBudgetGuard && gGdxInterpHostCfg.tickDuration > 0.0) {
            const double slip = gGdxInterpSimSlipSec;
            const double tickDur = gGdxInterpHostCfg.tickDuration;
            if (sAimdCooldownTicks > 0) {
                --sAimdCooldownTicks;
            }
            if (slip > tickDur * 0.5) {
                // Sustained slip: the sim is losing real time. Halve, then hold for ~0.5 s so the
                // smaller burst can reach the published signal before the next decision.
                if (sAimdCooldownTicks == 0 && sAimdReplayCeiling > 0) {
                    sAimdReplayCeiling /= 2;
                    sAimdCooldownTicks = 30;
                }
                sAimdHealthyTicks = 0;
            } else if (slip < tickDur * 0.125) {
                if (++sAimdHealthyTicks >= 30 && sAimdReplayCeiling < 7) {
                    ++sAimdReplayCeiling;
                    sAimdHealthyTicks = 0;
                }
            }
            if (passes > 1 + sAimdReplayCeiling) {
                budgetDropped += passes - (1 + sAimdReplayCeiling);
                passes = 1 + sAimdReplayCeiling;
            }
        }

        int presented = 0;
        float lastT = 1.0f;
        // Pool quiescence: latch the GfxPool double-buffer parity before the loop.
        // The toggle (D_800DCCFC ^= 1) only ever runs in the NEXT tick's Gfx_InitBuffer (inside
        // gdx_dispatch), which this loop never re-enters, so the parity CANNOT change while
        // we replay both quiescent pools. We re-check after the loop and log once if the invariant is
        // ever violated — a falsifiable guard rather than a mid-present abort (aborting after frames
        // are already on screen would be worse than a diagnostic on a can't-happen path).
        const int poolParityAtStart = gdx_interp::PoolParity();
        // Issue G (in-race flicker under interpolation) — DOCUMENTED LIMITATION, not a bug here.
        // Every sub-frame below replays the SAME retained command buffer, substituting only
        // interpolated pool MATRICES (scratch slots). The command stream's combiner/alpha/vertex
        // data is whatever game logic emitted for THIS tick, keyed on gGameFrameCount (F-Zero X's
        // flicker-blend transparency: e.g. the low-energy body-color gradient blend and the
        // pursuit check-marker prim-alpha pulse alternate every 60 Hz tick — see decomp racer.c
        // gGameFrameCount & 7 / & 0xF modulation). That phase is
        // therefore FROZEN across all M sub-frames and only advances at the next 60 Hz tick.
        // Consequence: motion is smooth at the target rate, but the 60 Hz alternation is displayed
        // for M refreshes per phase, and because the rational accumulator makes M oscillate (2,3,2,3
        // at 144 Hz = target/60 = 2.4) consecutive phases get UNEQUAL screen time, so the intended
        // even blend reads as strobing. Option (c) "render each sub-frame's matching phase" is
        // infeasible: the other phase lives only in the adjacent tick's command buffer, which was
        // never retained, and re-deriving it would require re-running (or reaching back into) game
        // logic — a violation of the render-only prime directive. Option (a) "detect and blend
        // flicker-blend DL pairs" cannot distinguish game-logic-keyed alternation from legitimate
        // per-frame animation at the DL level, so it would blur correct content. The honest, safe
        // resolution is to leave interpolation matrix-only (SoH-class ports carry the identical
        // artifact) and surface it to the user via the FrameInterpolation menu tooltip.
        // Sub-frames the DXGI/GL limiter refused. DrawAndRunGraphicsCommands returns false without
        // rendering when the frame is dropped (Fast3dWindow.cpp: the IsFrameReady guard); counting
        // those as delivered made presents/s an upper bound and produced readings ABOVE the target
        // (a 153.5 sample against a 144 Hz target), which is what hid the real menu cost.
        // Seeded with the pre-sizer's drops so presented+dropped always sums to the PLANNED count:
        // a pass shed before the loop and a pass refused inside it are the same fact to telemetry.
        int dropped = budgetDropped;
        if (passes > 1) {
            ++gGdxIdemMultiPassTicks; // denominator for the divergence ratio
        }
        for (int k = 0; k < passes; ++k) {
            // TRIED AND REVERTED (2026-08-02): holding each pass until loopStart + window*k/passes.
            // The [interp-pace] probe proved the wait worked -- gaps came out at exactly
            // window/passes, 7.93ms for two passes -- and presents were STILL refused, frequently
            // every pass in a tick including pass 0. A refused FIRST present cannot be caused by
            // intra-tick spacing, so burst-calling was not the fault. It also regressed throughput
            // (median 63 presents/s, never above 128) and could not have reached 144 by
            // construction: window/passes yields 7.93ms gaps inside the tick and 8.75ms across the
            // tick boundary, averaging ~120 fps. Do not re-attempt without first explaining why
            // whole ticks are refused.
            // Pass 0 is never dropped: this loop owns presentation for the whole tick
            // (gGdxInterpPresentedLastTick tells the host not to present again), so skipping every
            // pass would leave the previous frame on screen and turn a rate dip into a freeze. One
            // present per tick is the floor, which is exactly the non-interpolated behaviour.
            // Replays are bounded by the AIMD pre-sizer above; there is deliberately no mid-loop
            // refusal (see the pre-sizer comment on the skipped-terminal-pose judder).
            const double gdxPassStart = (gGdxInterpNowFn != nullptr) ? gGdxInterpNowFn() : 0.0;
            if (k < 8) {
                gdxAttemptAt[k] = gdxPassStart;
            }
            // Deterministic even spacing: t = (k+1)/passes. Last pass = 1.0 (newest pose, exact).
            // GDX_INTERP_FORCE_T1: diagnostic only. Pins every sub-frame to t=1 so all M passes get
            // IDENTICAL matrices, while still replaying and presenting M times. This is the only way
            // to read idem_div as a statement about STATE: with varying t the passes render different
            // poses, so different triangles cull and clip and the bind sequence legitimately differs
            // -- divergence then means nothing. With t pinned, the input to every pass is identical,
            // so ANY divergence is state leaking across replays. Motion is frozen to 60 Hz while
            // this is set, which is expected; it is a measurement mode, not a play mode.
            static const bool sForceT1 = [] {
                const char* e = getenv("GDX_INTERP_FORCE_T1");
                const bool on = e != nullptr && e[0] != '\0' && strcmp(e, "0") != 0;
                // Announced unconditionally, once, because an A/B whose arm state is not in the log
                // is not an experiment. A FORCE_T1 play-test was run to falsify the booster
                // afterimage mechanism and the log carried NO evidence the pin was live -- lerped=
                // stays nonzero either way (it counts classification in ConvertRoot, not refills),
                // and t_last reads 1.000 either way. The result was uninterpretable and the run
                // wasted. Every A/B toggle must write its state to the log at arm time.
                gdx_port_logf("[interp] GDX_INTERP_FORCE_T1=%s (sub-frame t %s)\n", on ? "1" : "unset/0",
                              on ? "PINNED to 1.0 -- measurement mode, 60Hz motion expected" : "normal");
                return on;
            }();
            float t = (degenerate || sForceT1) ? 1.0f
                                               : (static_cast<float>(k + 1) / static_cast<float>(passes));
            if (t > 1.0f) {
                t = 1.0f;
            }
            adapter.GdxP0RefillScratch(t); // lerp(prev,cur,t) into every non-snapped scratch slot
            adapter.GdxVpRefillScratch(t);  // Tier 2: carousel viewports, same t
            adapter.GdxVtxRefillScratch(t); // Tier 3: effects vertices, same t
            adapter.GdxScreenProbe(t);      // where the machine actually lands this sub-frame
            // Re-arm the task's entry ucode variant. Replaying a display list must start from the
            // same RSP state pass 0 started from; a mid-list variant switch leaks forward otherwise.
            // See gdxTaskVariant above for the measurement this fixes.
            interp->SetF3dex2Variant(gdxTaskVariant);
            interp->ResetGeometryDiagnostics();
            // Full present of the SAME retained command buffer at this sub-frame's t. VSync ON: each
            // present blocks on the panel refresh, so the presents self-pace to the display and the
            // host logic-deadline wait (main.cpp) becomes a near-no-op. VSync OFF: presents don't
            // block; main.cpp's logic-deadline wait paces the SIM to 60 Hz. Logic stays 60 Hz either
            // way; the rational accumulator keeps the long-run present rate at the target.
            // Bracketed per PASS, not around the whole loop: a single bracket could not separate
            // pass 0 from pass 2, nor CPU work from the vsync wait. gdx_perf_sub_end accumulates
            // (subMs[id] += ...), so the per-tick total is unchanged and the phase mean now
            // reports per-sub-frame cost.
            // [interp-idem] MAKE THE REPLAY IDEMPOTENT. Measured before this existed: 182 of 3928
            // multi-pass ticks (4.6%) bound DIFFERENT textures on a replay than on pass 0, which is
            // the in-race floor flicker -- it appears the instant M > 1 and is clean at M == 1, and
            // uneven phase timing is ruled out (a true 120 Hz panel gives M == 2 exactly, every
            // phase on screen for 16.67ms, and it still strobed).
            //
            // Geometry cannot differ between replays: the track carries no matrix of its own and
            // the camera is not rerouted in a Release build. So the difference was STATE. Interpreter
            // RDP state survives Run(): loaded_texture[512], the emulated tmem[4096], the palette
            // staging, the tile descriptors -- and tmem_generation, which the header describes as
            // "bumped on every TMEM write so the texture cache can key on content". Pass 1 therefore
            // began from pass 0's END state and generated DIFFERENT cache keys for identical
            // content, selecting different textures for the same draws.
            //
            // Snapshot before the first pass, restore before every later one, so each sub-frame is
            // a pure function of (command buffer, matrices) exactly as it must be. The GPU-side
            // texture cache is deliberately NOT restored: it is content-keyed, so re-executing the
            // same loads hits it rather than re-uploading. ~21 KB memcpy once or twice per tick.
            // REVERTED 2026-08-02. The snapshot/restore below is left here, disabled, as a record of
            // a fix that was wrong. Restoring *mRdp before each replay produced a VISIBLE
            // REGRESSION -- boost/heal plates rendered black and the HUD position digit vanished --
            // because RDP is not self-contained: loaded_texture[] carries raw_tex_metadata with
            // live resource handles, and the emulated tmem/generation pair is what the GPU-side
            // texture cache keys on. Rewinding that half of the pair while the cache itself (which
            // is deliberately NOT restored, and which evicts and frees GPU textures during a pass)
            // moves forward leaves the two describing different worlds, and the replay binds
            // textures that no longer exist.
            //
            // The idea was sound -- every sub-frame should be a pure function of (command buffer,
            // matrices) -- but this is the wrong seam to enforce it at, and it was shipped on a
            // hypothesis that had never been measured. It made things worse for three builds.
            // [interp-idem] Reset interpreter state that survives Run() so each replay starts from
            // the same deterministic baseline. The prior full *mRdp snapshot/restore was reverted:
            // rewinding loaded_texture live resource handles while the GPU cache moved forward made
            // later passes bind textures that no longer existed. Starting from a clean default RDP
            // state avoids stale handles and keeps the emulated TMEM/generation pair in sync with
            // the content-keyed GPU cache. SpReset() already resets most RSP state but leaves
            // geometry_mode untouched, so pass 1 was inheriting pass 0's cull/lighting flags.
            // Perfection-plan Slice 1: the reset now lives in Interpreter::ResetRdpForReplay() and
            // covers tile descriptors, rasterizer modes, colors, chroma key, viewport/scissor, and
            // the renderer-side tracked bindings on top of the original TMEM family.
            if (interp != nullptr && k > 0) {
                interp->ResetRdpForReplay();
                gdx_cki("[interp-idem] replay reset tick", (int)gGdxIdemMultiPassTicks);
                gdx_cki("[interp-idem] replay reset pass", k);
            }
            // Is replaying one tick's display list IDEMPOTENT? Kept after the fix as a regression
            // guard: idem_div must stay at 0 now. Any future change that reintroduces cross-replay
            // state will show up here instead of as a bug report about flicker.
            // [interp-shot] Arm the capture for THIS pass. gdx_gfx_post_run_capture (below) runs
            // inside DrawAndRunGraphicsCommands right after Interpreter::Run, which is the only
            // point where the sub-frame's image exists and nothing has presented yet.
            {
                static const long sDumpTick = [] {
                    const char* e = getenv("GDX_INTERP_DUMP_TICK");
                    return (e != nullptr && e[0] != 0) ? strtol(e, nullptr, 10) : -1L;
                }();
                gGdxShotArmedPass = (sDumpTick >= 0 && gGdxIdemMultiPassTicks == (size_t) sDumpTick)
                                        ? k
                                        : -1;
            }
            gdx_gfx_texbind_hash_reset();
            // [interp-geo] Decoupled from the screenshot tick. Pinning the census to one fixed tick
            // made it a lottery: whether a pass renders at all depends on the swapchain limiter, and
            // the first attempt landed on a tick where 2 of 3 passes were refused, so there was no
            // pass-to-pass comparison to make. GDX_INTERP_GEO=<n> logs the census for EVERY pass of
            // every nth multi-pass tick, so a single run yields many comparable pairs and the ones
            // where two or more passes actually rendered can be picked out afterwards.
            static const long sGeoEvery = [] {
                const char* e = getenv("GDX_INTERP_GEO");
                return (e != nullptr && e[0] != 0) ? strtol(e, nullptr, 10) : 0L;
            }();
            const bool gdxGeoDiag =
                gGdxShotArmedPass >= 0 ||
                (sGeoEvery > 0 && (gGdxIdemMultiPassTicks % (size_t) sGeoEvery) == 0);
            // [interp-geo] RSP state this pass INHERITS. Interpreter::Run calls SpReset first, but
            // SpReset resets a specific list (extra_geometry_mode, matrix stack SIZE, branch_z
            // target, viewport z scale/trans, lights, dmem) and geometry_mode is NOT in it --
            // interpreter.cpp:7187-7210. geometry_mode carries G_CULL_BACK/FRONT, G_LIGHTING,
            // G_FOG and G_TEXTURE_GEN, and is read at :3105 for the cull test. So pass 0 inherits it
            // from the PREVIOUS tick's last list while pass 1 inherits it from pass 0's end. If
            // those differ, the two passes cull differently -- and the measured cull counts do
            // differ (1483 vs 1477) while vertices loaded are identical.
            const uint32_t gdxInheritedGeoMode =
                (interp != nullptr && interp->mRsp != nullptr) ? interp->mRsp->geometry_mode : 0u;
            // [interp-geo] The converted command buffer is supposed to be immutable across the M
            // replays -- the same bytes handed to Run() every pass. Hashing it either side of the
            // run tests that directly: if cmd_in differs between pass 0 and pass 1, the interpreter
            // rewrote operands in place during pass 0 and later passes are walking a DIFFERENT
            // display list, which would explain one-shot divergence without any RSP state leak.
            const uint64_t gdxCmdIn = gdxGeoDiag ? adapter.GdxP0HashCommands() : 0ull;
            gdx_perf_sub_begin(GDX_PERF_SUB_RUN);
            const bool delivered = fw->DrawAndRunGraphicsCommands(reinterpret_cast<Gfx*>(converted), {});
            gdx_perf_sub_end(GDX_PERF_SUB_RUN);
            const uint64_t gdxCmdOut = gdxGeoDiag ? adapter.GdxP0HashCommands() : 0ull;

            // Perfection-plan Slice 2: the idempotence divergence counter. sGdxIdemPass0Hash was
            // declared but never compared; the hash itself accumulates in the interpreter on every
            // texture bind (gdx_gfx_texbind_hash_reset above clears it per pass). Pass 0 stores the
            // baseline, every later pass compares: a mismatch means the replay bound different
            // textures for identical content -- the exact signature of the #28 floor flicker.
            // Under GDX_INTERP_FORCE_T1=1 with the hermetic reset this must stay at 0.
            {
                const unsigned long long texbindHash = gdx_gfx_texbind_hash();
                if (k == 0) {
                    sGdxIdemPass0Hash = texbindHash;
                    sGdxIdemTickCounted = false;
                } else if (!sGdxIdemTickCounted && texbindHash != sGdxIdemPass0Hash) {
                    sGdxIdemTickCounted = true;
                    ++gGdxIdemDivergentTicks;
                    static size_t sDivergenceLogCount = 0;
                    if (sDivergenceLogCount < 32) {
                        ++sDivergenceLogCount;
                        gdx_port_logf("[interp-idem] DIVERGENT tick=%lu pass=%d/%d hash0=%016llX hashK=%016llX\n",
                                      (unsigned long) gGdxIdemMultiPassTicks, k, passes,
                                      (unsigned long long) sGdxIdemPass0Hash, (unsigned long long) texbindHash);
                    }
                }
            }

            // [interp-geo] Per-pass geometry census on the dump tick. Owner-confirmed symptom: on
            // replay passes the FLOOR AND CLOUDS ARE NOT DRAWN AT ALL -- not shaded differently,
            // absent. ResetGeometryDiagnostics() already runs per pass, so these counters say WHERE
            // the draws are lost: fewer vertices loaded means the walk stopped reaching them; more
            // cull/clip/invisible means they were reached and thrown away; identical counts with
            // different pixels would put the loss in render state rather than geometry.
            if (gdxGeoDiag && interp != nullptr) {
                const auto& g = interp->GetGeometryDiagnostics();
                // forcet1 is recorded because it is otherwise invisible after the fact: t_last in the
                // [interp-p2] line is the LAST sub-frame's t, which is ~1.0 under ordinary
                // interpolation too, so it cannot distinguish a pinned run from a normal one. Every
                // conclusion about state leakage depends on knowing t was pinned.
                gdx_port_logf("[interp-geo] tick=%lu pass=%d/%d forcet1=%d drawn=%d in_geomode=%08X out_geomode=%08X "
                              "vtx=%llu invalid=%llu tris_sub=%llu clip=%llu "
                              "cull=%llu invis=%llu emitted=%llu "
                              "vhash=%016llX mphash=%016llX cmd_in=%016llX cmd_out=%016llX\n",
                              (unsigned long) gGdxIdemMultiPassTicks, k, passes, sForceT1 ? 1 : 0,
                              delivered ? 1 : 0, gdxInheritedGeoMode,
                              (interp->mRsp != nullptr) ? interp->mRsp->geometry_mode : 0u,
                              (unsigned long long) g.verticesLoaded,
                              (unsigned long long) g.invalidVertices,
                              (unsigned long long) g.trianglesSubmitted,
                              (unsigned long long) g.trianglesClipRejected,
                              (unsigned long long) g.trianglesCullRejected,
                              (unsigned long long) g.trianglesInvisible,
                              (unsigned long long) g.trianglesEmitted,
                              (unsigned long long) g.vertexHash,
                              (unsigned long long) g.mpFirstHash,
                              (unsigned long long) gdxCmdIn,
                              (unsigned long long) gdxCmdOut);
            }

            if (delivered) {
                ++presented;
            } else {
                ++dropped;
            }
            if (k < 8) {
                gdxAttemptOk[k] = delivered;
            }
            lastT = t;
        }
        // Hand pacing back before leaving the burst, so the host's own taskless-VI present and
        // every non-interpolated path keep honouring the limiter exactly as they do today.
        gdx_fast3d_set_subframe_present(0);

        gdx_perf_sub_begin(GDX_PERF_SUB_POST);
        gdxPostTimerOpen = true;

        // [interp-pace] One line per 120 multi-pass ticks. gN is the wall-clock gap in ms between
        // pass N-1's attempt and pass N's; an 'X' suffix marks a pass the limiter refused. Reading:
        // even gaps with dropped=0 confirms burst-calling was the fault; even gaps with drops still
        // present kills the theory outright and says the limiter is rejecting for another reason;
        // near-zero gaps mean the wait above is not taking effect at all.
        // Gate on the PLANNED count: a tick the pre-sizer shrank from 3 to 1 is precisely the tick
        // this line exists to expose, and gating on the sized count would hide it.
        if (gdxPlannedPasses > 1 && gGdxInterpNowFn != nullptr) {
            static size_t sPaceLogTick = 0;
            if ((++sPaceLogTick % 120u) == 0u) {
                // Gaps are only meaningful between passes that were ACTUALLY ATTEMPTED. When the
                // budget guard breaks out, the remaining gdxAttemptAt[] slots keep their zero
                // initialiser, and differencing those against a real timestamp printed nonsense
                // like g1=-23547.66 -- a seconds-since-epoch value wearing a milliseconds label.
                // -1.0 is this line's established "not applicable" marker; reuse it rather than
                // emit a number that looks like a measurement.
                const auto gap = [&](int i) {
                    if (passes <= i || gdxAttemptAt[i] <= 0.0 || gdxAttemptAt[i - 1] <= 0.0) {
                        return -1.0;
                    }
                    return (gdxAttemptAt[i] - gdxAttemptAt[i - 1]) * 1000.0;
                };
                const double g1 = gap(1);
                const double g2 = gap(2);
                const double g3 = gap(3);
                // budget= is the subset of `dropped` this tick that the budget guard skipped because
                // the tick was already spent, as distinct from presents the limiter refused. The two
                // mean opposite things: limiter drops say presentation is ahead of schedule, budget
                // drops say the tick could not afford the burst and the sim was about to lose time.
                gdx_port_logf("[interp-pace] passes=%d planned=%d window=%.2fms g1=%.2f%s g2=%.2f%s g3=%.2f%s "
                              "presented=%d dropped=%d budget=%d aimd=%d slip=%.2fms\n",
                              passes, gdxPlannedPasses, gdxPaceWindow * 1000.0,
                              g1, (passes > 1 && !gdxAttemptOk[1]) ? "X" : "",
                              g2, (passes > 2 && !gdxAttemptOk[2]) ? "X" : "",
                              g3, (passes > 3 && !gdxAttemptOk[3]) ? "X" : "",
                              presented, dropped, budgetDropped, sAimdReplayCeiling,
                              gGdxInterpSimSlipSec * 1000.0);
            }
        }

        gGdxInterpPresentedLastTick = true; // host must NOT present again for this tick
        gGdxInterpLastSubframes = presented;
        gGdxInterpLastDropped = dropped;
        gGdxInterpLastT = static_cast<double>(lastT);
        gGdxInterpLastLerped = adapter.GdxP1Lerped();
        gGdxInterpLastBorrowed = adapter.GdxP1BorrowedKeyframes();
        // Read straight from the file-scope camera counters rather than through the adapter: the
        // pose history outlives the adapter by design (several tasks per tick), and so do these.
        gGdxInterpLastBasisFixed = gGdxBasisJumpFixed;
        gGdxInterpLastCamRebuilt = gGdxCamRebuilds;
        gGdxInterpLastCamRejected = gGdxCamRejects;
        gGdxInterpLastCamEyeDelta = gGdxCamMaxEyeDelta;
        for (size_t w = 0; w < kCamWhyCount; ++w) {
            gGdxInterpLastCamWhy[w] = gGdxCamWhy[w];
        }
        gGdxInterpLastVpLerped = adapter.GdxVpLerped();
        gGdxInterpLastVpSnapped = adapter.GdxVpSnapped();
        gGdxInterpLastVtxLerped = adapter.GdxVtxLerped();
        gGdxInterpLastVtxSnapped = adapter.GdxVtxSnapped();
        if (adapter.GdxP1PairMaxDelta() > gGdxInterpPairMaxDelta) {
            gGdxInterpPairMaxDelta = adapter.GdxP1PairMaxDelta();
        }
        gGdxInterpPairSuspect += adapter.GdxP1PairSuspect();
        gGdxInterpPairLerped += adapter.GdxP1Lerped();
        gGdxInterpLastSnapped = adapter.GdxP1SnappedAbsent() + adapter.GdxP1SnappedTeleport() +
                                adapter.GdxP1SnappedCut() + adapter.GdxP1PoolBaseMisses();
        // Rolling presents-per-second meter (real presented FPS, not logic ticks).
        if (gGdxInterpNowFn != nullptr) {
            const double nowSec = gGdxInterpNowFn();
            if (gGdxInterpPresentWindowStart < 0.0) {
                gGdxInterpPresentWindowStart = nowSec;
                gGdxInterpPresentWindowCount = 0;
            }
            gGdxInterpPresentWindowCount += presented;
            const double elapsed = nowSec - gGdxInterpPresentWindowStart;
            if (elapsed >= 0.5) {
                gGdxInterpPresentsPerSec = gGdxInterpPresentWindowCount / elapsed;
                gGdxInterpPresentWindowStart = nowSec;
                gGdxInterpPresentWindowCount = 0;
            }
        }

        // Verify the pools stayed quiescent across the replay window. This can only
        // fire if the structural guarantee (no gdx_dispatch re-entry in the loop) is ever broken.
        if (gdx_interp::PoolParity() != poolParityAtStart) {
            static bool sPoolQuiescenceViolationLogged = false;
            if (!sPoolQuiescenceViolationLogged) {
                sPoolQuiescenceViolationLogged = true;
                gdx_port_logf("[interp-p4] FINDING pool parity changed across sub-frame loop "
                              "(%d -> %d): the GfxPool double-buffer toggled mid-replay — "
                              "quiescence invariant broken; sub-frames may have read torn pools\n",
                              poolParityAtStart, gdx_interp::PoolParity());
            }
        }

        // P4 evidence: on a transition-capture tick the whole frame was forced to snap, so
        // every one of the loop's `count` passes rendered the canonical t=1 content (constant-cadence
        // fix: the pass COUNT stays at M, but all passes are byte-identical un-interpolated frames)
        // and the frame mirror the game samples in Transition_SetBackgroundBuffer (later this same
        // tick) is un-interpolated. Capture snaps are rare (once per screen transition), so this
        // line is never a per-tick spam source.
        if (adapter.GdxCaptureSnapThisTick()) {
            gdx_port_logf("[interp-p4] transition-capture tick: forced canonical t=1 (subframes=%d) "
                          "so the captured background is un-interpolated\n", presented);
        }
    } else {
    const bool p1Active = adapter.GdxP1Enabled();
    const bool p0Active = adapter.GdxP0Enabled() && !p1Active; // P0 evidence only when P1 is off
    const bool interpActive = p0Active || p1Active;
    std::vector<uint64_t> p0Snapshot;
    uint64_t p0Hash0 = 0, p0Hash1 = 0;
    const size_t interpSlots = interpActive ? adapter.GdxP0ScratchSlots() : 0;
    const float pass1T = p1Active ? adapter.GdxInterpPresentT() : 1.0f;
    if (interpActive) {
        // Pass 0: refill scratch at t=1 (== current-pool matrix, byte-identical to stock). In P0
        // this also hashes/snapshots the retained command stream for cross-pass mutation counting.
        adapter.GdxP0RefillScratch(1.0f);
        adapter.GdxVpRefillScratch(1.0f);
        adapter.GdxVtxRefillScratch(1.0f);
        if (p0Active) {
            p0Hash0 = adapter.GdxP0HashCommands();
            adapter.GdxP0SnapshotCommands(p0Snapshot);
            gdx_port_logf("[interp-p0] pass=0 cmdhash=%016llX scratch_slots=%u\n",
                          static_cast<unsigned long long>(p0Hash0), static_cast<unsigned>(interpSlots));
        }
    }

    // Sub-phase: the interpreter Run (F3D command execution -> GPU submission). See gdx_perf.h.
    gdx_perf_sub_begin(GDX_PERF_SUB_RUN);
    interp->Run(reinterpret_cast<Gfx*>(converted), {}); // pass 0 (real render, t=1)
    gdx_perf_sub_end(GDX_PERF_SUB_RUN);

    if (interpActive) {
        // Pass 1 preamble: refill scratch for the presented pass — t=1 in P0 (no-op replay), the
        // configured presentT in P1 (writes lerp(prev,cur,t) into every non-snapped scratch slot).
        adapter.GdxP0RefillScratch(pass1T);
        adapter.GdxVpRefillScratch(pass1T);
        adapter.GdxVtxRefillScratch(pass1T);
        if (p0Active) {
            // An identical hash proves the retained buffer is stable and interp->Run() did NOT
            // mutate it in place; any changed operand is counted as a P0 FINDING (not hidden).
            p0Hash1 = adapter.GdxP0HashCommands();
            const size_t p0Muts = adapter.GdxP0CountMutations(p0Snapshot);
            const size_t p0Viol = adapter.GdxP0TransparencyViolations();
            gdx_port_logf("[interp-p0] pass=1 cmdhash=%016llX scratch_slots=%u\n",
                          static_cast<unsigned long long>(p0Hash1), static_cast<unsigned>(interpSlots));
            if (p0Hash1 != p0Hash0 || p0Muts != 0) {
                gdx_port_logf("[interp-p0] FINDING interpreter mutates retained buffer in place: "
                              "%u operand(s) changed across pass 0 (h0=%016llX h1=%016llX) -> P2 replay "
                              "must snapshot/restore the buffer per pass\n",
                              static_cast<unsigned>(p0Muts),
                              static_cast<unsigned long long>(p0Hash0),
                              static_cast<unsigned long long>(p0Hash1));
            }
            static bool sP0TransparencyLogged = false;
            if (!sP0TransparencyLogged) {
                sP0TransparencyLogged = true;
                gdx_port_logf("[interp-p0] scratch indirection: %u pool matrices rerouted, "
                              "t=1 transparent (violations=%u)\n",
                              static_cast<unsigned>(interpSlots), static_cast<unsigned>(p0Viol));
            }
        }
        if (p1Active) {
            // Per-tick lerp evidence. lerped = slots tweened; snapped_absent = no prev keyframe
            // (spawn/despawn via referenced-set, or unreadable/mismatched pool) ; snapped_teleport
            // = translation-magnitude cut heuristic. t is the presented pass's fraction.
            // Rate-limited: the tick counter advances every tick, but the line is
            // emitted only for the first 8 ticks and then every 120th (~1/2 s at 60 Hz) — mirroring
            // this file's shouldLogDiagnostics cadence — PLUS on any tick where the teleport-snap
            // heuristic fired (a notable cut event), so the log is never spammed at
            // 60 lines/s while still surfacing the steady-state counts and every discontinuity.
            static size_t sInterpP1Tick = 0;
            const size_t tick = sInterpP1Tick++;
            const size_t teleports = adapter.GdxP1SnappedTeleport();
            const size_t cutSnaps = adapter.GdxP1SnappedCut();
            const bool captureSnap = adapter.GdxCaptureSnapThisTick();
            // P3/P4: also surface on any tick a whole-frame cut/pause/capture snap fired,
            // alongside the first-8 / every-120th / teleport cadence.
            if (tick < 8 || (tick % 120) == 0 || teleports != 0 || cutSnaps != 0 || captureSnap) {
                gdx_port_logf("[interp-p1] tick=%zu lerped=%u snapped_absent=%u snapped_teleport=%u "
                              "snapped_cut=%u capture=%d t=%.3f\n",
                              tick,
                              static_cast<unsigned>(adapter.GdxP1Lerped()),
                              static_cast<unsigned>(adapter.GdxP1SnappedAbsent() +
                                                    adapter.GdxP1PoolBaseMisses()),
                              static_cast<unsigned>(teleports),
                              static_cast<unsigned>(cutSnaps),
                              captureSnap ? 1 : 0,
                              static_cast<double>(pass1T));
            }
        }

        // Genuine M=2: re-execute the SAME retained buffer a second time. Safe on the shipping DX11
        // backend: interp->Run() begins with mRapi->StartFrame() + StartDrawToFramebuffer +
        // ClearFramebuffer(true,true) and ends after the MSAA resolve WITHOUT presenting — present
        // is interp->EndFrame() -> mRapi->EndFrame()/SwapBuffers (interpreter.cpp:6884), called ONCE
        // by the host loop. So the second Run clears and redraws mGameFb (no draw-list accumulation,
        // no double present) and the host presents only this final pass — the interpolated midpoint
        // in P1. Presenting BOTH passes for real smoothness requires P2's loop decoupling
        // (main.cpp wrapping each sub-frame in its own StartFrame/EndFrame present bracket).
        // P1 "mid" therefore only demonstrates correct lerp math by presenting the midpoint
        // frame (motion appears half-a-tick behind, uniform).
        // ResetGeometryDiagnostics keeps the downstream [geodiag] counters reflecting a single pass.
        interp->ResetGeometryDiagnostics();
        interp->Run(reinterpret_cast<Gfx*>(converted), {}); // pass 1 (presented: t=1 in P0, presentT in P1)
    }
    } // end else (non-P2 path: default single Run + env-gated P0/P1 in-bridge M=2)

    // A real GFX task produced this host frame — the VI-scanout fallback must
    // not also draw over it (see gdx_vi_present_fallback).
    gHostFrameGfxTaskRan = true;
    // From now on, taskless presents hold this frame (via the mirror) instead
    // of scanning out the CPU VI framebuffer. sGpuHoldPixelsStale marks that
    // the mirror just got fresh content from this task (diag-only signal for
    // GdxDiagHoldTick — see gdx_vi_present_fallback's holdGpuFrame branch).
    sGpuContentLive = true;
    sGpuHoldPixelsStale = true;

    /* Transition_Draw releases its back-arena capture after emitting the last
       textured frame, but conversion and sampling happen here later in the
       same task.  Retire native-RGBA16 ownership only after Run has consumed
       that frame.  Otherwise clearing in Transition_Draw breaks the final
       strip/wipe; never clearing lets the reused arena address misclassify
       ordinary menu textures on subsequent frames. */
    if (!gPendingNativeRgba16RangeClears.empty()) {
        std::sort(gPendingNativeRgba16RangeClears.begin(), gPendingNativeRgba16RangeClears.end());
        gPendingNativeRgba16RangeClears.erase(
            std::unique(gPendingNativeRgba16RangeClears.begin(), gPendingNativeRgba16RangeClears.end()),
            gPendingNativeRgba16RangeClears.end());
        for (uintptr_t begin : gPendingNativeRgba16RangeClears) {
            gNativeRgba16Ranges.erase(
                std::remove_if(gNativeRgba16Ranges.begin(), gNativeRgba16Ranges.end(),
                               [begin](const HostRange& range) { return range.begin == begin; }),
                gNativeRgba16Ranges.end());
            if (gDiagTransitionCaptureBegin == begin) {
                gDiagTransitionCaptureBegin = 0;
                gDiagTransitionCaptureSize = 0;
            }
        }
        gPendingNativeRgba16RangeClears.clear();
    }

    /* Retired-buffer FREE happens AFTER Run: a texture copy that
       resizes during this frame's ProcessList moves its old buffer into
       gPersistentAllocations, but the converted command stream built above
       may still carry that old buffer's pointer as a texture source.
       Freeing before Run served the interpreter a dangling pointer for one
       frame per resize (MSVC debug heap 0xDD fill). Freeing after the frame
       has drawn is always safe: the next frame re-translates and re-imports
       from the live copies. */
    gPersistentAllocations.clear();

    // Transition snapshot mirror: with a DX11 flip-model swapchain the
    // backbuffer contents are undefined after present, so
    // gdx_read_current_framebuffer cannot read last frame's pixels out of
    // fb 0 on demand (observed as all-zero transition captures). Keep a
    // persistent GPU-side copy of every completed game frame instead; the
    // transition readback samples this mirror.
    // Sub-phase: frame-mirror refresh. See gdx_perf.h.
    gdx_perf_sub_begin(GDX_PERF_SUB_MIRROR);
    GdxUpdateFrameMirror(interp);
    gdx_perf_sub_end(GDX_PERF_SUB_MIRROR);

    const Fast::GeometryDiagnostics& geometry = interp->GetGeometryDiagnostics();
    /* Print-budget split: a single global sBigTriPrints<60 cap was
       shared across the whole process lifetime, including the machine-select/
       track-preview screens that run for many seconds before a race starts.
       Those screens routinely emit >60 frames with a surviving oversized
       triangle (that's the documented normal case for the Reject ucode there),
       so the entire diagnostic budget was silently exhausted before the race
       - and hence the actual course decorations/machines the user is reporting
       spikes on - ever produced a single [bigtri] line. Give race-active
       frames their own, much larger budget so a live run always captures
       this evidence regardless of what happened on earlier menu screens. */
    static int sBigTriPrintsMenu = 0;
    static int sBigTriPrintsRace = 0;
    constexpr int kBigTriMenuCap = 20;
    constexpr int kBigTriRaceCap = 4000;
    const bool bigTriRaceActive = gGdxRaceActive != 0;
    int& bigTriPrints = bigTriRaceActive ? sBigTriPrintsRace : sBigTriPrintsMenu;
    const int bigTriCap = bigTriRaceActive ? kBigTriRaceCap : kBigTriMenuCap;
    if (gdx_diag_verbose() && geometry.bigTriangles != 0 && bigTriPrints < bigTriCap) {
        ++bigTriPrints;
        gdx_port_logf(
            "[bigtri] race=%d ucode=%d count=%llu v0=(%.1f,%.1f,%.1f,%.2f) v1=(%.1f,%.1f,%.1f,%.2f) "
            "v2=(%.1f,%.1f,%.1f,%.2f) geo=%08X combine=%016llX tile=%u tex=%p "
            "vp=%.1f,%.1f %.1fx%.1f\n",
            (int)bigTriRaceActive,
            static_cast<int>(taskUcode),
            static_cast<unsigned long long>(geometry.bigTriangles),
            geometry.bigTriX[0], geometry.bigTriY[0], geometry.bigTriZ[0], geometry.bigTriW[0],
            geometry.bigTriX[1], geometry.bigTriY[1], geometry.bigTriZ[1], geometry.bigTriW[1],
            geometry.bigTriX[2], geometry.bigTriY[2], geometry.bigTriZ[2], geometry.bigTriW[2],
            geometry.bigTriGeometryMode,
            static_cast<unsigned long long>(geometry.bigTriCombine),
            static_cast<unsigned>(geometry.bigTriTile),
            geometry.bigTriTexture,
            geometry.bigTriViewportX, geometry.bigTriViewportY,
            geometry.bigTriViewportW, geometry.bigTriViewportH);
    }
    static int sLastGeometryTaskUcode = -1;
    const bool geometryUcodeChanged = sLastGeometryTaskUcode != static_cast<int>(taskUcode);
    // [geodiag]/[gpustate]/[phasegeom] are per-frame diagnostics: silent unless GDX_DIAG_VERBOSE=1.
    if (gdx_diag_verbose() &&
        (geometryUcodeChanged || (sDiagFrames % 120) == 0 ||
         geometry.invalidVertices != 0 || geometry.variantSwitches != 0)) {
        gdx_port_logf(
            "[geodiag] ucode=%d vtx=%llu invalid=%llu w_nonpos=%llu near=%llu far=%llu "
            "ndc_x=%.3f..%.3f ndc_y=%.3f..%.3f ndc_z=%.3f..%.3f "
            "tri_in=%llu clip=%llu cull=%llu "
            "invisible=%llu emitted=%llu dma=%llu flx_alpha_vtx=%llu gpu_draws=%llu gpu_tris=%llu\n",
            static_cast<int>(taskUcode),
            static_cast<unsigned long long>(geometry.verticesLoaded),
            static_cast<unsigned long long>(geometry.invalidVertices),
            static_cast<unsigned long long>(geometry.verticesNonPositiveW),
            static_cast<unsigned long long>(geometry.verticesOutsideNear),
            static_cast<unsigned long long>(geometry.verticesOutsideFar),
            geometry.minNdcX, geometry.maxNdcX,
            geometry.minNdcY, geometry.maxNdcY,
            geometry.minNdcZ, geometry.maxNdcZ,
            static_cast<unsigned long long>(geometry.trianglesSubmitted),
            static_cast<unsigned long long>(geometry.trianglesClipRejected),
            static_cast<unsigned long long>(geometry.trianglesCullRejected),
            static_cast<unsigned long long>(geometry.trianglesInvisible),
            static_cast<unsigned long long>(geometry.trianglesEmitted),
            static_cast<unsigned long long>(geometry.dmaIoLoads),
            static_cast<unsigned long long>(geometry.f3dflxAlphaVertices),
            static_cast<unsigned long long>(geometry.gpuDrawCalls),
            static_cast<unsigned long long>(geometry.gpuTriangles));
        gdx_port_logf(
            "[gpustate] vp=%.1f,%.1f %.1fx%.1f sc=%.1f,%.1f %.1fx%.1f "
            "other=%08X/%08X cimg=%p zimg=%p same=%d renders_fb=%d fb_active=%d\n",
            static_cast<double>(interp->mRdp->viewport.x),
            static_cast<double>(interp->mRdp->viewport.y),
            static_cast<double>(interp->mRdp->viewport.width),
            static_cast<double>(interp->mRdp->viewport.height),
            static_cast<double>(interp->mRdp->scissor.x),
            static_cast<double>(interp->mRdp->scissor.y),
            static_cast<double>(interp->mRdp->scissor.width),
            static_cast<double>(interp->mRdp->scissor.height),
            interp->mRdp->other_mode_h, interp->mRdp->other_mode_l,
            interp->mRdp->color_image_address, interp->mRdp->z_buf_address,
            interp->mRdp->color_image_address == interp->mRdp->z_buf_address,
            static_cast<int>(interp->mRendersToFb), static_cast<int>(interp->mFbActive));
        if (gdx_diag_verbose() && geometry.variantSwitches != 0) {
            gdx_port_logf(
                "[phasegeom] switches=%llu pre_flx_vtx=%llu pre_flx_tri=%llu "
                "pre_flx_emit=%llu pre_flx_draws=%llu pre_flx_gpu_tri=%llu "
                "pre_flx_rgba16=%llu/%llu forced_opaque=%llu pre_flx_depth_bypass=%llu "
                "material tex=%llu bound0=%llu missing0=%llu forced_simple=%llu "
                "shader=%016llX/%016llX uv=%.3f..%.3f,%.3f..%.3f "
                "texstate=%ux%u line=%u size=%u tile=%u tmem=%u mask=%u/%u scale=%04X/%04X "
                "fog tri=%llu bypass=%llu factor=%.3f..%.3f mul=%d off=%d "
                "other=%08X/%08X combine=%016llX texture=%p "
                "post_flx_vtx=%llu post_flx_emit=%llu post_flx_gpu_tri=%llu "
                "post_flx_rgba16=%llu/%llu forced_opaque=%llu post_flx_depth_bypass=%llu\n",
                static_cast<unsigned long long>(geometry.variantSwitches),
                static_cast<unsigned long long>(geometry.preFlxVertices),
                static_cast<unsigned long long>(geometry.preFlxTrianglesSubmitted),
                static_cast<unsigned long long>(geometry.preFlxTrianglesEmitted),
                static_cast<unsigned long long>(geometry.preFlxGpuDrawCalls),
                static_cast<unsigned long long>(geometry.preFlxGpuTriangles),
                static_cast<unsigned long long>(geometry.preFlxRgba16OpaquePixels),
                static_cast<unsigned long long>(geometry.preFlxRgba16TransparentPixels),
                static_cast<unsigned long long>(geometry.preFlxRgba16ForcedOpaquePixels),
                static_cast<unsigned long long>(geometry.preFlxDepthBypassTriangles),
                static_cast<unsigned long long>(geometry.preFlxTexturedTriangles),
                static_cast<unsigned long long>(geometry.preFlxTexture0BoundTriangles),
                static_cast<unsigned long long>(geometry.preFlxTexture0MissingTriangles),
                static_cast<unsigned long long>(geometry.preFlxForcedSimpleMaterialTriangles),
                static_cast<unsigned long long>(geometry.preFlxShaderId0),
                static_cast<unsigned long long>(geometry.preFlxShaderId1),
                geometry.preFlxMinTextureU, geometry.preFlxMaxTextureU,
                geometry.preFlxMinTextureV, geometry.preFlxMaxTextureV,
                geometry.preFlxTextureWidth, geometry.preFlxTextureHeight,
                geometry.preFlxTextureLineBytes, geometry.preFlxTextureSizeBytes,
                static_cast<unsigned int>(geometry.preFlxTextureTile),
                static_cast<unsigned int>(geometry.preFlxTextureTmem),
                static_cast<unsigned int>(geometry.preFlxTextureMaskS),
                static_cast<unsigned int>(geometry.preFlxTextureMaskT),
                static_cast<unsigned int>(geometry.preFlxTextureScaleS),
                static_cast<unsigned int>(geometry.preFlxTextureScaleT),
                static_cast<unsigned long long>(geometry.preFlxFogTriangles),
                static_cast<unsigned long long>(geometry.preFlxFogBypassTriangles),
                geometry.preFlxMinFogFactor, geometry.preFlxMaxFogFactor,
                static_cast<int>(geometry.preFlxFogMul),
                static_cast<int>(geometry.preFlxFogOffset),
                geometry.preFlxOtherModeH, geometry.preFlxOtherModeL,
                static_cast<unsigned long long>(geometry.preFlxCombineMode),
                geometry.preFlxTexture,
                static_cast<unsigned long long>(geometry.verticesLoaded - geometry.preFlxVertices),
                static_cast<unsigned long long>(geometry.trianglesEmitted -
                                                geometry.preFlxTrianglesEmitted),
                static_cast<unsigned long long>(geometry.gpuTriangles -
                                                geometry.preFlxGpuTriangles),
                static_cast<unsigned long long>(geometry.rgba16OpaquePixels -
                                                geometry.preFlxRgba16OpaquePixels),
                static_cast<unsigned long long>(geometry.rgba16TransparentPixels -
                                                geometry.preFlxRgba16TransparentPixels),
                static_cast<unsigned long long>(geometry.rgba16ForcedOpaquePixels -
                                                geometry.preFlxRgba16ForcedOpaquePixels),
                static_cast<unsigned long long>(geometry.depthBypassTriangles -
                                                geometry.preFlxDepthBypassTriangles));
        }
    }
    sLastGeometryTaskUcode = static_cast<int>(taskUcode);

    /* Mirror every distinct N64 framebuffer that was targeted as CIMG anywhere
       in this task, not just whatever CIMG happens to be set at the very end.
       A single task's display list frequently redirects CIMG to an offscreen
       framebuffer mid-task (the SETCIMG "canvas" idiom in texture_utils.c's
       func_8007AB88/func_8007ABA4, driven by the OBJECT_FRAMEBUFFER object
       type) and restores the normal display target before the task ends.
       Checking only the final color_image_address silently dropped every such
       mid-task target: it never became valid, so gdx_read_current_framebuffer
       callers (transition captures / canvas composites) never saw real data
       for it. stats.colorImageTargets was populated while converting this
       task's display list (every resolved G_SETCOLORIMAGE, deduped); fold in
       the interpreter's own final color image in case it was set by a path
       the adapter didn't see, then mirror each match against gN64Framebuffers.
       Only the buffer matching the task's FINAL color image updates
       gLastRenderedFramebuffer — that flag distinguishes "the active display
       target" (always re-read fresh) from other buffers holding frozen
       snapshots (preserved as-is by gdx_read_current_framebuffer). */
    const uintptr_t finalColorImage = reinterpret_cast<uintptr_t>(interp->mRdp->color_image_address);
    bool finalColorImageTracked = false;
    for (size_t ti = 0; ti < stats.colorImageTargetCount; ti++) {
        if (stats.colorImageTargets[ti] == finalColorImage) {
            finalColorImageTracked = true;
            break;
        }
    }
    if (!finalColorImageTracked && finalColorImage != 0 &&
        stats.colorImageTargetCount < stats.colorImageTargets.size()) {
        stats.colorImageTargets[stats.colorImageTargetCount++] = finalColorImage;
    }

    gdx_perf_sub_begin(GDX_PERF_SUB_FBMIRROR);
    for (size_t ti = 0; ti < stats.colorImageTargetCount; ti++) {
        const uintptr_t targetAddress = stats.colorImageTargets[ti];
        auto framebuffer = std::find_if(gN64Framebuffers.begin(), gN64Framebuffers.end(),
                                        [targetAddress](const N64FramebufferInfo& info) {
                                            return info.address == targetAddress;
                                        });
        if (framebuffer == gN64Framebuffers.end()) {
            continue;
        }
        // LAZY, not eager. This used to call ReadFramebufferToCPU here on EVERY task -- a full
        // GPU->CPU readback, which stalls the pipeline until the GPU drains and then drags the
        // pixels back across the bus. Measured at 7.16 ms of a 16.68 ms tick: the entire bridge
        // overhead of the frame, and single-handedly the reason frame interpolation could not
        // afford the sub-frames its target rate asked for (fbmirror=7.16 against bridge=7.18).
        //
        // It is redundant with its only consumer. gdx_read_current_framebuffer already performs
        // this readback on demand, preferring gFrameMirrorFb -- a GPU->GPU copy maintained by
        // GdxUpdateFrameMirror, which costs 0.00 ms because it never touches the CPU -- and falling
        // back to a direct read; it also sets valid and gLastRenderedFramebuffer itself once the
        // pixels are proven non-empty. Transition captures happen once per screen change, so paying
        // a full readback on all sixty ticks per second to have one ready was the wrong trade by
        // roughly three orders of magnitude.
        //
        // What still happens every task is the bookkeeping: the buffer is registered as the render
        // target so the on-demand path knows where to read from. Only the copy is deferred.
        //
        // GDX_EAGER_FBMIRROR=1 restores the per-task readback for A/B without a rebuild. If a
        // transition capture ever comes back empty, set it -- and if that fixes it, the on-demand
        // path is missing a case rather than this deferral being wrong.
        // CAPTURE TICKS STILL READ BACK EAGERLY. Deferring unconditionally regressed the Cup Select
        // transition: the readback also published framebuffer->valid, and a transition capture on
        // the FIRST visit to a screen consumes that flag before anything has set it
        // (gdx_read_current_framebuffer only sets it once it has proven non-empty pixels, which on a
        // first visit has not happened yet). The symptom was the historical "Cup Select squeeze",
        // first entry only -- see the gFrameMirrorFb dimension note in that investigation.
        //
        // GdxTransitionCapturePendingThisTick is the right gate rather than a heuristic: the capture
        // runs LATER IN THE SAME TICK than this loop (sys_gfx.c calls gdx_read_current_framebuffer
        // after the task), which is the ordering that function was written to establish. So the
        // expensive path costs one readback per screen transition instead of one per tick -- the
        // frequency it was always worth paying at.
        const bool gdxCaptureThisTick = GdxTransitionCapturePendingThisTick();
        static const bool sEagerFbMirror = [] {
            const char* e = getenv("GDX_EAGER_FBMIRROR");
            return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
        }();
        const size_t mirroredBytes =
            static_cast<size_t>(framebuffer->width) * framebuffer->height * sizeof(uint16_t);
        if (sEagerFbMirror || gdxCaptureThisTick) {
            const int hostFramebuffer = interp->mRendersToFb ? interp->mGameFb : 0;
            interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(
                hostFramebuffer, framebuffer->width, framebuffer->height,
                reinterpret_cast<uint16_t*>(framebuffer->address));
            framebuffer->valid = true;
            RecordHostWrite(framebuffer->address, mirroredBytes);
        }
        const bool isFinalTarget = (targetAddress == finalColorImage);
        if (isFinalTarget) {
            gLastRenderedFramebuffer = framebuffer->address;
        }

        // One-shot-per-buffer diagnostic: confirms whether/when the mirror
        // actually fires for each registered N64 framebuffer, and whether it
        // was the task's final CIMG or a mid-task canvas target.
        static bool sMirrorLogged[8] = {};
        const size_t fbIndex = static_cast<size_t>(framebuffer - gN64Framebuffers.begin());
        if (fbIndex < 8 && !sMirrorLogged[fbIndex]) {
            sMirrorLogged[fbIndex] = true;
            gdx_port_logf("[fbmirror] fired fb=%zu addr=%p bytes=%zu final=%d\n", fbIndex,
                          reinterpret_cast<void*>(framebuffer->address), mirroredBytes,
                          static_cast<int>(isFinalTarget));
        }
    }

    // Close POST here, at the last STATEMENT, rather than leaving it to the scope guard. The guard
    // is declared before `adapter`, so by C++ destruction order the adapter is torn down FIRST and
    // its cost would land inside POST. Ending here excludes destructors, which makes the summary's
    // (bridge - post) residual read as exactly the teardown cost of this function's locals -- the
    // N64DisplayListAdapter holds the whole converted display list (an unordered_map of vectors,
    // plus the per-matrix scratch records), and it is built and freed once per 60 Hz tick.
    // A residual near zero exonerates teardown; a large one names it.
    gdx_perf_sub_end(GDX_PERF_SUB_FBMIRROR);
    gdx_perf_sub_end(GDX_PERF_SUB_POST);
    gdxPostTimerOpen = false;
}

/* Instrumentation: log what the transition readback actually
   hands the game (dimensions, which source path fed it, offset of the first
   nonzero pixel) and dump the FIRST capture to transition-capture.bmp
   (RGBA5551 -> 24bpp, bottom-up) next to the exe, so the next soak shows
   exactly what the game receives instead of guessing. */
static void LogAndDumpTransitionCapture(const uint16_t* pixels, unsigned int width,
                                        unsigned int height, const char* sourcePath) {
    // Budget split: 8 was consumed entirely by boot-phase
    // captures (VI-fallback frames + the title/logo transitions), so the
    // menu-transition captures a soak actually cares about never got logged.
    // Raised so later, more interesting captures still show up.
    // The one-shot BMP dump is instrumentation. Gate it (and the
    // expensive per-pixel row conversion + file write it needs) behind GDX_DIAG_TRANSITION_DUMP
    // so a normal play session never writes transition-capture.bmp to disk. The cheap log line
    // stays (it only reaches a file when the opt-in log sink is enabled — see port_log.h), and
    // its "dump=" suffix now only advertises the dump when the dump is actually going to happen.
    const bool sDumpEnabled = gdx_dev_gate(GDX_GATE_TRANSITION_DUMP) != 0;
    static int sCaptureCount = 0;
    static bool sDumped = false;
    if (pixels == nullptr || sCaptureCount >= 24) {
        return;
    }
    ++sCaptureCount;

    const size_t pixelCount = static_cast<size_t>(width) * height;
    long long firstNonZero = -1;
    for (size_t i = 0; i < pixelCount; i++) {
        if (pixels[i] != 0) {
            firstNonZero = static_cast<long long>(i);
            break;
        }
    }
    const bool willDump = sDumpEnabled && !sDumped;
    gdx_port_logf("[transition] capture #%d %ux%u source=%s mode=%d firstNonZeroPx=%lld%s\n",
                  sCaptureCount, width, height, sourcePath, (gGameMode & 0x1F), firstNonZero,
                  willDump ? " dump=transition-capture.bmp" : "");

    if (!sDumpEnabled || sDumped) {
        return;
    }
    sDumped = true;

    std::FILE* f = std::fopen("transition-capture.bmp", "wb");
    if (f == nullptr) {
        gdx_port_logf("[transition] BMP dump failed: fopen errno path\n");
        return;
    }
    const unsigned int rowBytes = (width * 3u + 3u) & ~3u;
    const unsigned int imageBytes = rowBytes * height;
    const unsigned int fileBytes = 14u + 40u + imageBytes;
    unsigned char header[54] = { 0 };
    header[0] = 'B'; header[1] = 'M';
    header[2] = static_cast<unsigned char>(fileBytes);
    header[3] = static_cast<unsigned char>(fileBytes >> 8);
    header[4] = static_cast<unsigned char>(fileBytes >> 16);
    header[5] = static_cast<unsigned char>(fileBytes >> 24);
    header[10] = 54; // pixel data offset
    header[14] = 40; // BITMAPINFOHEADER size
    header[18] = static_cast<unsigned char>(width);
    header[19] = static_cast<unsigned char>(width >> 8);
    header[20] = static_cast<unsigned char>(width >> 16);
    header[21] = static_cast<unsigned char>(width >> 24);
    header[22] = static_cast<unsigned char>(height);
    header[23] = static_cast<unsigned char>(height >> 8);
    header[24] = static_cast<unsigned char>(height >> 16);
    header[25] = static_cast<unsigned char>(height >> 24);
    header[26] = 1;  // planes
    header[28] = 24; // bpp
    std::fwrite(header, 1, sizeof(header), f);

    std::vector<unsigned char> row(rowBytes, 0);
    for (unsigned int y = 0; y < height; y++) {
        // BMP rows are bottom-up; N64 framebuffer is top-down.
        const uint16_t* src = pixels + static_cast<size_t>(height - 1 - y) * width;
        for (unsigned int x = 0; x < width; x++) {
            // N64 RGBA5551: RRRRRGGG GGBBBBBA (big-endian u16 already decoded to
            // host order by the readback/mirror path).
            const uint16_t p = src[x];
            const unsigned char r5 = (p >> 11) & 0x1F;
            const unsigned char g5 = (p >> 6) & 0x1F;
            const unsigned char b5 = (p >> 1) & 0x1F;
            row[x * 3 + 2] = static_cast<unsigned char>((r5 << 3) | (r5 >> 2));
            row[x * 3 + 1] = static_cast<unsigned char>((g5 << 3) | (g5 >> 2));
            row[x * 3 + 0] = static_cast<unsigned char>((b5 << 3) | (b5 >> 2));
        }
        std::fwrite(row.data(), 1, rowBytes, f);
    }
    std::fclose(f);
    gdx_port_logf("[transition] first capture dumped to transition-capture.bmp\n");
}

/* Fade-transition garbled horizontal-stripe band probe.
 *
 * The stock [transition] line only reports firstNonZeroPx, which is nearly useless:
 * `firstNonZero` is initialised to -1 and set to the first index whose pixel != 0, so
 * firstNonZeroPx=0 means "pixel 0 is non-zero" (the capture HAS content), NOT "no
 * non-zero pixels" (that would print -1). It cannot tell a valid title image from a
 * stride-scrambled or wrong-aspect one.
 *
 * This env-gated probe (GDX_DIAG_CAPTURE_PROBE) emits conviction-grade layout evidence
 * on the already-downscaled 320x240 capture so an attract-mode run pinpoints
 * the failure mode WITHOUT guessing:
 *  - source render dims + aspect: the mirror is a resizable FB tracking mCurDimensions,
 *    read back with a nearest-neighbour downscale to 320x240. At a widescreen source
 *    (aspect > ~1.4) that downscale squeezes the full frame into 320 columns -> a
 *    horizontally-compressed (but still recognisable) title image, the leading
 *    "the band IS the title screen" hypothesis. A ~1.333 aspect rules that out.
 *  - shear signature: for the first rows, the column of the first non-zero pixel. A
 *    genuine stride mismatch (rows read at the wrong pitch) makes that column DRIFT by
 *    a roughly constant delta per row -> the diagonal "horizontal dashes" look. A stable
 *    or content-driven column rules stride shear out.
 *  - a rolling checksum: identical across two consecutive captures => the mirror is
 *    STALE (frozen wrong frame), a different failure than a live-but-mislaid capture.
 * Zero cost unless the env var is set (single cached bool). Diagnostic only. */
static void GdxDiagCaptureContentProbe(const uint16_t* px, unsigned int width, unsigned int height, int srcW,
                                       int srcH, const char* sourcePath) {
    if (!gdx_dev_gate(GDX_GATE_CAPTURE_PROBE) || px == nullptr || width == 0 || height == 0) {
        return;
    }
    static int sCount = 0;
    if (sCount >= 48) {
        return;
    }
    ++sCount;

    const size_t pixelCount = static_cast<size_t>(width) * height;
    size_t nonZero = 0;
    uint16_t vMin = 0xFFFF, vMax = 0;
    uint64_t fnv = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < pixelCount; i++) {
        const uint16_t p = px[i];
        fnv = (fnv ^ p) * 0x100000001b3ull;
        if (p != 0) {
            ++nonZero;
            if (p < vMin) vMin = p;
            if (p > vMax) vMax = p;
        }
    }
    if (nonZero == 0) {
        vMin = 0;
    }

    static uint64_t sPrevFnv = 0;
    const bool stale = (sCount > 1 && fnv == sPrevFnv);
    sPrevFnv = fnv;

    // First-non-zero column for the first rows: a constant per-row drift == stride shear.
    char shear[128];
    int off = 0;
    off += std::snprintf(shear + off, sizeof(shear) - off, "firstNZcol[");
    const unsigned probeRows = height < 8 ? height : 8;
    for (unsigned y = 0; y < probeRows; y++) {
        int col = -1;
        const uint16_t* row = px + static_cast<size_t>(y) * width;
        for (unsigned x = 0; x < width; x++) {
            if (row[x] != 0) {
                col = static_cast<int>(x);
                break;
            }
        }
        // snprintf returns the WOULD-HAVE-WRITTEN length; clamp the accumulator so a future
        // larger probeRows/column width can never push `off` past the buffer and feed a
        // wrapped unsigned size into the next call (judge hardening finding).
        off += std::snprintf(shear + off, sizeof(shear) - off, "%s%d", y ? "," : "", col);
        if (off >= static_cast<int>(sizeof(shear)) - 1) {
            off = static_cast<int>(sizeof(shear)) - 1;
            break;
        }
    }
    std::snprintf(shear + off, sizeof(shear) - off, "]");

    const double srcAspect = (srcH > 0) ? static_cast<double>(srcW) / srcH : 0.0;
    const bool wideSrc = srcAspect > 1.4;
    gdx_port_logf("[capture-probe] #%d %ux%u src=%dx%d aspect=%.3f%s nonzero=%.1f%% range=[%u..%u] "
                  "fnv=%016llx%s %s src_path=%s\n",
                  sCount, width, height, srcW, srcH, srcAspect,
                  wideSrc ? "(WIDE:downscale-squeezes-horizontally)" : "",
                  100.0 * static_cast<double>(nonZero) / static_cast<double>(pixelCount), vMin, vMax,
                  static_cast<unsigned long long>(fnv), stale ? " STALE(==prev)" : "", shear, sourcePath);
}

extern "C" int gdx_read_current_framebuffer(void* rgba16Buffer, unsigned int width, unsigned int height) {
    if (rgba16Buffer == nullptr || width == 0 || height == 0) {
        return 0;
    }

    auto wnd = Ship::Context::GetInstance()->GetWindow();
    auto* fw = static_cast<Fast::Fast3dWindow*>(wnd.get());
    if (fw == nullptr) {
        return 0;
    }

    auto interp = fw->GetInterpreterWeak().lock();
    if (!interp) {
        return 0;
    }

    const uintptr_t requestedAddress = reinterpret_cast<uintptr_t>(rgba16Buffer);
    auto requested = std::find_if(gN64Framebuffers.begin(), gN64Framebuffers.end(),
                                  [requestedAddress](const N64FramebufferInfo& info) {
                                      return info.address == requestedAddress;
                                  });
    // The persistent frame mirror IS the last completed frame — exactly what a
    // transition snapshot wants — so when it exists it is authoritative and the
    // "preserve this buffer's historical contents" early-return must not fire:
    // that path previously served bytes poisoned by an earlier all-zero boot
    // capture (requested->valid was set even though the readback produced
    // nothing), which kept transitions black forever after.
    const bool mirrorAvailable = (gFrameMirrorFb >= 0 && gFrameMirrorValid);
    if (!mirrorAvailable && requested != gN64Framebuffers.end() && requested->valid &&
        requestedAddress != gLastRenderedFramebuffer) {
        LogAndDumpTransitionCapture(static_cast<const uint16_t*>(rgba16Buffer), width, height,
                                    "preserved-mirror");
        return 1;
    }

    interp->Flush();
    const auto hasContent = [](const uint16_t* px, unsigned int w, unsigned int h) {
        const size_t count = static_cast<size_t>(w) * h;
        for (size_t k = 0; k < count; k++) {
            if (px[k] != 0) {
                return true;
            }
        }
        return false;
    };
    // Prefer the persistent frame mirror (a real framebuffer with its own
    // texture, copied GPU->GPU before present). Reading fb 0 after present is
    // undefined on DX11 flip-model swapchains. If the mirror somehow has no
    // content yet (nothing presented), fall back to the direct read rather
    // than serving zeros.
    const char* sourcePath = "none";
    uint16_t* out = static_cast<uint16_t*>(rgba16Buffer);
    bool gotContent = false;
    if (mirrorAvailable) {
        interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(gFrameMirrorFb, width, height, out);
        sourcePath = "frame-mirror";
        gotContent = hasContent(out, width, height);
    }
    if (!gotContent) {
        const int direct = interp->mRendersToFb ? interp->mGameFb : 0;
        interp->GetCurrentRenderingAPI()->ReadFramebufferToCPU(direct, width, height, out);
        sourcePath = interp->mRendersToFb ? "host-fb-game" : "host-fb-0";
        gotContent = hasContent(out, width, height);
    }
    // Only publish this buffer as a valid frame source when it actually holds
    // pixels — an all-zero capture must never poison the preserved path or the
    // dirty-range tracking.
    if (gotContent && requested != gN64Framebuffers.end()) {
        requested->valid = true;
        gLastRenderedFramebuffer = requestedAddress;
        RecordHostWrite(requestedAddress,
                        static_cast<size_t>(width) * height * sizeof(uint16_t));
    }
    LogAndDumpTransitionCapture(static_cast<const uint16_t*>(rgba16Buffer), width, height, sourcePath);
    // Issue C conviction probe: report the source render dims/aspect and a layout
    // fingerprint of the downscaled capture (env-gated, GDX_DIAG_CAPTURE_PROBE). The
    // captured mirror is read from mCurDimensions -> 320x240 nearest-neighbour, so a
    // widescreen source is the prime suspect for the "title-screen band"; the probe's
    // aspect + shear + staleness stats convict the exact failure on the next soak.
    GdxDiagCaptureContentProbe(out, width, height, static_cast<int>(interp->mCurDimensions.width),
                               static_cast<int>(interp->mCurDimensions.height), sourcePath);
    return 1;
}
