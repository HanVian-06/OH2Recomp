#pragma once

#include "vk_foundation/vk_types.h"

#include <cstdint>
#include <deque>

namespace oh2::vk {

struct StagingAllocation {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
    uint64_t end_cursor = 0;
};

class StagingRingBuffer {
public:
    static constexpr uint64_t kMinCapacityBytes = 64ull * 1024ull * 1024ull;
    static constexpr uint64_t kMaxCapacityBytes = 128ull * 1024ull * 1024ull;

    StagingRingBuffer() = default;
    ~StagingRingBuffer();

    StagingRingBuffer(const StagingRingBuffer&) = delete;
    StagingRingBuffer& operator=(const StagingRingBuffer&) = delete;

    bool init(VmaAllocator allocator,
              VkDevice device,
              const VkCapabilities& caps,
              uint64_t requested_bytes = kMinCapacityBytes);
    void destroy();

    bool allocate(VkDeviceSize size, VkDeviceSize align, StagingAllocation& out_alloc);
    void flush(VkDeviceSize offset, VkDeviceSize size) const;
    void track_submission(uint64_t end_cursor, VkFence submitted_fence);
    void notify_fence_completed(VkFence fence);
    void retire_all_wait();

    uint64_t capacity_bytes() const { return capacity_; }
    uint64_t wrap_count() const { return wrap_count_; }
    uint64_t reclaim_count() const { return reclaim_count_; }
    uint64_t total_allocated_bytes() const { return total_allocated_bytes_; }
    bool is_host_coherent() const { return host_coherent_; }

private:
    struct PendingSpan {
        uint64_t end_cursor = 0;
        VkFence fence = VK_NULL_HANDLE;
    };

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    uint8_t* mapped_base_ = nullptr;

    uint64_t capacity_ = 0;
    uint64_t write_cursor_ = 0;
    uint64_t retired_cursor_ = 0;
    uint64_t wrap_count_ = 0;
    uint64_t reclaim_count_ = 0;
    uint64_t total_allocated_bytes_ = 0;

    VkDeviceSize copy_offset_align_ = 16;
    VkDeviceSize non_coherent_atom_size_ = 128;
    bool host_coherent_ = true;

    std::deque<PendingSpan> pending_;
};

}  // namespace oh2::vk
