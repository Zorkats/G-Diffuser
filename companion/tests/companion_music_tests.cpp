// Copyright (c) Zorkats
//
// Headless tests for the Music workspace: loop rewrites read back through the game's own decoders (Ogg
// rewrites are checked page by page, and through the decoder when GDX_TEST_OGG names a real Ogg Vorbis), the
// staged install with its .previous backups, rollback and restore, course song names, the native WAV
// writer and the background task. Recordings are synthesized here; no game audio is used. Every file is
// made in a fresh temporary folder. Links gdx_companion_host only.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "decoders.h"
#include "gdx_companion_loudness.h"
#include "gdx_companion_music.h"
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

bool Contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void Le16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void Le32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void Tag(std::vector<uint8_t>& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

int16_t Tone(uint32_t frame, uint32_t channel) {
    return static_cast<int16_t>(std::lround(8000.0 * std::sin(0.05 * frame + channel)));
}

// 16-bit PCM WAV with a LIST chunk after data, so the rewrite has a chunk to keep.
std::vector<uint8_t> MakeWav(uint32_t rate, uint32_t channels, uint32_t frames) {
    std::vector<uint8_t> out;
    Tag(out, "RIFF");
    Le32(out, 0);
    Tag(out, "WAVE");
    Tag(out, "fmt ");
    Le32(out, 16);
    Le16(out, 1);
    Le16(out, channels);
    Le32(out, rate);
    Le32(out, rate * channels * 2);
    Le16(out, channels * 2);
    Le16(out, 16);
    Tag(out, "data");
    Le32(out, frames * channels * 2);
    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < channels; ++c) Le16(out, static_cast<uint16_t>(Tone(f, c)));
    }
    Tag(out, "LIST");
    Le32(out, 5);
    out.insert(out.end(), {'I', 'N', 'F', 'O', 'x', 0});  // odd length, padded
    const uint32_t riff = static_cast<uint32_t>(out.size() - 8);
    for (int i = 0; i < 4; ++i) out[4 + static_cast<size_t>(i)] = static_cast<uint8_t>(riff >> (8 * i));
    return out;
}

