/*
 * dxgi_vram_probe.c — 测量「Windows 程序在 Proton 里能看到多少显存」
 *
 * 依次打印四条路径的结果，覆盖游戏可能用到的所有显存查询方式：
 *   1) IDXGIAdapter::GetDesc()          -> DedicatedVideoMemory / SharedSystemMemory
 *                                          这是 DXVK 从 Vulkan DEVICE_LOCAL 堆推出的值
 *   2) IDXGIAdapter3::QueryVideoMemoryInfo()
 *                                        -> Budget / CurrentUsage / Reservation
 *                                          这是 VK_EXT_memory_budget 的 heapBudget 原样透传
 *                                          现代 DX12 游戏（含 GTA V Enhanced）用它决定画质预算
 *   3) 创建 ID3D12Device 之后再查一次       -> 观察设备创建本身吃掉了多少预算
 *   4) 注册表 HardwareInformation.qwMemorySize
 *                                        -> 老式/WMI 路径，dxdiag 与部分游戏读这里
 *
 * 编译: 见 probe/build.sh
 */

#include <initguid.h>
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>

static void put_utf8(const char *label, const WCHAR *w)
{
    char buf[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof(buf) - 1, NULL, NULL);
    if (n <= 0) buf[0] = '\0';
    printf("  %-26s %s\n", label, buf);
}

static void put_mib(const char *label, UINT64 bytes)
{
    printf("  %-26s %llu B  (%.1f MiB)\n", label,
           (unsigned long long)bytes, (double)bytes / 1048576.0);
}

static const char *seg_name(DXGI_MEMORY_SEGMENT_GROUP g)
{
    return g == DXGI_MEMORY_SEGMENT_GROUP_LOCAL ? "LOCAL (显存)" : "NON_LOCAL (内存)";
}

static void dump_budget(IDXGIAdapter3 *a3, const char *when)
{
    DXGI_MEMORY_SEGMENT_GROUP groups[2] = {
        DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
        DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
    };

    for (int i = 0; i < 2; i++) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info;
        ZeroMemory(&info, sizeof(info));
        HRESULT hr = IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, groups[i], &info);
        printf("  [%s] %s\n", when, seg_name(groups[i]));
        if (FAILED(hr)) {
            printf("      QueryVideoMemoryInfo 失败 hr=0x%08lx\n", (unsigned long)hr);
            continue;
        }
        put_mib("Budget", info.Budget);
        put_mib("CurrentUsage", info.CurrentUsage);
        put_mib("AvailableForReservation", info.AvailableForReservation);
        put_mib("CurrentReservation", info.CurrentReservation);
    }
}

static void dump_registry_memory(void)
{
    HKEY video;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Control\\Video",
                      0, KEY_READ, &video) != ERROR_SUCCESS) {
        printf("  (打不开 HKLM\\...\\Control\\Video)\n");
        return;
    }

    for (DWORD i = 0;; i++) {
        char name[256];
        DWORD nlen = sizeof(name);
        if (RegEnumKeyExA(video, i, name, &nlen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        char sub[300];
        snprintf(sub, sizeof(sub), "%s\\0000", name);
        HKEY inst;
        if (RegOpenKeyExA(video, sub, 0, KEY_READ, &inst) != ERROR_SUCCESS)
            continue;

        BYTE data[64];
        DWORD size = sizeof(data), type = 0;
        if (RegQueryValueExA(inst, "HardwareInformation.qwMemorySize", NULL, &type,
                             data, &size) == ERROR_SUCCESS && size >= 8) {
            UINT64 v = 0;
            memcpy(&v, data, 8);
            printf("  %s\n", sub);
            put_mib("qwMemorySize", v);
        }
        RegCloseKey(inst);
    }

    RegCloseKey(video);
}

static void dump_adapter(IDXGIFactory1 *factory, UINT index)
{
    IDXGIAdapter1 *adapter = NULL;
    if (FAILED(IDXGIFactory1_EnumAdapters1(factory, index, &adapter)) || !adapter)
        return;

    DXGI_ADAPTER_DESC1 desc;
    ZeroMemory(&desc, sizeof(desc));
    if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc))) {
        printf("\n---------------- 适配器 %u ----------------\n", index);
        put_utf8("Description", desc.Description);
        printf("  %-26s 0x%04X\n", "VendorId", desc.VendorId);
        printf("  %-26s 0x%04X\n", "DeviceId", desc.DeviceId);
        printf("  %-26s 0x%08X\n", "Flags", desc.Flags);
        put_mib("DedicatedVideoMemory", desc.DedicatedVideoMemory);
        put_mib("DedicatedSystemMemory", desc.DedicatedSystemMemory);
        put_mib("SharedSystemMemory", desc.SharedSystemMemory);
    }

    IDXGIAdapter3 *a3 = NULL;
    if (SUCCEEDED(IDXGIAdapter1_QueryInterface(adapter, &IID_IDXGIAdapter3, (void **)&a3)) && a3) {
        printf("\n  -- 仅枚举适配器时 --\n");
        dump_budget(a3, "枚举后");

        ID3D12Device *device = NULL;
        HRESULT hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                                       &IID_ID3D12Device, (void **)&device);
        printf("\n  -- D3D12CreateDevice hr=0x%08lx --\n", (unsigned long)hr);
        if (SUCCEEDED(hr) && device) {
            printf("  ID3D12Device 创建成功，再次查询:\n");
            dump_budget(a3, "建设备后");
            ID3D12Device_Release(device);
        }

        IDXGIAdapter3_Release(a3);
    } else {
        printf("\n  (该适配器不支持 IDXGIAdapter3，无法查询显存预算)\n");
    }

    IDXGIAdapter1_Release(adapter);
}

int main(void)
{
    printf("==== dxgi_vram_probe ====\n");
    printf("时间戳(局部): %s\n", "见采集文件名");

    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    char path[MAX_PATH] = {0};
    if (dxgi && GetModuleFileNameA(dxgi, path, MAX_PATH))
        printf("  %-26s %s\n", "本进程 dxgi.dll", path);
    printf("  %-26s %s\n", "nvapi64.dll 可加载",
           LoadLibraryA("nvapi64.dll") ? "是" : "否");

    IDXGIFactory1 *factory = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr) || !factory) {
        printf("CreateDXGIFactory1 失败 hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    for (UINT i = 0; i < 8; i++)
        dump_adapter(factory, i);

    printf("\n---------------- 注册表路径 ----------------\n");
    dump_registry_memory();

    IDXGIFactory1_Release(factory);
    printf("\n==== 结束 ====\n");
    return 0;
}
