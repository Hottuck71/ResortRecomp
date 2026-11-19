"""Make the native build's copy of DolRecomp's CPU helpers, without Dolphin-derived code.

DolRecomp's src/cpu/cpu.c credits its fres and frsqrte estimate tables to Dolphin
(GPL-2.0-or-later). This writes a copy with those tables and the two functions that use
them removed. native/src/estimate.c provides replacements written for this project.
The original file isn't changed.

Usage: python scripts/40_make_cpu_core.py [source cpu.c] [output file]
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
src = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r"C:\Tools\DolRecomp\src\cpu\cpu.c")
out = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "exports/native-build/cpu_core.c"

text = src.read_text(encoding="utf-8")


def cut(pattern, label):
    global text
    text, count = re.subn(pattern, "", text, count=1, flags=re.S)
    if count != 1:
        sys.exit(f"Expected to remove {label} once, removed {count}. cpu.c may have changed.")


cut(r"/\* Adapted from Dolphin Emulator's Common/FloatUtils\.cpp\..*?\*/\n", "credit comment")
cut(r"static const EstimateEntry frsqrte_table\[32\] = \{.*?\n\};\n", "frsqrte table")
cut(r"static const EstimateEntry fres_table\[32\] = \{.*?\n\};\n", "fres table")
cut(r"f64 ppc_approx_rsqrt\(f64 value\) \{.*?\n\}\n", "ppc_approx_rsqrt")
cut(r"f64 ppc_approx_reciprocal\(f64 value\) \{.*?\n\}\n", "ppc_approx_reciprocal")

if re.search("dolphin", text, re.I):
    sys.exit("cpu.c still mentions Dolphin after the cut. Review it by hand.")

out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(text, encoding="utf-8", newline="\n")
print(f"Wrote {out} ({len(text.splitlines())} lines, no Dolphin mentions)")
