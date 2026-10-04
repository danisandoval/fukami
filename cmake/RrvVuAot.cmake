# VU execution support shared by the product and diagnostic graphs.
#
# There is no VU JIT (removed 2026-09-30). The product's staged runtime runs
# statically recompiled VU microcode (scripts/vu_aot_overlay.py,
# src/vu-aot/rrv_vu_aot_engine.inc, generated/rr5/vu/). Graphs that compile the
# pinned producer's ps2_vu1.cpp unmodified link this interface library, whose
# rrv_vu_jit.h never creates an engine, so they interpret.
include_guard(GLOBAL)

function(rrv_add_vu_compat target)
    add_library(${target} INTERFACE)
    target_include_directories(${target} INTERFACE "${CMAKE_SOURCE_DIR}/src/vu-aot/compat")
endfunction()
