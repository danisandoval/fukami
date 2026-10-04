// Synthetic, data-free driver for executing CodeGenerator branch output.
// Build it against a PS2Recomp checkout's ps2_recomp_lib, then pass an output
// path.  The output is a self-checking C++ program containing the actual
// branch strings emitted by CodeGenerator::handleBranchDelaySlots.

#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"

#include <array>
#include <cstdint>
#include <fstream>

using namespace ps2recomp;

enum class Relation : uint8_t { Less, GreaterEqual, LessEqual, Greater };

struct Form {
    const char *name;
    uint32_t opcode;
    uint32_t rt;
    Relation relation;
    bool likely;
    bool link;
};

static constexpr std::array<Form, 12> kForms = {{
    {"blez", OPCODE_BLEZ, 0u, Relation::LessEqual, false, false},
    {"bgtz", OPCODE_BGTZ, 0u, Relation::Greater, false, false},
    {"blezl", OPCODE_BLEZL, 0u, Relation::LessEqual, true, false},
    {"bgtzl", OPCODE_BGTZL, 0u, Relation::Greater, true, false},
    {"bltz", OPCODE_REGIMM, REGIMM_BLTZ, Relation::Less, false, false},
    {"bgez", OPCODE_REGIMM, REGIMM_BGEZ, Relation::GreaterEqual, false, false},
    {"bltzl", OPCODE_REGIMM, REGIMM_BLTZL, Relation::Less, true, false},
    {"bgezl", OPCODE_REGIMM, REGIMM_BGEZL, Relation::GreaterEqual, true, false},
    {"bltzal", OPCODE_REGIMM, REGIMM_BLTZAL, Relation::Less, false, true},
    {"bgezal", OPCODE_REGIMM, REGIMM_BGEZAL, Relation::GreaterEqual, false, true},
    {"bltzall", OPCODE_REGIMM, REGIMM_BLTZALL, Relation::Less, true, true},
    {"bgezall", OPCODE_REGIMM, REGIMM_BGEZALL, Relation::GreaterEqual, true, true},
}};

static Instruction makeBranch(const Form &form)
{
    Instruction inst{};
    inst.address = 0x1000u;
    inst.opcode = form.opcode;
    inst.rs = 1u;
    inst.rt = form.rt;
    inst.simmediate = 4u; // target = 0x1014, external to the test function.
    inst.isBranch = true;
    inst.hasDelaySlot = true;
    return inst;
}

static Instruction makeDelaySlot()
{
    Instruction inst{};
    inst.address = 0x1004u;
    inst.opcode = OPCODE_ADDIU;
    inst.rs = 0u;
    inst.rt = 2u;
    inst.simmediate = 1u;
    return inst;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;

    Function function{};
    function.start = 0x1000u;
    function.end = 0x1008u;
    CodeGenerator generator({}, {});
    CodeGenerator::AnalysisResult analysis{};
    std::ofstream output(argv[1]);
    if (!output)
        return 3;

    output << R"CPP(#include <array>
#include <cstdint>
#include <cstdio>

struct Ctx {
    std::uint64_t r[32]{};
    std::uint32_t pc{};
    bool in_delay_slot{};
    std::uint32_t branch_pc{};
};

#define GPR_U32(ctx, reg) (static_cast<std::uint32_t>((ctx)->r[(reg)]))
#define GPR_S32(ctx, reg) (static_cast<std::int32_t>(static_cast<std::uint32_t>((ctx)->r[(reg)])))
#define GPR_S64(ctx, reg) (static_cast<std::int64_t>((ctx)->r[(reg)]))
#define ADD32(a, b) (static_cast<std::uint32_t>((a) + (b)))
#define SET_GPR_U32(ctx, reg, value) do { if ((reg) != 0) (ctx)->r[(reg)] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)); } while (0)
#define SET_GPR_S32(ctx, reg, value) do { if ((reg) != 0) (ctx)->r[(reg)] = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value))); } while (0)

)CPP";

    for (const Form &form : kForms)
    {
        output << "extern \"C\" void generated_" << form.name << "(Ctx *ctx) {\n";
        output << generator.handleBranchDelaySlots(makeBranch(form), makeDelaySlot(), function, analysis);
        output << "}\n\n";
    }

    output << R"CPP(using Fn = void (*)(Ctx *);
enum class Relation : int { Less, GreaterEqual, LessEqual, Greater };
struct Form { const char *name; Fn fn; Relation relation; bool likely; bool link; };

static bool taken(std::int64_t value, Relation relation)
{
    switch (relation) {
    case Relation::Less: return value < 0;
    case Relation::GreaterEqual: return value >= 0;
    case Relation::LessEqual: return value <= 0;
    case Relation::Greater: return value > 0;
    }
    return false;
}

int main()
{
    constexpr std::uint64_t kLinkSentinel = 0xfeedfacefeedfaceULL;
    constexpr std::array<std::uint64_t, 8> kValues = {{
        0xfff0000000000000ULL, 0x0000000080000000ULL,
        0xffffffff00000000ULL, 0x0000000000000000ULL,
        0x0000000000000001ULL, 0xffffffffffffffffULL,
        0x8000000000000000ULL, 0x7fffffffffffffffULL,
    }};
    const std::array<Form, 12> forms = {{
)CPP";
    for (const Form &form : kForms)
    {
        output << "        {\"" << form.name << "\", generated_" << form.name << ", Relation::";
        switch (form.relation)
        {
        case Relation::Less: output << "Less"; break;
        case Relation::GreaterEqual: output << "GreaterEqual"; break;
        case Relation::LessEqual: output << "LessEqual"; break;
        case Relation::Greater: output << "Greater"; break;
        }
        output << ", " << (form.likely ? "true" : "false") << ", "
               << (form.link ? "true" : "false") << "},\n";
    }
    output << R"CPP(    }};
    int failures = 0;
    for (const Form &form : forms) {
        for (std::uint64_t raw : kValues) {
            Ctx ctx{};
            ctx.r[1] = raw;
            ctx.r[31] = kLinkSentinel;
            form.fn(&ctx);
            const bool branch_taken = taken(static_cast<std::int64_t>(raw), form.relation);
            const bool expected_delay = form.likely ? branch_taken : true;
            const bool expected_link = form.link && (form.likely ? branch_taken : true);
            const bool actual_delay = ctx.r[2] == 1u;
            const std::uint64_t expected_ra = expected_link ? 0x1008u : kLinkSentinel;
            const std::uint32_t expected_pc = branch_taken ? 0x1014u : 0x1008u;
            const std::uint32_t expected_branch_pc = expected_delay ? 0x1000u : 0u;
            if (actual_delay != expected_delay || ctx.r[31] != expected_ra || ctx.pc != expected_pc ||
                ctx.in_delay_slot || ctx.branch_pc != expected_branch_pc) {
                std::fprintf(stderr, "%s raw=%016llx taken=%d delay=%d/%d ra=%016llx/%016llx pc=%08x/%08x\n",
                    form.name, static_cast<unsigned long long>(raw), branch_taken,
                    actual_delay, expected_delay, static_cast<unsigned long long>(ctx.r[31]),
                    static_cast<unsigned long long>(expected_ra), ctx.pc, expected_pc);
                ++failures;
            }
        }
    }
    return failures == 0 ? 0 : 1;
}
)CPP";
    return output ? 0 : 4;
}
