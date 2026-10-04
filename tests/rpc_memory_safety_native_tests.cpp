// SPDX-License-Identifier: GPL-3.0+
// Asset-free real RPC dispatch and guest-stack lifetime regression test.
#include "Common.h"
#include "RPC.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace
{
constexpr uint32_t kClient = 0x30000u;
constexpr uint32_t kServer = 0x30100u;
constexpr uint32_t kSend = 0x30200u;
constexpr uint32_t kRecv = 0x30300u;
constexpr uint32_t kOuter = 0x200000u;
constexpr uint32_t kInner = 0x200100u;
constexpr uint32_t kThrow = 0x200200u;
constexpr uint32_t kStackReply = 0x200300u;
constexpr uint32_t kHeapBase = 0x01000000u;
constexpr uint32_t kHeapLimit = kHeapBase + 0x8000u;
uint32_t outerSp = 0u, innerSp = 0u;
bool enteredInner = false;
uint32_t callbackCount = 0u;

void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void put32(uint8_t *ram, uint32_t address, uint32_t value)
{
    std::memcpy(ram + address, &value, sizeof(value));
}

uint32_t get32(const uint8_t *ram, uint32_t address)
{
    uint32_t value = 0u;
    std::memcpy(&value, ram + address, sizeof(value));
    return value;
}

void callRpc(uint8_t *ram, PS2Runtime *runtime, uint32_t rpc,
             uint32_t receiveSize, uint32_t endFunc = 0u,
             uint32_t expectedStatus = 0u)
{
    R5900Context call{};
    setRegU32(&call, 4, kClient);
    setRegU32(&call, 5, rpc);
    setRegU32(&call, 6, 0u);
    setRegU32(&call, 7, kSend);
    setRegU32(&call, 8, 12u);
    setRegU32(&call, 9, kRecv);
    setRegU32(&call, 10, receiveSize);
    setRegU32(&call, 11, endFunc);
    setRegU32(&call, 29, 0x31000u);
    ps2_syscalls::SifCallRpc(ram, &call, runtime);
    check(getRegU32(&call, 2) == expectedStatus, "real SifCallRpc status mismatch");
}

void inner(uint8_t *ram, R5900Context *ctx, PS2Runtime *)
{
    ++callbackCount;
    enteredInner = true;
    innerSp = getRegU32(ctx, 29);
    check(innerSp != outerSp &&
              (innerSp >= outerSp + 0x4000u || outerSp >= innerSp + 0x4000u),
          "nested RPC dispatch reused the live outer stack");
    put32(ram, innerSp - 0x40u, 0xA1B2C3D4u);
    setRegU32(ctx, 2, kSend);
    ctx->pc = getRegU32(ctx, 31);
}

void outer(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    ++callbackCount;
    outerSp = getRegU32(ctx, 29);
    const uint32_t ra = getRegU32(ctx, 31);
    put32(ram, outerSp - 0x40u, 0x12345678u);
    t_SifRpcServerData *server = reinterpret_cast<t_SifRpcServerData *>(ram + kServer);
    server->func = kInner;
    R5900Context nested = *ctx;
    setRegU32(&nested, 4, kClient);
    setRegU32(&nested, 5, 7u);
    setRegU32(&nested, 6, 0u);
    setRegU32(&nested, 7, kSend);
    setRegU32(&nested, 8, 4u);
    setRegU32(&nested, 9, kRecv);
    setRegU32(&nested, 10, 4u);
    setRegU32(&nested, 11, 0u);
    ps2_syscalls::SifCallRpc(ram, &nested, runtime);
    check(getRegU32(&nested, 2) == 0u && enteredInner, "nested SifCallRpc did not dispatch");
    check(get32(ram, outerSp - 0x40u) == 0x12345678u,
          "nested RPC overwrote the outer guest frame");
    check(getRegU32(ctx, 29) == outerSp && getRegU32(ctx, 31) == ra,
          "nested RPC changed the outer guest context");
    setRegU32(ctx, 2, 0xBEEFu);
    ctx->pc = ra;
}

void throws(uint8_t *, R5900Context *, PS2Runtime *)
{
    ++callbackCount;
    throw std::runtime_error("synthetic guest callback exception");
}

void stackReply(uint8_t *ram, R5900Context *ctx, PS2Runtime *)
{
    ++callbackCount;
    const uint32_t reply = getRegU32(ctx, 29) - 0x40u;
    put32(ram, reply, 0xD00DFEEDu);
    setRegU32(ctx, 2, reply);
    ctx->pc = getRegU32(ctx, 31);
}
}