uint8_t Crc8(const std::vector<uint8_t>& data, size_t from, size_t to) {
    uint8_t crc = 0;
    for (size_t i = from; i < to; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

uint16_t Crc16(const std::vector<uint8_t>& data, size_t from, size_t to) {
    uint16_t crc = 0;
    for (size_t i = from; i < to; ++i) {
        crc ^= static_cast<uint16_t>(data[i] << 8);
        for (int k = 0; k < 8; ++k) crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}

void Block(std::vector<uint8_t>& out, uint8_t type, bool last, const std::vector<uint8_t>& body) {
    out.push_back(static_cast<uint8_t>((last ? 0x80 : 0) | type));
    out.push_back(static_cast<uint8_t>(body.size() >> 16));
    out.push_back(static_cast<uint8_t>(body.size() >> 8));
    out.push_back(static_cast<uint8_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

// A mono 16-bit 44100 Hz FLAC with one verbatim frame, a comment block holding comments, and padding.
std::vector<uint8_t> MakeFlac(uint32_t frames, const std::vector<std::string>& comments) {
    const uint32_t rate = 44100;
    std::vector<uint8_t> out = {'f', 'L', 'a', 'C'};
    std::vector<uint8_t> info;
    info.push_back(static_cast<uint8_t>(frames >> 8));  // min block size
    info.push_back(static_cast<uint8_t>(frames));
    info.push_back(static_cast<uint8_t>(frames >> 8));  // max block size
    info.push_back(static_cast<uint8_t>(frames));
    info.insert(info.end(), 6, 0);  // frame sizes unknown
    // 20 bits rate, 3 bits channels - 1, 5 bits bits - 1, 36 bits total samples.
    const uint64_t packed = (static_cast<uint64_t>(rate) << 44) | (0ull << 41) | (15ull << 36) | frames;
    for (int i = 7; i >= 0; --i) info.push_back(static_cast<uint8_t>(packed >> (8 * i)));
    info.insert(info.end(), 16, 0);  // MD5 unknown
    Block(out, 0, false, info);
    std::vector<uint8_t> vorbis;
    const std::string vendor = "test";
    Le32(vorbis, static_cast<uint32_t>(vendor.size()));
    vorbis.insert(vorbis.end(), vendor.begin(), vendor.end());
    Le32(vorbis, static_cast<uint32_t>(comments.size()));
    for (const std::string& comment : comments) {
        Le32(vorbis, static_cast<uint32_t>(comment.size()));
        vorbis.insert(vorbis.end(), comment.begin(), comment.end());
    }
    Block(out, 4, false, vorbis);
    Block(out, 1, true, std::vector<uint8_t>(64, 0));

    const size_t frame = out.size();
    out.push_back(0xFF);
    out.push_back(0xF8);
    out.push_back(0x79);  // block size: 16-bit value at the end; rate 44.1 kHz
    out.push_back(0x08);  // mono, 16 bits
    out.push_back(0x00);  // frame 0
    out.push_back(static_cast<uint8_t>((frames - 1) >> 8));
    out.push_back(static_cast<uint8_t>(frames - 1));
    out.push_back(Crc8(out, frame, out.size()));
    out.push_back(0x02);  // verbatim subframe
    for (uint32_t f = 0; f < frames; ++f) {
        const uint16_t sample = static_cast<uint16_t>(Tone(f, 0));
        out.push_back(static_cast<uint8_t>(sample >> 8));
        out.push_back(static_cast<uint8_t>(sample));
    }
    const uint16_t crc = Crc16(out, frame, out.size());
    out.push_back(static_cast<uint8_t>(crc >> 8));
    out.push_back(static_cast<uint8_t>(crc));
    return out;
}

void Slots() {
    const auto& slots = MusicSlots();
    bool ids = slots.size() == 30;
    for (size_t i = 0; ids && i < slots.size(); ++i) ids = slots[i].id == static_cast<int>(i);
    Check(ids, "30 slots in game order");
    Check(std::string(slots[14].stem) == "bgm_title" && slots[20].streams && !slots[21].streams && !slots[29].streams,
          "slots 0 to 20 stream, 21 to 29 do not");
}

void WavLoops(const fs::path& dir) {
    const fs::path source = dir / "tone.wav";
    WriteBytes(source, MakeWav(44100, 2, 1000));
    AudioInfo info;
    std::string error;
    Check(InspectAudio(source, info, error) && info.format == AudioFormat::Wav && info.rate == 44100 &&
              info.channels == 2 && info.frames == 1000 && !info.has_loop,
          "a WAV opens with the game's decoder");

    std::vector<uint8_t> in(MakeWav(44100, 2, 1000));
    std::vector<uint8_t> out;
    LoopChoice custom{LoopPolicy::Custom, 100, 900};
    Check(RewriteWavLoop(in, custom, 44100, out, error), "a WAV loop is written");
    const fs::path looped = dir / "looped.wav";
    WriteBytes(looped, out);
    AudioInfo back;
    Check(InspectAudio(looped, back, error) && back.has_loop && back.loop_start == 100 && back.loop_end == 900 &&
              back.frames == 1000,
          "the decoder reads the loop back as [100, 900)");
    Check(std::search(out.begin(), out.end(), std::begin("LIST"), std::begin("LIST") + 4) != out.end(),
          "other chunks are kept");

    std::vector<uint8_t> again;
    LoopChoice second{LoopPolicy::Custom, 10, 20};
    Check(RewriteWavLoop(out, second, 44100, again, error), "a looped WAV is rewritten");
    WriteBytes(looped, again);
    size_t smpl = 0;
    for (size_t i = 0; i + 4 <= again.size(); ++i) smpl += std::memcmp(again.data() + i, "smpl", 4) == 0 ? 1 : 0;
    Check(InspectAudio(looped, back, error) && back.loop_start == 10 && back.loop_end == 20 && smpl == 1,
          "the old smpl chunk is replaced, not added to");

    std::vector<uint8_t> whole;
    Check(RewriteWavLoop(again, LoopChoice{LoopPolicy::Whole, 0, 0}, 44100, whole, error), "Whole removes the loop");
    WriteBytes(looped, whole);
    Check(InspectAudio(looped, back, error) && !back.has_loop && back.loop_start == 0 && back.loop_end == 1000,
          "without tags the whole recording repeats");
    Check(!RewriteWavLoop(std::vector<uint8_t>(20, 0), custom, 44100, out, error), "a non-WAV is refused");
}

void FlacLoops(const fs::path& dir) {
    const fs::path source = dir / "tone.flac";
    WriteBytes(source, MakeFlac(1000, {"TITLE=Tone", "LOOPSTART=5", "LOOPLENGTH=10"}));
    AudioInfo info;
    std::string error;
    Check(InspectAudio(source, info, error) && info.format == AudioFormat::Flac && info.frames == 1000 &&
              info.has_loop && info.loop_start == 5 && info.loop_end == 15,
          "a FLAC with loop comments opens");

    const std::vector<uint8_t> in = MakeFlac(1000, {"TITLE=Tone", "LOOPSTART=5", "LOOPLENGTH=10"});
    std::vector<uint8_t> out;
    Check(RewriteFlacLoop(in, LoopChoice{LoopPolicy::Custom, 200, 800}, out, error), "a FLAC loop is written");
    const fs::path looped = dir / "looped.flac";
    WriteBytes(looped, out);
    AudioInfo back;
    Check(InspectAudio(looped, back, error) && back.has_loop && back.loop_start == 200 && back.loop_end == 800 &&
              back.frames == 1000,
          "the decoder reads the new loop as [200, 800)");
    const std::string text(out.begin(), out.end());
    Check(Contains(text, "TITLE=Tone") && !Contains(text, "LOOPSTART=5") && out.size() < in.size() + 64,
          "other comments stay, old loop comments and padding go");

    Check(RewriteFlacLoop(in, LoopChoice{LoopPolicy::Whole, 0, 0}, out, error), "Whole removes the FLAC loop");
    WriteBytes(looped, out);
    Check(InspectAudio(looped, back, error) && !back.has_loop && back.loop_end == 1000, "the whole FLAC repeats");

    const std::vector<uint8_t> bare = MakeFlac(1000, {});
    Check(RewriteFlacLoop(bare, LoopChoice{LoopPolicy::Custom, 1, 2}, out, error), "a FLAC without comments gets a loop");
    WriteBytes(looped, out);
    Check(InspectAudio(looped, back, error) && back.loop_start == 1 && back.loop_end == 2, "its loop reads back");
    Check(!RewriteFlacLoop(std::vector<uint8_t>(8, 0), LoopChoice{}, out, error), "a non-FLAC is refused");
}

// The Ogg page checksum, bit by bit, written apart from the table version under test.
uint32_t OggCrcBits(const std::vector<uint8_t>& page) {
    uint32_t crc = 0;
    for (const uint8_t byte : page) {
        crc ^= static_cast<uint32_t>(byte) << 24;
        for (int k = 0; k < 8; ++k) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

struct OggOut {
    std::vector<uint8_t> bytes;
    uint32_t sequence = 0;
};

// Laces packets into pages of up to 255 segments. granule goes on pages where a packet ends.
void OggPages(OggOut& out, const std::vector<std::vector<uint8_t>>& packets, uint8_t first_flags, uint64_t granule,
              bool eos, uint32_t serial = 0x1234) {
    std::vector<std::pair<const uint8_t*, uint8_t>> segments;
    std::vector<bool> ends;
    for (const std::vector<uint8_t>& packet : packets) {
        size_t left = packet.size();
        const uint8_t* data = packet.data();
        for (;;) {
            const uint8_t length = static_cast<uint8_t>(std::min<size_t>(left, 255));
            segments.push_back({data, length});
            ends.push_back(length < 255);
            data += length;
            left -= length;
            if (length < 255) break;
        }
    }
    bool continued = false;
    for (size_t first = 0; first < segments.size(); first += 255) {
        const size_t last = std::min(segments.size(), first + 255);
        bool any_end = false;
        for (size_t i = first; i < last; ++i) any_end = any_end || ends[i];
        uint8_t flags = continued ? 0x01 : 0x00;
        if (first == 0) flags |= first_flags;
        if (last == segments.size() && eos) flags |= 0x04;
        std::vector<uint8_t> page = {'O', 'g', 'g', 'S', 0, flags};
        const uint64_t g = any_end ? granule : ~0ull;
        for (int i = 0; i < 8; ++i) page.push_back(static_cast<uint8_t>(g >> (8 * i)));
        Le32(page, serial);
        Le32(page, out.sequence++);
        Le32(page, 0);
        page.push_back(static_cast<uint8_t>(last - first));
        for (size_t i = first; i < last; ++i) page.push_back(segments[i].second);
        for (size_t i = first; i < last; ++i) page.insert(page.end(), segments[i].first, segments[i].first + segments[i].second);
        const uint32_t crc = OggCrcBits(page);
        for (int i = 0; i < 4; ++i) page[22 + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
        out.bytes.insert(out.bytes.end(), page.begin(), page.end());
        continued = !ends[last - 1];
    }
}

std::vector<uint8_t> VorbisComment(const std::vector<std::string>& comments) {
    std::vector<uint8_t> out = {0x03, 'v', 'o', 'r', 'b', 'i', 's'};
    const std::string vendor = "test";
    Le32(out, static_cast<uint32_t>(vendor.size()));
    out.insert(out.end(), vendor.begin(), vendor.end());
    Le32(out, static_cast<uint32_t>(comments.size()));
    for (const std::string& comment : comments) {
        Le32(out, static_cast<uint32_t>(comment.size()));
        out.insert(out.end(), comment.begin(), comment.end());
    }
    out.push_back(0x01);
    return out;
}

std::vector<uint8_t> Filler(size_t size, uint8_t seed) {
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; ++i) out[i] = static_cast<uint8_t>(seed + i * 31u + (i >> 7));
    return out;
}

struct FakeOgg {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> ident;
    std::vector<uint8_t> setup;
    std::vector<std::vector<uint8_t>> audio;  // two packets on each of three pages
};

// The page layout of an Ogg Vorbis file with a setup header larger than one page, so the headers span
// several pages, and three audio pages. The packet contents are not real Vorbis, which the rewrite
// does not need.
FakeOgg MakeFakeOgg(const std::vector<std::string>& comments, uint32_t second_serial = 0x1234) {
    FakeOgg ogg;
    ogg.ident = Filler(30, 1);
    ogg.ident[0] = 0x01;
    std::memcpy(ogg.ident.data() + 1, "vorbis", 6);
    ogg.setup = Filler(70000, 5);
    ogg.setup[0] = 0x05;
    std::memcpy(ogg.setup.data() + 1, "vorbis", 6);
    OggOut out;
    OggPages(out, {ogg.ident}, 0x02, 0, false);
    OggPages(out, {VorbisComment(comments), ogg.setup}, 0, 0, false);
    for (int page = 0; page < 3; ++page) {
        ogg.audio.push_back(Filler(300 + page * 10, static_cast<uint8_t>(page)));
        ogg.audio.push_back(Filler(40, static_cast<uint8_t>(page + 9)));
        OggPages(out, {ogg.audio[ogg.audio.size() - 2], ogg.audio.back()}, 0, 1000u * (page + 1), page == 2,
                 page == 2 ? second_serial : 0x1234);
    }
    ogg.bytes = out.bytes;
    return ogg;
}

struct ParsedOgg {
    bool ok = true;  // every page well formed with a good checksum, one serial, sequence 0, 1, 2...
    std::vector<std::vector<uint8_t>> packets;
    std::vector<size_t> packet_page;  // the page each packet ends on
    std::vector<bool> packet_ends_page;
    std::vector<uint64_t> granules;
    std::vector<uint8_t> flags;
};

ParsedOgg ParseOgg(const std::vector<uint8_t>& in) {
    ParsedOgg parsed;
    size_t pos = 0;
    uint32_t serial = 0;
    std::vector<uint8_t> packet;
    while (pos < in.size() && parsed.ok) {
        if (in.size() - pos < 27 || std::memcmp(in.data() + pos, "OggS", 4) != 0) {
            parsed.ok = false;
            break;
        }
        const size_t segments = in[pos + 26];
        size_t body = 0;
        for (size_t i = 0; i < segments; ++i) body += in[pos + 27 + i];
        const size_t size = 27 + segments + body;
        std::vector<uint8_t> page(in.begin() + static_cast<std::ptrdiff_t>(pos),
                                  in.begin() + static_cast<std::ptrdiff_t>(pos + size));
        const uint32_t stored = static_cast<uint32_t>(page[22]) | (static_cast<uint32_t>(page[23]) << 8) |
                                (static_cast<uint32_t>(page[24]) << 16) | (static_cast<uint32_t>(page[25]) << 24);
        for (int i = 22; i < 26; ++i) page[static_cast<size_t>(i)] = 0;
        const uint32_t page_serial = static_cast<uint32_t>(page[14]) | (static_cast<uint32_t>(page[15]) << 8) |
                                     (static_cast<uint32_t>(page[16]) << 16) | (static_cast<uint32_t>(page[17]) << 24);
        const uint32_t sequence = static_cast<uint32_t>(page[18]) | (static_cast<uint32_t>(page[19]) << 8) |
                                  (static_cast<uint32_t>(page[20]) << 16) | (static_cast<uint32_t>(page[21]) << 24);
        if (parsed.flags.empty()) serial = page_serial;
        parsed.ok = parsed.ok && OggCrcBits(page) == stored && page_serial == serial && sequence == parsed.flags.size();
        uint64_t granule = 0;
        for (int i = 7; i >= 0; --i) granule = (granule << 8) | page[6 + static_cast<size_t>(i)];
        parsed.granules.push_back(granule);
        parsed.flags.push_back(page[5]);
        size_t at = 27 + segments;
        for (size_t i = 0; i < segments; ++i) {
            const uint8_t length = page[27 + i];
            packet.insert(packet.end(), page.begin() + static_cast<std::ptrdiff_t>(at),
                          page.begin() + static_cast<std::ptrdiff_t>(at + length));
            at += length;
            if (length < 255) {
                parsed.packets.push_back(packet);
                parsed.packet_page.push_back(parsed.flags.size() - 1);
                parsed.packet_ends_page.push_back(i + 1 == segments);
                packet.clear();
            }
        }
        pos += size;
    }
    return parsed;
}

void OggLoops(const fs::path& dir) {
    const FakeOgg ogg = MakeFakeOgg({"TITLE=Tone", "LOOPSTART=5", "LOOPLENGTH=10"});
    const ParsedOgg before = ParseOgg(ogg.bytes);
    Check(before.ok && before.packets.size() == 9 && before.flags.size() > 5,
          "the test stream is well formed, with headers on several pages");

    std::vector<uint8_t> out;
    std::string error;
    Check(RewriteOggLoop(ogg.bytes, LoopChoice{LoopPolicy::Custom, 100, 900}, out, error), "an Ogg loop is written");
    const ParsedOgg after = ParseOgg(out);
    Check(after.ok, "every page has a good checksum and the page sequence has no gap");
    Check(after.packets.size() == 9 && after.packets[0] == ogg.ident && after.packets[2] == ogg.setup,
          "the identification and setup headers are kept byte for byte");
    bool audio = after.packets.size() == 9;
    for (size_t i = 0; audio && i < ogg.audio.size(); ++i) audio = after.packets[3 + i] == ogg.audio[i];
    Check(audio, "every audio packet is kept byte for byte");
    const std::string comment = after.packets.size() > 1 ? std::string(after.packets[1].begin(), after.packets[1].end())
                                                         : std::string();
    Check(Contains(comment, "TITLE=Tone") && Contains(comment, "LOOPSTART=100") && Contains(comment, "LOOPLENGTH=800") &&
              !Contains(comment, "LOOPSTART=5") && !Contains(comment, "LOOPLENGTH=10") && comment.back() == '\x01',
          "the comment header holds the new loop, the other comments and the framing bit");
    Check(after.packet_page.size() == 9 && after.packet_page[0] == 0 && after.packet_ends_page[0] &&
              after.packet_ends_page[2] && after.packet_page[3] == after.packet_page[2] + 1,
          "the identification header is alone on page 0 and the audio starts on a fresh page");
    const size_t last = after.flags.size() - 1;
    Check(after.granules[last] == 3000 && after.granules[last - 1] == 2000 && (after.flags[last] & 0x04) != 0 &&
              (after.flags[0] & 0x02) != 0,
          "audio granules and the first and last page flags are kept");

    std::vector<uint8_t> whole;
    Check(RewriteOggLoop(ogg.bytes, LoopChoice{LoopPolicy::Whole, 0, 0}, whole, error), "Whole rewrites an Ogg");
    const ParsedOgg cleared = ParseOgg(whole);
    const std::string cleared_comment = cleared.packets.size() > 1
                                            ? std::string(cleared.packets[1].begin(), cleared.packets[1].end())
                                            : std::string();
    Check(cleared.ok && Contains(cleared_comment, "TITLE=Tone") && !Contains(cleared_comment, "LOOP"),
          "Whole removes the Ogg loop comments");

    std::vector<uint8_t> damaged = ogg.bytes;
    damaged[damaged.size() - 5] ^= 0x40u;
    Check(!RewriteOggLoop(damaged, LoopChoice{LoopPolicy::Custom, 1, 2}, out, error) && Contains(error, "checksum"),
          "a page with a bad checksum is refused");
    const FakeOgg chained = MakeFakeOgg({}, 0x9999);
    Check(!RewriteOggLoop(chained.bytes, LoopChoice{LoopPolicy::Custom, 1, 2}, out, error) &&
              Contains(error, "more than one stream"),
          "a file with two streams is refused");
    Check(!RewriteOggLoop(std::vector<uint8_t>(40, 0), LoopChoice{}, out, error), "a non-Ogg is refused");
    Check(!RewriteOggLoop(ogg.bytes, LoopChoice{LoopPolicy::Custom, 9, 9}, out, error), "an empty loop is refused");

    // A real Ogg Vorbis through the game's decoder, when the environment names one.
    const char* real = std::getenv("GDX_TEST_OGG");
    if (real == nullptr || real[0] == '\0') {
        std::printf("SKIP: GDX_TEST_OGG is not set, so no real Ogg Vorbis goes through the decoder\n");
        return;
    }
    const fs::path source = PathFromUtf8(real);
    AudioInfo info;
    Check(InspectAudio(source, info, error) && info.format == AudioFormat::Ogg && info.frames > 1000,
          "the real Ogg opens");
    if (info.frames <= 1000) return;
    const LoopChoice choice{LoopPolicy::Custom, 123, info.frames - 77};
    const fs::path staged = dir / "staged.ogg";
    CancelFlag cancel{false};
    ProgressValue progress{0.0f};
    Check(StageMusic(source, info, choice, staged, cancel, progress, error),
          "the real Ogg is staged with a chosen loop and decodes to the end");
    AudioInfo back;
    Check(InspectAudio(staged, back, error) && back.has_loop && back.loop_start == 123 &&
              back.loop_end == info.frames - 77 && back.frames == info.frames && back.rate == info.rate,
          "the decoder reads the chosen loop from the real Ogg");
    const fs::path cleared_path = dir / "cleared.ogg";
    Check(StageMusic(staged, back, LoopChoice{LoopPolicy::Whole, 0, 0}, cleared_path, cancel, progress, error),
          "the looped Ogg is staged again with Whole");
    Check(InspectAudio(cleared_path, back, error) && !back.has_loop && back.loop_end == info.frames,
          "the decoder finds no loop after Whole");
}

void LoopRules() {
    AudioInfo ogg;
    ogg.format = AudioFormat::Ogg;
    ogg.frames = 1000;
    Check(LoopPolicyOffered(AudioFormat::Ogg, LoopPolicy::Custom) && LoopPolicyOffered(AudioFormat::Flac, LoopPolicy::Custom) &&
              LoopPolicyOffered(AudioFormat::Wav, LoopPolicy::Whole) && !LoopPolicyOffered(AudioFormat::Unknown, LoopPolicy::Keep),
          "every playable format can keep, clear or choose its loop");
    Check(CheckLoop(ogg, LoopChoice{LoopPolicy::Custom, 10, 900}).empty(), "an Ogg loop change is allowed");
    AudioInfo wav = ogg;
    wav.format = AudioFormat::Wav;
    Check(CheckLoop(wav, LoopChoice{LoopPolicy::Custom, 0, 1000}).empty(), "a loop to the last frame is fine");
    Check(!CheckLoop(wav, LoopChoice{LoopPolicy::Custom, 0, 1001}).empty(), "a loop past the end is refused");
    Check(!CheckLoop(wav, LoopChoice{LoopPolicy::Custom, 50, 50}).empty(), "an empty loop is refused");
    uint64_t start = 1;
    uint64_t end = 1;
    EffectiveLoop(wav, LoopChoice{}, start, end);
    Check(start == 0 && end == 1000, "Keep without tags repeats the whole recording");
}

void Names() {
    Check(CheckCourseSongName("My Song 2").empty(), "a plain name is allowed");
    Check(!CheckCourseSongName("bgm_title").empty() && !CheckCourseSongName("BGM_Title").empty(),
          "slot names are refused in any case");
    Check(!CheckCourseSongName("bgm_1e").empty(), "a hex slot id from 30 up is refused");
    Check(CheckCourseSongName("bgm_63").empty(), "bgm_63 (id 99, no music) is allowed");
    Check(!CheckCourseSongName("").empty() && !CheckCourseSongName(std::string(65, 'a')).empty(),
          "empty and over-64-byte names are refused");
    Check(!CheckCourseSongName("a/b").empty() && !CheckCourseSongName("a:b").empty() &&
              !CheckCourseSongName("song.").empty() && !CheckCourseSongName("CON").empty(),
          "names a file cannot hold are refused");
}

void Install(const fs::path& base) {
    const fs::path game = base / "game";
    const fs::path music = game / "music";
    fs::create_directories(music);
    fs::create_directories(game / "mods" / "music");
    WriteText(music / "bgm_title.ogg", "current ogg");
    WriteText(music / "bgm_title.ogg.previous", "older ogg");
    WriteText(music / "BGM_TITLE.flac", "current flac");
    WriteText(music / "bgm_select.ogg", "other slot");
    WriteText(game / "mods" / "music" / "bgm_title.wav", "legacy");
    const fs::path source = base / "new.wav";
    WriteBytes(source, MakeWav(44100, 2, 1000));
    AudioInfo info;
    std::string error;
    InspectAudio(source, info, error);

    const InstallPlan plan = PlanMusicInstall(game, "bgm_title", AudioFormat::Wav);
    Check(plan.destination == music / "bgm_title.wav" && plan.displaced.size() == 2 &&
              plan.replaced_backups.size() == 1 && plan.legacy.size() == 1,
          "the plan lists both same-stem files, the old backup and the legacy copy");

    const fs::path staging = MakeMusicStaging(music, error);
    Check(!staging.empty() && fs::is_directory(staging) && Contains(PathToUtf8(staging.filename()), ".gdx-companion-staging-"),
          "a staging folder is made inside music/");
    const fs::path staged = staging / "bgm_title.wav";
    CancelFlag cancel{false};
    ProgressValue progress{0.0f};
    Check(StageMusic(source, info, LoopChoice{LoopPolicy::Custom, 100, 900}, staged, cancel, progress, error) &&
              progress.load() > 0.99f,
          "the staged copy is written and verified");
    cancel.store(true);
    Check(!StageMusic(source, info, LoopChoice{}, staging / "c.wav", cancel, progress, error) && error == "cancelled",
          "a cancelled stage stops");

    // A file that appears after the plan stops the commit and puts everything back.
    WriteText(plan.destination, "surprise");
    Check(!CommitMusicInstall(plan, staged, error), "a commit that would overwrite a new file fails");
    Check(ReadText(music / "bgm_title.ogg") == "current ogg" && ReadText(music / "bgm_title.ogg.previous") == "older ogg" &&
              ReadText(music / "BGM_TITLE.flac") == "current flac" && fs::exists(staged),
          "the failed commit undid every move");
    fs::remove(plan.destination);

    Check(CommitMusicInstall(plan, staged, error), "the commit succeeds");
    AudioInfo installed;
    Check(InspectAudio(music / "bgm_title.wav", installed, error) && installed.loop_start == 100,
          "the installed copy carries the loop");
    Check(ReadText(music / "bgm_title.ogg.previous") == "current ogg" && ReadText(music / "BGM_TITLE.flac.previous") == "current flac" &&
              !fs::exists(music / "bgm_title.ogg") && !fs::exists(music / "bgm_title.ogg.previous.gdx-old"),
          "each displaced file became its .previous and the older backup is gone");
    Check(ReadText(music / "bgm_select.ogg") == "other slot" && ReadText(game / "mods" / "music" / "bgm_title.wav") == "legacy",
          "other slots and the legacy folder are untouched");
    Check(RemoveMusicStaging(staging, music) && !fs::exists(staging), "the staging folder is removed");
    Check(!RemoveMusicStaging(music, game) && fs::exists(music), "music/ itself is never removed as staging");

    const RestorePlan restore = PlanMusicRestore(game, "bgm_title");
    Check(restore.current.size() == 1 && restore.previous.size() == 2, "restore finds the install and both backups");
    Check(RestoreMusic(restore, error), "restore succeeds");
    Check(ReadText(music / "bgm_title.ogg") == "current ogg" && ReadText(music / "BGM_TITLE.flac") == "current flac" &&
              fs::exists(music / "bgm_title.wav.previous") && !fs::exists(music / "bgm_title.wav"),
          "restore swaps the install and the backups");
    Check(!RestoreMusic(PlanMusicRestore(game, "bgm_select"), error), "restore without a backup changes nothing");
    Check(ReadText(music / "bgm_select.ogg") == "other slot", "the slot without a backup is untouched");

    const InstallPlan song = PlanMusicInstall(game, "My Song", AudioFormat::Flac);
    Check(song.destination == music / "My Song.flac" && song.displaced.empty(), "a course song goes to music/");
}

void Native(const fs::path& base) {
    std::vector<NativeTarget> targets;
    std::string error;
    Check(LoadNativeTargets(GDX_TEST_MUSIC_TARGETS, targets, error) && targets.size() == 3, "three native targets load");
    if (targets.size() == 3) {
        Check(targets[0].id == "title" && targets[0].bgm_id == 14 && targets[0].cartridge.size() == 1 &&
                  targets[0].cartridge[0].rate_hz == 22050 && targets[0].cartridge[0].channel == "mono",
              "title has one mono 22050 Hz cartridge resource");
        Check(targets[2].id == "big_blue" && targets[2].cartridge.empty() && targets[2].expansion_kit.size() == 2 &&
                  targets[2].expansion_kit[1].channel == "part_1" && targets[2].expansion_kit[0].rate_hz == 32000,
              "big_blue is Expansion Kit only, in two 32000 Hz parts");
    }
    const fs::path bad = base / "bad_targets.json";
    WriteText(bad, "{\"schema_version\": 2, \"targets\": []}");
    Check(!LoadNativeTargets(bad, targets, error) && targets.empty(), "another schema is refused");

    Check(ResampledFrames(44100, 44100, 22050) == 22050 && ResampledFrames(3, 44100, 44100) == 3 &&
              ResampledFrames(1, 3, 2) == 1,
          "resampled length rounds like the sample packer");
    Check(NativeBytes(0) == 0 && NativeBytes(16) == 9 && NativeBytes(17) == 18, "native memory is 9 bytes per 16");
    Check(kNativeBudgetBytes == 1048576, "all native packs share 1 MiB");

    const fs::path source = base / "native_source.wav";
    WriteBytes(source, MakeWav(44100, 2, 1000));
    AudioInfo info;
    InspectAudio(source, info, error);
    CancelFlag cancel{false};
    ProgressValue progress{0.0f};
    const fs::path wav = base / "SAMPLE_X.wav";
    const fs::path loop = base / "SAMPLE_X.loop.json";
    Check(WriteNativeWav(source, info, 22050, LoopChoice{LoopPolicy::Custom, 100, 900}, wav, loop, cancel, progress,
                         error),
          "a native WAV is written");
    AudioInfo out;
    Check(InspectAudio(wav, out, error) && out.rate == 22050 && out.channels == 1 && out.frames == 500,
          "it is mono at the target rate, resampled to 500 frames");
    Check(ReadText(loop) == "{\"start\": 48, \"end\": 450}\n", "the loop start is rounded down to 16");
    Check(WriteNativeWav(source, info, 22050, LoopChoice{}, wav, loop, cancel, progress, error) &&
              ReadText(loop) == "{\"start\": 0, \"end\": 500}\n",
          "Keep without tags loops the whole sample");
}

void Task() {
    BackgroundTask task;
    Check(task.Poll() == BackgroundTask::State::Idle, "a new task is idle");
    Check(task.Start("ok", [](const CancelFlag&, ProgressValue& progress, std::string& message) {
        progress.store(1.0f);
        message = "done";
        return true;
    }), "a task starts");
    while (task.Poll() == BackgroundTask::State::Running) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(task.Poll() == BackgroundTask::State::Succeeded && task.Message() == "done" && task.Label() == "ok",
          "it succeeds with its message");

    Check(task.Start("wait", [](const CancelFlag& cancel, ProgressValue&, std::string& message) {
        while (!cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        message = "stopped";
        return false;
    }), "a second task starts after the first");
    Check(!task.Start("other", [](const CancelFlag&, ProgressValue&, std::string&) { return true; }),
          "a task is refused while one runs");
    task.Cancel();
    while (task.Poll() == BackgroundTask::State::Running) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(task.Poll() == BackgroundTask::State::Cancelled && task.Message() == "stopped", "Cancel stops the task");
}

// Interleaved sine frames: amplitude 1.0 is full scale.
std::vector<float> Sine(uint32_t rate, uint32_t channels, double seconds, double hz, double amplitude) {
    const size_t frames = static_cast<size_t>(seconds * rate);
    std::vector<float> out(frames * channels);
    for (size_t f = 0; f < frames; ++f) {
        const float value = static_cast<float>(amplitude * std::sin(2.0 * 3.14159265358979323846 * hz * f / rate));
        for (uint32_t c = 0; c < channels; ++c) out[f * channels + c] = value;
    }
    return out;
}

double Measure(uint32_t rate, uint32_t channels, const std::vector<std::vector<float>>& parts, bool& ok) {
    LoudnessMeter meter(rate, channels);
    for (const std::vector<float>& part : parts) meter.Add(part.data(), part.size() / channels);
    double lufs = 0;
    ok = meter.Integrated(lufs);
    return lufs;
}

bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

void Meter() {
    bool ok = false;
    // BS.1770: a 997 Hz sine at -20 dBFS on both channels reads -20 LUFS.
    double lufs = Measure(48000, 2, {Sine(48000, 2, 10.0, 997.0, 0.1)}, ok);
    Check(ok && Near(lufs, -20.0, 0.05), "a -20 dBFS stereo 997 Hz sine reads -20 LUFS at 48 kHz");
    lufs = Measure(44100, 2, {Sine(44100, 2, 10.0, 997.0, 0.1)}, ok);
    Check(ok && Near(lufs, -20.0, 0.05), "and at 44.1 kHz");
    lufs = Measure(32000, 1, {Sine(32000, 1, 10.0, 997.0, 0.1)}, ok);
    Check(ok && Near(lufs, -20.0, 0.05), "mono reads as dual mono, as the game plays it on both speakers");
    lufs = Measure(48000, 2, {Sine(48000, 2, 10.0, 997.0, 0.1), std::vector<float>(48000 * 2 * 10, 0.0f)}, ok);
    // The three blocks across the edge hold 3/4, 1/2 and 1/4 of the tone and pass the gate, so the 100
    // blocks read 10 * log10(98.5 / 100) = -0.07 LU from the tone alone.
    Check(ok && Near(lufs, -20.066, 0.02), "silence falls below the absolute gate");
    lufs = Measure(48000, 2, {Sine(48000, 2, 10.0, 997.0, 0.1), Sine(48000, 2, 10.0, 997.0, 0.01)}, ok);
    Check(ok && Near(lufs, -20.0, 0.2), "a passage 20 LU quieter falls below the relative gate");
    Measure(48000, 2, {std::vector<float>(48000 * 2 * 5, 0.0f)}, ok);
    Check(!ok, "a silent recording has no loudness");
    Measure(48000, 2, {Sine(48000, 2, 0.3, 997.0, 0.1)}, ok);
    Check(!ok, "a recording under 400 ms has no loudness");
    LoudnessMeter meter(48000, 2);
    const std::vector<float> tone = Sine(48000, 2, 1.0, 997.0, 0.5);
    meter.Add(tone.data(), tone.size() / 2);
    Check(Near(meter.Peak(), 0.5, 1e-3), "the sample peak is the largest sample");

    Check(ReplayGainComment(-12.3456) == "REPLAYGAIN_TRACK_GAIN=-12.35 dB" &&
              ReplayGainComment(3.0) == "REPLAYGAIN_TRACK_GAIN=3.00 dB" &&
              ReplayGainComment(-0.004) == "REPLAYGAIN_TRACK_GAIN=0.00 dB",
          "track gain comments have two decimals and a dB unit");
    Check(ReplayPeakComment(0.5f) == "REPLAYGAIN_TRACK_PEAK=0.500000" &&
              ReplayPeakComment(1.25f) == "REPLAYGAIN_TRACK_PEAK=1.250000",
          "track peak comments have six decimals");
    Check(Near(ReplayGainFor(-5.0), -13.0, 1e-9), "ReplayGain brings a recording to -18 LUFS");
}

void GameLevel() {
    GdxMusicInfo info{};
    Check(gdx_music_level_gain(&info) == 1.0f && gdx_music_level_gain(nullptr) == 1.0f,
          "a file without a level tag plays as decoded");
    info.hasGain = 1;
    info.gainDb = -13.0f;  // a -5 LUFS recording
    Check(Near(20.0 * std::log10(gdx_music_level_gain(&info)), -12.0, 1e-3),
          "a -5 LUFS recording is turned down 12 dB, to the -17 LUFS stock level");
    info.gainDb = 3.0f;
    Check(gdx_music_level_gain(&info) == 1.0f, "without a peak a quiet recording is never raised");
    info.hasPeak = 1;
    info.peak = 0.5f;
    Check(Near(20.0 * std::log10(gdx_music_level_gain(&info)), 4.0, 1e-3), "with headroom it is raised");
    info.gainDb = 20.0f;
    Check(Near(gdx_music_level_gain(&info), 2.0, 1e-4), "but never past full scale at its peak");
    info.gainDb = -200.0f;
    Check(Near(20.0 * std::log10(gdx_music_level_gain(&info)), -60.0, 1e-3), "the cut stops at 60 dB");
}

void LevelTags(const fs::path& dir) {
    std::string error;
    AudioInfo back;
    const fs::path flac = dir / "level.flac";
    WriteBytes(flac, MakeFlac(1000, {"TITLE=Tone", "replaygain_track_gain=-6.50 dB", "REPLAYGAIN_TRACK_PEAK=0.5"}));
    Check(InspectAudio(flac, back, error) && back.has_gain && Near(back.gain_db, -6.5, 1e-4) && back.has_peak &&
              Near(back.peak, 0.5, 1e-6),
          "the decoder reads ReplayGain comments in any case");
    WriteBytes(flac, MakeFlac(1000, {"REPLAYGAIN_TRACK_GAIN=loud", "REPLAYGAIN_TRACK_PEAK=-1"}));
    Check(InspectAudio(flac, back, error) && !back.has_gain && !back.has_peak, "unreadable level comments are ignored");
    WriteBytes(flac, MakeFlac(1000, {"REPLAYGAIN_TRACK_GAIN=+2,5 dB"}));
    Check(InspectAudio(flac, back, error) && !back.has_gain, "a comma decimal is not read as a gain");
    WriteBytes(flac, MakeFlac(1000, {"REPLAYGAIN_TRACK_GAIN= +2.5dB "}));
    Check(InspectAudio(flac, back, error) && back.has_gain && Near(back.gain_db, 2.5, 1e-4),
          "spaces and a plus sign are accepted");

    const MusicLevel level{true, -7.25, 0.25f};
    const std::vector<uint8_t> tagged =
        MakeFlac(1000, {"TITLE=Tone", "LOOPSTART=5", "LOOPLENGTH=10", "REPLAYGAIN_TRACK_GAIN=+1.00 dB"});
    std::vector<uint8_t> out;
    Check(RewriteFlacLoop(tagged, LoopChoice{}, out, error, level), "a FLAC takes a level and keeps its loop");
    WriteBytes(flac, out);
    std::string text(out.begin(), out.end());
    Check(InspectAudio(flac, back, error) && back.has_loop && back.loop_start == 5 && back.loop_end == 15 &&
              back.has_gain && Near(back.gain_db, -7.25, 1e-4) && Near(back.peak, 0.25, 1e-6) &&
              !Contains(text, "+1.00") && Contains(text, "TITLE=Tone"),
          "the old gain comment is replaced and the loop and other comments stay");
    Check(RewriteFlacLoop(tagged, LoopChoice{LoopPolicy::Whole, 0, 0}, out, error), "Whole without a level");
    WriteBytes(flac, out);
    Check(InspectAudio(flac, back, error) && !back.has_loop && back.has_gain && Near(back.gain_db, 1.0, 1e-4),
          "without a level the file's own gain comment stays");

    const FakeOgg ogg = MakeFakeOgg({"TITLE=Tone", "LOOPSTART=5", "LOOPLENGTH=10", "REPLAYGAIN_TRACK_PEAK=0.9"});
    Check(RewriteOggLoop(ogg.bytes, LoopChoice{}, out, error, level), "an Ogg takes a level");
    const ParsedOgg parsed = ParseOgg(out);
    const std::string comment =
        parsed.packets.size() > 1 ? std::string(parsed.packets[1].begin(), parsed.packets[1].end()) : std::string();
    Check(parsed.ok && Contains(comment, "REPLAYGAIN_TRACK_GAIN=-7.25 dB") &&
              Contains(comment, "REPLAYGAIN_TRACK_PEAK=0.250000") && !Contains(comment, "PEAK=0.9") &&
              Contains(comment, "LOOPSTART=5") && Contains(comment, "LOOPLENGTH=10"),
          "the Ogg comment header holds the level and keeps its loop under Keep");

    const fs::path wav = dir / "level.wav";
    std::vector<uint8_t> looped;
    Check(RewriteWavLoop(MakeWav(44100, 2, 1000), LoopChoice{LoopPolicy::Custom, 100, 900}, 44100, looped, error),
          "a WAV loop to keep");
    Check(RewriteWavLoop(looped, LoopChoice{}, 44100, out, error, level), "a WAV takes a level in a new bext chunk");
    WriteBytes(wav, out);
    Check(InspectAudio(wav, back, error) && back.has_loop && back.loop_start == 100 && back.loop_end == 900 &&
              back.has_gain && Near(back.gain_db, -7.25, 0.006) && back.has_peak && Near(back.peak, 0.25, 0.001),
          "the decoder reads the bext loudness and the kept smpl loop");
    std::vector<uint8_t> again;
    Check(RewriteWavLoop(out, LoopChoice{}, 44100, again, error, MusicLevel{true, 2.0, 0.125f}),
          "a WAV with a bext takes a new level");
    WriteBytes(wav, again);
    size_t bext = 0;
    for (size_t i = 0; i + 4 <= again.size(); ++i) bext += std::memcmp(again.data() + i, "bext", 4) == 0 ? 1 : 0;
    Check(InspectAudio(wav, back, error) && bext == 1 && again.size() == out.size() && Near(back.gain_db, 2.0, 0.006) &&
              Near(back.peak, 0.125, 0.001) && back.loop_start == 100,
          "the bext chunk is updated in place, not added to");
}

void StageLevel(const fs::path& dir) {
    const fs::path source = dir / "loud.wav";
    WriteBytes(source, MakeWav(44100, 2, 44100));
    AudioInfo info;
    std::string error;
    InspectAudio(source, info, error);
    LoudnessResult direct;
    CancelFlag cancel{false};
    ProgressValue progress{0.0f};
    Check(MeasureLoudness(source, cancel, progress, 0.0f, 1.0f, direct, error) && Near(direct.peak, 8000.0 / 32768.0, 1e-3),
          "a recording is measured through the game's decoder");
    MusicLevel written;
    const fs::path staged = dir / "staged_level.wav";
    Check(StageMusic(source, info, LoopChoice{}, staged, cancel, progress, error, LevelRequest{true, 2.0}, &written) &&
              progress.load() > 0.99f,
          "an install with the level matched is staged and verified");
    AudioInfo back;
    Check(written.set && Near(written.gain_db, ReplayGainFor(direct.lufs) + 2.0, 1e-9) && InspectAudio(staged, back, error) &&
              Near(back.gain_db, written.gain_db, 0.006) && !back.has_loop,
          "the staged copy carries the measured gain with the adjustment");
    const fs::path silent = dir / "silent.wav";
    std::vector<uint8_t> quiet = MakeWav(44100, 2, 44100);
    std::fill(quiet.begin() + 44, quiet.begin() + 44 + 44100 * 4, 0);
    WriteBytes(silent, quiet);
    InspectAudio(silent, info, error);
    Check(!StageMusic(silent, info, LoopChoice{}, dir / "silent_staged.wav", cancel, progress, error, LevelRequest{true, 0.0}) &&
              Contains(error, "silent"),
          "a silent recording cannot have its level matched");

    // A real recording, when the environment names one: print its loudness to compare with another meter.
    const char* real = std::getenv("GDX_TEST_LOUDNESS");
    if (real == nullptr || real[0] == '\0') {
        std::printf("SKIP: GDX_TEST_LOUDNESS is not set, so no real recording is measured\n");
        return;
    }
    LoudnessResult result;
    Check(MeasureLoudness(PathFromUtf8(real), cancel, progress, 0.0f, 1.0f, result, error), "the real recording is measured");
    std::printf("INFO: %s: %.2f LUFS, sample peak %.2f dBFS\n", real, result.lufs, 20.0 * std::log10(result.peak));

    // With GDX_TEST_LOUDNESS_OUT also set, stage it as Install does, with the level matched, and keep the copy
    // there for a run of the game.
    const char* keep = std::getenv("GDX_TEST_LOUDNESS_OUT");
    if (keep == nullptr || keep[0] == '\0') return;
    AudioInfo real_info;
    Check(InspectAudio(PathFromUtf8(real), real_info, error), "the real recording opens");
    const fs::path out = PathFromUtf8(keep);
    Check(StageMusic(PathFromUtf8(real), real_info, LoopChoice{}, out, cancel, progress, error, LevelRequest{true, 0.0},
                     &written),
          "the real recording is staged with its level matched");
    AudioInfo staged_info;
    Check(InspectAudio(out, staged_info, error) && staged_info.has_gain &&
              staged_info.has_loop == real_info.has_loop && staged_info.loop_start == real_info.loop_start &&
              staged_info.loop_end == real_info.loop_end,
          "the staged copy keeps its loop and carries the level");
    std::printf("INFO: staged %s: track gain %.2f dB, peak %.6f\n", keep, staged_info.gain_db, staged_info.peak);
}

}  // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("gdx_companion_music_tests_" + std::to_string(stamp));
    fs::create_directories(base);

    Slots();
    WavLoops(base);
    FlacLoops(base);
    OggLoops(base);
    LoopRules();
    Names();
    Install(base);
    Native(base);
    Task();
    Meter();
    GameLevel();
    LevelTags(base);
    StageLevel(base);

    std::error_code ec;
    fs::remove_all(base, ec);
    std::printf("%d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
