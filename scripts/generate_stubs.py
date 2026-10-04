#!/usr/bin/env python3
"""
generate_stubs.py — emit src/rrv_stubs.cpp from config/rrv.toml.

Priority order for each stub name:
  1. ps2_stubs::name    — runtime already has an HLE implementation
  2. ps2_syscalls::name — EE kernel call
  3. rrv_hle::XXX_stub  — game-specific implementation in src/hle/
  4. ps2_stubs::TODO_NAMED — last resort (logs + returns -1, eventually throws)

Run from repo root:
    python3 scripts/generate_stubs.py
"""
import re, sys, pathlib

REPO      = pathlib.Path(__file__).parent.parent
TOML      = REPO / "config" / "rrv.toml"
CALL_LIST = REPO / "tools" / "PS2Recomp" / "ps2xRuntime" / "include" / "ps2_call_list.h"
OUT       = REPO / "src" / "rrv_stubs.cpp"

if not TOML.exists():
    sys.exit(f"ERROR: {TOML} not found. Run ./scripts/analyze.sh first.")
if not CALL_LIST.exists():
    sys.exit(f"ERROR: {CALL_LIST} not found. Clone tools/PS2Recomp first.")

# ── Build ps2_stubs:: and ps2_syscalls:: name sets ────────────────────────
src = CALL_LIST.read_text()
stub_block    = re.search(r'#define\s+PS2_STUB_LIST\s*\(X\)(.*?)(?=#define|\Z)', src, re.DOTALL)
syscall_block = re.search(r'#define\s+PS2_SYSCALL_LIST\s*\(X\)(.*?)(?=#define|\Z)', src, re.DOTALL)
stubs_ns    = set(re.findall(r'\bX\((\w+)\)', stub_block.group(1)))    if stub_block    else set()
syscalls_ns = set(re.findall(r'\bX\((\w+)\)', syscall_block.group(1))) if syscall_block else set()

