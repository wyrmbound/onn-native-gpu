/* vktest: links libvulkan.so (the onn shim) like a port would. On the box:
 *   cd /data/local/tmp/pb && LD_LIBRARY_PATH=$PWD ./linker64 $PWD/vktest      (prints PASS or FAIL) */
#include <stdio.h>
#include <vulkan/vulkan.h>

int main(void) {
    uint32_t v = 0, n = 0;
    VkResult r = vkEnumerateInstanceVersion(&v);
    printf("vktest: vkEnumerateInstanceVersion r=%d -> %u.%u.%u\n", r,
           VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    printf("vktest: %u instance extensions\n", n);
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vktest",
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    if ((r = vkCreateInstance(&ci, NULL, &inst)) != VK_SUCCESS) { printf("vktest FAIL: vkCreateInstance %d\n", r); return 1; }
    VkPhysicalDevice pd[4];
    n = 4;
    r = vkEnumeratePhysicalDevices(inst, &n, pd);
    if (r < 0 || n == 0) { printf("vktest FAIL: vkEnumeratePhysicalDevices r=%d n=%u\n", r, n); return 1; }
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        printf("vktest: gpu %u: %s, Vulkan %u.%u\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
    }
    vkDestroyInstance(inst, NULL);
    printf("vktest PASS\n");
    return 0;
}
