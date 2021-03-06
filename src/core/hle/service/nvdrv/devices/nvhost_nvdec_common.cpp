// Copyright 2020 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/service/nvdrv/devices/nvhost_nvdec_common.h"
#include "core/hle/service/nvdrv/devices/nvmap.h"
#include "core/memory.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_base.h"

namespace Service::Nvidia::Devices {

namespace {
// Splice vectors will copy count amount of type T from the input vector into the dst vector.
template <typename T>
std::size_t SpliceVectors(const std::vector<u8>& input, std::vector<T>& dst, std::size_t count,
                          std::size_t offset) {
    std::memcpy(dst.data(), input.data() + offset, count * sizeof(T));
    offset += count * sizeof(T);
    return offset;
}

// Write vectors will write data to the output buffer
template <typename T>
std::size_t WriteVectors(std::vector<u8>& dst, const std::vector<T>& src, std::size_t offset) {
    std::memcpy(dst.data() + offset, src.data(), src.size() * sizeof(T));
    offset += src.size() * sizeof(T);
    return offset;
}
} // Anonymous namespace

namespace NvErrCodes {
constexpr u32 Success{};
constexpr u32 OutOfMemory{static_cast<u32>(-12)};
constexpr u32 InvalidInput{static_cast<u32>(-22)};
} // namespace NvErrCodes

nvhost_nvdec_common::nvhost_nvdec_common(Core::System& system, std::shared_ptr<nvmap> nvmap_dev)
    : nvdevice(system), nvmap_dev(std::move(nvmap_dev)) {}
nvhost_nvdec_common::~nvhost_nvdec_common() = default;

u32 nvhost_nvdec_common::SetNVMAPfd(const std::vector<u8>& input) {
    IoctlSetNvmapFD params{};
    std::memcpy(&params, input.data(), sizeof(IoctlSetNvmapFD));
    LOG_DEBUG(Service_NVDRV, "called, fd={}", params.nvmap_fd);

    nvmap_fd = params.nvmap_fd;
    return 0;
}

u32 nvhost_nvdec_common::Submit(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlSubmit params{};
    std::memcpy(&params, input.data(), sizeof(IoctlSubmit));
    LOG_DEBUG(Service_NVDRV, "called NVDEC Submit, cmd_buffer_count={}", params.cmd_buffer_count);

    // Instantiate param buffers
    std::size_t offset = sizeof(IoctlSubmit);
    std::vector<CommandBuffer> command_buffers(params.cmd_buffer_count);
    std::vector<Reloc> relocs(params.relocation_count);
    std::vector<u32> reloc_shifts(params.relocation_count);
    std::vector<SyncptIncr> syncpt_increments(params.syncpoint_count);
    std::vector<SyncptIncr> wait_checks(params.syncpoint_count);
    std::vector<Fence> fences(params.fence_count);

    // Splice input into their respective buffers
    offset = SpliceVectors(input, command_buffers, params.cmd_buffer_count, offset);
    offset = SpliceVectors(input, relocs, params.relocation_count, offset);
    offset = SpliceVectors(input, reloc_shifts, params.relocation_count, offset);
    offset = SpliceVectors(input, syncpt_increments, params.syncpoint_count, offset);
    offset = SpliceVectors(input, wait_checks, params.syncpoint_count, offset);
    offset = SpliceVectors(input, fences, params.fence_count, offset);

    // TODO(ameerj): For async gpu, utilize fences for syncpoint 'max' increment

    auto& gpu = system.GPU();

    for (const auto& cmd_buffer : command_buffers) {
        auto object = nvmap_dev->GetObject(cmd_buffer.memory_id);
        ASSERT_OR_EXECUTE(object, return NvErrCodes::InvalidInput;);
        const auto map = FindBufferMap(object->dma_map_addr);
        if (!map) {
            LOG_ERROR(Service_NVDRV, "Tried to submit an invalid offset 0x{:X} dma 0x{:X}",
                      object->addr, object->dma_map_addr);
            return 0;
        }
        Tegra::ChCommandHeaderList cmdlist(cmd_buffer.word_count);
        gpu.MemoryManager().ReadBlock(map->StartAddr() + cmd_buffer.offset, cmdlist.data(),
                                      cmdlist.size() * sizeof(u32));
        gpu.PushCommandBuffer(cmdlist);
    }

    std::memcpy(output.data(), &params, sizeof(IoctlSubmit));
    // Some games expect command_buffers to be written back
    offset = sizeof(IoctlSubmit);
    offset = WriteVectors(output, command_buffers, offset);
    offset = WriteVectors(output, relocs, offset);
    offset = WriteVectors(output, reloc_shifts, offset);
    offset = WriteVectors(output, syncpt_increments, offset);
    offset = WriteVectors(output, wait_checks, offset);

    return NvErrCodes::Success;
}

u32 nvhost_nvdec_common::GetSyncpoint(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlGetSyncpoint params{};
    std::memcpy(&params, input.data(), sizeof(IoctlGetSyncpoint));
    LOG_DEBUG(Service_NVDRV, "called GetSyncpoint, id={}", params.param);

    // We found that implementing this causes deadlocks with async gpu, along with degraded
    // performance. TODO: RE the nvdec async implementation
    params.value = 0;
    std::memcpy(output.data(), &params, sizeof(IoctlGetSyncpoint));

    return NvErrCodes::Success;
}

u32 nvhost_nvdec_common::GetWaitbase(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlGetWaitbase params{};
    std::memcpy(&params, input.data(), sizeof(IoctlGetWaitbase));
    params.value = 0; // Seems to be hard coded at 0
    std::memcpy(output.data(), &params, sizeof(IoctlGetWaitbase));
    return 0;
}

u32 nvhost_nvdec_common::MapBuffer(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlMapBuffer params{};
    std::memcpy(&params, input.data(), sizeof(IoctlMapBuffer));
    std::vector<MapBufferEntry> cmd_buffer_handles(params.num_entries);

    SpliceVectors(input, cmd_buffer_handles, params.num_entries, sizeof(IoctlMapBuffer));

    auto& gpu = system.GPU();

    for (auto& cmf_buff : cmd_buffer_handles) {
        auto object{nvmap_dev->GetObject(cmf_buff.map_handle)};
        if (!object) {
            LOG_ERROR(Service_NVDRV, "invalid cmd_buffer nvmap_handle={:X}", cmf_buff.map_handle);
            std::memcpy(output.data(), &params, output.size());
            return NvErrCodes::InvalidInput;
        }
        if (object->dma_map_addr == 0) {
            // NVDEC and VIC memory is in the 32-bit address space
            // MapAllocate32 will attempt to map a lower 32-bit value in the shared gpu memory space
            const GPUVAddr low_addr = gpu.MemoryManager().MapAllocate32(object->addr, object->size);
            object->dma_map_addr = static_cast<u32>(low_addr);
            // Ensure that the dma_map_addr is indeed in the lower 32-bit address space.
            ASSERT(object->dma_map_addr == low_addr);
        }
        if (!object->dma_map_addr) {
            LOG_ERROR(Service_NVDRV, "failed to map size={}", object->size);
        } else {
            cmf_buff.map_address = object->dma_map_addr;
            AddBufferMap(object->dma_map_addr, object->size, object->addr,
                         object->status == nvmap::Object::Status::Allocated);
        }
    }
    std::memcpy(output.data(), &params, sizeof(IoctlMapBuffer));
    std::memcpy(output.data() + sizeof(IoctlMapBuffer), cmd_buffer_handles.data(),
                cmd_buffer_handles.size() * sizeof(MapBufferEntry));

    return NvErrCodes::Success;
}

u32 nvhost_nvdec_common::UnmapBuffer(const std::vector<u8>& input, std::vector<u8>& output) {
    IoctlMapBuffer params{};
    std::memcpy(&params, input.data(), sizeof(IoctlMapBuffer));
    std::vector<MapBufferEntry> cmd_buffer_handles(params.num_entries);
    SpliceVectors(input, cmd_buffer_handles, params.num_entries, sizeof(IoctlMapBuffer));

    auto& gpu = system.GPU();

    for (auto& cmf_buff : cmd_buffer_handles) {
        const auto object{nvmap_dev->GetObject(cmf_buff.map_handle)};
        if (!object) {
            LOG_ERROR(Service_NVDRV, "invalid cmd_buffer nvmap_handle={:X}", cmf_buff.map_handle);
            std::memcpy(output.data(), &params, output.size());
            return NvErrCodes::InvalidInput;
        }
        if (const auto size{RemoveBufferMap(object->dma_map_addr)}; size) {
            gpu.MemoryManager().Unmap(object->dma_map_addr, *size);
        } else {
            // This occurs quite frequently, however does not seem to impact functionality
            LOG_DEBUG(Service_NVDRV, "invalid offset=0x{:X} dma=0x{:X}", object->addr,
                      object->dma_map_addr);
        }
        object->dma_map_addr = 0;
    }
    std::memset(output.data(), 0, output.size());
    return NvErrCodes::Success;
}

u32 nvhost_nvdec_common::SetSubmitTimeout(const std::vector<u8>& input, std::vector<u8>& output) {
    std::memcpy(&submit_timeout, input.data(), input.size());
    LOG_WARNING(Service_NVDRV, "(STUBBED) called");
    return NvErrCodes::Success;
}

std::optional<nvhost_nvdec_common::BufferMap> nvhost_nvdec_common::FindBufferMap(
    GPUVAddr gpu_addr) const {
    const auto it = std::find_if(
        buffer_mappings.begin(), buffer_mappings.upper_bound(gpu_addr), [&](const auto& entry) {
            return (gpu_addr >= entry.second.StartAddr() && gpu_addr < entry.second.EndAddr());
        });

    ASSERT(it != buffer_mappings.end());
    return it->second;
}

void nvhost_nvdec_common::AddBufferMap(GPUVAddr gpu_addr, std::size_t size, VAddr cpu_addr,
                                       bool is_allocated) {
    buffer_mappings.insert_or_assign(gpu_addr, BufferMap{gpu_addr, size, cpu_addr, is_allocated});
}

std::optional<std::size_t> nvhost_nvdec_common::RemoveBufferMap(GPUVAddr gpu_addr) {
    const auto iter{buffer_mappings.find(gpu_addr)};
    if (iter == buffer_mappings.end()) {
        return std::nullopt;
    }
    std::size_t size = 0;
    if (iter->second.IsAllocated()) {
        size = iter->second.Size();
    }
    buffer_mappings.erase(iter);
    return size;
}

} // namespace Service::Nvidia::Devices
