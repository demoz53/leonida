// CPU-only unit tests for Depth24Converter (runtime/src/vulkan_depth24.h): integer overflow guards in prepare(),
// make_part(), scratch(), and run(), plus descriptor/buffer/pool lifecycle under synthetic device procs.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "../src/vulkan_depth24.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using leonida::Depth24Converter;
using leonida::Depth24Job;

namespace {

template <typename Handle>
Handle make_handle(std::uintptr_t id) {
    return reinterpret_cast<Handle>(id);
}

struct RecordedDispatch {
    std::uint32_t x = 0, y = 0, z = 0;
};

struct FakeVulkan {
    std::uintptr_t next_id = 1;
    std::unordered_set<std::uintptr_t> live_shader_modules;
    std::unordered_set<std::uintptr_t> live_set_layouts;
    std::unordered_set<std::uintptr_t> live_pipeline_layouts;
    std::unordered_set<std::uintptr_t> live_pipelines;
    std::unordered_set<std::uintptr_t> live_pools;
    std::unordered_map<std::uintptr_t, VkDeviceSize> live_buffers;
    std::unordered_set<std::uintptr_t> live_memory;
    std::unordered_map<std::uintptr_t, std::uint64_t> semaphore_values;
    std::vector<RecordedDispatch> dispatches;
    std::vector<VkDescriptorBufferInfo> updated_buffers;

