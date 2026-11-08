/* onn_vkshim.c: minimal Vulkan loader (libvulkan.so) for the onn box's 64-bit bionic game runtime.
 * One ICD, no layers, no trampolines. Exports core 1.0-1.3 plus the WSI/AHardwareBuffer commands
 * Android's libvulkan.so exports (onn_vkshim_gen.c, generated from vk.xml by gen_vkshim.py).
 *
 * ICD: dlopen($ONN_VK_ICD or "libvulkan_panfrost.so") on first use, negotiate, then everything goes
 * through its vk_icdGetInstanceProcAddr. Mesa's GIPA serves the global commands for any instance
 * value and, once an instance exists, every instance, physical-device and device command too, so
 * one table (one cached slot per exported name) covers all of it.
 *
 * Dispatchable handles: the ICD's handles reach the app unchanged. A real loader overwrites the
 * first word of each one (VK_LOADER_DATA) with its own dispatch table; we never touch it, so Mesa's
 * vk_object_base keeps the ICD_LOADER_MAGIC it wrote and dispatches through its own tables. That is
 * exactly how our dlopen harnesses (m2_helper.c, keepalive.c) already drive the ICD.
 *
 * WSI: see onn_vkshim.h for the hook interface and lookup order.
 */
#define VK_USE_PLATFORM_ANDROID_KHR
#include "onn_vkshim.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ONN_EXPORT __attribute__((visibility("default")))

typedef VkResult (VKAPI_PTR *PFN_negotiate)(uint32_t *pVersion);

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static PFN_vkGetInstanceProcAddr g_gipa; /* the ICD's vk_icdGetInstanceProcAddr, NULL if it failed to load */
/* shortcut: one VkInstance at a time, upgrade when a port creates two */
static VkInstance g_instance;
static PFN_vkGetDeviceProcAddr g_gdpa; /* the ICD's, fetched from g_instance */

/* ---- weak no-op WSI hooks: a strong definition in onnm4_wsi.c replaces them at link time ---- */
__attribute__((weak)) PFN_vkVoidFunction onnm4_wsi_proc(const char *name) { (void)name; return NULL; }
__attribute__((weak)) uint32_t onnm4_wsi_instance_extensions(const VkExtensionProperties **props) { *props = NULL; return 0; }
__attribute__((weak)) uint32_t onnm4_wsi_device_extensions(const VkExtensionProperties **props) { *props = NULL; return 0; }
__attribute__((weak)) uint32_t onnm4_wsi_hidden_extensions(const char *const **names) { *names = NULL; return 0; }

static void load_icd(void) {
    const char *path = getenv("ONN_VK_ICD");
    if (!path || !*path) path = "libvulkan_panfrost.so";
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "onn_vkshim: dlopen %s: %s\n", path, dlerror()); return; }
    PFN_negotiate negotiate = (PFN_negotiate)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    uint32_t ver = 5; /* v5: the ICD owns surfaces (v3) and honours apiVersion > 1.0; we need nothing newer */
    if (!negotiate || negotiate(&ver) != VK_SUCCESS || ver < 3) {
        fprintf(stderr, "onn_vkshim: %s: loader interface negotiation failed (%s, version %u)\n",
                path, negotiate ? "refused" : "no vk_icdNegotiateLoaderICDInterfaceVersion", ver);
        return;
    }
    g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!g_gipa) fprintf(stderr, "onn_vkshim: %s has no vk_icdGetInstanceProcAddr\n", path);
}

static int icd_ready(void) {
    pthread_once(&g_once, load_icd);
    return g_gipa != NULL;
}

static PFN_vkVoidFunction own(const char *name);

#ifdef ONN_TRACE
/* Diagnostic build (libvulkan.so.trace): with ONN_VK_TRACE set, GIPA/GDPA hand the app our own exported
 * trampolines instead of the ICD's pointers, and every trampoline logs its name, so a port's Vulkan
 * calls show in order on stderr. */
static int trace_on(void) { static int t = -1; if (t < 0) t = getenv("ONN_VK_TRACE") != NULL; return t; }
static FILE *trace_fp(void) { static FILE *f; if (!f) { f = fopen(getenv("ONN_VK_TRACE"), "a"); if (f) setvbuf(f, NULL, _IONBF, 0); else f = stderr; } return f; }
static void trace_call(const char *name) { if (trace_on()) fprintf(trace_fp(), "vk> %s\n", name); }
static PFN_vkVoidFunction own_export(const char *name); /* table lookup, after the generated file */
#define ONN_TRACE_CALL(n) trace_call(n)
#else
#define ONN_TRACE_CALL(n) ((void)0)
#endif

/* Lookup order: the shim's own handful, then the WSI module, then the ICD. */
static PFN_vkVoidFunction lookup_icd(VkInstance instance, const char *name) {
    PFN_vkVoidFunction f = own(name);
    if (!f) f = onnm4_wsi_proc(name);
    if (!f && icd_ready()) f = g_gipa(instance, name);
    return f;
}
static PFN_vkVoidFunction lookup(VkInstance instance, const char *name) {
    PFN_vkVoidFunction f = lookup_icd(instance, name);
#ifdef ONN_TRACE
    if (f && trace_on()) { PFN_vkVoidFunction t = own_export(name); if (t) f = t; }
#endif
    return f;
}

