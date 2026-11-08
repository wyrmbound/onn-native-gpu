/* onn_vkshim.h: the interface between libvulkan.so (the onn loader shim) and a WSI module.
 *
 * The shim forwards every Vulkan call to the one ICD (libvulkan_panfrost.so, or $ONN_VK_ICD).
 * A WSI module (onnm4_wsi.c: VK_KHR_surface, VK_KHR_swapchain, VK_KHR_android_surface,
 * VK_EXT_headless_surface) plugs in by defining the hooks below. It must be compiled INTO
 * libvulkan.so (build.sh picks up onnm4_wsi.c from this folder): the shim has weak no-op
 * defaults, and a strong definition in the same link replaces them. Without the WSI file the
 * shim builds and behaves as a plain pass-through.
 *
 * Extension lists: the ICD's, minus the names the WSI module provides or hides, plus the WSI module's.
 *
 * Lookup order for every name (exported symbol, vkGetInstanceProcAddr, vkGetDeviceProcAddr):
 *   1. the shim's own handful (vkGet*ProcAddr, vkCreateInstance, vkDestroyInstance, vkCreateDevice,
 *      vkEnumerateInstanceExtensionProperties, vkEnumerateDeviceExtensionProperties)
 *   2. onnm4_wsi_proc(name): non-NULL wins
 *   3. the ICD
 */
#ifndef ONN_VKSHIM_H
#define ONN_VKSHIM_H
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- hooks the WSI module defines (weak no-op defaults live in onn_vkshim.c) ---- */

/* Return your implementation of `name`, or NULL to let the ICD answer. Called for instance and
 * device lookups alike, so return only the commands you implement. Default: NULL. */
PFN_vkVoidFunction onnm4_wsi_proc(const char *name);

/* Instance extensions you provide. Set *props to a static array, return its length. The shim
 * puts them in place of the ICD's same-named entries in vkEnumerateInstanceExtensionProperties(NULL, ...) and strips
 * them from VkInstanceCreateInfo before the ICD sees it, so the ICD never rejects them.
 * Default: 0. */
uint32_t onnm4_wsi_instance_extensions(const VkExtensionProperties **props);

/* Same for device extensions (VK_KHR_swapchain): appended to vkEnumerateDeviceExtensionProperties
 * (pLayerName == NULL) and stripped from VkDeviceCreateInfo. Default: 0. */
uint32_t onnm4_wsi_device_extensions(const VkExtensionProperties **props);

/* Names the shim drops from the ICD's instance AND device extension lists: PanVK (platforms=android)
 * reports Mesa's own WSI extensions and the ones built on them, but has no swapchain backend here,
 * so they would reach wsi_common with our handles. Set *names to a static array, return its length.
 * Your own extensions (above) are re-added after this filter. Default: 0. */
uint32_t onnm4_wsi_hidden_extensions(const char *const **names);

/* When the app's VkDeviceCreateInfo asks for VK_KHR_swapchain, the shim also enables in the ICD (when
 * the ICD reports them, deduplicated): VK_KHR_external_memory, VK_KHR_external_memory_fd,
 * VK_EXT_external_memory_dma_buf, VK_EXT_image_drm_format_modifier, VK_KHR_image_format_list. */

/* ---- what the shim exports for the WSI module ---- */

/* The ICD's real function, bypassing onnm4_wsi_proc and the shim: vk_icdGetInstanceProcAddr
 * (instance, name). Pass the app's VkInstance (from your vkCreate*SurfaceKHR) or NULL for the
 * global commands. Mesa returns device-level commands here too. NULL if the ICD failed to load.
 * Note: extensions you strip are not enabled in the ICD, so Mesa hides their entry points. */
PFN_vkVoidFunction onn_icd_proc(VkInstance instance, const char *name);

#ifdef __cplusplus
}
#endif
#endif
