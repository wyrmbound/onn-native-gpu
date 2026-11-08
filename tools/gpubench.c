/* keepalive: the H2 probe (GPU idle power collapse). Keeps the Mali busy with tiny jobs so it never goes idle
 * between a game's frames. One 64x64 image, one command buffer that only clears it (render pass loadOp CLEAR,
 * no shaders), submitted and waited on in a loop with a sleep between. No present, no readback.
 * If a game that freezes the box survives with this running next to it, idle power collapse is the cause, and
 * this is also the workaround (ZeroCopyActivity would start it for the duration of a game).
 *
 * Env: KEEPALIVE_US  sleep between submits in microseconds (default 5000 = about 200 Hz)
 *      PB_SECS       stop after this many seconds (default 0 = until SIGTERM/SIGINT)
 * Build (same shim as m2): aarch64-linux-android29-clang -O1 -DVK_NO_PROTOTYPES keepalive.c -o keepalive -ldl
 * Run:   LD_LIBRARY_PATH=/data/local/tmp/pb ./linker64 /data/local/tmp/pb/keepalive /data/local/tmp/pb/libvulkan_panfrost.so
 * Output: one line per minute with the submit count, plus a final line. Exit 0 on a clean stop, 1 on any error.
 */
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

static uint32_t W = 1920;
static uint32_t H = 1080;

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name); if (!name) die("missing " #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name); if (!name) die("missing " #name)

static void die(const char *m) { fprintf(stderr, "keepalive FAIL: %s\n", m); exit(1); }
static void ck(VkResult r, const char *m) { if (r != VK_SUCCESS) { fprintf(stderr, "keepalive FAIL: %s -> %d\n", m, r); exit(1); } }

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    die("no suitable memory type");
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0); /* one line at a time, so a log tail survives a hang */
    long us = getenv("KEEPALIVE_US") ? strtol(getenv("KEEPALIVE_US"), NULL, 0) : 0; /* gpubench: no sleep, GPU-bound */
    if (getenv("BENCH_W")) W = strtoul(getenv("BENCH_W"), NULL, 0);
    if (getenv("BENCH_H")) H = strtoul(getenv("BENCH_H"), NULL, 0);
    double secs = getenv("PB_SECS") ? strtod(getenv("PB_SECS"), NULL) : 0;
    if (us < 0) us = 0;
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal);

    const char *icd = argc > 1 ? argv[1] : "libvulkan_panfrost.so";
    void *h = dlopen(icd, RTLD_NOW);
    if (!h) { fprintf(stderr, "keepalive FAIL: dlopen %s: %s\n", icd, dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) die("no vk_icdGetInstanceProcAddr export");

    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) die("vkCreateInstance");
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "keepalive", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    ck(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkGetPhysicalDeviceMemoryProperties); IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr); IFN(vkDestroyInstance);
    gdpa = vkGetDeviceProcAddr; /* fetched after vkCreateInstance: a NULL-instance lookup only serves global commands */

    uint32_t n = 1; VkPhysicalDevice pd;
    VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) die("no physical device");

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn, sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    uint32_t qi = qn;
    for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qi = i; break; }
    free(qf);
    if (qi == qn) die("no graphics queue");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; ck(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice");

    DFN(vkGetDeviceQueue); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory);
    DFN(vkBindImageMemory); DFN(vkCreateImageView); DFN(vkCreateRenderPass); DFN(vkCreateFramebuffer);
    DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdBeginRenderPass);
    DFN(vkCmdEndRenderPass); DFN(vkEndCommandBuffer); DFN(vkQueueSubmit); DFN(vkQueueWaitIdle); DFN(vkDeviceWaitIdle);
    DFN(vkDestroyDevice);

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkQueue q; vkGetDeviceQueue(dev, qi, 0, &q);

    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; ck(vkCreateImage(dev, &ic, NULL, &img), "vkCreateImage");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory imem; ck(vkAllocateMemory(dev, &mai, NULL, &imem), "alloc image mem");
    ck(vkBindImageMemory(dev, img, imem, 0), "bind image");
    VkImageViewCreateInfo ivc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view; ck(vkCreateImageView(dev, &ivc, NULL, &view), "vkCreateImageView");

    /* Clear-only pass: the contents are thrown away every time (UNDEFINED in), so one recording serves every submit */
    VkAttachmentDescription att = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference aref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &aref };
    VkRenderPassCreateInfo rpc = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub };
    VkRenderPass rp; ck(vkCreateRenderPass(dev, &rpc, NULL, &rp), "vkCreateRenderPass");
    VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
        .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
    VkFramebuffer fb; ck(vkCreateFramebuffer(dev, &fbc, NULL, &fb), "vkCreateFramebuffer");

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qi };
    VkCommandPool pool; ck(vkCreateCommandPool(dev, &pci, NULL, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; ck(vkAllocateCommandBuffers(dev, &cai, &cmd), "alloc cmdbuf");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; /* reusable: no ONE_TIME_SUBMIT */
    ck(vkBeginCommandBuffer(cmd, &bi), "begin");
    VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 0 } } };
    VkRenderPassBeginInfo rpb = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &clear };
    /* gpubench: 16 full-image clear passes per submit, so GPU time dominates the per-submit overhead */
    for (int k = 0; k < 16; k++) { vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE); vkCmdEndRenderPass(cmd); }
    ck(vkEndCommandBuffer(cmd), "end");

    printf("keepalive: start, %ld us between submits, %s\n", us, secs > 0 ? "timed" : "until SIGTERM");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    unsigned long long count = 0;
    double t0 = now_s(), next = t0 + 5; unsigned long long last = 0;
    while (!stop) {
        ck(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
        ck(vkQueueWaitIdle(q), "vkQueueWaitIdle");
        count++;
        double t = now_s();
        /* gpubench: rate per 5 s window; Mpx/s = clears x W x H, comparable between screen on and off */
        if (t >= next) { printf("gpubench: %.0f s, %.1f submits/s, %.0f Mpx/s\n", t - t0, (count - last) / 5.0, (count - last) * 16.0 * W * H / 5.0 / 1e6); last = count; next += 5; }
        if (secs > 0 && t - t0 >= secs) break;
        if (us) usleep((useconds_t)us);
    }
    vkDeviceWaitIdle(dev);
    printf("keepalive: stop after %.0f s, %llu submits\n", now_s() - t0, count);
    vkDestroyDevice(dev, NULL); /* the process exit would free the rest; the device goes first so kbase sees a clean close */
    vkDestroyInstance(inst, NULL);
    return 0;
}
