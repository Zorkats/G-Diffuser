#!/usr/bin/env python3
"""Shared helpers for the O2R extraction harness.

Stdlib only, deterministic. Every script in tools/o2r_harness/ imports this so
the archive-parsing logic (entry inventory, CRCs, version entry, family counts)
lives in exactly one place and stays consistent across the gauntlet.

Contract references (docs/investigation/2026-07-18/o2r-migration/P0_CONTRACTS.md):
  C3 - output contract (entry count, expected-header macros)
  C4 - version-entry contract ([0x01 big][u32 ROM CRC = 0x78D90EB3])
  C5 - validation-before-install checks 2-4
"""

import hashlib
import struct
import zipfile

# --- C4 constants (US rev0 profile) ---------------------------------------
# Torch stamps generic.o2r's `version` entry as [endianness u8][u32 ROM CRC].
# libultraship Archive.cpp reads: [endianness u8] then ReadUInt32() (big-endian).
VERSION_ENTRY_NAME = "version"
VERSION_ENDIANNESS_BIG = 0x01          # libultraship "big" marker
EXPECTED_ROM_CRC = 0x78D90EB3          # US rev0 cartridge CRC (C4)
VERSION_ENTRY_LEN = 5                  # 1 endianness byte + 4 CRC bytes

# C3 golden entry count (full recipe output including the inert families).
#
# This is the number of central-directory RECORDS (dup-inclusive) — EXACTLY what the runtime gate
# at the zipEntryCount EOCD comparison in gdx_extract_launch.cpp compares (it reads EOCD offset 10,
# "total central-directory records"). record_count() below computes the same thing
# (len(z.infolist())). The current
# deterministic archive has NO duplicate records, so records == unique names; the historical
# 4240-records / 664-duplicates / 3576-unique split described the pre-2026-07-18 archive (Windows
# double-emit bug) and no longer applies after the Torch parity fix.
#
# 3611 = 3610 prior census + 1 course_strings record (2026-08-29). FZX:COURSE_STRINGS extracts
# the per-course track names, subtitles and BGM assignment, which are compiled C data no other
# recipe reaches, so a ROM hack could not carry its own. Strictly additive: one record added,
# none removed, no existing payload altered. See decomp/assets/yaml/us/rev0/course_strings.yaml.
#
# 3610 = 3608 prior census + 2 rsp_blob families (aspmain_text, aspmain_data; 2026-08-08). The
# RSP microcode moved out of the source tree (it is Nintendo-copyrighted) into the
# locally-generated archive; strictly additive, no existing payload touched.
#
# 3608 = 3604 prior census + 4 create_machine_textures records (2026-07-25).
#
# 3604 = 3576 pre-R1 base + 22 R1 segment_blob families + 3 R2 audio_blob families + 3 R4
# segment_blob families (common_assets_compressed, kanji_tables, rom_boot_tuning -- W-R4.S1
# census). The base 3576 includes the portVersion entry, which Torch only emits when gdx-extract
# is run with `-u <version>` (the runtime passes `-u 2027490995`, the US-rev0 ROM CRC 0x78D90EB3).
# The gauntlet MUST pass the same `-u` or it will mint a golden that is one record short and whose
# SHA-256 will not match the runtime archive.
#
# The +4 comes from the create_machine_textures re-slice in port/gen/AssetBindings.c: four slots
# that had been reading a neighbour's bytes were given symbols of their own (D_4002F40_2640,
# D_4003180, D_40033C0, D_4003600) and the four around them were repointed at their correct
# segment offsets. Blessed only after checking the shape of the change: the new archive is a
# strict SUPERSET of the old one -- 4 records added, 0 removed, 0 payloads altered, each new
# record a real 656-byte texture rather than the blank a half-populated family would produce.
EXPECTED_ENTRY_COUNT = 3611

# Macro names are a frozen code-level contract with agent 1-B (C3).
MACRO_SHA256 = "GDX_O2R_EXPECTED_SHA256"
MACRO_ENTRY_COUNT = "GDX_O2R_EXPECTED_ENTRY_COUNT"

# --- R5 (C-R5.5) JP profile constants -------------------------------------
# The JP ROM is NOT on disk in this repo, so a real JP golden (archive SHA-256, entry count, and the
# JP cartridge CRC that stamps the archive's version entry) is OWNER-RUN-REQUIRED. These stay None so
# no script can fabricate a JP golden; the JP profile emits an OWNER-RUN-REQUIRED placeholder header
# until the owner runs a real JP extraction. The JP rev0 ROM SHA-1 below is documented in
# decomp/config.yml (an identity, NOT a fabricated golden).
EXPECTED_ROM_CRC_JP = None            # JP cartridge CRC unknown (owner-run)
EXPECTED_ENTRY_COUNT_JP = None        # JP archive entry count unknown (owner-run)
EXPECTED_ROM_SHA1_JP = "a418b0151521b76691fa03f8658c8b567c69498b"
MACRO_SHA256_JP = "GDX_O2R_EXPECTED_SHA256_JP"
MACRO_ENTRY_COUNT_JP = "GDX_O2R_EXPECTED_ENTRY_COUNT_JP"

# Placeholder SHA-256 written into a JP header until a real JP archive exists. An all-zero hash can
# never match a real archive, so the runtime JP gate stays "experimental" (install without the golden
# gate) rather than silently accepting a wrong archive.
PLACEHOLDER_SHA256 = "0" * 64


