/* onnm4_wsi.c: VK_KHR_surface + VK_KHR_swapchain for the onn box, presenting through OnnShell's ONNM4 path.
 *
 * PanVK on the box has no Android WSI. ZeroCopyActivity owns 3 HardwareBuffers (1280x720 RGBA8, linear); the
 * BufferBridge hands their dma-buf fds to us on abstract unix socket @onn-m4, and we tell the app over TCP
 * 127.0.0.1:7331 which buffer is ready, then wait for its ack before reusing that buffer. Same wire format as
 * m4_helper.c and sdl_onnm4_present.c:
 *   @onn-m4 (we listen, once):  u32 'M4BF' count width height format(1=RGBA8888)  u32 stride[count]  + fds
 *   to the app, per frame:      u32 'M4RV' buffer_index frame_number  (M4RV, not M4RD: rows are top-down, the app skips its GL flip)
 *   from the app, per frame:    u32 buffer_index (done with it)
 * The swapchain images ARE the app's buffers (dma-buf import, DRM_FORMAT_MOD_LINEAR, gralloc stride): no copy.
 *
 * Plugs into the loader shim (onn_vkshim.h; vkshim/build.sh links this file into libvulkan.so): the shim asks
 * onnm4_wsi_proc() for every exported symbol and every vkGet*ProcAddr lookup, advertises and strips our
 * instance/device extensions (onnm4_wsi_*_extensions), and gives us onn_icd_proc() to reach the real ICD.
 *
 * Env: PB_SOCK (default onn-m4), PB_HOST/PB_PORT (127.0.0.1:7331), ONNM4_WSI_DEBUG=1 (more stderr).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "onn_vkshim.h"

/* Not every header has the Android surface types without VK_USE_PLATFORM_ANDROID_KHR; we only read sType. */
#ifndef VK_KHR_android_surface
#define VK_KHR_android_surface 1
typedef struct VkAndroidSurfaceCreateInfoKHR { VkStructureType sType; const void *pNext; VkFlags flags; void *window; } VkAndroidSurfaceCreateInfoKHR;
#endif

#define MAXBUF 4
#define LOG(...) do { if (dbg()) fprintf(stderr, "onnm4_wsi: " __VA_ARGS__); } while (0)
#define ERR(...) fprintf(stderr, "onnm4_wsi: " __VA_ARGS__)

static int dbg(void) { static int d = -1; if (d < 0) d = getenv("ONNM4_WSI_DEBUG") != NULL; return d; }

/* ---------------------------------------------------------------- M4 session (one per process) */

enum { BUF_FREE, BUF_ACQUIRED, BUF_PRESENTING };
static struct {
    int ready, count;
    int fds[MAXBUF]; uint32_t strides[MAXBUF], w, h;
    int app;                    /* TCP to ZeroCopyActivity, -1 until the first present */
    int state[MAXBUF];
    int next;                   /* round-robin start for acquire */
    uint32_t frame;
} M = { .app = -1 };

static int read_all(int fd, void *p, size_t n)
{
    unsigned char *b = p;
    while (n) { ssize_t k = read(fd, b, n); if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; } b += k; n -= k; }
    return 0;
}
static int write_all(int fd, const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n) { ssize_t k = write(fd, b, n); if (k < 0) { if (errno == EINTR) continue; return -1; } b += k; n -= k; }
    return 0;
}

/* Same handshake as m4_helper.c: listen on @onn-m4 and take header + strides + fds. Waits at most 30 s for the bridge,
 * so a game started without ZeroCopyActivity fails instead of hanging. */
