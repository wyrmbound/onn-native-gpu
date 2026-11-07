/* M3: render the M2 triangle at 1280x720 on the dlopen'ed PanVK ICD,
 * read it back, self-check it, then hand each frame to the 32-bit display app over loopback TCP.
 *
 * Why TCP and not a unix socket or a file: the renderer runs in the SELinux shell domain and the
 * app in an app domain. On a user build, apps cannot read /data/local/tmp (shell_data_file), and
 * unix_stream_socket connectto between those domains is not granted. Both domains may use TCP on
 * 127.0.0.1, so a loopback socket crosses the boundary with no policy fight.
 *
 * Wire format, little-endian, one message per frame:
 *   u32 magic 'M3F1' (0x3146334d)  u32 width  u32 height  u32 format (1 = RGBA8888, stride = width*4)
 *   then width*height*4 bytes of pixels, top row first.
 *
 * Env: PB_HOST (default 127.0.0.1), PB_PORT (default 7331), PB_FRAMES (default 1; the same command
 * buffer is resubmitted and each frame is re-read and re-sent, which measures the GPU+transport
 * rate), PB_OUT=<path> writes frames to a file instead of the socket (for debugging without the app),
 * PB_WATCH_VA (see M2).
 *
 * Build/run like m2h:
 *   glslangValidator -V --vn tri_vert_spv tri.vert -o tri_vert.h ; same for frag
 *   aarch64-linux-android29-clang -O1 -DVK_NO_PROTOTYPES m3_helper.c -o m3h -ldl
 *   LD_LIBRARY_PATH=/data/local/tmp/pb ./linker64 /data/local/tmp/pb/m3h /data/local/tmp/pb/libvulkan_panfrost.so
 */
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "tri_vert.h"
#include "tri_frag.h"

#define W 1280
#define H 720

static int write_all(int fd, const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n) { ssize_t k = write(fd, b, n); if (k < 0) { if (errno == EINTR) continue; return -1; } b += k; n -= k; }
    return 0;
}

/* Open the frame sink: a file if PB_OUT is set, else a TCP connection to the display app. */
static int open_sink(void)
{
    const char *out = getenv("PB_OUT");
    if (out) { int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) perror("PB_OUT"); return fd; }
    const char *host = getenv("PB_HOST") ? getenv("PB_HOST") : "127.0.0.1";
    int port = getenv("PB_PORT") ? atoi(getenv("PB_PORT")) : 7331;
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { fprintf(stderr, "bad PB_HOST %s\n", host); return -1; }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { fprintf(stderr, "connect %s:%d: %s\n", host, port, strerror(errno)); close(fd); return -1; }
    return fd;
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

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
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "m3h", .apiVersion = VK_API_VERSION_1_1 };
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
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, };
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

    unsigned char *px; ck(vkMapMemory(dev, bmem, 0, bsz, 0, (void **)&px), "vkMapMemory");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    int frames = getenv("PB_FRAMES") ? atoi(getenv("PB_FRAMES")) : 1;
    if (frames < 1) frames = 1;
    int sink = -1;
    unsigned bad = 0;
    double t0 = now_s();
    for (int f = 0; f < frames; f++) {
        ck(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
        ck(vkQueueWaitIdle(q), "vkQueueWaitIdle");

        if (f == 0) {
            /* Self-check before sending anything: same geometry as M2, scaled to W x H.
             * Base row ~ 0.05*H (top), apex row ~ 0.95*H (bottom), half-width shrinks linearly. */
            struct { int x, y, inside; } probes[] = {
                { W / 2, H / 2, 1 },          /* center */
                { W / 2, H / 8, 1 },          /* near the wide top */
                { 0, 0, 0 },                  /* corner, above the base */
                { W - 4, H / 2, 0 },          /* right of the triangle at mid-height */
                { W / 2, H - 1, 0 },          /* below the apex */
            };
            const unsigned char in_c[4] = { 255, 0, 0, 255 }, out_c[4] = { 0, 0, 0, 0 };
            for (unsigned i = 0; i < sizeof probes / sizeof *probes; i++) {
                const unsigned char *p = px + (probes[i].y * W + probes[i].x) * 4;
                const unsigned char *want = probes[i].inside ? in_c : out_c;
                int ok = 1;
                for (int c = 0; c < 4; c++) if (abs((int)p[c] - (int)want[c]) > 1) ok = 0;
                printf("pixel(%4d,%4d) %s: got %3u %3u %3u %3u  %s\n", probes[i].x, probes[i].y,
                       probes[i].inside ? "inside " : "outside", p[0], p[1], p[2], p[3], ok ? "ok" : "WRONG");
                if (!ok) bad++;
            }
            /* Triangle spanning 90% of width and height covers 0.405 of the pixels. */
            unsigned red = 0;
            for (unsigned i = 0; i < W * H; i++) if (px[i * 4] > 200 && px[i * 4 + 1] < 50) red++;
            double frac = (double)red / (W * H);
            printf("red pixels: %u / %u (%.3f, expect ~0.405)\n", red, W * H, frac);
            if (frac < 0.37 || frac > 0.44) { printf("coverage out of range\n"); bad++; }
            if (bad) { printf("M3 FAIL: %u checks wrong, not sending\n", bad); return 2; }

            sink = open_sink();
            if (sink < 0) { printf("M3 RENDER OK but no sink (is the display app listening?)\n"); return 3; }
        }

        uint32_t hdr[4] = { 0x3146334d, W, H, 1 };
        if (write_all(sink, hdr, sizeof hdr) || write_all(sink, px, W * H * 4)) { perror("send frame"); return 3; }
    }
    double dt = now_s() - t0;
    printf("sent %d frame(s) %ux%u RGBA8888 in %.3f s (%.1f fps incl. GPU + readback + copy)\n", frames, W, H, dt, frames / dt);
    close(sink);
    printf("M3 PASS\n");
    return 0;
}
