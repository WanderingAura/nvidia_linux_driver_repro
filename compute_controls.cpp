// CONTROL CASES for the VK_EXT_descriptor_heap repro. These are all
// expected to PASS on every driver, including the affected NVIDIA one --
// they exist to rule out innocent explanations for the failure that
// graphics_repro.cpp demonstrates. See README.md.
//
// Headless compute only: no window, no swapchain, no surface, no graphics
// pipeline. It writes six storage-buffer descriptors into a descriptor heap
// and runs two compute shaders that differ in exactly one respect:
//
//   heap_constant.comp - indexes the heap with literal constants
//   heap_dynamic.comp  - indexes the heap with a value loaded from memory
//                        (uniform across invocations, no nonuniformEXT)
//
// Both must produce identical output. Each prints PASS or FAIL with the
// expected and actual values, and the process exits non-zero on any failure.
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

constexpr uint32_t kInputCount = 4;
constexpr uint32_t kOutputSlot = 4;
constexpr uint32_t kControlSlot = 5;
constexpr uint32_t kHeapSlots = 6;

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

class Repro {
public:
    int run() {
        createInstance();
        pickPhysicalDevice();
        createDevice();
        loadFunctions();
        createBuffers();
        createHeap();
        createCommandPool();

        bool constantOk = runCase("constant index        ", "/heap_constant.comp.spv");
        bool dynamicOk = runCase("dynamic index (buffer)", "/heap_dynamic.comp.spv");
        bool pushOk = runCase("pushdata index = 0     ", "/heap_pushdata.comp.spv", true, 0);
        bool pushNonZeroOk = runCase("pushdata index = 1     ",
                                     "/heap_pushdata_nonzero.comp.spv", true, 1, 1);

        std::cout << "\n";
        if (constantOk && dynamicOk && pushOk && pushNonZeroOk) {
            std::cout << "RESULT: all cases passed.\n";
        } else if (constantOk && dynamicOk && pushOk && !pushNonZeroOk) {
            std::cout << "RESULT: push-data index 0 works but NON-ZERO push-data index is BROKEN\n"
                      << "        in compute as well as fragment.\n";
        } else if (constantOk && dynamicOk && !pushOk) {
            std::cout << "RESULT: heap indexing works; vkCmdPushDataEXT IS BROKEN.\n";
        } else if (constantOk && !dynamicOk) {
            std::cout << "RESULT: constant indexing works, DYNAMIC INDEXING IS BROKEN.\n";
        } else {
            std::cout << "RESULT: failures present (see above).\n";
        }

        cleanup();
        return (constantOk && dynamicOk && pushOk && pushNonZeroOk) ? 0 : 1;
    }

private:
    void createInstance() {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "descriptor_heap_repro";
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

        bool hasHeap = false, hasUntyped = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME) == 0) hasHeap = true;
            if (std::strcmp(e.extensionName, VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME) == 0)
                hasUntyped = true;
        }
        return hasHeap && hasUntyped;
    }

    void pickPhysicalDevice() {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());

        for (VkPhysicalDevice device : devices) {
            if (deviceSupportsHeaps(device)) {
                physicalDevice_ = device;
                break;
            }
        }
        if (physicalDevice_ == VK_NULL_HANDLE) {
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
        float priority = 1.0f;
        uint32_t queueFamily = 0;
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());
        for (uint32_t i = 0; i < familyCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                queueFamily = i;
                break;
            }
        }
        queueFamily_ = queueFamily;

        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untyped{};
        untyped.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR;
        untyped.shaderUntypedPointers = VK_TRUE;

        VkPhysicalDeviceDescriptorHeapFeaturesEXT heap{};
        heap.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
        heap.descriptorHeap = VK_TRUE;
        heap.pNext = &untyped;

        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.bufferDeviceAddress = VK_TRUE;
        features12.pNext = &heap;

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
        vkGetDeviceQueue(device_, queueFamily, 0, &queue_);
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
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((filter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props) {
                return i;
            }
        }
        throw std::runtime_error("no suitable memory type");
    }

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer b{};
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device_, &info, nullptr, &b.buffer));

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, b.buffer, &req);

        VkMemoryAllocateFlagsInfo flags{};
        flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.pNext = &flags;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(device_, &alloc, nullptr, &b.memory));
        VK_CHECK(vkBindBufferMemory(device_, b.buffer, b.memory, 0));

        VkBufferDeviceAddressInfo addr{};
        addr.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addr.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(device_, &addr);

        VK_CHECK(vkMapMemory(device_, b.memory, 0, size, 0, &b.mapped));
        return b;
    }

    void createBuffers() {
        for (uint32_t i = 0; i < kInputCount; ++i) {
            inputs_[i] = createBuffer(sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            *static_cast<uint32_t*>(inputs_[i].mapped) = 1000 + i;
        }
        output_ = createBuffer(sizeof(uint32_t) * kInputCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        control_ = createBuffer(sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        *static_cast<uint32_t*>(control_.mapped) = 0;
    }

    void createHeap() {
        descriptorSize_ =
            vkGetPhysicalDeviceDescriptorSizeEXT_(physicalDevice_, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

        reservedOffset_ = alignUp(descriptorSize_ * kHeapSlots, heapProps_.resourceHeapAlignment);
        reservedSize_ =
            alignUp(heapProps_.minResourceHeapReservedRange, heapProps_.resourceHeapAlignment);

        heap_ = createBuffer(reservedOffset_ + reservedSize_, VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT);

        std::cout << "storage buffer descriptor size: " << descriptorSize_ << " bytes\n"
                  << "heap reserved range:            " << reservedSize_ << " bytes at offset "
                  << reservedOffset_ << "\n\n";

        const Buffer* slots[kHeapSlots] = {&inputs_[0], &inputs_[1], &inputs_[2],
                                           &inputs_[3], &output_,   &control_};
        const VkDeviceSize sizes[kHeapSlots] = {sizeof(uint32_t),
                                                sizeof(uint32_t),
                                                sizeof(uint32_t),
                                                sizeof(uint32_t),
                                                sizeof(uint32_t) * kInputCount,
                                                sizeof(uint32_t)};

        for (uint32_t i = 0; i < kHeapSlots; ++i) {
            VkDeviceAddressRangeEXT range{};
            range.address = slots[i]->address;
            range.size = sizes[i];

            VkResourceDescriptorInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT;
            info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            info.data.pAddressRange = &range;

            VkHostAddressRangeEXT dest{};
            dest.address = static_cast<uint8_t*>(heap_.mapped) + i * descriptorSize_;
            dest.size = descriptorSize_;

            VK_CHECK(vkWriteResourceDescriptorsEXT_(device_, 1, &info, &dest));
        }
    }

    void createCommandPool() {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        info.queueFamilyIndex = queueFamily_;
        VK_CHECK(vkCreateCommandPool(device_, &info, nullptr, &commandPool_));
    }

    VkPipeline createComputePipeline(const std::string& spvPath) {
        auto code = readFile(std::string(SHADER_DIR) + spvPath);

        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = code.size();
        moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule module;
        VK_CHECK(vkCreateShaderModule(device_, &moduleInfo, nullptr, &module));

        // VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT requires layout == VK_NULL_HANDLE.
        VkPipelineCreateFlags2CreateInfo flags2{};
        flags2.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
        flags2.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.pNext = &flags2;
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = VK_NULL_HANDLE;

        VkPipeline pipeline;
        VK_CHECK(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
        vkDestroyShaderModule(device_, module, nullptr);
        return pipeline;
    }

    bool runCase(const std::string& label, const std::string& spvPath,
                 bool usePushData = false, uint32_t pushValue = 0,
                 int32_t expectAllFromSlot = -1) {
        std::memset(output_.mapped, 0, sizeof(uint32_t) * kInputCount);

        VkPipeline pipeline = createComputePipeline(spvPath);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool_;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer cmd;
        VK_CHECK(vkAllocateCommandBuffers(device_, &allocInfo, &cmd));

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

        VkBindHeapInfoEXT bind{};
        bind.sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT;
        bind.heapRange.address = heap_.address;
        bind.heapRange.size = reservedOffset_ + reservedSize_;
        bind.reservedRangeOffset = reservedOffset_;
        bind.reservedRangeSize = reservedSize_;
        vkCmdBindResourceHeapEXT_(cmd, &bind);

        // Same call order as the real renderer: bind heap, then push data.
        uint32_t pushBase = pushValue;
        if (usePushData) {
            VkPushDataInfoEXT push{};
            push.sType = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT;
            push.offset = 0;
            push.data.address = &pushBase;
            push.data.size = sizeof(pushBase);
            vkCmdPushDataEXT_(cmd, &push);
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdDispatch(cmd, 1, 1, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(queue_));

        const uint32_t* results = static_cast<const uint32_t*>(output_.mapped);
        bool ok = true;
        std::string detail;
        for (uint32_t i = 0; i < kInputCount; ++i) {
            uint32_t expected =
                (expectAllFromSlot >= 0) ? 1000 + uint32_t(expectAllFromSlot) : 1000 + i;
            if (results[i] != expected) {
                ok = false;
                detail += "\n    slot " + std::to_string(i) + ": expected " +
                          std::to_string(expected) + ", got " + std::to_string(results[i]);
            }
        }

        std::cout << label << " : " << (ok ? "PASS" : "FAIL") << detail << "\n";

        vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
        vkDestroyPipeline(device_, pipeline, nullptr);
        return ok;
    }

    void destroyBuffer(Buffer& b) {
        if (b.mapped) vkUnmapMemory(device_, b.memory);
        if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
        b = Buffer{};
    }

    void cleanup() {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        destroyBuffer(heap_);
        destroyBuffer(control_);
        destroyBuffer(output_);
        for (auto& b : inputs_) destroyBuffer(b);
        vkDestroyDevice(device_, nullptr);
        vkDestroyInstance(instance_, nullptr);
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProps_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;

    Buffer inputs_[kInputCount];
    Buffer output_;
    Buffer control_;
    Buffer heap_;
    VkDeviceSize descriptorSize_ = 0;
    VkDeviceSize reservedOffset_ = 0;
    VkDeviceSize reservedSize_ = 0;

    PFN_vkGetPhysicalDeviceDescriptorSizeEXT vkGetPhysicalDeviceDescriptorSizeEXT_ = nullptr;
    PFN_vkWriteResourceDescriptorsEXT vkWriteResourceDescriptorsEXT_ = nullptr;
    PFN_vkCmdBindResourceHeapEXT vkCmdBindResourceHeapEXT_ = nullptr;
    PFN_vkCmdPushDataEXT vkCmdPushDataEXT_ = nullptr;
};

}  // namespace

int runComputeControls() {
    try {
        Repro repro;
        return repro.run();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
