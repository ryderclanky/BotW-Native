"""PowerPC (Espresso) -> C++ code generator.

Each guest function becomes `extern "C" void f_XXXXXXXX(PPCContext* __restrict c)`. Instructions
are emitted in address order with a `#line <address>` marker each (host crash backtraces then
symbolize to guest addresses), labels only where something branches. Semantics follow the Cemu
interpreter (src/Cafe/HW/Espresso/Interpreter), including its paired-single rounding rules.
"""
from . import ppc
from .analyze import Function, bo_always
from .image import Image


def R(n: int) -> str:
    return f"c->r[{n}]"


def F(n: int) -> str:
    return f"c->f[{n}]"


def P(n: int) -> str:
    return f"c->ps1[{n}]"


def RA0(n: int) -> str:
    return "0u" if n == 0 else R(n)


def sanitize(s: str) -> str:
    return "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in s)


def hle_symbol(lib: str, name: str) -> str:
    return f"hle_{sanitize(lib)}_{sanitize(name)}"


class Generator:
    def __init__(self, img: Image, entries: set[int]):
        self.img = img
        self.entries = entries
        self.unsupported: dict[str, int] = {}
        self.imports_used: dict[str, tuple[str, str]] = {}  # hle symbol -> (lib, name)
        self.probes: set[int] = set()
        self.hooks: set[int] = set()  # host hook_0xADDR(c) runs after the (non-branch) instruction at ADDR
        self.check_returns = True

    # ---- branch conditions --------------------------------------------------------------
    def cond(self, i: ppc.Insn, pre: list[str]) -> str | None:
        """C condition for BO/BI; None means 'always'. CTR decrement goes into `pre`."""
        bo, bi = i.BO, i.BI
        parts = []
        if not bo & 0x04:
            pre.append("c->ctr--;")
            parts.append("c->ctr == 0" if bo & 0x02 else "c->ctr != 0")
        if not bo & 0x10:
            parts.append(f"c->cr[{bi}]" if bo & 0x08 else f"!c->cr[{bi}]")
        if not parts:
            return None
        return " && ".join(f"({p})" for p in parts)

    def call_direct(self, fn: Function, a: int, t: int, callees: set[str]) -> str:
        sym = f"f_{t:08x}"
        callees.add(sym)
        return f"c->lr = 0x{a + 4:08x}u; {sym}(c);"

    def tail(self, t: int, callees: set[str]) -> str:
        sym = f"f_{t:08x}"
        callees.add(sym)
        return f"[[clang::musttail]] return {sym}(c);"

    def ret(self) -> str:
        return "RETURN();"

    # ---- function ---------------------------------------------------------------------------
    def gen_function(self, fn: Function) -> tuple[str, set[str]]:
        callees: set[str] = set()
        out = [f'extern "C" void f_{fn.entry:08x}(PPCContext* __restrict c) {{',
               f"  if (__builtin_expect(g_has_guest_fn_overrides.load(std::memory_order_acquire), 0) && rt_try_override(c, 0x{fn.entry:08x}u, f_{fn.entry:08x})) return;",
               "  uint8_t* const M = g_mem; (void)M;"]
        if self.check_returns:
            out.append("  const uint32_t lr0 = c->lr; (void)lr0;")
        addrs = sorted(fn.insns)
        labels = set(fn.labels)
        if addrs and addrs[0] != fn.entry:
            # the body includes code below the entry (shared tails): start at the entry
            labels.add(fn.entry)
            out.append(f"  goto L_{fn.entry:08x};")
        body: list[tuple[int, list[str]]] = []
        for idx, a in enumerate(addrs):
            i = fn.insns[a]
            code, falls = self.insn(fn, i, callees, labels)
            nxt = addrs[idx + 1] if idx + 1 < len(addrs) else None
            if falls and nxt != a + 4:
                if a + 4 in fn.insns:
                    labels.add(a + 4)
                    code.append(f"goto L_{a + 4:08x};")
                elif a + 4 in self.entries:
                    code.append(self.tail(a + 4, callees))
                else:
                    code.append(f'rt_trap(c, 0x{a + 4:08x}u, "fell off function");')
            body.append((a, code))
        for a, code in body:
            if a in labels:
                out.append(f"L_{a:08x}:;")
            out.append(f"#line {a} \"botw.ppc\"")
            if a in self.probes:
                out.append(f"  rt_probe(c, 0x{a:08x}u);")
            for line in code:
                out.append("  " + line)
            if a in self.hooks:
                out.append(f"  hook_0x{a:08x}(c);")
                callees.add(f"hook_0x{a:08x}")
        out.append("}")
        return "\n".join(out), callees

    def unsup(self, i: ppc.Insn) -> list[str]:
        self.unsupported[i.op] = self.unsupported.get(i.op, 0) + 1
        return [f'rt_trap(c, 0x{i.addr:08x}u, "unsupported {i.op} {i.word:08x}");']

    # ---- instructions ---------------------------------------------------------------------
    def insn(self, fn: Function, i: ppc.Insn, callees: set[str], labels: set[int]) -> tuple[list[str], bool]:
        """Returns (C++ lines, falls through)."""
        op = i.op
        a = i.addr
        d, ra, rb = i.rD, i.rA, i.rB
        img = self.img

        # -- branches ---------------------------------------------------------------------
        if op == "b":
            if i.LK:
                imp = img.reloc_import(a)
                if imp is not None:
                    sym = hle_symbol(imp.lib, imp.name)
                    self.imports_used[sym] = (imp.lib, imp.name)
                    return [f'c->lr = 0x{a + 4:08x}u; HLE_CALL({sym}, "{imp.lib}.{imp.name}");'], True
                r = img.reloc_at.get(a)
                if r is not None:
                    tgt = img.reloc_target(a)
                    if tgt == 0:
                        return [f'rt_trap(c, 0x{a:08x}u, "call to undefined weak symbol");'], True
                    return [f'rt_trap(c, 0x{a:08x}u, "unhandled REL24 call");'], True
                if i.target == a + 4:
                    return [f"c->lr = 0x{a + 4:08x}u;"], True
                if not img.is_text(i.target):
                    return [f'rt_trap(c, 0x{a:08x}u, "call outside text");'], True
                return [self.call_direct(fn, a, i.target, callees)], True
            imp = img.reloc_import(a)
            if imp is not None:
                sym = hle_symbol(imp.lib, imp.name)
                self.imports_used[sym] = (imp.lib, imp.name)
                return [f'HLE_TAIL({sym}, "{imp.lib}.{imp.name}");'], False
            if img.reloc_at.get(a) is not None:
                return [f'rt_trap(c, 0x{a:08x}u, "tail call to unresolved symbol");'], False
            t = i.target
            if t in fn.tail_targets:
                return [self.tail(t, callees)], False
            labels.add(t)
            return [f"goto L_{t:08x};"], False

        if op == "bc":
            pre: list[str] = []
            cnd = self.cond(i, pre)
            t = i.target
            if i.LK:
                if t == a + 4:
                    body = f"c->lr = 0x{a + 4:08x}u;"  # bcl 20,31,$+4: read PC
                else:
                    body = self.call_direct(fn, a, t, callees)
                return pre + ([f"if ({cnd}) {{ {body} }}"] if cnd else [body]), True
            if t in fn.tail_targets:
                act = self.tail(t, callees)
            else:
                labels.add(t)
                act = f"goto L_{t:08x};"
            if cnd is None:
                return pre + [act], False
            return pre + [f"if ({cnd}) {{ {act} }}"], True

        if op == "bclr":
            pre = []
            cnd = self.cond(i, pre)
            if i.LK:
                body = f"{{ uint32_t t_ = c->lr; c->lr = 0x{a + 4:08x}u; rt_call(c, t_); }}"
                return pre + ([f"if ({cnd}) {body}"] if cnd else [body]), True
            if cnd is None:
                return pre + [self.ret()], False
            return pre + [f"if ({cnd}) {{ {self.ret()} }}"], True

        if op == "bcctr":
            pre = []
            cnd = self.cond(i, pre)
            if i.LK:
                body = f"{{ c->lr = 0x{a + 4:08x}u; rt_call(c, c->ctr); }}"
                return pre + ([f"if ({cnd}) {body}"] if cnd else [body]), True
            sw = fn.switches.get(a)
            if sw is not None:
                lines = ["switch (c->ctr) {"]
                for t in sw.targets:
                    labels.add(t)
                    lines.append(f"  case 0x{t:08x}u: goto L_{t:08x};")
                lines.append(f"  default: rt_bad_switch(c, 0x{a:08x}u, c->ctr); return;")
                lines.append("}")
                return lines, False
            body = "{ rt_call(c, c->ctr); RETURN_TAIL(); }"
            if cnd is None:
                return pre + [body], False
            return pre + [f"if ({cnd}) {body}"], True

        if op == "tw":
            if d == 31:
                return [f'rt_trap(c, 0x{a:08x}u, "tw (trap)");'], False
            return [f"if ({self.trap_cond(d, R(ra), R(rb))}) rt_trap(c, 0x{a:08x}u, \"tw\");"], True
        if op == "twi":
            return [f"if ({self.trap_cond(d, R(ra), f'(uint32_t){i.imm}')}) rt_trap(c, 0x{a:08x}u, \"twi\");"], True
        if op in ("illegal", "sc", "rfi"):
            return [f'rt_trap(c, 0x{a:08x}u, "{op} {i.word:08x}");'], False

        lines = self.simple(i)
        if lines is None:
            return self.unsup(i), True
        return lines, True

    @staticmethod
    def trap_cond(to: int, x: str, y: str) -> str:
        p = []
        if to & 16: p.append(f"(int32_t){x} < (int32_t){y}")
        if to & 8: p.append(f"(int32_t){x} > (int32_t){y}")
        if to & 4: p.append(f"{x} == {y}")
        if to & 2: p.append(f"{x} < {y}")
        if to & 1: p.append(f"{x} > {y}")
        return " || ".join(p) if p else "0"

    # ---- non-branch instructions ------------------------------------------------------------
    def simple(self, i: ppc.Insn) -> list[str] | None:
        op = i.op
        d, ra, rb, rc = i.rD, i.rA, i.rB, i.rC
        imm = i.imm
        a = i.addr
        L: list[str] = []
        rec = (lambda v: L.append(f"CR0({v});")) if i.Rc else (lambda v: None)

        # integer arithmetic (D-form)
        if op == "addi":
            L.append(f"{R(d)} = {RA0(ra)} + (uint32_t){imm};")
            return L
        if op == "addis":
            L.append(f"{R(d)} = {RA0(ra)} + 0x{(imm << 16) & 0xFFFFFFFF:08x}u;")
            return L
        if op in ("addic", "addic."):
            L.append(f"{{ uint32_t a_ = {R(ra)}; {R(d)} = a_ + (uint32_t){imm}; c->ca = {R(d)} < a_; }}")
            if op == "addic.":
                L.append(f"CR0({R(d)});")
            return L
        if op == "subfic":
            L.append(f"{{ uint32_t a_ = {R(ra)}; {R(d)} = adde_(c, ~a_, (uint32_t){imm}, 1); }}")
            return L
        if op == "mulli":
            L.append(f"{R(d)} = (uint32_t)((int32_t){R(ra)} * (int32_t){imm});")
            return L
        if op in ("cmpi", "cmpli", "cmp", "cmpl"):
            f = d
            if op == "cmpi":
                L.append(f"set_cr_s(c, {f}, (int32_t){R(ra)}, {imm});")
            elif op == "cmpli":
                L.append(f"set_cr_u(c, {f}, {R(ra)}, {imm}u);")
            elif op == "cmp":
                L.append(f"set_cr_s(c, {f}, (int32_t){R(ra)}, (int32_t){R(rb)});")
            else:
                L.append(f"set_cr_u(c, {f}, {R(ra)}, {R(rb)});")
            return L
        # logical immediates: rS=d, rA=ra
        if op in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
            v = imm << 16 if op.endswith(("is", "is.")) else imm
            sym = {"o": "|", "x": "^", "a": "&"}[op[0]]
            L.append(f"{R(ra)} = {R(d)} {sym} 0x{v:08x}u;")
            if op.startswith("and"):
                L.append(f"CR0({R(ra)});")
            return L
        # rotates
        if op == "rlwinm":
            m = self.mask(i.mb, i.me)
            sh = imm & 31
            src = R(d) if sh == 0 else f"rotl32({R(d)}, {sh})"
            L.append(f"{R(ra)} = {src} & 0x{m:08x}u;")
            rec(R(ra))
            return L
        if op == "rlwimi":
            m = self.mask(i.mb, i.me)
            sh = imm & 31
            L.append(f"{R(ra)} = (rotl32({R(d)}, {sh}) & 0x{m:08x}u) | ({R(ra)} & 0x{(~m) & 0xFFFFFFFF:08x}u);")
            rec(R(ra))
            return L
        if op == "rlwnm":
            m = self.mask(i.mb, i.me)
            L.append(f"{R(ra)} = rotl32({R(d)}, {R(rb)} & 31) & 0x{m:08x}u;")
            rec(R(ra))
            return L

        # X-form logical: rS=d, rA=ra, rB=rb
        logic = {"and": "{s} & {b}", "or": "{s} | {b}", "xor": "{s} ^ {b}", "nand": "~({s} & {b})",
                 "nor": "~({s} | {b})", "eqv": "~({s} ^ {b})", "andc": "{s} & ~{b}", "orc": "{s} | ~{b}"}
        if op in logic:
            L.append(f"{R(ra)} = {logic[op].format(s=R(d), b=R(rb))};")
            rec(R(ra))
            return L
        if op == "extsb":
            L.append(f"{R(ra)} = (uint32_t)(int32_t)(int8_t){R(d)};")
            rec(R(ra))
            return L
        if op == "extsh":
            L.append(f"{R(ra)} = (uint32_t)(int32_t)(int16_t){R(d)};")
            rec(R(ra))
            return L
        if op == "cntlzw":
            L.append(f"{R(ra)} = {R(d)} ? (uint32_t)__builtin_clz({R(d)}) : 32u;")
            rec(R(ra))
            return L
        if op == "slw":
            L.append(f"{{ uint32_t n_ = {R(rb)} & 63; {R(ra)} = n_ & 32 ? 0u : {R(d)} << n_; }}")
            rec(R(ra))
            return L
        if op == "srw":
            L.append(f"{{ uint32_t n_ = {R(rb)} & 63; {R(ra)} = n_ & 32 ? 0u : {R(d)} >> n_; }}")
            rec(R(ra))
            return L
        if op == "sraw":
            L.append(f"{{ uint32_t n_ = {R(rb)} & 63; int32_t s_ = (int32_t){R(d)}; "
                     f"if (n_ & 32) {{ {R(ra)} = (uint32_t)(s_ >> 31); c->ca = s_ < 0; }} "
                     f"else {{ {R(ra)} = (uint32_t)(s_ >> n_); c->ca = s_ < 0 && n_ && ((uint32_t)s_ & ((1u << n_) - 1)) != 0; }} }}")
            rec(R(ra))
            return L
        if op == "srawi":
            n = imm & 31
            if n == 0:
                L.append(f"{R(ra)} = {R(d)}; c->ca = 0;")
            else:
                L.append(f"{{ int32_t s_ = (int32_t){R(d)}; {R(ra)} = (uint32_t)(s_ >> {n}); "
                         f"c->ca = s_ < 0 && ((uint32_t)s_ & 0x{(1 << n) - 1:x}u) != 0; }}")
            rec(R(ra))
            return L

        # XO-form arithmetic: rD=d, rA=ra, rB=rb
        if op in ("add", "subf", "addc", "subfc", "adde", "subfe", "addze", "subfze", "addme", "subfme",
                  "neg", "mullw", "mulhw", "mulhwu", "divw", "divwu"):
            A, B = R(ra), R(rb)
            if op == "add":
                L.append(f"{R(d)} = add_ov(c, {A}, {B});" if i.OE else f"{R(d)} = {A} + {B};")
            elif op == "subf":
                L.append(f"{R(d)} = sub_ov(c, {B}, {A});" if i.OE else f"{R(d)} = {B} - {A};")
            elif op == "addc":
                L.append(f"{{ uint32_t a_ = {A}, b_ = {B}; {R(d)} = adde_(c, a_, b_, 0); }}")
            elif op == "subfc":
                L.append(f"{{ uint32_t a_ = {A}, b_ = {B}; {R(d)} = adde_(c, ~a_, b_, 1); }}")
            elif op == "adde":
                L.append(f"{{ uint32_t a_ = {A}, b_ = {B}; {R(d)} = adde_(c, a_, b_, c->ca); }}")
            elif op == "subfe":
                L.append(f"{{ uint32_t a_ = {A}, b_ = {B}; {R(d)} = adde_(c, ~a_, b_, c->ca); }}")
            elif op == "addze":
                L.append(f"{R(d)} = adde_(c, {A}, 0, c->ca);")
            elif op == "subfze":
                L.append(f"{R(d)} = adde_(c, ~{A}, 0, c->ca);")
            elif op == "addme":
                L.append(f"{R(d)} = adde_(c, {A}, 0xFFFFFFFFu, c->ca);")
            elif op == "subfme":
                L.append(f"{R(d)} = adde_(c, ~{A}, 0xFFFFFFFFu, c->ca);")
            elif op == "neg":
                L.append(f"{R(d)} = sub_ov(c, 0, {A});" if i.OE else f"{R(d)} = 0u - {A};")
            elif op == "mullw":
                if i.OE:
                    L.append(f"{{ int64_t p_ = (int64_t)(int32_t){A} * (int32_t){B}; {R(d)} = (uint32_t)p_; "
                             f"c->ov = p_ != (int32_t)p_; c->so |= c->ov; }}")
                else:
                    L.append(f"{R(d)} = (uint32_t)((int32_t){A} * (int32_t){B});")
            elif op == "mulhw":
                L.append(f"{R(d)} = (uint32_t)(((int64_t)(int32_t){A} * (int32_t){B}) >> 32);")
            elif op == "mulhwu":
                L.append(f"{R(d)} = (uint32_t)(((uint64_t){A} * {B}) >> 32);")
            elif op == "divw":
                L.append(f"{R(d)} = ppc_divw((int32_t){A}, (int32_t){B});")
            elif op == "divwu":
                L.append(f"{R(d)} = ppc_divwu({A}, {B});")
            if i.OE and op not in ("add", "subf", "neg", "mullw"):
                pass  # carry-form overflow bits are not tracked (not used by this binary)
            rec(R(d))
            return L

        # loads/stores (D-form)
        dform_ld = {"lwz": "LD32({ea})", "lbz": "LD8({ea})", "lhz": "LD16({ea})",
                    "lha": "(uint32_t)(int32_t)(int16_t)LD16({ea})"}
        dform_st = {"stw": "ST32", "stb": "ST8", "sth": "ST16"}
        base = op[:-1] if op.endswith("u") and op[:-1] in dform_ld | dform_st else op
        upd = base != op
        if base in dform_ld or base in dform_st:
            ea = f"({RA0(ra)} + (uint32_t){imm})" if not upd else f"({R(ra)} + (uint32_t){imm})"
            if upd:
                L.append(f"{{ uint32_t ea_ = {ea};")
                ea = "ea_"
            if base in dform_ld:
                L.append(f"{R(d)} = {dform_ld[base].format(ea=ea)};")
            else:
                L.append(f"{dform_st[base]}({ea}, {R(d)});")
            if upd:
                L.append(f"{R(ra)} = ea_; }}")
            return L
        # loads/stores (X-form)
        xform = {"lwzx": ("ld", "LD32({ea})"), "lbzx": ("ld", "LD8({ea})"), "lhzx": ("ld", "LD16({ea})"),
                 "lhax": ("ld", "(uint32_t)(int32_t)(int16_t)LD16({ea})"),
                 "lwbrx": ("ld", "ld32r_(M, {ea})"), "lhbrx": ("ld", "(uint32_t)ld16r_(M, {ea})"),
                 "stwx": ("st", "ST32"), "stbx": ("st", "ST8"), "sthx": ("st", "ST16"),
                 "stwbrx": ("st", "st32r_"), "sthbrx": ("st", "st16r_")}
        xbase = op.replace("ux", "x") if op.endswith("ux") else op
        if xbase in xform:
            upd = op != xbase
            kind, tmpl = xform[xbase]
            ea_e = f"({R(ra)} + {R(rb)})" if upd else f"({RA0(ra)} + {R(rb)})"
            L.append(f"{{ uint32_t ea_ = {ea_e};")
            if kind == "ld":
                L.append(f"{R(d)} = {tmpl.format(ea='ea_')};")
            elif tmpl.endswith("r_"):
                L.append(f"{tmpl}(M, ea_, {R(d)});")
            else:
                L.append(f"{tmpl}(ea_, {R(d)});")
            if upd:
                L.append(f"{R(ra)} = ea_;")
            L.append("}")
            return L
        if op == "lmw":
            L.append(f"{{ uint32_t ea_ = {RA0(ra)} + (uint32_t){imm};")
            for k in range(d, 32):
                L.append(f"{R(k)} = LD32(ea_ + {4 * (k - d)}u);")
            L.append("}")
            return L
        if op == "stmw":
            L.append(f"{{ uint32_t ea_ = {RA0(ra)} + (uint32_t){imm};")
            for k in range(d, 32):
                L.append(f"ST32(ea_ + {4 * (k - d)}u, {R(k)});")
            L.append("}")
            return L
        if op == "lswi":
            n = imm
            L.append(f"{{ uint32_t ea_ = {RA0(ra)};")
            reg = d - 1
            for k in range(n):
                if k % 4 == 0:
                    reg = (reg + 1) % 32
                    L.append(f"{R(reg)} = 0;")
                L.append(f"{R(reg)} |= (uint32_t)LD8(ea_ + {k}u) << {24 - 8 * (k % 4)};")
            L.append("}")
            return L
        if op == "stswi":
            n = imm
            L.append(f"{{ uint32_t ea_ = {RA0(ra)};")
            reg = d - 1
            for k in range(n):
                if k % 4 == 0:
                    reg = (reg + 1) % 32
                L.append(f"ST8(ea_ + {k}u, {R(reg)} >> {24 - 8 * (k % 4)});")
            L.append("}")
            return L
        if op == "lwarx":
            L.append(f"{R(d)} = ppc_lwarx(c, M, {RA0(ra)} + {R(rb)});")
            return L
        if op == "stwcx.":
            L.append(f"ppc_stwcx(c, M, {RA0(ra)} + {R(rb)}, {R(d)});")
            return L

        # floating point loads/stores
        fl = {"lfs": "s", "lfsu": "s", "lfd": "d", "lfdu": "d"}
        fs = {"stfs": "s", "stfsu": "s", "stfd": "d", "stfdu": "d"}
        if op in fl or op in fs:
            upd = op.endswith("u")
            ea = f"({R(ra) if upd else RA0(ra)} + (uint32_t){imm})"
            L.append(f"{{ uint32_t ea_ = {ea};")
            L += self.fp_mem(op[:4] if op.startswith("stf") else op[:3], d)
            if upd:
                L.append(f"{R(ra)} = ea_;")
            L.append("}")
            return L
        fx = {"lfsx": "lfs", "lfsux": "lfs", "lfdx": "lfd", "lfdux": "lfd",
              "stfsx": "stfs", "stfsux": "stfs", "stfdx": "stfd", "stfdux": "stfd"}
        if op in fx:
            upd = op.endswith("ux")
            ea = f"({R(ra) if upd else RA0(ra)} + {R(rb)})"
            L.append(f"{{ uint32_t ea_ = {ea};")
            L += self.fp_mem(fx[op], d)
            if upd:
                L.append(f"{R(ra)} = ea_;")
            L.append("}")
            return L
        if op == "stfiwx":
            L.append(f"ST32({RA0(ra)} + {R(rb)}, (uint32_t)dbits({F(d)}));")
            return L
        if op in ("psq_l", "psq_lu", "psq_st", "psq_stu", "psq_lx", "psq_lux", "psq_stx", "psq_stux"):
            upd = op in ("psq_lu", "psq_stu", "psq_lux", "psq_stux")
            if op in ("psq_l", "psq_lu", "psq_st", "psq_stu"):
                ea = f"({R(ra) if upd else RA0(ra)} + (uint32_t){imm})"
            else:
                ea = f"({R(ra) if upd else RA0(ra)} + {R(rb)})"
            fnn = "psq_load" if "_l" in op else "psq_store"
            L.append(f"{{ uint32_t ea_ = {ea}; {fnn}(c, M, ea_, {i.W}, c->gqr[{i.I}], {d});")
            if upd:
                L.append(f"{R(ra)} = ea_;")
            L.append("}")
            return L

        # SPRs / CR
        if op == "mfspr":
            spr = imm
            if spr == 8:
                L.append(f"{R(d)} = c->lr;")
            elif spr == 9:
                L.append(f"{R(d)} = c->ctr;")
            elif spr == 1:
                L.append(f"{R(d)} = ((uint32_t)c->so << 31) | ((uint32_t)c->ov << 30) | ((uint32_t)c->ca << 29) | c->xer_bc;")
            elif 896 <= spr <= 903 or 912 <= spr <= 919:
                L.append(f"{R(d)} = c->gqr[{spr & 7}];")
            elif spr in (268, 269):
                L.append(f"{R(d)} = (uint32_t)(rt_timebase() >> {32 if spr == 269 else 0});")
            else:
                return None
            return L
        if op == "mtspr":
            spr = imm
            if spr == 8:
                L.append(f"c->lr = {R(d)};")
            elif spr == 9:
                L.append(f"c->ctr = {R(d)};")
            elif spr == 1:
                L.append(f"{{ uint32_t x_ = {R(d)}; c->so = x_ >> 31; c->ov = (x_ >> 30) & 1; c->ca = (x_ >> 29) & 1; c->xer_bc = x_ & 0x7F; }}")
            elif 896 <= spr <= 903 or 912 <= spr <= 919:
                L.append(f"c->gqr[{spr & 7}] = {R(d)};")
            else:
                return None
            return L
        if op == "mftb":
            L.append(f"{R(d)} = (uint32_t)(rt_timebase() >> {32 if imm == 269 else 0});")
            return L
        if op == "mfcr":
            parts = " | ".join(f"((uint32_t)c->cr[{b}] << {31 - b})" for b in range(32))
            L.append(f"{R(d)} = {parts};")
            return L
        if op == "mtcrf":
            crm = imm
            L.append(f"{{ uint32_t v_ = {R(d)};")
            for f in range(8):
                if crm & (0x80 >> f):
                    for k in range(4):
                        b = 4 * f + k
                        L.append(f"c->cr[{b}] = (v_ >> {31 - b}) & 1;")
            L.append("}")
            return L
        if op == "mcrf":
            for k in range(4):
                if d != ra:
                    L.append(f"c->cr[{4 * d + k}] = c->cr[{4 * ra + k}];")
            return L
        if op == "mcrxr":
            L.append(f"c->cr[{4 * d}] = c->so; c->cr[{4 * d + 1}] = c->ov; c->cr[{4 * d + 2}] = c->ca; c->cr[{4 * d + 3}] = 0;")
            L.append("c->so = c->ov = c->ca = 0;")
            return L
        crops = {"crand": "{a} & {b}", "cror": "{a} | {b}", "crxor": "{a} ^ {b}", "crnand": "!({a} & {b})",
                 "crnor": "!({a} | {b})", "creqv": "!({a} ^ {b})", "crandc": "{a} & !{b}", "crorc": "{a} | !{b}"}
        if op in crops:
            L.append(f"c->cr[{d}] = (uint8_t)({crops[op].format(a=f'c->cr[{ra}]', b=f'c->cr[{rb}]')});")
            return L

        # cache / sync
        if op in ("sync", "eieio"):
            L.append("__atomic_thread_fence(__ATOMIC_SEQ_CST);")
            return L
        if op == "isync":
            L.append("__asm__ volatile(\"\" ::: \"memory\");")
            return L
        if op in ("dcbst", "dcbf", "dcbi", "dcbt", "dcbtst", "icbi"):
            return ["/* cache op */"]
        if op in ("dcbz", "dcbz_l"):
            L.append(f"memset(M + (({RA0(ra)} + {R(rb)}) & ~31u), 0, 32);")
            return L

        return self.fpu(i)

    def fp_mem(self, op: str, d: int) -> list[str]:
        if op == "lfs":
            return [f"{F(d)} = {P(d)} = f32_to_f64(LD32(ea_));"]
        if op == "lfd":
            return [f"{F(d)} = bitsd(LD64(ea_));"]
        if op == "stfs":
            return [f"ST32(ea_, f64_to_f32({F(d)}));"]
        if op == "stfd":
            return [f"ST64(ea_, dbits({F(d)}));"]
        raise ValueError(op)

    @staticmethod
    def mask(mb: int, me: int) -> int:
        m1 = 0xFFFFFFFF >> mb
        m2 = 0xFFFFFFFF if me >= 31 else (~(0xFFFFFFFF >> (me + 1))) & 0xFFFFFFFF
        return (m1 & m2) if mb <= me else (m1 | m2)

    # ---- FPU / paired singles ---------------------------------------------------------------
    def fpu(self, i: ppc.Insn) -> list[str] | None:
        op = i.op
        d, a_, b, c_ = i.rD, i.rA, i.rB, i.rC
        L: list[str] = []
        fa, fb, fc = F(a_), F(b), F(c_)
        pa, pb, pc = P(a_), P(b), P(c_)

        def single(expr: str) -> list[str]:
            # result rounded to single; ps1 mirrors ps0 (HID2[PSE] set on Cafe)
            return [f"{{ double t_ = round_single({expr}); {F(d)} = t_; {P(d)} = t_; }}"]

        dbl = {"fadd": f"{fa} + {fb}", "fsub": f"{fa} - {fb}", "fmul": f"{fa} * {fc}", "fdiv": f"{fa} / {fb}",
               "fmadd": f"fma({fa}, {fc}, {fb})", "fmsub": f"fma({fa}, {fc}, -{fb})",
               "fnmadd": f"-fma({fa}, {fc}, {fb})", "fnmsub": f"-fma({fa}, {fc}, -{fb})", "fsqrt": f"sqrt({fb})"}
        sgl = {"fadds": f"{fa} + {fb}", "fsubs": f"{fa} - {fb}", "fmuls": f"{fa} * round25({fc})",
               "fdivs": f"{fa} / {fb}", "fmadds": f"fma({fa}, round25({fc}), {fb})",
               "fmsubs": f"fma({fa}, round25({fc}), -{fb})", "fnmadds": f"-fma({fa}, round25({fc}), {fb})",
               "fnmsubs": f"-fma({fa}, round25({fc}), -{fb})", "fsqrts": f"sqrt({fb})"}
        if op in dbl:
            L.append(f"{F(d)} = {dbl[op]};")
        elif op in sgl:
            L += single(sgl[op])
        elif op == "fres":
            L.append(f"{{ double t_ = ppc_fres({fb}); {F(d)} = t_; {P(d)} = t_; }}")
        elif op == "frsqrte":
            L.append(f"{F(d)} = ppc_frsqrte({fb});")
        elif op == "frsp":
            L += single(fb)
        elif op == "fsel":
            L.append(f"if ({fa} >= -0.0) {{ {F(d)} = {fc}; {P(d)} = {pc}; }} else {{ {F(d)} = {fb}; {P(d)} = {pb}; }}")
        elif op == "fmr":
            L.append(f"{F(d)} = {fb};")
        elif op == "fneg":
            L.append(f"{F(d)} = bitsd(dbits({fb}) ^ 0x8000000000000000ULL);")
        elif op == "fabs":
            L.append(f"{F(d)} = bitsd(dbits({fb}) & ~0x8000000000000000ULL);")
        elif op == "fnabs":
            L.append(f"{F(d)} = bitsd(dbits({fb}) | 0x8000000000000000ULL);")
        elif op == "fctiwz":
            L.append(f"{F(d)} = bitsd(ppc_fctiwz({fb}));")
        elif op == "fctiw":
            L.append(f"{F(d)} = bitsd(ppc_fctiw({fb}));")
        elif op in ("fcmpu", "fcmpo"):
            L.append(f"set_cr_f(c, {d}, {fa}, {fb});")
            return L
        elif op == "mffs":
            L.append(f"{F(d)} = bitsd((uint64_t)c->fpscr);")
        elif op == "mtfsf":
            fm = i.imm
            mask = 0
            for k in range(8):
                if fm & (0x80 >> k):
                    mask |= 0xF << (28 - 4 * k)
            L.append(f"c->fpscr = ((uint32_t)dbits({fb}) & 0x{mask:08x}u) | (c->fpscr & 0x{(~mask) & 0xFFFFFFFF:08x}u);")
        elif op == "mtfsfi":
            sh = 28 - 4 * d
            L.append(f"c->fpscr = (c->fpscr & ~(0xFu << {sh})) | ({i.imm}u << {sh});")
        elif op == "mtfsb0":
            L.append(f"c->fpscr &= ~(1u << {31 - d});")
        elif op == "mtfsb1":
            L.append(f"c->fpscr |= 1u << {31 - d};")
        elif op == "mcrfs":
            sh = 28 - 4 * a_
            for k in range(4):
                L.append(f"c->cr[{4 * d + k}] = (c->fpscr >> {sh + 3 - k}) & 1;")
            return L
        # paired singles
        elif op == "ps_add":
            L += self.ps2(d, f"{fa} + {fb}", f"{pa} + {pb}")
        elif op == "ps_sub":
            L += self.ps2(d, f"{fa} - {fb}", f"{pa} - {pb}")
        elif op == "ps_mul":
            L += self.ps2(d, f"{fa} * round25({fc})", f"{pa} * round25({pc})", ftz=True)
        elif op == "ps_div":
            L += self.ps2(d, f"{fa} / {fb}", f"{pa} / {pb}")
        elif op == "ps_madd":
            L += self.ps2(d, f"(double)(float)({fa} * round25({fc})) + {fb}", f"(double)(float)({pa} * round25({pc})) + {pb}", ftz=True)
        elif op == "ps_msub":
            L += self.ps2(d, f"{fa} * round25({fc}) - {fb}", f"{pa} * round25({pc}) - {pb}")
        elif op == "ps_nmadd":
            L += self.ps2(d, f"-({fa} * round25({fc}) + {fb})", f"-({pa} * round25({pc}) + {pb})")
        elif op == "ps_nmsub":
            L += self.ps2(d, f"-({fa} * round25({fc}) - {fb})", f"-({pa} * round25({pc}) - {pb})")
        elif op == "ps_madds0":
            L.append(f"{{ double k_ = round25({fc}); double x_ = (float)({fa} * k_ + {fb}), y_ = (float)({pa} * k_ + {pb}); {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_madds1":
            L.append(f"{{ double k_ = round25({pc}); double x_ = (float)({fa} * k_ + {fb}), y_ = (float)({pa} * k_ + {pb}); {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_muls0":
            L.append(f"{{ double k_ = round25({fc}); double x_ = (float)({fa} * k_), y_ = (float)({pa} * k_); {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_muls1":
            L.append(f"{{ double k_ = round25({pc}); double x_ = (float)({fa} * k_), y_ = (float)({pa} * k_); {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_sum0":
            L.append(f"{{ double x_ = (float)({fa} + {pb}), y_ = (float)({pc}); {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_sum1":
            L.append(f"{{ double x_ = (float)({fc}), y_ = (float)({fa} + {pb}); {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_sel":
            L.append(f"{{ double x_ = {fa} >= -0.0 ? {fc} : {fb}; double y_ = {pa} >= -0.0 ? {pc} : {pb}; {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_res":
            L.append(f"{{ double x_ = (float)ppc_fres({fb}), y_ = (float)ppc_fres({pb}); {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_rsqrte":
            L.append(f"{{ double x_ = (float)ppc_frsqrte({fb}), y_ = (float)ppc_frsqrte({pb}); {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_mr":
            L.append(f"{{ double x_ = {fb}, y_ = {pb}; {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_neg":
            L.append(f"{{ double x_ = bitsd(dbits({fb}) ^ 0x8000000000000000ULL), y_ = bitsd(dbits({pb}) ^ 0x8000000000000000ULL); {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_abs":
            L.append(f"{{ double x_ = bitsd(dbits({fb}) & ~0x8000000000000000ULL), y_ = bitsd(dbits({pb}) & ~0x8000000000000000ULL); {F(d)} = x_; {P(d)} = y_; }}")
        elif op == "ps_nabs":
            L.append(f"{{ double x_ = bitsd(dbits({fb}) | 0x8000000000000000ULL), y_ = bitsd(dbits({pb}) | 0x8000000000000000ULL); {F(d)} = x_; {P(d)} = y_; }}")
        elif op in ("ps_merge00", "ps_merge01", "ps_merge10", "ps_merge11"):
            x = fa if op[8] == "0" else pa
            y = fb if op[9] == "0" else pb
            L.append(f"{{ double x_ = {x}, y_ = {y}; {F(d)} = x_; {P(d)} = y_; }}")
        elif op in ("ps_cmpu0", "ps_cmpo0"):
            L.append(f"set_cr_f(c, {d}, {fa}, {fb});")
            return L
        elif op in ("ps_cmpu1", "ps_cmpo1"):
            L.append(f"set_cr_f(c, {d}, {pa}, {pb});")
            return L
        else:
            return None
        if i.Rc:
            L.append("c->cr[4] = c->fpscr >> 31; c->cr[5] = (c->fpscr >> 30) & 1; c->cr[6] = (c->fpscr >> 29) & 1; c->cr[7] = (c->fpscr >> 28) & 1;")
        return L

    @staticmethod
    def ps2(d: int, e0: str, e1: str, ftz: bool = False) -> list[str]:
        if ftz:
            return [f"{{ double x_ = ftz_single({e0}), y_ = ftz_single({e1}); c->f[{d}] = x_; c->ps1[{d}] = y_; }}"]
        return [f"{{ double x_ = (float)({e0}), y_ = (float)({e1}); c->f[{d}] = x_; c->ps1[{d}] = y_; }}"]