static int m4_recv_buffers(void)
{
    if (M.ready) return 0;
    const char *name = getenv("PB_SOCK") ? getenv("PB_SOCK") : "onn-m4";
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (ls < 0) return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    size_t nl = strlen(name); if (nl > sizeof sa.sun_path - 2) { close(ls); return -1; }
    memcpy(sa.sun_path + 1, name, nl);                  /* sun_path[0] == 0: abstract namespace */
    socklen_t sl = offsetof(struct sockaddr_un, sun_path) + 1 + nl;
    if (bind(ls, (struct sockaddr *)&sa, sl) < 0 || listen(ls, 1) < 0) { ERR("bind/listen @%s: %s\n", name, strerror(errno)); close(ls); return -1; }
    ERR("waiting for buffers on @%s\n", name);
    struct pollfd lp = { .fd = ls, .events = POLLIN };
    int pr;
    do pr = poll(&lp, 1, 30000); while (pr < 0 && errno == EINTR);
    if (pr <= 0) { close(ls); return -1; }
    int s = accept(ls, NULL, NULL); close(ls); if (s < 0) return -1;
    uint32_t hdr[5]; char cbuf[CMSG_SPACE(sizeof(int) * MAXBUF)];
    struct iovec iov = { hdr, sizeof hdr };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    if (recvmsg(s, &msg, MSG_CMSG_CLOEXEC) != (ssize_t)sizeof hdr || hdr[0] != 0x4642344d || hdr[1] < 1 || hdr[1] > MAXBUF || hdr[4] != 1) {
        ERR("bad buffer header\n"); close(s); return -1;
    }
    int count = hdr[1];
    if (read_all(s, M.strides, count * sizeof(uint32_t))) { close(s); return -1; }
    int got = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) { got = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int); memcpy(M.fds, CMSG_DATA(c), got * sizeof(int)); }
    close(s);
    if (got != count) { ERR("%d buffers announced, %d fds received\n", count, got); for (int i = 0; i < got; i++) close(M.fds[i]); return -1; }
    M.count = count; M.w = hdr[2]; M.h = hdr[3]; M.ready = 1;
    ERR("%d buffers %ux%u\n", M.count, M.w, M.h);
    return 0;
}

static int m4_connect_app(void)
{
    if (M.app >= 0) return 0;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); if (fd < 0) return -1;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(getenv("PB_PORT") ? atoi(getenv("PB_PORT")) : 7331) };
    inet_pton(AF_INET, getenv("PB_HOST") ? getenv("PB_HOST") : "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { ERR("cannot reach the app: %s\n", strerror(errno)); close(fd); return -1; }
    M.app = fd;
    return 0;
}

/* Read one ack if it arrives within timeout_ms (-1 = forever). 1 = got one, 0 = timed out, -1 = app gone. */
static int m4_read_ack(int timeout_ms)
{
    if (M.app < 0) return -1;
    struct pollfd p = { M.app, POLLIN, 0 };
    int r;
    do r = poll(&p, 1, timeout_ms); while (r < 0 && errno == EINTR);
    if (r == 0) return 0;
    uint32_t done;
    if (r < 0 || read_all(M.app, &done, 4)) return -1;
    if (done < (uint32_t)M.count && M.state[done] == BUF_PRESENTING) M.state[done] = BUF_FREE;
    return 1;
}

/* ---------------------------------------------------------------- ICD state */

/* The shim owns vkCreateInstance/vkCreateDevice, so we learn the instance at surface creation and the device at
 * swapchain creation, and fetch what we need from the ICD then.
 * shortcut: one VkInstance and one VkDevice at a time (globals), upgrade if a port creates several. */
static struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;              /* for the acquire signal submit: the app's present queue once seen */
    VkFence fence;              /* our fence for the present wait submit */
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
} D;

static PFN_vkVoidFunction icd(const char *name) { return onn_icd_proc(D.inst, name); }

static const VkExtensionProperties inst_exts[] = {
    { VK_KHR_SURFACE_EXTENSION_NAME, 25 },
    { "VK_KHR_android_surface", 6 },
    { VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, 1 },
    { VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, 1 },
};
static const VkExtensionProperties dev_exts[] = { { VK_KHR_SWAPCHAIN_EXTENSION_NAME, 70 } };

