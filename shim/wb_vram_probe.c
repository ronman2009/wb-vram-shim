/*
 * wb_vram_probe.c —— 原生验证工具（ELF，直接跑，不需要游戏）
 *
 * 作用：把 Vulkan 报出来的整张显存表打出来，用来对比
 *        「不加载 wb-vram-shim」和「加载 wb-vram-shim」两组数值。
 *
 * 用法：
 *   ./wb_vram_probe                       # 基线
 *   VK_INSTANCE_LAYERS=VK_LAYER_WB_vram_shim ./wb_vram_probe
 *   VK_INSTANCE_LAYERS=VK_LAYER_WB_vram_shim WBVRAM_LOG=1 ./wb_vram_probe
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan_core.h>

#define TAG_LAYER "VK_LAYER_WB_vram_shim"

static void mib(const char *label, VkDeviceSize v)
{
    printf("      %-24s %10llu MiB", label, (unsigned long long)(v >> 20));
    if ((v & 0xFFFFF) && v < (1ull << 30))
        printf("   (%llu B)", (unsigned long long)v);
    printf("\n");
}

int main(void)
{
    VkApplicationInfo app = { 0 };
    VkInstanceCreateInfo ci = { 0 };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r;
    uint32_t i, n = 0;
    VkPhysicalDevice *devs = NULL;

    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "wb_vram_probe";
    app.apiVersion         = VK_API_VERSION_1_1;

    ci.sType               = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo    = &app;

    printf("============ wb_vram_probe ============\n");

    /* 层是否可见 */
    {
        uint32_t lc = 0, k;
        VkLayerProperties *lp = NULL;
        int found = 0;
        if (vkEnumerateInstanceLayerProperties(&lc, NULL) == VK_SUCCESS && lc) {
            lp = calloc(lc, sizeof(*lp));
            if (lp && vkEnumerateInstanceLayerProperties(&lc, lp) == VK_SUCCESS) {
                for (k = 0; k < lc; k++)
                    if (strcmp(lp[k].layerName, TAG_LAYER) == 0) found = 1;
            }
        }
        printf("层 %s : %s（共发现 %u 个已安装层）\n",
               TAG_LAYER, found ? "已安装" : "未安装", lc);
        free(lp);
    }
    printf("环境 VK_INSTANCE_LAYERS = %s\n",
           getenv("VK_INSTANCE_LAYERS") ? getenv("VK_INSTANCE_LAYERS") : "(未设置)");
    printf("环境 WBVRAM_*           = margin=%s dryrun=%s log=%s\n",
           getenv("WBVRAM_MARGIN_MB") ? getenv("WBVRAM_MARGIN_MB") : "(默认)",
           getenv("WBVRAM_DRYRUN") ? getenv("WBVRAM_DRYRUN") : "(默认)",
           getenv("WBVRAM_LOG") ? getenv("WBVRAM_LOG") : "(默认)");

    r = vkCreateInstance(&ci, NULL, &inst);
    if (r != VK_SUCCESS) {
        printf("vkCreateInstance 失败: %d\n", (int)r);
        return 1;
    }

    r = vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (r != VK_SUCCESS || n == 0) {
        printf("没有 Vulkan 物理设备 (r=%d, n=%u)\n", (int)r, n);
        vkDestroyInstance(inst, NULL);
        return 1;
    }

    devs = calloc(n, sizeof(*devs));
    vkEnumeratePhysicalDevices(inst, &n, devs);
    printf("物理设备数: %u\n", n);

    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceIDProperties id;
        VkPhysicalDeviceProperties2 p2;
        VkPhysicalDeviceMemoryProperties2 mp2;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget;
        VkPhysicalDeviceMemoryProperties mp1;
        uint32_t h;

        vkGetPhysicalDeviceProperties(devs[i], &props);

        memset(&id, 0, sizeof(id));
        memset(&p2, 0, sizeof(p2));
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &id;
        vkGetPhysicalDeviceProperties2(devs[i], &p2);

        printf("\n---------------- 设备 %u ----------------\n", i);
        printf("  name        : %s\n", props.deviceName);
        printf("  deviceType  : %d\n", (int)props.deviceType);
        printf("  driver      : %u.%u.%u.%u\n",
               VK_API_VERSION_MAJOR(props.driverVersion),
               VK_API_VERSION_MINOR(props.driverVersion),
               VK_API_VERSION_PATCH(props.driverVersion),
               VK_API_VERSION_VARIANT(props.driverVersion));
        printf("  apiVersion  : %u.%u\n",
               VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));

        /* 1.0 路径：只看堆大小 */
        memset(&mp1, 0, sizeof(mp1));
        vkGetPhysicalDeviceMemoryProperties(devs[i], &mp1);
        printf("\n  [vkGetPhysicalDeviceMemoryProperties  (1.0 路径)]\n");
        printf("    memoryTypeCount=%u memoryHeapCount=%u\n",
               mp1.memoryTypeCount, mp1.memoryHeapCount);
        for (h = 0; h < mp1.memoryHeapCount; h++)
            mib((mp1.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    ? "heap DEVICE_LOCAL" : "heap NON_LOCAL", mp1.memoryHeaps[h].size);

        /* 1.1 路径：堆大小 + heapBudget/heapUsage */
        memset(&budget, 0, sizeof(budget));
        memset(&mp2, 0, sizeof(mp2));
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        mp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        mp2.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(devs[i], &mp2);

        printf("\n  [vkGetPhysicalDeviceMemoryProperties2 (+EXT_memory_budget)]\n");
        printf("    memoryHeapCount=%u\n", mp2.memoryProperties.memoryHeapCount);
        for (h = 0; h < mp2.memoryProperties.memoryHeapCount; h++) {
            int local = (mp2.memoryProperties.memoryHeaps[h].flags &
                         VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
            printf("    heap[%u] %s\n", h, local ? "DEVICE_LOCAL" : "NON_LOCAL");
            mib("size", mp2.memoryProperties.memoryHeaps[h].size);
            mib("heapBudget", budget.heapBudget[h]);
            mib("heapUsage", budget.heapUsage[h]);
        }
        printf("\n  提示：若 heapBudget 全为 0，说明该设备没暴露 VK_EXT_memory_budget，\n"
               "        此时 DXVK 会退化为用堆大小当预算。\n");
    }

    free(devs);
    vkDestroyInstance(inst, NULL);
    printf("\n============ 结束 ============\n");
    return 0;
}
