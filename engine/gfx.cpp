#include "gfx.h"

// Desktop builds find Vulkan at run time through volk. The iPhone build links
// MoltenVK directly, so it uses the ordinary Vulkan function names.
#if defined(USE_VOLK)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "volk.h"
#else
#include <vulkan/vulkan.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <vector>

#include "font8x8_basic.h"

namespace gfx {
namespace {

struct Vertex {
    float pos[3];
    float normal[3];
    float color[4];
};

struct FrameData {
    float viewProj[16];
    float ortho[16];
    float lightDir[4];
    float params[4];
};

struct PushData {
    float model[16];
    float color[4];
    float flags[4];
};

struct Draw {
    VkPipeline pipeline;
    uint32_t first, count;
    PushData push;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

const float VIEW_HEIGHT = 720.0f;
const uint32_t MAX_2D_VERTS = 120000;

SDL_Window* window = nullptr;
VkInstance instance = VK_NULL_HANDLE;
VkSurfaceKHR surface = VK_NULL_HANDLE;
VkPhysicalDevice gpu = VK_NULL_HANDLE;
VkDevice device = VK_NULL_HANDLE;
uint32_t queueFamily = 0;
VkQueue queue = VK_NULL_HANDLE;

VkSwapchainKHR swapchain = VK_NULL_HANDLE;
VkFormat colorFormat = VK_FORMAT_UNDEFINED;
VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
VkExtent2D extent = {0, 0};
std::vector<VkImageView> colorViews;
std::vector<VkFramebuffer> framebuffers;
VkImage depthImage = VK_NULL_HANDLE;
VkDeviceMemory depthMemory = VK_NULL_HANDLE;
VkImageView depthView = VK_NULL_HANDLE;
bool swapchainStale = false;

VkRenderPass renderPass = VK_NULL_HANDLE;
VkCommandPool commandPool = VK_NULL_HANDLE;
VkCommandBuffer cmd = VK_NULL_HANDLE;
VkSemaphore imageReady = VK_NULL_HANDLE, renderDone = VK_NULL_HANDLE;
VkFence frameFence = VK_NULL_HANDLE;

VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
VkDescriptorSet frameSet = VK_NULL_HANDLE;
VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
VkPipeline pipeline3D = VK_NULL_HANDLE, pipeline2D = VK_NULL_HANDLE;
std::map<std::string, VkPipeline> gamePipelines;

Buffer frameBuffer, meshBuffer, batchBuffer;
uint32_t meshFirst[MESH_COUNT], meshCount[MESH_COUNT];

std::string shaderDir, gameShaderDir, shaderProblem;

// Per-frame state
uint32_t imageIndex = 0;
float clearColor[3] = {0.06f, 0.07f, 0.10f};
float viewProj[16];
float lightDir[4] = {-0.4f, -1.0f, -0.3f, 0.0f};
VkPipeline currentPipeline = VK_NULL_HANDLE;
std::vector<Draw> draws;
uint32_t batchCount = 0;
std::vector<UiBox> uiBoxes;
bool uiLogging = false;

// ---- small matrix helpers (column-major, the same layout Vulkan and GLSL use) ----

void identity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void multiply(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++)
            r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] +
                             a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
    memcpy(out, r, sizeof(r));
}

void normalize3(float v[3]) {
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-6f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

float aspect() { return extent.height ? (float)extent.width / (float)extent.height : 1.0f; }

// ---- Vulkan helpers ----

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
    info.size = size;
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
    return vkMapMemory(device, out.memory, 0, size, 0, &out.mapped) == VK_SUCCESS;
}

void destroy_buffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = Buffer();
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list)
        if (strcmp(e.extensionName, name) == 0) return true;
    return false;
}

void destroy_swapchain_views() {
    for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
    for (auto view : colorViews) vkDestroyImageView(device, view, nullptr);
    framebuffers.clear();
    colorViews.clear();
    if (depthView) vkDestroyImageView(device, depthView, nullptr);
    if (depthImage) vkDestroyImage(device, depthImage, nullptr);
    if (depthMemory) vkFreeMemory(device, depthMemory, nullptr);
    depthView = VK_NULL_HANDLE;
    depthImage = VK_NULL_HANDLE;
    depthMemory = VK_NULL_HANDLE;
}

