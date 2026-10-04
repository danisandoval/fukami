#pragma once

// Linux (Gate 5, Steam Deck): per-thread CPU load in the session log, so one
// player run on real hardware shows which thread is the bottleneck. Every 2 s
// it reads /proc/self/task/*/stat and prints the threads above 5% of a core:
//   [cpu] t=12s GameThread=98% rrv-gs-owner=64% GS-SW... total=231%
// A thread near 100% is saturated. No-ops on other platforms.

namespace rrv::host
{
// Starts the sampler once (detached; it ends with the process).
void startThreadCpuLog();
// Names the calling thread for that log (15 characters at most on Linux).
void nameCurrentThread(const char *name);
} // namespace rrv::host
