#!/usr/bin/env python3
"""Reject Linux binaries that will not run on the distributions we claim to support.

The v1.0.0 tarball was built on a rolling distribution and inherited three defects that
were invisible on the build machine: a glibc floor of 2.43, an x86-64-v4 CPU requirement
that the code never actually uses, and dependencies on shared libraries that shipped
nowhere. Each one was reported as a separate bug by a separate user. Running this against
the packaged tree turns all three into a build failure instead of a release.

    python3 tools/check_linux_abi.py --lib-dir dist/lib dist/G-Diffuser dist/gdx-extract

Exits non-zero on the first binary that fails.
"""

import argparse
import os
import struct
import sys

# Libraries a game may rely on the host to provide, in two groups.
#
# The glibc family, whose acceptable age is what --max-glibc bounds; and the graphics, display
# and audio stack, which must come from the running system or it will not match the installed
# driver, display server or sound daemon. Shipping our own copy of those would break machines
# rather than fix them.
#
# Anything outside this set has to travel in lib/, because there is no version of Linux we can
# assume has it. That is the rule v1.0.0 broke.
SYSTEM_LIBS = {
    "ld-linux-x86-64.so.2",
    "ld-linux-aarch64.so.1",
    "libc.so.6",
    "libm.so.6",
    "libdl.so.2",
    "libpthread.so.0",
    "librt.so.1",
    "libutil.so.1",
    "libresolv.so.2",
    "libgcc_s.so.1",
    "libstdc++.so.6",
    # Graphics drivers.
    "libGL.so.1",
    "libGLX.so.0",
    "libGLdispatch.so.0",
    "libOpenGL.so.0",
    "libEGL.so.1",
    "libGLESv2.so.2",
    "libdrm.so.2",
    "libgbm.so.1",
    # Display servers. SDL2 normally dlopens these, but a build that links them directly is
    # still correct: they belong to the user's session, not to us.
    "libX11.so.6",
    "libXau.so.6",
    "libXcursor.so.1",
    "libXdmcp.so.6",
    "libXext.so.6",
    "libXfixes.so.3",
    "libXi.so.6",
    "libXinerama.so.1",
    "libXrandr.so.2",
    "libXrender.so.1",
    "libXss.so.1",
    "libxcb.so.1",
    "libxkbcommon.so.0",
    "libwayland-client.so.0",
    "libwayland-cursor.so.0",
    "libwayland-egl.so.1",
    # Sound servers and device enumeration.
    "libasound.so.2",
    "libpulse.so.0",
    "libpulse-simple.so.0",
    "libdbus-1.so.3",
    "libudev.so.1",
}

# GNU_PROPERTY_X86_ISA_1_NEEDED is cumulative: v4 sets the v2 and v3 bits below it. Anything
# past bit 0 is a hard refusal by the dynamic loader on CPUs that lack the level.
PROP_X86_ISA_1_NEEDED = 0xC0008002
PROP_X86_ISA_1_USED = 0xC0010002
ISA_LEVEL_NAMES = {0: "baseline", 1: "x86-64-v2", 2: "x86-64-v3", 3: "x86-64-v4"}

EM_X86_64 = 62
EM_AARCH64 = 183


def parse_version(text):
    """GLIBC_2.38 -> (2, 38). Unversioned tags sort below everything."""
    tail = text.rsplit("_", 1)[-1]
    try:
        return tuple(int(part) for part in tail.split("."))
    except ValueError:
        return (0,)