/* First call of an exported command: resolve and cache. A NULL here means the app called a command
 * the ICD does not offer (or before vkCreateInstance); say which instead of jumping to address 0. */
static PFN_vkVoidFunction resolve_slow(PFN_vkVoidFunction *slot, const char *name) {
    PFN_vkVoidFunction f = lookup_icd(g_instance, name);
    if (!f) {
        fprintf(stderr, "onn_vkshim: %s is not available (%s)\n", name,
                !g_gipa ? "ICD not loaded" : !g_instance ? "no VkInstance yet" : "ICD does not expose it");
        abort();
    }
    __atomic_store_n(slot, f, __ATOMIC_RELAXED);
    return f;
}

#define ONN_RESOLVE(n) (ONN_TRACE_CALL(#n), (PFN_##n)(__atomic_load_n(&p_##n, __ATOMIC_RELAXED) ?: resolve_slow(&p_##n, #n)))

static int in_props(const char *name, const VkExtensionProperties *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(name, p[i].extensionName)) return 1;
    return 0;
}
static int in_names(const char *name, const char *const *v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(name, v[i])) return 1;
    return 0;
}

/* Copies the extension names the WSI module provides out of the list the ICD will see. */
static uint32_t strip(uint32_t n, const char *const *in, const VkExtensionProperties *wsi, uint32_t nw, const char **out) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!in_props(in[i], wsi, nw)) out[k++] = in[i];
    return k;
}

/* all[] holds the ICD's n extensions with room for nw more. The ICD's entries the WSI module provides
 * or hides go (PanVK reports Mesa's own WSI extensions but has no swapchain backend here), the WSI
 * module's are appended, then the usual two-call count/fill semantics. */
static VkResult merge(uint32_t n, VkExtensionProperties *all, const VkExtensionProperties *wsi, uint32_t nw,
                      uint32_t *pCount, VkExtensionProperties *out) {
    const char *const *hidden;
    uint32_t nh = onnm4_wsi_hidden_extensions(&hidden), k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!in_props(all[i].extensionName, wsi, nw) && !in_names(all[i].extensionName, hidden, nh)) all[k++] = all[i];
    for (uint32_t j = 0; j < nw; j++) all[k++] = wsi[j];
    n = k;
    if (!out) { *pCount = n; return VK_SUCCESS; }
    k = *pCount < n ? *pCount : n;
    memcpy(out, all, k * sizeof *out);
    *pCount = k;
    return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- the hand-written exports ---- */

ONN_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    PFN_vkVoidFunction f = pName ? lookup(instance, pName) : NULL;
#ifdef ONN_TRACE
    if (trace_on()) fprintf(trace_fp(), "vk> GIPA(%p, %s) -> %s\n", (void *)instance, pName ? pName : "(null)", f ? "ok" : "NULL");
#endif
    return f;
}

ONN_EXPORT PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    if (!pName) return NULL;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    PFN_vkVoidFunction f = onnm4_wsi_proc(pName);
    if (!f && g_gdpa) f = g_gdpa(device, pName);
#ifdef ONN_TRACE
    if (f && trace_on()) { PFN_vkVoidFunction t = own_export(pName); if (t) f = t; }
    if (trace_on()) fprintf(trace_fp(), "vk> GDPA(%s) -> %s\n", pName, f ? "ok" : "NULL");
#endif
    return f;
}

ONN_EXPORT VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
    if (!icd_ready()) return VK_ERROR_INCOMPATIBLE_DRIVER;
    const VkExtensionProperties *wsi;
    uint32_t nw = onnm4_wsi_instance_extensions(&wsi);
    const char *names[pCreateInfo->enabledExtensionCount + 1];
    VkInstanceCreateInfo ci = *pCreateInfo;
    ci.enabledExtensionCount = strip(pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames, wsi, nw, names);
    ci.ppEnabledExtensionNames = names;
    VkResult r = ((PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance"))(&ci, pAllocator, pInstance);
#ifdef ONN_TRACE
    if (trace_on()) {
        fprintf(trace_fp(), "vk> vkCreateInstance -> %d, api 0x%x, %u extensions:", r,
                pCreateInfo->pApplicationInfo ? pCreateInfo->pApplicationInfo->apiVersion : 0, pCreateInfo->enabledExtensionCount);
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) fprintf(stderr, " %s", pCreateInfo->ppEnabledExtensionNames[i]);
        fprintf(stderr, "\n");
    }
#endif
    if (r == VK_SUCCESS) {
        g_instance = *pInstance;
        g_gdpa = (PFN_vkGetDeviceProcAddr)g_gipa(g_instance, "vkGetDeviceProcAddr");
    }
    return r;
}

