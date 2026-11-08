/* wsitest: an ordinary Vulkan WSI app (instance, device, Android surface, swapchain) against the shim + onnm4_wsi.
 * Each swapchain image is cleared to its own color (0 red, 1 green, 2 blue, pulsing), with a white band across
 * the TOP 72 rows. The WSI tags frames M4RV (rows top-down), so ZeroCopyActivity skips its GL flip: on the TV the
 * white band must appear at the TOP. Band at the bottom = the app still flips M4RV frames (old OnnShell build).
 * Run on the box (ZeroCopyActivity started with --ez launch false):
 *   cd /data/local/tmp/pb && ONNM4_WSI_DEBUG=1 LD_LIBRARY_PATH=$PWD ./linker64 $PWD/wsitest $PWD/libvulkan.so.wsi
 * The shim loads the ICD from $ONN_VK_ICD or libvulkan_panfrost.so on LD_LIBRARY_PATH.
 * Env: WSI_FRAMES (default 300). Exit 0 + "WSITEST PASS" on success.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#ifndef VK_KHR_android_surface
typedef struct VkAndroidSurfaceCreateInfoKHR { VkStructureType sType; const void *pNext; VkFlags flags; void *window; } VkAndroidSurfaceCreateInfoKHR;
#endif
typedef VkResult (VKAPI_PTR *PFN_CreateAndroidSurface)(VkInstance, const VkAndroidSurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *);
#define STYPE_ANDROID_SURFACE ((VkStructureType)1000008000)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name); if (!name) die("missing " #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name); if (!name) die("missing " #name)
static void die(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }
static void ck(VkResult r, const char *m) { if (r != VK_SUCCESS) { fprintf(stderr, "FAIL: %s -> %d\n", m, r); exit(1); } }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

#define NIMG 8
#define BAND 72

int main(int argc, char **argv)
{
    const char *lib = argc > 1 ? argv[1] : "libvulkan.so.wsi";
    void *h = dlopen(lib, RTLD_NOW);
    if (!h) { fprintf(stderr, "FAIL: dlopen %s: %s\n", lib, dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    if (!gipa) die("no vkGetInstanceProcAddr export");
    int frames = getenv("WSI_FRAMES") ? atoi(getenv("WSI_FRAMES")) : 300;

    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkEnumerateInstanceExtensionProperties enumInst = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!enumInst || !vkCreateInstance) die("global entry points");
    uint32_t ne = 0; enumInst(NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep); enumInst(NULL, &ne, ep);
    int surf_ok = 0, asurf_ok = 0;
    for (uint32_t i = 0; i < ne; i++) {
        surf_ok |= !strcmp(ep[i].extensionName, "VK_KHR_surface");
        asurf_ok |= !strcmp(ep[i].extensionName, "VK_KHR_android_surface");
    }
    printf("instance extensions: %u (VK_KHR_surface %s, VK_KHR_android_surface %s)\n", ne, surf_ok ? "yes" : "NO", asurf_ok ? "yes" : "NO");
    if (!surf_ok || !asurf_ok) die("WSI instance extensions not reported");

    const char *iexts[] = { "VK_KHR_surface", "VK_KHR_android_surface" };
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "wsitest", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts };
    ck(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance (with WSI extensions)");

    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkGetPhysicalDeviceMemoryProperties); IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr); IFN(vkEnumerateDeviceExtensionProperties);
    IFN(vkGetPhysicalDeviceSurfaceSupportKHR); IFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); IFN(vkGetPhysicalDeviceSurfaceFormatsKHR);
    IFN(vkGetPhysicalDeviceSurfacePresentModesKHR); IFN(vkDestroySurfaceKHR); IFN(vkDestroyInstance);
    PFN_CreateAndroidSurface vkCreateAndroidSurfaceKHR = (PFN_CreateAndroidSurface)gipa(inst, "vkCreateAndroidSurfaceKHR");
    if (!vkCreateAndroidSurfaceKHR) die("missing vkCreateAndroidSurfaceKHR");
    gdpa = vkGetDeviceProcAddr;

    uint32_t n = 1; VkPhysicalDevice pd;
    VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) die("no physical device");
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);

    uint32_t nd = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &nd, NULL);
    VkExtensionProperties *dp = calloc(nd, sizeof *dp); vkEnumerateDeviceExtensionProperties(pd, NULL, &nd, dp);
    int sw_ok = 0; for (uint32_t i = 0; i < nd; i++) sw_ok |= !strcmp(dp[i].extensionName, "VK_KHR_swapchain");
    if (!sw_ok) die("VK_KHR_swapchain not reported");

    /* Surface first: on this path it is what waits for the app's buffers. */
    VkAndroidSurfaceCreateInfoKHR sci = { .sType = STYPE_ANDROID_SURFACE };
    VkSurfaceKHR surf; ck(vkCreateAndroidSurfaceKHR(inst, &sci, NULL, &surf), "vkCreateAndroidSurfaceKHR");

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn, sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    uint32_t qi = qn;
    for (uint32_t i = 0; i < qn; i++) {
        VkBool32 sup = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &sup);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && sup) { qi = i; break; }
    }
    if (qi == qn) die("no graphics+present queue");

    const char *dexts[] = { "VK_KHR_swapchain" };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts };
    VkDevice dev; ck(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice (with VK_KHR_swapchain)");

    DFN(vkGetDeviceQueue); DFN(vkCreateSwapchainKHR); DFN(vkGetSwapchainImagesKHR); DFN(vkAcquireNextImageKHR); DFN(vkQueuePresentKHR);
    DFN(vkDestroySwapchainKHR); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer);
    DFN(vkCmdPipelineBarrier); DFN(vkCmdClearColorImage); DFN(vkCmdCopyBufferToImage); DFN(vkQueueSubmit); DFN(vkCreateSemaphore);
    DFN(vkCreateFence); DFN(vkWaitForFences); DFN(vkResetFences); DFN(vkDeviceWaitIdle); DFN(vkCreateBuffer);
    DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindBufferMemory); DFN(vkMapMemory); DFN(vkDestroyDevice);
    VkQueue q; vkGetDeviceQueue(dev, qi, 0, &q);

    VkSurfaceCapabilitiesKHR caps; ck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps), "surface caps");
    printf("surface: %ux%u, images %u..%u, usage 0x%x\n", caps.currentExtent.width, caps.currentExtent.height, caps.minImageCount, caps.maxImageCount, caps.supportedUsageFlags);
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL);
    VkSurfaceFormatKHR *sf = calloc(nf, sizeof *sf); vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, sf);
    for (uint32_t i = 0; i < nf; i++) printf("format %d colorspace %d\n", sf[i].format, sf[i].colorSpace);
    uint32_t nm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, NULL);
    VkPresentModeKHR pm[8]; if (nm > 8) nm = 8; vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, pm);
    if (nm < 1 || pm[0] != VK_PRESENT_MODE_FIFO_KHR) die("FIFO not offered first");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) die("TRANSFER_DST not supported (test clears by transfer)");
    uint32_t W = caps.currentExtent.width, H = caps.currentExtent.height;

    VkSwapchainCreateInfoKHR swci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = caps.minImageCount,
        .imageFormat = sf[0].format, .imageColorSpace = sf[0].colorSpace, .imageExtent = caps.currentExtent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    VkSwapchainKHR sc; ck(vkCreateSwapchainKHR(dev, &swci, NULL, &sc), "vkCreateSwapchainKHR");
    uint32_t ni = 0; vkGetSwapchainImagesKHR(dev, sc, &ni, NULL);
    if (ni == 0 || ni > NIMG) die("bad image count");
    VkImage img[NIMG]; ck(vkGetSwapchainImagesKHR(dev, sc, &ni, img), "vkGetSwapchainImagesKHR");
    printf("swapchain: %u images %ux%u format %d\n", ni, W, H, swci.imageFormat);

    /* White band source: a host-visible buffer of BAND rows. */
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)W * BAND * 4, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VkBuffer band; ck(vkCreateBuffer(dev, &bci, NULL, &band), "vkCreateBuffer");
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, band, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = 32, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) { mt = t; break; }
    if (mt == 32) die("no host-visible memory");
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    VkDeviceMemory bmem; ck(vkAllocateMemory(dev, &mai, NULL, &bmem), "alloc band");
    ck(vkBindBufferMemory(dev, band, bmem, 0), "bind band");
    void *ptr; ck(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &ptr), "map band");
    memset(ptr, 0xff, bci.size);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qi };
    VkCommandPool pool; ck(vkCreateCommandPool(dev, &pci, NULL, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = ni };
    VkCommandBuffer cmd[NIMG]; ck(vkAllocateCommandBuffers(dev, &cai, cmd), "alloc cmdbufs");
    VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VkSemaphore acq[NIMG + 1], done[NIMG]; VkFence fence[NIMG];
    for (uint32_t i = 0; i <= ni; i++) ck(vkCreateSemaphore(dev, &semci, NULL, &acq[i]), "semaphore");
    for (uint32_t i = 0; i < ni; i++) { ck(vkCreateSemaphore(dev, &semci, NULL, &done[i]), "semaphore"); ck(vkCreateFence(dev, &fci, NULL, &fence[i]), "fence"); }

    double t0 = now_s();
    for (int f = 0; f < frames; f++) {
        VkSemaphore a = acq[f % (ni + 1)];   /* one more than images: never reuse one still pending */
        uint32_t i;
        ck(vkAcquireNextImageKHR(dev, sc, UINT64_MAX, a, VK_NULL_HANDLE, &i), "vkAcquireNextImageKHR");
        ck(vkWaitForFences(dev, 1, &fence[i], VK_TRUE, UINT64_MAX), "wait fence"); ck(vkResetFences(dev, 1, &fence[i]), "reset fence");

        float pulse = 0.4f + 0.6f * (float)((f % 60) / 59.0);
        VkClearColorValue c = { .float32 = { i % 3 == 0 ? pulse : 0, i % 3 == 1 ? pulse : 0, i % 3 == 2 ? pulse : 0, 1 } };
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        ck(vkBeginCommandBuffer(cmd[i], &bi), "begin");
        VkImageMemoryBarrier toDst = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img[i], .subresourceRange = rng };
        vkCmdPipelineBarrier(cmd[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst);
        vkCmdClearColorImage(cmd[i], img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rng);
        VkImageMemoryBarrier waw = toDst;
        waw.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; waw.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(cmd[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &waw);
        VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, BAND, 1 } };
        vkCmdCopyBufferToImage(cmd[i], band, img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
        VkImageMemoryBarrier toPresent = waw;
        toPresent.dstAccessMask = 0; toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &toPresent);
        ck(vkEndCommandBuffer(cmd[i]), "end");

        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &a, .pWaitDstStageMask = &ws,
            .commandBufferCount = 1, .pCommandBuffers = &cmd[i], .signalSemaphoreCount = 1, .pSignalSemaphores = &done[i] };
        ck(vkQueueSubmit(q, 1, &si, fence[i]), "vkQueueSubmit");
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done[i],
            .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &i };
        ck(vkQueuePresentKHR(q, &pi), "vkQueuePresentKHR");
        if (f == 0) printf("first frame presented (image %u)\n", i);
    }
    double dt = now_s() - t0;
    vkDeviceWaitIdle(dev);
    vkDestroySwapchainKHR(dev, sc, NULL);
    printf("%d frames via VK_KHR_swapchain, %ux%u, %.3f s = %.1f fps\n", frames, W, H, dt, frames / dt);
    printf("WSITEST PASS\n");
    return 0;
}
