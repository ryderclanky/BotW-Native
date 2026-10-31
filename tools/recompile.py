"""Recompile U-King.rpx to C++.

usage: uv run python -I tools/recompile.py <U-King.rpx> <out dir>

Writes <out>/fn_NNNN.cpp (functions bucketed by guest address), big_/huge_*.cpp (oversized
functions, built at lower -O), table.cpp (guest address -> host function, import stubs),
hle_decls.h, hle_stubs.cpp (weak fallbacks that trap with the import's name) and report.txt.
Files are only rewritten when their content changes, so rebuilds stay incremental.
Env: BOTW_PROBES=addr,... adds rt_probe() register dumps at those guest addresses.
Hooks: every BOTW_HOOK(0xADDR) in runtime/src/game/*.cpp makes the code call that host function right
after the instruction at ADDR (game patches such as 60 fps).
"""
import glob
import os
import re
import sys
import time

_tools_dir = os.path.dirname(os.path.abspath(__file__))
sys.path[:] = [p for p in sys.path if os.path.abspath(p or ".") != _tools_dir]
sys.path.insert(0, os.path.join(_tools_dir, ".."))
from cafe.analyze import Analyzer  # noqa: E402
from cafe.codegen import Generator, hle_symbol  # noqa: E402
from cafe.image import Image  # noqa: E402

BUCKET = 0x8000  # guest code bytes per translation unit
BIG_LINES = 30000
HUGE_LINES = 120000

HEADER = """#include "ppc.h"
#include <atomic>
#include "hle_decls.h"
extern std::atomic<bool> g_has_guest_fn_overrides;
extern "C" bool rt_try_override(PPCContext*, uint32_t, void (*)(PPCContext*));
extern "C" PPC_NORETURN void rt_bad_return(PPCContext* c, uint32_t addr, uint32_t lr0);
#define RETURN() do { if (__builtin_expect(c->lr != lr0, 0)) rt_bad_return(c, __LINE__, lr0); return; } while (0)
#define RETURN_TAIL() return
extern "C" int g_trace_hle;
extern "C" void rt_trace_hle(PPCContext* c, const char* name, int ret);
#define HLE_CALL(f, n) do { if (__builtin_expect(g_trace_hle, 0)) { rt_trace_hle(c, n, 0); f(c); rt_trace_hle(c, n, 1); } else f(c); } while (0)
#define HLE_TAIL(f, n) do { if (__builtin_expect(g_trace_hle, 0)) { rt_trace_hle(c, n, 0); f(c); rt_trace_hle(c, n, 1); return; } [[clang::musttail]] return f(c); } while (0)
"""


def write_if_changed(path: str, text: str) -> bool:
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == text:
                return False
    with open(path, "w") as f:
        f.write(text)
    return True


def run_demo_progress(out: str, stop_at: int = 0) -> None:
    os.makedirs(out, exist_ok=True)
    print("[recomp 1/4] Decompressing & parsing U-King.rpx...", flush=True)
    time.sleep(0.08)
    print("[recomp 2/4] Analyzing 66137 PowerPC functions...", flush=True)
    time.sleep(0.08)
    total = 258
    limit = stop_at if 0 < stop_at <= total else total
    for idx in range(1, limit + 1):
        pct = (idx * 100) // total
        shard_name = f"fn_{idx - 1:04x}.cpp" if idx <= 220 else f"big_{0x02000000 + idx * 0x1000:08x}.cpp"
        print(f"[recomp 3/4] Generating C++ shard {idx}/{total} ({pct}%) [{shard_name}]...", flush=True)
        if stop_at > 0 and idx == stop_at:
            time.sleep(30.0)
            return
        time.sleep(0.01)
    print("[recomp 4/4] Writing dispatch table & HLE stubs (100%)", flush=True)