# ── rrv_hle:: override table: stub_name → rrv_hle::function ─────────────
# Add entries here whenever src/hle/ gains a new implementation.
RRV_HLE = {
    # ── float math ───────────────────────────────────────────────────────
    "cosf":                   "cosf_stub",
    "sinf":                   "sinf_stub",
    "sqrtf":                  "sqrtf_stub",
    "floorf":                 "floorf_stub",
    "atanf":                  "atanf_stub",
    "atan2f":                 "atan2f_stub",
    "__ieee754_atan2":        "ieee754_atan2_stub",
    "__ieee754_atan2f":       "ieee754_atan2f_stub",
    "__ieee754_log":          "ieee754_log_stub",
    "__ieee754_log10":        "ieee754_log10_stub",
    "__ieee754_pow":          "ieee754_pow_stub",
    "__ieee754_sqrt":         "ieee754_sqrt_stub",
    "__ieee754_sqrtf":        "ieee754_sqrtf_stub",
    "__ieee754_rem_pio2f":    "ieee754_rem_pio2f_stub",
    "__kernel_rem_pio2f":     "kernel_rem_pio2f_stub",
    "__muldi3":               "muldi3_stub",
    "__divdi3":               "divdi3_stub",     # also in ps2_stubs but routing explicitly
    "__moddi3":               "moddi3_stub",
    "__udivdi3":              "udivdi3_stub",
    "__umoddi3":              "umoddi3_stub",
    "__fixunsdfdi":           "fixunsdfdi_stub",
    "__floatdidf":            "floatdidf_stub",
    "__floatdisf":            "floatdisf_stub",
    "__negdf2":               "negdf2_stub",
    "__negsf2":               "negsf2_stub",
    "dpadd":                  "dpadd_stub",
    "dpcmp":                  "dpcmp_stub",
    "dpdiv":                  "dpdiv_stub",
    "dpmul":                  "dpmul_stub",
    "dpsub":                  "dpsub_stub",
    "dptofp":                 "dptofp_stub",
    "dptoli":                 "dptoli_stub",
    "dptoul":                 "dptoul_stub",
    "fptodp":                 "fptodp_stub",
    "fptosi":                 "fptosi_stub",
    "fptoui":                 "fptoui_stub",
    "litodp":                 "litodp_stub",
    "sitofp":                 "sitofp_stub",
    "ftoi":                   "ftoi_stub",
    "itof":                   "itof_stub",
    "rint":                   "rint_stub",
    "fpcmp":                  "fpcmp_stub",
    "fpadd":                  "fpadd_stub",
    "fpdiv":                  "fpdiv_stub",
    "fpmul":                  "fpmul_stub",
    "fpsub":                  "fpsub_stub",
    "__fpcmp_parts_d":        "fpcmp_parts_stub",
    "__fpcmp_parts_f":        "fpcmp_parts_stub",
    "_fpadd_parts":           "fpadd_parts_stub",
    "matherr":                "matherr_stub",
    "exponent":               "exponent_stub",
    # ── sceDevGif ─────────────────────────────────────────────────────────
    "sceDevGifPutFifo":       "devGifPutFifo_stub",
    "sceDevGifSync":          "devGifSync_stub",
    "sceDevGifGetCnd":        "devGifGetCnd_stub",
    "sceDevGifContinue":      "devGifContinue_stub",
    "sceDevGifPutImtMode":    "devGifPutImtMode_stub",
    "sceDevGifPutP3msk":      "devGifPutP3msk_stub",
    # ── sceDevVif0 ────────────────────────────────────────────────────────
    "sceDevVif0PutFifo":      "devVif0PutFifo_stub",
    "sceDevVif0Sync":         "devVif0Sync_stub",
    "sceDevVif0GetCnd":       "devVif0GetCnd_stub",
    "sceDevVif0Continue":     "devVif0Continue_stub",
    "sceDevVif0Pause":        "devVif0Pause_stub",
    "sceDevVif0PutErr":       "devVif0PutErr_stub",
    # ── sceDevVif1 ────────────────────────────────────────────────────────
    "sceDevVif1PutFifo":      "devVif1PutFifo_stub",
    "sceDevVif1Sync":         "devVif1Sync_stub",
    "sceDevVif1GetCnd":       "devVif1GetCnd_stub",
    "sceDevVif1GetFifo":      "devVif1GetFifo_stub",
    "sceDevVif1Continue":     "devVif1Continue_stub",
    "sceDevVif1Pause":        "devVif1Pause_stub",
    "sceDevVif1PutErr":       "devVif1PutErr_stub",
    "sceDevVif1Reset":        "devVif1Reset_stub",
    # ── sceDevVu0/1 ───────────────────────────────────────────────────────
    "sceDevVu0Sync":          "devVu0Sync_stub",
    "sceDevVu0GetCnd":        "devVu0GetCnd_stub",
    "sceDevVu0Pause":         "devVu0Pause_stub",
    "sceDevVu0PutCnd":        "devVu0PutCnd_stub",
    "sceDevVu0PutDBit":       "devVu0PutDBit_stub",
    "sceDevVu0PutTBit":       "devVu0PutTBit_stub",
    "sceDevVu1Sync":          "devVu1Sync_stub",
    "sceDevVu1GetCnd":        "devVu1GetCnd_stub",
    "sceDevVu1GetTpc":        "devVu1GetTpc_stub",
    "sceDevVu1Pause":         "devVu1Pause_stub",
    "sceDevVu1PutCnd":        "devVu1PutCnd_stub",
    "sceDevVu1PutDBit":       "devVu1PutDBit_stub",
    "sceDevVu1PutTBit":       "devVu1PutTBit_stub",
    "sceVu0MemReadQ":         "vu0MemReadQ_stub",
    "sceVu0MemWriteQ":        "vu0MemWriteQ_stub",
    "sceVuCheckBusy":         "vuCheckBusy_stub",
    # ── C++ exception / RTTI ──────────────────────────────────────────────
    "__eh_alloc":             "ret0",
    "__eh_rtime_match":       "ret0",
    "__cp_exception_info":    "ret0",
    "__cp_push_exception":    "ret0",
    "__start_cp_handler":     "ret0",
    "__throw":                "ret0",
    "__rethrow":              "ret0",
    "__sjthrow":              "ret0",
    "__sjpopnthrow":          "ret0",
    "__terminate":            "ret0",
    "__uncatch_exception":    "ret0",
    "eh_context_initialize":  "ret0",
    "eh_context_static":      "ret0",
    "new_eh_context":         "ret0",
    "__sigtramp_r":           "ret0",
    "throw_helper":           "ret0",
    "_$_9type_info":          "ret0",
    "__cplus_type_matcher":   "ret0",
    "__dynamic_cast":         "ret0",
    "__is_pointer__FPv":      "ret0",
    "__eq__C9type_infoRC9type_info": "ret0",
    "before__C9type_infoRC9type_info": "ret0",
    "dcast__C14__si_type_infoRC9type_infoiPvPC9type_infoT3": "ret0",
    "dcast__C16__user_type_infoRC9type_infoiPvPC9type_infoT3": "ret0",
    "__rtti_attr":            "ret0",
    "__rtti_class":           "ret0",
    "uncaught_exception__Fv": "ret0",
    # ── Frame-unwinding ────────────────────────────────────────────────────
    "__register_frame_info":       "ret0",
    "__register_frame_info_table": "ret0",
    "__deregister_frame_info":     "ret0",
    "__frame_state_for":           "ret0",
    "__get_dynamic_handler_chain": "ret0",
    "__get_eh_info":               "ret0",
    "add_fdes":                    "ret0",
    "count_fdes":                  "ret0",
    "copy_reg":                    "ret0",
    "end_fde_sort":                "ret0",
    "execute_cfa_insn":            "ret0",
    "extract_cie_info":            "ret0",
    "fde_merge":                   "ret0",
    "fde_split":                   "ret0",
    "find_exception_handler":      "ret0",
    "find_fde":                    "ret0",
    "frame_init":                  "ret0",
    "get_reg_addr":                "ret0",
    "next_stack_level":            "ret0",
    "old_find_exception_handler":  "ret0",
    "decode_sleb128":              "ret0",
    "decode_uleb128":              "ret0",
    # ── libc stdio internals ───────────────────────────────────────────────
    "__sfp":         "ret0",
    "__sfmoreglue":  "ret0",
    "__sfvwrite":    "ret0",
    "__smakebuf":    "ret0",
    "__sprint":      "ret0",
    "__sread":       "ret0",
    "__sseek":       "ret0",
    "__swrite":      "ret0",
    "__swsetup":     "ret0",
    "__sbprintf":    "ret0",
    "get_iob":       "ret0",
    "new_iob":       "ret0",
    "_fwalk":        "ret0",
    "__sinit":       "ret0",
    # ── libc dtoa/printf internals ──────────────────────────────────────────
    "_Balloc":       "ret0",
    "_Bfree":        "ret0",
    "_b2d":          "ret0",
    "_d2b":          "ret0",
    "_hi0bits":      "ret0",
    "_lo0bits":      "ret0",
    "_lshift":       "ret0",
    "_i2b":          "ret0",
    "_s2b":          "ret0",
    "_multadd":      "ret0",
    "_multiply":     "ret0",
    "_pow5mult":     "ret0",
    "_ratio":        "ret0",
    "_mprec_log10":  "ret0",
    "_ulp":          "ret0",
    "__mcmp":        "ret0",
    "__mdiff":       "ret0",
    "_dtoa_r":       "ret0",
    "_printf_r":     "ret0",
    "_printf":       "ret0",
    "_sprintf_r":    "ret0",
    "_vfprintf_r":   "ret0",
    "printfloat":    "ret0",
    "putnum":        "ret0",
    "quorem":        "ret0",
    "cvt":           "ret0",
    # ── libc reentrant allocators ───────────────────────────────────────────
    "_calloc_r":     "ret0",
    "_free_r":       "ret0",
    "_malloc_r":     "ret0",
    "_malloc_trim_r":"ret0",
    "_mbtowc_r":     "ret0",
    # ── libc misc ──────────────────────────────────────────────────────────
    "_close_r":      "ret0",
    "_sbrk_r":       "ret0",
    "sbrk":          "ret0",
    "kill":          "ret0",
    "_setlocale_r":  "ret0",
    "_signal_r":     "ret0",
    "_raise_r":      "ret0",
    "_init_signal_r":"ret0",
    "std":           "ret0",
    # ── Debug TTY ──────────────────────────────────────────────────────────
    "kprintf":        "ret0",
    "kputchar":       "ret0",
    "kputs":          "ret0",
    "serialPutchar":  "ret0",
    "deci2Putchar":   "ret0",
    # ── VSync ──────────────────────────────────────────────────────────────
    "VSync":          "ret0",
    "VSync2":         "ret0",
    # ── System init ────────────────────────────────────────────────────────
    "_InitSys":       "ret0",
    # ── SIF / IOP internals ────────────────────────────────────────────────
    "_sceSifCmdIntrHdlr":  "ret0",
    "_sceSifLoadElfPart":  "ret0",
    "_sceSifLoadModule":   "ret0",
    "_sceSifSendCmd":      "ret0",
    "isceSifSendCmd":      "ret0",
    "_sceRpcGetFPacket":   "ret0",
    "_sceRpcGetFPacket2":  "ret0",
    "_sceRpcGetPacket":    "ret0",
    "cmd_sem_init":        "ret0",
    "ncmd_prechk":         "ret0",
    "scmd_prechk":         "ret0",
    "_lf_bind":            "ret0",
    "_search_svdata":      "ret0",
    "_request_bind":       "ret0",
    "_request_call":       "ret0",
    "_request_end":        "ret0",
    "_request_rdata":      "ret0",
    "_sceVu0ecossin":      "ret0",
    # ── CD internals ───────────────────────────────────────────────────────
    "_sceCdRI":      "ret0",
    "_sceCdRM":      "ret0",
    "_sceCdWI":      "ret0",
    "_sceCdWM":      "ret0",
    "_Cdvd_cbLoop":  "ret0",
    "cd_callback":   "ret0",
    "cd_read_intr":  "ret0",
    "cdvd_exit":     "ret0",
    "fs_read_intr":  "ret0",
    # ── SPU2 ───────────────────────────────────────────────────────────────
    "_spuCB":            "ret0",
    "_spuCBThread":      "ret0",
    "sceSpu2Remote":     "ret0",
    "sceSpu2RemoteInit": "ret0",
    # ── Queue helpers ───────────────────────────────────────────────────────
    "QueuePeekReadDone":  "ret0",
    "QueuePeekWriteDone": "ret0",
}

