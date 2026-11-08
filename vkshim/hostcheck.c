/* hostcheck.c: host-side dry run of the shim + onnm4_wsi.c against hostcheck_icd.c (no GPU needed).
 * Prints the merged extension lists and what the ICD was handed, and fails on any rule broken:
 * hidden names gone, WSI names listed once and kept from the ICD, swapchain needs added only if reported.
 * Run by build.sh: ONN_VK_ICD=<dir>/libhostcheck_icd.so ./hostcheck  (HOSTCHECK_SURFACE=1 also waits out the
 * 30 s bridge timeout of vkCreateHeadlessSurfaceEXT). */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *hidden[] = { "VK_KHR_surface_maintenance1", "VK_EXT_surface_maintenance1", "VK_KHR_swapchain_mutable_format",
    "VK_KHR_swapchain_maintenance1", "VK_EXT_swapchain_maintenance1", "VK_KHR_present_id", "VK_KHR_present_id2",
    "VK_KHR_present_wait", "VK_KHR_present_wait2", "VK_EXT_present_timing", "VK_GOOGLE_display_timing", "VK_ANDROID_native_buffer" };

static int count(const VkExtensionProperties *p, uint32_t n, const char *name) {
    int c = 0;
    for (uint32_t i = 0; i < n; i++) c += !strcmp(p[i].extensionName, name);
    return c;
}
static void show(const char *what, const VkExtensionProperties *p, uint32_t n) {
    printf("%s (%u):", what, n);
    for (uint32_t i = 0; i < n; i++) printf(" %s", p[i].extensionName);
    printf("\n");
    for (size_t h = 0; h < sizeof hidden / sizeof *hidden; h++) CHECK(!count(p, n, hidden[h]), "%s still listed", hidden[h]);
}
static int seen(const char *list, const char *name) {
    char key[300];
    snprintf(key, sizeof key, "%s ", name);
    return strstr(list, key) != NULL;
}

int main(void) {
    VkExtensionProperties p[64];
    uint32_t n = 64;
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &n, p) == VK_SUCCESS, "instance enumerate");
    show("instance extensions", p, n);
    const char *wsi_inst[] = { "VK_KHR_surface", "VK_KHR_android_surface", "VK_EXT_headless_surface", "VK_KHR_get_surface_capabilities2" };
    for (int i = 0; i < 4; i++) CHECK(count(p, n, wsi_inst[i]) == 1, "%s listed %d times", wsi_inst[i], count(p, n, wsi_inst[i]));
    uint32_t two = 2;
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &two, p) == VK_INCOMPLETE && two == 2, "short array must give VK_INCOMPLETE");

    const char *want_inst[] = { "VK_KHR_surface", "VK_KHR_android_surface", "VK_EXT_debug_utils" };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionCount = 3, .ppEnabledExtensionNames = want_inst };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst) == VK_SUCCESS, "vkCreateInstance");

    void *icd = dlopen(getenv("ONN_VK_ICD"), RTLD_NOW | RTLD_NOLOAD);
    const char *si = icd ? dlsym(icd, "icd_seen_instance") : NULL, *sd = icd ? dlsym(icd, "icd_seen_device") : NULL;
    if (!si || !sd) { printf("FAIL: fake ICD not loaded\n"); return 1; }
    printf("ICD vkCreateInstance got: %s\n", si);
    CHECK(!seen(si, "VK_KHR_surface") && !seen(si, "VK_KHR_android_surface") && seen(si, "VK_EXT_debug_utils"), "instance strip");

    VkPhysicalDevice pd;
    n = 1;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd) == VK_SUCCESS && n == 1, "vkEnumeratePhysicalDevices");
    n = 64;
    CHECK(vkEnumerateDeviceExtensionProperties(pd, NULL, &n, p) == VK_SUCCESS, "device enumerate");
    show("device extensions", p, n);
    CHECK(count(p, n, "VK_KHR_swapchain") == 1, "VK_KHR_swapchain listed %d times", count(p, n, "VK_KHR_swapchain"));

    const char *want_dev[] = { "VK_KHR_swapchain", "VK_KHR_image_format_list", "VK_KHR_maintenance5" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .enabledExtensionCount = 3, .ppEnabledExtensionNames = want_dev };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev) == VK_SUCCESS, "vkCreateDevice");
    printf("ICD vkCreateDevice got: %s\n", sd);
    CHECK(!seen(sd, "VK_KHR_swapchain"), "VK_KHR_swapchain reached the ICD");
    CHECK(seen(sd, "VK_KHR_external_memory") && seen(sd, "VK_KHR_external_memory_fd") && seen(sd, "VK_EXT_external_memory_dma_buf"),
          "swapchain needs not added");
    CHECK(!seen(sd, "VK_EXT_image_drm_format_modifier"), "added an extension the ICD does not report");
    const char *ifl = strstr(sd, "VK_KHR_image_format_list ");
    CHECK(ifl && !strstr(ifl + 1, "VK_KHR_image_format_list "), "VK_KHR_image_format_list missing or duplicated");

    if (getenv("HOSTCHECK_SURFACE")) {
        PFN_vkCreateHeadlessSurfaceEXT mk = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(inst, "vkCreateHeadlessSurfaceEXT");
        VkHeadlessSurfaceCreateInfoEXT hci = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
        VkSurfaceKHR s;
        VkResult r = mk ? mk(inst, &hci, NULL, &s) : VK_ERROR_EXTENSION_NOT_PRESENT;
        printf("vkCreateHeadlessSurfaceEXT with no bridge: %d\n", r);
        CHECK(r == VK_ERROR_SURFACE_LOST_KHR, "expected VK_ERROR_SURFACE_LOST_KHR");
    }
    vkDestroyInstance(inst, NULL);
    printf(fails ? "hostcheck FAIL (%d)\n" : "hostcheck PASS\n", fails);
    return fails != 0;
}
