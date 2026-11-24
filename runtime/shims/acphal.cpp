// acphal.dll: Audio Control Processor HAL. Scalable Hardware Audio Processing Engine
// (SHAPE) and APU (Audio Processing Unit) physical memory management for Xbox Series.
//
// Provides:
//   - A VirtualAlloc-backed APU heap (cached + non-cached) with 32-bit device addresses;
//   - SHAPE context arrays sized to the documented GDK strides;
//   - The IAcpHal command and message queue (registration, completion, disconnect,
//     flowgraph acknowledgement, and APU notification word writes);
//   - A background XMA2 decoder pumping enabled SHAPE_XMA_CONTEXT output rings.
//
// Signatures from the public GDK reference (devdocs.xbox.com):
//   AcpHal, APU reference, SHAPE_CONTEXTS, APU_ADDRESS.
// Measured call-site shapes and findings from the retail executable (1.0.2.1):
//   - docs/retail_boot_chain.md, docs/retail_boot_frame_loop.md (Stop 28)
//   - PR #6885 (call sites and parameter analysis), PR #7009 (audio/HAL bring-up).

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "shims.h"
#include "startup_trace.h"
#include "../src/xma2_decoder.h"

namespace {

// ---- APU_ADDRESS (from the GDK) ----

// The console APU uses a 40-bit physical address space.
// On PC we translate to a 32-bit APU address offset.
struct APU_ADDRESS {
    uint64_t value;  // 40-bit physical address
};

// ---- SHAPE context structures (from the public GDK documentation at devdocs.xbox.com) ----
//
// These structures are defined here with their documented GDK field layouts so that
// sizeof() returns the correct per-context byte count. Without the real layouts the
// empty structs would be 1 byte each, and the title's writes (e.g. memset with the
// real stride) would overflow the allocation.

// UINT32 bitfields packed into 32-bit words
struct SHAPE_SRC_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t blocksToSkip : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t samplingIncrement : 21;
    uint32_t reserved0 : 1;
    uint32_t cmd : 2;
    uint32_t mixBufPeakMag2 : 4;
    uint32_t peakMag2 : 4;
    uint32_t sampleCount;
    uint32_t samplePointer : 25;
    uint32_t reserved1 : 7;
    uint32_t samplingIncrementTarget : 21;
    uint32_t reserved2 : 11;
    uint32_t reserved[3];
};
static_assert(sizeof(SHAPE_SRC_CONTEXT) == 32, "SHAPE_SRC_CONTEXT is 32 bytes per GDK");

// The EQ/Comp context: 88 UINT32s = 352 bytes per GDK definition.
#pragma pack(push, 1)
struct alignas(4) SHAPE_EQCOMP_CONTEXT {
    uint8_t data[352];
};
#pragma pack(pop)
static_assert(sizeof(SHAPE_EQCOMP_CONTEXT) == 352, "SHAPE_EQCOMP_CONTEXT is 352 bytes per GDK");

struct SHAPE_FILTVOL_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t delay1 : 30;
    uint32_t headroom : 2;
    uint32_t delay2 : 30;
    uint32_t svfMode : 2;
    uint32_t gainTarget : 16;
    uint32_t gain : 16;
    uint32_t qRecip : 24;
    uint32_t reserved1 : 8;
    uint32_t qRecipTarget : 24;
    uint32_t reserved2 : 8;
    uint32_t fc : 24;
    uint32_t reserved3 : 8;
    uint32_t fcTarget : 24;
    uint32_t reserved4 : 8;
};
static_assert(sizeof(SHAPE_FILTVOL_CONTEXT) == 32, "SHAPE_FILTVOL_CONTEXT is 32 bytes per GDK");

struct SHAPE_DMA_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 3;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t readPointer : 5;
    uint32_t reserved1 : 3;
    uint32_t writePointer : 5;
    uint32_t reserved2 : 3;
    uint32_t full : 1;
    uint32_t reserved3 : 15;
    uint32_t reserved4 : 9;
    uint32_t address : 23;
    uint32_t numFrames : 5;
    uint32_t reserved5 : 3;
    uint32_t floatConvert : 1;
    uint32_t reserved6 : 7;
    uint32_t numChannels : 3;
    uint32_t reserved7 : 5;
    uint32_t channel : 3;
    uint32_t reserved8 : 5;
};
static_assert(sizeof(SHAPE_DMA_CONTEXT) == 16, "SHAPE_DMA_CONTEXT is 16 bytes per GDK");

struct SHAPE_XMA_CONTEXT {
    uint32_t sizeRead0 : 12;
    uint32_t numLoops : 8;
    uint32_t validBuffer : 2;
    uint32_t sizeWrite : 5;
    uint32_t offsetWrite : 5;
    uint32_t sizeRead1 : 12;
    uint32_t sequenceNumber : 12;
    uint32_t stopSampleAddress : 8;
    uint32_t subframeOffset : 4;
    uint32_t reserved0 : 1;
    uint32_t isLooping : 1;
    uint32_t subframeSkip : 2;
    uint32_t packetMetadata : 16;
    uint32_t currentByteOffset : 26;
    uint32_t reserved1 : 6;
    uint32_t loopStartOffset : 26;
    uint32_t loopEndOffset : 6;
    uint32_t reserved[11];  // the GDK's 64-byte context: the published field list leaves one trailing word
};
static_assert(sizeof(SHAPE_XMA_CONTEXT) == 64, "SHAPE_XMA_CONTEXT is 64 bytes per GDK");

