import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib


def run(executable, option, directory):
    subprocess.run([str(executable), option, str(directory)], check=True)


def read_png(path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    cursor = 8
    compressed = bytearray()
    width = height = 0
    while cursor < len(data):
        size = struct.unpack_from(">I", data, cursor)[0]
        name = data[cursor + 4:cursor + 8]
        payload = data[cursor + 8:cursor + 8 + size]
        crc = struct.unpack_from(">I", data, cursor + 8 + size)[0]
        assert zlib.crc32(name + payload) == crc
        if name == b"IHDR":
            width, height, bits, color, _, _, interlace = struct.unpack(">IIBBBBB", payload)
            assert (bits, color, interlace) == (8, 6, 0)
        if name == b"IDAT":
            compressed.extend(payload)
        cursor += size + 12
    raw = zlib.decompress(compressed)
    stride = width * 4
    decoded = bytearray()
    for y in range(height):
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        mode = raw[y * (stride + 1)]
        for x in range(stride):
            left = row[x - 4] if x >= 4 else 0
            above = decoded[(y - 1) * stride + x] if y else 0
            upper_left = decoded[(y - 1) * stride + x - 4] if y and x >= 4 else 0
            predictors = [0, left, above, (left + above) // 2]
            if mode == 4:
                p = left + above - upper_left
                distances = [abs(p - left), abs(p - above), abs(p - upper_left)]
                predictor = [left, above, upper_left][distances.index(min(distances))]
            else:
                predictor = predictors[mode]
            row[x] = (row[x] + predictor) & 255
        decoded.extend(row)
    return width, height, bytes(decoded)


def main():
    executable = pathlib.Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="gdx-native-machine-") as temporary:
        root = pathlib.Path(temporary).resolve()
        exports = root / "exports"
        existing = exports / "machine_1_lod1_1"
        existing.mkdir(parents=True)
        sentinel = existing / "user.txt"
        sentinel.write_text("preserve")
        run(executable, "--export-fixture", exports)
        directory = exports / "machine_1_lod1_2"
        assert sentinel.read_text() == "preserve"
        document = json.loads((directory / "machine.gltf").read_text())
        binary = (directory / "machine.bin").read_bytes()
        assert document["asset"]["version"] == "2.0"
        assert document["buffers"][0]["byteLength"] == len(binary) == 120
        assert document["bufferViews"][0]["byteStride"] == 36
        assert document["accessors"][0]["min"] == [0, 0, 0]
        assert document["accessors"][0]["max"] == [10, 10, 0]
        assert struct.unpack_from("<3I", binary, 108) == (0, 1, 2)
        assert struct.unpack_from("<3f", binary, 36) == (10, 0, 0)
        assert binary[32:36] == bytes([128, 255, 255, 255])
        assert document["accessors"][3]["normalized"] is True
        assert document["samplers"][0]["wrapS"] == 33648
        assert document["samplers"][0]["wrapT"] == 33071
        assert document["materials"][0]["alphaMode"] == "BLEND"
        assert document["meshes"][0]["primitives"][0]["indices"] == 4
        width, height, rgba = read_png(directory / "material_0.png")
        assert (width, height) == (2, 2)
        assert rgba == bytes([255, 0, 0, 128, 0, 255, 0, 128, 0, 0, 255, 128, 255, 255, 255, 128])
        obj = (directory / "machine.obj").read_text()
        assert "f 1/1/1 2/2/2 3/3/3" in obj
        assert "map_Kd material_0.png" in (directory / "machine.mtl").read_text()
        run(executable, "--export-fixture", exports)
        assert (exports / "machine_1_lod1_3" / "machine.obj").is_file()
        run(executable, "--export-native-fixture", exports)
        native = exports / "machine_1_lod1_4"
        document = json.loads((native / "machine.gltf").read_text())
        binary = (native / "machine.bin").read_bytes()
        assert len(document["materials"]) == len(document["meshes"][0]["primitives"]) == 3
        vertex_bytes = 18 * 36
        assert document["buffers"][0]["byteLength"] == len(binary) == vertex_bytes + 18 * 4
        for i in range(3):
            accessor = document["accessors"][4 + i]
            assert accessor["count"] == 6
            assert struct.unpack_from("<6I", binary, vertex_bytes + accessor["byteOffset"]) == tuple(range(i * 6, i * 6 + 6))
        assert read_png(native / "material_1.png")[0:2] == (16, 16)
        blocked = root / "not-a-directory"
        blocked.write_text("preserve")
        run(executable, "--export-error", blocked)
        assert blocked.read_text() == "preserve"
    print("machine export artifact tests passed")


if __name__ == "__main__":
    main()
