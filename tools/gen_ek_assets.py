#!/usr/bin/env python3
"""G-Diffuser Expansion Kit asset generator (EK slice 1).

Reads the disk-side asset yamls from the fzerox-expansion-kit decomp and emits:
  * include/assets/us/ek/**.h        -- Torch-style headers (extern decls + WIDTH/HEIGHT
                                        defines) so EK sources compile with ASSET_VERSION=us.
  * decomp/src/assets/us/ek/**.c     -- zero-initialized storage definitions included by decomp
                                        sources; runtime disk loading supplies their data.
  * port/gen/EkAssetBindings.c       -- real-size array DEFINITIONS for every EK asset
                                        symbol (never 1-byte stubs: indexed tables and the
                                        runtime resolvers need real storage sizes), plus
                                        gdx_ek_assets_fill() copying data from the 64DD
                                        disk image.
  * port/gen/ek_slice_manifest.txt   -- deterministic disk-slice metadata used by extraction.

--compile-assets-only leaves the curated port/gen runtime outputs untouched. CMake uses that
mode because the checked-in bindings contain reviewed size corrections that must win at link time.

The yaml offsets address the synthetic baserom.jp.z64dd assembled by
fzerox-expansion-kit/tools/extract_baserom.py. They are not physical offsets in
an .ndd image. This generator maps each synthetic section back to its source
LBA, then converts that LBA to the physical/block-linear .ndd byte offset.
"""
import argparse
import ast
import bisect
import glob
import io
import os
import re

try:
    import yaml
except ModuleNotFoundError as error:
    raise SystemExit(
        "gen_ek_assets.py requires PyYAML; install it with "
        "'python -m pip install pyyaml'"
    ) from error


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--compile-assets-only",
        action="store_true",
        help=(
            "generate compile-time headers and zero-filled storage definitions without "
            "rewriting the curated runtime bindings in port/gen"
        ),
    )
    return parser.parse_args()


ARGS = parse_args()

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EK_YAML_DIR = os.path.join(REPO, "fzerox-expansion-kit", "assets", "yaml", "jp")
HEADER_OUT_DIR = os.path.join(REPO, "include", "assets", "us", "ek")
PAYLOAD_OUT_DIR = os.path.join(REPO, "decomp", "src", "assets", "us", "ek")
BINDING_C = os.path.join(REPO, "port", "gen", "EkAssetBindings.c")
SLICE_MANIFEST = os.path.join(REPO, "port", "gen", "ek_slice_manifest.txt")
DECOMP_SRC = os.path.join(REPO, "decomp", "src")
EK_PATCH_SCRIPT = os.path.join(REPO, "fzerox-expansion-kit", "tools", "patch.py")

# Both the retail JP disk and LuigiBlood's translated disk are type 3. These
# tables mirror decomp/src/leo/lib/leo_tbl.c and LeoLBAToByte().
EK_DISK_TYPE = 3
NDD_SYSTEM_AREA_BYTES = 0x738C0
NUM_LBAS = 4292
LEO_BLOCK_BYTES = (0x4D08, 0x47B8, 0x4510, 0x3FC0, 0x3A70, 0x3520, 0x2FD0, 0x2A80, 0x2530)
LEO_VZONE_BOUNDS = (
    (0x0124, 0x0248, 0x035A, 0x047E, 0x05A2, 0x06B4, 0x07C6, 0x08D8,
     0x09EA, 0x0AB6, 0x0B82, 0x0C94, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC),
    (0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x06A2, 0x07C6, 0x08D8,
     0x09EA, 0x0AFC, 0x0BC8, 0x0C94, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC),
    (0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08C6,
     0x09EA, 0x0AFC, 0x0C0E, 0x0CDA, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC),
    (0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4,
     0x09C6, 0x0AEA, 0x0C0E, 0x0D20, 0x0DEC, 0x0EB8, 0x0FCA, 0x10DC),
    (0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4,
     0x09C6, 0x0AD8, 0x0BEA, 0x0D0E, 0x0E32, 0x0EFE, 0x0FCA, 0x10DC),
    (0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x086E,
     0x0980, 0x0A92, 0x0BA4, 0x0CB6, 0x0DC8, 0x0EEC, 0x1010, 0x10DC),
    (0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x086E,
     0x093A, 0x0A4C, 0x0B5E, 0x0C70, 0x0D82, 0x0E94, 0x0FB8, 0x10DC),
)
LEO_VZONE_PZONE = (
    (0x00, 0x01, 0x02, 0x09, 0x08, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A),
    (0x00, 0x01, 0x02, 0x03, 0x0A, 0x09, 0x08, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B),
    (0x00, 0x01, 0x02, 0x03, 0x04, 0x0B, 0x0A, 0x09, 0x08, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C),
    (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x06, 0x07, 0x0F, 0x0E, 0x0D),
    (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x0F, 0x0E),
    (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x0F),
    (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08),
)