struct SHAPE_PCM_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 11;
    uint32_t loopCount : 8;
    uint32_t reserved1 : 8;
    uint32_t full : 1;
    uint32_t reserved2 : 15;
    uint32_t readPointer : 21;
    uint32_t reserved3 : 11;
    uint32_t loopStartWritePointer : 21;
    uint32_t reserved4 : 11;
    uint32_t loopEnd : 21;
    uint32_t reserved5 : 11;
    uint32_t mode : 1;
    uint32_t reserved6 : 15;
    uint32_t format : 2;
    uint32_t reserved7 : 14;
    uint32_t reserved[2];  // the GDK's 32-byte context: the published field list leaves one trailing word
};
static_assert(sizeof(SHAPE_PCM_CONTEXT) == 32, "SHAPE_PCM_CONTEXT is 32 bytes per GDK");

// The SHAPE_CONTEXTS structure (from the GDK)
struct SHAPE_CONTEXTS {
    uint32_t numSrcContexts;
    uint32_t numEqCompContexts;
    uint32_t numFiltVolContexts;
    uint32_t numDmaContexts;
    uint32_t numXmaContexts;
    uint32_t numPcmContexts;
    SHAPE_SRC_CONTEXT* srcContextArray;
    SHAPE_EQCOMP_CONTEXT* eqCompContextArray;
    SHAPE_FILTVOL_CONTEXT* filtVolContextArray;
    SHAPE_DMA_CONTEXT* dmaContextArray;
    SHAPE_XMA_CONTEXT* xmaContextArray;
    SHAPE_PCM_CONTEXT* pcmContextArray;
    APU_ADDRESS apuSrcContextArray;
    APU_ADDRESS apuEqCompContextArray;
    APU_ADDRESS apuFiltVolContextArray;
    APU_ADDRESS apuDmaContextArray;
    APU_ADDRESS apuXmaContextArray;
    APU_ADDRESS apuPcmContextArray;
};

static_assert(sizeof(SHAPE_CONTEXTS) == 120, "SHAPE_CONTEXTS is 120 bytes per GDK");
static_assert(offsetof(SHAPE_CONTEXTS, xmaContextArray) == 0x38,
              "xmaContextArray is at offset +0x38 per retail binary measurement");

// ---- Startup trace helper ----

void trace_call(const char* api, const std::string& detail) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<std::string> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert(std::string(api) + detail).second) return;
    }
    rt::startup_trace::event("acphal",
                             std::string("\"api\":\"") + api + "\"" + detail);
}

// ---- Genuine APU Heap Implementation ----

constexpr uint32_t kApuBaseAddress = 0x40000000u;

struct ApuRegion {
    void* base = nullptr;
    uint64_t size = 0;
    std::map<uint32_t, uint32_t> blocks;  // offset -> size of active allocations

    bool contains(const void* ptr) const {
        if (!base || !ptr) return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        return p >= b && p < b + size;
    }

    bool is_valid_address(const void* ptr, uint32_t alignment) const {
        if (!base || !ptr) return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        if (p < b || p >= b + size) return false;
        if (alignment == 0) alignment = 1;
        return ((p - b) & (static_cast<uintptr_t>(alignment) - 1)) == 0;
    }

    uint32_t to_offset(const void* ptr) const {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr) -
                                     reinterpret_cast<uintptr_t>(base));
    }

    void* from_offset(uint32_t offset) const {
        if (offset >= size) return nullptr;
        return static_cast<uint8_t*>(base) + offset;
    }

    uint8_t* span_from_offset(uint32_t offset, std::size_t len) const {
        if (!base || len == 0 || offset >= size) return nullptr;
        const uint64_t remaining = size - offset;
        if (len > remaining) return nullptr;
        return static_cast<uint8_t*>(base) + offset;
    }

    void* allocate(uint32_t alloc_size, uint32_t alignment, uint32_t* out_offset) {
        if (alloc_size == 0 || alignment == 0) return nullptr;
        uint64_t at = 0;
        for (auto it = blocks.begin();; ++it) {
            at = (at + alignment - 1) & ~static_cast<uint64_t>(alignment - 1);
            const uint64_t limit = (it == blocks.end()) ? size : it->first;
            if (at + alloc_size <= limit) break;
            if (it == blocks.end()) return nullptr;  // out of memory
            at = static_cast<uint64_t>(it->first) + it->second;
        }
        const uint32_t offset = static_cast<uint32_t>(at);
        blocks.emplace(offset, alloc_size);
        uint8_t* ptr = static_cast<uint8_t*>(base) + offset;
        std::memset(ptr, 0, alloc_size);
        if (out_offset) *out_offset = offset;
        return ptr;
    }

    bool free_block(void* ptr) {
        if (!contains(ptr)) return false;
        const uint32_t offset = to_offset(ptr);
        return blocks.erase(offset) > 0;
    }

    void release() {
        if (base) {
            VirtualFree(base, 0, MEM_RELEASE);
            base = nullptr;
        }
        size = 0;
        blocks.clear();
    }
};

std::mutex g_apu_mutex;
bool g_apu_heap_created = false;
ApuRegion g_cached_heap;
ApuRegion g_noncached_heap;

// Tracked SHAPE context arrays
std::vector<void*> g_active_shape_arrays;

// Resolves a contiguous [apu, apu + len) byte range inside the APU heap.
uint8_t* apu_span(uint32_t apu, std::size_t len) {
    if (apu < kApuBaseAddress || len == 0) return nullptr;
    std::lock_guard lock(g_apu_mutex);
    const uint32_t offset = apu - kApuBaseAddress;
    if (g_cached_heap.base && offset < g_cached_heap.size) {
        return g_cached_heap.span_from_offset(offset, len);
    }
    if (g_noncached_heap.base) {
        const uint64_t cached_span = g_cached_heap.size;
        if (offset >= cached_span && offset - cached_span < g_noncached_heap.size) {
            return g_noncached_heap.span_from_offset(
                static_cast<uint32_t>(offset - cached_span), len);
        }
    }
    return nullptr;
}

