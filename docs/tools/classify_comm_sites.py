import re, pathlib, collections
ROOTS = ["/home/nbaumann/src/pypto-ccfusion", "/home/nbaumann/src/pypto",
         "/home/nbaumann/src/pypto-lib", "/home/nbaumann/src/pypto-serving"]
call = re.compile(r'pld\.system\.(notify|wait)\s*\(', re.S)
RANKISH = re.compile(r'\b(my_rank|src_rank|src|peer|rank|tp_rank|source_tp|src_tp|owner_tp|owner|'
                     r'cp_rank|source_cp|dst|dest)\b')
rows = []
for root in ROOTS:
    for p in pathlib.Path(root).rglob("*.py"):
        if ".git" in p.parts: continue
        try: src = p.read_text()
        except Exception: continue
        for m in call.finditer(src):
            i = m.end() - 1; depth = 0
            for j in range(i, min(i + 4000, len(src))):
                if src[j] == "(": depth += 1
                elif src[j] == ")":
                    depth -= 1
                    if depth == 0: break
            args = src[m.end():j]
            mo = re.search(r'offsets\s*=\s*\[([^\]]*)\]', args)
            if not mo:
                rows.append(("NO-OFFSETS", m.group(1), "<none>")); continue
            comps = [c.strip() for c in mo.group(1).split(",")]
            rank_axes = [k for k, c in enumerate(comps) if RANKISH.search(c)]
            if len(rank_axes) == 1 and comps[rank_axes[0]].isidentifier():
                cls = "ARROW-ABLE (single rank-valued axis)"
            elif not rank_axes and all(re.fullmatch(r'-?\d+', c) for c in comps):
                cls = "AGGREGATE (all-constant offsets)"
            elif rank_axes:
                cls = "NEEDS-INSPECTION (rank in a computed expr)"
            else:
                cls = "NEEDS-INSPECTION (no rank-valued axis)"
            rows.append((cls, m.group(1), mo.group(1).strip()))
c = collections.Counter(r[0] for r in rows)
tot = len(rows)
print(f"{tot} call sites\n")
for k, v in c.most_common(): print(f"  {v:3d}  ({100*v/tot:4.1f}%)  {k}")
print("\nNEEDS-INSPECTION shapes:")
for k, v in collections.Counter(f"{r[1]:6s} offsets=[{r[2]}]" for r in rows if r[0].startswith("NEEDS")).most_common():
    print(f"  {v:2d}x  {k}")