ONN_EXPORT void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    if (!instance) return;
    ((PFN_vkDestroyInstance)g_gipa(instance, "vkDestroyInstance"))(instance, pAllocator);
    if (instance == g_instance) { g_instance = NULL; g_gdpa = NULL; }
}

ONN_EXPORT VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                                              const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    /* What the WSI swapchain needs from the ICD to import the app's dma-bufs as swapchain images. */
    static const char *const swapchain_needs[] = {
        "VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier", "VK_KHR_image_format_list",
    };
    const uint32_t nneed = sizeof swapchain_needs / sizeof *swapchain_needs;
    const VkExtensionProperties *wsi;
    uint32_t nw = onnm4_wsi_device_extensions(&wsi);
    const char *names[pCreateInfo->enabledExtensionCount + nneed + 1];
    VkDeviceCreateInfo ci = *pCreateInfo;
    ci.enabledExtensionCount = strip(pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames, wsi, nw, names);
    ci.ppEnabledExtensionNames = names;
    if (in_names("VK_KHR_swapchain", pCreateInfo->ppEnabledExtensionNames, pCreateInfo->enabledExtensionCount)) {
        PFN_vkEnumerateDeviceExtensionProperties en =
            (PFN_vkEnumerateDeviceExtensionProperties)g_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
        uint32_t n = 0;
        if (en(physicalDevice, NULL, &n, NULL) == VK_SUCCESS) {
            VkExtensionProperties icd[n + 1];
            if (en(physicalDevice, NULL, &n, icd) >= 0)
                for (uint32_t i = 0; i < nneed; i++)
                    if (in_props(swapchain_needs[i], icd, n) && !in_names(swapchain_needs[i], names, ci.enabledExtensionCount))
                        names[ci.enabledExtensionCount++] = swapchain_needs[i];
        }
    }
    VkResult r = ((PFN_vkCreateDevice)g_gipa(g_instance, "vkCreateDevice"))(physicalDevice, &ci, pAllocator, pDevice);
#ifdef ONN_TRACE
    if (trace_on()) {
        fprintf(trace_fp(), "vk> vkCreateDevice -> %d, %u extensions:", r, ci.enabledExtensionCount);
        for (uint32_t i = 0; i < ci.enabledExtensionCount; i++) fprintf(stderr, " %s", names[i]);
        fprintf(stderr, "\n");
    }
#endif
    return r;
}

ONN_EXPORT VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount,
                                                                      VkExtensionProperties *pProperties) {
    ONN_TRACE_CALL("vkEnumerateInstanceExtensionProperties");
    if (!icd_ready()) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceExtensionProperties f =
        (PFN_vkEnumerateInstanceExtensionProperties)g_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (pLayerName) return f(pLayerName, pPropertyCount, pProperties);
    const VkExtensionProperties *wsi;
    uint32_t nw = onnm4_wsi_instance_extensions(&wsi), n = 0;
    VkResult r = f(NULL, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties all[n + nw + 1];
    if ((r = f(NULL, &n, all)) < 0) return r;
    return merge(n, all, wsi, nw, pPropertyCount, pProperties);
}

ONN_EXPORT VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName,
                                                                    uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    ONN_TRACE_CALL("vkEnumerateDeviceExtensionProperties");
    PFN_vkEnumerateDeviceExtensionProperties f =
        (PFN_vkEnumerateDeviceExtensionProperties)g_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
    if (pLayerName) return f(physicalDevice, pLayerName, pPropertyCount, pProperties);
    const VkExtensionProperties *wsi;
    uint32_t nw = onnm4_wsi_device_extensions(&wsi), n = 0;
    VkResult r = f(physicalDevice, NULL, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties all[n + nw + 1];
    if ((r = f(physicalDevice, NULL, &n, all)) < 0) return r;
    return merge(n, all, wsi, nw, pPropertyCount, pProperties);
}

ONN_EXPORT PFN_vkVoidFunction onn_icd_proc(VkInstance instance, const char *name) {
    return icd_ready() ? g_gipa(instance, name) : NULL;
}

static PFN_vkVoidFunction own(const char *name) {
    static const struct { const char *name; PFN_vkVoidFunction f; } t[] = {
#define OWN(n) { #n, (PFN_vkVoidFunction)n }
        OWN(vkGetInstanceProcAddr), OWN(vkGetDeviceProcAddr), OWN(vkCreateInstance), OWN(vkDestroyInstance),
        OWN(vkCreateDevice), OWN(vkEnumerateInstanceExtensionProperties), OWN(vkEnumerateDeviceExtensionProperties),
#undef OWN
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(name, t[i].name)) return t[i].f;
    return NULL;
}

/* ---- the generated plain forwards ---- */
#include "onn_vkshim_gen.c"

#ifdef ONN_TRACE
static PFN_vkVoidFunction own_export(const char *name) {
    for (size_t i = 0; i < sizeof onn_gen_exports / sizeof onn_gen_exports[0]; i++)
        if (!strcmp(name, onn_gen_exports[i].name)) return onn_gen_exports[i].f;
    return NULL;
}
#endif
