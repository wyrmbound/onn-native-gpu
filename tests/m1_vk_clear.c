/* M1: Vulkan clear-and-readback through a dlopen'ed PanVK ICD on kbase.
 * No Vulkan loader, no WSI. Pass = every pixel of a 64x64 RGBA8 image reads
 * back as {64,128,191,255} after vkCmdClearColorImage(0.25,0.5,0.75,1).
 *
 * Build (NDK, dynamic bionic; run on the box via a bundled linker64):
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang \
 *     -O1 -DVK_NO_PROTOTYPES m1_vk_clear.c -o m1 -ldl
 * Run:
 *   LD_LIBRARY_PATH=/data/local/tmp/pb ./linker64 /data/local/tmp/pb/m1 \
 *     /data/local/tmp/pb/libvulkan_panfrost.so
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define W 64
#define H 64

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name); if (!name) die("missing " #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name); if (!name) die("missing " #name)

static void die(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }
static void ck(VkResult r, const char *m) { if (r != VK_SUCCESS) { fprintf(stderr, "FAIL: %s -> %d\n", m, r); exit(1); } }

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    die("no suitable memory type");
    return 0;
}

int main(int argc, char **argv)
{
    const char *icd = argc > 1 ? argv[1] : "libvulkan_panfrost.so";
    void *h = dlopen(icd, RTLD_NOW);
    if (!h) { fprintf(stderr, "FAIL: dlopen %s: %s\n", icd, dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) die("no vk_icdGetInstanceProcAddr export");

    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) die("vkCreateInstance");
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "m1", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    ck(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties);
    IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
    IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr);
    gdpa = vkGetDeviceProcAddr;

    uint32_t n = 1; VkPhysicalDevice pd;
    VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) die("no physical device (kbase open/handshake or unknown gpu_id?)");
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s  api %u.%u.%u  driver 0x%x  vendor 0x%x devid 0x%x\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
           props.driverVersion, props.vendorID, props.deviceID);

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn, sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    uint32_t qi = qn;
    for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qi = i; break; }
    if (qi == qn) for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT) { qi = i; break; }
    if (qi == qn) die("no graphics/transfer queue");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; ck(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice");

    DFN(vkGetDeviceQueue); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory);
    DFN(vkBindImageMemory); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkBindBufferMemory);
    DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdPipelineBarrier);
    DFN(vkCmdClearColorImage); DFN(vkCmdCopyImageToBuffer); DFN(vkEndCommandBuffer); DFN(vkQueueSubmit);
    DFN(vkQueueWaitIdle); DFN(vkMapMemory); DFN(vkDeviceWaitIdle);

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkQueue q; vkGetDeviceQueue(dev, qi, 0, &q);

    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; ck(vkCreateImage(dev, &ic, NULL, &img), "vkCreateImage");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory imem; ck(vkAllocateMemory(dev, &mai, NULL, &imem), "alloc image mem (kbase MEM_ALLOC)");
    ck(vkBindImageMemory(dev, img, imem, 0), "bind image");

    VkDeviceSize bsz = W * H * 4;
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bsz,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf; ck(vkCreateBuffer(dev, &bc, NULL, &buf), "vkCreateBuffer");
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem; ck(vkAllocateMemory(dev, &mai, NULL, &bmem), "alloc buffer mem");
    ck(vkBindBufferMemory(dev, buf, bmem, 0), "bind buffer");

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qi };
    VkCommandPool pool; ck(vkCreateCommandPool(dev, &pci, NULL, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; ck(vkAllocateCommandBuffers(dev, &cai, &cb), "alloc cmdbuf");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    ck(vkBeginCommandBuffer(cb, &bi), "begin");

    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = rng };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);

    VkClearColorValue col = { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } };
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);

    VkImageMemoryBarrier b2 = b1;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);

    VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);

    VkBufferMemoryBarrier b3 = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = buf, .size = VK_WHOLE_SIZE };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &b3, 0, NULL);
    ck(vkEndCommandBuffer(cb), "end");

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    ck(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit (CSF queue kick)");
    ck(vkQueueWaitIdle(q), "vkQueueWaitIdle");
    ck(vkDeviceWaitIdle(dev), "vkDeviceWaitIdle");

    unsigned char *px; ck(vkMapMemory(dev, bmem, 0, bsz, 0, (void **)&px), "vkMapMemory");
    const unsigned char want[4] = { 64, 128, 191, 255 };
    unsigned bad = 0;
    for (unsigned i = 0; i < W * H; i++)
        for (unsigned c = 0; c < 4; c++)
            if (abs((int)px[i * 4 + c] - (int)want[c]) > 1) { if (!bad) fprintf(stderr, "first bad pixel %u: got %u %u %u %u\n", i, px[i*4], px[i*4+1], px[i*4+2], px[i*4+3]); bad++; break; }
    printf("pixel[0] = %u %u %u %u  (want 64 128 191 255)\n", px[0], px[1], px[2], px[3]);
    printf(bad ? "M1 FAIL: %u/%u pixels wrong\n" : "M1 PASS: %u/%u pixels wrong\n", bad, W * H);
    return bad ? 2 : 0;
}
