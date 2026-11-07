/* M2: one triangle through a real graphics pipeline, offscreen,
 * on a dlopen'ed PanVK ICD. Pass = inside pixels are (255,0,0,255), outside pixels are the
 * clear color (0,0,0,0), and red coverage is in the expected range.
 *
 * Shaders: compile tri.vert / tri.frag into C headers first:
 *   glslangValidator -V --vn tri_vert_spv tri.vert -o tri_vert.h
 *   glslangValidator -V --vn tri_frag_spv tri.frag -o tri_frag.h
 * Build (same shim as m1):
 *   aarch64-linux-android29-clang -O1 -DVK_NO_PROTOTYPES m2_helper.c -o m2h -ldl
 * Run:
 *   LD_LIBRARY_PATH=/data/local/tmp/pb ./linker64 /data/local/tmp/pb/m2h /data/local/tmp/pb/libvulkan_panfrost.so
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "tri_vert.h"
#include "tri_frag.h"

#define W 64
#define H 64

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name); if (!name) die("missing " #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name); if (!name) die("missing " #name)

static void die(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }

/* Mapping watch: PB_WATCH_VA=0x... (a BO's VA from the kmod trace). After each API call, report the
 * first call after which that VA is no longer inside any VMA of this process (reads /proc/self/maps). */
static uintptr_t watch_va; static int watch_alive = -1;
static void watch(const char *step)
{
    if (!watch_va) return;
    FILE *f = fopen("/proc/self/maps", "r"); if (!f) return;
    char line[512]; unsigned long lo, hi; int now = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && watch_va >= lo && watch_va < hi) { now = 1; break; }
    fclose(f);
    if (now != watch_alive) fprintf(stderr, "WATCH %#lx %s after %s\n", (unsigned long)watch_va, now ? "ALIVE" : "GONE", step);
    watch_alive = now;
}
static void ck(VkResult r, const char *m) { if (r != VK_SUCCESS) { fprintf(stderr, "FAIL: %s -> %d\n", m, r); exit(1); } watch(m); }

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    die("no suitable memory type");
    return 0;
}