def load_patch_dict(name):
    """Read a literal dictionary from patch.py without importing its tooling dependencies."""
    with open(EK_PATCH_SCRIPT, encoding="utf-8") as f:
        module = ast.parse(f.read(), EK_PATCH_SCRIPT)
    for node in module.body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1 and
                isinstance(node.targets[0], ast.Name) and node.targets[0].id == name):
            return ast.literal_eval(node.value)
    raise RuntimeError("{} not found in {}".format(name, EK_PATCH_SCRIPT))


def load_synthetic_sections():
    rom_addrs = load_patch_dict("diskRomAddrs")
    disk_lbas = load_patch_dict("diskSegments")
    if set(rom_addrs) != set(disk_lbas):
        raise RuntimeError("patch.py diskRomAddrs and diskSegments disagree")

    # extract_baserom.py starts the main block at synthetic offset zero. The
    # patcher's main ROM symbol starts at 0x60 because that initial header is
    # outside the linked main section.
    rom_addrs["main"] = 0

    # Audio is appended after the ghost blocks by extract_baserom.py but is not
    # part of patch.py's relocatable section table.
    rom_addrs.update({"audio_bank_dd": 0x266B30, "audio_seq_dd": 0x26C520})
    disk_lbas.update({"audio_bank_dd": 1000, "audio_seq_dd": 1002})

    return sorted((int(rom_addrs[name]), int(disk_lbas[name]), name) for name in rom_addrs)


def lba_block_size(user_lba):
    physical_lba = user_lba + 0x18
    bounds = LEO_VZONE_BOUNDS[EK_DISK_TYPE]
    vzone = bisect.bisect_right(bounds, physical_lba)
    if vzone >= len(bounds):
        raise ValueError("LBA {} is outside the disk".format(user_lba))
    pzone = LEO_VZONE_PZONE[EK_DISK_TYPE][vzone]
    zone = pzone - 7 if pzone >= 8 else pzone
    return LEO_BLOCK_BYTES[zone]


def build_lba_file_offsets():
    offsets = [NDD_SYSTEM_AREA_BYTES]
    for lba in range(NUM_LBAS):
        offsets.append(offsets[-1] + lba_block_size(lba))
    return offsets


SYNTHETIC_SECTIONS = load_synthetic_sections()
SYNTHETIC_SECTION_STARTS = [section[0] for section in SYNTHETIC_SECTIONS]
SYNTHETIC_SECTIONS_BY_NAME = {section[2]: section for section in SYNTHETIC_SECTIONS}
LBA_FILE_OFFSETS = build_lba_file_offsets()
EXPECTED_NDD_SIZE = 64_931_840
if LBA_FILE_OFFSETS[-1] != EXPECTED_NDD_SIZE:
    raise RuntimeError(
        "64DD geometry produced {} bytes, expected {}".format(LBA_FILE_OFFSETS[-1], EXPECTED_NDD_SIZE)
    )


def synthetic_to_physical(synthetic_offset):
    section_index = bisect.bisect_right(SYNTHETIC_SECTION_STARTS, synthetic_offset) - 1
    if section_index < 0:
        raise ValueError("synthetic offset 0x{:X} precedes the first disk section".format(synthetic_offset))
    section_start, start_lba, _name = SYNTHETIC_SECTIONS[section_index]
    return LBA_FILE_OFFSETS[start_lba] + (synthetic_offset - section_start)


