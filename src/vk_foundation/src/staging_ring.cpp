#include "vk_foundation/staging_ring.h"
#include "vk_foundation/vk_log.h"

#include <algorithm>
#include <numeric>

namespace oh2::vk {

namespace {

inline uint64_t align_up(uint64_t val, uint64_t align) {
    if (align <= 1) return val;
    return ((val + align - 1) / align) * align;
}

}  // namespace

StagingRingBuffer::~StagingRingBuffer() {
    destroy();
}

bool StagingRingBuffer::init(VmaAllocator allocator,
                             VkDevice device,
                             const VkCapabilities& caps,
                             uint64_t requested_bytes) {
    destroy();
    if (!allocator || !device) {
        M3_LOGE("StagingRingBuffer::init invalid allocator/device");
        return false;
    }

    allocator_ = allocator;
    device_ = device;
    capacity_ = std::clamp(requested_bytes, kMinCapacityBytes, kMaxCapacityBytes);
    copy_offset_align_ = std::max<VkDeviceSize>(16, caps.optimal_buffer_copy_offset_alignment);
    non_coherent_atom_size_ = std::max<VkDeviceSize>(1, caps.non_coherent_atom_size);

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = capacity_;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_info.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo vma_info{};
    VkResult res = vmaCreateBuffer(allocator_, &buf_info, &alloc_info,
                                   &buffer_, &allocation_, &vma_info);
    if (res != VK_SUCCESS || !vma_info.pMappedData) {
        M3_LOGE("vmaCreateBuffer staging ring (%llu B) failed: %d",
                static_cast<unsigned long long>(capacity_), static_cast<int>(res));
        destroy();
        return false;
    }

    mapped_base_ = static_cast<uint8_t*>(vma_info.pMappedData);

    VkMemoryPropertyFlags mem_flags = 0;
    vmaGetMemoryTypeProperties(allocator_, vma_info.memoryType, &mem_flags);
    host_coherent_ = (mem_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    write_cursor_ = 0;
    retired_cursor_ = 0;
    wrap_count_ = 0;
    reclaim_count_ = 0;
    total_allocated_bytes_ = 0;
    pending_.clear();

    M3_LOGI("StagingRingBuffer initialized: capacity=%llu MiB coherent=%d copyAlign=%llu",
            static_cast<unsigned long long>(capacity_ / (1024ull * 1024ull)),
            host_coherent_ ? 1 : 0,
            static_cast<unsigned long long>(copy_offset_align_));
    return true;
}

void StagingRingBuffer::destroy() {
    pending_.clear();
    if (buffer_ != VK_NULL_HANDLE && allocation_ != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, buffer_, allocation_);
    }
    buffer_ = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
    mapped_base_ = nullptr;
    allocator_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    capacity_ = 0;
    write_cursor_ = 0;
    retired_cursor_ = 0;
}

bool StagingRingBuffer::allocate(VkDeviceSize size, VkDeviceSize align, StagingAllocation& out_alloc) {
    if (size == 0 || size > capacity_ || !buffer_ || !mapped_base_) {
        M3_LOGE("StagingRingBuffer::allocate invalid size=%llu capacity=%llu",
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(capacity_));
        return false;
    }

    const uint64_t eff_align = std::lcm<uint64_t>(copy_offset_align_, std::max<uint64_t>(align, 16));
    uint64_t aligned = align_up(write_cursor_, eff_align);

    // Ensure contiguous slice within [0, capacity_).
    const uint64_t start_bucket = aligned / capacity_;
    const uint64_t end_bucket = (aligned + size - 1) / capacity_;
    if (start_bucket != end_bucket) {
        aligned = (start_bucket + 1) * capacity_;
        wrap_count_++;
    }

    // Wait on oldest pending GPU spans until enough ring capacity is free.
    while ((aligned + size) - retired_cursor_ > capacity_) {
        if (pending_.empty()) {
            M3_LOGE("StagingRingBuffer out of space with no pending fences (size=%llu)",
                    static_cast<unsigned long long>(size));
            return false;
        }
        PendingSpan oldest = pending_.front();
        pending_.pop_front();
        if (oldest.fence != VK_NULL_HANDLE) {
            VkResult wr = vkWaitForFences(device_, 1, &oldest.fence, VK_TRUE, UINT64_MAX);
            if (wr != VK_SUCCESS) {
                M3_LOGE("vkWaitForFences inside StagingRingBuffer failed: %d", static_cast<int>(wr));
                return false;
            }
        }
        if (oldest.end_cursor > retired_cursor_) {
            retired_cursor_ = oldest.end_cursor;
        }
        reclaim_count_++;
    }

    const uint64_t offset = aligned % capacity_;
    write_cursor_ = aligned + size;
    total_allocated_bytes_ += size;

    out_alloc.buffer = buffer_;
    out_alloc.offset = offset;
    out_alloc.size = size;
    out_alloc.mapped = mapped_base_ + offset;
    out_alloc.end_cursor = write_cursor_;
    return true;
}

void StagingRingBuffer::flush(VkDeviceSize offset, VkDeviceSize size) const {
    if (host_coherent_ || !allocator_ || !allocation_ || size == 0) {
        return;
    }
    const VkDeviceSize atom = non_coherent_atom_size_;
    const VkDeviceSize aligned_start = (offset / atom) * atom;
    const VkDeviceSize raw_end = std::min<VkDeviceSize>(capacity_, offset + size);
    const VkDeviceSize aligned_end = std::min<VkDeviceSize>(
        capacity_, align_up(raw_end, atom));
    vmaFlushAllocation(allocator_, allocation_, aligned_start, aligned_end - aligned_start);
}

void StagingRingBuffer::track_submission(uint64_t end_cursor, VkFence submitted_fence) {
    pending_.push_back(PendingSpan{end_cursor, submitted_fence});
}

void StagingRingBuffer::notify_fence_completed(VkFence fence) {
    if (fence == VK_NULL_HANDLE) return;
    while (!pending_.empty()) {
        const auto& front = pending_.front();
        if (front.fence != fence) {
            // Look for matching spans; retire any earlier spans only if fence matched in order.
            break;
        }
        if (front.end_cursor > retired_cursor_) {
            retired_cursor_ = front.end_cursor;
        }
        pending_.pop_front();
        reclaim_count_++;
    }

    // Also remove any non-front entries referencing this fence before vkResetFences is called.
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->fence == fence) {
            if (it->end_cursor > retired_cursor_) {
                retired_cursor_ = it->end_cursor;
            }
            it = pending_.erase(it);
            reclaim_count_++;
        } else {
            ++it;
        }
    }
}

void StagingRingBuffer::retire_all_wait() {
    while (!pending_.empty()) {
        PendingSpan span = pending_.front();
        pending_.pop_front();
        if (span.fence != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
            vkWaitForFences(device_, 1, &span.fence, VK_TRUE, UINT64_MAX);
        }
        if (span.end_cursor > retired_cursor_) {
            retired_cursor_ = span.end_cursor;
        }
        reclaim_count_++;
    }
}

}  // namespace oh2::vk
