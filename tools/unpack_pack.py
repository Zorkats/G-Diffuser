#!/usr/bin/env python3
# Copyright (c) Zorkats
"""Read or unpack a G-Diffuser pack (.o2r) or track package (.gdxp), for analysis.

Quick start:

    python -I tools/unpack_pack.py their-pack.o2r --list
    python -I tools/unpack_pack.py their-pack.o2r -o their-pack-unpacked --dump-dir <game>/dump

The Companion's Inspect workspace runs the same code through its worker.

Output folder (it must not exist yet):
  contents.tsv       one row per archive entry: path, kind, detail, and where it was written
  workshop.json      the pack's metadata, as stored
  textures/          texture overrides as PNG, laid out like the game's dump, with a manifest.tsv, so
                     the Textures page (or gen_texture_pack.py) packs the folder again
  alt-textures/      the same for the alternate assets channel (alt/textures/pack/...)
  palette-variants/  GLideN64 palette variants (<path>.pal/<CC>-<CRC>) as PNG
  files/             every other entry at its archive path, byte for byte

Paletted textures (CI4, CI8) hold colour indices only: the palette is the game's. With --dump-dir
naming the game's dump folder, the PNG takes its colours from the palette swatch the dump wrote, and
manifest.tsv takes each texture's native size and format from the dump. Without it, or when the dump
lacks a texture, a paletted PNG shows its indices as grey levels, and a raw HD texture (one the
.htc converter wrote as RGBA32) gets no manifest.tsv row, since its native format is unknown.

Archives come from other people, so they are untrusted: an entry with an unsafe name is listed and
skipped, entry sizes, counts and texture dimensions are capped, and nothing is written outside the
output folder. PNG files are written with the standard library. Reading a dump's palette swatches
needs Pillow; without it paletted textures show their indices in grey.
"""

import argparse
import json
import math
import os
import shutil
import struct
import sys
import zlib
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import n64_texels  # noqa: E402  (path insert must precede import)

MAX_ENTRIES = 50000
MAX_ENTRY_BYTES = 128 * 1024 * 1024
MAX_TOTAL_BYTES = 4 * 1024 * 1024 * 1024
MAX_TEXTURE_SIDE = 8192
MAX_NAME_CHARS = 400
MAX_PART_CHARS = 120
MAX_LISTED = 400          # entries a listing returns; contents.tsv always holds every entry
MAX_LISTED_BYTES = 40000  # keeps one Companion protocol line well under 64 KiB
MAX_META_TEXT = 200
MAX_SUMMARY_LINE = 600
PROGRESS_EVERY = 250

OTR_HEADER_SIZE = 64
OTEX_V1_SUBHEADER = struct.Struct("<IIIIffI")  # type, width, height, flags, h scale, v scale, size
OTEX_V0_SUBHEADER = struct.Struct("<IIII")     # type, width, height, size (textures Torch extracts)
TEX_FLAG_LOAD_AS_RAW = 1

# Fast::TextureType (libultraship fast/resource/type/Texture.h), as gen_texture_pack.py writes it.
TEXTURE_TYPES = {1: "RGBA32", 2: "RGBA16", 3: "CI4", 4: "CI8", 5: "I4", 6: "I8", 7: "IA4", 8: "IA8",
                 9: "IA16"}

RESOURCE_NAMES = {"OTEX": "Texture", "ODLT": "Display list", "OVTX": "Vertices", "OMTX": "Matrix",
                  "OBLB": "Blob", "OARR": "Array", "OTXT": "Text"}

FILE_KINDS = (
    ((".lua",), "Lua script"),
    ((".json",), "JSON"),
    ((".wav", ".ogg", ".flac", ".mp3"), "Audio"),
    ((".png", ".jpg", ".jpeg", ".bmp"), "Image"),
    ((".gdxc",), "Track"),
    ((".gcs",), "Track snapshot"),
    ((".glsl", ".vert", ".frag", ".shader", ".slang"), "Shader"),
    ((".txt", ".md"), "Text"),
)

WINDOWS_RESERVED = {"CON", "PRN", "AUX", "NUL"} | {"COM%d" % i for i in range(1, 10)} | \
    {"LPT%d" % i for i in range(1, 10)}
UNSAFE_CHARS = set('<>:"|?*\\')

TEXTURE_FOLDERS = {"default": "textures", "alt": "alt-textures", "variant": "palette-variants"}
MANIFEST_HEADER = "# key\tnative_w\tnative_h\tn64_fmt\tpalette_key   (written by unpack_pack.py)\n"


