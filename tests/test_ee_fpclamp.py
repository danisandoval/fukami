#!/usr/bin/env python3
"""Asset-free test of the committed EE/VU0 clamp helpers (RRV_EE_FPCLAMP).

Reads third_party/ps2recomp/ps2xRuntime/include/ps2_runtime_macros.h and the
recompiler's third_party/ps2recomp/ps2xRecomp/src/lib/code_generator.cpp,
compiles the helpers and checks them against the PCSX2 rules the
NaN census (known_issues NAN-001) measured: the x-x=0 idiom on a NaN lane,
compares against 0x7FFFFFFF, overflow saturation, FTOI saturation, fp_max,
DIV/RSQRT zero cases and the CLIP bit order. RRV_EE_FPCLAMP=0 must restore the
old expressions.
"""
from __future__ import annotations

import os
import platform
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

MACROS = ROOT / "third_party/ps2recomp/ps2xRuntime/include/ps2_runtime_macros.h"
GEN = ROOT / "third_party/ps2recomp/ps2xRecomp/src/lib/code_generator.cpp"


def sse2neon_dir() -> Path | None:
    """sse2neon is only needed on ARM hosts (x86 uses <immintrin.h>)."""
    candidates = [os.environ.get("SSE2NEON_DIR")]
    candidates += [str(p) for p in ROOT.glob("build-deps/sse2neon-*")]
    candidates += [str(p) for p in ROOT.glob("build*/_deps/sse2neon-src")]
    for c in candidates:
        if c and (Path(c) / "sse2neon.h").is_file():
            return Path(c)
    return None

TEST_MAIN = r'''
#include <cstdio>
static uint32_t B(float f){uint32_t u;std::memcpy(&u,&f,4);return u;}
static float F(uint32_t u){float f;std::memcpy(&f,&u,4);return f;}
static __m128 V(uint32_t x,uint32_t y,uint32_t z,uint32_t w){return _mm_castsi128_ps(_mm_set_epi32(w,z,y,x));}
static uint32_t L(__m128 v,int i){uint32_t a[4];std::memcpy(a,&v,16);return a[i];}
static int fails=0;
#define EXPECT(c,msg) do{ if(!(c)){ std::printf("FAIL %s\n",msg); ++fails;} }while(0)
int main(){
  __m128 a=V(0,0,0,0xFFFF8123u);
  EXPECT(L(PS2_VSUB(a,a),3)==0u,"x-x on a NaN-bit lane is +0");
  EXPECT(L(PS2_VMULQ(V(0x7F000000u,0,0,0),F(0x7F000000u)),0)==0x7F7FFFFFu,"overflow -> +Fmax");
  EXPECT(L(PS2_VMUL(V(0,0,0,0),V(0xFFFFFB85u,0,0,0)),0)==0x80000000u,"0 * NaN-bits = -0");
  __m128 t=ps2VuFtoi(V(0x7F800000u,0xFF800000u,B(2.5f),B(3e9f)),16.0f);
  EXPECT(L(t,0)==0x7FFFFFFFu&&L(t,1)==0x80000000u&&L(t,2)==40u&&L(t,3)==0x7FFFFFFFu,"FTOI4");
  __m128 mx=ps2VuMax(V(0x7FFFFFFFu,B(-1.f),B(-2.f),0),V(B(1.f),B(-3.f),B(-1.f),0x80000000u));
  EXPECT(L(mx,0)==0x7FFFFFFFu&&L(mx,1)==B(-1.f)&&L(mx,2)==B(-1.f)&&L(mx,3)==0u,"MAX");
  __m128 mn=ps2VuMin(V(B(1.f),B(-1.f),B(-2.f),0),V(B(2.f),B(-3.f),B(-1.f),0x80000000u));
  EXPECT(L(mn,0)==B(1.f)&&L(mn,1)==B(-3.f)&&L(mn,2)==B(-2.f)&&L(mn,3)==0x80000000u,"MINI");
  EXPECT(B(ps2Vu0Div(1.0f,F(0x80000000u)))==0xFF7FFFFFu,"1/-0");
  EXPECT(B(ps2Vu0Div(F(0x7F000000u),F(0x00800000u)))==0x7F7FFFFFu,"DIV overflow");
  EXPECT(B(ps2Vu0Rsqrt(0.0f,F(0x80000000u)))==0x80000000u,"RSQRT 0/-0");
  EXPECT(ps2Vu0Sqrt(-4.0f)==2.0f,"SQRT");
  EXPECT(ps2Vu0Clip(V(B(2.f),B(-2.f),0,0),V(B(1.f),B(1.f),B(1.f),B(1.f)))==0x9u,"CLIP bit order");
  EXPECT(ps2Vu0Clip(V(0x7FFFFFFFu,0,0,0),V(B(1.f),B(1.f),B(1.f),B(1.f)))==0x1u,"CLIP NaN-bits");
  EXPECT(FPU_C_OLT_S(1.0f,F(0x7FFFFFFFu)),"C.LT.S vs 0x7FFFFFFF");
  EXPECT(FPU_C_OLT_S(F(0xFFFFFFFFu),F(0x7FFFFFFFu)),"C.LT.S -Fmax < +Fmax");
  EXPECT(B(FPU_SQRT_S(F(0x80000001u)))==0x80000000u,"SQRT.S denormal");
  std::printf("%d\n",fails); return 0;
}
'''


