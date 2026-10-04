#ifndef RRV_FP_ROUNDING_H
#define RRV_FP_ROUNDING_H

// rrv_fp_rounding.h — scoped "round toward zero" for the host FPU.
//
// WHY THIS EXISTS
//   The PS2 VUs (and PCSX2's DEFAULT_VU_FP_CONTROL_REGISTER, ChopZero) round every multiply and
//   add toward zero, so VU0/VU1 microcode and the libvu0 host helpers run under FE_TOWARDZERO,
//   scoped to the call: EE COP1 arithmetic stays round-to-nearest. The scope is entered on every
//   VU0 macro-mode VCALLMS (about 880,000 times a second in the demo race) and every VU1 program.
//
//   std::fesetround is the wrong tool on x86-64: glibc writes the x87 control word (fnstcw/fldcw)
//   AND MXCSR on every call, plus a fegetround to save the old mode, plus the call itself. The
//   Steam Deck profile (docs/TESTING.md T-LINUX-DECK) put libc, led by fesetround, at about 10% of
//   the game thread. All float maths here is SSE/NEON, so only that unit's rounding bits matter.
//
// WHAT IT DOES
//   * aarch64: FPCR.RMode (bits 23:22) only, exactly the Gate-4 G4-8 code that lived in ps2_vu1.cpp.
//   * x86-64:  MXCSR.RC (bits 14:13) only is changed on entry (0b11 is toward zero); DAZ/FTZ and the
//              exception masks are kept. On exit the value MXCSR had on entry is written back, with
//              no read: a read straight after the ldmxcsr stalls behind it, and that stall was
//              most of the cost (measured, tests/fp_rounding_tests.cpp --bench). The consequence is
//              that the sticky exception FLAGS raised inside the scope are discarded on exit.
//              Nothing in the product reads them (they are masked, never trap; the VU's own MAC and
//              status flags are computed in software) except the Gate-5 owner-thread denormal
//              diagnostic (ps2_runtime.cpp, gate5OwnerCommandV1), which as a result no longer sees
//              denormal operands that occur inside a VU scope. That hypothesis was falsified on a
//              Steam Deck (docs/TESTING.md T-LINUX-DECK) and the number is a diagnostic only.
//   * Both:    a write is skipped when the mode already is what is wanted, so a nested scope (a
//              libvu0 wrapper around a VU0 microprogram) costs one register read on entry and
//              nothing on exit (x86) or one read on exit (aarch64).
//   * Elsewhere: the portable std::fesetround, as before.
//
// SEMANTICS ARE UNCHANGED for all SSE/NEON arithmetic: the same rounding bits end up in the same unit,
// and the previous mode is restored on scope exit. The x86 branch is tested against std::fesetround
// (tests/fp_rounding_tests.cpp).
//
// ONE DIFFERENCE, deliberate: on x86-64 the x87 control word is no longer touched, so inside a scope
// std::fegetround() (which glibc answers from the x87 word) still reports the outer mode, and x87
// arithmetic (long double) keeps it. Nothing under these scopes uses x87: all float maths is SSE, and
// the only `long double` in the runtime is a printf formatting helper outside any VU scope.
// The libm rounding functions (nearbyintf, lrintf...) are SSE on x86-64 and follow MXCSR.
//
// ORDERING
//   On x86 the intrinsics are fenced with a compiler memory barrier. Everything this scope guards
//   reads its operands from memory and writes its results to memory (VU register files, matrix
//   arrays), so the barrier keeps those loads after the mode is set and those stores before it is
//   restored. (An opaque call to fesetround gave that for free; an inline ldmxcsr does not.)
//   The aarch64 branch is the unchanged G4-8 asm.

#include <cfenv>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#  include <xmmintrin.h>
#  define RRV_FP_ROUNDING_X86 1
#elif defined(__aarch64__)
#  define RRV_FP_ROUNDING_A64 1
#endif

namespace rrv::fp
{

class ScopedRoundTowardZero
{
public:
#if defined(RRV_FP_ROUNDING_A64)
    // Gate-4 G4-8: fesetround changes only FPCR.RMode (bits 23:22); do the same directly, and skip
    // an FPCR write that would not change it.
    static constexpr uint64_t kRMode = 3ull << 22;
    static constexpr uint64_t kTowardZero = 3ull << 22;
    static uint64_t readFpcr() { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
    static void writeFpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" : : "r"(v)); }
    explicit ScopedRoundTowardZero(bool enabled = true) : m_enabled(enabled), m_previous(0)
    {
        if (!m_enabled)
            return;
        const uint64_t fpcr = readFpcr();
        m_previous = fpcr & kRMode;
        if (m_previous != kTowardZero)
            writeFpcr((fpcr & ~kRMode) | kTowardZero);
    }
    ~ScopedRoundTowardZero()
    {
        if (!m_enabled)
            return;
        const uint64_t fpcr = readFpcr();
        if ((fpcr & kRMode) != m_previous)
            writeFpcr((fpcr & ~kRMode) | m_previous);
    }
private:
    bool m_enabled;
    uint64_t m_previous;
#elif defined(RRV_FP_ROUNDING_X86)
    static constexpr unsigned kRC = 0x6000u;         // MXCSR.RC, bits 14:13
    static constexpr unsigned kTowardZero = 0x6000u; // RC = 0b11
    explicit ScopedRoundTowardZero(bool enabled = true) : m_restore(false), m_saved(0)
    {
        if (!enabled)
            return;
        const unsigned csr = _mm_getcsr();
        if ((csr & kRC) != kTowardZero)
        {
            m_saved = csr;
            m_restore = true;
            _mm_setcsr((csr & ~kRC) | kTowardZero);
        }
        __asm__ volatile("" ::: "memory");
    }
    ~ScopedRoundTowardZero()
    {
        __asm__ volatile("" ::: "memory");
        if (m_restore)
            _mm_setcsr(m_saved); // the entry value: no read, so no stall behind the ldmxcsr above
    }
private:
    bool m_restore; // the scope changed the mode and must put it back
    unsigned m_saved;
#else
    explicit ScopedRoundTowardZero(bool enabled = true) : m_enabled(enabled), m_previous(std::fegetround())
    {
        if (m_enabled)
            std::fesetround(FE_TOWARDZERO);
    }
    ~ScopedRoundTowardZero()
    {
        if (m_enabled)
            std::fesetround(m_previous);
    }
private:
    bool m_enabled;
    int m_previous;
#endif
    ScopedRoundTowardZero(const ScopedRoundTowardZero &) = delete;
    ScopedRoundTowardZero &operator=(const ScopedRoundTowardZero &) = delete;
};

} // namespace rrv::fp

#endif // RRV_FP_ROUNDING_H
