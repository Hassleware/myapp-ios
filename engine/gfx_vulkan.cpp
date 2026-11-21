// The renderer's Vulkan set-up: instance, device, swapchain, render passes, buffers, images, shaders and
// pipelines, and init / shutdown. Touched only when Vulkan or MoltenVK misbehaves.
#include "gfx_internal.h"

namespace gfx {
namespace internal {

bool fail(std::string& error, const char* what) {
    error = what;
    SDL_Log("gfx: %s", what);
    return false;
}

uint32_t find_memory(uint32_t typeBits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    return 0;
}

bool create_buffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size ? size : 4;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, out.buffer, out.memory, 0);
    return vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) == VK_SUCCESS;
}

void destroy_buffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = Buffer();
}

bool create_image(VkImage& image, VkDeviceMemory& memory, int w, int h, uint32_t mips, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits count) {
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {(uint32_t)w, (uint32_t)h, 1};
    info.mipLevels = mips;
    info.arrayLayers = 1;
    info.samples = count;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, image, memory, 0);
    return true;
}

VkImageView create_view(VkImage image, VkFormat format, VkImageAspectFlags aspectBits, uint32_t mips) {
    VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.image = image;
    info.format = format;
    info.subresourceRange.aspectMask = aspectBits;
    info.subresourceRange.levelCount = mips;
    info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    vkCreateImageView(device, &info, nullptr, &view);
    return view;
}

// A short job for the GPU that is finished before the call returns: uploads and the like.
VkCommandBuffer begin_one_time_commands() {
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(onceCmd, 0);
    vkBeginCommandBuffer(onceCmd, &begin);
    return onceCmd;
}

void end_one_time_commands() {
    vkEndCommandBuffer(onceCmd);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &onceCmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
}

VkAccessFlags access_for(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
    default: return 0;
    }
}

void transition(VkCommandBuffer c, VkImage image, VkImageAspectFlags aspectBits, uint32_t mips, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspectBits, 0, mips, 0, 1};
    barrier.srcAccessMask = access_for(from);
    barrier.dstAccessMask = access_for(to);
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list)
        if (strcmp(e.extensionName, name) == 0) return true;
    return false;
}

VkSampler sampler_for(int flags) {
    int index = flags & 7;
    if (samplers[index]) return samplers[index];
    VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkFilter filter = (flags & TEX_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.magFilter = info.minFilter = filter;
    info.mipmapMode = (flags & TEX_NEAREST) ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = info.addressModeV = info.addressModeW = (flags & TEX_CLAMP) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.maxLod = (flags & TEX_NOMIP) ? 0.0f : 16.0f;
    if (anisotropy && !(flags & (TEX_NEAREST | TEX_NOMIP))) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = 4.0f;
    }
    vkCreateSampler(device, &info, nullptr, &samplers[index]);
    return samplers[index];
}

void destroy_swapchain_views() {
    for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    for (auto view : colorViews) vkDestroyImageView(device, view, nullptr);
    for (auto done : renderDone) vkDestroySemaphore(device, done, nullptr);
    renderDone.clear();
    framebuffers.clear();
    colorViews.clear();
    if (depthView) vkDestroyImageView(device, depthView, nullptr);
    if (depthImage) vkDestroyImage(device, depthImage, nullptr);
    if (depthMemory) vkFreeMemory(device, depthMemory, nullptr);
    if (msaaView) vkDestroyImageView(device, msaaView, nullptr);
    if (msaaImage) vkDestroyImage(device, msaaImage, nullptr);
    if (msaaMemory) vkFreeMemory(device, msaaMemory, nullptr);
    depthView = msaaView = VK_NULL_HANDLE;
    depthImage = msaaImage = VK_NULL_HANDLE;
    depthMemory = msaaMemory = VK_NULL_HANDLE;
}

VkAttachmentDescription attachment(VkFormat format, VkSampleCountFlagBits count, bool clearIt, bool store, VkImageLayout finalLayout) {
    VkAttachmentDescription a = {};
    a.format = format;
    a.samples = count;
    a.loadOp = clearIt ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = finalLayout;
    return a;
}