bool create_render_pass() {
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = colorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[1] = attachments[0];
    attachments[1].format = depthFormat;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = dep.srcStageMask;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dep;
    return vkCreateRenderPass(device, &info, nullptr, &renderPass) == VK_SUCCESS;
}

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
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = f; break; }
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

    if (!renderPass && !create_render_pass()) return false;

    VkImageCreateInfo depthInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    depthInfo.imageType = VK_IMAGE_TYPE_2D;
    depthInfo.format = depthFormat;
    depthInfo.extent = {extent.width, extent.height, 1};
    depthInfo.mipLevels = 1;
    depthInfo.arrayLayers = 1;
    depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (vkCreateImage(device, &depthInfo, nullptr, &depthImage) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, depthImage, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &depthMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, depthImage, depthMemory, 0);

    VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    viewInfo.image = depthImage;
    viewInfo.format = depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vkCreateImageView(device, &viewInfo, nullptr, &depthView) != VK_SUCCESS) return false;

    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    std::vector<VkImage> images(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());
    for (VkImage image : images) {
        viewInfo.image = image;
        viewInfo.format = colorFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) return false;
        colorViews.push_back(view);

        VkImageView attachments[2] = {view, depthView};
        VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &fb) != VK_SUCCESS) return false;
        framebuffers.push_back(fb);
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
VkPipeline create_pipeline(const std::string& dir, const std::string& name, bool is3D) {
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
        VkVertexInputAttributeDescription attrs[3] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
        };
        VkPipelineVertexInputStateCreateInfo vertexInput = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 3;
        vertexInput.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = is3D ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = is3D ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blendAttachment = {};
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

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
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = pipelineLayout;
        info.renderPass = renderPass;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            shaderProblem = "Couldn't build a pipeline from shader '" + name + "'";
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (vert) vkDestroyShaderModule(device, vert, nullptr);
    if (frag) vkDestroyShaderModule(device, frag, nullptr);
    return pipeline;
}

// ---- built-in meshes ----

void add_quad(std::vector<Vertex>& out, const float a[3], const float b[3], const float c[3], const float d[3], const float n[3]) {
    const float* corners[6] = {a, b, c, a, c, d};
    for (const float* p : corners) {
        Vertex v = {{p[0], p[1], p[2]}, {n[0], n[1], n[2]}, {1, 1, 1, 1}};
        out.push_back(v);
    }
}

void build_meshes(std::vector<Vertex>& out) {
    // Cube, one unit wide, centred on the origin.
    meshFirst[MESH_CUBE] = (uint32_t)out.size();
    for (int axis = 0; axis < 3; axis++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            int u = (axis + 1) % 3, w = (axis + 2) % 3;
            float n[3] = {0, 0, 0}, p[4][3];
            n[axis] = (float)sign;
            const float su[4] = {-0.5f, 0.5f, 0.5f, -0.5f}, sw[4] = {-0.5f, -0.5f, 0.5f, 0.5f};
            for (int i = 0; i < 4; i++) {
                p[i][axis] = 0.5f * sign;
                p[i][u] = su[i];
                p[i][w] = sw[i];
            }
            add_quad(out, p[0], p[1], p[2], p[3], n);
        }
    }
    meshCount[MESH_CUBE] = (uint32_t)out.size() - meshFirst[MESH_CUBE];

    // Sphere, one unit across.
    meshFirst[MESH_SPHERE] = (uint32_t)out.size();
    const int rings = 12, segments = 20;
    const float pi = 3.14159265f;
    auto point = [&](int ring, int seg, float p[3]) {
        float phi = pi * ring / rings, theta = 2 * pi * seg / segments;
        p[0] = 0.5f * sinf(phi) * cosf(theta);
        p[1] = 0.5f * cosf(phi);
        p[2] = 0.5f * sinf(phi) * sinf(theta);
    };
    for (int ring = 0; ring < rings; ring++) {
        for (int seg = 0; seg < segments; seg++) {
            float p[4][3];
            point(ring, seg, p[0]);
            point(ring + 1, seg, p[1]);
            point(ring + 1, seg + 1, p[2]);
            point(ring, seg + 1, p[3]);
            const int order[6] = {0, 1, 2, 0, 2, 3};
            for (int i : order) {
                Vertex v = {{p[i][0], p[i][1], p[i][2]}, {p[i][0] * 2, p[i][1] * 2, p[i][2] * 2}, {1, 1, 1, 1}};
                out.push_back(v);
            }
        }
    }
    meshCount[MESH_SPHERE] = (uint32_t)out.size() - meshFirst[MESH_SPHERE];

    // Flat square on the ground, one unit wide, facing up.
    meshFirst[MESH_PLANE] = (uint32_t)out.size();
    const float a[3] = {-0.5f, 0, -0.5f}, b[3] = {0.5f, 0, -0.5f}, c[3] = {0.5f, 0, 0.5f}, d[3] = {-0.5f, 0, 0.5f}, up[3] = {0, 1, 0};
    add_quad(out, a, b, c, d, up);
    meshCount[MESH_PLANE] = (uint32_t)out.size() - meshFirst[MESH_PLANE];
}