def synthetic_section_end(synthetic_offset):
    section_index = bisect.bisect_right(SYNTHETIC_SECTION_STARTS, synthetic_offset) - 1
    if section_index < 0 or section_index + 1 >= len(SYNTHETIC_SECTIONS):
        return None
    return SYNTHETIC_SECTIONS[section_index + 1][0]


def synthetic_section_name(synthetic_offset):
    section_index = bisect.bisect_right(SYNTHETIC_SECTION_STARTS, synthetic_offset) - 1
    if section_index < 0:
        return None
    return SYNTHETIC_SECTIONS[section_index][2]


def scan_payload_includes():
    """Payload .c paths the decomp includes via ASSET_SOURCE_EK(...).

    Torch emits asset payload C files (initialized arrays); several EK data
    files #include them directly, so the yaml's symbols must be DEFINED in a
    generated payload file rather than in EkAssetBindings.c (the include would
    otherwise produce duplicate definitions at link).

    A file that includes a payload is compiled by definition, so every include found here
    counts. This was previously filtered against source paths scraped out of
    port/CMakeLists.txt, which silently stopped matching once those sources moved behind a
    ${DECOMP} glob: the regex then matched nothing, no payload file was written at all, and
    the breakage stayed invisible on any tree still holding the previously generated ones.

    Note that the payload definitions do NOT replace the ones in EkAssetBindings.c. Both are
    tentative definitions of the same symbols, and -fcommon (port/CMakeLists.txt) merges them
    at the larger size -- which is what lets a hand-edited size in EkAssetBindings.c win over
    the generator's. Regenerating that file over its hand-edits therefore changes runtime
    behaviour even though the build still succeeds."""
    paths = set()
    for root, _dirs, files in os.walk(DECOMP_SRC):
        if os.sep + "assets" in root:
            continue
        for name in files:
            if not name.endswith((".c", ".h")):
                continue
            with open(os.path.join(root, name), encoding="utf-8", errors="ignore") as f:
                for m in re.finditer(r"ASSET_SOURCE_EK\(([^)]+\.c)\)", f.read()):
                    paths.add(m.group(1).strip())
    return paths

TEXTURE_BPP = {
    "RGBA16": 2, "RGBA32": 4, "IA16": 2, "IA8": 1, "IA4": 0.5,
    "I8": 1, "I4": 0.5, "CI8": 1, "CI4": 0.5, "TLUT": 2,
}

CTYPE_FOR_TYPE = {"GFX": "Gfx", "VTX": "Vtx", "MTX": "Mtx"}


def entry_ctype(val):
    if val.get("ctype"):
        return val["ctype"]
    return CTYPE_FOR_TYPE.get(val.get("type"), "u8")


def entry_size(val, next_offset):
    # COMPRESSED_TEXTURE symbols hold the compressed stream (the game inflates
    # at runtime), so their storage size is the on-disk span, not w*h*bpp.
    if val.get("size") is not None:
        return int(val["size"])
    if val.get("type") == "TEXTURE":
        bpp = TEXTURE_BPP.get(val.get("format", ""), 2)
        w = int(val.get("width", 0))
        h = int(val.get("height", 0))
        size = int(w * h * bpp)
        if size > 0:
            return size
    offset = val.get("offset")
    if offset is not None and next_offset is not None and next_offset > int(offset):
        return next_offset - int(offset)
    return 8


def ctype_bytes(ctype):
    return {"u8": 1, "s8": 1, "u16": 2, "s16": 2, "u32": 4, "s32": 4,
            "f32": 4, "Gfx": 8, "Vtx": 16, "Mtx": 64}.get(ctype, 1)


def write_text_if_changed(path, content):
    try:
        with open(path, encoding="utf-8") as current:
            if current.read() == content:
                return False
    except FileNotFoundError:
        pass

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as output:
        output.write(content)
    return True