uint32_t onnm4_wsi_instance_extensions(const VkExtensionProperties **props)
{ *props = inst_exts; return sizeof inst_exts / sizeof *inst_exts; }
uint32_t onnm4_wsi_device_extensions(const VkExtensionProperties **props)
{ *props = dev_exts; return sizeof dev_exts / sizeof *dev_exts; }

/* PanVK is built with platforms=android, so it ALSO reports Mesa's own WSI extensions. These build on a
 * swapchain or surface we implement, and their commands (vkReleaseSwapchainImagesKHR, vkWaitForPresentKHR, ...)
 * would reach Mesa's wsi_common with our handles and crash, or their pNext structs would be silently ignored.
 * PROPOSED shim hook (not in onn_vkshim.h yet): the shim drops these names from the ICD's extension lists. */
static const char *const hidden_exts[] = {
    "VK_KHR_surface_maintenance1", "VK_EXT_surface_maintenance1",                         /* instance */
    "VK_KHR_swapchain_mutable_format", "VK_KHR_swapchain_maintenance1", "VK_EXT_swapchain_maintenance1",
    "VK_KHR_present_id", "VK_KHR_present_id2", "VK_KHR_present_wait", "VK_KHR_present_wait2",
    "VK_EXT_present_timing", "VK_GOOGLE_display_timing", "VK_ANDROID_native_buffer",      /* device */
};
uint32_t onnm4_wsi_hidden_extensions(const char *const **names);
uint32_t onnm4_wsi_hidden_extensions(const char *const **names)
{ *names = hidden_exts; return sizeof hidden_exts / sizeof *hidden_exts; }

/* The physical device for swapchain creation (it has none in its arguments): the first; the box has one GPU. */
static VkPhysicalDevice phys(void)
{
    if (!D.pd && D.inst) {
        PFN_vkEnumeratePhysicalDevices f = (PFN_vkEnumeratePhysicalDevices)icd("vkEnumeratePhysicalDevices");
        uint32_t n = 1;
        if (f) f(D.inst, &n, &D.pd);
    }
    return D.pd;
}

/* First swapchain on a device: fetch the device commands, pick a queue for acquire, make our fence. */
static int device_init(VkDevice dev)
{
    if (D.dev == dev && D.fence) return 0;
    /* shortcut: a previous device's fence is dropped, not destroyed (that device is gone), upgrade together
     * with the one-device shortcut above. */
    D.dev = VK_NULL_HANDLE; D.queue = VK_NULL_HANDLE; D.fence = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)icd("vkGetDeviceProcAddr");
    if (!gdpa || !phys()) return -1;
#define DFN(x) D.x = (PFN_vk##x)gdpa(dev, "vk" #x); if (!D.x) { ERR("ICD lacks vk" #x "\n"); return -1; }
    DFN(GetDeviceQueue); DFN(CreateImage); DFN(DestroyImage); DFN(GetImageMemoryRequirements); DFN(AllocateMemory);
    DFN(FreeMemory); DFN(BindImageMemory); DFN(QueueSubmit); DFN(CreateFence); DFN(WaitForFences); DFN(ResetFences);
#undef DFN
    /* Mesa's vkGetDeviceProcAddr hides commands of extensions the app did not enable, and the shim does not enable
     * the import extensions; Mesa's instance-level trampoline still reaches the driver. Optional: without it the
     * import falls back to the image's own memory types. */
    D.GetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)gdpa(dev, "vkGetMemoryFdPropertiesKHR");
    if (!D.GetMemoryFdPropertiesKHR) D.GetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)icd("vkGetMemoryFdPropertiesKHR");
    /* shortcut: until the first present hands us the app's queue, acquire signals on queue 0 of the first
     * graphics family (PanVK has one family), upgrade if the shim passes us the device create info. */
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qfp = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)icd("vkGetPhysicalDeviceQueueFamilyProperties");
    VkQueueFamilyProperties qf[8]; uint32_t qn = 8;
    if (qfp) qfp(D.pd, &qn, qf); else qn = 0;
    for (uint32_t i = 0; i < qn && !D.queue; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) D.GetDeviceQueue(dev, i, 0, &D.queue);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (D.CreateFence(dev, &fci, NULL, &D.fence) != VK_SUCCESS) { D.fence = VK_NULL_HANDLE; return -1; }
    D.dev = dev;
    return 0;
}