int main()
{
    PS2Runtime runtime;
    check(runtime.memory().initialize(), "guest memory initialization failed");
    uint8_t *ram = runtime.memory().getRDRAM();
    check(ram != nullptr, "guest RAM unavailable");
    runtime.configureGuestHeap(kHeapBase, kHeapLimit);
    auto *client = reinterpret_cast<t_SifRpcClientData *>(ram + kClient);
    auto *server = reinterpret_cast<t_SifRpcServerData *>(ram + kServer);
    std::memset(client, 0, sizeof(*client));
    std::memset(server, 0, sizeof(*server));
    client->server = kServer;
    g_rpc_clients[kClient].sid = 0x7654u;
    g_rpc_servers[0x7654u] = {0x7654u, kServer};

    PS2DtxCompatLayout layout{};
    layout.rpcSid = 0x7654u;
    ps2_syscalls::setDtxCompatLayout(layout);
    constexpr uint32_t handle = 0x00900000u;
    g_dtx_sjrmt_by_handle[handle].roomBytes = 0x1234u;
    put32(ram, kSend, handle);
    put32(ram, kSend + 4u, 0u);
    put32(ram, kSend + 8u, 1u);
    for (uint32_t size : {0u, 1u, 2u, 3u, 4u, 8u})
    {
        std::memset(ram + kRecv - 4u, 0xA5, 20u);
        callRpc(ram, &runtime, 0x42Au, size);
        for (uint32_t i = 0; i < 4u; ++i)
            check(ram[kRecv - 4u + i] == 0xA5u, "receive prefix canary changed");
        for (uint32_t i = size; i < 16u; ++i)
            check(ram[kRecv + i] == 0xA5u, "receive suffix canary changed");
        if (size >= 4u) check(get32(ram, kRecv) == 1u, "first reply word missing");
        if (size >= 8u) check(get32(ram, kRecv + 4u) == 0x1234u,
                               "eight-byte reply lost second word");
    }
    ps2_syscalls::clearDtxCompatLayout();
    server->func = 0u;

    runtime.registerFunction(kOuter, outer);
    runtime.registerFunction(kInner, inner);
    runtime.registerFunction(kThrow, throws);
    runtime.registerFunction(kStackReply, stackReply);
    server->func = kStackReply;
    std::memset(ram + kRecv, 0, 4u);
    callRpc(ram, &runtime, 7u, 4u);
    check(get32(ram, kRecv) == 0xD00DFEEDu,
          "real SifCallRpc failed to copy stack-backed reply");
    uint32_t whole = runtime.guestMalloc(0x8000u, 16u);
    check(whole == kHeapBase, "SifCallRpc retained stack after receive copy");
    runtime.guestFree(whole);

    R5900Context ctx{};
    setRegU32(&ctx, 29, 0x60000u);
    setRegU32(&ctx, 31, 0x70000u);
    uint32_t returnValue = 0u;
    callbackCount = 0u;
    check(rpcInvokeFunction(ram, &ctx, &runtime, kOuter, 0u, 0u, 0u, 0u, &returnValue),
          "outer real guest dispatch failed");
    check(returnValue == 0xBEEFu && callbackCount == 2u,
          "outer return value or nested callback count wrong");
    check(getRegU32(&ctx, 29) == 0x60000u && getRegU32(&ctx, 31) == 0x70000u,
          "rpcInvokeFunction changed caller SP/RA");
    whole = runtime.guestMalloc(0x8000u, 16u);
    check(whole == kHeapBase, "nested stacks were not freed and coalesced");
    runtime.guestFree(whole);

    RpcInvokeStackLease retainedReply;
    uint32_t replyPtr = 0u;
    check(rpcInvokeFunction(ram, &ctx, &runtime, kStackReply, 0u, 0u, 0u, 0u,
                            &replyPtr, &retainedReply), "stack-backed callback failed");
    check(retainedReply.base == kHeapBase && replyPtr == kHeapBase + 0x3FC0u,
          "stack-backed reply lease was not retained");
    const uint32_t competing = runtime.guestMalloc(0x4000u, 16u);
    check(competing == kHeapBase + 0x4000u,
          "live stack-backed reply was reused by another allocation");
    check(get32(ram, replyPtr) == 0xD00DFEEDu, "live stack-backed reply changed");
    runtime.guestFree(competing);
    retainedReply.reset();
    whole = runtime.guestMalloc(0x8000u, 16u);
    check(whole == kHeapBase, "released reply stack did not coalesce");
    runtime.guestFree(whole);

    whole = runtime.guestMalloc(0x8000u, 16u);
    const uint32_t beforeFailure = callbackCount;
    server->func = kInner;
    put32(ram, kRecv, 0xABCDEF01u);
    callRpc(ram, &runtime, 7u, 4u, 0u, 0xFFFFFFFFu);
    check(get32(ram, kRecv) == 0xABCDEF01u,
          "allocation failure fabricated a receive reply");
    check(!g_rpc_clients[kClient].busy, "allocation failure left RPC client busy");
    check(callbackCount == beforeFailure, "SifCallRpc ran callback after allocation failed");
    server->func = 0u;
    callRpc(ram, &runtime, 7u, 4u, kInner, 0xFFFFFFFFu);
    check(!g_rpc_clients[kClient].busy, "end-callback allocation failure left RPC client busy");
    check(callbackCount == beforeFailure, "end callback ran after allocation failed");
    check(!rpcInvokeFunction(ram, &ctx, &runtime, kInner, 0u, 0u, 0u, 0u, nullptr),
          "allocation failure ran guest code");
    check(callbackCount == beforeFailure, "guest callback ran after stack allocation failed");
    runtime.guestFree(whole);
    bool caught = false;
    try { (void)rpcInvokeFunction(ram, &ctx, &runtime, kThrow, 0u, 0u, 0u, 0u, nullptr); }
    catch (const std::runtime_error &) { caught = true; }
    check(caught, "guest exception did not propagate");
    whole = runtime.guestMalloc(0x8000u, 16u);
    check(whole == kHeapBase, "exception path leaked the guest stack");
    runtime.guestFree(whole);
    return 0;
}
