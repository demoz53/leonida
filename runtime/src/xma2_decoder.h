// Portable XMA2 packet parser and WMA Pro frame decoder (1–2 channels, 512-sample frames, 128–512 subframes).
//
// Algorithm and canonical Huffman / quantization tables follow FFmpeg's libavcodec/wmaprodec.c and wmaprodata.h
// (LGPL-2.1-or-later, used here under GPL-3.0-or-later), specialized for Xbox XMA2 streams (up to 2 channels, no LFE,
// 2048-byte packets with 32-bit MSB-first packet headers).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace leonida::xma2 {

inline constexpr std::size_t kPacketBytes = 2048;
inline constexpr std::size_t kPacketBits = kPacketBytes * 8;
inline constexpr std::size_t kHeaderBits = 32;
inline constexpr std::size_t kFrameSamples = 512;
inline constexpr std::size_t kSubframeSamples = 128;

struct PacketHeader {
    std::uint32_t frames = 0;
    std::uint32_t first_frame_bit = 0;
    std::uint32_t skip = 0;
};

struct FrameBits {
    std::vector<std::uint8_t> data;
    std::size_t bits = 0;
    std::size_t start = 0;
    std::optional<std::size_t> next;
};

// Parses the 32-bit MSB-first header of packet `packet_index` inside `data[0..size)`.
std::optional<PacketHeader> parse_packet(const std::uint8_t* data, std::size_t size, std::size_t packet_index);

// Gathers one XMA2 frame starting at bit offset `start_bit` inside a buffer of 2048-byte packets, crossing packet
// headers and skipped interleaved stream packets, and computes the bit offset of the next frame if present.
std::optional<FrameBits> read_frame(const std::uint8_t* data, std::size_t size, std::size_t start_bit);

// Half-length inverse MDCT (negated DCT-IV scaled by `scale`) and sine window overlap-add helpers.
void imdct_half(const float* input, float* output, std::size_t n, float scale);
void window_overlap(float* block, const float* window, std::size_t block_len);
const std::vector<float>& sine_window(std::size_t n);

class Decoder {
public:
    static constexpr std::size_t kBlockSizes = 3;
    static constexpr std::size_t kMaxBands = 29;
    static constexpr std::size_t kMaxSubframes = 32;

    Decoder(std::size_t channels, std::uint32_t sample_rate);

    std::size_t channels() const { return channels_; }
    void reset();

    // Decodes one frame (`data`, `bits`) and appends kFrameSamples interleaved 16-bit PCM samples per channel to `out`.
    // Returns false and optionally sets `*error` on a bitstream error (still emitting timing-preserving silence).
    bool decode(const std::uint8_t* data, std::size_t bits, std::vector<std::int16_t>& out,
                const char** error = nullptr);

    // Emits the remaining half-frame overlap tail (kFrameSamples / 2 samples per channel) and resets state.
    void flush(std::vector<std::int16_t>& out);

private:
    struct Channel {
        std::size_t prev_block_len = kFrameSamples;
        bool transmit_coefs = false;
        std::size_t num_subframes = 0;
        std::array<std::size_t, kMaxSubframes> subframe_len{};
        std::size_t cur_subframe = 0;
        std::size_t decoded_samples = 0;
        std::int32_t quant_step = 0;
        bool reuse_sf = false;
        std::int32_t scale_factor_step = 1;
        std::int32_t max_scale_factor = 0;
        std::array<std::array<std::int32_t, kMaxBands>, 2> saved_scale_factors{};
        std::size_t scale_factor_idx = 0;
        std::size_t scale_factors = 0;
        std::size_t table_idx = 0;
        std::size_t num_vec_coeffs = 0;
        std::array<float, kFrameSamples> coeffs{};
        std::array<float, kFrameSamples + kFrameSamples / 2> out{};
    };

    struct Bits;

    bool decode_frame(Bits& b, std::size_t len, const char*& err);
    bool decode_tile_header(Bits& b, const char*& err);
    bool decode_subframe_length(Bits& b, std::size_t offset, std::size_t& out_len, const char*& err) const;
    bool decode_subframe(Bits& b, const char*& err);
    bool decode_channel_transform(Bits& b, const char*& err);
    bool decode_scale_factors(Bits& b, const char*& err);
    bool decode_coefficients(Bits& b, std::size_t c, const char*& err);
    void inverse_channel_transform();

    std::size_t channels_ = 1;
    std::array<std::size_t, kBlockSizes> num_sfb_{};
    std::array<std::array<std::size_t, kMaxBands + 1>, kBlockSizes> sfb_offsets_{};
    std::array<std::array<std::array<std::size_t, kMaxBands>, kBlockSizes>, kBlockSizes> sf_offsets_{};
    std::array<Channel, 2> ch_{};

    std::size_t subframe_len_ = kFrameSamples;
    std::size_t channels_for_cur_subframe_ = 0;
    std::array<std::size_t, 2> channel_indexes_{0, 1};
    std::size_t num_bands_ = 0;
    std::size_t table_idx_ = 0;
    std::uint32_t esc_len_ = 0;
    bool transmit_num_vec_coeffs_ = false;
    bool transform_ = false;
    std::array<bool, kMaxBands> transform_band_{};
    bool parsed_all_subframes_ = false;
};

}  // namespace leonida::xma2
