// Checks the registered acphal.dll shim against the measured call shapes
// (docs/retail_boot_chain.md, docs/retail_boot_frame_loop.md Stop 28, PR #6885, PR #7009).
//
// All 9 exports are resolved through rt::shims::find so the test exercises
// the exact shim table and calling convention the runtime installs.

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "shims.h"

namespace {

using AcpHalCreateFn = HRESULT(__stdcall*)(void**);
using AcpHalAllocShapeCtxFn = HRESULT(__stdcall*)(void*);
using AcpHalReleaseShapeCtxFn = HRESULT(__stdcall*)(void*);
using ApuCreateHeapFn = HRESULT(__stdcall*)(uint32_t, uint32_t);
// Measured: ApuAlloc(void** va, uint32_t* apu, uint32_t size, uint32_t align, uint32_t flags)
using ApuAllocFn = HRESULT(__stdcall*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t);
using ApuFreeFn = void(__stdcall*)(void*);
// Measured: ApuIsVirtualAddressValid(void* va, uint32_t align)
using ApuIsValidFn = BOOL(__stdcall*)(void*, uint32_t);
using ApuMapApuAddrFn = void*(__stdcall*)(uint32_t);
using ApuMapVirtualAddrFn = uint32_t(__stdcall*)(void*);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

// Test mirror of SHAPE_CONTEXTS matching devdocs.xbox.com GDK spec
struct APU_ADDRESS {
    uint64_t value;
};

struct TestShapeContexts {
    uint32_t numSrcContexts;
    uint32_t numEqCompContexts;
    uint32_t numFiltVolContexts;
    uint32_t numDmaContexts;
    uint32_t numXmaContexts;
    uint32_t numPcmContexts;
    void* srcContextArray;
    void* eqCompContextArray;
    void* filtVolContextArray;
    void* dmaContextArray;
    void* xmaContextArray;
    void* pcmContextArray;
    APU_ADDRESS apuSrcContextArray;
    APU_ADDRESS apuEqCompContextArray;
    APU_ADDRESS apuFiltVolContextArray;
    APU_ADDRESS apuDmaContextArray;
    APU_ADDRESS apuXmaContextArray;
    APU_ADDRESS apuPcmContextArray;
};

static_assert(sizeof(TestShapeContexts) == 120, "TestShapeContexts is 120 bytes");
static_assert(offsetof(TestShapeContexts, xmaContextArray) == 0x38,
              "xmaContextArray is at offset +0x38");

}  // namespace