// ---- ACP Command / Message Protocol Constants ----

constexpr uint32_t kCmdLoadFlowgraph = 1;
constexpr uint32_t kCmdRegisterMessages = 2;
constexpr uint32_t kCmdUnregisterMessages = 3;
constexpr uint32_t kCmdEnableXma = 5;
constexpr uint32_t kCmdEnableXmas = 6;
constexpr uint32_t kCmdDisableXma = 7;
constexpr uint32_t kCmdDisableXmas = 8;
constexpr uint32_t kCmdAdvanceXmaRead = 18;

constexpr uint32_t kMsgFlowgraphCompleted = 0x02u;
constexpr uint32_t kMsgCommandCompleted = 0x10u;
constexpr uint32_t kMsgDisconnected = 0x80u;

// 128 samples per audio frame at 48 kHz = 375 audio frames/second.
constexpr uint64_t kAudioFramesPerSecond = 375;

// ---- XMA2 Hardware Context Decoder Engine ----
//
// Each 64-byte SHAPE_XMA_CONTEXT holds 16 32-bit words shared with the title:
//   Word 0: input 0 packets (0..11), loop count (12..19, 255 = forever),
//           valid input bits (20..21), output ring blocks (22..26), write block (27..31).
//   Word 1: input 1 packets (0..11), loop end subframe (12..13), loop start skip subframes
//           (17..19), initial skip subframes (24..26), rate index (27..28: 24/32/44.1/48 kHz),
//           stereo (29), output writable (31).
//   Word 2: read bit offset (0..25), error status (26..30), error flag (31).
//   Word 3: loop start bit offset (0..25).
//   Word 4: loop end bit offset (0..25), current input buffer index (31).
//   Word 5: input 0 APU address.
//   Word 6: input 1 APU address.
//   Word 7: output ring APU address.
//   Word 8: overlap buffer APU address.
//   Word 9: output read block index (0..4).

constexpr std::size_t kXmaBlockBytes = 256;
constexpr std::size_t kXmaSamplesPerBlock = kXmaBlockBytes / 2;
constexpr uint32_t kXmaErrorInputInvalid = 4;

struct XmaContextView {
    volatile uint32_t* words = nullptr;

    uint32_t get(std::size_t index) const { return words[index]; }
    void set(std::size_t index, uint32_t value) const { words[index] = value; }

    uint32_t field(std::size_t index, uint32_t shift, uint32_t bits) const {
        const uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        return (get(index) >> shift) & mask;
    }

    void set_field(std::size_t index, uint32_t shift, uint32_t bits, uint32_t value) const {
        const uint32_t raw_mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        const uint32_t mask = raw_mask << shift;
        set(index, (get(index) & ~mask) | ((value << shift) & mask));
    }

    std::size_t channels() const { return 1u + static_cast<std::size_t>(field(1, 29, 1)); }
};

struct XmaVoice {
    std::optional<leonida::xma2::Decoder> decoder;
    std::deque<int16_t> pcm;
    std::size_t skip = 0;  // samples per channel to drop (start/loop subframe skip)
    bool ended = false;
    std::size_t produced = 0;
    uint32_t frames = 0;
    uint32_t bad_frames = 0;
    int16_t peak = 0;
};

struct XmaVoiceSlot {
    std::mutex mutex;
    std::optional<XmaVoice> voice;
};

class XmaEngine {
public:
    ~XmaEngine() { unbind(); }