class ElfFile:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as handle:
            self.data = handle.read()
        if self.data[:4] != b"\x7fELF" or self.data[4] != 2:
            raise ValueError(f"{path}: not a 64-bit ELF")
        (self.e_machine,) = struct.unpack_from("<H", self.data, 18)
        (_, _, _, _, _, sh_off, _, _, _, _, sh_entsize, sh_num,
         sh_strndx) = struct.unpack_from("<HHIQQQIHHHHHH", self.data, 16)
        self.sections = []
        for index in range(sh_num):
            fields = struct.unpack_from("<IIQQQQIIQQ", self.data,
                                        sh_off + index * sh_entsize)
            self.sections.append({
                "name_off": fields[0], "type": fields[1], "offset": fields[4],
                "size": fields[5], "link": fields[6],
            })
        names = self._raw(self.sections[sh_strndx])
        for section in self.sections:
            section["name"] = _cstr(names, section["name_off"])

    def _raw(self, section):
        return self.data[section["offset"]:section["offset"] + section["size"]]

    def section(self, name):
        for candidate in self.sections:
            if candidate["name"] == name:
                return candidate
        return None

    def blob(self, name):
        section = self.section(name)
        return self._raw(section) if section else b""

    def needed(self):
        """DT_NEEDED entries, plus the RPATH/RUNPATH the loader will search."""
        dynamic, strings = self.blob(".dynamic"), self.blob(".dynstr")
        libs, runpath = [], None
        for offset in range(0, max(0, len(dynamic) - 15), 16):
            tag, value = struct.unpack_from("<qQ", dynamic, offset)
            if tag == 0:
                break
            if tag == 1:
                libs.append(_cstr(strings, value))
            elif tag in (15, 29):
                runpath = _cstr(strings, value)
        return libs, runpath

    def imported_versions(self):
        """Map each required symbol version to the symbols that pull it in."""
        section = self.section(".gnu.version_r")
        if not section:
            return {}
        body, strings = self._raw(section), self.blob(".dynstr")
        by_index, offset = {}, 0
        while offset < len(body):
            _, count, file_off, aux_off, next_off = struct.unpack_from(
                "<HHIII", body, offset)
            aux = offset + aux_off
            for _ in range(count):
                _, _, other, name_off, aux_next = struct.unpack_from("<IHHII", body, aux)
                by_index[other] = _cstr(strings, name_off)
                if not aux_next:
                    break
                aux += aux_next
            if not next_off:
                break
            offset += next_off

        versym, dynsym = self.blob(".gnu.version"), self.blob(".dynsym")
        result = {version: [] for version in by_index.values()}
        for index in range(min(len(versym) // 2, len(dynsym) // 24)):
            (raw,) = struct.unpack_from("<H", versym, index * 2)
            version = by_index.get(raw & 0x7FFF)
            if version is None:
                continue
            (name_off,) = struct.unpack_from("<I", dynsym, index * 24)
            result[version].append(_cstr(strings, name_off))
        return result

    def isa_level_required(self):
        """Highest x86-64 ISA level the loader will demand, as a bit index."""
        if self.e_machine != EM_X86_64:
            # .note.gnu.property ISA bits are x86-64-specific; aarch64 has no equivalent check.
            return 0
        body, offset = self.blob(".note.gnu.property"), 0
        while offset + 12 <= len(body):
            namesz, descsz, ntype = struct.unpack_from("<III", body, offset)
            name = body[offset + 12:offset + 12 + namesz].rstrip(b"\0")
            desc_at = offset + 12 + ((namesz + 3) & ~3)
            if name == b"GNU" and ntype == 5:
                desc, cursor = body[desc_at:desc_at + descsz], 0
                while cursor + 8 <= len(desc):
                    prop_type, datasz = struct.unpack_from("<II", desc, cursor)
                    if prop_type == PROP_X86_ISA_1_NEEDED and datasz <= 8:
                        mask = int.from_bytes(desc[cursor + 8:cursor + 8 + datasz],
                                              "little")
                        return max((bit for bit in range(4) if mask & (1 << bit)),
                                   default=0)
                    cursor += 8 + ((datasz + 7) & ~7)
            offset = desc_at + ((descsz + 3) & ~3)
        return 0


def _cstr(blob, offset):
    end = blob.index(b"\0", offset)
    return blob[offset:end].decode("utf-8", "replace")


def check(path, limits, bundled):
    elf = ElfFile(path)
    failures = []
    name = os.path.basename(path)

    level = elf.isa_level_required()
    if level > 0:
        failures.append(
            f"demands {ISA_LEVEL_NAMES[level]} from the CPU; the build host's startup "
            f"objects stamped a requirement the code does not use")

    versions = elf.imported_versions()
    for prefix, ceiling in limits.items():
        required = [v for v in versions if v.startswith(prefix + "_")]
        if not required:
            continue
        worst = max(required, key=parse_version)
        if parse_version(worst) > parse_version(ceiling):
            culprits = ", ".join(sorted(set(versions[worst]))[:6]) or "?"
            failures.append(f"needs {worst} (ceiling {ceiling}) via {culprits}")

    libs, runpath = elf.needed()
    missing = [lib for lib in libs if lib not in SYSTEM_LIBS and lib not in bundled]
    if missing:
        failures.append("depends on unbundled libraries: " + ", ".join(missing))
    # Only a binary that actually loads something out of lib/ needs to be told to look there;
    # gdx-extract links nothing bundled, and a bundled library usually depends on none of its
    # neighbours. When one does, it needs its own entry: DT_RUNPATH is not inherited, so the
    # executable's does not cover a library's own dependencies.
    from_lib = [lib for lib in libs if lib in bundled]
    if from_lib and "$ORIGIN" not in (runpath or ""):
        failures.append("loads {} from lib/, but its RUNPATH is {}".format(
            ", ".join(from_lib), runpath or "unset"))

    for problem in failures:
        print(f"FAIL  {name}: {problem}")
    if not failures:
        floors = ", ".join(sorted(
            max((v for v in versions if v.startswith(p + "_")),
                key=parse_version, default=f"no {p}")
            for p in limits))
        print(f"ok    {name}: {floors}; {len(libs)} shared dependencies")
    return not failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binaries", nargs="+", help="executables or shared objects")
    parser.add_argument("--lib-dir",
                        help="directory of libraries shipped beside the executable")
    parser.add_argument("--max-glibc", default="2.35",
                        help="highest glibc symbol version allowed (default: %(default)s, "
                             "Ubuntu 22.04)")
    parser.add_argument("--max-glibcxx", default="3.4.30",
                        help="highest libstdc++ symbol version allowed "
                             "(default: %(default)s, GCC 12)")
    parser.add_argument("--list-unbundled", action="store_true",
                        help="print the dependencies that must ship in lib/, one per line, "
                             "and exit; this is how the packaging step decides what to copy")
    args = parser.parse_args()

    if args.list_unbundled:
        for lib in sorted({lib for path in args.binaries
                           for lib in ElfFile(path).needed()[0]
                           if lib not in SYSTEM_LIBS}):
            print(lib)
        return 0

    bundled = set()
    if args.lib_dir and os.path.isdir(args.lib_dir):
        bundled = {entry for entry in os.listdir(args.lib_dir) if ".so" in entry}

    limits = {"GLIBC": f"GLIBC_{args.max_glibc}",
              "GLIBCXX": f"GLIBCXX_{args.max_glibcxx}"}

    targets = list(args.binaries) + [
        os.path.join(args.lib_dir, entry) for entry in sorted(bundled)]
    # Check every binary before deciding, so one CI run reports every problem at once.
    if all([check(path, limits, bundled) for path in targets]):
        return 0
    print("\nThese binaries would fail to start on the distributions the release targets.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