    int create_buffer_calls = 0;
    int destroy_buffer_calls = 0;
    int allocate_calls = 0;
    int wait_idle_calls = 0;
    const char* omit_proc = nullptr;
};

FakeVulkan* g_vk = nullptr;
const VkDevice k_device = make_handle<VkDevice>(0x100);
const VkBuffer k_app_buf = make_handle<VkBuffer>(0x200);
const VkBuffer k_user_scratch_buf = make_handle<VkBuffer>(0x201);
const VkCommandBuffer k_cmd = make_handle<VkCommandBuffer>(0x300);

VKAPI_ATTR VkResult VKAPI_CALL fake_create_shader_module(
    VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule* out) {
    const std::uintptr_t id = g_vk->next_id++;
    g_vk->live_shader_modules.insert(id);
    *out = make_handle<VkShaderModule>(id);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_shader_module(
    VkDevice, VkShaderModule module, const VkAllocationCallbacks*) {
    g_vk->live_shader_modules.erase(reinterpret_cast<std::uintptr_t>(module));
}

VKAPI_ATTR VkResult VKAPI_CALL fake_create_descriptor_set_layout(
    VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* out) {
    const std::uintptr_t id = g_vk->next_id++;
    g_vk->live_set_layouts.insert(id);
    *out = make_handle<VkDescriptorSetLayout>(id);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_descriptor_set_layout(
    VkDevice, VkDescriptorSetLayout layout, const VkAllocationCallbacks*) {
    g_vk->live_set_layouts.erase(reinterpret_cast<std::uintptr_t>(layout));
}

VKAPI_ATTR VkResult VKAPI_CALL fake_create_pipeline_layout(
    VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout* out) {
    const std::uintptr_t id = g_vk->next_id++;
    g_vk->live_pipeline_layouts.insert(id);
    *out = make_handle<VkPipelineLayout>(id);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_pipeline_layout(
    VkDevice, VkPipelineLayout layout, const VkAllocationCallbacks*) {
    g_vk->live_pipeline_layouts.erase(reinterpret_cast<std::uintptr_t>(layout));
}

VKAPI_ATTR VkResult VKAPI_CALL fake_create_compute_pipelines(
    VkDevice, VkPipelineCache, std::uint32_t count, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*,
    VkPipeline* out) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uintptr_t id = g_vk->next_id++;
        g_vk->live_pipelines.insert(id);
        out[i] = make_handle<VkPipeline>(id);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_pipeline(
    VkDevice, VkPipeline pipeline, const VkAllocationCallbacks*) {
    g_vk->live_pipelines.erase(reinterpret_cast<std::uintptr_t>(pipeline));
}

VKAPI_ATTR VkResult VKAPI_CALL fake_create_descriptor_pool(
    VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool* out) {
    const std::uintptr_t id = g_vk->next_id++;
    g_vk->live_pools.insert(id);
    *out = make_handle<VkDescriptorPool>(id);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_descriptor_pool(
    VkDevice, VkDescriptorPool pool, const VkAllocationCallbacks*) {
    g_vk->live_pools.erase(reinterpret_cast<std::uintptr_t>(pool));
}

VKAPI_ATTR VkResult VKAPI_CALL fake_allocate_descriptor_sets(
    VkDevice, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out) {
    for (std::uint32_t i = 0; i < info->descriptorSetCount; ++i)
        out[i] = make_handle<VkDescriptorSet>(g_vk->next_id++);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_update_descriptor_sets(
    VkDevice, std::uint32_t write_count, const VkWriteDescriptorSet* writes, std::uint32_t,
    const VkCopyDescriptorSet*) {
    for (std::uint32_t i = 0; i < write_count; ++i) {
        if (writes[i].pBufferInfo) g_vk->updated_buffers.push_back(writes[i].pBufferInfo[0]);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL fake_create_buffer(
    VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* out) {
    ++g_vk->create_buffer_calls;
    const std::uintptr_t id = g_vk->next_id++;
    g_vk->live_buffers[id] = info->size;
    *out = make_handle<VkBuffer>(id);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_buffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) {
    ++g_vk->destroy_buffer_calls;
    g_vk->live_buffers.erase(reinterpret_cast<std::uintptr_t>(buffer));
}

VKAPI_ATTR void VKAPI_CALL fake_get_buffer_memory_requirements(
    VkDevice, VkBuffer buffer, VkMemoryRequirements* reqs) {
    const auto it = g_vk->live_buffers.find(reinterpret_cast<std::uintptr_t>(buffer));
    reqs->size = it != g_vk->live_buffers.end() ? it->second : 0;
    reqs->alignment = 256;
    reqs->memoryTypeBits = 1;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_bind_buffer_memory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_free_memory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    g_vk->live_memory.erase(reinterpret_cast<std::uintptr_t>(memory));
}

VKAPI_ATTR void VKAPI_CALL fake_cmd_bind_pipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}

VKAPI_ATTR void VKAPI_CALL fake_cmd_bind_descriptor_sets(
    VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, std::uint32_t, std::uint32_t, const VkDescriptorSet*,
    std::uint32_t, const std::uint32_t*) {}

VKAPI_ATTR void VKAPI_CALL fake_cmd_push_constants(
    VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t, const void*) {}

VKAPI_ATTR void VKAPI_CALL fake_cmd_dispatch(
    VkCommandBuffer, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    g_vk->dispatches.push_back({x, y, z});
}

VKAPI_ATTR VkResult VKAPI_CALL fake_get_semaphore_counter_value(
    VkDevice, VkSemaphore semaphore, std::uint64_t* value) {
    const auto it = g_vk->semaphore_values.find(reinterpret_cast<std::uintptr_t>(semaphore));
    *value = it != g_vk->semaphore_values.end() ? it->second : 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_device_wait_idle(VkDevice) {
    ++g_vk->wait_idle_calls;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_get_device_proc(VkDevice, const char* name) {
    if (g_vk->omit_proc && std::strcmp(name, g_vk->omit_proc) == 0) return nullptr;
    if (std::strcmp(name, "vkCreateShaderModule") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_create_shader_module);
    if (std::strcmp(name, "vkDestroyShaderModule") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_destroy_shader_module);
    if (std::strcmp(name, "vkCreateDescriptorSetLayout") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_create_descriptor_set_layout);
    if (std::strcmp(name, "vkDestroyDescriptorSetLayout") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_destroy_descriptor_set_layout);
    if (std::strcmp(name, "vkCreatePipelineLayout") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_create_pipeline_layout);
    if (std::strcmp(name, "vkDestroyPipelineLayout") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_destroy_pipeline_layout);
    if (std::strcmp(name, "vkCreateComputePipelines") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_create_compute_pipelines);
    if (std::strcmp(name, "vkDestroyPipeline") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_destroy_pipeline);
    if (std::strcmp(name, "vkCreateDescriptorPool") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_create_descriptor_pool);
    if (std::strcmp(name, "vkDestroyDescriptorPool") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_destroy_descriptor_pool);
    if (std::strcmp(name, "vkAllocateDescriptorSets") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_allocate_descriptor_sets);
    if (std::strcmp(name, "vkUpdateDescriptorSets") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_update_descriptor_sets);
    if (std::strcmp(name, "vkCreateBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(fake_create_buffer);
    if (std::strcmp(name, "vkDestroyBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(fake_destroy_buffer);
    if (std::strcmp(name, "vkGetBufferMemoryRequirements") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_get_buffer_memory_requirements);
    if (std::strcmp(name, "vkBindBufferMemory") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_bind_buffer_memory);
    if (std::strcmp(name, "vkFreeMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(fake_free_memory);
    if (std::strcmp(name, "vkCmdBindPipeline") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_cmd_bind_pipeline);
    if (std::strcmp(name, "vkCmdBindDescriptorSets") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_cmd_bind_descriptor_sets);
    if (std::strcmp(name, "vkCmdPushConstants") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_cmd_push_constants);
    if (std::strcmp(name, "vkCmdDispatch") == 0) return reinterpret_cast<PFN_vkVoidFunction>(fake_cmd_dispatch);
    if (std::strcmp(name, "vkGetSemaphoreCounterValue") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_get_semaphore_counter_value);
    if (std::strcmp(name, "vkDeviceWaitIdle") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(fake_device_wait_idle);
    return nullptr;
}

VkDeviceMemory fake_allocate(const VkMemoryRequirements&) {
    ++g_vk->allocate_calls;
    const std::uintptr_t id = g_vk->next_id++;
    g_vk->live_memory.insert(id);
    return make_handle<VkDeviceMemory>(id);
}

Depth24Job make_basic_job(std::uint32_t width = 1, std::uint32_t height = 1, std::uint32_t slices = 1) {
    Depth24Job job;
    job.to_float = true;
    job.app = k_app_buf;
    job.app_size = 4096;
    job.width = width;
    job.height = height;
    job.slices = slices;
    job.app_row_words = width;
    job.app_slice_words = width * height;
    return job;
}

}  // namespace

int main() {
    constexpr std::uint64_t k_u64_max = std::numeric_limits<std::uint64_t>::max();

    // 1-5: prepare() validation & overflow guards fail cleanly without allocating scratch buffers.
    {
        FakeVulkan vk;
        g_vk = &vk;
        {
            Depth24Converter conv(fake_get_device_proc, k_device, fake_allocate);
            EXPECT(conv.loaded());
            std::vector<Depth24Job> parts;

            // 1. app_offset > app_size must not wrap app_offset + span_words * 4 past UINT64_MAX.
            {
                Depth24Job job = make_basic_job();
                job.app_offset = k_u64_max - 3u;
                job.app_size = 4096;
                EXPECT(!conv.prepare(job, parts));
                EXPECT(job.scratch == VK_NULL_HANDLE && vk.create_buffer_calls == 0);
            }

            // 2. span_words * 4 overflow must fail cleanly even when app_size == UINT64_MAX.
            {
                Depth24Job job = make_basic_job(1, 1, 0x80000001u);
                job.app_row_words = 1;
                job.app_slice_words = 0x80000000u;
                job.app_size = k_u64_max;
                EXPECT(!conv.prepare(job, parts));
                EXPECT(job.scratch == VK_NULL_HANDLE && vk.create_buffer_calls == 0);
            }

            // 3. sw * h * s * 4 overflow must fail cleanly even when span_bytes fits (rgb96: 3->4 words).
            {
                Depth24Job job;
                job.to_float = true;
                job.rgb96 = true;
                job.app = k_app_buf;
                job.app_size = k_u64_max;
                job.width = 16;
                job.height = 0x04000000u;
                job.slices = 0x40000000u;
                job.app_row_words = 48;
                job.app_slice_words = 0xC0000000u;
                EXPECT(!conv.prepare(job, parts));
                EXPECT(job.scratch == VK_NULL_HANDLE && vk.create_buffer_calls == 0);
            }

            // 4. Caller-provided scratch buffer rejects unaligned or overflowing scratch_offset.
            {
                Depth24Job bad_align = make_basic_job();
                bad_align.scratch = k_user_scratch_buf;
                bad_align.scratch_offset = 2;
                EXPECT(!conv.prepare(bad_align, parts));

                Depth24Job overflow_scratch = make_basic_job();
                overflow_scratch.scratch = k_user_scratch_buf;
                overflow_scratch.scratch_offset = k_u64_max - 3u;  // % 4 == 0, (2^64 - 4) + 4 == 2^64
                EXPECT(!conv.prepare(overflow_scratch, parts));
                overflow_scratch.height = 2;
                overflow_scratch.app_slice_words = 2;
                overflow_scratch.scratch_offset = k_u64_max - 7u;  // % 4 == 0, total = 8 -> (2^64 - 8) + 8 == 2^64
                EXPECT(!conv.prepare(overflow_scratch, parts));
                EXPECT(vk.create_buffer_calls == 0);
            }
        }
        {
            // 5. max_range <= 256 or a single row wider than max_range fails before creating scratch buffers.
            Depth24Converter tiny_256(fake_get_device_proc, k_device, fake_allocate, 256);
            std::vector<Depth24Job> parts;
            Depth24Job job1 = make_basic_job();
            EXPECT(!tiny_256.prepare(job1, parts));
            EXPECT(job1.scratch == VK_NULL_HANDLE && vk.create_buffer_calls == 0);

            Depth24Converter tiny_300(fake_get_device_proc, k_device, fake_allocate, 300);
            Depth24Job job2 = make_basic_job(32, 1, 1);
            EXPECT(!tiny_300.prepare(job2, parts));
            EXPECT(job2.scratch == VK_NULL_HANDLE && vk.create_buffer_calls == 0);
        }
        EXPECT(vk.live_buffers.empty() && vk.live_memory.empty() && vk.live_pools.empty() &&
               vk.live_pipelines.empty() && vk.live_pipeline_layouts.empty() && vk.live_set_layouts.empty() &&
               vk.live_shader_modules.empty());
    }

    // 6. scratch() start + bytes wraparound: a UINT64_MAX - 3 request after a 4-byte allocation must spill to buf2.
    {
        FakeVulkan vk;
        g_vk = &vk;
        {
            Depth24Converter conv(fake_get_device_proc, k_device, fake_allocate, k_u64_max);
            std::vector<Depth24Job> parts;

            Depth24Job small_job = make_basic_job();
            EXPECT(conv.prepare(small_job, parts));
            const VkBuffer buf1 = small_job.scratch;
            EXPECT(buf1 != VK_NULL_HANDLE && small_job.scratch_offset == 0 && vk.create_buffer_calls == 1);

            Depth24Job huge_job = make_basic_job(3, 715827883u, 2147483647u);
            huge_job.app_size = k_u64_max;
            huge_job.app_row_words = 3;
            huge_job.app_slice_words = 0x80000001u;
            EXPECT(conv.prepare(huge_job, parts));
            EXPECT(parts.size() == 2);
            EXPECT(huge_job.scratch != VK_NULL_HANDLE && huge_job.scratch != buf1);
            EXPECT(huge_job.scratch_offset == 0 && vk.create_buffer_calls == 2);
        }
        EXPECT(vk.live_buffers.empty() && vk.live_memory.empty() && vk.live_pools.empty() &&
               vk.live_pipelines.empty() && vk.live_pipeline_layouts.empty() && vk.live_set_layouts.empty() &&
               vk.live_shader_modules.empty());
    }

    // 7. scratch() chunk.used + 255 wraparound: a 4-byte allocation after UINT64_MAX - 3 must spill to buf2.
    {
        FakeVulkan vk;
        g_vk = &vk;
        {
            Depth24Converter conv(fake_get_device_proc, k_device, fake_allocate, k_u64_max);
            std::vector<Depth24Job> parts;

            Depth24Job huge_job = make_basic_job(3, 715827883u, 2147483647u);
            huge_job.app_size = k_u64_max;
            huge_job.app_row_words = 3;
            huge_job.app_slice_words = 0x80000001u;
            EXPECT(conv.prepare(huge_job, parts));
            const VkBuffer buf1 = huge_job.scratch;
            EXPECT(parts.size() == 2 && buf1 != VK_NULL_HANDLE && vk.create_buffer_calls == 1);

            Depth24Job small_job = make_basic_job();
            EXPECT(conv.prepare(small_job, parts));
            EXPECT(small_job.scratch != VK_NULL_HANDLE && small_job.scratch != buf1);
            EXPECT(small_job.scratch_offset == 0 && vk.create_buffer_calls == 2);
        }
        EXPECT(vk.live_buffers.empty() && vk.live_memory.empty() && vk.live_pools.empty() &&
               vk.live_pipelines.empty() && vk.live_pipeline_layouts.empty() && vk.live_set_layouts.empty() &&
               vk.live_shader_modules.empty());
    }

    // 8 & 9. run() 32-bit workgroup count promotion, descriptor window ranges, retire/collect, and full cleanup.
    {
        FakeVulkan vk;
        g_vk = &vk;
        const VkSemaphore sem = make_handle<VkSemaphore>(0x501);
        {
            Depth24Converter conv(fake_get_device_proc, k_device, fake_allocate, 1024);
            std::vector<Depth24Job> parts;

            Depth24Job job = make_basic_job(16, 9, 1);
            job.app_offset = 12;
            EXPECT(conv.prepare(job, parts));
            EXPECT(parts.size() == 1);
            EXPECT(vk.updated_buffers.size() == 2);
            EXPECT(vk.updated_buffers[0].buffer == k_app_buf && vk.updated_buffers[0].offset == 0 &&
                   vk.updated_buffers[0].range == 12u + 16u * 9u * 4u);
            EXPECT(vk.updated_buffers[1].buffer == job.scratch && vk.updated_buffers[1].offset == 0 &&
                   vk.updated_buffers[1].range == 16u * 9u * 4u);

            conv.run(k_cmd, parts[0]);
            EXPECT(vk.dispatches.size() == 1);
            EXPECT(vk.dispatches[0].x == 2 && vk.dispatches[0].y == 2 && vk.dispatches[0].z == 1);

            Depth24Job huge_dims = parts[0];
            huge_dims.width = 0xFFFFFFF9u;
            huge_dims.height = 0xFFFFFFFFu;
            huge_dims.slices = 3;
            conv.run(k_cmd, huge_dims);
            EXPECT(vk.dispatches.size() == 2);
            EXPECT(vk.dispatches[1].x == 0x20000000u && vk.dispatches[1].y == 0x20000000u &&
                   vk.dispatches[1].z == 3u);

            conv.retire(sem, 1);
            vk.semaphore_values[reinterpret_cast<std::uintptr_t>(sem)] = 1;
            conv.collect();
            EXPECT(vk.live_pools.empty() && vk.live_buffers.size() == 1);
        }
        EXPECT(vk.wait_idle_calls == 1);
        EXPECT(vk.live_buffers.empty() && vk.live_memory.empty() && vk.live_pools.empty() &&
               vk.live_pipelines.empty() && vk.live_pipeline_layouts.empty() && vk.live_set_layouts.empty() &&
               vk.live_shader_modules.empty());
    }

    std::printf("depth24_bounds: all checks passed\n");
    return 0;
}