headers = []       # (relative_header_path, [lines])
definitions = []   # (ctype, sym, element_count, byte_size)
payloads = []      # (relative_payload_c_path, [definition lines])
fills = []         # (ctype, asset_type, sym, disk_byte_offset, byte_size, n64_address)
# R8 Step 2: the same slice list projected for the disk archive step and the future Dump All.
# Each row = (sym, disk_byte_offset, byte_size, asset_type, format, width, height, tlut). The
# gdx-extract `disk` subcommand consumes only (sym, offset, size) to append verbatim ek/<symbol>
# archive entries; the texture metadata (format/width/height/tlut) rides along for Dump All.
slice_manifest = []
seen_syms = set()
payload_includes = scan_payload_includes()
yaml_paths = sorted(glob.glob(os.path.join(EK_YAML_DIR, "**", "*.yaml"), recursive=True))
if not yaml_paths:
    raise SystemExit(
        "No Expansion Kit YAML inputs were found under '{}'. Initialize the "
        "fzerox-expansion-kit submodule before building.".format(EK_YAML_DIR)
    )

for path in yaml_paths:
    rel = os.path.relpath(path, EK_YAML_DIR).replace("\\", "/")
    stem_rel = os.path.splitext(rel)[0]
    stem = os.path.basename(stem_rel)
    stem_dir = os.path.dirname(stem_rel)
    # Torch payload path convention: <yaml dir>/<stem>/<stem>.c
    payload_rel = ("{}/{}/{}.c".format(stem_dir, stem, stem) if stem_dir
                   else "{}/{}.c".format(stem, stem))
    is_payload = payload_rel in payload_includes
    payload_lines = []

    with open(path) as f:
        data = yaml.safe_load(f.read()) or {}

    config = data.get(":config", {}) or {}
    segs = config.get("segments") or []
    synthetic_base = 0
    expected_section = None
    address_kind = None
    address_base = 0
    if segs and isinstance(segs[0], (list, tuple)) and len(segs[0]) >= 2:
        synthetic_base = int(segs[0][1])
        expected_section = synthetic_section_name(synthetic_base)
        address_kind = "segment"
        address_base = int(segs[0][0]) << 24
    elif (isinstance(config.get("virtual"), (list, tuple)) and
          len(config["virtual"]) >= 2):
        expected_section = synthetic_section_name(int(config["virtual"][1]))
        address_kind = "virtual"
        address_base = int(config["virtual"][0])
    elif stem in SYNTHETIC_SECTIONS_BY_NAME:
        expected_section = stem

    items = []
    for key, val in data.items():
        if isinstance(val, dict) and not str(key).startswith(":") and val.get("offset") is not None:
            items.append((int(val["offset"]), key, val))
    items.sort(key=lambda item: item[0])
    next_offsets = {}
    for idx, (offset, key, _val) in enumerate(items):
        if idx + 1 < len(items):
            next_offsets[key] = items[idx + 1][0]
        else:
            section_end = synthetic_section_end(synthetic_base + offset)
            if section_end is not None and section_end > synthetic_base + offset:
                next_offsets[key] = section_end - synthetic_base

    guard = stem_rel.replace("/", "_").replace("-", "_").upper() + "_H"
    lines = ["#ifndef {}".format(guard), "#define {}".format(guard), "", '#include "gfx.h"', ""]

    for key, val in data.items():
        if not isinstance(val, dict) or str(key).startswith(":"):
            continue
        sym = val.get("symbol", key)
        ctype = entry_ctype(val)
        size = entry_size(val, next_offsets.get(key))
        lines.append("extern {} {}[];".format(ctype, sym))
        if val.get("type") in ("TEXTURE", "COMPRESSED_TEXTURE"):
            lines.append("#define _{}_WIDTH 0x{:x}".format(sym, int(val.get("width", 0))))
            lines.append("#define _{}_HEIGHT 0x{:x}".format(sym, int(val.get("height", 0))))
        if val.get("type") == "COMPRESSED_TEXTURE" or val.get("compression"):
            lines.append("#define _{}_COMPRESSED_SIZE 0x{:x}".format(sym, size))

        if sym in seen_syms:
            continue
        seen_syms.add(sym)

        elem = ctype_bytes(ctype)
        count = max(1, (size + elem - 1) // elem)
        if is_payload:
            payload_lines.append("{} {}[{}];".format(ctype, sym, count))
        else:
            definitions.append((ctype, sym, count, size))
        if val.get("offset") is not None:
            synthetic_offset = synthetic_base + int(val["offset"])
            actual_section = synthetic_section_name(synthetic_offset)
            if expected_section is not None and actual_section != expected_section:
                raise ValueError(
                    "{}:{} maps to synthetic section {}, expected {}".format(
                        rel, key, actual_section, expected_section
                    )
                )
            n64_address = 0
            if address_kind == "segment":
                n64_address = address_base | int(val["offset"])
            elif address_kind == "virtual":
                n64_address = address_base + int(val["offset"]) - int(config["virtual"][1])
            disk_off = synthetic_to_physical(synthetic_offset)
            fills.append((
                ctype,
                val.get("type", ""),
                sym,
                disk_off,
                size,
                n64_address,
            ))
            slice_manifest.append((
                sym,
                disk_off,
                size,
                val.get("type", "") or "-",
                val.get("format"),
                val.get("width"),
                val.get("height"),
                val.get("tlut"),
            ))

    lines += ["", "#endif", ""]
    headers.append((stem_rel + ".h", lines))
    if is_payload:
        payloads.append((payload_rel, payload_lines))

for rel_header, lines in headers:
    out_path = os.path.join(HEADER_OUT_DIR, rel_header)
    content = "\n".join(["// AUTO-GENERATED by tools/gen_ek_assets.py. Do not edit by hand."] + lines)
    write_text_if_changed(out_path, content)

for rel_payload, lines in payloads:
    out_path = os.path.join(PAYLOAD_OUT_DIR, rel_payload)
    # Torch payloads carry the asset dimension #defines with them; consumers
    # (e.g. course_select.c struct initializers) rely on that, so include the
    # generated header alongside the zero-filled definitions.
    header_rel = os.path.dirname(os.path.dirname(rel_payload))
    stem = os.path.splitext(os.path.basename(rel_payload))[0]
    header_path = "{}/{}.h".format(header_rel, stem) if header_rel else "{}.h".format(stem)
    content = (
        "// AUTO-GENERATED by tools/gen_ek_assets.py. Zero-filled asset payload\n"
        "// definitions (data arrives at runtime via gdx_ek_assets_fill).\n"
        '#include "assets/us/ek/{}"\n'.format(header_path)
        + "\n".join(lines)
        + "\n"
    )
    write_text_if_changed(out_path, content)

with (io.StringIO() if ARGS.compile_assets_only else open(BINDING_C, "w")) as f:
    f.write("// AUTO-GENERATED by tools/gen_ek_assets.py. Do not edit by hand.\n")
    f.write("// Real-size definitions for Expansion Kit disk asset symbols plus the\n")
    f.write("// disk-image fill table. Compiled only when GDX_EXPANSION_KIT is enabled.\n")
    f.write('#include "global.h"\n\n')
    for ctype, sym, count, _size in definitions:
        f.write("{} {}[{}];\n".format(ctype, sym, count))
    for ctype, _asset_type, sym, disk_off, size, _n64_address in fills:
        f.write("extern {} {}[];\n".format(ctype, sym))
    f.write("\nextern void gdx_register_host_raw_n64_range(void* ptr, unsigned long long size);\n")
    f.write("extern void gdx_register_host_n64_command_range(void* ptr, unsigned long long size);\n")
    f.write("extern void gdx_register_n64_address_range(unsigned int n64Begin, void* hostBegin, unsigned long long size);\n")
    # E3: registers each filled entry's own host address/size so
    # ResolveHostPointerStub (port/n64_gfx_bridge.cpp) recognizes a wide SETTIMG
    # pointer that carries the array's address directly, covering the full fill
    # table instead of a hand-maintained subset.
    f.write("extern void gdx_register_host_pointer_stub(void* dest, unsigned long long size);\n")
    f.write("\nenum { GDX_EK_ASSET_OTHER, GDX_EK_ASSET_GFX, GDX_EK_ASSET_VP, GDX_EK_ASSET_VTX };\n")
    f.write("typedef struct { void* dest; unsigned int diskOffset; unsigned int size; unsigned int n64Address; unsigned char kind; } GdxEkAssetFill;\n")
    f.write("static const GdxEkAssetFill sEkAssetFills[] = {\n")
    for _ctype, asset_type, sym, disk_off, size, n64_address in fills:
        kind = {
            "GFX": "GDX_EK_ASSET_GFX",
            "VP": "GDX_EK_ASSET_VP",
            "VTX": "GDX_EK_ASSET_VTX",
        }.get(asset_type, "GDX_EK_ASSET_OTHER")
        f.write("    {{ {}, 0x{:08X}U, 0x{:X}U, 0x{:08X}U, {} }},\n".format(
            sym, disk_off, size, n64_address, kind
        ))
    f.write("    { 0, 0U, 0U, 0U, GDX_EK_ASSET_OTHER }\n};\n\n")
    f.write("/* Copies EK payloads from the 64DD image and records their original N64\n")
    f.write(" * address ranges. Gfx commands remain big-endian/raw; Vp and Vtx fields\n")
    f.write(" * used directly by the host interpreter are converted to native order. */\n")
    f.write("void gdx_ek_assets_fill(const unsigned char* disk, unsigned long long diskSize) {\n")
    f.write("    int i;\n")
    f.write("    unsigned int b;\n")
    f.write("    /* Table-shift bracket: gSampleBankTable entries read -0x30 at audio init\n")
    f.write("       in static-variant boots. Canary before/after this fill localizes\n")
    f.write("       whether the corruption happens inside it. */\n")
    f.write("    extern void gdx_cki(const char* s, int v);\n")
    f.write("    {\n")
    f.write("        typedef struct { unsigned int p[4]; } GdxTblHdr;\n")
    f.write("        extern unsigned char gSampleBankTable[];\n")
    f.write("        unsigned int v;\n")
    f.write("        const unsigned char* e1 = gSampleBankTable + 0x10 + 0x10; /* entries[1].romAddr */\n")
    f.write("        v = (unsigned int)e1[0] | ((unsigned int)e1[1] << 8) | ((unsigned int)e1[2] << 16) | ((unsigned int)e1[3] << 24);\n")
    f.write("        gdx_cki(\"[tblwatch] pre-fill entries[1].romAddr\", (int) v);\n")
    f.write("    }\n")
    f.write("    if (disk == 0) { return; }\n")
    f.write("    for (i = 0; sEkAssetFills[i].dest != 0; i++) {\n")
    f.write("        if ((unsigned long long)sEkAssetFills[i].diskOffset + sEkAssetFills[i].size <= diskSize) {\n")
    f.write("            unsigned char* dest = (unsigned char*)sEkAssetFills[i].dest;\n")
    f.write("            const unsigned char* src = disk + sEkAssetFills[i].diskOffset;\n")
    f.write("            /* byte loop: this TU compiles with the decomp's headers, which\n")
    f.write("               clash with the MSVC CRT's <string.h> */\n")
    f.write("            for (b = 0; b < sEkAssetFills[i].size; b++) {\n")
    f.write("                dest[b] = src[b];\n")
    f.write("            }\n")
    f.write("            if (sEkAssetFills[i].kind == GDX_EK_ASSET_VP) {\n")
    f.write("                for (b = 0; b + 1 < sEkAssetFills[i].size; b += 2) {\n")
    f.write("                    unsigned char hi = dest[b];\n")
    f.write("                    dest[b] = dest[b + 1];\n")
    f.write("                    dest[b + 1] = hi;\n")
    f.write("                }\n")
    f.write("            } else if (sEkAssetFills[i].kind == GDX_EK_ASSET_VTX) {\n")
    f.write("                /* Vtx fields are big-endian on disk. Swap xyz, flag, and st;\n")
    f.write("                   leave the final four colour/normal bytes untouched. */\n")
    f.write("                for (b = 0; b + 16 <= sEkAssetFills[i].size; b += 16) {\n")
    f.write("                    unsigned int k;\n")
    f.write("                    for (k = 0; k < 12; k += 2) {\n")
    f.write("                        unsigned char hi = dest[b + k];\n")
    f.write("                        dest[b + k] = dest[b + k + 1];\n")
    f.write("                        dest[b + k + 1] = hi;\n")
    f.write("                    }\n")
    f.write("                }\n")
    f.write("            } else if (sEkAssetFills[i].kind == GDX_EK_ASSET_GFX) {\n")
    f.write("                gdx_register_host_raw_n64_range(dest, sEkAssetFills[i].size);\n")
    f.write("            }\n")
    f.write("            if (sEkAssetFills[i].n64Address != 0) {\n")
    f.write("                gdx_register_n64_address_range(sEkAssetFills[i].n64Address, dest, sEkAssetFills[i].size);\n")
    f.write("            }\n")
    f.write("            gdx_register_host_pointer_stub(dest, sEkAssetFills[i].size);\n")
    f.write("        }\n")
    f.write("    }\n")
    f.write("    {\n")
    f.write("        extern unsigned char gSampleBankTable[];\n")
    f.write("        unsigned int v;\n")
    f.write("        const unsigned char* e1 = gSampleBankTable + 0x10 + 0x10;\n")
    f.write("        v = (unsigned int)e1[0] | ((unsigned int)e1[1] << 8) | ((unsigned int)e1[2] << 16) | ((unsigned int)e1[3] << 24);\n")
    f.write("        gdx_cki(\"[tblwatch] post-fill entries[1].romAddr\", (int) v);\n")
    f.write("    }\n")
    f.write("}\n")
    f.write("\n/* Returns the byte extent of a generated disk-resident segmented image. */\n")
    f.write("unsigned int gdx_ek_segment_image_size(unsigned char segment) {\n")
    f.write("    unsigned int required = 0;\n")
    f.write("    int i;\n")
    f.write("    for (i = 0; sEkAssetFills[i].dest != 0; i++) {\n")
    f.write("        unsigned int address = sEkAssetFills[i].n64Address;\n")
    f.write("        unsigned int offset;\n")
    f.write("        unsigned int end;\n")
    f.write("        if ((address >> 24) != segment) { continue; }\n")
    f.write("        offset = address & 0x00FFFFFFU;\n")
    f.write("        end = offset + sEkAssetFills[i].size;\n")
    f.write("        if (end >= offset && end > required) { required = end; }\n")
    f.write("    }\n")
    f.write("    return required;\n")
    f.write("}\n")
    f.write("\n/* Rebuilds one disk-resident segment in its original address layout while\n")
    f.write(" * applying only the host-order conversions required by direct consumers. */\n")
    f.write("int gdx_ek_segment_image_fill(unsigned char segment, const unsigned char* disk,\n")
    f.write("                              unsigned long long diskSize, unsigned char* dest,\n")
    f.write("                              unsigned int capacity) {\n")
    f.write("    unsigned int required = gdx_ek_segment_image_size(segment);\n")
    f.write("    unsigned int b;\n")
    f.write("    int i;\n")
    f.write("    if (disk == 0 || dest == 0 || required == 0 || required > capacity) { return 0; }\n")
    f.write("    for (b = 0; b < required; b++) { dest[b] = 0; }\n")
    f.write("    for (i = 0; sEkAssetFills[i].dest != 0; i++) {\n")
    f.write("        unsigned int address = sEkAssetFills[i].n64Address;\n")
    f.write("        unsigned int offset;\n")
    f.write("        unsigned char* item;\n")
    f.write("        const unsigned char* src;\n")
    f.write("        if ((address >> 24) != segment) { continue; }\n")
    f.write("        offset = address & 0x00FFFFFFU;\n")
    f.write("        if (offset > capacity || sEkAssetFills[i].size > capacity - offset ||\n")
    f.write("            (unsigned long long)sEkAssetFills[i].diskOffset + sEkAssetFills[i].size > diskSize) {\n")
    f.write("            return 0;\n")
    f.write("        }\n")
    f.write("        item = dest + offset;\n")
    f.write("        src = disk + sEkAssetFills[i].diskOffset;\n")
    f.write("        for (b = 0; b < sEkAssetFills[i].size; b++) { item[b] = src[b]; }\n")
    f.write("        if (sEkAssetFills[i].kind == GDX_EK_ASSET_VP) {\n")
    f.write("            for (b = 0; b + 1 < sEkAssetFills[i].size; b += 2) {\n")
    f.write("                unsigned char hi = item[b]; item[b] = item[b + 1]; item[b + 1] = hi;\n")
    f.write("            }\n")
    f.write("        } else if (sEkAssetFills[i].kind == GDX_EK_ASSET_VTX) {\n")
    f.write("            for (b = 0; b + 16 <= sEkAssetFills[i].size; b += 16) {\n")
    f.write("                unsigned int k;\n")
    f.write("                for (k = 0; k < 12; k += 2) {\n")
    f.write("                    unsigned char hi = item[b + k];\n")
    f.write("                    item[b + k] = item[b + k + 1]; item[b + k + 1] = hi;\n")
    f.write("                }\n")
    f.write("            }\n")
    f.write("        } else if (sEkAssetFills[i].kind == GDX_EK_ASSET_GFX) {\n")
    f.write("            for (b = 0; b + 3 < sEkAssetFills[i].size; b += 4) {\n")
    f.write("                unsigned char b0 = item[b]; unsigned char b1 = item[b + 1];\n")
    f.write("                item[b] = item[b + 3]; item[b + 1] = item[b + 2];\n")
    f.write("                item[b + 2] = b1; item[b + 3] = b0;\n")
    f.write("            }\n")
    f.write("            gdx_register_host_n64_command_range(item, sEkAssetFills[i].size);\n")
    f.write("        }\n")
    f.write("    }\n")
    f.write("    return 1;\n")
    f.write("}\n")


def _mf_field(value):
    """Manifest cell: '-' for absent, hex for the tlut segment address, else str."""
    if value is None:
        return "-"
    return str(value)


# R8 Step 2: deterministic EK slice manifest. One row per named disk asset, sorted by symbol so the
# archive step inserts ek/<symbol> entries in a stable order (container stays byte-identical across
# runs). Columns: symbol offset(hex) len(dec) type format width height tlut. The disk subcommand
# reads only the first three; the rest is texture metadata for the future offline Dump All.
manifest_rows = sorted(slice_manifest, key=lambda row: row[0])
texture_types = ("TEXTURE", "COMPRESSED_TEXTURE")
texture_count = sum(1 for row in manifest_rows if row[3] in texture_types)
with (io.StringIO() if ARGS.compile_assets_only else open(SLICE_MANIFEST, "w", newline="\n")) as f:
    f.write("# AUTO-GENERATED by tools/gen_ek_assets.py. Do not edit by hand.\n")
    f.write("# G-Diffuser Expansion Kit disk-asset slice manifest (R8 Step 2), format v1.\n")
    f.write("# Source of truth = the same fill table baked into port/gen/EkAssetBindings.c\n")
    f.write("# (sEkAssetFills), so an ek/<symbol> archive entry is byte-identical to the bytes\n")
    f.write("# gdx_ek_assets_fill copies from the 64DD image at runtime.\n")
    f.write("# fields: symbol offset(hex) len(dec) type format width(dec) height(dec) tlut(hex)\n")
    f.write("# absent metadata is '-'. offsets are physical 64DD .ndd byte offsets; the manifest\n")
    f.write("# serves both retail JP and the fan-translated disk (both leo disk type 3).\n")
    f.write("version 1\n")
    f.write("count {}\n".format(len(manifest_rows)))
    f.write("textures {}\n".format(texture_count))
    for sym, disk_off, size, typ, fmt, w, h, tlut in manifest_rows:
        tlut_hex = "-" if tlut is None else "0x{:X}".format(int(tlut))
        f.write("{} 0x{:08X} {} {} {} {} {} {}\n".format(
            sym, disk_off, size, typ,
            _mf_field(fmt), _mf_field(w), _mf_field(h), tlut_hex,
        ))

print("EK assets: {} headers, {} symbol defs, {} disk fills".format(len(headers), len(definitions), len(fills)))
if ARGS.compile_assets_only:
    print("EK runtime bindings unchanged (--compile-assets-only)")
else:
    print("EK slice manifest: {} rows ({} textures) -> {}".format(len(manifest_rows), texture_count, SLICE_MANIFEST))
