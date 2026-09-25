# wb-vram-shim —— 自研显存计算与传递链

Vulkan explicit layer，**纯 C**，不依赖、不修改 Wine / DXVK / vkd3d-proton。
把 Windows 的显存预算逻辑搬到 Linux，让应用看到的是
「物理总量 − 其它进程占用 − 小余量」，而不是驱动那个保守的 `heapBudget`。

---

## 1. 复刻的是 Windows 的哪一段逻辑

Windows 的内核显存管理器（VidMm）自己记账：谁占了多少它都知道，
于是交给游戏的是 `Total − 其它应用占用 − 系统保留`，并且**不把请求方自己的占用算进去**，
所以应用的额度在运行中是稳定的。

Linux 这边，应用拿到的是驱动通过 `VK_EXT_memory_budget` 报的 `heapBudget`：
本机实测 `8160 MiB` 的堆只给 `5861 MiB`，中间那约 800 MiB 既不是其它进程真实占用，
也不是应用自己的分配 —— 是驱动自己的余量。DX12 游戏用它反推「其它软件占了多少」，
然后自我限流，于是就有了设置页那句 `其它软件: 2299 MB` 和红条。

本模块把第一段逻辑补上：

```
可用 = 物理总量(NVML total) − 其它进程占用(首次查询时刻的 NVML used) − 余量(默认 200 MiB)
```

* 余量默认 200 MiB，可用 `WBVRAM_MARGIN_MB` 调（建议 100~300）。
* 刷新时**只升不降**，保证应用额度不会因为自己分配而缩水。
* NVML 不可用时退化为 `DEVICE_LOCAL 堆大小 − 余量`，并如实写日志。

## 2. 传递链：为什么只改一个点就够了

```
vkGetPhysicalDeviceMemoryProperties / ...Properties2        <-- 我们在这里
   |
   +-- memoryHeaps[DEVICE_LOCAL].size  -> 物理总量（抹掉驱动那 32 MiB）
   +-- heapBudget[DEVICE_LOCAL]        -> 上面算出的可用值
   |
   v
DXVK        DedicatedVideoMemory = max(DEVICE_LOCAL heap.size)
            QueryVideoMemoryInfo().Budget = heapBudget     （源码里是原样累加，无二次裁剪）
   v
vkd3d-proton / dxvk-nvapi   共用同一份 DXGI 实现，自动跟着变
   v
游戏
```

关键事实：**DXVK、vkd3d-proton、dxvk-nvapi 都是 Linux 原生库，直接 `dlopen("libvulkan.so.1")`**，
不走 Windows 的 `dxgi.dll`、也不走 `vulkan-1.dll`。
所以在这里改一次，整条链一起变 —— 这比写一个 Windows 侧 `dxgi.dll` 代理（要手写整条
COM vtable 包装）代码量小一个数量级，而且天然同时满足「Wine 优先」与「DXVK 也要覆盖」：
Wine 的 `winevulkan` 同样走宿主的 Vulkan loader。

## 3. 文件

| 文件 | 说明 |
|---|---|
| `wb_vram_shim.c` | 层本体：配置 / NVML 绑定 / 显存计算 / 表改写 / 日志 |
| `wb_vram_probe.c` | 原生验证工具（ELF），把整张显存表打出来，用于对比加载前后 |
| `wb_vram_shim.json.in` | 层清单模板（`library_path` 由 install.sh 填入） |
| `build.sh` | 编译层与验证工具（**零警告**为目标） |
| `install.sh` | 安装/卸载层清单 |
| `selftest.sh` | 不需要 GPU 的离线自检 |

Vulkan 头文件已 vendor 在 `third_party/`（`vulkan/` 与 `vk_video/` 是同级目录，与官方布局一致）。

## 4. 构建 · 部署 · 使用

```bash
./shim/build.sh                 # 编出 libwb_vram_shim.so 与 wb_vram_probe
./shim/selftest.sh              # 离线自检（不用 GPU）
./shim/install.sh               # 装 manifest 到 ~/.local/share/vulkan/explicit_layer.d
```