class EeFpClampTests(unittest.TestCase):
    def test_generator_emits_helpers(self):
        gen = GEN.read_text()
        for needle in ("ps2VuMax(", "ps2VuMin(", "ps2VuFtoi(src", "ps2Vu0Div(fs, ft)",
                       "ps2Vu0Rsqrt(fs, ft)", "ps2Vu0Sqrt(ft)", "ps2Vu0Clip(fs, ft)",
                       '"__m128 mul_res = PS2_VMUL(', '"__m128 res = PS2_VMUL('):
            self.assertIn(needle, gen)
        for gone in ("_mm_max_ps(", "_mm_min_ps(", "_mm_cvttps_epi32", "_mm_cmpgt_ps(fs, ft)"):
            self.assertNotIn(gone, gen)

    def test_helpers_match_pcsx2(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "a C++ compiler is required")
        neon = sse2neon_dir()
        arm = platform.machine().lower() in ("arm64", "aarch64")
        if arm:
            self.assertIsNotNone(neon, "ARM host needs sse2neon.h (SSE2NEON_DIR, build-deps/sse2neon-*, or build*/_deps/sse2neon-src)")
        mac = MACROS.read_text()
        start = mac.index("// RRV_EE_FPCLAMP (scripts/ee_fpclamp_overlay.py)")
        end = mac.index("#define PS2_VBLEND")
        fpu = mac[mac.index("inline uint32_t ps2FpuBits(float f)"):mac.index("// FPU (COP1) operations")]
        defines = "\n".join(line for line in mac.splitlines()
                            if line.startswith(("#define FPU_SQRT_S", "#define FPU_C_EQ_S",
                                                "#define FPU_C_OLT_S", "#define FPU_C_OLE_S")))
        prelude = ("#include <cstdint>\n#include <cstring>\n#include <cmath>\n"
                   + ('#include "sse2neon.h"\n' if arm else "#include <immintrin.h>\n"))
        source = prelude + mac[start:end] + fpu + defines + TEST_MAIN
        with tempfile.TemporaryDirectory(prefix="rrv-ee-fpclamp-") as directory:
            cpp = Path(directory) / "t.cpp"
            exe = Path(directory) / "t"
            cpp.write_text(source)
            cmd = [compiler, "-std=c++20", "-O1", str(cpp), "-o", str(exe)]
            if arm:
                cmd.insert(1, f"-I{neon}")
            else:
                cmd.insert(1, "-msse4.1")
            subprocess.run(cmd, check=True)
            on = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
            self.assertEqual(on.stdout.strip().splitlines()[-1], "0", on.stdout)
            env = dict(os.environ, RRV_EE_FPCLAMP="0")
            off = subprocess.run([str(exe)], capture_output=True, text=True, check=True, env=env)
            self.assertGreater(int(off.stdout.strip().splitlines()[-1]), 0, "rollback must restore raw math")


if __name__ == "__main__":
    unittest.main()
