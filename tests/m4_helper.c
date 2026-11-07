/* M4 / B2: zero-copy rendering into app-owned buffers.
 *
 * Roles:
 *   OnnShell (app)            allocates N HardwareBuffers and shows them (Bitmap.wrapHardwareBuffer on a
 *                             hardware canvas). Talks to us over loopback TCP 127.0.0.1:7331.
 *   BufferBridge (shell uid)  a Shizuku user service that pulls each buffer's dma-buf fd + stride and sends
 *                             them to us over an abstract unix socket with SCM_RIGHTS (shell -> shell).
 *   m4h (this, 64-bit)        imports each fd as a LINEAR VkImage (VK_KHR_external_memory_fd +
 *                             VK_EXT_image_drm_format_modifier), renders into it, tells the app which
 *                             buffer is ready, waits for the app's ack before reusing a buffer.
 *
 * Unix socket (we listen, abstract name "onn-m4", bridge connects), one message:
 *   u32 magic 'M4BF' (0x4642344d)  u32 count  u32 width  u32 height  u32 format (1 = RGBA8888)
 *   u32 stride_bytes[count]   ... with `count` dma-buf fds attached via SCM_RIGHTS, same order.
 * TCP to the app, per frame:   u32 'M4RD' (0x4452344d)  u32 buffer_index  u32 frame_number
 * TCP from the app, per frame: u32 buffer_index (the app is done drawing that buffer)
 *
 * The triangle is the M2 one; the clear color cycles hue per frame so a live rate is visible.
 * Env: PB_FRAMES (default 600), PB_HOST/PB_PORT (127.0.0.1:7331), PB_NOACK=1 (don't wait for acks; for
 * measuring the raw import+render rate), PB_SOCK (abstract socket name, default onn-m4).
 * Build: aarch64-linux-android29-clang -O1 -DVK_NO_PROTOTYPES m4_helper.c -o m4h -ldl -lm
 */
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "tri_vert.h"
#include "tri_frag.h"

#define MAXBUF 4

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name); if (!name) die("missing " #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name); if (!name) die("missing " #name)

static void die(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }
static void ck(VkResult r, const char *m) { if (r != VK_SUCCESS) { fprintf(stderr, "FAIL: %s -> %d\n", m, r); exit(1); } }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static int write_all(int fd, const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n) { ssize_t k = write(fd, b, n); if (k < 0) { if (errno == EINTR) continue; return -1; } b += k; n -= k; }
    return 0;
}
static int read_all(int fd, void *p, size_t n)
{
    unsigned char *b = p;
    while (n) { ssize_t k = read(fd, b, n); if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; } b += k; n -= k; }
    return 0;
}

/* Wait for the bridge on the abstract unix socket and take the buffers. */
static int recv_buffers(int fds[], uint32_t strides[], uint32_t *w, uint32_t *h)
{
    const char *name = getenv("PB_SOCK") ? getenv("PB_SOCK") : "onn-m4";
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (ls < 0) die("unix socket");
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    size_t nl = strlen(name); if (nl > sizeof sa.sun_path - 2) die("PB_SOCK too long");
    memcpy(sa.sun_path + 1, name, nl);                  /* sun_path[0] == 0: abstract namespace */
    socklen_t sl = offsetof(struct sockaddr_un, sun_path) + 1 + nl;
    if (bind(ls, (struct sockaddr *)&sa, sl) < 0) { perror("bind @onn-m4"); exit(1); }
    if (listen(ls, 1) < 0) die("listen");
    fprintf(stderr, "waiting for buffers on @%s\n", name);
    int s = accept(ls, NULL, NULL); if (s < 0) die("accept"); close(ls);

    uint32_t hdr[5];
    char cbuf[CMSG_SPACE(sizeof(int) * MAXBUF)];
    struct iovec iov = { hdr, sizeof hdr };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    ssize_t k = recvmsg(s, &msg, MSG_CMSG_CLOEXEC);
    if (k != (ssize_t)sizeof hdr) die("short buffer header");
    if (hdr[0] != 0x4642344d) die("bad buffer magic");
    uint32_t count = hdr[1]; *w = hdr[2]; *h = hdr[3];
    if (count < 1 || count > MAXBUF || hdr[4] != 1) die("bad count/format");
    if (read_all(s, strides, count * sizeof *strides)) die("short strides");
    int got = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            got = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            memcpy(fds, CMSG_DATA(c), got * sizeof(int));
        }
    if (got != (int)count) { fprintf(stderr, "FAIL: %u buffers announced, %d fds received\n", count, got); exit(1); }
    close(s);
    for (uint32_t i = 0; i < count; i++) fprintf(stderr, "buffer %u: fd %d stride %u\n", i, fds[i], strides[i]);
    return count;
}

