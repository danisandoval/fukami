#pragma once
// Names the calling thread for the operating system's tools: the Linux [cpu] log and `top -H` (Gate 5), and on
// Windows the thread description that debuggers, Process Explorer and per-thread CPU samplers show. macOS threads keep
// their default names, as before. Host diagnostics only.
//
// Windows: the two kernel32 functions are declared here instead of including <windows.h>, whose macros clash with
// runtime headers that are included after it (EXCEPTION_BREAKPOINT in ps2_runtime.h). The declarations match the SDK's.

#if defined(__linux__)
#include <pthread.h>
#elif defined(_WIN32)
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread(void);
extern "C" __declspec(dllimport) long __stdcall SetThreadDescription(void* thread, const wchar_t* description);
#endif

namespace rrv
{

// `name` is ASCII and at most 15 characters (the Linux limit).
inline void setCurrentThreadName(const char* name)
{
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#elif defined(_WIN32)
    wchar_t wide[16] = {};
    for (int i = 0; i < 15 && name[i] != '\0'; ++i)
        wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(name[i]));
    SetThreadDescription(GetCurrentThread(), wide);
#else
    (void)name;
#endif
}

} // namespace rrv
