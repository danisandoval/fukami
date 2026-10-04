#include "../src/gs-control/rrv_gs_result_boundary.h"

#include <cstdlib>
#include <iostream>

namespace {
void check(bool value, const char *message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void rejectsEveryRetainedProducerClass() {
    using namespace rrv::gs;
    constexpr ProducerWork blocked[] = {
        ProducerWork::RendererVisibleGif, ProducerWork::DeferredVif1,
        ProducerWork::PartialDirect, ProducerWork::BlockedGifDma,
        ProducerWork::VifCompletion, ProducerWork::ReplayOnly,
        ProducerWork::HeldVif1, ProducerWork::PendingPath2Image,
        ProducerWork::MaskedPath3,
    };
    for (const auto work : blocked) {
        const ResultBoundary boundary{work};
        check(!boundary.admits(ResultOperation::ReadLocalMemory),
              "retained producer work admitted a local-memory read");
        check(!boundary.admits(ResultOperation::SnapshotLocalMemory),
              "retained producer work admitted a local-memory snapshot");
        check(!boundary.admits(ResultOperation::RestoreLocalMemory),
              "retained producer work admitted a local-memory restore");
    }
}

void acceptsOnlyQuiescentProducerState() {
    using namespace rrv::gs;
    const ResultBoundary boundary{};
    check(boundary.admits(ResultOperation::ReadLocalMemory),
          "quiescent producer rejected local-memory read");
    check(boundary.admits(ResultOperation::SnapshotLocalMemory),
          "quiescent producer rejected local-memory snapshot");
    check(boundary.admits(ResultOperation::RestoreLocalMemory),
          "quiescent producer rejected local-memory restore");
}
}

int main() {
    rejectsEveryRetainedProducerClass();
    acceptsOnlyQuiescentProducerState();
    std::cout << "GS result boundary classification passed\n";
}