static int connect_app(void)
{
    const char *host = getenv("PB_HOST") ? getenv("PB_HOST") : "127.0.0.1";
    int port = getenv("PB_PORT") ? atoi(getenv("PB_PORT")) : 7331;
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) die("tcp socket");
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, host, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { fprintf(stderr, "connect %s:%d: %s\n", host, port, strerror(errno)); exit(3); }
    return fd;
}

int main(int argc, char **argv)
{
    const char *icd = argc > 1 ? argv[1] : "libvulkan_panfrost.so";
    void *hdl = dlopen(icd, RTLD_NOW);
    if (!hdl) { fprintf(stderr, "FAIL: dlopen %s: %s\n", icd, dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(hdl, "vk_icdGetInstanceProcAddr");
    if (!gipa) die("no vk_icdGetInstanceProcAddr export");

    /* Buffers first: the bridge connects as soon as the app has allocated them. */
    int fds[MAXBUF]; uint32_t strides[MAXBUF], W, H;
    int count = recv_buffers(fds, strides, &W, &H);

    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) die("vkCreateInstance");
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "m4h", .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    ck(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");

    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkGetPhysicalDeviceMemoryProperties); IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr);
    gdpa = vkGetDeviceProcAddr;

    uint32_t n = 1; VkPhysicalDevice pd;
    VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) die("no physical device");
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s  api %u.%u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn, sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    uint32_t qi = qn;
    for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qi = i; break; }
    if (qi == qn) die("no graphics queue");

    const char *exts[] = { "VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
                           "VK_EXT_image_drm_format_modifier", "VK_KHR_image_format_list" };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = sizeof exts / sizeof *exts, .ppEnabledExtensionNames = exts };
    VkDevice dev; ck(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice (with import extensions)");

    DFN(vkGetDeviceQueue); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindImageMemory);
    DFN(vkCreateImageView); DFN(vkCreateShaderModule); DFN(vkCreateRenderPass); DFN(vkCreateFramebuffer);
    DFN(vkCreatePipelineLayout); DFN(vkCreateGraphicsPipelines); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers);
    DFN(vkBeginCommandBuffer); DFN(vkCmdBeginRenderPass); DFN(vkCmdBindPipeline); DFN(vkCmdDraw); DFN(vkCmdEndRenderPass);
    DFN(vkEndCommandBuffer); DFN(vkQueueSubmit); DFN(vkQueueWaitIdle); DFN(vkResetCommandBuffer);
    DFN(vkGetMemoryFdPropertiesKHR);

    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkQueue q; vkGetDeviceQueue(dev, qi, 0, &q);

    /* Import each dma-buf as a linear RGBA8 color attachment with the gralloc stride. */
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImage img[MAXBUF]; VkImageView view[MAXBUF]; VkFramebuffer fb[MAXBUF];
    for (int i = 0; i < count; i++) {
        VkSubresourceLayout plane = { .offset = 0, .size = 0, .rowPitch = strides[i] };
        VkImageDrmFormatModifierExplicitCreateInfoEXT mod = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = 0 /* DRM_FORMAT_MOD_LINEAR */, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane };
        VkExternalMemoryImageCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &mod,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ext, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
            .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        ck(vkCreateImage(dev, &ic, NULL, &img[i]), "vkCreateImage (drm modifier, linear)");

        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img[i], &mr);
        VkMemoryFdPropertiesKHR fdp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
        ck(vkGetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fds[i], &fdp), "vkGetMemoryFdPropertiesKHR");
        uint32_t bits = mr.memoryTypeBits & fdp.memoryTypeBits, type = 32;
        for (uint32_t t = 0; t < 32; t++) if (bits & (1u << t)) { type = t; break; }
        if (type == 32) die("no memory type accepts the dma-buf");
        VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img[i] };
        VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup(fds[i]) /* import consumes it */ };
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = mr.size, .memoryTypeIndex = type };
        VkDeviceMemory mem; ck(vkAllocateMemory(dev, &mai, NULL, &mem), "vkAllocateMemory (import dma-buf)");
        ck(vkBindImageMemory(dev, img[i], mem, 0), "vkBindImageMemory (imported)");
        VkImageViewCreateInfo ivc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        ck(vkCreateImageView(dev, &ivc, NULL, &view[i]), "vkCreateImageView");
        printf("imported buffer %d: memreq %llu bytes, type %u\n", i, (unsigned long long)mr.size, type);
    }

    /* Render pass: clear + draw, end in GENERAL (the compositor reads it through gralloc, not Vulkan). */
    VkAttachmentDescription att = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference aref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &aref };
    VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT };
    VkRenderPassCreateInfo rpc = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
    VkRenderPass rp; ck(vkCreateRenderPass(dev, &rpc, NULL, &rp), "vkCreateRenderPass");
    for (int i = 0; i < count; i++) {
        VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
            .pAttachments = &view[i], .width = W, .height = H, .layers = 1 };
        ck(vkCreateFramebuffer(dev, &fbc, NULL, &fb[i]), "vkCreateFramebuffer");
    }

    /* Shaders + pipeline (M2) */
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
    VkPipeline pipe; ck(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpc, NULL, &pipe), "vkCreateGraphicsPipelines");

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qi };
    VkCommandPool pool; ck(vkCreateCommandPool(dev, &pci, NULL, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; ck(vkAllocateCommandBuffers(dev, &cai, &cmd), "alloc cmdbuf");

    int app = connect_app();
    int frames = getenv("PB_FRAMES") ? atoi(getenv("PB_FRAMES")) : 600;
    int noack = getenv("PB_NOACK") != NULL;
    int busy[MAXBUF] = { 0 };
    double t0 = now_s();
    for (int f = 0; f < frames; f++) {
        int i = f % count;
        while (busy[i] && !noack) {            /* the app still shows this buffer: wait for its ack */
            uint32_t done; if (read_all(app, &done, 4)) { perror("ack"); return 3; }
            if (done < (uint32_t)count) busy[done] = 0;
        }
        /* Hue-cycling clear so motion is visible on the TV. */
        float hue = (f % 180) / 180.0f * 6.0f, x = 1.0f - fabsf(fmodf(hue, 2.0f) - 1.0f);
        float rgb[3] = { 0 };
        switch ((int)hue) { case 0: rgb[0] = 1; rgb[1] = x; break; case 1: rgb[0] = x; rgb[1] = 1; break; case 2: rgb[1] = 1; rgb[2] = x; break;
                            case 3: rgb[1] = x; rgb[2] = 1; break; case 4: rgb[0] = x; rgb[2] = 1; break; default: rgb[0] = 1; rgb[2] = x; }
        VkClearValue clear = { .color = { .float32 = { rgb[0] * 0.25f, rgb[1] * 0.25f, rgb[2] * 0.25f, 1.0f } } };
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        ck(vkResetCommandBuffer(cmd, 0), "reset"); ck(vkBeginCommandBuffer(cmd, &bi), "begin");
        VkRenderPassBeginInfo rpb = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb[i],
            .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &clear };
        vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        ck(vkEndCommandBuffer(cmd), "end");
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        ck(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
        ck(vkQueueWaitIdle(q), "vkQueueWaitIdle");   /* shortcut: idle wait instead of a fence; upgrade when pipelining frames */

        uint32_t ready[3] = { 0x4452344d, (uint32_t)i, (uint32_t)f };
        if (write_all(app, ready, sizeof ready)) { perror("notify"); return 3; }
        busy[i] = 1;
        if (f == 0) printf("first frame delivered into buffer 0\n");
    }
    double dt = now_s() - t0;
    printf("%d frames into %d app buffers, %ux%u, %.3f s = %.1f fps (zero-copy)\n", frames, count, W, H, dt, frames / dt);
    printf("M4 PASS\n");
    return 0;
}