class UnpackError(Exception):
    """The archive cannot be read at all."""


# ── names ─────────────────────────────────────────────────────────────────────────────────────────────
def safe_parts(name):
    """The path parts of an archive name that is safe to write on every desktop system, or None."""
    if not name or len(name) > MAX_NAME_CHARS or name.startswith("/") or "\x00" in name:
        return None
    parts = name.split("/")
    for part in parts:
        if part in ("", ".", "..") or len(part) > MAX_PART_CHARS:
            return None
        if any(ord(c) < 32 or c in UNSAFE_CHARS for c in part):
            return None
        if part[-1] in " .":
            return None
        if part.split(".")[0].upper() in WINDOWS_RESERVED:
            return None
    return parts


def archive_format(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".gdxp":
        return "gdxp"
    if ext == ".o2r":
        return "o2r"
    return "zip"


# ── classification ────────────────────────────────────────────────────────────────────────────────────
def resource_tag(head):
    """The four-letter type of a game resource header ('OTEX'), or None."""
    if len(head) < OTR_HEADER_SIZE or head[0] not in (0, 1):
        return None
    tag = head[4:8][::-1]  # stored as a little-endian word
    if all(0x41 <= c <= 0x5A for c in tag) and tag[:1] == b"O":
        return tag.decode("ascii")
    return None


def texture_header(head):
    """(fmt, width, height, flags, h scale, v scale, payload offset) of an OTEX resource, or a reason."""
    version = struct.unpack_from("<I", head, 8)[0]
    if version == 1 and len(head) >= OTR_HEADER_SIZE + OTEX_V1_SUBHEADER.size:
        tex_type, width, height, flags, h_scale, v_scale, _size = OTEX_V1_SUBHEADER.unpack_from(head, OTR_HEADER_SIZE)
        offset = OTR_HEADER_SIZE + OTEX_V1_SUBHEADER.size
    elif version == 0 and len(head) >= OTR_HEADER_SIZE + OTEX_V0_SUBHEADER.size:
        tex_type, width, height, _size = OTEX_V0_SUBHEADER.unpack_from(head, OTR_HEADER_SIZE)
        flags, h_scale, v_scale = 0, 1.0, 1.0
        offset = OTR_HEADER_SIZE + OTEX_V0_SUBHEADER.size
    else:
        return "texture version %d is not one this tool reads" % version
    fmt = TEXTURE_TYPES.get(tex_type)
    if fmt is None:
        return "texture type %d is unknown" % tex_type
    if not (1 <= width <= MAX_TEXTURE_SIDE and 1 <= height <= MAX_TEXTURE_SIDE):
        return "%s %dx%d is outside 1 to %d texels per side" % (fmt, width, height, MAX_TEXTURE_SIDE)
    return fmt, width, height, flags, h_scale, v_scale, offset


def texture_place(name):
    """(channel, key) for an OTEX entry: where the game applies it and the dump key it replaces."""
    channel = "default"
    rest = name
    if rest.startswith("alt/"):
        channel, rest = "alt", rest[len("alt/"):]
    if rest.startswith("textures/pack/"):
        rest = rest[len("textures/pack/"):]
    if ".pal/" in rest:
        channel = "variant"
    return channel, rest


def native_size(width, height, h_scale, v_scale):
    def axis(size, scale):
        if not (isinstance(scale, float) and math.isfinite(scale)) or scale < 1.0:
            return size, 1
        whole = round(scale)
        if whole < 1 or abs(scale - whole) > 1e-3 or size % whole:
            return size, 1
        return size // whole, whole
    native_w, sx = axis(width, h_scale)
    native_h, sy = axis(height, v_scale)
    return native_w, native_h, max(sx, sy)


def file_kind(name, head):
    lower = name.lower()
    if lower == "workshop.json":
        return "Pack metadata"
    if lower.startswith("audio/seq/"):
        return "Sequence fonts" if lower.endswith(".font") else "Sequence"
    if head[:4] == b"GSMP":
        return "N64 sample (VADPCM)"
    if head[:4] == b"GFT1":
        return "Soundfont"
    tag = resource_tag(head)
    if tag is not None:
        return RESOURCE_NAMES.get(tag, "Game resource " + tag)
    for extensions, kind in FILE_KINDS:
        if lower.endswith(extensions):
            return kind
    return "Binary file"


# ── the dump ──────────────────────────────────────────────────────────────────────────────────────────
class Dump:
    """The game's dump folder: native size and format per texture key, and the palette swatches."""

    def __init__(self, folder):
        self.folder = folder
        self.rows = {}
        self._palettes = {}
        if folder is None:
            return
        path = os.path.join(folder, "manifest.tsv")
        if not os.path.isfile(path):
            raise UnpackError("the dump folder has no manifest.tsv: " + folder)
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                parts = line.rstrip("\r\n").split("\t")
                if line.startswith("#") or len(parts) < 4:
                    continue
                try:
                    width, height = int(parts[1]), int(parts[2])
                except ValueError:
                    continue
                palette = parts[4].strip() if len(parts) >= 5 else ""
                self.rows[parts[0]] = (width, height, parts[3].strip(),
                                       palette if palette and palette != "-" else None)

    def palette_path(self, palette_key):
        parts = safe_parts(palette_key) if palette_key else None
        if self.folder is None or parts is None:
            return None
        path = os.path.join(self.folder, *parts) + ".png"
        return path if os.path.isfile(path) else None

    def palette(self, palette_key):
        if palette_key in self._palettes:
            return self._palettes[palette_key]
        colors = None
        path = self.palette_path(palette_key)
        image_module = _pillow() if path is not None else None
        if image_module is not None:
            try:
                with image_module.open(path) as image:
                    if image.width * image.height <= 4096:
                        raw = image.convert("RGBA").tobytes()
                        colors = [tuple(raw[i:i + 4]) for i in range(0, len(raw), 4)]
            except (OSError, ValueError):
                colors = None
        self._palettes[palette_key] = colors
        return colors

    def can_read_palettes(self):
        return self.folder is not None and _pillow() is not None


def _pillow():
    """PIL.Image, or None. Swatches are any PNG the user saved, so Pillow reads them; PNG output needs no Pillow."""
    try:
        from PIL import Image
    except ImportError:
        return None
    return Image


# ── reading ───────────────────────────────────────────────────────────────────────────────────────────
def open_archive(path):
    if not os.path.isfile(path):
        raise UnpackError("file not found: " + path)
    if not zipfile.is_zipfile(path):
        raise UnpackError("not a zip archive. G-Diffuser packs (.o2r) and track packages (.gdxp) are zip "
                          "files; an .otr from another port is a different format")
    try:
        archive = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as exc:
        raise UnpackError("the archive cannot be read: %s" % exc)
    infos = [info for info in archive.infolist() if not info.is_dir()]
    if len(infos) > MAX_ENTRIES:
        archive.close()
        raise UnpackError("the archive holds %d entries, more than the %d this tool reads" % (len(infos), MAX_ENTRIES))
    return archive, infos


def read_entry(archive, info, limit):
    if info.flag_bits & 0x1:
        raise ValueError("the entry is encrypted")
    if info.file_size > limit:
        raise ValueError("the entry is %d bytes, over the %d byte limit" % (info.file_size, limit))
    with archive.open(info) as fh:
        data = fh.read(limit + 1)
    if len(data) > limit:
        raise ValueError("the entry is over the %d byte limit" % limit)
    return data


def read_head(archive, info):
    if info.flag_bits & 0x1:
        return b""
    try:
        with archive.open(info) as fh:
            return fh.read(OTR_HEADER_SIZE + OTEX_V1_SUBHEADER.size)
    except (zipfile.BadZipFile, OSError, RuntimeError, ValueError, NotImplementedError):
        return b""


def clip(text):
    text = "".join(c for c in str(text) if ord(c) >= 32)
    return text[:MAX_META_TEXT]


def read_metadata(archive, names):
    if "workshop.json" not in names:
        return {}
    try:
        info = archive.getinfo("workshop.json")
        meta = json.loads(read_entry(archive, info, 1024 * 1024).decode("utf-8"))
    except (ValueError, KeyError, UnicodeDecodeError, zipfile.BadZipFile, OSError, RuntimeError):
        return {"problem": "workshop.json is not valid JSON"}
    if not isinstance(meta, dict):
        return {"problem": "workshop.json is not an object"}
    out = {}
    for key, field in (("name", "title"), ("author", "author"), ("version", "version"), ("id", "pack_id"),
                       ("description", "description"), ("game_version", "game_version")):
        value = meta.get(key)
        if isinstance(value, (str, int, float)) and not isinstance(value, bool):
            out[field] = clip(value)
    for key in ("depends", "conflicts"):
        value = meta.get(key)
        if isinstance(value, list):
            out[key] = [clip(item) for item in value if isinstance(item, str)][:32]
    return out


def describe_texture(head, name, dump):
    """(detail, texture info or None) for an OTEX entry, from its header alone."""
    parsed = texture_header(head)
    if isinstance(parsed, str):
        return parsed, None
    fmt, width, height, flags, h_scale, v_scale, offset = parsed
    channel, key = texture_place(name)
    native_w, native_h, scale = native_size(width, height, h_scale, v_scale)
    row = dump.rows.get(key) if channel != "variant" else None
    text = "%s %dx%d" % (fmt, width, height)
    if flags & TEX_FLAG_LOAD_AS_RAW:
        text += ", raw HD"
        if row is not None:
            text += " over native %s %dx%d" % (row[2], row[0], row[1])
    elif scale > 1:
        text += ", %dx of %dx%d" % (scale, native_w, native_h)
    if fmt in ("CI4", "CI8"):
        palette_key = row[3] if row is not None else None
        if not dump.palette_path(palette_key):
            text += ", no palette: indices shown as grey"
        elif not dump.can_read_palettes():
            text += ", reading the dump's palette needs Pillow: indices shown as grey"
        else:
            text += ", palette from the dump"
    if channel == "alt":
        text += ", alternate assets"
    elif channel == "variant":
        text += ", palette variant"
    elif not name.startswith(("textures/pack/", "alt/")):
        text += ", direct key"
    info = {"fmt": fmt, "width": width, "height": height, "flags": flags, "offset": offset, "channel": channel,
            "key": key, "native": (native_w, native_h), "row": row}
    return text, info


def scan(path, dump_dir=None):
    """Reads an archive's names and headers. Returns the listing dict; writes nothing."""
    dump = Dump(dump_dir)
    archive, infos = open_archive(path)
    with archive:
        names = {info.filename for info in infos}
        meta = read_metadata(archive, names)
        entries = []
        for info in sorted(infos, key=lambda i: i.filename):
            name = info.filename
            entry = {"path": name, "bytes": info.file_size, "safe": safe_parts(name) is not None}
            head = read_head(archive, info)
            entry["kind"] = file_kind(name, head)
            entry["detail"] = ""
            entry["texture"] = None
            if not entry["safe"]:
                entry["detail"] = "unsafe name, skipped"
            elif info.flag_bits & 0x1:
                entry["detail"] = "encrypted, skipped"
            elif entry["kind"] == "Texture":
                entry["detail"], entry["texture"] = describe_texture(head, name, dump)
            elif info.file_size:
                entry["detail"] = human_bytes(info.file_size)
            entries.append(entry)
    return {"format": archive_format(path), "meta": meta, "entries": entries, "dump": dump}


def human_bytes(count):
    if count < 1024:
        return "%d bytes" % count
    if count < 1024 * 1024:
        return "%.1f KiB" % (count / 1024.0)
    return "%.1f MiB" % (count / (1024.0 * 1024.0))


def summary_lines(listing):
    meta = listing["meta"]
    lines = []
    if meta.get("problem"):
        lines.append(meta["problem"] + ".")
    elif not meta and listing["format"] == "o2r":
        lines.append("No workshop.json: the pack has no name or author.")
    counts = {}
    for entry in listing["entries"]:
        counts[entry["kind"]] = counts.get(entry["kind"], 0) + 1
    for kind in sorted(counts, key=lambda k: (-counts[k], k)):
        lines.append("%d x %s" % (counts[kind], kind))
    grey = sum(1 for e in listing["entries"] if "indices shown as grey" in e["detail"])
    if grey:
        lines.append("%d paletted texture(s) have no palette in the dump and show their indices as grey."
                     % grey)
    unsafe = sum(1 for e in listing["entries"] if not e["safe"])
    if unsafe:
        lines.append("%d entry name(s) are unsafe to write and are skipped." % unsafe)
    for key in ("depends", "conflicts"):
        if meta.get(key):
            text = "%s: %s" % ("Needs" if key == "depends" else "Conflicts with", ", ".join(meta[key]))
            lines.append(text[:MAX_SUMMARY_LINE])
    return lines


def listing_result(listing):
    """The listing as plain JSON values: metadata, summary, and the first entries.

    Entries are capped by their size as escaped ASCII JSON, the form the worker sends, so a pack of
    non-ASCII names still fits one protocol line.
    """
    rows = []
    used = 0
    for entry in listing["entries"]:
        row = "%s\t%s\t%s" % (entry["path"], entry["kind"], entry["detail"])
        cost = len(json.dumps(row)) + 1
        if len(rows) >= MAX_LISTED or used + cost > MAX_LISTED_BYTES:
            break
        rows.append(row)
        used += cost
    result = {"format": listing["format"], "summary": summary_lines(listing), "entries": rows,
              "total": len(listing["entries"])}
    for field in ("title", "author", "version", "pack_id", "description"):
        if listing["meta"].get(field):
            result[field] = listing["meta"][field]
    return result


# ── writing ───────────────────────────────────────────────────────────────────────────────────────────
class Writer:
    """Writes files under one folder and refuses two names that only differ in case."""

    def __init__(self, root):
        self.root = root
        self.taken = set()

    def write(self, rel_parts, data):
        rel = "/".join(rel_parts)
        folded = rel.casefold()
        if folded in self.taken:
            raise ValueError("another entry already wrote " + rel + " (names differ only in case)")
        path = os.path.join(self.root, *rel_parts)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)
        self.taken.add(folded)
        return rel

    def claim(self, rel_parts):
        rel = "/".join(rel_parts)
        folded = rel.casefold()
        if folded in self.taken:
            raise ValueError("another entry already wrote " + rel + " (names differ only in case)")
        self.taken.add(folded)
        path = os.path.join(self.root, *rel_parts)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        return rel, path


