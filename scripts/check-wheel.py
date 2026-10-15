"""Checks that a wheel's platform tag says what the library inside it is. A wheel is installed on every machine its tag allows, so a tag that promises more than the
library delivers (a universal2 tag on an arm64-only library) installs on a machine where the library then cannot be loaded.
    python scripts/check-wheel.py dist/*.whl
Exit status 0 only if every wheel's tag matches the architecture of the forkdb library in it."""
import re
import struct
import sys
import zipfile

MACHO_CPU = {0x0100000C: "arm64", 0x01000007: "x86_64"}
PE_MACHINE = {0x8664: "x86_64", 0xAA64: "arm64", 0x14C: "i386"}
ELF_MACHINE = {0x3E: "x86_64", 0xB7: "arm64", 0x03: "i386"}


def describe(b):
    """(format, architecture) of a shared library, from its header."""
    if b[:4] == b"\xcf\xfa\xed\xfe":
        return "macho", MACHO_CPU.get(struct.unpack("<I", b[4:8])[0], "unknown")
    if b[:4] in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
        return "macho", "universal"
    if b[:2] == b"MZ":
        pe = struct.unpack("<I", b[0x3C:0x40])[0]
        return "pe", PE_MACHINE.get(struct.unpack("<H", b[pe + 4:pe + 6])[0], "unknown")
    if b[:4] == b"\x7fELF":
        return "elf", ELF_MACHINE.get(struct.unpack("<H", b[18:20])[0], "unknown")
    return "unknown", "unknown"


EXPECTED = {
    ("macho", "arm64"): r"macosx_\d+_\d+_arm64",
    ("macho", "x86_64"): r"macosx_\d+_\d+_x86_64",
    ("macho", "universal"): r"macosx_\d+_\d+_universal2",
    ("pe", "x86_64"): r"win_amd64",
    ("elf", "x86_64"): r"(manylinux\w*_x86_64|linux_x86_64)",
    ("elf", "arm64"): r"(manylinux\w*_aarch64|linux_aarch64)",
}

bad = 0
for path in sys.argv[1:]:
    with zipfile.ZipFile(path) as z:
        libs = [n for n in z.namelist() if re.search(r"forkdb/_lib/forkdb\.(so|dylib|dll)$", n)]
        wheel = [n for n in z.namelist() if n.endswith(".dist-info/WHEEL")]
        if len(libs) != 1 or len(wheel) != 1:
            print("FAIL %s: expected one library and one WHEEL file, found %r and %r" % (path, libs, wheel))
            bad += 1
            continue
        fmt, arch = describe(z.read(libs[0])[:4096])
        tags = [l.split(":", 1)[1].strip() for l in z.read(wheel[0]).decode().splitlines() if l.startswith("Tag:")]
    want = EXPECTED.get((fmt, arch))
    ok = bool(want) and len(tags) == 1 and re.fullmatch(r"py3-none-" + want, tags[0]) is not None
    print("%s %s: the library is %s %s, the tag is %s" % ("ok  " if ok else "FAIL", path.replace("\\", "/").split("/")[-1], fmt, arch, ", ".join(tags)))
    if not ok:
        bad += 1
sys.exit(1 if bad else 0)
