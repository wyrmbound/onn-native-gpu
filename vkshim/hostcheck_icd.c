/* hostcheck_icd.c: a fake ICD for the host-side dry run of the shim (build.sh). It reports PanVK's WSI
 * extensions the way the platforms=android build does, and records the extensions the shim passes on. */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define X __attribute__((visibility("default")))
static const char *inst_ext[] = { "VK_KHR_surface", "VK_KHR_android_surface", "VK_EXT_headless_surface",
    "VK_KHR_get_surface_capabilities2", "VK_KHR_surface_maintenance1", "VK_EXT_surface_maintenance1",
    "VK_KHR_external_memory_capabilities", "VK_EXT_debug_utils" };
/* VK_EXT_image_drm_format_modifier left out on purpose: the shim must add only what the ICD reports. */
static const char *dev_ext[] = { "VK_KHR_swapchain", "VK_KHR_swapchain_mutable_format", "VK_KHR_swapchain_maintenance1",
    "VK_EXT_swapchain_maintenance1", "VK_KHR_present_id", "VK_KHR_present_id2", "VK_KHR_present_wait",
    "VK_KHR_present_wait2", "VK_EXT_present_timing", "VK_GOOGLE_display_timing", "VK_ANDROID_native_buffer",
    "VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
    "VK_KHR_image_format_list", "VK_KHR_maintenance5" };
#define N(a) (uint32_t)(sizeof a / sizeof *a)

X char icd_seen_instance[1024], icd_seen_device[1024]; /* space-separated, read by hostcheck.c */
static int obj[2];

static VkResult fill(const char **v, uint32_t n, uint32_t *c, VkExtensionProperties *p) {
    if (!p) { *c = n; return VK_SUCCESS; }
    uint32_t k = *c < n ? *c : n;
    for (uint32_t i = 0; i < k; i++) { memset(&p[i], 0, sizeof p[i]); strcpy(p[i].extensionName, v[i]); p[i].specVersion = 1; }
    *c = k;
    return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}
static void record(char *out, uint32_t n, const char *const *names) {
    out[0] = 0;
    for (uint32_t i = 0; i < n; i++) { strcat(out, names[i]); strcat(out, " "); }
}
static VkResult EnumInst(const char *l, uint32_t *c, VkExtensionProperties *p) { (void)l; return fill(inst_ext, N(inst_ext), c, p); }
static VkResult EnumDev(VkPhysicalDevice pd, const char *l, uint32_t *c, VkExtensionProperties *p) { (void)pd; (void)l; return fill(dev_ext, N(dev_ext), c, p); }
static VkResult CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void)a; record(icd_seen_instance, ci->enabledExtensionCount, ci->ppEnabledExtensionNames);
    *out = (VkInstance)&obj[0]; return VK_SUCCESS;
}
static void DestroyInstance(VkInstance i, const VkAllocationCallbacks *a) { (void)i; (void)a; }
static VkResult EnumPD(VkInstance i, uint32_t *c, VkPhysicalDevice *p) {
    (void)i; if (p && *c) p[0] = (VkPhysicalDevice)&obj[1]; *c = 1; return VK_SUCCESS;
}
static VkResult CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void)pd; (void)a; record(icd_seen_device, ci->enabledExtensionCount, ci->ppEnabledExtensionNames);
    *out = (VkDevice)&obj[1]; return VK_SUCCESS;
}
static PFN_vkVoidFunction GDPA(VkDevice d, const char *n) { (void)d; (void)n; return NULL; }

X PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance i, const char *n) {
    (void)i;
#define F(name, fn) if (!strcmp(n, name)) return (PFN_vkVoidFunction)fn;
    F("vkEnumerateInstanceExtensionProperties", EnumInst) F("vkEnumerateDeviceExtensionProperties", EnumDev)
    F("vkCreateInstance", CreateInstance) F("vkDestroyInstance", DestroyInstance) F("vkEnumeratePhysicalDevices", EnumPD)
    F("vkCreateDevice", CreateDevice) F("vkGetDeviceProcAddr", GDPA)
    return NULL;
}
X VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *v) { if (*v > 7) *v = 7; return VK_SUCCESS; }
