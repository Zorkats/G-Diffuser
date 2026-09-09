import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile


MATTE = 0x18C7
ASSETS = {"aMachineBodyBoostGripTex": (128, 64)} | {
    f"aMachineStat{grade}Tex": (16, 16) for grade in "ABCDE"
}


def rgba16(data, expected):
    if len(data) < 80 or data[0] != 0 or data[4:8] != b"XETO":
        raise ValueError("Expected a little-endian OTEX resource")
    version = struct.unpack_from("<I", data, 8)[0]
    if version not in (0, 1):
        raise ValueError("Unsupported OTEX version")
    offset = 80 if version == 0 else 92
    if len(data) < offset:
        raise ValueError("Truncated OTEX header")
    kind, width, height = struct.unpack_from("<III", data, 64)
    length = struct.unpack_from("<I", data, offset - 4)[0]
    if kind != 2 or (width, height) != expected or length != width * height * 2 or len(data) != offset + length:
        raise ValueError("Unexpected RGBA16 dimensions or payload length")
    if version == 1:
        flags, sx, sy = struct.unpack_from("<Iff", data, 76)
        if flags != 0 or sx != 4 or sy != 4:
            raise ValueError("Expected unmasked 4x OTEX metadata")
    pixels = struct.unpack_from(f">{width * height}H", data, offset)
    if any(pixel & 1 == 0 for pixel in pixels):
        raise ValueError("Expected opaque stats artwork")
    return offset, pixels


def repair_texture(native, hd, dimensions):
    width, height = dimensions
    _, reference = rgba16(native, dimensions)
    offset, pixels = rgba16(hd, (width * 4, height * 4))
    corrected = bytearray(hd)
    changed = 0
    for index, pixel in enumerate(pixels):
        y, x = divmod(index, width * 4)
        if reference[(y // 4) * width + x // 4] != MATTE:
            continue
        channels = (pixel >> 11, (pixel >> 6) & 31, (pixel >> 1) & 31)
        if max(channels) <= 3 and pixel != MATTE:
            struct.pack_into(">H", corrected, offset + index * 2, MATTE)
            changed += 1
    return bytes(corrected), changed


def repair_archive(native_path, pack_path, output=None):
    if output is not None and (output.exists() or output.resolve() in (native_path.resolve(), pack_path.resolve())):
        raise ValueError("Output must be a new file, not either input archive")
    replacements = {}
    with native_path.open("rb") as native_stream, pack_path.open("rb") as pack_stream:
        report = {"native_sha256": hashlib.file_digest(native_stream, "sha256").hexdigest(),
                  "pack_sha256": hashlib.file_digest(pack_stream, "sha256").hexdigest(), "assets": []}
    with zipfile.ZipFile(native_path) as native, zipfile.ZipFile(pack_path) as pack:
        if len(pack.namelist()) != len(set(pack.namelist())):
            raise ValueError("Duplicate pack entries")
        for name, dimensions in ASSETS.items():
            key = "common_assets_compressed/" + name
            path = "textures/pack/" + key
            original = pack.read(path)
            fixed, count = repair_texture(native.read(key), original, dimensions)
            replacements[path] = fixed
            report["assets"].append({"path": path, "changed_pixels": count,
                                     "before_sha256": hashlib.sha256(original).hexdigest(),
                                     "after_sha256": hashlib.sha256(fixed).hexdigest()})
        if "workshop.json" in pack.namelist():
            metadata = json.loads(pack.read("workshop.json"))
            for entry in metadata.get("files", []):
                if entry["path"] in replacements:
                    entry["sha256"] = hashlib.sha256(replacements[entry["path"]]).hexdigest()
            replacements["workshop.json"] = (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode()
        if output is not None:
            with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as target:
                for name in sorted(pack.namelist()):
                    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                    info.compress_type = zipfile.ZIP_DEFLATED
                    target.writestr(info, replacements[name] if name in replacements else pack.read(name))
            with output.open("rb") as stream:
                report["output_sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
    return report


def main():
    parser = argparse.ArgumentParser(description="Restore native gray matte in six HDCube 4x stats textures; keep HD lettering.")
    parser.add_argument("native", type=Path, help="Original fzerox.o2r (read only)")
    parser.add_argument("pack", type=Path, help="HDCube4.o2r (read only)")
    parser.add_argument("output", type=Path, nargs="?", help="New corrected .o2r; omit for a dry run")
    args = parser.parse_args()
    try:
        print(json.dumps(repair_archive(args.native, args.pack, args.output), indent=2))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as exc:
        parser.exit(1, f"Stats repair failed: {exc}\n")


if __name__ == "__main__":
    main()
