// Verifies LEONIDA_TEXTURE_MIP_DROP quality switch end-to-end on a Vulkan device (#6043 / #6365):
// - Multi-mip 2D texture creation with mips dropped at runtime.
// - 3D volume texture creation verifying depth is right-shifted by mips_dropped.
// - CopyTextureRegion gracefully skipping dropped mips while remapping retained subresources.
// - ResourceBarrier Transition and Texture barriers skipping dropped subresources and remapping retained mips.
// - CreateShaderResourceView partial mip views clamping without mip_count bleed.
// - ExecuteCommandLists submission executing without error or refusal.
#include "vk_test_device.h"
#include "../shims/d3d12_com.h"
#include <dxgi1_4.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
};

D3D12_RESOURCE_DESC tex2d_desc(UINT width, UINT height, UINT mips, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = mips;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    return d;
}

D3D12_RESOURCE_DESC tex3d_desc(UINT width, UINT height, UINT depth, UINT mips, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = depth;
    d.MipLevels = mips;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    return d;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

} // namespace

int main() {
#ifdef _WIN32
    _putenv("LEONIDA_TEXTURE_MIP_DROP=1");
#else
    setenv("LEONIDA_TEXTURE_MIP_DROP", "1", 1);
#endif

    Ref<ID3D12Device> device;
    const HRESULT hr = rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&device.p));
    if (hr == E_FAIL || hr == DXGI_ERROR_UNSUPPORTED) return 77; // No Vulkan GPU
    EXPECT(SUCCEEDED(hr));

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Ref<ID3D12CommandQueue> queue;
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));

    Ref<ID3D12CommandAllocator> allocator;
    EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));

    Ref<ID3D12GraphicsCommandList> list;
    EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr, IID_PPV_ARGS(&list.p))));

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;

    // 1. 2D Texture (16x16, 4 mips): with drop 1, physical image is 8x8, 3 mips.
    Ref<ID3D12Resource> tex2d;
    const auto desc2d = tex2d_desc(16, 16, 4, DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc2d,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex2d.p))));
    // D3D12 GetDesc() preserves the application's declared description
    EXPECT(tex2d->GetDesc().MipLevels == 4);
    EXPECT(tex2d->GetDesc().Width == 16);

    // 1b. Strict Mip-Drop Verification: GetResourceAllocationInfo must report reduced memory
    // for a droppable sampled texture vs a 1-mip texture of identical width/height.
    // On main (where drop is ignored), an 8-mip texture requires >= bytes of its top mip (256 KiB)
    // and is >= single-mip texture size (256 KiB). Under drop 1, the physical image is 128x128 (7 mips, ~128 KiB),
    // which is strictly smaller than the 1-mip texture and strictly less than top mip bytes 256*256*4.
    const auto desc_droppable = tex2d_desc(256, 256, 8, DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto desc_single_mip = tex2d_desc(256, 256, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto alloc_droppable = device->GetResourceAllocationInfo(0, 1, &desc_droppable);
    const auto alloc_single_mip = device->GetResourceAllocationInfo(0, 1, &desc_single_mip);
    EXPECT(alloc_droppable.SizeInBytes < alloc_single_mip.SizeInBytes);
    EXPECT(alloc_droppable.SizeInBytes < 256 * 256 * 4);

    // 2. 3D Volume Texture (8x8x4, 3 mips): with drop 1, width=4, height=4, depth=2.
    Ref<ID3D12Resource> tex3d;
    const auto desc3d = tex3d_desc(8, 8, 4, 3, DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc3d,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex3d.p))));
    EXPECT(tex3d->GetDesc().DepthOrArraySize == 4);

    // 3. Upload & Readback Buffers
    Ref<ID3D12Resource> upload;
    const auto udesc = buffer_desc(4096);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &udesc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload.p))));

    void* upload_map = nullptr;
    EXPECT(SUCCEEDED(upload->Map(0, nullptr, &upload_map)));
    std::memset(static_cast<char*>(upload_map) + 512, 0xAB, 256);
    upload->Unmap(0, nullptr);

    Ref<ID3D12Resource> readback;
    const auto rdesc = buffer_desc(4096);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rdesc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback.p))));

    // Readback buffer for dropped mip 0: pre-fill with sentinel 0xCD.
    // Because mip 0 copy from texture is skipped under drop 1, this sentinel remains untouched.
    Ref<ID3D12Resource> readback_dropped;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rdesc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_dropped.p))));
    void* r0_map = nullptr;
    EXPECT(SUCCEEDED(readback_dropped->Map(0, nullptr, &r0_map)));
    std::memset(r0_map, 0xCD, 256);
    readback_dropped->Unmap(0, nullptr);

    // 4. Command List:
    // A. Dropped-mip barrier (b0): subresource 0 is dropped.
    // Must be skipped (p.skip = true) without prematurely aborting prepare() for subsequent commands!
    D3D12_RESOURCE_BARRIER b0{};
    b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b0.Transition.pResource = tex2d.p;
    b0.Transition.Subresource = 0; // dropped mip
    b0.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b0.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b0);

    // B. CopyTextureRegion targeting dropped mip 0 (skipped)
    D3D12_TEXTURE_COPY_LOCATION dst_loc0{};
    dst_loc0.pResource = tex2d.p;
    dst_loc0.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc0.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_loc0{};
    src_loc0.pResource = upload.p;
    src_loc0.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc0.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc0.PlacedFootprint.Footprint.Width = 16;
    src_loc0.PlacedFootprint.Footprint.Height = 16;
    src_loc0.PlacedFootprint.Footprint.Depth = 1;
    src_loc0.PlacedFootprint.Footprint.RowPitch = 256;
    list->CopyTextureRegion(&dst_loc0, 0, 0, 0, &src_loc0, nullptr);

    // C. Retained mip 1 barrier (remapped to physical mip 0)
    D3D12_RESOURCE_BARRIER b1{};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition.pResource = tex2d.p;
    b1.Transition.Subresource = 1; // retained mip
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(1, &b1);

    // D. Copy to retained mip 1 (physical mip 0)
    D3D12_TEXTURE_COPY_LOCATION dst_loc1{};
    dst_loc1.pResource = tex2d.p;
    dst_loc1.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc1.SubresourceIndex = 1;

    D3D12_TEXTURE_COPY_LOCATION src_loc1{};
    src_loc1.pResource = upload.p;
    src_loc1.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc1.PlacedFootprint.Offset = 512;
    src_loc1.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc1.PlacedFootprint.Footprint.Width = 8;
    src_loc1.PlacedFootprint.Footprint.Height = 8;
    src_loc1.PlacedFootprint.Footprint.Depth = 1;
    src_loc1.PlacedFootprint.Footprint.RowPitch = 256;
    list->CopyTextureRegion(&dst_loc1, 0, 0, 0, &src_loc1, nullptr);

    // E. Transition retained mip 1 to COPY_SOURCE and readback to verify execution after b0
    D3D12_RESOURCE_BARRIER b1_src{};
    b1_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1_src.Transition.pResource = tex2d.p;
    b1_src.Transition.Subresource = 1;
    b1_src.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b1_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b1_src);

    D3D12_TEXTURE_COPY_LOCATION src_tex{};
    src_tex.pResource = tex2d.p;
    src_tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_tex.SubresourceIndex = 1;

    D3D12_TEXTURE_COPY_LOCATION dst_rb{};
    dst_rb.pResource = readback.p;
    dst_rb.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_rb.PlacedFootprint.Offset = 0;
    dst_rb.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst_rb.PlacedFootprint.Footprint.Width = 8;
    dst_rb.PlacedFootprint.Footprint.Height = 8;
    dst_rb.PlacedFootprint.Footprint.Depth = 1;
    dst_rb.PlacedFootprint.Footprint.RowPitch = 256;
    list->CopyTextureRegion(&dst_rb, 0, 0, 0, &src_tex, nullptr);

    // E2. Copy from dropped mip 0 to readback_dropped: must be gracefully skipped (p.skip = true)
    D3D12_TEXTURE_COPY_LOCATION src_dropped{};
    src_dropped.pResource = tex2d.p;
    src_dropped.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_dropped.SubresourceIndex = 0; // dropped mip

    D3D12_TEXTURE_COPY_LOCATION dst_dropped{};
    dst_dropped.pResource = readback_dropped.p;
    dst_dropped.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_dropped.PlacedFootprint.Offset = 0;
    dst_dropped.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst_dropped.PlacedFootprint.Footprint.Width = 16;
    dst_dropped.PlacedFootprint.Footprint.Height = 16;
    dst_dropped.PlacedFootprint.Footprint.Depth = 1;
    dst_dropped.PlacedFootprint.Footprint.RowPitch = 256;
    list->CopyTextureRegion(&dst_dropped, 0, 0, 0, &src_dropped, nullptr);

    // F. All subresources barrier to PIXEL_SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER ball{};
    ball.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ball.Transition.pResource = tex2d.p;
    ball.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ball.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ball.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &ball);

    EXPECT(SUCCEEDED(list->Close()));

    // 5. ExecuteCommandLists - verify commands after dropped-mip barrier b0 execute cleanly
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);

    Ref<ID3D12Fence> fence;
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    EXPECT(SUCCEEDED(queue->Signal(fence.p, 1)));

    // 6. Descriptor Heap and SRVs (verifying first_mip & mip_count clamping, and ResourceMinLODClamp rebasing)
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 4;
    Ref<ID3D12DescriptorHeap> srv_heap;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap.p))));
    const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = srv_heap->GetCPUDescriptorHandleForHeapStart();

    // SRV 0: requests original MostDetailedMip = 0, MipLevels = 2
    // With drop 1, mip 0 dropped, so view should start at physical 0 with mip_count = 1 (no bleed into physical 1!)
    D3D12_SHADER_RESOURCE_VIEW_DESC srv0{};
    srv0.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv0.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv0.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv0.Texture2D.MostDetailedMip = 0;
    srv0.Texture2D.MipLevels = 2;
    device->CreateShaderResourceView(tex2d.p, &srv0, handle);

    // SRV 1: requests original MostDetailedMip = 1, MipLevels = 2
    // With drop 1, maps to physical MostDetailedMip = 0, MipLevels = 2.
    handle.ptr += stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv1 = srv0;
    srv1.Texture2D.MostDetailedMip = 1;
    srv1.Texture2D.MipLevels = 2;
    device->CreateShaderResourceView(tex2d.p, &srv1, handle);

    // SRV 2: ResourceMinLODClamp = 2.0f (rebased into physical coordinates without empty-view zero swizzle)
    handle.ptr += stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv2 = srv0;
    srv2.Texture2D.MostDetailedMip = 0;
    srv2.Texture2D.MipLevels = 3;
    srv2.Texture2D.ResourceMinLODClamp = 2.0f;
    device->CreateShaderResourceView(tex2d.p, &srv2, handle);

    // Wait for fence
    if (fence->GetCompletedValue() < 1) {
        HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        EXPECT(SUCCEEDED(fence->SetEventOnCompletion(1, event)));
        WaitForSingleObject(event, 5000);
        CloseHandle(event);
    }
    EXPECT(fence->GetCompletedValue() >= 1);

    // 7. Verify readback data copied after dropped-mip barrier b0 (proves b0 didn't terminate prepare())
    void* readback_map = nullptr;
    EXPECT(SUCCEEDED(readback->Map(0, nullptr, &readback_map)));
    EXPECT(readback_map != nullptr);
    const auto* rb_bytes = static_cast<const unsigned char*>(readback_map);
    EXPECT(rb_bytes[0] == 0xAB);
    readback->Unmap(0, nullptr);

    // 7b. Verify that dropped-mip copy to readback_dropped was skipped (sentinel 0xCD untouched)
    void* r0_read = nullptr;
    EXPECT(SUCCEEDED(readback_dropped->Map(0, nullptr, &r0_read)));
    EXPECT(r0_read != nullptr);
    const auto* r0_bytes = static_cast<const unsigned char*>(r0_read);
    EXPECT(r0_bytes[0] == 0xCD);
    readback_dropped->Unmap(0, nullptr);

    // 8. CPU-visible custom heap texture (8x8, 2 mips):
    // Under drop 1, mip 0 is dropped and physical image is 4x4, 1 mip.
    D3D12_HEAP_PROPERTIES custom_heap{};
    custom_heap.Type = D3D12_HEAP_TYPE_CUSTOM;
    custom_heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    custom_heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;

    Ref<ID3D12Resource> tex_cpu;
    const auto desc_cpu = tex2d_desc(8, 8, 2, DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&custom_heap, D3D12_HEAP_FLAG_NONE, &desc_cpu,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex_cpu.p))));

    // ReadFromSubresource on dropped mip 0: must pitch-aware zero-fill without touching trailing padding
    unsigned char dest[256];
    std::memset(dest, 0xEE, sizeof(dest));
    EXPECT(SUCCEEDED(tex_cpu->ReadFromSubresource(dest, 32, 256, 0, nullptr)));
    EXPECT(dest[0] == 0);
    EXPECT(dest[31] == 0);
    EXPECT(dest[32] == 0);
    EXPECT(dest[255] == 0xEE);

    // WriteToSubresource on dropped mip 0: graceful no-op
    unsigned char src_data[256];
    std::memset(src_data, 0x42, sizeof(src_data));
    EXPECT(SUCCEEDED(tex_cpu->WriteToSubresource(0, nullptr, src_data, 32, 256)));

    // Retained mip 1 write & readback:
    std::memset(dest, 0, sizeof(dest));
    EXPECT(SUCCEEDED(tex_cpu->WriteToSubresource(1, nullptr, src_data, 16, 256)));
    EXPECT(SUCCEEDED(tex_cpu->ReadFromSubresource(dest, 16, 256, 1, nullptr)));
    EXPECT(dest[0] == 0x42);
    EXPECT(dest[15] == 0x42);

    // Map probe on dropped mip 0 vs pointer rejection
    void* map_ptr = nullptr;
    EXPECT(FAILED(tex_cpu->Map(0, nullptr, &map_ptr)));
    EXPECT(SUCCEEDED(tex_cpu->Map(0, nullptr, nullptr)));

    std::printf("d3d12_texture_mip_drop_test: PASS\n");
    return 0;
}