int main() {
    std::printf("acphal_test: registered acphal.dll shim verification\n");

    const auto create = reinterpret_cast<AcpHalCreateFn>(
        rt::shims::find("acphal.dll", "AcpHalCreate"));
    const auto alloc_ctx = reinterpret_cast<AcpHalAllocShapeCtxFn>(
        rt::shims::find("acphal.dll", "AcpHalAllocateShapeContexts"));
    const auto release_ctx = reinterpret_cast<AcpHalReleaseShapeCtxFn>(
        rt::shims::find("acphal.dll", "AcpHalReleaseShapeContexts"));
    const auto create_heap = reinterpret_cast<ApuCreateHeapFn>(
        rt::shims::find("acphal.dll", "ApuCreateHeap"));
    const auto alloc = reinterpret_cast<ApuAllocFn>(
        rt::shims::find("acphal.dll", "ApuAlloc"));
    const auto free_mem = reinterpret_cast<ApuFreeFn>(
        rt::shims::find("acphal.dll", "ApuFree"));
    const auto is_valid = reinterpret_cast<ApuIsValidFn>(
        rt::shims::find("acphal.dll", "ApuIsVirtualAddressValid"));
    const auto map_apu = reinterpret_cast<ApuMapApuAddrFn>(
        rt::shims::find("acphal.dll", "ApuMapApuAddress"));
    const auto map_virtual = reinterpret_cast<ApuMapVirtualAddrFn>(
        rt::shims::find("acphal.dll", "ApuMapVirtualAddress"));

    check(create != nullptr, "AcpHalCreate is registered in registry");
    check(alloc_ctx != nullptr, "AcpHalAllocateShapeContexts is registered in registry");
    check(release_ctx != nullptr, "AcpHalReleaseShapeContexts is registered in registry");
    check(create_heap != nullptr, "ApuCreateHeap is registered in registry");
    check(alloc != nullptr, "ApuAlloc is registered in registry");
    check(free_mem != nullptr, "ApuFree is registered in registry");
    check(is_valid != nullptr, "ApuIsVirtualAddressValid is registered in registry");
    check(map_apu != nullptr, "ApuMapApuAddress is registered in registry");
    check(map_virtual != nullptr, "ApuMapVirtualAddress is registered in registry");
    check(rt::shims::owns_dll("acphal.dll"), "the registry owns acphal.dll");

    if (!create || !alloc_ctx || !release_ctx || !create_heap || !alloc ||
        !free_mem || !is_valid || !map_apu || !map_virtual) {
        std::printf("Fatal: Missing export bindings for acphal.dll\n");
        return 1;
    }

    // 1. AcpHalCreate
    void* acp = nullptr;
    check(create(&acp) == S_OK && acp != nullptr,
          "AcpHalCreate returns S_OK with a non-null object");
    check(create(nullptr) == E_POINTER,
          "AcpHalCreate(nullptr) returns E_POINTER");

    if (acp) {
        using Slot0Fn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
        using Slot1Fn = HRESULT(__stdcall*)(void*);
        using Slot2Fn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t);
        using Slot3Fn = uint32_t(__stdcall*)(void*);
        using Slot5Fn = HRESULT(__stdcall*)(void*);

        auto vtable = *static_cast<void* const* const*>(acp);
        check(vtable[0] != nullptr, "IAcpHal vtable slot 0 (connect) is present");
        check(vtable[1] != nullptr, "IAcpHal vtable slot 1 (disconnect) is present");
        check(vtable[2] != nullptr, "IAcpHal vtable slot 2 (submit_command) is present");
        check(vtable[3] != nullptr, "IAcpHal vtable slot 3 (pop_message) is present");
        check(vtable[4] != nullptr, "IAcpHal vtable slot 4 (get_num_messages) is present");
        check(vtable[5] != nullptr, "IAcpHal vtable slot 5 (release) is present");

        // Slot 0: Connect (called at boot with (0x1000, 0x1000))
        check(reinterpret_cast<Slot0Fn>(vtable[0])(acp, 0x1000, 0x1000) == S_OK,
              "IAcpHal slot 0 (connect) returns S_OK");

        // Slot 2: SubmitCommand. Critical boot requirement: The title calls
        // slot 2 as (2, 0xf0f0f0f0, -1, config, 0) and treats failure as fatal int3.
        check(reinterpret_cast<Slot2Fn>(vtable[2])(acp, 2, 0xf0f0f0f0, 0xffffffffu, nullptr, 0) == S_OK,
              "IAcpHal slot 2 (submit_command) returns S_OK (vital boot unblocker)");

        // Slots 3 and 4: Message polling returns 0
        check(reinterpret_cast<Slot3Fn>(vtable[3])(acp) == 0, "IAcpHal slot 3 returns 0");
        check(reinterpret_cast<Slot3Fn>(vtable[4])(acp) == 0, "IAcpHal slot 4 returns 0");

        // Slot 1: Disconnect returns S_OK
        check(reinterpret_cast<Slot1Fn>(vtable[1])(acp) == S_OK, "IAcpHal slot 1 returns S_OK");

        // Slot 5: Release returns S_OK
        check(reinterpret_cast<Slot5Fn>(vtable[5])(acp) == S_OK, "IAcpHal slot 5 returns S_OK");
    }

    // 2. ApuCreateHeap: title passes (cachedSize, nonCachedSize=0x226e00)
    check(create_heap(4 * 1024 * 1024, 0x226e00) == S_OK,
          "ApuCreateHeap(4 MiB, 0x226e00) succeeds");
    check(create_heap(4 * 1024 * 1024, 0x226e00) != S_OK,
          "ApuCreateHeap second call fails (heap already created)");

    // 3. ApuAlloc argument validation
    void* dummy = nullptr;
    check(alloc(nullptr, nullptr, 1024, 0x800, 0) == E_POINTER,
          "ApuAlloc(nullptr, ...) returns E_POINTER");
    check(alloc(&dummy, nullptr, 0, 0x800, 0) == E_INVALIDARG,
          "ApuAlloc with size=0 returns E_INVALIDARG");
    check(alloc(&dummy, nullptr, 1024, 3, 0) == E_INVALIDARG,
          "ApuAlloc with unaligned alignment (3) returns E_INVALIDARG");

    // 4. ApuAlloc real boot sequence:
    // First call: flags=2, size=0x222e00, align=0x800 (noncached heap)
    void* va_nc = nullptr;
    uint32_t apu_nc = 0;
    check(alloc(&va_nc, &apu_nc, 0x222e00, 0x800, 2) == S_OK && va_nc != nullptr,
          "ApuAlloc flags=2 at title's non-cached size 0x222e00 succeeds");
    check(apu_nc != 0, "ApuAlloc provides non-zero physical APU address");

    // Second call: flags=1, size=4096, align=0x800 (cached heap)
    void* va_c = nullptr;
    uint32_t apu_c = 0;
    check(alloc(&va_c, &apu_c, 4096, 0x800, 1) == S_OK && va_c != nullptr,
          "ApuAlloc flags=1 succeeds on cached heap");
    check(apu_c != 0, "ApuAlloc flags=1 provides non-zero APU address");

    // Address verification & disjoint ranges
    check(va_nc != va_c, "Cached and non-cached allocations return different virtual pointers");
    check(apu_nc != apu_c, "Cached and non-cached APU addresses are distinct");
    check(is_valid(va_nc, 0x800) == TRUE, "Non-cached allocation is in a valid heap");
    check(is_valid(va_c, 0x800) == TRUE, "Cached allocation is in a valid heap");
    check(is_valid(nullptr, 0x800) == FALSE, "ApuIsVirtualAddressValid(nullptr) returns FALSE");
    check(is_valid(reinterpret_cast<void*>(0x1234), 0x800) == FALSE,
          "ApuIsVirtualAddressValid(bogus) returns FALSE");

    // Round-trip mapping
    check(map_virtual(va_nc) == apu_nc, "ApuMapVirtualAddress matches apu_nc");
    check(map_virtual(va_c) == apu_c, "ApuMapVirtualAddress matches apu_c");
    check(map_apu(apu_nc) == va_nc, "ApuMapApuAddress round-trips non-cached address");
    check(map_apu(apu_c) == va_c, "ApuMapApuAddress round-trips cached address");
    check(map_apu(0) == nullptr, "ApuMapApuAddress(0) returns nullptr");
    check(map_virtual(nullptr) == 0, "ApuMapVirtualAddress(nullptr) returns 0");

    // 5. Genuine State Tracking: Free & Memory Reuse
    void* reusable_block1 = nullptr;
    check(alloc(&reusable_block1, nullptr, 0x1000, 0x800, 1) == S_OK && reusable_block1 != nullptr,
          "Allocate 4 KiB reusable block");
    free_mem(reusable_block1);
    check(true, "ApuFree on valid address succeeds without error");
    free_mem(nullptr);
    check(true, "ApuFree(nullptr) is a safe no-op");

    // Reallocate in the freed slot:
    void* reusable_block2 = nullptr;
    check(alloc(&reusable_block2, nullptr, 0x1000, 0x800, 1) == S_OK && reusable_block2 != nullptr,
          "Re-allocate in cached heap succeeds (memory successfully reused)");
    check(reusable_block2 == reusable_block1,
          "Free block was genuinely reused at the same offset by the allocator");
    free_mem(reusable_block2);

    // 6. AcpHalAllocateShapeContexts & AcpHalReleaseShapeContexts
    check(alloc_ctx(nullptr) == E_POINTER,
          "AcpHalAllocateShapeContexts(nullptr) returns E_POINTER");

    // Real executable call: 0 SRC, 350 XMA (+0x38), 0 PCM
    TestShapeContexts ctx{};
    ctx.numXmaContexts = 350;
    check(alloc_ctx(&ctx) == S_OK, "AcpHalAllocateShapeContexts succeeds");
    check(ctx.xmaContextArray != nullptr,
          "xmaContextArray (+0x38) is non-null after allocation");
    check(ctx.srcContextArray == nullptr, "srcContextArray is null when count is 0");

    // Verify zeroed memory across all 350 * 64 bytes
    const uint8_t* xma_bytes = static_cast<const uint8_t*>(ctx.xmaContextArray);
    bool all_zero = true;
    for (size_t i = 0; i < 350 * 64; ++i) {
        if (xma_bytes[i] != 0) {
            all_zero = false;
            break;
        }
    }
    check(all_zero, "SHAPE context buffer is zero-initialized for title's stride");

    // Test multi-context allocation
    TestShapeContexts ctx_all{};
    ctx_all.numSrcContexts = 10;
    ctx_all.numEqCompContexts = 5;
    ctx_all.numFiltVolContexts = 8;
    ctx_all.numDmaContexts = 4;
    ctx_all.numXmaContexts = 12;
    ctx_all.numPcmContexts = 6;
    check(alloc_ctx(&ctx_all) == S_OK, "AcpHalAllocateShapeContexts with all 6 types succeeds");
    check(ctx_all.srcContextArray != nullptr, "srcContextArray is allocated");
    check(ctx_all.eqCompContextArray != nullptr, "eqCompContextArray is allocated");
    check(ctx_all.filtVolContextArray != nullptr, "filtVolContextArray is allocated");
    check(ctx_all.dmaContextArray != nullptr, "dmaContextArray is allocated");
    check(ctx_all.xmaContextArray != nullptr, "xmaContextArray is allocated");
    check(ctx_all.pcmContextArray != nullptr, "pcmContextArray is allocated");

    // Release shape contexts. The GDK call takes no argument, so whatever the first argument register holds is not a
    // structure to write: a stand-in for a stale RCX (a block of 0xAA) must come back untouched.
    unsigned char stale_rcx[sizeof(ctx_all)];
    std::memset(stale_rcx, 0xAA, sizeof(stale_rcx));
    check(release_ctx(stale_rcx) == S_OK, "AcpHalReleaseShapeContexts succeeds");
    bool untouched = true;
    for (unsigned char b : stale_rcx) untouched = untouched && b == 0xAA;
    check(untouched, "AcpHalReleaseShapeContexts writes nothing through its (absent) argument");
    check(release_ctx(nullptr) == S_OK, "AcpHalReleaseShapeContexts again succeeds");

    // 7. ACP command/message queue protocol (register, complete, overflow drops, disconnect, notify word)
    {
        #pragma pack(push, 1)
        struct TestAcpMessage {
            uint32_t kind;
            uint32_t dropped;
            uint32_t usec;
            uint8_t data[16];
        };
        #pragma pack(pop)
        static_assert(sizeof(TestAcpMessage) == 28, "ACP message is 28 bytes");

        using ConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
        using DisconnectFn = HRESULT(__stdcall*)(void*);
        using SubmitFn = HRESULT(__stdcall*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
        using PopFn = uint32_t(__stdcall*)(void*, TestAcpMessage*);
        using CountFn = uint32_t(__stdcall*)(void*);
        using ReleaseFn = HRESULT(__stdcall*)(void*);

        void* q_acp = nullptr;
        check(create(&q_acp) == S_OK && q_acp != nullptr, "AcpHalCreate for queue test");
        auto vt = *static_cast<void* const* const*>(q_acp);
        const auto fn_connect = reinterpret_cast<ConnectFn>(vt[0]);
        const auto fn_disconnect = reinterpret_cast<DisconnectFn>(vt[1]);
        const auto fn_submit = reinterpret_cast<SubmitFn>(vt[2]);
        const auto fn_pop = reinterpret_cast<PopFn>(vt[3]);
        const auto fn_count = reinterpret_cast<CountFn>(vt[4]);
        const auto fn_release = reinterpret_cast<ReleaseFn>(vt[5]);

        check(fn_connect(q_acp, 0, 16) == E_INVALIDARG, "Connect(commands=0) returns E_INVALIDARG");
        check(fn_disconnect(q_acp) == E_FAIL, "Disconnect before Connect returns E_FAIL");
        check(fn_connect(q_acp, 16, 2) == S_OK, "Connect(16, 2) succeeds");

        // Allocate a 4-byte notification word in APU memory.
        void* notify_va = nullptr;
        uint32_t notify_apu = 0;
        check(alloc(&notify_va, &notify_apu, 16, 16, 1) == S_OK && notify_va != nullptr,
              "ApuAlloc notification word succeeds");
        *static_cast<uint32_t*>(notify_va) = 0;

        const uint32_t reg_mask = 0x92u;  // FLOWGRAPH_COMPLETED(0x02) | COMMAND_COMPLETED(0x10) | DISCONNECTED(0x80)
        check(fn_submit(q_acp, 2, 0x1122334455667788ull, 0xffffffffu, &reg_mask, notify_apu) == S_OK,
              "REGISTER_MESSAGES with id and notify word succeeds");
        check(*static_cast<volatile uint32_t*>(notify_va) == 1u,
              "SubmitCommand writes 1 to APU notification word");
        check(fn_count(q_acp) == 1u, "COMMAND_COMPLETED queued after REGISTER_MESSAGES");

        TestAcpMessage msg{};
        check(fn_pop(q_acp, &msg) == 1u, "PopMessage retrieves COMMAND_COMPLETED");
        uint32_t ack_cmd = 0;
        uint64_t ack_id = 0;
        std::memcpy(&ack_cmd, msg.data, 4);
        std::memcpy(&ack_id, msg.data + 4, 8);
        check(msg.kind == 0x10u && msg.dropped == 0u && ack_cmd == 2u && ack_id == 0x1122334455667788ull,
              "COMMAND_COMPLETED payload carries command id and 64-bit token");

        // Overflow queue of capacity 2 with 4 commands -> 2 dropped, reported on the 5th.
        for (uint64_t tok = 1; tok <= 4; ++tok) {
            check(fn_submit(q_acp, 12, tok, 0xffffffffu, nullptr, 0) == S_OK,
                  "SubmitCommand during overflow succeeds");
        }
        check(fn_count(q_acp) == 2u, "Message queue caps at capacity 2");
        check(fn_pop(q_acp, &msg) == 1u && fn_pop(q_acp, &msg) == 1u,
              "Drained the 2 queued messages");
        check(fn_submit(q_acp, 12, 5, 0xffffffffu, nullptr, 0) == S_OK,
              "SubmitCommand 5 after drain succeeds");
        check(fn_pop(q_acp, &msg) == 1u && msg.dropped == 2u,
              "Next queued message reports dropped == 2");

        // LOAD_FLOWGRAPH posts FLOWGRAPH_COMPLETED carrying word(1).
        const uint32_t fg_cfg[2] = {0, 0x42u};
        check(fn_submit(q_acp, 1, 0, 0xffffffffu, fg_cfg, 0) == S_OK, "LOAD_FLOWGRAPH succeeds");
        check(fn_pop(q_acp, &msg) == 1u && msg.kind == 0x02u, "FLOWGRAPH_COMPLETED queued");
        uint32_t fg_id = 0;
        std::memcpy(&fg_id, msg.data, 4);
        check(fg_id == 0x42u, "FLOWGRAPH_COMPLETED carries flowgraph word");

        // Disconnect posts DISCONNECTED.
        check(fn_disconnect(q_acp) == S_OK, "Disconnect succeeds when connected");
        check(fn_count(q_acp) == 1u, "Disconnect queued DISCONNECTED message");
        check(fn_pop(q_acp, &msg) == 1u && msg.kind == 0x80u, "Popped DISCONNECTED message");

        free_mem(notify_va);
        check(fn_release(q_acp) == S_OK, "Release queue AcpHal succeeds");
    }

    // 8. End-to-end XMA2 context ring decoding via background decoder thread
    {
        // Build a 2-packet XMA2 buffer with 3 silent mono frames per packet (6 frames total).
        std::vector<uint8_t> stream(2 * 2048, 0xFFu);
        auto put_bits = [&](size_t at, uint32_t value, size_t width) {
            for (size_t i = 0; i < width; ++i) {
                const size_t p = at + i;
                const uint32_t bit = (value >> (width - 1 - i)) & 1u;
                stream[p / 8] = static_cast<uint8_t>(
                    (stream[p / 8] & ~(0x80u >> (p % 8))) |
                    (bit << (7 - (p % 8))));
            }
        };
        for (size_t packet = 0; packet < 2; ++packet) {
            const size_t base = packet * 2048 * 8;
            put_bits(base, 3u << 26, 32);  // 3 frames, first_frame_bit = 0, skip = 0
            for (size_t f = 0; f < 3; ++f) {
                const size_t at = base + 32 + f * 31;
                put_bits(at, 31, 15);        // frame length
                put_bits(at + 15, 0b10u, 2); // fixed layout, one 512-sample subframe
                put_bits(at + 17, 0, 14);    // gain(8) + trim(1) + subframe_skip/empty(5)
            }
        }

        TestShapeContexts xma_ctx{};
        xma_ctx.numXmaContexts = 4;
        check(alloc_ctx(&xma_ctx) == S_OK && xma_ctx.xmaContextArray != nullptr,
              "Allocate 4 XMA SHAPE contexts");

        void* in_va = nullptr;
        uint32_t in_apu = 0;
        check(alloc(&in_va, &in_apu, 0x1000, 0x800, 1) == S_OK && in_va != nullptr,
              "ApuAlloc 2-packet XMA2 input buffer");
        std::memcpy(in_va, stream.data(), stream.size());

        void* out_va = nullptr;
        uint32_t out_apu = 0;
        check(alloc(&out_va, &out_apu, 0x400, 0x100, 2) == S_OK && out_va != nullptr,
              "ApuAlloc 4-block XMA2 output ring");
        std::memset(out_va, 0x55, 0x400);

        // Configure voice index 1: 2 packets in input 0, input 0 valid, 4 output blocks,
        // writable = 1, 48 kHz mono, read_bit = 32.
        auto* words = static_cast<volatile uint32_t*>(xma_ctx.xmaContextArray) + 16;
        words[0] = 2u | (1u << 20) | (4u << 22);
        words[1] = (1u << 31) | (3u << 27);
        words[2] = 32u;
        words[5] = in_apu;
        words[7] = out_apu;

        using ConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
        using SubmitFn = HRESULT(__stdcall*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
        using ReleaseFn = HRESULT(__stdcall*)(void*);

        void* x_acp = nullptr;
        check(create(&x_acp) == S_OK && x_acp != nullptr, "AcpHalCreate for XMA decode test");
        auto vt = *static_cast<void* const* const*>(x_acp);
        const auto fn_connect = reinterpret_cast<ConnectFn>(vt[0]);
        const auto fn_submit = reinterpret_cast<SubmitFn>(vt[2]);
        const auto fn_release = reinterpret_cast<ReleaseFn>(vt[5]);

        check(fn_connect(x_acp, 0x1000, 0x1000) == S_OK, "Connect for XMA decode test");

        // ENABLE_XMA (cmd 5) on voice 1.
        const uint32_t enable_arg = 1u;
        check(fn_submit(x_acp, 5, 0xf1, 0xffffffffu, &enable_arg, 0) == S_OK,
              "ENABLE_XMA voice 1 succeeds");

        // Wait up to 500 ms for the background decoder thread to fill the 4-block ring.
        for (int wait = 0; wait < 500 && ((words[1] >> 31) & 1u) == 1u; ++wait) {
            Sleep(1);
        }
        check(((words[1] >> 31) & 1u) == 0u,
              "Background XMA2 decoder fills ring and clears writable flag");
        check(((words[0] >> 27) & 0x1Fu) == 0u,
              "Write block wrapped back to 0 after filling 4 blocks");

        bool ring_zeroed = true;
        const auto* ring_bytes = static_cast<const uint8_t*>(out_va);
        for (size_t i = 0; i < 0x400; ++i) {
            if (ring_bytes[i] != 0) {
                ring_zeroed = false;
                break;
            }
        }
        check(ring_zeroed, "Decoded silent PCM overwrote sentinel bytes across all 4 ring blocks");

        // Advance read pointer 4 blocks at a time until input exhausts and INPUT_INVALID fires.
        const uint32_t advance_args[2] = {1u, 4u};
        for (int step = 0; step < 20; ++step) {
            check(fn_submit(x_acp, 18, 0xf2 + static_cast<uint64_t>(step),
                            0xffffffffu, advance_args, 0) == S_OK,
                  "ADVANCE_XMA_READ succeeds");
            // Wait until either ring fills again (writable == 0) or stream hits error bit.
            for (int wait = 0; wait < 200; ++wait) {
                const uint32_t w1 = words[1];
                const uint32_t w2 = words[2];
                if (((w1 >> 31) & 1u) == 0u || ((w2 >> 31) & 1u) == 1u) break;
                Sleep(1);
            }
            if (((words[2] >> 31) & 1u) == 1u && ((words[1] >> 31) & 1u) == 1u) {
                break;
            }
        }
        check(((words[0] >> 20) & 3u) == 0u, "XMA input buffer valid bits cleared on exhaustion");
        check(((words[2] >> 31) & 1u) == 1u, "XMA error flag set on exhaustion");
        check(((words[2] >> 26) & 0x1Fu) == 4u, "XMA error status is INPUT_INVALID (4)");

        // DISABLE_XMA (cmd 7) on voice 1.
        const uint32_t disable_arg = 1u;
        check(fn_submit(x_acp, 7, 0xf9, 0xffffffffu, &disable_arg, 0) == S_OK,
              "DISABLE_XMA voice 1 succeeds");
        check(fn_release(x_acp) == S_OK, "Release XMA AcpHal succeeds");
        free_mem(in_va);
        free_mem(out_va);
        check(release_ctx(&xma_ctx) == S_OK, "Release XMA SHAPE contexts joins decoder cleanly");
    }

    // 9. ENABLE_XMA (starts/wakes the decoder worker) racing bind/release (stops it).
    //    Every release must return; a lost stop would hang this block.
    {
        using RaceConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
        using RaceSubmitFn = HRESULT(__stdcall*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
        using RaceReleaseFn = HRESULT(__stdcall*)(void*);

        void* race_acp = nullptr;
        check(create(&race_acp) == S_OK && race_acp != nullptr, "AcpHalCreate for XMA race test");
        auto race_vt = *static_cast<void* const* const*>(race_acp);
        const auto race_connect = reinterpret_cast<RaceConnectFn>(race_vt[0]);
        const auto race_submit = reinterpret_cast<RaceSubmitFn>(race_vt[2]);
        const auto race_release = reinterpret_cast<RaceReleaseFn>(race_vt[5]);
        check(race_connect(race_acp, 0x1000, 0x1000) == S_OK, "Connect for XMA race test");

        constexpr int kRaceIterations = 20000;
        std::atomic<int> bind_failures{0};
        std::thread enabler([&]() {
            const uint32_t enable_arg = 1u;
            for (int i = 0; i < kRaceIterations; ++i) {
                race_submit(race_acp, 5, 0xe000u + static_cast<uint64_t>(i), 0xffffffffu,
                            &enable_arg, 0);
            }
        });
        for (int i = 0; i < kRaceIterations; ++i) {
            TestShapeContexts race_ctx{};
            race_ctx.numXmaContexts = 4;
            if (alloc_ctx(&race_ctx) != S_OK) ++bind_failures;
            if (release_ctx(&race_ctx) != S_OK) ++bind_failures;
        }
        enabler.join();
        check(bind_failures == 0, "20000 bind/release cycles race ENABLE_XMA without hanging");
        check(race_release(race_acp) == S_OK, "Release race AcpHal succeeds");
    }

    std::printf("acphal_test complete: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