def texture_rgba(data, texture, dump):
    fmt, width, height = texture["fmt"], texture["width"], texture["height"]
    payload = data[texture["offset"]:]
    if fmt in ("CI4", "CI8"):
        row = texture["row"]
        colors = dump.palette(row[3]) if row is not None and row[3] else None
        return n64_texels.decode_indexed(fmt, payload, width, height, colors)
    return n64_texels.decode(fmt, payload, width, height)


def png_bytes(rgba, width, height):
    """An 8-bit RGBA PNG, standard library only, so unpacking works without Pillow."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    stride = width * 4
    rows = b"".join(b"\x00" + bytes(rgba[y * stride:(y + 1) * stride]) for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 6))
            + chunk(b"IEND", b""))


def write_png(writer, rel_parts, rgba, width, height):
    return writer.write(rel_parts, png_bytes(rgba, width, height))


def manifest_row(texture):
    """The manifest.tsv row that lets the Textures page pack this PNG again, or None."""
    row = texture["row"]
    if row is not None:
        return row
    if texture["flags"] & TEX_FLAG_LOAD_AS_RAW:
        return None
    native_w, native_h = texture["native"]
    return native_w, native_h, texture["fmt"], None


def copy_swatch(writer, folder_name, palette_key, dump, log):
    """Copies the dump's palette swatch beside the textures, so the Textures page can pack a CI texture.
    Returns True when the swatch was written."""
    source = dump.palette_path(palette_key)
    if source is None:
        return False
    parts = safe_parts(palette_key)
    parts[-1] += ".png"
    try:
        with open(source, "rb") as fh:
            swatch = fh.read(1024 * 1024)
        writer.write([folder_name] + parts, swatch)
    except (OSError, ValueError) as exc:
        log("palette %s not copied: %s" % (palette_key, exc))
        return False
    return True


def unpack_into(path, folder, dump_dir=None, log=print):
    """Unpacks the archive into folder, which must exist and be empty. Returns the result dict."""
    listing = scan(path, dump_dir)
    dump = listing["dump"]
    writer = Writer(folder)
    manifests = {}
    palettes_copied = set()
    contents = ["# path\tkind\tdetail\twritten"]
    written = failed = 0
    total_bytes = 0
    archive, _infos = open_archive(path)
    with archive:
        for index, entry in enumerate(listing["entries"], 1):
            name = entry["path"]
            where = "-"
            try:
                if not entry["safe"] or entry["detail"] == "encrypted, skipped":
                    raise ValueError(entry["detail"])
                data = read_entry(archive, archive.getinfo(name), MAX_ENTRY_BYTES)
                total_bytes += len(data)
                if total_bytes > MAX_TOTAL_BYTES:
                    raise UnpackError("the archive unpacks to more than %s; stopped" % human_bytes(MAX_TOTAL_BYTES))
                texture = entry["texture"]
                if texture is not None:
                    channel, key = texture["channel"], texture["key"]
                    folder_name = TEXTURE_FOLDERS[channel]
                    parts = [folder_name] + safe_parts(key)
                    parts[-1] += ".png"
                    rgba = texture_rgba(data, texture, dump)
                    where = write_png(writer, parts, rgba, texture["width"], texture["height"])
                    row = manifest_row(texture) if channel != "variant" else None
                    if row is not None:
                        manifests.setdefault(folder_name, []).append((key,) + tuple(row))
                        if (folder_name, row[3]) not in palettes_copied:
                            palettes_copied.add((folder_name, row[3]))
                            if copy_swatch(writer, folder_name, row[3], dump, log) and row[3] in dump.rows:
                                manifests[folder_name].append((row[3],) + tuple(dump.rows[row[3]]))
                elif name == "workshop.json":
                    where = writer.write(["workshop.json"], data)
                else:
                    where = writer.write(["files"] + safe_parts(name), data)
                written += 1
            except UnpackError:
                raise
            except (ValueError, OSError, RuntimeError, zipfile.BadZipFile, NotImplementedError) as exc:
                failed += 1
                where = "not written: %s" % exc
                if entry["safe"]:
                    log("skipped %s: %s" % (name, exc))
            contents.append("%s\t%s\t%s\t%s" % (name.replace("\t", " "), entry["kind"], entry["detail"], where))
            if index % PROGRESS_EVERY == 0:
                log("unpacked %d of %d entries" % (index, len(listing["entries"])))
    for folder_name, rows in manifests.items():
        lines = [MANIFEST_HEADER]
        for key, width, height, fmt, palette_key in sorted(rows):
            lines.append("%s\t%d\t%d\t%s\t%s\n" % (key, width, height, fmt, palette_key or "-"))
        writer.write([folder_name, "manifest.tsv"], "".join(lines).encode("utf-8"))
    writer.write(["contents.tsv"], ("\n".join(contents) + "\n").encode("utf-8"))
    result = listing_result(listing)
    result["written"] = written
    result["failed"] = failed
    return result


def unpack(path, output, dump_dir=None, log=print):
    """Unpacks into output, a folder that must not exist yet, through a staging folder beside it."""
    output = os.path.abspath(output)
    if os.path.lexists(output):
        raise UnpackError("the output folder already exists: " + output)
    parent = os.path.dirname(output)
    if not os.path.isdir(parent):
        raise UnpackError("the folder for the output does not exist: " + parent)
    staging = os.path.join(parent, ".unpack-staging-%d" % os.getpid())
    if os.path.lexists(staging):
        raise UnpackError("a staging folder is in the way: " + staging)
    os.mkdir(staging)
    try:
        result = unpack_into(path, staging, dump_dir, log)
        os.rename(staging, output)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    result["folder"] = output
    log("unpacked %d of %d entries into %s" % (result["written"], result["total"], output))
    return result


# ── command line ──────────────────────────────────────────────────────────────────────────────────────
def print_listing(result):
    for field in ("title", "author", "version", "pack_id"):
        if result.get(field):
            print("%-8s %s" % (field.replace("_", " ") + ":", result[field]))
    for line in result["summary"]:
        print(line)
    print()
    for row in result["entries"]:
        print(row.replace("\t", "  |  "))
    if result["total"] > len(result["entries"]):
        print("... %d more; unpack to get contents.tsv with every entry"
              % (result["total"] - len(result["entries"])))


def main():
    ap = argparse.ArgumentParser(
        description="Read or unpack a G-Diffuser pack (.o2r) or track package (.gdxp) for analysis",
        epilog="Examples:\n"
               "  unpack_pack.py their-pack.o2r --list\n"
               "  unpack_pack.py their-pack.o2r -o their-pack-unpacked --dump-dir <game>/dump",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive", help="the .o2r or .gdxp file")
    ap.add_argument("--list", action="store_true", help="list what the archive holds; write nothing")
    ap.add_argument("-o", "--output", help="the folder to create (must not exist)")
    ap.add_argument("--dump-dir", help="the game's dump folder, for palettes and native texture formats")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    args = ap.parse_args()
    if args.list == bool(args.output):
        sys.stderr.write("error: pass --list or -o <folder>\n")
        return 2
    try:
        if args.list:
            result = listing_result(scan(args.archive, args.dump_dir))
            if not args.json:
                print_listing(result)
        else:
            result = unpack(args.archive, args.output, args.dump_dir)
    except UnpackError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 1
    if args.json:
        print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
