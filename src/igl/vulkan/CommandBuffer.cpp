/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <igl/vulkan/CommandBuffer.h>

#include <igl/vulkan/VulkanBuffer.h>
#include <igl/vulkan/VulkanContext.h>
#include <igl/vulkan/VulkanImage.h>
#include <igl/vulkan/VulkanTexture.h>

namespace lvk::vulkan {

CommandBuffer::CommandBuffer(VulkanContext* ctx) : ctx_(ctx), wrapper_(&ctx_->immediate_->acquire()) {}

CommandBuffer::~CommandBuffer() {
  IGL_ASSERT(!isRendering_); // did you forget to call cmdEndRendering()?
}

namespace {

void transitionColorAttachment(VkCommandBuffer buffer, lvk::vulkan::VulkanTexture* colorTex) {
  if (!IGL_VERIFY(colorTex)) {
    return;
  }

  const VulkanImage& colorImg = *colorTex->image_.get();
  if (!IGL_VERIFY(!colorImg.isDepthFormat_ && !colorImg.isStencilFormat_)) {
    IGL_ASSERT_MSG(false, "Color attachments cannot have depth/stencil formats");
    return;
  }
  IGL_ASSERT_MSG(colorImg.imageFormat_ != VK_FORMAT_UNDEFINED, "Invalid color attachment format");
  colorImg.transitionLayout(buffer,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // wait for all subsequent
                                                                                                          // fragment/compute shaders
                            VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS});
}

VkAttachmentLoadOp loadOpToVkAttachmentLoadOp(lvk::LoadOp a) {
  using lvk::LoadOp;
  switch (a) {
  case LoadOp_DontCare:
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  case LoadOp_Load:
    return VK_ATTACHMENT_LOAD_OP_LOAD;
  case LoadOp_Clear:
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
  case LoadOp_None:
    return VK_ATTACHMENT_LOAD_OP_NONE_EXT;
  }
  IGL_ASSERT(false);
  return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp storeOpToVkAttachmentStoreOp(lvk::StoreOp a) {
  using lvk::StoreOp;
  switch (a) {
  case StoreOp_DontCare:
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
  case StoreOp_Store:
    return VK_ATTACHMENT_STORE_OP_STORE;
  case StoreOp_MsaaResolve:
    // for MSAA resolve, we have to store data into a special "resolve" attachment
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
  case StoreOp_None:
    return VK_ATTACHMENT_STORE_OP_NONE;
  }
  IGL_ASSERT(false);
  return VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

VkStencilOp stencilOpToVkStencilOp(lvk::StencilOp op) {
  switch (op) {
  case lvk::StencilOp_Keep:
    return VK_STENCIL_OP_KEEP;
  case lvk::StencilOp_Zero:
    return VK_STENCIL_OP_ZERO;
  case lvk::StencilOp_Replace:
    return VK_STENCIL_OP_REPLACE;
  case lvk::StencilOp_IncrementClamp:
    return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
  case lvk::StencilOp_DecrementClamp:
    return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
  case lvk::StencilOp_Invert:
    return VK_STENCIL_OP_INVERT;
  case lvk::StencilOp_IncrementWrap:
    return VK_STENCIL_OP_INCREMENT_AND_WRAP;
  case lvk::StencilOp_DecrementWrap:
    return VK_STENCIL_OP_DECREMENT_AND_WRAP;
  }
  IGL_ASSERT(false);
  return VK_STENCIL_OP_KEEP;
}

VkIndexType indexFormatToVkIndexType(lvk::IndexFormat fmt) {
  switch (fmt) {
  case lvk::IndexFormat_UI16:
    return VK_INDEX_TYPE_UINT16;
  case lvk::IndexFormat_UI32:
    return VK_INDEX_TYPE_UINT32;
  };
  IGL_ASSERT(false);
  return VK_INDEX_TYPE_NONE_KHR;
}

VkPrimitiveTopology primitiveTypeToVkPrimitiveTopology(lvk::PrimitiveType t) {
  switch (t) {
  case lvk::Primitive_Point:
    return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  case lvk::Primitive_Line:
    return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  case lvk::Primitive_LineStrip:
    return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  case lvk::Primitive_Triangle:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  case lvk::Primitive_TriangleStrip:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  }
  IGL_ASSERT_MSG(false, "Implement PrimitiveType = %u", (uint32_t)t);
  return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
}

} // namespace

void CommandBuffer ::transitionToShaderReadOnly(TextureHandle handle) const {
  IGL_PROFILER_FUNCTION();

  const lvk::vulkan::VulkanTexture& tex = *ctx_->texturesPool_.get(handle);
  const VulkanImage* img = tex.image_.get();

  IGL_ASSERT(!tex.isSwapchainTexture());

  // transition only non-multisampled images - MSAA images cannot be accessed from shaders
  if (img->samples_ == VK_SAMPLE_COUNT_1_BIT) {
    const VkImageAspectFlags flags = tex.image_->getImageAspectFlags();
    const VkPipelineStageFlags srcStage = lvk::vulkan::isDepthOrStencilVkFormat(tex.image_->imageFormat_)
                                              ? VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                                              : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    // set the result of the previous render pass
    img->transitionLayout(wrapper_->cmdBuf_,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          srcStage,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, // wait for subsequent
                                                                                                        // fragment/compute shaders
                          VkImageSubresourceRange{flags, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS});
  }
}

void CommandBuffer::cmdBindComputePipeline(lvk::ComputePipelineHandle handle) {
  IGL_PROFILER_FUNCTION();

  if (!IGL_VERIFY(!handle.empty())) {
    return;
  }

  VkPipeline* pipeline = ctx_->computePipelinesPool_.get(handle);

  IGL_ASSERT(pipeline);
  IGL_ASSERT(*pipeline != VK_NULL_HANDLE);

  if (lastPipelineBound_ != *pipeline) {
    lastPipelineBound_ = *pipeline;
    if (pipeline != VK_NULL_HANDLE) {
      vkCmdBindPipeline(wrapper_->cmdBuf_, VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
    }
  }
}

void CommandBuffer::cmdDispatchThreadGroups(const Dimensions& threadgroupCount, const Dependencies& deps) {
  IGL_ASSERT(!isRendering_);

  for (uint32_t i = 0; i != Dependencies::IGL_MAX_SUBMIT_DEPENDENCIES && deps.textures[i]; i++) {
    useComputeTexture(deps.textures[i]);
  }

  ctx_->checkAndUpdateDescriptorSets();
  ctx_->bindDefaultDescriptorSets(wrapper_->cmdBuf_, VK_PIPELINE_BIND_POINT_COMPUTE);

  vkCmdDispatch(wrapper_->cmdBuf_, threadgroupCount.width, threadgroupCount.height, threadgroupCount.depth);
}

void CommandBuffer::cmdPushDebugGroupLabel(const char* label, const lvk::Color& color) const {
  IGL_ASSERT(label);

  ivkCmdBeginDebugUtilsLabel(wrapper_->cmdBuf_, label, color.toFloatPtr());
}

void CommandBuffer::cmdInsertDebugEventLabel(const char* label, const lvk::Color& color) const {
  IGL_ASSERT(label);

  ivkCmdInsertDebugUtilsLabel(wrapper_->cmdBuf_, label, color.toFloatPtr());
}

void CommandBuffer::cmdPopDebugGroupLabel() const {
  ivkCmdEndDebugUtilsLabel(wrapper_->cmdBuf_);
}

void CommandBuffer::useComputeTexture(TextureHandle handle) {
  IGL_PROFILER_FUNCTION();

  IGL_ASSERT(!handle.empty());
  lvk::vulkan::VulkanTexture* tex = ctx_->texturesPool_.get(handle);
  const lvk::vulkan::VulkanImage& vkImage = *tex->image_.get();
  if (!vkImage.isStorageImage()) {
    IGL_ASSERT_MSG(false, "Did you forget to specify TextureUsageBits::Storage on your texture?");
    return;
  }

  // "frame graph" heuristics: if we are already in VK_IMAGE_LAYOUT_GENERAL, wait for the previous
  // compute shader
  const VkPipelineStageFlags srcStage = (vkImage.imageLayout_ == VK_IMAGE_LAYOUT_GENERAL) ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                                                                          : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  vkImage.transitionLayout(
      wrapper_->cmdBuf_,
      VK_IMAGE_LAYOUT_GENERAL,
      srcStage,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VkImageSubresourceRange{vkImage.getImageAspectFlags(), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS});
}

void CommandBuffer::cmdBeginRendering(const lvk::RenderPass& renderPass, const lvk::Framebuffer& fb) {
  IGL_PROFILER_FUNCTION();

  IGL_ASSERT(!isRendering_);

  isRendering_ = true;

  const uint32_t numFbColorAttachments = fb.getNumColorAttachments();
  const uint32_t numPassColorAttachments = renderPass.getNumColorAttachments();

  IGL_ASSERT(numPassColorAttachments == numFbColorAttachments);

  framebuffer_ = fb;

  // transition all the color attachments
  for (uint32_t i = 0; i != numFbColorAttachments; i++) {
    if (const auto handle = fb.color[i].texture) {
      lvk::vulkan::VulkanTexture* colorTex = ctx_->texturesPool_.get(handle);
      transitionColorAttachment(wrapper_->cmdBuf_, colorTex);
    }
    // handle MSAA
    if (TextureHandle handle = fb.color[i].resolveTexture) {
      lvk::vulkan::VulkanTexture* colorResolveTex = ctx_->texturesPool_.get(handle);
      transitionColorAttachment(wrapper_->cmdBuf_, colorResolveTex);
    }
  }
  // transition depth-stencil attachment
  TextureHandle depthTex = fb.depthStencil.texture;
  if (depthTex) {
    const lvk::vulkan::VulkanTexture& vkDepthTex = *ctx_->texturesPool_.get(depthTex);
    const lvk::vulkan::VulkanImage* depthImg = vkDepthTex.image_.get();
    IGL_ASSERT_MSG(depthImg->imageFormat_ != VK_FORMAT_UNDEFINED, "Invalid depth attachment format");
    const VkImageAspectFlags flags = vkDepthTex.image_->getImageAspectFlags();
    depthImg->transitionLayout(wrapper_->cmdBuf_,
                               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // wait for all subsequent
                                                                  // operations
                               VkImageSubresourceRange{flags, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS});
  }

  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  uint32_t mipLevel = 0;
  uint32_t fbWidth = 0;
  uint32_t fbHeight = 0;

  // Process depth attachment
  dynamicState_.depthBiasEnable_ = false;

  VkRenderingAttachmentInfo colorAttachments[LVK_MAX_COLOR_ATTACHMENTS];

  for (uint32_t i = 0; i != numFbColorAttachments; i++) {
    const lvk::Framebuffer::AttachmentDesc& attachment = fb.color[i];
    IGL_ASSERT(!attachment.texture.empty());

    const lvk::vulkan::VulkanTexture& colorTexture = *ctx_->texturesPool_.get(attachment.texture);
    const auto& descColor = renderPass.color[i];
    if (mipLevel && descColor.level) {
      IGL_ASSERT_MSG(descColor.level == mipLevel, "All color attachments should have the same mip-level");
    }
    const lvk::Dimensions dim = colorTexture.getDimensions();
    if (fbWidth) {
      IGL_ASSERT_MSG(dim.width == fbWidth, "All attachments should have the save width");
    }
    if (fbHeight) {
      IGL_ASSERT_MSG(dim.height == fbHeight, "All attachments should have the save width");
    }
    mipLevel = descColor.level;
    fbWidth = dim.width;
    fbHeight = dim.height;
    samples = colorTexture.image_->samples_;
    colorAttachments[i] = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = colorTexture.getVkImageViewForFramebuffer(0),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = (samples > 1) ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = loadOpToVkAttachmentLoadOp(descColor.loadOp),
        .storeOp = storeOpToVkAttachmentStoreOp(descColor.storeOp),
        .clearValue = ivkGetClearColorValue(descColor.clearColor.r, descColor.clearColor.g, descColor.clearColor.b, descColor.clearColor.a),
    };
    // handle MSAA
    if (descColor.storeOp == StoreOp_MsaaResolve) {
      IGL_ASSERT(samples > 1);
      IGL_ASSERT_MSG(!attachment.resolveTexture.empty(), "Framebuffer attachment should contain a resolve texture");
      const lvk::vulkan::VulkanTexture& colorResolveTexture = *ctx_->texturesPool_.get(attachment.resolveTexture);
      colorAttachments[i].resolveImageView = colorResolveTexture.getVkImageViewForFramebuffer(0);
      colorAttachments[i].resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
  }

  VkRenderingAttachmentInfo depthAttachment = {};

  if (fb.depthStencil.texture) {
    const auto& depthTexture = *ctx_->texturesPool_.get(fb.depthStencil.texture);
    const auto& descDepth = renderPass.depth;
    IGL_ASSERT_MSG(descDepth.level == mipLevel, "Depth attachment should have the same mip-level as color attachments");
    depthAttachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = depthTexture.getVkImageViewForFramebuffer(0),
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = loadOpToVkAttachmentLoadOp(descDepth.loadOp),
        .storeOp = storeOpToVkAttachmentStoreOp(descDepth.storeOp),
        .clearValue = ivkGetClearDepthStencilValue(descDepth.clearDepth, descDepth.clearStencil),
    };
    const lvk::Dimensions dim = depthTexture.getDimensions();
    if (fbWidth) {
      IGL_ASSERT_MSG(dim.width == fbWidth, "All attachments should have the save width");
    }
    if (fbHeight) {
      IGL_ASSERT_MSG(dim.height == fbHeight, "All attachments should have the save width");
    }
    mipLevel = descDepth.level;
    fbWidth = dim.width;
    fbHeight = dim.height;
  }

  const uint32_t width = std::max(fbWidth >> mipLevel, 1u);
  const uint32_t height = std::max(fbHeight >> mipLevel, 1u);
  const lvk::Viewport viewport = {0.0f, 0.0f, (float)width, (float)height, 0.0f, +1.0f};
  const lvk::ScissorRect scissor = {0, 0, width, height};

  VkRenderingAttachmentInfo stencilAttachment = depthAttachment;

  const bool isStencilFormat = renderPass.stencil.loadOp != lvk::LoadOp_Invalid;

  const VkRenderingInfo renderingInfo = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .pNext = nullptr,
      .flags = 0,
      .renderArea = {VkOffset2D{(int32_t)scissor.x, (int32_t)scissor.y}, VkExtent2D{scissor.width, scissor.height}},
      .layerCount = 1,
      .viewMask = 0,
      .colorAttachmentCount = numFbColorAttachments,
      .pColorAttachments = colorAttachments,
      .pDepthAttachment = depthTex ? &depthAttachment : nullptr,
      .pStencilAttachment = isStencilFormat ? &stencilAttachment : nullptr,
  };

  cmdBindViewport(viewport);
  cmdBindScissorRect(scissor);

  ctx_->checkAndUpdateDescriptorSets();
  ctx_->bindDefaultDescriptorSets(wrapper_->cmdBuf_, VK_PIPELINE_BIND_POINT_GRAPHICS);

  vkCmdBeginRendering(wrapper_->cmdBuf_, &renderingInfo);
}

void CommandBuffer::cmdEndRendering() {
  IGL_ASSERT(isRendering_);

  isRendering_ = false;

  vkCmdEndRendering(wrapper_->cmdBuf_);

  const uint32_t numFbColorAttachments = framebuffer_.getNumColorAttachments();

  // set image layouts after the render pass
  for (uint32_t i = 0; i != numFbColorAttachments; i++) {
    const auto& attachment = framebuffer_.color[i];
    const vulkan::VulkanTexture& tex = *ctx_->texturesPool_.get(attachment.texture);
    // this must match the final layout of the render pass
    tex.image_->imageLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }

  if (framebuffer_.depthStencil.texture) {
    const vulkan::VulkanTexture& tex = *ctx_->texturesPool_.get(framebuffer_.depthStencil.texture);
    // this must match the final layout of the render pass
    tex.image_->imageLayout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  }

  framebuffer_ = {};
}

void CommandBuffer::cmdBindViewport(const Viewport& viewport) {
  // https://www.saschawillems.de/blog/2019/03/29/flipping-the-vulkan-viewport/
  const VkViewport vp = {
      .x = viewport.x, // float x;
      .y = viewport.height - viewport.y, // float y;
      .width = viewport.width, // float width;
      .height = -viewport.height, // float height;
      .minDepth = viewport.minDepth, // float minDepth;
      .maxDepth = viewport.maxDepth, // float maxDepth;
  };
  vkCmdSetViewport(wrapper_->cmdBuf_, 0, 1, &vp);
}

void CommandBuffer::cmdBindScissorRect(const ScissorRect& rect) {
  const VkRect2D scissor = {
      VkOffset2D{(int32_t)rect.x, (int32_t)rect.y},
      VkExtent2D{rect.width, rect.height},
  };
  vkCmdSetScissor(wrapper_->cmdBuf_, 0, 1, &scissor);
}

void CommandBuffer::cmdBindRenderPipeline(lvk::RenderPipelineHandle handle) {
  if (!IGL_VERIFY(!handle.empty())) {
    return;
  }

  currentPipeline_ = handle;

  const lvk::vulkan::RenderPipelineState* rps = ctx_->renderPipelinesPool_.get(handle);

  IGL_ASSERT(rps);

  const RenderPipelineDesc& desc = rps->getRenderPipelineDesc();

  const bool hasDepthAttachmentPipeline = desc.depthFormat != Format_Invalid;
  const bool hasDepthAttachmentPass = !framebuffer_.depthStencil.texture.empty();

  if (hasDepthAttachmentPipeline != hasDepthAttachmentPass) {
    IGL_ASSERT(false);
    LLOGW("Make sure your render pass and render pipeline both have matching depth attachments");
  }

  lastPipelineBound_ = VK_NULL_HANDLE;
}

void CommandBuffer::cmdBindDepthStencilState(const DepthStencilState& desc) {
  IGL_PROFILER_FUNCTION();

  dynamicState_.depthWriteEnable_ = desc.isDepthWriteEnabled;
  dynamicState_.setDepthCompareOp(compareOpToVkCompareOp(desc.compareOp));

  auto setStencilState = [this](VkStencilFaceFlagBits faceMask, const lvk::StencilStateDesc& desc) {
    dynamicState_.setStencilStateOps(faceMask,
                                     stencilOpToVkStencilOp(desc.stencilFailureOp),
                                     stencilOpToVkStencilOp(desc.depthStencilPassOp),
                                     stencilOpToVkStencilOp(desc.depthFailureOp),
                                     compareOpToVkCompareOp(desc.stencilCompareOp));
    vkCmdSetStencilReference(wrapper_->cmdBuf_, faceMask, desc.readMask);
    vkCmdSetStencilCompareMask(wrapper_->cmdBuf_, faceMask, 0xFF);
    vkCmdSetStencilWriteMask(wrapper_->cmdBuf_, faceMask, desc.writeMask);
  };

  setStencilState(VK_STENCIL_FACE_FRONT_BIT, desc.frontFaceStencil);
  setStencilState(VK_STENCIL_FACE_BACK_BIT, desc.backFaceStencil);
}

void CommandBuffer::cmdBindVertexBuffer(uint32_t index, BufferHandle buffer, size_t bufferOffset) {
  IGL_PROFILER_FUNCTION();

  if (!IGL_VERIFY(!buffer.empty())) {
    return;
  }

  lvk::vulkan::VulkanBuffer* buf = ctx_->buffersPool_.get(buffer);

  VkBuffer vkBuf = buf->getVkBuffer();

  IGL_ASSERT(buf->getUsageFlags() & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

  const VkDeviceSize offset = bufferOffset;
  vkCmdBindVertexBuffers(wrapper_->cmdBuf_, index, 1, &vkBuf, &offset);
}

void CommandBuffer::cmdPushConstants(const void* data, size_t size, size_t offset) {
  IGL_PROFILER_FUNCTION();

  IGL_ASSERT(size % 4 == 0); // VUID-vkCmdPushConstants-size-00369: size must be a multiple of 4

  // check push constant size is within max size
  const VkPhysicalDeviceLimits& limits = ctx_->getVkPhysicalDeviceProperties().limits;
  if (!IGL_VERIFY(size + offset <= limits.maxPushConstantsSize)) {
    LLOGW("Push constants size exceeded %u (max %u bytes)", size + offset, limits.maxPushConstantsSize);
  }

  vkCmdPushConstants(wrapper_->cmdBuf_,
                     ctx_->vkPipelineLayout_,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                     (uint32_t)offset,
                     (uint32_t)size,
                     data);
}

void CommandBuffer::bindGraphicsPipeline() {
  const lvk::vulkan::RenderPipelineState* rps = ctx_->renderPipelinesPool_.get(currentPipeline_);

  if (!IGL_VERIFY(rps)) {
    return;
  }

  VkPipeline pipeline = rps->getVkPipeline(dynamicState_);

  if (lastPipelineBound_ != pipeline) {
    lastPipelineBound_ = pipeline;
    if (pipeline != VK_NULL_HANDLE) {
      vkCmdBindPipeline(wrapper_->cmdBuf_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    }
  }
}

void CommandBuffer::cmdDraw(PrimitiveType primitiveType, size_t vertexStart, size_t vertexCount) {
  IGL_PROFILER_FUNCTION();

  if (vertexCount == 0) {
    return;
  }

  dynamicState_.setTopology(primitiveTypeToVkPrimitiveTopology(primitiveType));
  bindGraphicsPipeline();

  vkCmdDraw(wrapper_->cmdBuf_, (uint32_t)vertexCount, 1, (uint32_t)vertexStart, 0);
}

void CommandBuffer::cmdDrawIndexed(PrimitiveType primitiveType,
                                   size_t indexCount,
                                   IndexFormat indexFormat,
                                   BufferHandle indexBuffer,
                                   size_t indexBufferOffset) {
  IGL_PROFILER_FUNCTION();

  if (indexCount == 0) {
    return;
  }

  dynamicState_.setTopology(primitiveTypeToVkPrimitiveTopology(primitiveType));
  bindGraphicsPipeline();

  lvk::vulkan::VulkanBuffer* buf = ctx_->buffersPool_.get(indexBuffer);

  const VkIndexType type = indexFormatToVkIndexType(indexFormat);
  vkCmdBindIndexBuffer(wrapper_->cmdBuf_, buf->getVkBuffer(), indexBufferOffset, type);

  vkCmdDrawIndexed(wrapper_->cmdBuf_, (uint32_t)indexCount, 1, 0, 0, 0);
}

void CommandBuffer::cmdDrawIndirect(PrimitiveType primitiveType,
                                    BufferHandle indirectBuffer,
                                    size_t indirectBufferOffset,
                                    uint32_t drawCount,
                                    uint32_t stride) {
  IGL_PROFILER_FUNCTION();

  dynamicState_.setTopology(primitiveTypeToVkPrimitiveTopology(primitiveType));
  bindGraphicsPipeline();

  lvk::vulkan::VulkanBuffer* bufIndirect = ctx_->buffersPool_.get(indirectBuffer);

  vkCmdDrawIndirect(
      wrapper_->cmdBuf_, bufIndirect->getVkBuffer(), indirectBufferOffset, drawCount, stride ? stride : sizeof(VkDrawIndirectCommand));
}

void CommandBuffer::cmdDrawIndexedIndirect(PrimitiveType primitiveType,
                                           IndexFormat indexFormat,
                                           BufferHandle indexBuffer,
                                           BufferHandle indirectBuffer,
                                           size_t indirectBufferOffset,
                                           uint32_t drawCount,
                                           uint32_t stride) {
  IGL_PROFILER_FUNCTION();

  dynamicState_.setTopology(primitiveTypeToVkPrimitiveTopology(primitiveType));
  bindGraphicsPipeline();

  lvk::vulkan::VulkanBuffer* bufIndex = ctx_->buffersPool_.get(indexBuffer);
  lvk::vulkan::VulkanBuffer* bufIndirect = ctx_->buffersPool_.get(indirectBuffer);

  const VkIndexType type = indexFormatToVkIndexType(indexFormat);
  vkCmdBindIndexBuffer(wrapper_->cmdBuf_, bufIndex->getVkBuffer(), 0, type);

  vkCmdDrawIndexedIndirect(wrapper_->cmdBuf_,
                           bufIndirect->getVkBuffer(),
                           indirectBufferOffset,
                           drawCount,
                           stride ? stride : sizeof(VkDrawIndexedIndirectCommand));
}

void CommandBuffer::cmdSetStencilReferenceValues(uint32_t frontValue, uint32_t backValue) {
  vkCmdSetStencilReference(wrapper_->cmdBuf_, VK_STENCIL_FACE_FRONT_BIT, frontValue);
  vkCmdSetStencilReference(wrapper_->cmdBuf_, VK_STENCIL_FACE_BACK_BIT, backValue);
}

void CommandBuffer::cmdSetBlendColor(Color color) {
  vkCmdSetBlendConstants(wrapper_->cmdBuf_, color.toFloatPtr());
}

void CommandBuffer::cmdSetDepthBias(float depthBias, float slopeScale, float clamp) {
  dynamicState_.depthBiasEnable_ = true;
  vkCmdSetDepthBias(wrapper_->cmdBuf_, depthBias, clamp, slopeScale);
}

} // namespace lvk::vulkan