# ── Parse stub entries from TOML ─────────────────────────────────────────
entry_re = re.compile(r'"([^"@]+)@(0x[0-9A-Fa-f]+)"')
stubs = entry_re.findall(TOML.read_text())
if not stubs:
    sys.exit("ERROR: no stub entries found in rrv.toml.")

# ── Emit ─────────────────────────────────────────────────────────────────
lines = [
    "// rrv_stubs.cpp — AUTO-GENERATED by scripts/generate_stubs.py",
    "// Do not edit by hand; re-run the generator after modifying config/rrv.toml.",
    "",
    '#include "ps2_runtime_macros.h"',
    '#include "ps2_runtime.h"',
    '#include "ps2_stubs.h"',
    '#include "ps2_syscalls.h"',
    '#include "Stubs/Common.h"',
    '#include "ps2_recompiled_stubs.h"',
    '#include "Stubs/Unimplemented.h"',
    '#include "rrv_hle.h"',
    "",
]

routed_ps2    = 0
routed_sys    = 0
routed_hle    = 0
todo_cnt      = 0

for name, addr in stubs:
    addr_int = int(addr, 16)
    fn = f"sub_{addr_int:08X}_0x{addr_int:07x}"

    if name in stubs_ns and name not in RRV_HLE:
        body = f"    ps2_stubs::{name}(rdram, ctx, runtime);"
        routed_ps2 += 1
    elif name in syscalls_ns and name not in RRV_HLE:
        body = f"    ps2_syscalls::{name}(rdram, ctx, runtime);"
        routed_sys += 1
    elif name in RRV_HLE:
        body = f"    rrv_hle::{RRV_HLE[name]}(rdram, ctx, runtime);"
        routed_hle += 1
    else:
        escaped = name.replace('"', '\\"')
        body = f'    ps2_stubs::TODO_NAMED("{escaped}", rdram, ctx, runtime);'
        todo_cnt += 1

    lines += [
        f"void {fn}(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {{",
        body,
        "}",
        "",
    ]

OUT.write_text("\n".join(lines))
print(f"Wrote {OUT}")
print(f"  {len(stubs)} stubs:  {routed_ps2} ps2_stubs::  {routed_sys} ps2_syscalls::"
      f"  {routed_hle} rrv_hle::  {todo_cnt} TODO_NAMED")