// kind: the screen (smoothed edges, resolved into the window), a texture target, or the shadow map (depth only).
bool create_render_pass(int kind, VkRenderPass& out) {
    VkAttachmentDescription attachments[3];
    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef = {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    uint32_t count = 2;
    if (kind == KIND_SCREEN) {
        bool multi = samples != VK_SAMPLE_COUNT_1_BIT;
        attachments[0] = attachment(colorFormat, samples, true, !multi, multi ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        attachments[1] = attachment(depthFormat, samples, true, false, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        attachments[2] = attachment(colorFormat, VK_SAMPLE_COUNT_1_BIT, false, true, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        if (multi) {
            subpass.pResolveAttachments = &resolveRef;
            count = 3;
        }
    } else if (kind == KIND_TARGET) {
        attachments[0] = attachment(targetFormat, VK_SAMPLE_COUNT_1_BIT, true, true, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        attachments[1] = attachment(depthFormat, VK_SAMPLE_COUNT_1_BIT, true, false, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
    } else {
        attachments[0] = attachment(depthFormat, VK_SAMPLE_COUNT_1_BIT, true, true, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        depthRef.attachment = 0;
        subpass.pDepthStencilAttachment = &depthRef;
        count = 1;
    }

    // What was drawn into a texture is read by later passes, so each pass waits for the one before.
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = deps[0].srcStageMask;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = deps[0].srcStageMask;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = deps[0].dstAccessMask;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = deps;
    return vkCreateRenderPass(device, &info, nullptr, &out) == VK_SUCCESS;
}

bool is_srgb(VkFormat f) { return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB; }

// Builds (or rebuilds) the swapchain to match the window. Leaves extent at 0x0 if the window is hidden.
bool create_swapchain() {
    vkDeviceWaitIdle(device);
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps) != VK_SUCCESS) return false;

    VkExtent2D size = caps.currentExtent;
    if (size.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        size.width = SDL_clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width);
        size.height = SDL_clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (size.width == 0 || size.height == 0) {
        extent = {0, 0};
        return true;
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, formats.data());
    if (formats.empty()) return false;
    // An sRGB format makes the screen do the conversion back from linear light.
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }
    for (const auto& f : formats)
        if (is_srgb(f.format)) { chosen = f; break; }
    if (colorFormat != VK_FORMAT_UNDEFINED)
        for (const auto& f : formats)
            if (f.format == colorFormat) { chosen = f; break; }

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alphaChoices[] = {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                                        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR};
    for (auto choice : alphaChoices)
        if (caps.supportedCompositeAlpha & choice) { alpha = choice; break; }

    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && info.minImageCount > caps.maxImageCount) info.minImageCount = caps.maxImageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = size;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
#if defined(GFX_SCREENSHOT)
    info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#endif
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(device, &info, nullptr, &created) != VK_SUCCESS) return false;
    destroy_swapchain_views();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = created;
    extent = size;
    colorFormat = chosen.format;

    if (!screenPass && !create_render_pass(KIND_SCREEN, screenPass)) return false;

    if (!create_image(depthImage, depthMemory, (int)extent.width, (int)extent.height, 1, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, samples)) return false;
    depthView = create_view(depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    bool multi = samples != VK_SAMPLE_COUNT_1_BIT;
    if (multi) {
        if (!create_image(msaaImage, msaaMemory, (int)extent.width, (int)extent.height, 1, colorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, samples)) return false;
        msaaView = create_view(msaaImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    }

    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    std::vector<VkImage> images(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());
    for (VkImage image : images) {
        VkImageView view = create_view(image, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        if (!view) return false;
        colorViews.push_back(view);

        VkImageView attachments[3] = {multi ? msaaView : view, depthView, view};
        VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = screenPass;
        fbInfo.attachmentCount = multi ? 3 : 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &fb) != VK_SUCCESS) return false;
        framebuffers.push_back(fb);
        VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore done = VK_NULL_HANDLE;
        vkCreateSemaphore(device, &semInfo, nullptr, &done);
        renderDone.push_back(done);
    }
    swapchainStale = false;
    return true;
}

VkShaderModule load_shader(const std::string& path) {
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data || size == 0 || size % 4 != 0) {
        SDL_free(data);
        shaderProblem = "Missing compiled shader: " + path;
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = size;
    info.pCode = (const uint32_t*)data;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        shaderProblem = "The GPU rejected shader: " + path;
        module = VK_NULL_HANDLE;
    }
    SDL_free(data);
    return module;
}

// Shaders are stored as <dir>/<name>.vert.spv and <dir>/<name>.frag.spv.
VkPipeline create_pipeline(const std::string& dir, const std::string& name, int kind, int blendMode, bool depthTest, bool depthWrite, bool cullBack) {
    VkShaderModule vert = load_shader(dir + "/" + name + ".vert.spv");
    VkShaderModule frag = load_shader(dir + "/" + name + ".frag.spv");
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vert && frag) {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[5] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
            {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
        };
        VkPipelineVertexInputStateCreateInfo vertexInput = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 5;
        vertexInput.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        // Meshes wind their triangles counter-clockwise seen from outside, as glTF does.
        VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = (cullBack && kind != KIND_SHADOW) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        if (kind == KIND_SHADOW) {
            raster.depthBiasEnable = VK_TRUE;
            raster.depthBiasConstantFactor = 1.25f;
            raster.depthBiasSlopeFactor = 1.75f;
        }

        VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = kind == KIND_SCREEN ? samples : VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthState = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthState.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
        depthState.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
        depthState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendAttachment = {};
        blendAttachment.blendEnable = blendMode != BLEND_OPAQUE ? VK_TRUE : VK_FALSE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = blendMode == BLEND_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = blendMode == BLEND_ADD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blendState = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blendState.attachmentCount = kind == KIND_SHADOW ? 0 : 1;
        blendState.pAttachments = &blendAttachment;

        VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthState;
        info.pColorBlendState = &blendState;
        info.pDynamicState = &dynamic;
        info.layout = pipelineLayout;
        info.renderPass = kind == KIND_SCREEN ? screenPass : kind == KIND_TARGET ? targetPass : shadowPass;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            shaderProblem = "Couldn't build a pipeline from shader '" + name + "'";
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (vert) vkDestroyShaderModule(device, vert, nullptr);
    if (frag) vkDestroyShaderModule(device, frag, nullptr);
    return pipeline;
}

// A game's own shader wins over an engine shader of the same name.
ShaderEntry* find_shader(const std::string& name) {
    auto found = shaders.find(name);
    if (found != shaders.end()) return &found->second;
    ShaderEntry entry;
    entry.name = name;
    entry.dir = shaderDir;
    if (!gameDir.empty() && SDL_GetPathInfo((gameDir + "/shaders/" + name + ".vert.spv").c_str(), nullptr)) entry.dir = gameDir + "/shaders";
    return &shaders.emplace(name, entry).first->second;
}

VkPipeline pipeline_for(ShaderEntry* entry, int kind, int blendMode, bool depthTest, bool depthWrite, bool cullBack) {
    ShaderEntry* standard = find_shader("mesh");
    if (!entry || entry->broken) entry = standard;
    uint32_t variant = (uint32_t)kind | ((uint32_t)blendMode << 2) | (depthTest ? 16u : 0u) | (depthWrite ? 32u : 0u) | (cullBack ? 64u : 0u);
    auto found = entry->variants.find(variant);
    if (found != entry->variants.end()) return found->second;
    VkPipeline created = create_pipeline(entry->dir, entry->name, kind, blendMode, depthTest, depthWrite, cullBack);
    if (!created && entry != standard) {
        // Keep drawing with the standard shader. The problem is shown on screen.
        entry->broken = true;
        return pipeline_for(standard, kind, blendMode, depthTest, depthWrite, cullBack);
    }
    entry->variants[variant] = created;
    return created;
}

}  // namespace internal

using namespace internal;

bool init(SDL_Window* win, const std::string& engineShaderDir, std::string& error) {
    window = win;
    shaderDir = engineShaderDir;

    if (!SDL_Vulkan_LoadLibrary(nullptr)) return fail(error, "This device has no Vulkan driver.");
#if defined(USE_VOLK)
    volkInitializeCustom((PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr());
#else
    // MoltenVK is linked into the app. SDL finds it by this name, so make sure the linker keeps it.
    static volatile PFN_vkGetInstanceProcAddr keepLinked = &vkGetInstanceProcAddr;
    (void)keepLinked;
#endif

    // Instance
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

    Uint32 sdlCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlCount);
    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlCount);
    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (has_extension(available, "VK_KHR_portability_enumeration")) {
        extensions.push_back("VK_KHR_portability_enumeration");
        instanceInfo.flags |= 0x00000001;  // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gamelab";
    app.apiVersion = VK_API_VERSION_1_2;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = (uint32_t)extensions.size();
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) return fail(error, "Couldn't start Vulkan (version 1.2 is needed).");
#if defined(USE_VOLK)
    volkLoadInstance(instance);
#endif

    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) return fail(error, SDL_GetError());

    // Pick a GPU that can draw to the window, preferring a dedicated one.
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    int bestScore = -1;
    for (VkPhysicalDevice candidate : gpus) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        for (uint32_t i = 0; i < familyCount; i++) {
            VkBool32 canPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &canPresent);
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !canPresent) continue;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(candidate, &props);
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > bestScore) {
                bestScore = score;
                gpu = candidate;
                queueFamily = i;
            }
            break;
        }
    }
    if (!gpu) return fail(error, "No graphics card here can draw to the window with Vulkan.");

    // Device
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> deviceAvailable(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, deviceAvailable.data());
    std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (has_extension(deviceAvailable, "VK_KHR_portability_subset")) deviceExtensions.push_back("VK_KHR_portability_subset");

    VkPhysicalDeviceFeatures supported, wanted = {};
    vkGetPhysicalDeviceFeatures(gpu, &supported);
    anisotropy = supported.samplerAnisotropy == VK_TRUE;
    wanted.samplerAnisotropy = supported.samplerAnisotropy;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    deviceInfo.pEnabledFeatures = &wanted;
    if (vkCreateDevice(gpu, &deviceInfo, nullptr, &device) != VK_SUCCESS) return fail(error, "Couldn't open the graphics card.");
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VkFormatProperties depthProps;
    vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_D32_SFLOAT, &depthProps);
    depthFormat = (depthProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;

    // Smooth edges (4x MSAA) where the GPU offers it, which every iPhone does.
    VkPhysicalDeviceProperties gpuProps;
    vkGetPhysicalDeviceProperties(gpu, &gpuProps);
    const char* noSmooth = SDL_getenv("GAMELAB_NO_MSAA");
    if (!noSmooth && (gpuProps.limits.framebufferColorSampleCounts & gpuProps.limits.framebufferDepthSampleCounts & VK_SAMPLE_COUNT_4_BIT)) samples = VK_SAMPLE_COUNT_4_BIT;

    if (!create_swapchain() || !screenPass) return fail(error, "Couldn't set up drawing to the window.");
    if (!create_render_pass(KIND_TARGET, targetPass) || !create_render_pass(KIND_SHADOW, shadowPass)) return fail(error, "Couldn't set up drawing to textures.");

    // Commands and frame pacing
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
    VkCommandBufferAllocateInfo cmdInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = commandPool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 2;
    VkCommandBuffer both[2];
    vkAllocateCommandBuffers(device, &cmdInfo, both);
    cmd = both[0];
    onceCmd = both[1];
    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device, &semInfo, nullptr, &imageReady);
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device, &fenceInfo, nullptr, &frameFence);

    // Set 0: the per-frame data block (camera, lights, time) and the shadow map. Set 1: a material's four textures.
    const VkShaderStageFlags bothStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding frameBindings[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, bothStages, nullptr},
                                                     {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = frameBindings;
    vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &frameLayout);
    VkDescriptorSetLayoutBinding materialBindings[4];
    for (uint32_t i = 0; i < 4; i++) materialBindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, bothStages, nullptr};
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = materialBindings;
    vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &materialLayout);

    const uint32_t maxMaterials = 2048;
    VkDescriptorPoolSize poolSizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxMaterials * 4 + 1}};
    VkDescriptorPoolCreateInfo descPoolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descPoolInfo.maxSets = maxMaterials + 1;
    descPoolInfo.poolSizeCount = 2;
    descPoolInfo.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(device, &descPoolInfo, nullptr, &descriptorPool);
    VkDescriptorSetAllocateInfo setInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &frameLayout;
    vkAllocateDescriptorSets(device, &setInfo, &frameSet);

    VkDeviceSize align = SDL_max(gpuProps.limits.minUniformBufferOffsetAlignment, (VkDeviceSize)16);
    frameStride = (sizeof(FrameData) + align - 1) / align * align;
    if (!create_buffer(frameBuffer, frameStride * FRAME_SLOTS, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !create_buffer(dynBuffer, (VkDeviceSize)MAX_DYNAMIC_VERTS * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return fail(error, "Out of graphics memory.");

    // The shadow map: a depth picture of the scene as the sun sees it.
    shadowTex.w = shadowTex.h = SHADOW_SIZE;
    if (!create_image(shadowTex.image, shadowTex.memory, SHADOW_SIZE, SHADOW_SIZE, 1, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT))
        return fail(error, "Out of graphics memory.");
    shadowTex.view = create_view(shadowTex.image, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1);
    VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbInfo.renderPass = shadowPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &shadowTex.view;
    fbInfo.width = fbInfo.height = SHADOW_SIZE;
    fbInfo.layers = 1;
    vkCreateFramebuffer(device, &fbInfo, nullptr, &shadowTex.framebuffer);
    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &samplerInfo, nullptr, &shadowSampler);
    VkCommandBuffer c = begin_one_time_commands();
    transition(c, shadowTex.image, VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    end_one_time_commands();

    VkDescriptorBufferInfo bufferInfo = {frameBuffer.buffer, 0, sizeof(FrameData)};
    VkDescriptorImageInfo shadowInfo = {shadowSampler, shadowTex.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType = writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = writes[1].dstSet = frameSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[0].pBufferInfo = &bufferInfo;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &shadowInfo;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    VkPushConstantRange pushRange = {bothStages, 0, sizeof(PushData)};
    VkDescriptorSetLayout setLayouts[2] = {frameLayout, materialLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);

    // Stand-ins: plain white where no texture is set, a flat normal map, and a loud checker for a missing file.
    const uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255};
    uint8_t checker[8 * 8 * 4];
    for (int i = 0; i < 64; i++) {
        bool on = ((i % 8) / 4 + (i / 8) / 4) % 2 == 0;
        const uint8_t pixel[4] = {(uint8_t)(on ? 255 : 0), 0, (uint8_t)(on ? 255 : 0), 255};
        memcpy(checker + i * 4, pixel, 4);
    }
    whiteTex = make_texture(1, 1, white, TEX_NOMIP, OWNER_ENGINE);
    normalTex = make_texture(1, 1, flat, TEX_NOMIP | TEX_DATA, OWNER_ENGINE);
    missingTex = make_texture(8, 8, checker, TEX_NOMIP | TEX_NEAREST, OWNER_ENGINE);
    build_meshes();
    if (!whiteTex || !normalTex || !missingTex || !builtin[MESH_CUBE]) return fail(error, "Out of graphics memory.");

    passes.resize(1);
    ShaderEntry* standard = find_shader("mesh");
    if (!pipeline_for(standard, KIND_SCREEN, BLEND_OPAQUE, true, true, true) || !pipeline_for(standard, KIND_SCREEN, BLEND_ALPHA, false, false, false))
        return fail(error, shaderProblem.c_str());
    shaderProblem.clear();
    return true;
}

void shutdown() {
    if (!device) return;
    vkDeviceWaitIdle(device);
    set_game_dir("");
    for (auto& entry : shaders)
        for (auto& variant : entry.second.variants)
            if (variant.second) vkDestroyPipeline(device, variant.second, nullptr);
    shaders.clear();
    textures.each([](Handle, Texture& t) { destroy_texture(t); });
    meshes.each([](Handle, MeshRes& m) { destroy_mesh(m); });
    destroy_texture(shadowTex);
    for (VkSampler s : samplers)
        if (s) vkDestroySampler(device, s, nullptr);
    if (shadowSampler) vkDestroySampler(device, shadowSampler, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    destroy_buffer(frameBuffer);
    destroy_buffer(dynBuffer);
    if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    if (frameLayout) vkDestroyDescriptorSetLayout(device, frameLayout, nullptr);
    if (materialLayout) vkDestroyDescriptorSetLayout(device, materialLayout, nullptr);
    if (frameFence) vkDestroyFence(device, frameFence, nullptr);
    if (imageReady) vkDestroySemaphore(device, imageReady, nullptr);
    if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
    destroy_swapchain_views();
    if (screenPass) vkDestroyRenderPass(device, screenPass, nullptr);
    if (targetPass) vkDestroyRenderPass(device, targetPass, nullptr);
    if (shadowPass) vkDestroyRenderPass(device, shadowPass, nullptr);
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    if (surface) SDL_Vulkan_DestroySurface(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
}

}  // namespace gfx