/* ---------------------------------------------------------------- surface */

typedef struct { int dummy; } Surface;

static VkResult make_surface(VkInstance inst, VkSurfaceKHR *out)
{
    if (D.inst != inst) { D.inst = inst; D.pd = VK_NULL_HANDLE; }
    /* The buffers fix the surface size, so take them now (as the GL path does at make-current). */
    if (m4_recv_buffers() < 0) { ERR("no buffers from the bridge within 30 s (is ZeroCopyActivity up with --ez launch false?)\n"); return VK_ERROR_SURFACE_LOST_KHR; }
    Surface *s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = (VkSurfaceKHR)(uintptr_t)s;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateAndroidSurfaceKHR(VkInstance i, const VkAndroidSurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *out)
{ (void)ci; (void)a; return make_surface(i, out); }  /* the ANativeWindow is ignored: there is none on this path */
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateHeadlessSurfaceEXT(VkInstance i, const VkHeadlessSurfaceCreateInfoEXT *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *out)
{ (void)ci; (void)a; return make_surface(i, out); }
static VKAPI_ATTR void VKAPI_CALL w_DestroySurfaceKHR(VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a)
{ (void)i; (void)a; free((void *)(uintptr_t)s); }

static VKAPI_ATTR VkResult VKAPI_CALL w_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t q, VkSurfaceKHR s, VkBool32 *ok)
{ (void)pd; (void)q; (void)s; *ok = VK_TRUE; return VK_SUCCESS; }

/* Can a buffer be imported as this format with this usage? Asked the same way the import is done. */
static int import_ok(VkPhysicalDevice pd, VkFormat fmt, VkImageUsageFlags usage)
{
    PFN_vkGetPhysicalDeviceImageFormatProperties2 f = (PFN_vkGetPhysicalDeviceImageFormatProperties2)icd("vkGetPhysicalDeviceImageFormatProperties2");
    if (!f) f = (PFN_vkGetPhysicalDeviceImageFormatProperties2)icd("vkGetPhysicalDeviceImageFormatProperties2KHR");
    if (!f) return fmt == VK_FORMAT_R8G8B8A8_UNORM;   /* 1.0 instance, query hidden: m4h's format, vkCreateImage decides */
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = 0 /* DRM_FORMAT_MOD_LINEAR */, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkPhysicalDeviceExternalImageFormatInfo ext = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, .pNext = &mod,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkPhysicalDeviceImageFormatInfo2 info = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &ext,
        .format = fmt, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, .usage = usage };
    VkExternalImageFormatProperties eprops = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 props = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &eprops };
    if (f(pd, &info, &props) != VK_SUCCESS) return 0;
    return (eprops.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
           props.imageFormatProperties.maxExtent.width >= M.w && props.imageFormatProperties.maxExtent.height >= M.h;
}

#define WANT_USAGE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)

static VKAPI_ATTR VkResult VKAPI_CALL w_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c)
{
    (void)s;
    if (m4_recv_buffers() < 0) return VK_ERROR_SURFACE_LOST_KHR;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (import_ok(pd, VK_FORMAT_R8G8B8A8_UNORM, WANT_USAGE)) usage = WANT_USAGE;
    *c = (VkSurfaceCapabilitiesKHR){
        .minImageCount = M.count, .maxImageCount = M.count,
        .currentExtent = { M.w, M.h }, .minImageExtent = { M.w, M.h }, .maxImageExtent = { M.w, M.h },
        .maxImageArrayLayers = 1,
        .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .supportedUsageFlags = usage };
    return VK_SUCCESS;
}

