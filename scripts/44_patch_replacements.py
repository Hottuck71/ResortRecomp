"""Make the copies of the translated chunks that the native build compiles.

A call inside one 16 KiB chunk jumps straight to its target and never reaches the dispatch
loop, so a replacement has to sit at the function's own entry label. For every address in
native/replacements.txt this adds a check after that label, in a copy of the chunk under
exports/native-build/chunks-patched/. The generated originals aren't changed. Files that
come out the same aren't rewritten, so only changed chunks are rebuilt.

With --strip-pc it also removes "ctx->pc = ..." from in front of plain instructions, leaving the ones
beside control flow. It is off because it didn't pay: 1.17 million stores gone, the program 6% smaller,
and the Table Tennis VS screen went from 18.0 to 17.9 ms. Keeping them leaves --profile exact.

Usage: python scripts/44_patch_replacements.py [--strip-pc]
"""
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
chunks = root / "exports/recomp-c/generated/chunks"
out_dir = root / "exports/native-build/chunks-patched"
out_dir.mkdir(parents=True, exist_ok=True)

# Instructions that only touch registers or memory. Branches, system instructions, and anything the
# translator couldn't decode are left out on purpose.
PLAIN = """lbz lbzu lbzx lbzux lhz lhzu lhzx lhzux lha lhau lhax lhaux lwz lwzu lwzx lwzux
stb stbu stbx stbux sth sthu sthx sthux stw stwu stwx stwux lmw stmw
lfs lfsu lfsx lfsux lfd lfdu lfdx lfdux stfs stfsu stfsx stfsux stfd stfdu stfdx stfdux
psq_l psq_lu psq_lx psq_st psq_stu psq_stx
li lis addi addis addic addc adde addze add subf subfc subfe subfic subfze neg mulli mullw mulhw mulhwu
divw divwu and andc andi andis or orc ori oris xor xori xoris nor nand eqv extsb extsh cntlzw
slw srw sraw srawi rlwinm rlwimi rlwnm cmp cmpi cmpl cmpli cmpw cmpwi cmplw cmplwi
mr nop mflr mtlr mfctr mtctr mfcr mtcrf mfxer mtxer
fadd fadds fsub fsubs fmul fmuls fdiv fdivs fmadd fmadds fmsub fmsubs fnmadd fnmadds fnmsub fnmsubs
fmr fneg fabs fnabs frsp fctiw fctiwz fcmpu fcmpo fsel fres frsqrte
ps_add ps_sub ps_mul ps_div ps_madd ps_msub ps_nmadd ps_nmsub ps_madds0 ps_madds1 ps_muls0 ps_muls1
ps_sum0 ps_sum1 ps_merge00 ps_merge01 ps_merge10 ps_merge11 ps_mr ps_neg ps_abs ps_nabs ps_sel
ps_res ps_rsqrte ps_cmpu0 ps_cmpo0 ps_cmpu1 ps_cmpo1""".split()
plain_pc = re.compile(r"^    ctx->pc = 0x[0-9A-Fa-f]+u;\n((?:    ctx->downcount -= \d+;\n)?"
                      r"    // [0-9A-F]{8}: (?:" + "|".join(re.escape(op) + r"\.?" for op in PLAIN) + r")[ \t\n])", re.M)
strip_pc = "--strip-pc" in sys.argv

wanted = {}
for line in (root / "native/replacements.txt").read_text().splitlines():
    line = line.split("#")[0].strip()
    if line:
        addr, name = line.split()[:2]
        wanted[addr.upper()] = name

starts = []
for path in chunks.glob("*.c"):
    starts.append((int(re.search(r"_([0-9A-F]{8})\.c$", path.name).group(1), 16), path))
starts.sort(key=lambda item: item[0])

by_chunk = {}
for addr in wanted:
    value = int(addr, 16)
    owner = [path for start, path in starts if start <= value][-1]
    by_chunk.setdefault(owner, []).append(addr)

keep = set()
removed = 0
for _, path in starts:
    text = path.read_text(encoding="utf-8", errors="replace")
    text = text.replace('#include "../generated.h"',
                        '#include "generated.h"\nint native_replaced(CPUState* ctx, u32 pc);', 1)
    for addr in by_chunk.get(path, []):
        label = f"label_{addr}:\n"
        if text.count(label) != 1:
            sys.exit(f"Expected one {label.strip()} in {path.name} for {wanted[addr]}")
        text = text.replace(label, f"{label}    if (native_replaced(ctx, 0x{addr}u)) return;\n", 1)
    if strip_pc:
        text, count = plain_pc.subn(r"\1", text)
        removed += count
    target = out_dir / path.name
    keep.add(target.name)
    if not target.exists() or target.read_text(encoding="utf-8", errors="replace") != text:
        target.write_text(text, encoding="utf-8", newline="\n")

for old in out_dir.glob("*.c"):
    if old.name not in keep:
        old.unlink()
print(f"{len(wanted)} replacements in {len(by_chunk)} chunks, {removed} address stores removed")