void push_2d(float x, float y, const float rgba[4]) {
    if (batchCount >= MAX_2D_VERTS) return;
    Vertex v = {{x, y, 0}, {0, 0, 1}, {rgba[0], rgba[1], rgba[2], rgba[3]}};
    ((Vertex*)batchBuffer.mapped)[batchCount++] = v;
}

}  // namespace

// ---- public API ----

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
    if (vkCreateDevice(gpu, &deviceInfo, nullptr, &device) != VK_SUCCESS) return fail(error, "Couldn't open the graphics card.");
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VkFormatProperties depthProps;
    vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_D32_SFLOAT, &depthProps);
    depthFormat = (depthProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;

    if (!create_swapchain() || !renderPass) return fail(error, "Couldn't set up drawing to the window.");

    // Commands and frame pacing
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
    VkCommandBufferAllocateInfo cmdInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = commandPool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cmdInfo, &cmd);
    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device, &semInfo, nullptr, &imageReady);
    vkCreateSemaphore(device, &semInfo, nullptr, &renderDone);
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device, &fenceInfo, nullptr, &frameFence);

    // The per-frame data block (camera, light, time) that every shader receives.
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout);
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo descPoolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descPoolInfo.maxSets = 1;
    descPoolInfo.poolSizeCount = 1;
    descPoolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device, &descPoolInfo, nullptr, &descriptorPool);
    VkDescriptorSetAllocateInfo setInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &setLayout;
    vkAllocateDescriptorSets(device, &setInfo, &frameSet);

    std::vector<Vertex> meshes;
    build_meshes(meshes);
    if (!create_buffer(frameBuffer, sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !create_buffer(meshBuffer, meshes.size() * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !create_buffer(batchBuffer, MAX_2D_VERTS * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return fail(error, "Out of graphics memory.");
    memcpy(meshBuffer.mapped, meshes.data(), meshes.size() * sizeof(Vertex));

    VkDescriptorBufferInfo bufferInfo = {frameBuffer.buffer, 0, sizeof(FrameData)};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = frameSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    VkPushConstantRange pushRange = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushData)};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);

    pipeline3D = create_pipeline(shaderDir, "mesh", true);
    pipeline2D = create_pipeline(shaderDir, "mesh", false);
    if (!pipeline3D || !pipeline2D) return fail(error, shaderProblem.c_str());
    shaderProblem.clear();
    return true;
}

