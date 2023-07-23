/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <igl/vulkan/Common.h>
#include <igl/vulkan/VulkanHelpers.h>
#include <vector>

namespace lvk::vulkan {

class VulkanPipelineBuilder final {
 public:
  VulkanPipelineBuilder();
  ~VulkanPipelineBuilder() = default;

  VulkanPipelineBuilder& depthBiasEnable(bool enable);
  VulkanPipelineBuilder& depthWriteEnable(bool enable);
  VulkanPipelineBuilder& depthCompareOp(VkCompareOp compareOp);
  VulkanPipelineBuilder& dynamicState(VkDynamicState state);
  VulkanPipelineBuilder& dynamicStates(const std::vector<VkDynamicState>& states);
  VulkanPipelineBuilder& primitiveTopology(VkPrimitiveTopology topology);
  VulkanPipelineBuilder& rasterizationSamples(VkSampleCountFlagBits samples);
  VulkanPipelineBuilder& shaderStage(VkPipelineShaderStageCreateInfo stage);
  VulkanPipelineBuilder& shaderStages(const std::vector<VkPipelineShaderStageCreateInfo>& stages);
  VulkanPipelineBuilder& stencilStateOps(VkStencilFaceFlags faceMask,
                                         VkStencilOp failOp,
                                         VkStencilOp passOp,
                                         VkStencilOp depthFailOp,
                                         VkCompareOp compareOp);
  VulkanPipelineBuilder& cullMode(VkCullModeFlags mode);
  VulkanPipelineBuilder& frontFace(VkFrontFace mode);
  VulkanPipelineBuilder& polygonMode(VkPolygonMode mode);
  VulkanPipelineBuilder& vertexInputState(const VkPipelineVertexInputStateCreateInfo& state);
  VulkanPipelineBuilder& colorBlendAttachmentStates(
      lvk::Span<VkPipelineColorBlendAttachmentState> states);
  VulkanPipelineBuilder& colorAttachmentFormats(lvk::Span<VkFormat> formats);
  VulkanPipelineBuilder& depthAttachmentFormat(VkFormat format);
  VulkanPipelineBuilder& stencilAttachmentFormat(VkFormat format);

  VkResult build(VkDevice device,
                 VkPipelineCache pipelineCache,
                 VkPipelineLayout pipelineLayout,
                 VkPipeline* outPipeline,
                 const char* debugName = nullptr) noexcept;

  static uint32_t getNumPipelinesCreated() {
    return numPipelinesCreated_;
  }

 private:
  std::vector<VkDynamicState> dynamicStates_;
  std::vector<VkPipelineShaderStageCreateInfo> shaderStages_;
  
  VkPipelineVertexInputStateCreateInfo vertexInputState_;
  VkPipelineInputAssemblyStateCreateInfo inputAssembly_;
  VkPipelineRasterizationStateCreateInfo rasterizationState_;
  VkPipelineMultisampleStateCreateInfo multisampleState_;
  VkPipelineDepthStencilStateCreateInfo depthStencilState_;
  VkPipelineColorBlendAttachmentState colorBlendAttachmentStates_[IGL_COLOR_ATTACHMENTS_MAX] = {};
  VkFormat colorAttachmentFormats_[IGL_COLOR_ATTACHMENTS_MAX] = {};
  uint32_t numColorAttachments_ = 0;
  VkFormat depthAttachmentFormat_ = VK_FORMAT_UNDEFINED;
  VkFormat stencilAttachmentFormat_ = VK_FORMAT_UNDEFINED;
  static uint32_t numPipelinesCreated_;
};

} // namespace lvk::vulkan
