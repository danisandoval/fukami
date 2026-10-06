#pragma once

// Linux (Gate 5, Steam Deck): per-thread CPU load in the session log, so one
// player run on real hardware shows which thread is the bottleneck. Every 2 s
// it reads /proc/self/task/*/stat and prints the threads above 5% of a core:
//   [cpu] t=12s GameThread=98% rrv-gs-owner=64% GS-SW... total=231% cpu-mhz=2800 gpu-mhz=1600 gpu-busy=97%
// A thread near 100% is saturated. The clocks are read at the sample: on a
// Steam Deck the CPU and GPU share one power budget, so a busy GPU can lower
// the CPU clock. The PCSX2 bridge prints the matching [gs-wait] line (where the
// GS owner's time goes). No-ops on other platforms.

namespace rrv::host
{
// Starts the sampler once (detached; it ends with the process).
void startThreadCpuLog();
// Names the calling thread for that log (15 characters at most on Linux).
void nameCurrentThread(const char *name);
} // namespace rrv::host