def profile_macros(profile):
    """(sha_macro, count_macro) for a profile key ('us' or 'jp')."""
    if profile == "jp":
        return MACRO_SHA256_JP, MACRO_ENTRY_COUNT_JP
    return MACRO_SHA256, MACRO_ENTRY_COUNT


def profile_expected_crc(profile):
    """Expected version-entry ROM CRC for a profile, or None if unknown (JP, owner-run)."""
    return EXPECTED_ROM_CRC_JP if profile == "jp" else EXPECTED_ROM_CRC


def profile_expected_count(profile):
    """Expected archive entry count for a profile, or None if unknown (JP, owner-run)."""
    return EXPECTED_ENTRY_COUNT_JP if profile == "jp" else EXPECTED_ENTRY_COUNT


def sha256_file(path):
    """SHA-256 of a file's raw bytes (the whole .o2r container)."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def family_of(name):
    """Top-level category for an entry key.

    Keys are exact `category/symbol` strings; the metadata entries `version`
    and `portVersion` have no slash and are their own family.
    """
    return name.split("/", 1)[0] if "/" in name else name


class Entry:
    """One central-directory record, payload deferred until needed."""

    __slots__ = ("name", "crc", "size", "compress_type", "index")

    def __init__(self, name, crc, size, compress_type, index):
        self.name = name
        self.crc = crc                    # zip CRC-32 of the uncompressed payload
        self.size = size                  # uncompressed payload size
        self.compress_type = compress_type
        self.index = index                # position in the central directory


def read_records(path):
    """Return the ordered list of Entry records from the central directory.

    Counts by RECORD, not by unique name: the "entry count" (C3) and the runtime
    gate both compare the number of central-directory RECORDS, so counting must
    go through this list, never a name-keyed dict (which would collapse any
    duplicates). The current deterministic archive has no duplicate names, but a
    future recipe could reintroduce them; this list stays dup-inclusive so the
    count always matches the runtime gate.
    """
    records = []
    with zipfile.ZipFile(path) as z:
        for i, info in enumerate(z.infolist()):
            records.append(
                Entry(
                    info.filename,
                    info.CRC & 0xFFFFFFFF,
                    info.file_size,
                    info.compress_type,
                    i,
                )
            )
    return records


def record_count(path):
    """Number of central-directory records (the C3 entry count, dup-inclusive)."""
    with zipfile.ZipFile(path) as z:
        return len(z.infolist())


def read_entries(path):
    """Return {name: Entry} (last record wins on duplicates).

    Convenience for by-name lookups; NOT for counting (see read_records).
    """
    out = {}
    for e in read_records(path):
        out[e.name] = e
    return out


def read_order(path):
    """Return the ordered list of entry names (central-directory order, with dups)."""
    with zipfile.ZipFile(path) as z:
        return z.namelist()


def read_payload(path, name):
    """Decompressed payload bytes for a single entry."""
    with zipfile.ZipFile(path) as z:
        return z.read(name)


def family_counts(path):
    """Return {family: count} for every top-level category in the archive."""
    counts = {}
    for name in read_order(path):
        fam = family_of(name)
        counts[fam] = counts.get(fam, 0) + 1
    return counts


def parse_version_entry(data, expected_crc=EXPECTED_ROM_CRC):
    """Parse a `version` entry per C4.

    Returns dict: {ok, reason, endianness, crc, raw_hex}.
    ok is True when the entry is [0x01][u32 BE == expected_crc]. When expected_crc is None (a
    profile whose golden CRC is unknown, e.g. JP/owner-run), the CRC equality check is SKIPPED: a
    well-formed [0x01][u32] entry is reported ok with the observed CRC, so the JP scaffolding can run
    without a fabricated CRC.
    """
    result = {
        "ok": False,
        "reason": "",
        "endianness": None,
        "crc": None,
        "raw_hex": data.hex(),
    }
    if len(data) != VERSION_ENTRY_LEN:
        result["reason"] = (
            "version entry is %d bytes, expected %d ([endianness u8][u32 CRC])"
            % (len(data), VERSION_ENTRY_LEN)
        )
        return result
    endian = data[0]
    result["endianness"] = endian
    if endian != VERSION_ENDIANNESS_BIG:
        result["reason"] = (
            "endianness byte 0x%02X, expected 0x%02X (big)"
            % (endian, VERSION_ENDIANNESS_BIG)
        )
        return result
    crc = struct.unpack(">I", data[1:5])[0]
    result["crc"] = crc
    if expected_crc is None:
        result["ok"] = True
        result["reason"] = "ok (CRC not checked: profile golden CRC unknown / owner-run)"
        return result
    if crc != expected_crc:
        result["reason"] = (
            "ROM CRC 0x%08X, expected 0x%08X" % (crc, expected_crc)
        )
        return result
    result["ok"] = True
    result["reason"] = "ok"
    return result


def check_version_entry(path, expected_crc=EXPECTED_ROM_CRC):
    """Read and validate the `version` entry of an archive (C4). Returns parse dict.

    Adds `present` key; if absent, ok=False with a reason. `expected_crc=None` skips CRC equality
    (profile golden CRC unknown / owner-run).
    """
    entries = read_entries(path)
    if VERSION_ENTRY_NAME not in entries:
        return {
            "ok": False,
            "present": False,
            "reason": "no `version` entry in archive",
            "endianness": None,
            "crc": None,
            "raw_hex": "",
        }
    data = read_payload(path, VERSION_ENTRY_NAME)
    parsed = parse_version_entry(data, expected_crc)
    parsed["present"] = True
    return parsed
