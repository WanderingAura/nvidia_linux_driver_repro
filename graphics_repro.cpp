// Minimal repro: vkCmdPushDataEXT in a fragment shader, headless.
//
// No window and no swapchain. It renders a fullscreen triangle to an
// offscreen image, copies it back, and checks the pixel value, so the
// failure is a deterministic numeric mismatch rather than visible flicker.
//
// Heap slot 0 holds opaque red, slot 1 holds opaque green. Every frame the
// host pushes the index of the slot it wants via vkCmdPushDataEXT and
// alternates between them, mirroring a renderer that selects a
// frame-in-flight's resources per draw. Two frames are kept in flight and
// command buffers are reset and re-recorded each frame, as a real renderer
// would.
//
// Expected: frame N renders exactly the colour of slot (N % 2).
//
// Build and run instructions are in README.md.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kExtent = 64;
constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kFrameCount = 40;
constexpr uint32_t kHeapSlots = 2;  // slot 0: palette{red,green}; slot 1: green

#define VK_CHECK(expr)                                                            \
    do {                                                                          \
        VkResult result_ = (expr);                                                \
        if (result_ != VK_SUCCESS) {                                              \
            throw std::runtime_error(std::string(#expr) + " failed with " +       \
                                     std::to_string(result_));                    \
        }                                                                         \
    } while (0)

std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("cannot open " + path);
    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> data(size);
    file.seekg(0);
    file.read(data.data(), static_cast<std::streamsize>(size));
    return data;
}

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) & ~(a - 1); }

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    void* mapped = nullptr;
};

struct Rgba {
    uint8_t r, g, b, a;
    bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    std::string str() const {
        return "(" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + "," +
               std::to_string(a) + ")";
    }
};

class Repro {
public:
    int run() {
        createInstance();
        pickPhysicalDevice();
        createDevice();
        loadFunctions();
        createTargets();
        createUniformBuffers();
        createHeap();
        createPipelines();
        createCommandsAndSync();

        uint32_t slot0Failures =
            renderFrames("[A] constant heap index 0 (expect red every frame):", slot0Pipeline_,
                         false, 0);
        uint32_t slot1Failures =
            renderFrames("[B] constant heap index 1 (expect green every frame):", slot1Pipeline_,
                         false, 1);
        uint32_t workaroundFailures = renderFrames(
            "[C] heap index 0, pushed value selects inside the buffer:", slot0DynamicPipeline_,
            true);

        if (slot0Failures == 0 && slot1Failures == 0 && workaroundFailures == 0) {
            std::cout << "RESULT: all cases correct.\n";
        } else if (slot0Failures == 0 && slot1Failures > 0) {
            std::cout << "RESULT: heap slot 0 reads correctly, but a CONSTANT heap index of 1\n"
                      << "        reads the wrong descriptor (B failed " << slot1Failures << "/"
                      << kFrameCount << " frames).\n"
                      << "        No push data or dynamic indexing is involved in B at all.\n";
            if (workaroundFailures == 0) {
                std::cout << "        Keeping the heap index at 0 and selecting inside the buffer\n"
                          << "        works (C passed), which is a viable workaround.\n";
            }
        } else {
            std::cout << "RESULT: unexpected combination of failures (see above).\n";
        }

        cleanup();
        return (slot0Failures == 0 && slot1Failures == 0 && workaroundFailures == 0) ? 0 : 1;
    }

private:
    void createInstance() {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "descriptor_heap_graphics_repro";
        app.apiVersion = VK_API_VERSION_1_4;

        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        VK_CHECK(vkCreateInstance(&info, nullptr, &instance_));
    }

