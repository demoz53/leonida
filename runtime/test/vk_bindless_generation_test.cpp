// What a new generation of the bindless set 0 costs in descriptor writes (I8). A flush that changes a live entry of
// the hash table starts a new generation: new table buffers and a new descriptor set, so work still running keeps an
// unchanged table. Before, every generation was a new pool and set into which every descriptor written so far was
// written again (write_shadow): ~94k on a full-size set, for a one-entry change. Now a retired generation whose
// submissions are done is kept as a spare set, and the next generation reuses it, writing only what changed since it
// was current (the delta log).
//
// A real Vulkan device and a BindlessSet with 3000 images; vkUpdateDescriptorSets is intercepted to count the
// descriptors written. Each round re-maps one live key (a replacing flush), runs the flush's command buffer to
// completion and releases the retired generation. Checked: the rounds after the first write a handful of
// descriptors, not thousands; the table still finds every key afterwards; no validation errors. The counts are
// printed (before the change: every round rewrote every written descriptor).
// Skips with exit code 77 without a Vulkan 1.3 device with the bindless features. No game data.
#include "vk_test_device.h"
#include "../src/vulkan_bindless.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

PFN_vkGetDeviceProcAddr g_real_get_device = nullptr;
PFN_vkUpdateDescriptorSets g_real_update = nullptr;
std::uint64_t g_written = 0;  // descriptors written through vkUpdateDescriptorSets

VKAPI_ATTR void VKAPI_CALL update_sets(VkDevice device, std::uint32_t count, const VkWriteDescriptorSet* writes,
                                       std::uint32_t copies, const VkCopyDescriptorSet* copy) {
    for (std::uint32_t i = 0; i < count; ++i) g_written += writes[i].descriptorCount;
    g_real_update(device, count, writes, copies, copy);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL get_device(VkDevice device, const char* name) {
    if (std::string_view(name) == "vkUpdateDescriptorSets") return reinterpret_cast<PFN_vkVoidFunction>(update_sets);
    return g_real_get_device(device, name);
}

leonida::DescriptorKey key_of(std::uint32_t n) {
    leonida::DescriptorKey key{};
    key[0] = 0xA5000000u | n;
    key[3] = n * 2654435761u;
    return key;
}

}  // namespace

int main() {
    using namespace leonida;
    vktest::Device d;
    if (!d.create(false) || !d.bindless) {
        std::fprintf(stderr, "vk_bindless_generation_test: no Vulkan 1.3 device with the bindless features, skipped\n");
        return 77;
    }
    g_real_get_device = d.device_proc;
    g_real_update = d.fn<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
    EXPECT(g_real_update);

    BindlessFeatures features{};
    features.partially_bound = d.partially_bound;
    constexpr std::uint32_t images = 3000, rounds = 8;
    VkPhysicalDeviceMemoryProperties memory{};
    d.instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(d.physical, &memory);
    {
        BindlessSet set(get_device, d.device, memory, features);
        EXPECT(set.ok());
        std::vector<std::uint32_t> index(images);
        for (std::uint32_t i = 0; i < images; ++i) {
            index[i] = set.add_image(ImageDimension::dim_2d, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
            EXPECT(index[i] != 0);
            EXPECT(set.insert_key(key_of(i), index[i]));
        }
        std::uint64_t ticket = 0;
        const auto flush = [&] {
            const std::uint64_t before = g_written;
            bool ok = false;
            const bool ran = d.run([&](VkCommandBuffer cb) {
                if (!set.fallbacks_ready()) set.record_fallback_init(cb);
                ok = set.flush(cb, ++ticket);
                return ok;
            });
            if (!ran) return -1;
            set.table_upload_result(true);
            set.release_retired(ticket);  // the submission is done: every retired generation is free
            return ok ? int(g_written - before) : -1;
        };
        const int first = flush();
        EXPECT(first >= 0);
        std::printf("first flush: %d descriptors written (%u images)\n", first, images);

        std::vector<int> written;
        const auto start = std::chrono::steady_clock::now();
        for (std::uint32_t r = 0; r < rounds; ++r) {
            // Re-map a live key: the next flush must start a new generation.
            const std::uint32_t k = r * 7 % images;
            EXPECT(set.insert_key(key_of(k), index[(k + 1) % images]));
            EXPECT(set.flush_replaces_table());
            const std::uint64_t made = set.generations_made();
            const int n = flush();
            EXPECT(n >= 0 && set.generations_made() == made + 1);
            written.push_back(n);
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("descriptors written per new generation:");
        for (const int n : written) std::printf(" %d", n);
        std::printf("\n%u generations in %.2f ms (with their submissions)\n", rounds, ms);
        // The first replacement may need a new set (no spare yet); from the second on a spare is reused and only the
        // delta is written: the re-mapped key changes no descriptor, so at most the table binding and a few more.
        for (std::size_t r = 1; r < written.size(); ++r) EXPECT(written[r] <= 16);
        for (std::uint32_t i = 0; i < images; ++i) EXPECT(set.table().lookup(key_of(i)) != 0);
        EXPECT(d.run([&](VkCommandBuffer) { return true; }));
    }
    const unsigned errors = d.validation_errors();
    if (errors) {
        std::fprintf(stderr, "vk_bindless_generation_test: %u validation errors\n", errors);
        return 1;
    }
    std::printf("vk_bindless_generation_test: PASS\n");
    return 0;
}
