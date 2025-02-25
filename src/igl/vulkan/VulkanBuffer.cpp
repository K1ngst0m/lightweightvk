/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <igl/vulkan/Common.h>
#include <igl/vulkan/VulkanContext.h>

#include "VulkanBuffer.h"

#include <string.h>

namespace lvk::vulkan {

VulkanBuffer::VulkanBuffer(VulkanContext* ctx,
                           VkDevice device,
                           VkDeviceSize bufferSize,
                           VkBufferUsageFlags usageFlags,
                           VkMemoryPropertyFlags memFlags,
                           const char* debugName) :
  ctx_(ctx), device_(device), bufferSize_(bufferSize), usageFlags_(usageFlags), memFlags_(memFlags) {
  IGL_PROFILER_FUNCTION_COLOR(IGL_PROFILER_COLOR_CREATE);

  IGL_ASSERT(ctx);
  IGL_ASSERT(bufferSize > 0);

  // Initialize Buffer Info
  const VkBufferCreateInfo ci = ivkGetBufferCreateInfo(bufferSize, usageFlags);

  if (IGL_VULKAN_USE_VMA) {
    // Initialize VmaAllocation Info
    if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      vmaAllocInfo_.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      vmaAllocInfo_.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      vmaAllocInfo_.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    }
    if (memFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
      vmaAllocInfo_.requiredFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    vmaAllocInfo_.usage = VMA_MEMORY_USAGE_AUTO;

    vmaCreateBuffer((VmaAllocator)ctx_->getVmaAllocator(), &ci, &vmaAllocInfo_, &vkBuffer_, &vmaAllocation_, nullptr);

    // handle memory-mapped buffers
    if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      vmaMapMemory((VmaAllocator)ctx_->getVmaAllocator(), vmaAllocation_, &mappedPtr_);
    }
  } else {
    // create buffer
    VK_ASSERT(vkCreateBuffer(device_, &ci, nullptr, &vkBuffer_));

    // back the buffer with some memory
    {
      VkMemoryRequirements requirements = {};
      vkGetBufferMemoryRequirements(device_, vkBuffer_, &requirements);

      VK_ASSERT(ivkAllocateMemory(ctx_->getVkPhysicalDevice(), device_, &requirements, memFlags, &vkMemory_));
      VK_ASSERT(vkBindBufferMemory(device_, vkBuffer_, vkMemory_, 0));
    }

    // handle memory-mapped buffers
    if (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      VK_ASSERT(vkMapMemory(device_, vkMemory_, 0, bufferSize_, 0, &mappedPtr_));
    }
  }

  IGL_ASSERT(vkBuffer_ != VK_NULL_HANDLE);

  // set debug name
  VK_ASSERT(ivkSetDebugObjectName(device_, VK_OBJECT_TYPE_BUFFER, (uint64_t)vkBuffer_, debugName));

  // handle shader access
  if (usageFlags & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    const VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, vkBuffer_};
    vkDeviceAddress_ = vkGetBufferDeviceAddress(device_, &ai);
    IGL_ASSERT(vkDeviceAddress_);
  }
}

VulkanBuffer::~VulkanBuffer() {
  IGL_PROFILER_FUNCTION_COLOR(IGL_PROFILER_COLOR_DESTROY);

  if (!ctx_) {
    return;
  }

  if (IGL_VULKAN_USE_VMA) {
    if (mappedPtr_) {
      vmaUnmapMemory((VmaAllocator)ctx_->getVmaAllocator(), vmaAllocation_);
    }
    ctx_->deferredTask(std::packaged_task<void()>([vma = ctx_->getVmaAllocator(), buffer = vkBuffer_, allocation = vmaAllocation_]() {
      vmaDestroyBuffer((VmaAllocator)vma, buffer, allocation);
    }));
  } else {
    if (mappedPtr_) {
      vkUnmapMemory(device_, vkMemory_);
    }
    ctx_->deferredTask(std::packaged_task<void()>([device = device_, buffer = vkBuffer_, memory = vkMemory_]() {
      vkDestroyBuffer(device, buffer, nullptr);
      vkFreeMemory(device, memory, nullptr);
    }));
  }
}

VulkanBuffer::VulkanBuffer(VulkanBuffer&& other) :
  ctx_(other.ctx_),
  device_(other.device_),
  vkBuffer_(other.vkBuffer_),
  vkMemory_(other.vkMemory_),
  vmaAllocInfo_(other.vmaAllocInfo_),
  vmaAllocation_(other.vmaAllocation_),
  vkDeviceAddress_(other.vkDeviceAddress_),
  bufferSize_(other.bufferSize_),
  usageFlags_(other.usageFlags_),
  memFlags_(other.memFlags_),
  mappedPtr_(other.mappedPtr_) {
  other.ctx_ = nullptr;
}

VulkanBuffer& VulkanBuffer::operator=(VulkanBuffer&& other) {
  std::swap(ctx_, other.ctx_);
  std::swap(device_, other.device_);
  std::swap(vkBuffer_, other.vkBuffer_);
  std::swap(vkMemory_, other.vkMemory_);
  std::swap(vmaAllocInfo_, other.vmaAllocInfo_);
  std::swap(vmaAllocation_, other.vmaAllocation_);
  std::swap(vkDeviceAddress_, other.vkDeviceAddress_);
  std::swap(bufferSize_, other.bufferSize_);
  std::swap(usageFlags_, other.usageFlags_);
  std::swap(memFlags_, other.memFlags_);
  std::swap(mappedPtr_, other.mappedPtr_);
  return *this;
}

void VulkanBuffer::flushMappedMemory(VkDeviceSize offset, VkDeviceSize size) const {
  if (!IGL_VERIFY(isMapped())) {
    return;
  }

  if (IGL_VULKAN_USE_VMA) {
    vmaFlushAllocation((VmaAllocator)ctx_->getVmaAllocator(), vmaAllocation_, offset, size);
  } else {
    const VkMappedMemoryRange memoryRange = {
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        nullptr,
        vkMemory_,
        offset,
        size,
    };
    vkFlushMappedMemoryRanges(device_, 1, &memoryRange);
  }
}

void VulkanBuffer::getBufferSubData(size_t offset, size_t size, void* data) {
  // Only mapped host-visible buffers can be downloaded this way. All other
  // GPU buffers should use a temporary staging buffer

  IGL_ASSERT(mappedPtr_);

  if (!mappedPtr_) {
    return;
  }

  IGL_ASSERT(offset + size <= bufferSize_);

  const uint8_t* src = static_cast<uint8_t*>(mappedPtr_) + offset;
  memcpy(data, src, size);
}

void VulkanBuffer::bufferSubData(size_t offset, size_t size, const void* data) {
  // Only mapped host-visible buffers can be uploaded this way. All other GPU buffers should use a temporary staging buffer

  IGL_ASSERT(mappedPtr_);

  if (!mappedPtr_) {
    return;
  }

  IGL_ASSERT(offset + size <= bufferSize_);

  if (data) {
    memcpy((uint8_t*)mappedPtr_ + offset, data, size);
  } else {
    memset((uint8_t*)mappedPtr_ + offset, 0, size);
  }
}

} // namespace lvk::vulkan