    void bind(SHAPE_XMA_CONTEXT* array, uint32_t count) {
        stop_worker();
        std::unique_lock lock(table_mutex_);
        array_ = reinterpret_cast<volatile uint32_t*>(array);
        voices_.clear();
        voices_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            voices_.push_back(std::make_unique<XmaVoiceSlot>());
        }
        active_count_.store(0, std::memory_order_relaxed);
    }

    void unbind() {
        stop_worker();
        std::unique_lock lock(table_mutex_);
        array_ = nullptr;
        voices_.clear();
        active_count_.store(0, std::memory_order_relaxed);
    }

    bool handle_command(uint32_t command, const void* config) {
        auto read_word = [config](std::size_t index) -> uint32_t {
            if (!config) return 0u;
            uint32_t value = 0;
            std::memcpy(&value, static_cast<const uint8_t*>(config) + index * sizeof(uint32_t),
                        sizeof(uint32_t));
            return value;
        };

        switch (command) {
            case kCmdEnableXma:
                if (config) enable(read_word(0));
                return true;
            case kCmdDisableXma:
                if (config) disable(read_word(0));
                return true;
            case kCmdEnableXmas:
            case kCmdDisableXmas: {
                std::shared_lock lock(table_mutex_);
                const std::size_t count = voices_.size();
                lock.unlock();
                if (!config) return true;
                for (std::size_t i = 0; i < count; ++i) {
                    const uint32_t bits = read_word(i / 32);
                    if ((bits & (1u << (i % 32))) != 0) {
                        if (command == kCmdEnableXmas) {
                            enable(static_cast<uint32_t>(i));
                        } else {
                            disable(static_cast<uint32_t>(i));
                        }
                    }
                }
                return true;
            }
            case kCmdAdvanceXmaRead:
                if (config) advance_read(read_word(0), read_word(1));
                return true;
            default:
                return false;
        }
    }

    void enable(uint32_t index) {
        {
            std::shared_lock table_lock(table_mutex_);
            XmaContextView ctx = context_locked(index);
            if (!ctx.words) return;

            std::lock_guard slot_lock(voices_[index]->mutex);
            const bool was_active = voices_[index]->voice.has_value();
            // Clear error_status (26..30) and error flag (31).
            ctx.set_field(2, 26, 6, 0);

            XmaVoice voice;
            voice.skip = static_cast<std::size_t>(ctx.field(1, 24, 3)) *
                         leonida::xma2::kSubframeSamples;
            voices_[index]->voice.emplace(std::move(voice));
            if (!was_active) {
                active_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        ensure_worker_and_wake();
    }

    void disable(uint32_t index) {
        std::shared_lock table_lock(table_mutex_);
        if (index >= voices_.size()) return;
        std::lock_guard slot_lock(voices_[index]->mutex);
        if (voices_[index]->voice.has_value()) {
            voices_[index]->voice.reset();
            active_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void advance_read(uint32_t index, uint32_t blocks) {
        bool wake = false;
        {
            std::shared_lock table_lock(table_mutex_);
            XmaContextView ctx = context_locked(index);
            if (!ctx.words) return;

            std::lock_guard slot_lock(voices_[index]->mutex);
            const uint32_t raw_size = ctx.field(0, 22, 5);
            const uint32_t size = raw_size ? raw_size : 1u;
            const uint32_t read = (ctx.field(9, 0, 5) + blocks) % size;
            ctx.set_field(9, 0, 5, read);
            if (blocks > 0) {
                ctx.set_field(1, 31, 1, 1);
            }
            wake = voices_[index]->voice.has_value();
        }
        if (wake) ensure_worker_and_wake();
    }

private:
    XmaContextView context_locked(uint32_t index) const {
        if (!array_ || index >= voices_.size()) return {};
        return XmaContextView{array_ + static_cast<std::size_t>(index) * 16};
    }

    static bool decode_one_frame(XmaContextView ctx, XmaVoice& voice) {
        if (voice.ended) return false;

        const std::size_t channels = ctx.channels();
        uint32_t current = ctx.field(4, 31, 1);
        const uint32_t valid = ctx.field(0, 20, 2);

        if ((valid & (1u << current)) == 0) {
            const uint32_t other = 1u - current;
            if ((valid & (1u << other)) == 0) {
                // Both inputs exhausted: flush remaining overlap tail, then signal INPUT_INVALID.
                if (voice.decoder.has_value()) {
                    std::vector<int16_t> tail;
                    voice.decoder->flush(tail);
                    voice.pcm.insert(voice.pcm.end(), tail.begin(), tail.end());
                }
                voice.ended = true;
                ctx.set_field(2, 26, 5, kXmaErrorInputInvalid);
                ctx.set_field(2, 31, 1, 1);
                return !voice.pcm.empty();
            }
            current = other;
            ctx.set_field(4, 31, 1, current);
            ctx.set_field(2, 0, 26, 0);
        }

        const uint32_t address = (current == 0) ? ctx.get(5) : ctx.get(6);
        const uint32_t packets = (current == 0) ? ctx.field(0, 0, 12) : ctx.field(1, 0, 12);
        const std::size_t input_bytes =
            static_cast<std::size_t>(packets) * leonida::xma2::kPacketBytes;
        const uint8_t* data = apu_span(address & ~0x7FFu, input_bytes);
        if (!data) {
            ctx.set_field(0, 20 + current, 1, 0);
            return true;
        }

        std::size_t at = static_cast<std::size_t>(ctx.field(2, 0, 26));
        if (at < leonida::xma2::kHeaderBits) {
            const auto pkt = leonida::xma2::parse_packet(data, input_bytes, 0);
            at = leonida::xma2::kHeaderBits + (pkt.has_value() ? pkt->first_frame_bit : 0u);
        }

        const auto frame = leonida::xma2::read_frame(data, input_bytes, at);
        if (!frame.has_value()) {
            ctx.set_field(0, 20 + current, 1, 0);
            return true;
        }

        if (!voice.decoder.has_value()) {
            static constexpr uint32_t kRates[4] = {24000, 32000, 44100, 48000};
            const uint32_t rate_idx = ctx.field(1, 27, 2) & 3u;
            voice.decoder.emplace(channels, kRates[rate_idx]);
        }

        std::vector<int16_t> pcm;
        pcm.reserve(leonida::xma2::kFrameSamples * channels);
        ++voice.frames;
        if (!voice.decoder->decode(frame->data.data(), frame->bits, pcm)) {
            ++voice.bad_frames;
        }

        std::size_t next = frame->next.value_or(0);
        const uint32_t loops = ctx.field(0, 12, 8);
        const std::size_t loop_end = static_cast<std::size_t>(ctx.field(4, 0, 26));
        const bool ends_loop =
            loop_end != 0 &&
            (frame->start == loop_end ||
             (frame->start < loop_end && (!frame->next.has_value() || loop_end < *frame->next)));
        if (loops > 0 && ends_loop) {
            const std::size_t keep =
                (static_cast<std::size_t>(ctx.field(1, 12, 2)) + 1u) *
                leonida::xma2::kSubframeSamples * channels;
            if (keep < pcm.size()) pcm.resize(keep);
            next = static_cast<std::size_t>(ctx.field(3, 0, 26));
            voice.skip = static_cast<std::size_t>(ctx.field(1, 17, 3)) *
                         leonida::xma2::kSubframeSamples;
            if (loops != 255u) {
                ctx.set_field(0, 12, 8, loops - 1u);
            }
        }

        const std::size_t skip_samples = std::min(voice.skip * channels, pcm.size());
        voice.skip -= skip_samples / channels;
        voice.pcm.insert(voice.pcm.end(), pcm.begin() + static_cast<std::ptrdiff_t>(skip_samples),
                         pcm.end());

        if (next == 0) {
            ctx.set_field(0, 20 + current, 1, 0);
        }
        ctx.set_field(2, 0, 26, static_cast<uint32_t>(next));
        return true;
    }

    static void pump_voice(XmaContextView ctx, XmaVoice& voice) {
        const uint32_t size = ctx.field(0, 22, 5);
        if (size == 0) return;

        while (ctx.field(1, 31, 1) == 1) {
            int decode_guard = 0;
            while (voice.pcm.size() < kXmaSamplesPerBlock && decode_guard++ < 16) {
                if (!decode_one_frame(ctx, voice)) break;
            }
            if (voice.pcm.empty()) return;

            const uint32_t write = ctx.field(0, 27, 5);
            const uint32_t out_addr =
                ctx.get(7) + write * static_cast<uint32_t>(kXmaBlockBytes);
            uint8_t* out = apu_span(out_addr, kXmaBlockBytes);
            if (!out) return;

            for (std::size_t i = 0; i < kXmaSamplesPerBlock; ++i) {
                int16_t sample = 0;
                if (!voice.pcm.empty()) {
                    sample = voice.pcm.front();
                    voice.pcm.pop_front();
                }
                const int32_t mag =
                    (sample == INT16_MIN) ? INT16_MAX : std::abs(static_cast<int32_t>(sample));
                if (mag > voice.peak) voice.peak = static_cast<int16_t>(mag);
                const uint16_t raw = static_cast<uint16_t>(sample);
                out[2 * i] = static_cast<uint8_t>(raw & 0xFFu);
                out[2 * i + 1] = static_cast<uint8_t>((raw >> 8) & 0xFFu);
            }

            voice.produced += kXmaSamplesPerBlock / ctx.channels();
            const uint32_t next_write = (write + 1u) % size;
            ctx.set_field(0, 27, 5, next_write);
            if (next_write == ctx.field(9, 0, 5)) {
                ctx.set_field(1, 31, 1, 0);
            }
        }
    }

    void pump_all() {
        std::shared_lock table_lock(table_mutex_);
        if (!array_) return;
        for (std::size_t i = 0; i < voices_.size(); ++i) {
            std::lock_guard slot_lock(voices_[i]->mutex);
            if (voices_[i]->voice.has_value()) {
                pump_voice(context_locked(static_cast<uint32_t>(i)), *voices_[i]->voice);
            }
        }
    }

    void ensure_worker_and_wake() {
        std::lock_guard lock(worker_mutex_);
        if (!worker_.joinable()) {
            worker_ = std::jthread([this](std::stop_token stop) { worker_loop(stop); });
        }
        wake_requested_ = true;
        worker_cv_.notify_one();
    }

    // Each worker owns its stop_token, so a stop only ever reaches the thread it was
    // sent to; a worker started after this call is unaffected by it.
    void stop_worker() {
        std::jthread to_join;
        {
            std::lock_guard lock(worker_mutex_);
            if (!worker_.joinable()) return;
            worker_.request_stop();
            worker_cv_.notify_all();
            to_join = std::move(worker_);
        }
        if (to_join.joinable()) to_join.join();
    }

    void worker_loop(std::stop_token stop) {
        for (;;) {
            {
                std::unique_lock lock(worker_mutex_);
                if (stop.stop_requested()) return;
                if (active_count_.load(std::memory_order_relaxed) == 0 && !wake_requested_) {
                    worker_cv_.wait(lock, stop, [this]() { return wake_requested_; });
                    if (stop.stop_requested()) return;
                }
                wake_requested_ = false;
            }

            pump_all();

            std::unique_lock lock(worker_mutex_);
            if (stop.stop_requested()) return;
            if (!wake_requested_ && active_count_.load(std::memory_order_relaxed) > 0) {
                // One 256-byte output block lasts ~2.67 ms at 48 kHz mono.
                worker_cv_.wait_for(lock, stop, std::chrono::milliseconds(2),
                                    [this]() { return wake_requested_; });
            }
        }
    }

    mutable std::shared_mutex table_mutex_;
    volatile uint32_t* array_ = nullptr;
    std::vector<std::unique_ptr<XmaVoiceSlot>> voices_;
    std::atomic<uint32_t> active_count_{0};

    std::mutex worker_mutex_;
    std::condition_variable_any worker_cv_;
    std::jthread worker_;
    bool wake_requested_ = false;
};

XmaEngine g_xma_engine;

// ---- IAcpHal Interface Implementation ----

#pragma pack(push, 1)
struct AcpMessage {
    uint32_t kind = 0;
    uint32_t dropped = 0;
    uint32_t usec = 0;
    uint8_t data[16]{};
};
#pragma pack(pop)
static_assert(sizeof(AcpMessage) == 28, "AcpMessage is 28 bytes");

void write_u32_le(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void write_u64_le(uint8_t* dst, uint64_t value) {
    write_u32_le(dst, static_cast<uint32_t>(value & 0xFFFFFFFFu));
    write_u32_le(dst + 4, static_cast<uint32_t>(value >> 32));
}

struct IAcpHal {
    virtual HRESULT __stdcall connect(uint32_t commands, uint32_t messages) = 0;
    virtual HRESULT __stdcall disconnect() = 0;
    virtual HRESULT __stdcall submit_command(uint32_t cmd, uint64_t id, uint32_t frame,
                                             const void* config, uint32_t notification) = 0;
    virtual uint32_t __stdcall pop_message(void* out) = 0;
    virtual uint32_t __stdcall get_num_messages() = 0;
    virtual HRESULT __stdcall release() = 0;
};

struct AcpHalImpl final : IAcpHal {
    std::mutex mutex_;
    bool connected_ = false;
    uint32_t mask_ = 0;
    std::deque<AcpMessage> messages_;
    std::size_t capacity_ = 0;
    uint32_t dropped_ = 0;
    std::chrono::steady_clock::time_point since_ = std::chrono::steady_clock::now();

    uint64_t elapsed_usec_locked() const {
        const auto dt = std::chrono::steady_clock::now() - since_;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(dt).count());
    }

    uint32_t audio_frame_locked() const {
        return static_cast<uint32_t>((elapsed_usec_locked() * kAudioFramesPerSecond) / 1000000ull);
    }

    void post_locked(uint32_t kind, const uint8_t* payload, std::size_t len) {
        if ((mask_ & kind) == 0) return;
        if (messages_.size() >= capacity_) {
            if (dropped_ < UINT32_MAX) ++dropped_;
            return;
        }
        AcpMessage msg{};
        msg.kind = kind;
        msg.dropped = dropped_;
        dropped_ = 0;
        msg.usec = static_cast<uint32_t>(elapsed_usec_locked());
        if (payload && len > 0) {
            std::memcpy(msg.data, payload, std::min(len, sizeof(msg.data)));
        }
        messages_.push_back(msg);
    }

    // Slot 0: Connect (called at boot with (0x1000, 0x1000), result checked)
    HRESULT __stdcall connect(uint32_t commands, uint32_t messages) override {
        trace_call("AcpHal::Connect", "");
        if (commands == 0) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        connected_ = true;
        capacity_ = messages;
        return S_OK;
    }

    // Slot 1: Disconnect (called at shutdown)
    HRESULT __stdcall disconnect() override {
        trace_call("AcpHal::Disconnect", "");
        std::lock_guard lock(mutex_);
        if (!connected_) return E_FAIL;
        post_locked(kMsgDisconnected, nullptr, 0);
        connected_ = false;
        return S_OK;
    }

    // Slot 2: SubmitCommand
    // Acknowledges command completion, updates registered message masks, dispatches
    // XMA voice commands, and writes any requested APU notification word.
    HRESULT __stdcall submit_command(uint32_t cmd, uint64_t id, uint32_t frame,
                                     const void* config, uint32_t notification) override {
        (void)frame;
        auto read_word = [config](std::size_t index) -> uint32_t {
            if (!config) return 0u;
            uint32_t value = 0;
            std::memcpy(&value, static_cast<const uint8_t*>(config) + index * sizeof(uint32_t),
                        sizeof(uint32_t));
            return value;
        };

        const bool handled_xma = g_xma_engine.handle_command(cmd, config);
        {
            std::lock_guard lock(mutex_);
            if (!handled_xma) {
                switch (cmd) {
                    case kCmdRegisterMessages:
                        mask_ |= read_word(0);
                        break;
                    case kCmdUnregisterMessages:
                        mask_ &= ~read_word(0);
                        break;
                    case kCmdLoadFlowgraph: {
                        uint8_t payload[4];
                        write_u32_le(payload, read_word(1));
                        post_locked(kMsgFlowgraphCompleted, payload, sizeof(payload));
                        break;
                    }
                    default:
                        break;
                }
            }
            if (id != 0) {
                uint8_t done[16];
                write_u32_le(done, cmd);
                write_u64_le(done + 4, id);
                write_u32_le(done + 12, audio_frame_locked());
                post_locked(kMsgCommandCompleted, done, sizeof(done));
            }
        }

        if (notification != 0) {
            if (uint8_t* word_bytes = apu_span(notification, sizeof(uint32_t))) {
                if ((reinterpret_cast<uintptr_t>(word_bytes) & 3u) == 0) {
                    *reinterpret_cast<volatile uint32_t*>(word_bytes) = 1u;
                } else {
                    static constexpr uint8_t kOneLe[4] = {1, 0, 0, 0};
                    volatile uint8_t* dst = word_bytes;
                    for (std::size_t i = 0; i < 4; ++i) dst[i] = kOneLe[i];
                }
            }
        }

        trace_call("AcpHal::SubmitCommand", "");
        return S_OK;
    }

    // Slot 3: PopMessage — returns 0 without touching `out` when the queue is empty.
    uint32_t __stdcall pop_message(void* out) override {
        std::lock_guard lock(mutex_);
        if (messages_.empty()) return 0u;
        const AcpMessage msg = messages_.front();
        messages_.pop_front();
        if (out) std::memcpy(out, &msg, sizeof(msg));
        return 1u;
    }

    // Slot 4: GetNumMessages
    uint32_t __stdcall get_num_messages() override {
        std::lock_guard lock(mutex_);
        return static_cast<uint32_t>(messages_.size());
    }

    // Slot 5: Release
    HRESULT __stdcall release() override {
        trace_call("AcpHal::Release", "");
        delete this;
        return S_OK;
    }
};

// ---- AcpHalCreate ----

HRESULT __stdcall AcpHalCreate(void** ppAcpHal) {
    if (!ppAcpHal) return E_POINTER;
    *ppAcpHal = new AcpHalImpl();
    trace_call("AcpHalCreate", "");
    return S_OK;
}

// ---- AcpHalAllocateShapeContexts ----

HRESULT __stdcall AcpHalAllocateShapeContexts(SHAPE_CONTEXTS* ctx) {
    if (!ctx) return E_POINTER;

    SHAPE_XMA_CONTEXT* xma_array = nullptr;
    uint32_t xma_count = 0;
    {
        std::lock_guard lock(g_apu_mutex);

        auto alloc_array = [](uint32_t count, size_t elem_size) -> void* {
            if (count == 0) return nullptr;
            void* ptr = std::calloc(count, elem_size);
            if (ptr) g_active_shape_arrays.push_back(ptr);
            return ptr;
        };

        ctx->srcContextArray = static_cast<SHAPE_SRC_CONTEXT*>(
            alloc_array(ctx->numSrcContexts, sizeof(SHAPE_SRC_CONTEXT)));
        ctx->eqCompContextArray = static_cast<SHAPE_EQCOMP_CONTEXT*>(
            alloc_array(ctx->numEqCompContexts, sizeof(SHAPE_EQCOMP_CONTEXT)));
        ctx->filtVolContextArray = static_cast<SHAPE_FILTVOL_CONTEXT*>(
            alloc_array(ctx->numFiltVolContexts, sizeof(SHAPE_FILTVOL_CONTEXT)));
        ctx->dmaContextArray = static_cast<SHAPE_DMA_CONTEXT*>(
            alloc_array(ctx->numDmaContexts, sizeof(SHAPE_DMA_CONTEXT)));
        ctx->xmaContextArray = static_cast<SHAPE_XMA_CONTEXT*>(
            alloc_array(ctx->numXmaContexts, sizeof(SHAPE_XMA_CONTEXT)));
        ctx->pcmContextArray = static_cast<SHAPE_PCM_CONTEXT*>(
            alloc_array(ctx->numPcmContexts, sizeof(SHAPE_PCM_CONTEXT)));

        // On PC there is no physical APU address translation for host context arrays;
        // assign unique synthetic addresses from the APU base.
        ctx->apuSrcContextArray = {ctx->srcContextArray ? kApuBaseAddress : 0};
        ctx->apuEqCompContextArray = {ctx->eqCompContextArray ? kApuBaseAddress + 0x1000 : 0};
        ctx->apuFiltVolContextArray = {ctx->filtVolContextArray ? kApuBaseAddress + 0x2000 : 0};
        ctx->apuDmaContextArray = {ctx->dmaContextArray ? kApuBaseAddress + 0x3000 : 0};
        ctx->apuXmaContextArray = {ctx->xmaContextArray ? kApuBaseAddress + 0x4000 : 0};
        ctx->apuPcmContextArray = {ctx->pcmContextArray ? kApuBaseAddress + 0x5000 : 0};

        xma_array = ctx->xmaContextArray;
        xma_count = ctx->numXmaContexts;
    }

    g_xma_engine.bind(xma_array, xma_count);

    trace_call("AcpHalAllocateShapeContexts",
               ",\"src\":" + std::to_string(ctx->numSrcContexts) +
               ",\"xma\":" + std::to_string(ctx->numXmaContexts) +
               ",\"pcm\":" + std::to_string(ctx->numPcmContexts));
    return S_OK;
}

// ---- AcpHalReleaseShapeContexts ----

// The GDK's AcpHalReleaseShapeContexts() takes no argument: it frees what AcpHalAllocateShapeContexts made. It used
// to take a SHAPE_CONTEXTS* and null its six array pointers; a caller passes nothing, so that pointer was whatever RCX
// held and six words were written through it (intermittent heap corruption in audio_stub_test, and in the title's
// memory at its call). The caller's own structure is the caller's to clear.
HRESULT __stdcall AcpHalReleaseShapeContexts() {
    // Stop and detach the XMA worker before freeing backing arrays.
    g_xma_engine.unbind();

    std::lock_guard lock(g_apu_mutex);

    // Free all recorded context allocations
    for (void* p : g_active_shape_arrays) {
        if (p) std::free(p);
    }
    g_active_shape_arrays.clear();

    trace_call("AcpHalReleaseShapeContexts", "");
    return S_OK;
}

// ---- ApuCreateHeap ----

HRESULT __stdcall ApuCreateHeap(uint32_t cachedSizeInBytes,
                                uint32_t nonCachedSizeInBytes) {
    std::lock_guard lock(g_apu_mutex);
    if (g_apu_heap_created) return E_FAIL;

    if (cachedSizeInBytes > 0) {
        g_cached_heap.base = VirtualAlloc(
            nullptr, cachedSizeInBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_cached_heap.base) return E_OUTOFMEMORY;
        g_cached_heap.size = cachedSizeInBytes;
    }

    if (nonCachedSizeInBytes > 0) {
        g_noncached_heap.base = VirtualAlloc(
            nullptr, nonCachedSizeInBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_noncached_heap.base) {
            g_cached_heap.release();
            return E_OUTOFMEMORY;
        }
        g_noncached_heap.size = nonCachedSizeInBytes;
    }

    g_apu_heap_created = true;
    trace_call("ApuCreateHeap",
               ",\"cachedSize\":" + std::to_string(cachedSizeInBytes) +
               ",\"nonCachedSize\":" + std::to_string(nonCachedSizeInBytes));
    return S_OK;
}

// ---- ApuAlloc ----

HRESULT __stdcall ApuAlloc(void** virtualAddress, uint32_t* physicalAddress,
                           uint32_t sizeInBytes, uint32_t alignmentInBytes,
                           uint32_t flags) {
    if (!virtualAddress) return E_POINTER;
    *virtualAddress = nullptr;
    if (physicalAddress) *physicalAddress = 0;

    if (sizeInBytes == 0) return E_INVALIDARG;
    if (alignmentInBytes == 0) alignmentInBytes = 1;
    if ((alignmentInBytes & (alignmentInBytes - 1)) != 0) return E_INVALIDARG;

    std::lock_guard lock(g_apu_mutex);

    // Routing: flags=2 routes to non-cached heap; flags=0 or 1 routes to cached heap.
    // If non-cached heap wasn't created or has no room, fall back to cached heap
    // because PC memory is unified and cache-coherent.
    const bool prefer_noncached = (flags == 2);
    ApuRegion* target = prefer_noncached ? &g_noncached_heap : &g_cached_heap;

    if (!target->base) {
        if (!prefer_noncached) {
            // Lazy default cached heap if ApuCreateHeap wasn't called yet
            constexpr uint32_t kDefaultHeapSize = 64 * 1024 * 1024;
            target->base = VirtualAlloc(nullptr, kDefaultHeapSize,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!target->base) return E_OUTOFMEMORY;
            target->size = kDefaultHeapSize;
            g_apu_heap_created = true;
        } else {
            // Fall back to cached heap
            target = &g_cached_heap;
            if (!target->base) return E_OUTOFMEMORY;
        }
    }

    uint32_t offset = 0;
    void* ptr = target->allocate(sizeInBytes, alignmentInBytes, &offset);
    if (!ptr && prefer_noncached && g_cached_heap.base) {
        // Fall back to cached heap if non-cached heap is full
        target = &g_cached_heap;
        ptr = target->allocate(sizeInBytes, alignmentInBytes, &offset);
    }

    if (!ptr) return E_OUTOFMEMORY;

    *virtualAddress = ptr;
    if (physicalAddress) {
        // Disjoint APU address ranges:
        //   cached:     0x40000000 .. 0x40000000 + cached_size - 1
        //   non-cached: 0x40000000 + cached_size .. 0x40000000 + cached_size + noncached_size - 1
        uint32_t apu = kApuBaseAddress + offset;
        if (target == &g_noncached_heap) {
            apu += static_cast<uint32_t>(g_cached_heap.size);
        }
        *physicalAddress = apu;
    }

    return S_OK;
}

// ---- ApuFree ----

void __stdcall ApuFree(void* virtualAddress) {
    if (!virtualAddress) return;
    std::lock_guard lock(g_apu_mutex);
    if (!g_cached_heap.free_block(virtualAddress)) {
        g_noncached_heap.free_block(virtualAddress);
    }
    trace_call("ApuFree", "");
}

// ---- ApuIsVirtualAddressValid ----

BOOL __stdcall ApuIsVirtualAddressValid(void* virtualAddress, uint32_t alignment) {
    if (!virtualAddress) return FALSE;
    if (alignment == 0) alignment = 1;
    std::lock_guard lock(g_apu_mutex);
    if (g_cached_heap.is_valid_address(virtualAddress, alignment)) return TRUE;
    if (g_noncached_heap.is_valid_address(virtualAddress, alignment)) return TRUE;
    return FALSE;
}

// ---- ApuMapApuAddress ----

void* __stdcall ApuMapApuAddress(uint32_t apu) {
    if (apu == 0 || apu < kApuBaseAddress) return nullptr;
    std::lock_guard lock(g_apu_mutex);
    const uint32_t offset = apu - kApuBaseAddress;
    if (g_cached_heap.base && offset < g_cached_heap.size) {
        return g_cached_heap.from_offset(offset);
    }
    if (g_noncached_heap.base) {
        const uint32_t cached_span = static_cast<uint32_t>(g_cached_heap.size);
        if (offset >= cached_span && offset - cached_span < g_noncached_heap.size) {
            return g_noncached_heap.from_offset(offset - cached_span);
        }
    }
    return nullptr;
}

// ---- ApuMapVirtualAddress ----

uint32_t __stdcall ApuMapVirtualAddress(void* virtualAddress) {
    if (!virtualAddress) return 0;
    std::lock_guard lock(g_apu_mutex);
    if (g_cached_heap.contains(virtualAddress)) {
        return kApuBaseAddress + g_cached_heap.to_offset(virtualAddress);
    }
    if (g_noncached_heap.contains(virtualAddress)) {
        return kApuBaseAddress + static_cast<uint32_t>(g_cached_heap.size) +
               g_noncached_heap.to_offset(virtualAddress);
    }
    return 0;
}

}  // namespace

SHIM_TABLE(acphal,
    {"acphal.dll", "AcpHalAllocateShapeContexts", reinterpret_cast<void*>(&AcpHalAllocateShapeContexts)},
    {"acphal.dll", "AcpHalCreate", reinterpret_cast<void*>(&AcpHalCreate)},
    {"acphal.dll", "AcpHalReleaseShapeContexts", reinterpret_cast<void*>(&AcpHalReleaseShapeContexts)},
    {"acphal.dll", "ApuAlloc", reinterpret_cast<void*>(&ApuAlloc)},
    {"acphal.dll", "ApuCreateHeap", reinterpret_cast<void*>(&ApuCreateHeap)},
    {"acphal.dll", "ApuFree", reinterpret_cast<void*>(&ApuFree)},
    {"acphal.dll", "ApuIsVirtualAddressValid", reinterpret_cast<void*>(&ApuIsVirtualAddressValid)},
    {"acphal.dll", "ApuMapApuAddress", reinterpret_cast<void*>(&ApuMapApuAddress)},
    {"acphal.dll", "ApuMapVirtualAddress", reinterpret_cast<void*>(&ApuMapVirtualAddress)},
);