void shutdown() {
    if (!device) return;
    vkDeviceWaitIdle(device);
    set_game_dir("");
    if (pipeline3D) vkDestroyPipeline(device, pipeline3D, nullptr);
    if (pipeline2D) vkDestroyPipeline(device, pipeline2D, nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    destroy_buffer(frameBuffer);
    destroy_buffer(meshBuffer);
    destroy_buffer(batchBuffer);
    if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    if (frameFence) vkDestroyFence(device, frameFence, nullptr);
    if (imageReady) vkDestroySemaphore(device, imageReady, nullptr);
    if (renderDone) vkDestroySemaphore(device, renderDone, nullptr);
    if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
    destroy_swapchain_views();
    if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    vkDestroyDevice(device, nullptr);
    if (surface) SDL_Vulkan_DestroySurface(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
}

void set_game_dir(const std::string& gameDir) {
    if (!device) return;
    vkDeviceWaitIdle(device);
    for (auto& entry : gamePipelines)
        if (entry.second) vkDestroyPipeline(device, entry.second, nullptr);
    gamePipelines.clear();
    gameShaderDir = gameDir.empty() ? "" : gameDir + "/shaders";
    shaderProblem.clear();
    currentPipeline = pipeline3D;
}

bool begin_frame() {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (swapchainStale || extent.width == 0 || (uint32_t)w != extent.width || (uint32_t)h != extent.height) {
        if (!create_swapchain() || extent.width == 0) return false;
    }
    vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX);
    VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageReady, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainStale = true;
        return false;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return false;

    draws.clear();
    batchCount = 0;
    uiBoxes.clear();
    currentPipeline = pipeline3D;
    clearColor[0] = 0.06f; clearColor[1] = 0.07f; clearColor[2] = 0.10f;
    const float eye[3] = {0, 3, 8}, target[3] = {0, 0, 0};
    camera(eye, target, 60.0f);
    return true;
}

void end_frame(float timeSeconds) {
    FrameData frame;
    memcpy(frame.viewProj, viewProj, sizeof(viewProj));
    identity(frame.ortho);
    frame.ortho[0] = 2.0f / view_width();
    frame.ortho[5] = 2.0f / view_height();
    frame.ortho[12] = -1.0f;
    frame.ortho[13] = -1.0f;
    memcpy(frame.lightDir, lightDir, sizeof(lightDir));
    frame.params[0] = timeSeconds;
    frame.params[1] = (float)extent.width;
    frame.params[2] = (float)extent.height;
    frame.params[3] = 0;
    memcpy(frameBuffer.mapped, &frame, sizeof(frame));

    vkResetFences(device, 1, &frameFence);
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkClearValue clears[2] = {};
    clears[0].color.float32[0] = clearColor[0];
    clears[0].color.float32[1] = clearColor[1];
    clears[0].color.float32[2] = clearColor[2];
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = renderPass;
    pass.framebuffer = framebuffers[imageIndex];
    pass.renderArea.extent = extent;
    pass.clearValueCount = 2;
    pass.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {0, 0, (float)extent.width, (float)extent.height, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &frameSet, 0, nullptr);
    const VkShaderStageFlags pushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDeviceSize offset = 0;

    vkCmdBindVertexBuffers(cmd, 0, 1, &meshBuffer.buffer, &offset);
    VkPipeline bound = VK_NULL_HANDLE;
    for (const Draw& d : draws) {
        if (d.pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
            bound = d.pipeline;
        }
        vkCmdPushConstants(cmd, pipelineLayout, pushStages, 0, sizeof(PushData), &d.push);
        vkCmdDraw(cmd, d.count, 1, d.first, 0);
    }

    if (batchCount > 0) {
        PushData push = {};
        identity(push.model);
        push.color[0] = push.color[1] = push.color[2] = push.color[3] = 1.0f;
        push.flags[0] = 1.0f;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline2D);
        vkCmdBindVertexBuffers(cmd, 0, 1, &batchBuffer.buffer, &offset);
        vkCmdPushConstants(cmd, pipelineLayout, pushStages, 0, sizeof(PushData), &push);
        vkCmdDraw(cmd, batchCount, 1, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageReady;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderDone;
    vkQueueSubmit(queue, 1, &submit, frameFence);

    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderDone;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    VkResult result = vkQueuePresentKHR(queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) swapchainStale = true;
}

void clear(float r, float g, float b) {
    clearColor[0] = r; clearColor[1] = g; clearColor[2] = b;
}

void camera(const float eye[3], const float target[3], float fovDegrees) {
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    normalize3(f);
    const float worldUp[3] = {0, 1, 0};
    float s[3], u[3];
    cross3(f, worldUp, s);
    normalize3(s);
    cross3(s, f, u);

    float view[16];
    identity(view);
    view[0] = s[0]; view[4] = s[1]; view[8] = s[2];
    view[1] = u[0]; view[5] = u[1]; view[9] = u[2];
    view[2] = -f[0]; view[6] = -f[1]; view[10] = -f[2];
    view[12] = -dot3(s, eye); view[13] = -dot3(u, eye); view[14] = dot3(f, eye);

    // Vulkan's screen has Y pointing down and depth running 0 to 1.
    const float zNear = 0.1f, zFar = 500.0f;
    float focal = 1.0f / tanf(fovDegrees * 3.14159265f / 360.0f);
    float proj[16] = {};
    proj[0] = focal / aspect();
    proj[5] = -focal;
    proj[10] = zFar / (zNear - zFar);
    proj[11] = -1.0f;
    proj[14] = (zNear * zFar) / (zNear - zFar);
    multiply(proj, view, viewProj);
}

void light(float x, float y, float z) {
    lightDir[0] = x; lightDir[1] = y; lightDir[2] = z;
}

void shader(const std::string& name) {
    if (name.empty() || gameShaderDir.empty()) {
        currentPipeline = pipeline3D;
        return;
    }
    auto found = gamePipelines.find(name);
    if (found == gamePipelines.end()) {
        VkPipeline created = create_pipeline(gameShaderDir, name, true);
        found = gamePipelines.emplace(name, created).first;
    }
    currentPipeline = found->second ? found->second : pipeline3D;
}

void mesh(Mesh which, const float pos[3], const float size[3], float yawDegrees, const float rgba[4]) {
    if (which < 0 || which >= MESH_COUNT) return;
    Draw d;
    d.pipeline = currentPipeline;
    d.first = meshFirst[which];
    d.count = meshCount[which];
    float c = cosf(yawDegrees * 3.14159265f / 180.0f), s = sinf(yawDegrees * 3.14159265f / 180.0f);
    float* m = d.push.model;
    identity(m);
    m[0] = c * size[0];  m[2] = -s * size[0];
    m[5] = size[1];
    m[8] = s * size[2];  m[10] = c * size[2];
    m[12] = pos[0]; m[13] = pos[1]; m[14] = pos[2];
    memcpy(d.push.color, rgba, sizeof(float) * 4);
    d.push.flags[0] = d.push.flags[1] = d.push.flags[2] = d.push.flags[3] = 0.0f;
    draws.push_back(d);
}

float view_width() { return aspect() >= 1.0f ? VIEW_HEIGHT * aspect() : VIEW_HEIGHT; }
float view_height() { return aspect() >= 1.0f ? VIEW_HEIGHT : VIEW_HEIGHT / aspect(); }

void ui_log(bool on) { uiLogging = on; }
const std::vector<UiBox>& ui_boxes() { return uiBoxes; }

void rect(float x, float y, float w, float h, const float rgba[4]) {
    if (uiLogging) uiBoxes.push_back({x, y, w, h, false, ""});
    push_2d(x, y, rgba);
    push_2d(x + w, y, rgba);
    push_2d(x + w, y + h, rgba);
    push_2d(x, y, rgba);
    push_2d(x + w, y + h, rgba);
    push_2d(x, y + h, rgba);
}

void circle(float cx, float cy, float radius, const float rgba[4]) {
    if (uiLogging) uiBoxes.push_back({cx - radius, cy - radius, radius * 2, radius * 2, true, ""});
    const int segments = 28;
    for (int i = 0; i < segments; i++) {
        float a0 = 6.2831853f * i / segments, a1 = 6.2831853f * (i + 1) / segments;
        push_2d(cx, cy, rgba);
        push_2d(cx + cosf(a0) * radius, cy + sinf(a0) * radius, rgba);
        push_2d(cx + cosf(a1) * radius, cy + sinf(a1) * radius, rgba);
    }
}

// Text uses a built-in 8x8 pixel font, so it needs no texture. Each run of lit pixels is one rectangle.
void text(const std::string& s, float x, float y, float size, const float rgba[4]) {
    // Logged as one box for the whole text, not one per pixel run.
    bool logging = uiLogging;
    uiLogging = false;
    if (logging && s.find_first_not_of(" \n") != std::string::npos) {
        float lines = 1;
        for (char ch : s) lines += ch == '\n';
        uiBoxes.push_back({x, y, text_width(s, size), size * (1 + (lines - 1) * 1.25f), false, s});
    }
    float cell = size / 8.0f, penX = x;
    for (unsigned char ch : s) {
        if (ch == '\n') { penX = x; y += size * 1.25f; continue; }
        if (ch < 128) {
            for (int row = 0; row < 8; row++) {
                unsigned char bits = font8x8_basic[ch][row];
                int col = 0;
                while (col < 8) {
                    if (!(bits & (1 << col))) { col++; continue; }
                    int start = col;
                    while (col < 8 && (bits & (1 << col))) col++;
                    rect(penX + start * cell, y + row * cell, (col - start) * cell, cell, rgba);
                }
            }
        }
        penX += size;
    }
    uiLogging = logging;
}

float text_width(const std::string& s, float size) {
    float widest = 0, current = 0;
    for (char ch : s) {
        if (ch == '\n') { current = 0; continue; }
        current += size;
        if (current > widest) widest = current;
    }
    return widest;
}

const std::string& shader_error() { return shaderProblem; }

#if defined(GFX_SCREENSHOT)
// Debugging aid, off by default: saves the frame that was just drawn as a BMP file.
bool screenshot(const std::string& path) {
    vkDeviceWaitIdle(device);
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
    Buffer pixels;
    VkDeviceSize size = (VkDeviceSize)extent.width * extent.height * 4;
    if (!create_buffer(pixels, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;

    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[imageIndex];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixels.buffer, 1, &region);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    SDL_PixelFormat format = colorFormat == VK_FORMAT_B8G8R8A8_UNORM ? SDL_PIXELFORMAT_BGRX32 : SDL_PIXELFORMAT_RGBX32;
    SDL_Surface* surface = SDL_CreateSurfaceFrom((int)extent.width, (int)extent.height, format, pixels.mapped, (int)extent.width * 4);
    bool ok = surface && SDL_SaveBMP(surface, path.c_str());
    SDL_DestroySurface(surface);
    destroy_buffer(pixels);
    return ok;
}
#endif

}  // namespace gfx
