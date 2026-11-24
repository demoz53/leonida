// Portable unit tests for leonida::xma2 (runtime/src/xma2_decoder.h/.cpp): packet parsing, multi-packet frame walk,
// zero-length damaged packet guard, half-IMDCT accuracy, MDCT+IMDCT overlap-add signal reconstruction, and silent
// stream decoding. Needs no game data or Windows headers.
#include "../src/xma2_decoder.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

void put_bits(std::vector<std::uint8_t>& data, std::size_t pos, std::uint32_t n, std::uint32_t value) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t bit = static_cast<std::uint8_t>((value >> (n - 1u - i)) & 1u);
        const std::size_t p = pos + i;
        data[p / 8u] = static_cast<std::uint8_t>((data[p / 8u] & ~(1u << (7u - (p % 8u)))) | (bit << (7u - (p % 8u))));
    }
}

std::vector<float> imdct_reference(const std::vector<float>& input) {
    const std::size_t n = input.size();
    std::vector<float> out(n, 0.0f);
    for (std::size_t j = 0; j < n; ++j) {
        const double t = static_cast<double>(j + n / 2u) + 0.5 + static_cast<double>(n) / 2.0;
        double sum = 0.0;
        for (std::size_t k = 0; k < n; ++k) {
            sum += static_cast<double>(input[k]) *
                   std::cos(kPi / static_cast<double>(n) * t * (static_cast<double>(k) + 0.5));
        }
        out[j] = -static_cast<float>(sum);
    }
    return out;
}

}  // namespace