/* The buffers are Android RGBA_8888 shown as sRGB: R8G8B8A8 UNORM always, the SRGB view too if the import takes it.
 * No BGRA: the bytes are R,G,B,A, so a BGRA view would swap red and blue. */
static uint32_t surface_formats(VkPhysicalDevice pd, VkSurfaceFormatKHR out[2])
{
    uint32_t n = 0;
    out[n++] = (VkSurfaceFormatKHR){ VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    if (import_ok(pd, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        out[n++] = (VkSurfaceFormatKHR){ VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    return n;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *count, VkSurfaceFormatKHR *f)
{
    (void)s;
    VkSurfaceFormatKHR all[2]; uint32_t n = surface_formats(pd, all);
    if (!f) { *count = n; return VK_SUCCESS; }
    VkResult r = *count < n ? VK_INCOMPLETE : VK_SUCCESS;
    if (*count > n) *count = n;
    memcpy(f, all, *count * sizeof *f);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *count, VkPresentModeKHR *m)
{
    (void)pd; (void)s;
    /* shortcut: FIFO only (the app acks each shown buffer, which is FIFO already), upgrade to MAILBOX by dropping
     * superseded buffers if a port needs uncapped frame rates. */
    if (!m) { *count = 1; return VK_SUCCESS; }
    if (*count < 1) return VK_INCOMPLETE;
    *count = 1; m[0] = VK_PRESENT_MODE_FIFO_KHR;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *si, VkSurfaceCapabilities2KHR *c)
{ return w_GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, si->surface, &c->surfaceCapabilities); }  /* pNext outputs left untouched */

static VKAPI_ATTR VkResult VKAPI_CALL w_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *si, uint32_t *count, VkSurfaceFormat2KHR *f)
{
    (void)si;
    VkSurfaceFormatKHR all[2]; uint32_t n = surface_formats(pd, all);
    if (!f) { *count = n; return VK_SUCCESS; }
    VkResult r = *count < n ? VK_INCOMPLETE : VK_SUCCESS;
    if (*count > n) *count = n;
    for (uint32_t i = 0; i < *count; i++) f[i].surfaceFormat = all[i];
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *count, VkRect2D *r)
{
    (void)pd; (void)s;
    if (!r) { *count = 1; return VK_SUCCESS; }
    if (*count < 1) return VK_INCOMPLETE;
    *count = 1; r[0] = (VkRect2D){ { 0, 0 }, { M.w, M.h } };
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_GetDeviceGroupPresentCapabilitiesKHR(VkDevice d, VkDeviceGroupPresentCapabilitiesKHR *c)
{
    (void)d;
    memset(c->presentMask, 0, sizeof c->presentMask);
    c->presentMask[0] = 1;
    c->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_GetDeviceGroupSurfacePresentModesKHR(VkDevice d, VkSurfaceKHR s, VkDeviceGroupPresentModeFlagsKHR *m)
{ (void)d; (void)s; *m = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR; return VK_SUCCESS; }

/* ---------------------------------------------------------------- swapchain */

typedef struct {
    int retired;                /* replaced via oldSwapchain: acquire answers OUT_OF_DATE */
    uint32_t count;
    VkImage img[MAXBUF];
    VkDeviceMemory mem[MAXBUF];
} Swapchain;
static Swapchain *live;         /* the one non-retired swapchain */

#define SC(h) ((Swapchain *)(uintptr_t)(h))

static void free_images(VkDevice dev, Swapchain *sc)
{
    for (uint32_t i = 0; i < sc->count; i++) {
        if (sc->img[i]) D.DestroyImage(dev, sc->img[i], NULL);
        if (sc->mem[i]) D.FreeMemory(dev, sc->mem[i], NULL);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out)
{
    (void)a;
    if (m4_recv_buffers() < 0) return VK_ERROR_SURFACE_LOST_KHR;
    if (device_init(dev) < 0) { ERR("device setup failed\n"); return VK_ERROR_INITIALIZATION_FAILED; }
    /* shortcut: one swapchain at a time on one surface (all map to the same 3 app buffers), upgrade never: there is one screen. */
    if (live && SC(ci->oldSwapchain) != live) { ERR("a swapchain already exists\n"); return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR; }
    if (ci->imageExtent.width != M.w || ci->imageExtent.height != M.h || ci->imageArrayLayers != 1 || ci->flags ||
        ci->minImageCount > (uint32_t)M.count || ci->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        ERR("unsupported request: %ux%u layers %u flags 0x%x images %u colorspace %d (buffers are %ux%u x%d)\n",
            ci->imageExtent.width, ci->imageExtent.height, ci->imageArrayLayers, ci->flags, ci->minImageCount, ci->imageColorSpace, M.w, M.h, M.count);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if ((ci->imageFormat != VK_FORMAT_R8G8B8A8_UNORM && ci->imageFormat != VK_FORMAT_R8G8B8A8_SRGB) || !import_ok(D.pd, ci->imageFormat, ci->imageUsage)) {
        ERR("cannot import the buffers as format %d usage 0x%x\n", ci->imageFormat, ci->imageUsage);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    Swapchain *sc = calloc(1, sizeof *sc);
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->count = M.count;
    VkResult r = VK_SUCCESS;
    for (int i = 0; i < M.count && r == VK_SUCCESS; i++) {
        /* Exactly m4h's import: explicit linear modifier with the gralloc stride, dedicated dma-buf allocation. */
        VkSubresourceLayout plane = { .offset = 0, .size = 0, .rowPitch = M.strides[i] };
        VkImageDrmFormatModifierExplicitCreateInfoEXT mod = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = 0 /* DRM_FORMAT_MOD_LINEAR */, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane };
        VkExternalMemoryImageCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &mod,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ext, .imageType = VK_IMAGE_TYPE_2D,
            .format = ci->imageFormat, .extent = { M.w, M.h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, .usage = ci->imageUsage,
            .sharingMode = ci->imageSharingMode, .queueFamilyIndexCount = ci->queueFamilyIndexCount, .pQueueFamilyIndices = ci->pQueueFamilyIndices,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        if ((r = D.CreateImage(dev, &ic, NULL, &sc->img[i])) != VK_SUCCESS) { ERR("vkCreateImage %d -> %d\n", i, r); break; }
        VkMemoryRequirements mr; D.GetImageMemoryRequirements(dev, sc->img[i], &mr);
        VkMemoryFdPropertiesKHR fdp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR, .memoryTypeBits = ~0u };
        if (D.GetMemoryFdPropertiesKHR &&
            (r = D.GetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, M.fds[i], &fdp)) != VK_SUCCESS) { ERR("vkGetMemoryFdPropertiesKHR -> %d\n", r); break; }
        uint32_t bits = mr.memoryTypeBits & fdp.memoryTypeBits, type = 32;
        for (uint32_t t = 0; t < 32; t++) if (bits & (1u << t)) { type = t; break; }
        if (type == 32) { ERR("no memory type accepts dma-buf %d\n", i); r = VK_ERROR_INITIALIZATION_FAILED; break; }
        int fd = dup(M.fds[i]);  /* a successful import consumes the fd; ours stays for the next swapchain */
        if (fd < 0) { r = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
        VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = sc->img[i] };
        VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd };
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = mr.size, .memoryTypeIndex = type };
        if ((r = D.AllocateMemory(dev, &mai, NULL, &sc->mem[i])) != VK_SUCCESS) { ERR("import dma-buf %d -> %d\n", i, r); close(fd); break; }
        if ((r = D.BindImageMemory(dev, sc->img[i], sc->mem[i], 0)) != VK_SUCCESS) { ERR("bind %d -> %d\n", i, r); break; }
        LOG("imported buffer %d: stride %u, %llu bytes, type %u, format %d usage 0x%x\n", i, M.strides[i], (unsigned long long)mr.size, type, ci->imageFormat, ci->imageUsage);
    }
    if (r != VK_SUCCESS) { free_images(dev, sc); free(sc); return r == VK_ERROR_OUT_OF_HOST_MEMORY ? r : VK_ERROR_INITIALIZATION_FAILED; }
    if (live) live->retired = 1;
    /* Buffers the old swapchain acquired but never presented go back (presenting one later answers OUT_OF_DATE);
     * buffers on screen stay owned by the app until it acks. */
    for (int i = 0; i < M.count; i++) if (M.state[i] == BUF_ACQUIRED) M.state[i] = BUF_FREE;
    live = sc;
    *out = (VkSwapchainKHR)(uintptr_t)sc;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL w_DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR h, const VkAllocationCallbacks *a)
{
    (void)a;
    Swapchain *sc = SC(h);
    if (!sc) return;
    free_images(dev, sc);
    if (sc == live) {
        live = NULL;
        /* Acquired-but-never-presented buffers go back; buffers on screen stay owned by the app until it acks. */
        for (int i = 0; i < M.count; i++) if (M.state[i] == BUF_ACQUIRED) M.state[i] = BUF_FREE;
    }
    free(sc);
}

static VKAPI_ATTR VkResult VKAPI_CALL w_GetSwapchainImagesKHR(VkDevice dev, VkSwapchainKHR h, uint32_t *count, VkImage *imgs)
{
    (void)dev;
    Swapchain *sc = SC(h);
    if (!imgs) { *count = sc->count; return VK_SUCCESS; }
    VkResult r = *count < sc->count ? VK_INCOMPLETE : VK_SUCCESS;
    if (*count > sc->count) *count = sc->count;
    memcpy(imgs, sc->img, *count * sizeof *imgs);
    return r;
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

static VKAPI_ATTR VkResult VKAPI_CALL w_AcquireNextImageKHR(VkDevice dev, VkSwapchainKHR h, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *index)
{
    (void)dev;
    Swapchain *sc = SC(h);
    if (sc->retired) return VK_ERROR_OUT_OF_DATE_KHR;
    double deadline = timeout == UINT64_MAX ? -1 : now_ms() + timeout / 1e6;
    int pick = -1;
    for (;;) {
        for (int k = 0; k < M.count && pick < 0; k++) {
            int i = (M.next + k) % M.count;
            if (M.state[i] == BUF_FREE) pick = i;
        }
        if (pick >= 0) break;
        int presenting = 0;
        for (int i = 0; i < M.count; i++) presenting |= M.state[i] == BUF_PRESENTING;
        if (!presenting) return timeout ? VK_TIMEOUT : VK_NOT_READY;   /* all held by the game: no ack will free one */
        int wait = deadline < 0 ? -1 : (int)(deadline - now_ms());
        if (deadline >= 0 && wait < 0) wait = 0;
        int got = m4_read_ack(wait);
        if (got < 0) { ERR("app went away\n"); return VK_ERROR_SURFACE_LOST_KHR; }
        if (got == 0) return timeout ? VK_TIMEOUT : VK_NOT_READY;
    }
    /* The buffer is already free on the CPU side (its ack came in), so signal right away with an empty submit. */
    if (sem || fence) {
        if (!D.queue) { ERR("no queue to signal the acquire on\n"); return VK_ERROR_SURFACE_LOST_KHR; }
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = sem ? 1 : 0, .pSignalSemaphores = &sem };
        VkResult r = D.QueueSubmit(D.queue, 1, &si, fence);
        if (r != VK_SUCCESS) return r;
    }
    M.state[pick] = BUF_ACQUIRED;
    M.next = (pick + 1) % M.count;
    *index = pick;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_AcquireNextImage2KHR(VkDevice dev, const VkAcquireNextImageInfoKHR *ai, uint32_t *index)
{ return w_AcquireNextImageKHR(dev, ai->swapchain, ai->timeout, ai->semaphore, ai->fence, index); }

static VKAPI_ATTR VkResult VKAPI_CALL w_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi)
{
    /* Wait for the app's rendering: consume the wait semaphores in an empty submit and wait for it on the CPU.
     * shortcut: CPU wait per present (like m4h's vkQueueWaitIdle), upgrade to a present thread or a sync_file
     * exported from the semaphore if the GPU/CPU overlap is needed for frame rate. */
    VkResult r;
    D.queue = q;                /* a queue the app owns in a present-capable family: acquire signals here from now on */
    VkPipelineStageFlags *stages = NULL;
    if (pi->waitSemaphoreCount) {
        stages = malloc(pi->waitSemaphoreCount * sizeof *stages);
        if (!stages) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < pi->waitSemaphoreCount; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = pi->waitSemaphoreCount,
        .pWaitSemaphores = pi->pWaitSemaphores, .pWaitDstStageMask = stages };
    r = D.QueueSubmit(q, 1, &si, D.fence);
    free(stages);
    if (r == VK_SUCCESS) r = D.WaitForFences(D.dev, 1, &D.fence, VK_TRUE, UINT64_MAX);
    if (r == VK_SUCCESS) r = D.ResetFences(D.dev, 1, &D.fence);
    if (r != VK_SUCCESS) { for (uint32_t i = 0; i < pi->swapchainCount; i++) if (pi->pResults) pi->pResults[i] = r; return r; }

    VkResult worst = VK_SUCCESS;
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        Swapchain *sc = SC(pi->pSwapchains[i]);
        uint32_t idx = pi->pImageIndices[i];
        VkResult ri = VK_SUCCESS;
        if (idx >= sc->count || M.state[idx] != BUF_ACQUIRED) ri = VK_ERROR_OUT_OF_DATE_KHR;
        else if (m4_connect_app() < 0) ri = VK_ERROR_SURFACE_LOST_KHR;
        else {
            uint32_t ready[3] = { 0x5652344d /* 'M4RV' */, idx, M.frame++ };
            if (write_all(M.app, ready, sizeof ready)) { ERR("app went away\n"); ri = VK_ERROR_SURFACE_LOST_KHR; }
            else M.state[idx] = BUF_PRESENTING;
        }
        if (ri == VK_SUCCESS && sc->retired) ri = VK_ERROR_OUT_OF_DATE_KHR;  /* shown, but tell the app to move on */
        if (pi->pResults) pi->pResults[i] = ri;
        if (ri < 0 || worst == VK_SUCCESS) worst = ri;
    }
    return worst;
}

/* ---------------------------------------------------------------- dispatch */

PFN_vkVoidFunction onnm4_wsi_proc(const char *name)
{
    static const struct { const char *n; PFN_vkVoidFunction f; } t[] = {
#define E(x) { "vk" #x, (PFN_vkVoidFunction)w_##x }
        E(CreateAndroidSurfaceKHR), E(CreateHeadlessSurfaceEXT), E(DestroySurfaceKHR),
        E(GetPhysicalDeviceSurfaceSupportKHR), E(GetPhysicalDeviceSurfaceCapabilitiesKHR), E(GetPhysicalDeviceSurfaceFormatsKHR),
        E(GetPhysicalDeviceSurfacePresentModesKHR), E(GetPhysicalDeviceSurfaceCapabilities2KHR), E(GetPhysicalDeviceSurfaceFormats2KHR),
        E(GetPhysicalDevicePresentRectanglesKHR), E(GetDeviceGroupPresentCapabilitiesKHR), E(GetDeviceGroupSurfacePresentModesKHR),
        E(CreateSwapchainKHR), E(DestroySwapchainKHR), E(GetSwapchainImagesKHR), E(AcquireNextImageKHR), E(AcquireNextImage2KHR),
        E(QueuePresentKHR),
#undef E
    };
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(name, t[i].n)) return t[i].f;
    return NULL;
}