int main(int argc, char **argv)
{
    if (getenv("PB_WATCH_VA")) watch_va = strtoull(getenv("PB_WATCH_VA"), NULL, 0);
    const char *icd = argc > 1 ? argv[1] : "libvulkan_panfrost.so";
    void *h = dlopen(icd, RTLD_NOW);
    if (!h) { fprintf(stderr, "FAIL: dlopen %s: %s\n", icd, dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) die("no vk_icdGetInstanceProcAddr export");

    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) die("vkCreateInstance");
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "m2h", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    ck(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties);
    IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
    IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr);
    gdpa = vkGetDeviceProcAddr;

    uint32_t n = 1; VkPhysicalDevice pd;
    VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) die("no physical device");
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s  api %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn, sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    uint32_t qi = qn;
    for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qi = i; break; }
    if (qi == qn) die("no graphics queue");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev; ck(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice");

    DFN(vkGetDeviceQueue); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory);
    DFN(vkBindImageMemory); DFN(vkCreateImageView); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements);
    DFN(vkBindBufferMemory); DFN(vkCreateShaderModule); DFN(vkCreateRenderPass); DFN(vkCreateFramebuffer);
    DFN(vkCreatePipelineLayout); DFN(vkCreateGraphicsPipelines); DFN(vkCreateCommandPool);
    DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdBeginRenderPass); DFN(vkCmdBindPipeline);
    DFN(vkCmdDraw); DFN(vkCmdEndRenderPass); DFN(vkCmdPipelineBarrier); DFN(vkCmdCopyImageToBuffer);
    DFN(vkEndCommandBuffer); DFN(vkQueueSubmit); DFN(vkQueueWaitIdle); DFN(vkDeviceWaitIdle); DFN(vkMapMemory);

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkQueue q; vkGetDeviceQueue(dev, qi, 0, &q); watch("vkGetDeviceQueue");

    /* Color target */
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; ck(vkCreateImage(dev, &ic, NULL, &img), "vkCreateImage");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory imem; ck(vkAllocateMemory(dev, &mai, NULL, &imem), "alloc image mem");
    ck(vkBindImageMemory(dev, img, imem, 0), "bind image");
    VkImageViewCreateInfo ivc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view; ck(vkCreateImageView(dev, &ivc, NULL, &view), "vkCreateImageView");

    /* Readback buffer */
    VkDeviceSize bsz = W * H * 4;
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bsz,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf; ck(vkCreateBuffer(dev, &bc, NULL, &buf), "vkCreateBuffer");
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem; ck(vkAllocateMemory(dev, &mai, NULL, &bmem), "alloc buffer mem");
    ck(vkBindBufferMemory(dev, buf, bmem, 0), "bind buffer");

    /* Render pass: clear to (0,0,0,0), end in TRANSFER_SRC for the copy */
    VkAttachmentDescription att = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference aref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &aref };
    VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    VkRenderPassCreateInfo rpc = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
    VkRenderPass rp; ck(vkCreateRenderPass(dev, &rpc, NULL, &rp), "vkCreateRenderPass");
    VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
        .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
    VkFramebuffer fb; ck(vkCreateFramebuffer(dev, &fbc, NULL, &fb), "vkCreateFramebuffer");

    /* Shaders + pipeline */
    VkShaderModuleCreateInfo vsc = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof tri_vert_spv, .pCode = tri_vert_spv };
    VkShaderModuleCreateInfo fsc = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof tri_frag_spv, .pCode = tri_frag_spv };
    VkShaderModule vs, fs;
    ck(vkCreateShaderModule(dev, &vsc, NULL, &vs), "vkCreateShaderModule(vert)");
    ck(vkCreateShaderModule(dev, &fsc, NULL, &fs), "vkCreateShaderModule(frag)");
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, W, H, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl; ck(vkCreatePipelineLayout(dev, &plc, NULL, &pl), "vkCreatePipelineLayout");
    VkGraphicsPipelineCreateInfo gpc = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp, .subpass = 0 };
    VkPipeline pipe; ck(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpc, NULL, &pipe), "vkCreateGraphicsPipelines (shader compile)");

    /* Commands */
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qi };
    VkCommandPool pool; ck(vkCreateCommandPool(dev, &pci, NULL, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; ck(vkAllocateCommandBuffers(dev, &cai, &cmd), "alloc cmdbuf");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    ck(vkBeginCommandBuffer(cmd, &bi), "begin");

    VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 0 } } };
    VkRenderPassBeginInfo rpb = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE); watch("vkCmdBeginRenderPass");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); watch("vkCmdBindPipeline");
    vkCmdDraw(cmd, 3, 1, 0, 0); watch("vkCmdDraw");
    vkCmdEndRenderPass(cmd); watch("vkCmdEndRenderPass");

    VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg); watch("vkCmdCopyImageToBuffer");
    VkBufferMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = buf, .size = VK_WHOLE_SIZE };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL); watch("vkCmdPipelineBarrier");
    ck(vkEndCommandBuffer(cmd), "end");

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    ck(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    ck(vkQueueWaitIdle(q), "vkQueueWaitIdle");
    ck(vkDeviceWaitIdle(dev), "vkDeviceWaitIdle");

    unsigned char *px; ck(vkMapMemory(dev, bmem, 0, bsz, 0, (void **)&px), "vkMapMemory");

    /* Probe pixels: (x,y) and whether they must be inside the triangle.
     * NDC y=-0.9 is row ~3 (top), apex y=0.9 is row ~61 (bottom); half-width shrinks linearly. */
    struct { int x, y, inside; } probes[] = {
        { 32, 32, 1 },   /* center */
        { 32,  8, 1 },   /* near the wide top of the triangle */
        {  0,  0, 0 },   /* corner, above the base */
        { 60, 32, 0 },   /* right of the triangle at mid-height */
        { 32, 63, 0 },   /* below the apex */
    };
    const unsigned char in_c[4] = { 255, 0, 0, 255 }, out_c[4] = { 0, 0, 0, 0 };
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof probes / sizeof *probes; i++) {
        const unsigned char *p = px + (probes[i].y * W + probes[i].x) * 4;
        const unsigned char *want = probes[i].inside ? in_c : out_c;
        int ok = 1;
        for (int c = 0; c < 4; c++) if (abs((int)p[c] - (int)want[c]) > 1) ok = 0;
        printf("pixel(%2d,%2d) %s: got %3u %3u %3u %3u  want %3u %3u %3u %3u  %s\n", probes[i].x, probes[i].y,
               probes[i].inside ? "inside " : "outside", p[0], p[1], p[2], p[3], want[0], want[1], want[2], want[3], ok ? "ok" : "WRONG");
        if (!ok) bad++;
    }
    /* Coverage sanity: a triangle spanning 90% of width and height covers ~1660 of 4096 pixels. */
    unsigned red = 0;
    for (unsigned i = 0; i < W * H; i++) if (px[i * 4] > 200 && px[i * 4 + 1] < 50) red++;
    printf("red pixels: %u / %u (expect roughly 1500-1800)\n", red, W * H);
    if (red < 1200 || red > 2100) { printf("coverage out of range\n"); bad++; }

    printf(bad ? "M2 FAIL: %u checks wrong\n" : "M2 PASS: %u checks wrong\n", bad);
    return bad ? 2 : 0;
}