本层是 **explicit layer**：装了不会自动生效，必须显式打开，因此不影响你不在测试的其他程序。

原生验证（不用开游戏）：

```bash
./shim/wb_vram_probe                                                  # 基线
VK_INSTANCE_LAYERS=VK_LAYER_WB_vram_shim WBVRAM_LOG=1 ./shim/wb_vram_probe
```

游戏（Wine / Proton）启动项，放在 `%command%` 前面：

```
VK_INSTANCE_LAYERS=VK_LAYER_WB_vram_shim WBVRAM_LOG=1 %command%
```

先只记录不改写、确认算出来的数合理，再去掉 `WBVRAM_DRYRUN`：

```
VK_INSTANCE_LAYERS=VK_LAYER_WB_vram_shim WBVRAM_DRYRUN=1 WBVRAM_LOG=1 %command%
```

日志默认写在 `~/.cache/wb-vram-shim.log`。

## 5. 参数

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `WBVRAM_ENABLE` | 1 | 0 = 完全放行 |
| `WBVRAM_DRYRUN` | 0 | 1 = 只记录不改写 |
| `WBVRAM_MARGIN_MB` | 200 | 预留余量，建议 100~300 |
| `WBVRAM_PATCH_HEAP` | 1 | 把 DEVICE_LOCAL 堆大小改成物理总量 |
| `WBVRAM_PATCH_SHARED` | 1 | 处理 NON_LOCAL 段：预算不再打折 |
| `WBVRAM_SHARED_MB` | 0 | >0 时把 NON_LOCAL 堆大小改成该值（0 = 保留驱动值） |
| `WBVRAM_LOG` | 0 | 1 = 同时打到 stderr（Proton 里便于抓取） |
| `WBVRAM_LOG_FILE` | `~/.cache/wb-vram-shim.log` | 日志路径 |
| `WBVRAM_REFRESH_SEC` | 0 | >0 按间隔重算（只升不降） |

## 6. 已经验证 / 尚未验证

**已在本机验证（无 GPU 也行）：**

* 编译零警告；
* 导出表只有 `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` 两个符号（`-fvisibility=hidden`）；
* `selftest.sh`：`dlopen` 成功、构造函数跑到、入口表正确（6 个拦截 / 未知命令返回 NULL 让 loader 继续）；
* manifest 能被 Vulkan loader 识别（`vkEnumerateInstanceLayerProperties` 里出现）。

**尚未验证（需要你在有 GPU 的桌面会话里跑）：**

* 有 GPU 时算出来的数值、以及改写后 DXVK/游戏看到的数字；
* 在 Proton 里是否被 winevulkan 正常加载。

**已发现的一个坑（已规避）：**
`library_path` 含非 ASCII 字符时，loader 会**静默忽略**整个层。
本项目路径含中文 `proton显存问题探究`，所以 `install.sh` 会把 `.so` 复制到
`~/.local/share/vulkan/explicit_layer.d/` 并用相对路径引用。

## 7. 已知限制

* UUID 匹配需要 1.1 及以上的 instance；1.0 时退化为 NVML 设备 0（会写日志）。
* 「其它进程占用」取第一次查询时刻的 NVML `used`，前提是第一次查询来得足够早
  （DXVK 在适配器初始化阶段就会查，足够早）。若担心，可开 `WBVRAM_REFRESH_SEC`。
* **谎报预算不产生显存**：天花板仍是 `物理总量 − 真实其它占用`。
  所以余量别调太小，也别指望全开光追能塞进 8 GB。
* 目前只编译 x86_64。要支持 32 位程序需要在 manifest 里再加一条 `i686` 的条目与一份 32 位 `.so`。
* 不实现 device 级拦截（返回 NULL 交给 loader），因此理论上与其它共存的层兼容。

## 8. 回滚

```bash
./shim/install.sh uninstall          # 移除 manifest 与 .so
```

去掉启动项里的 `VK_INSTANCE_LAYERS=...` 即刻失效，无需改任何系统文件。