int main() {
    using namespace leonida::xma2;

    // 1. Out-of-range index and truncated packet header rejection.
    {
        const std::uint8_t tiny[4]{};
        check(!parse_packet(tiny, 4, std::numeric_limits<std::size_t>::max() / kPacketBytes + 1u).has_value(),
              "parse_packet rejects overflow packet index");
        check(!parse_packet(tiny, 3, 0).has_value(), "parse_packet rejects truncated header");
    }

    // 2. Zero-length damaged frame before start terminates search cleanly.
    {
        std::vector<std::uint8_t> data(kPacketBytes, 0u);
        put_bits(data, 0, 32, 2u << 26);
        const std::size_t start = kHeaderBits + 100u;
        put_bits(data, start, 15, 200u);
        const auto frame = read_frame(data.data(), data.size(), start);
        check(frame.has_value() && frame->bits == 200u && !frame->next.has_value(),
              "zero-length damaged frame before start ends search safely");
    }

    // 3. Multi-packet frame walk (frame crossing boundary into next packet).
    {
        std::vector<std::uint8_t> data(2u * kPacketBytes, 0xffu);
        auto hdr = [](std::uint32_t frames, std::uint32_t first) { return (frames << 26) | (first << 11); };
        put_bits(data, 0, 32, hdr(2, 0));
        put_bits(data, kPacketBits, 32, hdr(1, 48));
        put_bits(data, 32, 15, 100u);
        put_bits(data, 132, 15, 16300u);
        const std::size_t c_start = kPacketBits + 32u + 48u;
        put_bits(data, c_start, 15, 60u);

        const auto a = read_frame(data.data(), data.size(), 32u);
        const auto b = read_frame(data.data(), data.size(), 132u);
        const auto c = read_frame(data.data(), data.size(), c_start);
        check(a && a->bits == 100u && a->next == 132u, "frame A walks to frame B");
        check(b && b->bits == 16300u && b->next == c_start && b->data.size() == (16300u + 7u) / 8u,
              "frame B crosses packet header into packet 1");
        check(c && c->bits == 60u && !c->next.has_value(), "frame C terminates at end of buffer");
    }

    // 4. Fast half-IMDCT matches direct cosine sum for n = 128, 256, 512.
    {
        bool all_close = true;
        for (const std::size_t n : {std::size_t(128), std::size_t(256), std::size_t(512)}) {
            std::vector<float> input(n);
            for (std::size_t i = 0; i < n; ++i) input[i] = static_cast<float>((i * 7919u) % 97u) - 48.0f;
            std::vector<float> fast(n, 0.0f);
            imdct_half(input.data(), fast.data(), n, 1.0f);
            const auto ref = imdct_reference(input);
            for (std::size_t i = 0; i < n; ++i) {
                if (std::fabs(fast[i] - ref[i]) >= 1e-2f * (1.0f + std::fabs(ref[i]))) all_close = false;
            }
        }
        check(all_close, "fast imdct_half matches direct cosine sum for 128/256/512");
    }

    // 5. MDCT + imdct_half + window_overlap reconstructs a continuous sine wave.
    {
        constexpr std::size_t n = 256;
        std::vector<float> signal(n * 6u);
        for (std::size_t i = 0; i < signal.size(); ++i) {
            signal[i] = std::sin(static_cast<float>(i) * 0.05f) * 0.3f;
        }
        auto win = [&](std::size_t i) {
            return std::sin((static_cast<double>(i) + 0.5) * kPi / (2.0 * static_cast<double>(n)));
        };
        std::vector<float> out(n * 7u, 0.0f);
        for (std::size_t block = 0; block < 5u; ++block) {
            const std::size_t base = block * n;
            std::vector<float> coeffs(n, 0.0f);
            for (std::size_t k = 0; k < n; ++k) {
                double sum = 0.0;
                for (std::size_t i = 0; i < 2u * n; ++i) {
                    const double t = static_cast<double>(i) + 0.5 + static_cast<double>(n) / 2.0;
                    sum += -static_cast<double>(signal[base + i]) * win(i) *
                           std::cos(kPi / static_cast<double>(n) * t * (static_cast<double>(k) + 0.5));
                }
                coeffs[k] = static_cast<float>(sum);
            }
            const std::size_t at = base + n / 2u;
            imdct_half(coeffs.data(), out.data() + at, n, 2.0f / static_cast<float>(n));
            if (block > 0u) window_overlap(out.data() + (at - n / 2u), sine_window(n).data(), n);
        }
        bool exact = true;
        for (std::size_t i = n; i < 5u * n; ++i) {
            if (std::fabs(out[i] - signal[i]) >= 1e-3f) exact = false;
        }
        check(exact, "windowed MDCT + half-IMDCT overlap-add reconstructs sine wave within 1e-3");
    }

    // 6. Decode 2 packets of 3 silent mono frames each + overlap flush.
    {
        std::vector<std::uint8_t> data(2u * kPacketBytes, 0xffu);
        for (std::size_t pkt = 0; pkt < 2u; ++pkt) {
            const std::size_t base = pkt * kPacketBits;
            put_bits(data, base, 32, 3u << 26);
            for (std::size_t f = 0; f < 3u; ++f) {
                const std::size_t at = base + 32u + f * 31u;
                put_bits(data, at, 15, 31u);
                put_bits(data, at + 15u, 2, 0b10u);  // fixed layout, 1x 512-sample subframe
                put_bits(data, at + 17u, 14, 0u);    // gain, no trim, empty subframe
            }
        }
        Decoder dec(1, 48000);
        std::vector<std::int16_t> pcm;
        std::optional<std::size_t> cursor = 32u;
        int frames = 0;
        int errors = 0;
        while (cursor) {
            const auto frame = read_frame(data.data(), data.size(), *cursor);
            if (!frame) break;
            if (!dec.decode(frame->data.data(), frame->bits, pcm)) ++errors;
            ++frames;
            cursor = frame->next;
        }
        dec.flush(pcm);
        bool all_zero = true;
        for (std::int16_t s : pcm) {
            if (s != 0) all_zero = false;
        }
        check(frames == 6 && errors == 0 && pcm.size() == 6u * kFrameSamples + kFrameSamples / 2u && all_zero,
              "6 silent mono frames + flush produce 3328 zero PCM samples with 0 errors");
    }

    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