def main() -> None:
    if len(sys.argv) >= 3 and sys.argv[1].startswith("--demo-progress"):
        stop_at = 0
        if "=" in sys.argv[1]:
            stop_at = int(sys.argv[1].split("=", 1)[1])
        run_demo_progress(sys.argv[2], stop_at)
        return

    if len(sys.argv) != 3 or sys.argv[1] in ("-h", "--help"):
        print("usage: recompile.py <path to code/U-King.rpx> <output folder, usually generated>",
              file=sys.stderr)
        sys.exit(2)
    rpx_path, out = sys.argv[1], sys.argv[2]
    if not os.path.isfile(rpx_path):
        print(f"recompile.py: U-King.rpx not found: {rpx_path}", file=sys.stderr)
        sys.exit(2)
    os.makedirs(out, exist_ok=True)
    t0 = time.time()
    print("[recomp 1/4] Decompressing & parsing U-King.rpx...", flush=True)
    img = Image(rpx_path)
    an = Analyzer(img)
    an.run(progress=True)
    t1 = time.time()
    gen = Generator(img, an.entries)
    for tok in os.environ.get("BOTW_PROBES", "").split(","):
        if tok.strip():
            gen.probes.add(int(tok, 16))
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    for path in glob.glob(os.path.join(root, "runtime", "src", "game", "*.cpp")):
        with open(path) as f:
            gen.hooks.update(int(h, 16) for h in re.findall(r"^BOTW_HOOK\((0x[0-9a-fA-F]+)\)", f.read(), re.M))

    defined = {f"f_{entry:08x}" for entry in an.functions}
    bucket_entries: dict[int, list[int]] = {}
    for entry in sorted(an.functions):
        bucket_entries.setdefault((entry - img.text_lo) // BUCKET, []).append(entry)

    est_oversized = sum(1 for fn in an.functions.values() if len(fn.insns) >= 4200)
    total_shards = max(1, len(bucket_entries) + est_oversized)

    buckets: dict[str, list[str]] = {}
    undefined = set()
    written = 0
    shards_done = 0

    def flush_shard(key: str, codes: list[str], callees: set[str], remaining_groups: int) -> None:
        nonlocal written, shards_done, total_shards
        decls = sorted(c for c in callees if c.startswith(("f_", "hook_")))
        undefined.update(d for d in decls if d.startswith("f_") and d not in defined)
        text = HEADER + "".join(f'extern "C" void {d}(PPCContext*);\n' for d in decls) + "\n" + "\n\n".join(codes) + "\n"
        written += write_if_changed(os.path.join(out, f"{key}.cpp"), text)
        buckets[key] = codes
        shards_done += 1
        if remaining_groups == 0:
            total_shards = shards_done
        elif shards_done + remaining_groups > total_shards:
            total_shards = shards_done + remaining_groups
        pct = (shards_done * 100) // max(1, total_shards)
        print(f"[recomp 3/4] Generating C++ shard {shards_done}/{total_shards} ({pct}%) [{key}.cpp]...", flush=True)

    sorted_b_ids = sorted(bucket_entries)
    for b_pos, b_id in enumerate(sorted_b_ids):
        remaining_groups = len(sorted_b_ids) - 1 - b_pos
        fn_key = f"fn_{b_id:04x}"
        fn_codes: list[str] = []
        fn_callees: set[str] = set()
        for entry in bucket_entries[b_id]:
            code, callees = gen.gen_function(an.functions[entry])
            n = code.count("\n") + 1
            if n > BIG_LINES:
                big_key = f"{'huge' if n > HUGE_LINES else 'big'}_{entry:08x}"
                flush_shard(big_key, [code], set(callees), remaining_groups + (1 if fn_codes else 0))
            else:
                fn_codes.append(code)
                fn_callees.update(callees)
        if fn_codes:
            flush_shard(fn_key, fn_codes, fn_callees, remaining_groups)

    t2 = time.time()

    for name in os.listdir(out):
        if name.endswith(".cpp") and name.startswith(("fn_", "big_", "huge_")) and name[:-4] not in buckets:
            os.remove(os.path.join(out, name))

    print("[recomp 4/4] Writing dispatch table & HLE stubs (100%)", flush=True)
    # every import gets a symbol (also the ones only referenced as data, e.g. function pointers)
    imports = {}
    for imp in img.rpx.imports:
        if imp.name.startswith("."):
            continue  # section symbol
        addr = img.data_import_addr.get(imp.sym_index, imp.addr)
        imports[addr] = (imp.lib, imp.name, imp.is_data)
    syms = {}
    for addr, (lib, name, is_data) in imports.items():
        if not is_data:
            syms[hle_symbol(lib, name)] = (lib, name)
    syms.update(gen.imports_used)

    t = ['#include "ppc.h"', '#include "hle_decls.h"', '#include "guest_table.h"']
    t += [f'extern "C" void {d}(PPCContext*);' for d in sorted(defined)]
    t.append("const GuestFnEntry g_guest_functions[] = {")
    t += [f"  {{0x{e:08x}u, f_{e:08x}}}," for e in sorted(an.functions)]
    t.append("};")
    t.append(f"const uint32_t g_guest_function_count = {len(an.functions)};")
    t.append("const GuestImportEntry g_guest_imports[] = {")
    for addr in sorted(imports):
        lib, name, is_data = imports[addr]
        fnp = "nullptr" if is_data else hle_symbol(lib, name)
        t.append(f'  {{0x{addr:08x}u, "{lib}", "{name}", {"true" if is_data else "false"}, {fnp}}},')
    t.append("};")
    t.append(f"const uint32_t g_guest_import_count = {len(imports)};")
    write_if_changed(os.path.join(out, "table.cpp"), "\n".join(t) + "\n")

    decl = ["#pragma once", '#include "ppc.h"'] + [f'extern "C" void {s}(PPCContext*);' for s in sorted(syms)]
    write_if_changed(os.path.join(out, "hle_decls.h"), "\n".join(decl) + "\n")
    stubs = ['#include "ppc.h"',
             'extern "C" PPC_NORETURN void rt_unimplemented(PPCContext* c, const char* lib, const char* name);']
    for s in sorted(syms):
        lib, name = syms[s]
        stubs.append(f'extern "C" __attribute__((weak)) void {s}(PPCContext* c) {{ rt_unimplemented(c, "{lib}", "{name}"); }}')
    write_if_changed(os.path.join(out, "hle_stubs.cpp"), "\n".join(stubs) + "\n")

    rep = [f"{len(an.functions)} functions ({an.n_discovered} discovered, {an.gap_functions} gap), "
           f"{sum(len(f.insns) for f in an.functions.values())} insns, {len(buckets)} TUs ({written} changed), "
           f"{sum(len(f.switches) for f in an.functions.values())} switches; "
           f"analysis {t1 - t0:.0f}s, codegen {t2 - t1:.0f}s",
           f"unsupported: {sorted(gen.unsupported.items(), key=lambda x: -x[1])}",
           f"undefined callees: {sorted(undefined)[:50]}",
           f"analysis problems: {len(an.problems)}"] + ["  " + p for p in an.problems[:100]]
    write_if_changed(os.path.join(out, "report.txt"), "\n".join(rep) + "\n")
    print("\n".join(rep[:4]), flush=True)


if __name__ == "__main__":
    main()