    bool deviceSupportsHeaps(VkPhysicalDevice device) {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> exts(count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, exts.data());
        bool heap = false, untyped = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME) == 0) heap = true;
            if (std::strcmp(e.extensionName, VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME) == 0)
                untyped = true;
        }
        return heap && untyped;
    }

    void pickPhysicalDevice() {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        for (VkPhysicalDevice d : devices) {
            if (deviceSupportsHeaps(d)) {
                physicalDevice_ = d;
                break;
            }
        }
        if (!physicalDevice_) {
            throw std::runtime_error("no device supports VK_EXT_descriptor_heap + "
                                     "VK_KHR_shader_untyped_pointers");
        }

        heapProps_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &heapProps_;
        vkGetPhysicalDeviceProperties2(physicalDevice_, &props2);

        const VkPhysicalDeviceProperties& p = props2.properties;
        std::cout << "Device:      " << p.deviceName << "\n"
                  << "Driver:      " << VK_VERSION_MAJOR(p.driverVersion) << "."
                  << VK_VERSION_MINOR(p.driverVersion) << "." << VK_VERSION_PATCH(p.driverVersion)
                  << "\n"
                  << "API version: " << VK_VERSION_MAJOR(p.apiVersion) << "."
                  << VK_VERSION_MINOR(p.apiVersion) << "." << VK_VERSION_PATCH(p.apiVersion) << "\n\n";
    }

    void createDevice() {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());
        for (uint32_t i = 0; i < familyCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                queueFamily_ = i;
                break;
            }
        }

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untyped{};
        untyped.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR;
        untyped.shaderUntypedPointers = VK_TRUE;

        VkPhysicalDeviceDescriptorHeapFeaturesEXT heap{};
        heap.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
        heap.descriptorHeap = VK_TRUE;
        heap.pNext = &untyped;

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.pNext = &heap;

        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.bufferDeviceAddress = VK_TRUE;
        features12.pNext = &features13;

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features12;

        const char* extensions[] = {
            VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
            VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
        };

        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.pNext = &features2;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queueInfo;
        info.enabledExtensionCount = 2;
        info.ppEnabledExtensionNames = extensions;
        VK_CHECK(vkCreateDevice(physicalDevice_, &info, nullptr, &device_));
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    }

    void loadFunctions() {
        vkGetPhysicalDeviceDescriptorSizeEXT_ =
            reinterpret_cast<PFN_vkGetPhysicalDeviceDescriptorSizeEXT>(
                vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceDescriptorSizeEXT"));
        vkWriteResourceDescriptorsEXT_ = reinterpret_cast<PFN_vkWriteResourceDescriptorsEXT>(
            vkGetDeviceProcAddr(device_, "vkWriteResourceDescriptorsEXT"));
        vkCmdBindResourceHeapEXT_ = reinterpret_cast<PFN_vkCmdBindResourceHeapEXT>(
            vkGetDeviceProcAddr(device_, "vkCmdBindResourceHeapEXT"));
        vkCmdPushDataEXT_ = reinterpret_cast<PFN_vkCmdPushDataEXT>(
            vkGetDeviceProcAddr(device_, "vkCmdPushDataEXT"));
        if (!vkGetPhysicalDeviceDescriptorSizeEXT_ || !vkWriteResourceDescriptorsEXT_ ||
            !vkCmdBindResourceHeapEXT_ || !vkCmdPushDataEXT_) {
            throw std::runtime_error("failed to load VK_EXT_descriptor_heap entry points");
        }
    }

    uint32_t findMemoryType(uint32_t filter, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties mem;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &mem);
        for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
            if ((filter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) return i;
        }
        throw std::runtime_error("no suitable memory type");
    }

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool deviceAddress) {
        Buffer b{};
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage | (deviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device_, &info, nullptr, &b.buffer));

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, b.buffer, &req);

        VkMemoryAllocateFlagsInfo flags{};
        flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        if (deviceAddress) alloc.pNext = &flags;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(device_, &alloc, nullptr, &b.memory));
        VK_CHECK(vkBindBufferMemory(device_, b.buffer, b.memory, 0));

        if (deviceAddress) {
            VkBufferDeviceAddressInfo addr{};
            addr.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            addr.buffer = b.buffer;
            b.address = vkGetBufferDeviceAddress(device_, &addr);
        }
        VK_CHECK(vkMapMemory(device_, b.memory, 0, size, 0, &b.mapped));
        return b;
    }

    void createTargets() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VkImageCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = VK_FORMAT_R8G8B8A8_UNORM;
            info.extent = {kExtent, kExtent, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VK_CHECK(vkCreateImage(device_, &info, nullptr, &images_[i]));

            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(device_, images_[i], &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex =
                findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK_CHECK(vkAllocateMemory(device_, &alloc, nullptr, &imageMemory_[i]));
            VK_CHECK(vkBindImageMemory(device_, images_[i], imageMemory_[i], 0));

            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = images_[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = VK_FORMAT_R8G8B8A8_UNORM;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(device_, &view, nullptr, &imageViews_[i]));

            readback_[i] = createBuffer(kExtent * kExtent * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        }
    }

    void createUniformBuffers() {
        const float colors[kHeapSlots][4] = {
            {1.0f, 0.0f, 0.0f, 1.0f},  // slot 0: red
            {0.0f, 1.0f, 0.0f, 1.0f},  // slot 1: green
        };
        // Slot 0: both colours in one buffer.
        palette_ = createBuffer(sizeof(float) * 8, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        std::memcpy(palette_.mapped, colors, sizeof(float) * 8);
        // Slot 1: green on its own.
        green_ = createBuffer(sizeof(float) * 4, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        std::memcpy(green_.mapped, colors[1], sizeof(float) * 4);
    }

    void createHeap() {
        descriptorSize_ =
            vkGetPhysicalDeviceDescriptorSizeEXT_(physicalDevice_, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        reservedOffset_ = alignUp(descriptorSize_ * kHeapSlots, heapProps_.resourceHeapAlignment);
        reservedSize_ =
            alignUp(heapProps_.minResourceHeapReservedRange, heapProps_.resourceHeapAlignment);

        heap_ = createBuffer(reservedOffset_ + reservedSize_, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT,
                             true);

        for (uint32_t i = 0; i < kHeapSlots; ++i) {
            VkDeviceAddressRangeEXT range{};
            range.address = (i == 0) ? palette_.address : green_.address;
            range.size = (i == 0) ? sizeof(float) * 8 : sizeof(float) * 4;

            VkResourceDescriptorInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT;
            info.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            info.data.pAddressRange = &range;

            VkHostAddressRangeEXT dest{};
            dest.address = static_cast<uint8_t*>(heap_.mapped) + i * descriptorSize_;
            dest.size = descriptorSize_;
            VK_CHECK(vkWriteResourceDescriptorsEXT_(device_, 1, &info, &dest));
        }
    }

    VkShaderModule loadModule(const std::string& name) {
        auto code = readFile(std::string(SHADER_DIR) + name);
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.size();
        info.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m;
        VK_CHECK(vkCreateShaderModule(device_, &info, nullptr, &m));
        return m;
    }

    void createPipelines() {
        slot0Pipeline_ = createPipeline("/frag_slot0.frag.spv");
        slot1Pipeline_ = createPipeline("/frag_slot1.frag.spv");
        slot0DynamicPipeline_ = createPipeline("/frag_slot0_dynamic.frag.spv");
    }

    VkPipeline createPipeline(const char* fragPath) {
        VkShaderModule vert = loadModule("/fullscreen.vert.spv");
        VkShaderModule frag = loadModule(fragPath);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{0, 0, float(kExtent), float(kExtent), 0, 1};
        VkRect2D scissor{{0, 0}, {kExtent, kExtent}};
        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.pViewports = &viewport;
        vp.scissorCount = 1;
        vp.pScissors = &scissor;

        VkPipelineRasterizationStateCreateInfo rast{};
        rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rast.polygonMode = VK_POLYGON_MODE_FILL;
        rast.cullMode = VK_CULL_MODE_NONE;
        rast.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;

        // Descriptor-heap pipelines require layout == VK_NULL_HANDLE, so the
        // flag has to travel via the chained flags2 struct.
        VkPipelineCreateFlags2CreateInfo flags2{};
        flags2.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
        flags2.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;
        rendering.pNext = &flags2;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rast;
        info.pMultisampleState = &ms;
        info.pColorBlendState = &blend;
        info.layout = VK_NULL_HANDLE;
        VkPipeline pipeline;
        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));

        vkDestroyShaderModule(device_, frag, nullptr);
        vkDestroyShaderModule(device_, vert, nullptr);
        return pipeline;
    }

    void createCommandsAndSync() {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_));

        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = commandPool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = kFramesInFlight;
        VK_CHECK(vkAllocateCommandBuffers(device_, &alloc, commandBuffers_));

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &fences_[i]));
        }
    }

    void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                      VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void recordFrame(uint32_t slot, uint32_t pushedIndex, VkPipeline pipeline, bool usePush) {
        VkCommandBuffer cmd = commandBuffers_[slot];
        VK_CHECK(vkResetCommandBuffer(cmd, 0));

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

        imageBarrier(cmd, images_[slot], VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = imageViews_[slot];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 1.0f, 1.0f}};  // blue = shader never ran

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, {kExtent, kExtent}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;

        vkCmdBeginRendering(cmd, &rendering);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        VkBindHeapInfoEXT bind{};
        bind.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT;
        bind.heapRange.address = heap_.address;
        bind.heapRange.size = reservedOffset_ + reservedSize_;
        bind.reservedRangeOffset = reservedOffset_;
        bind.reservedRangeSize = reservedSize_;
        vkCmdBindResourceHeapEXT_(cmd, &bind);

        // The value under test. Stored in a member so its lifetime extends
        // past this call regardless of when the driver reads the host pointer.
        if (usePush) {
            pushValues_[slot] = pushedIndex;
            VkPushDataInfoEXT push{};
            push.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT;
            push.offset = 0;
            push.data.address = &pushValues_[slot];
            push.data.size = sizeof(uint32_t);
            vkCmdPushDataEXT_(cmd, &push);
        }

        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        imageBarrier(cmd, images_[slot], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kExtent, kExtent, 1};
        vkCmdCopyImageToBuffer(cmd, images_[slot], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_[slot].buffer, 1, &copy);

        VK_CHECK(vkEndCommandBuffer(cmd));
    }

    Rgba centerPixel(uint32_t slot) {
        const uint8_t* p = static_cast<const uint8_t*>(readback_[slot].mapped);
        size_t offset = (size_t(kExtent / 2) * kExtent + kExtent / 2) * 4;
        return Rgba{p[offset], p[offset + 1], p[offset + 2], p[offset + 3]};
    }

    uint32_t renderFrames(const char* label, VkPipeline pipeline, bool usePush,
                          int32_t forceExpectedIndex = -1) {
        const Rgba expectedForIndex[2] = {
            Rgba{255, 0, 0, 255},  // slot 0: red
            Rgba{0, 255, 0, 255},  // slot 1: green
        };

        std::cout << label << "\n";
        uint32_t failures = 0;
        bool pending[kFramesInFlight] = {false, false};
        uint32_t pendingFrame[kFramesInFlight] = {0, 0};
        uint32_t pendingIndex[kFramesInFlight] = {0, 0};

        auto verify = [&](uint32_t slot) {
            Rgba got = centerPixel(slot);
            uint32_t wantIdx = (forceExpectedIndex >= 0) ? uint32_t(forceExpectedIndex)
                                                         : pendingIndex[slot];
            Rgba want = expectedForIndex[wantIdx];
            if (!(got == want)) {
                ++failures;
                if (failures <= 6) {
                    std::cout << "frame " << pendingFrame[slot] << ": pushed index "
                              << pendingIndex[slot] << ", expected " << want.str() << ", got "
                              << got.str() << "\n";
                }
            }
        };

        for (uint32_t frame = 0; frame < kFrameCount; ++frame) {
            uint32_t slot = frame % kFramesInFlight;
            VK_CHECK(vkWaitForFences(device_, 1, &fences_[slot], VK_TRUE, UINT64_MAX));
            if (pending[slot]) verify(slot);
            VK_CHECK(vkResetFences(device_, 1, &fences_[slot]));

            uint32_t pushedIndex = frame % 2;
            recordFrame(slot, pushedIndex, pipeline, usePush);

            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &commandBuffers_[slot];
            VK_CHECK(vkQueueSubmit(queue_, 1, &submit, fences_[slot]));

            pending[slot] = true;
            pendingFrame[slot] = frame;
            pendingIndex[slot] = pushedIndex;
        }

        VK_CHECK(vkDeviceWaitIdle(device_));
        for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
            if (pending[slot]) verify(slot);
        }
        if (failures > 6) std::cout << "... and " << (failures - 6) << " more\n";
        std::cout << "  -> " << (kFrameCount - failures) << "/" << kFrameCount << " frames correct\n\n";
        return failures;
    }

    void destroyBuffer(Buffer& b) {
        if (b.mapped) vkUnmapMemory(device_, b.memory);
        if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
        b = Buffer{};
    }

    void cleanup() {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            vkDestroyFence(device_, fences_[i], nullptr);
            vkDestroyImageView(device_, imageViews_[i], nullptr);
            vkDestroyImage(device_, images_[i], nullptr);
            vkFreeMemory(device_, imageMemory_[i], nullptr);
            destroyBuffer(readback_[i]);
        }
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyPipeline(device_, slot0Pipeline_, nullptr);
        vkDestroyPipeline(device_, slot1Pipeline_, nullptr);
        vkDestroyPipeline(device_, slot0DynamicPipeline_, nullptr);
        destroyBuffer(heap_);
        destroyBuffer(palette_);
        destroyBuffer(green_);
        vkDestroyDevice(device_, nullptr);
        vkDestroyInstance(instance_, nullptr);
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProps_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;

    VkImage images_[kFramesInFlight]{};
    VkDeviceMemory imageMemory_[kFramesInFlight]{};
    VkImageView imageViews_[kFramesInFlight]{};
    Buffer readback_[kFramesInFlight];
    Buffer palette_;
    Buffer green_;
    Buffer heap_;
    VkDeviceSize descriptorSize_ = 0;
    VkDeviceSize reservedOffset_ = 0;
    VkDeviceSize reservedSize_ = 0;

    VkPipeline slot0Pipeline_ = VK_NULL_HANDLE;
    VkPipeline slot1Pipeline_ = VK_NULL_HANDLE;
    VkPipeline slot0DynamicPipeline_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers_[kFramesInFlight]{};
    VkFence fences_[kFramesInFlight]{};
    uint32_t pushValues_[kFramesInFlight]{};

    PFN_vkGetPhysicalDeviceDescriptorSizeEXT vkGetPhysicalDeviceDescriptorSizeEXT_ = nullptr;
    PFN_vkWriteResourceDescriptorsEXT vkWriteResourceDescriptorsEXT_ = nullptr;
    PFN_vkCmdBindResourceHeapEXT vkCmdBindResourceHeapEXT_ = nullptr;
    PFN_vkCmdPushDataEXT vkCmdPushDataEXT_ = nullptr;
};

}  // namespace

int runGraphicsRepro() {
    try {
        Repro repro;
        return repro.run();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
