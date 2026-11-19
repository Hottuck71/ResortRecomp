"""Create the native build's NAND folder: console settings a real Wii already has.

Writes shared2/sys/SYSCONF and title/00000001/00000002/data/setting.txt under
exports/native-nand. Both are made from scratch using the public formats
(wiibrew.org/wiki//shared2/sys/SYSCONF and /title/00000001/00000002/data/setting.txt).
No console's data is copied, and the serial number is a placeholder.

Existing files are left alone, so saves and changed settings survive. Pass --force to
rewrite the two settings files. Saves are never touched.

Usage: python scripts/45_make_nand.py [--force]
"""
import struct
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
nand = root / "exports/native-nand"
force = "--force" in sys.argv

BIG, SMALL, BYTE, LONG, BOOL = 1, 2, 3, 5, 7

# Name, type, value. Values are this project's defaults for a US English console.
SETTINGS = [
    ("BT.DINF", BIG, bytes(0x461)),
    ("BT.CDIF", BIG, bytes(0x205)),
    ("BT.SENS", LONG, 3),
    ("BT.BAR", BYTE, 1),      # sensor bar above the TV
    ("BT.SPKV", BYTE, 0x58),  # Wii Remote speaker volume
    ("BT.MOT", BYTE, 1),      # rumble on
    ("IPL.NIK", SMALL, "Wii".encode("utf-16-be").ljust(22, b"\0")),
    ("IPL.LNG", BYTE, 1),     # English
    ("IPL.SADR", BIG, bytes([49]) + bytes(0x1007)),  # United States
    ("IPL.PC", SMALL, bytes(0x4A)),
    ("IPL.CB", LONG, 0),
    ("IPL.AR", BYTE, 1),      # widescreen
    ("IPL.SSV", BYTE, 1),
    ("IPL.CD", BOOL, 1),      # setup finished
    ("IPL.CD2", BOOL, 1),
    ("IPL.EULA", BOOL, 1),
    ("IPL.UPT", BYTE, 2),
    ("IPL.PGS", BYTE, 0),     # progressive scan off
    ("IPL.E60", BYTE, 1),     # 60 Hz
    ("IPL.DH", BYTE, 0),
    ("IPL.INC", LONG, 8),
    ("IPL.FRC", LONG, 0x28),
    ("IPL.IDL", SMALL, bytes(2)),
    ("NET.WCFG", LONG, 1),
    ("NET.CTPC", LONG, 0),
    ("WWW.RST", BYTE, 0),
    ("MPLS.MOVIE", BOOL, 1),
]


def item(name, kind, value):
    head = bytes([(kind << 5) | (len(name) - 1)]) + name.encode()
    if kind == BIG:
        return head + struct.pack(">H", len(value) - 1) + value
    if kind == SMALL:
        return head + bytes([len(value) - 1]) + value
    if kind == LONG:
        return head + struct.pack(">I", value)
    return head + bytes([value])


def make_sysconf():
    table_end = 6 + 2 * (len(SETTINGS) + 1)
    body = bytearray()
    offsets = []
    for setting in SETTINGS:
        offsets.append(table_end + len(body))
        body += item(*setting)
    offsets.append(table_end + len(body))
    data = b"SCv0" + struct.pack(">H", len(SETTINGS))
    data += struct.pack(f">{len(offsets)}H", *offsets) + bytes(body)
    if len(data) > 0x4000 - 4:
        sys.exit("SYSCONF is too large")
    return data.ljust(0x4000 - 4, b"\0") + b"SCed"


def make_setting_txt():
    text = (
        "AREA=USA\r\nMODEL=RVL-001(USA)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LU\r\n"
        "SERNO=000000000\r\nVIDEO=NTSC\r\nGAME=US\r\n"
    ).encode()
    plain = text.ljust(0x100, b"\0")
    key = 0x73B5DBFA
    out = bytearray()
    for byte in plain:
        out.append(byte ^ (key & 0xFF))
        key = ((key << 1) | (key >> 31)) & 0xFFFFFFFF
    return bytes(out)


def write(relative, data):
    path = nand / relative
    if path.exists() and not force:
        print(f"Kept {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    print(f"Wrote {path} ({len(data)} bytes)")


write("shared2/sys/SYSCONF", make_sysconf())
write("title/00000001/00000002/data/setting.txt", make_setting_txt())
(nand / "title/00010000/525a5445/data").mkdir(parents=True, exist_ok=True)
(nand / "tmp").mkdir(parents=True, exist_ok=True)
