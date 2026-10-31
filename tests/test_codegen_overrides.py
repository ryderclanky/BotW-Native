"""Exercise generated direct and tail calls against the real runtime dispatcher."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
import sys
sys.path.insert(0, str(ROOT))
from cafe.analyze import Function
from cafe.codegen import Generator
from cafe.ppc import decode
from tools.recompile import HEADER


class OverrideTest(unittest.TestCase):
    def test_generated_calls_follow_overrides(self):
        a, b = 0x02000000, 0x02000010
        generator = Generator(None, {a, b})
        def function(addr, value):
            fn = Function(addr, {
                addr: decode(0x38600000 | value, addr),
                addr + 4: decode(0x4E800020, addr + 4),
            })
            return generator.gen_function(fn)[0]
        direct = generator.call_direct(None, 0x02000020, a, set())
        tail = generator.tail(a, set())
        source = HEADER + """
#include "guest_table.h"
#include "hle.h"
#include <cstdlib>
#include <cstdio>
extern "C" uint8_t* g_mem = nullptr;
void diag_dump(const char*) {}
void log_write(const char*, ...) {}
namespace threads { const char* current_name() { return "test"; } }
extern "C" PPC_NORETURN void rt_fatal_exit() { std::abort(); }
""" + function(a, 7) + "\n" + function(b, 9) + """
const GuestFnEntry g_guest_functions[] = {{0x02000000u, f_02000000}, {0x02000010u, f_02000010}};
const uint32_t g_guest_function_count = 2;
void direct_call(PPCContext* c) { """ + direct + """ }
void tail_call(PPCContext* c) { """ + tail + """ }
int main() {
    dispatch::init();
    PPCContext c{};
    auto check = [&](void (*call)(PPCContext*), uint32_t expected) {
        c.r[3] = 123; call(&c);
        if (c.r[3] != expected) { std::fprintf(stderr, "got %u expected %u\\n", c.r[3], expected); std::abort(); }
    };
    auto indirect = +[](PPCContext* ctx) { rt_call(ctx, 0x02000000u); };
    check(direct_call, 7); check(tail_call, 7); check(indirect, 7);
    dispatch::override_fn_ret_imm(0x02000000u, 42);
    check(direct_call, 42); check(tail_call, 42); check(indirect, 42);
    dispatch::override_fn_ret_imm(0x02000010u, 77);
    if (!dispatch::redirect_fn(0x02000000u, 0x02000010u)) std::abort();
    check(direct_call, 77); check(tail_call, 77); check(indirect, 77);
    if (dispatch::redirect_fn(0x02000010u, 0x02000000u)) std::abort();
    dispatch::override_fn_nop(0x02000000u);
    check(direct_call, 123); check(tail_call, 123); check(indirect, 123);
    dispatch::restore_fn_overrides();
    check(direct_call, 7); check(tail_call, 7); check(indirect, 7);
}
"""
        with tempfile.TemporaryDirectory(prefix="botw-override-") as directory:
            temp = Path(directory)
            (temp / "hle_decls.h").write_text("", encoding="utf-8")
            (temp / "test.cpp").write_text(source, encoding="utf-8")
            executable = temp / ("test.exe" if os.name == "nt" else "test")
            subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++20", "-pthread",
                "-I" + str(temp), "-I" + str(ROOT / "runtime" / "include"),
                "-I" + str(ROOT / "runtime" / "src"), str(temp / "test.cpp"),
                str(ROOT / "runtime" / "src" / "dispatch.cpp"), "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
