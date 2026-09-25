# wb-vram-shim — 让 Wine/Proton 游戏拿到完整显存

**wb-vram-shim** is a pure-C `dxgi.dll` that installs itself as a **Proton component**
(no game files touched) and reports a **real-time computed VRAM budget** to the game:
`budget = physical total − Σ(other processes' VRAM from NVML) − margin`, refreshed every 500 ms.
Works for DXVK / vkd3d-proton / dxvk-nvapi alike, since they all read memory through the same DXGI surface.

**快速开始 → [`winepart/README.md`](winepart/README.md)**（安装 / 卸载 / 回滚 / 参数，一条脚本搞定）。
本仓库同时保留完整的**调研记录**（下文）：问题现象、上游源码级根因链、每一步实验的实测数据与踩坑复盘。

License: [MIT](LICENSE)

---

# Proton / Linux 显存获取问题 — 调研与实验

目标：搞清楚 **Linux + Proton 下游戏到底能拿到多少显存、拿不到的部分是谁吃掉的**，
用可复现的测量代替推测。当前主案例：GTA V Enhanced（Steam appid `3240220`，DX12 → vkd3d-proton）。

---

## 1. 已确认的事实（来自本机实测，非推测）

| 项 | 观测值 | 来源 |
|---|---|---|
| GPU | NVIDIA GeForce RTX 3070 LHR，PCI `05:00.0` | `lspci` |
| 驱动 | 610.57.04 **open kernel module** | `/proc/driver/nvidia/version` |
| 系统 | Manjaro，kernel `7.1.13-2-MANJARO`，KDE **Wayland**，Ryzen 5 5600X，15 GiB RAM | `/etc/os-release` 等 |
| 游戏渲染路径 | **DX12 → vkd3d-proton**，DXGI 由 **DXVK 的 dxgi.dll** 提供 | 游戏目录含 `vkd3d-proton.cache`、`D3D12-REDIST`、`dstorage.dll` |
| Proton | **Proton Experimental**（`experimental-11.0-20260917b`，wine 11.0）—— 由前缀软链反查确认，见下 | 见 §1.1 |
| 该 Proton 的图形栈 | DXVK `v3.1.1-21-g7df3596e`，vkd3d-proton | `files/lib/wine/dxvk/version` |

### 1.1 更正：GTA 实际用的不是 GE-Proton

早期判断（"GTA 跑在 GE-Proton11-7 上"）**是错的**。证据：

* `steamapps/compatdata/3240220/pfx/drive_c/windows/explorer.exe` 是软链，指向
  `steamapps/common/Proton - Experimental/files/lib/wine/x86_64-windows/explorer.exe`；
* 前缀里 `system32/dxgi.dll` 的 md5（`182768d5…`）与 **Proton Experimental** 的
  `files/lib/wine/dxvk/x86_64-windows/dxgi.dll` 完全一致，而与 GE-Proton11-7 的不同。

所以此后所有针对本游戏的改动都指向 **Proton Experimental**。`winepart/install.sh`
也是靠这条软链自动反查 Proton 的，不需要手工指定。


**关键证据 A — 兼容层交给游戏的显存 ≈ 8 GB：**

`Documents/Rockstar Games/Launcher/launcher.log`（2026-09-24 21:55 这次运行）：

```
 GPU 0
  Description: NVIDIA GeForce RTX 3070 (Low Hash Rate)
  Capacity (Bytes): 8556380160
  Display Capacity: 8.0 GB
```

`8556380160 B = 8160 MiB`，即 8192 − 32 MiB。这就是 DXVK 通过
`IDXGIAdapter::GetDesc().DedicatedVideoMemory` 报给游戏的数字。

**关键证据 B — 注册表里的显存值也正确：**

`pfx/system.reg`：`"HardwareInformation.qwMemorySize"=hex(b):00,00,00,00,02,00,00,00`
= `0x0000000200000000` = **8589934592 B = 8192 MiB**。

> 结论：**"Proton 只给游戏 5 GB" 在本机目前没有证据支持**，能查到的三条路径
> （DXGI 适配器描述、注册表 qwMemorySize、Rockstar 启动器自检）都报 ~8 GB。
> 还需要确认"5 GB"这个数字究竟是从哪个界面/工具读出来的。

**关键证据 C — 现有启动项里有一个不存在的开关：**

`userdata/1297549117/config/localconfig.vdf`：

```
"LaunchOptions"  "VKD3D_CONFIG=\"dxgi_override_vram=7168,no_upload_hvv\" %command%"
```

* `no_upload_hvv` —— 真实存在的 vkd3d-proton 选项（阻止 UPLOAD 堆占用 host-visible VRAM）。
* `dxgi_override_vram` —— **在本机 vkd3d-proton 二进制里搜不到这个字符串**（0 命中），
  上游 `VKD3D_CONFIG` 选项表里也没有它。DXGI 显存上报是 **DXVK** 的职责，选项名是
  `dxgi.maxDeviceMemory` / `dxgi.maxSharedMemory`，且必须走 `DXVK_CONFIG`（或 `dxvk.conf`），
  不是 `VKD3D_CONFIG`。→ 这个 `7168` 大概率一直没生效。

---

## 2. 兼容层拿到显存的机制（上游源码）

`DXVK::DxvkAdapter::info()` —— 决定 `DedicatedVideoMemory`：

```cpp
if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
  // Assume that the largest available heap is the total amount of available VRAM.
  result.deviceMemory = std::max(result.deviceMemory, memory.memoryHeaps[i].size);
} else {
  result.systemMemory += memory.memoryHeaps[i].size;
}
```

`DXVK::DxvkAdapter::getMemoryHeapInfo()` —— 决定 `QueryVideoMemoryInfo` 的 budget：

```cpp
info.heaps[i].memoryBudget = memBudget.heapBudget[i];   // 直接来自 VK_EXT_memory_budget
```

也就是说：

* **"总显存"** = NVIDIA 驱动上报的 DEVICE_LOCAL **Vulkan 堆大小**（DXVK 不做任何扣减）。
  本机 = 8160 MiB。差的那 32 MiB 是驱动行为，不是 Proton 行为。
* **"可用预算"** = 驱动通过 `VK_EXT_memory_budget` 给的 `heapBudget`，会被原样转给游戏。
  这里才是"看起来只有 5 GB"最可能的地方 —— 但**值来自驱动，不是兼容层**。

可用的调节旋钮（都属于 DXVK，需要 `DXVK_CONFIG` / `dxvk.conf`）：

| 选项 | 作用 |
|---|---|
| `dxgi.maxDeviceMemory` / `dxgi.maxSharedMemory` | 覆盖**上报**给游戏的显存值（单位 MB，非硬上限） |
| `dxgi.maxMemoryBudget`（上游文档写作 `dxvk.maxMemoryBudget`） | 限制 DXVK **实际使用**的上限，官方标注为 debug 选项 |
| `dxgi.hideNvidiaGpu` | 把 NVIDIA 卡报成 AMD 卡（默认 Auto） |
| `dxgi.customVendorId` / `customDeviceId` / `customDeviceDesc` | 伪造适配器标识 |

vkd3d-proton 侧（`VKD3D_CONFIG`，本机二进制中实测存在的选项）：
`vk_debug, skip_application_workarounds, debug_utils, force_static_cbv, dxr, single_queue,
descriptor_qa_checks, nodxr, fault, no_upload_hvv, log_memory_budget, force_host_cached,
app_debug_marker_only, no_invariant_position, global_pipeline_cache,
pipeline_library_no_serialize_spirv, pipeline_library_sanitize_spirv`

其中 `log_memory_budget` 会打印显存预算，是本次实验最重要的现成仪器。

---

## 3. 实验设计

### 阶段 0（已有结论）
读游戏自身日志/注册表 → 兼容层上报 ~8 GB。**已完成**，见上。

### 阶段 0.5 — "5 GB" 的来源已定位（2026-09-24 21:39 实机截图）

游戏内「设置 → 图形」原文：

```
视频内存: 5882 MB / 8160 MB        ← 条是红的
其它软件: 2299 MB
输出适配器: NVIDIA GeForce RTX 3070 (Low Hash Rate)
分辨率: 2560 x 1440 60Hz
```

同一分钟的 nvtop 实测（`GPU 0` 行 + 进程表）：

```
Memory-Usage: 6645MiB / 8192MiB
  Xorg                36MiB
  kwin_wayland        31MiB
  kwin_wayland       137MiB
  plasmashell        137MiB
  steam               47MiB
  steamwebhelper     489MiB
  explorer.exe       169MiB     <- wine
  GamesLauncher.exe  180MiB     <- wine
  SocialClub...      143MiB     <- wine
  GTA5_Enhanced.exe 5146MiB
  ----------------------------
  非游戏合计         ≈1499MiB
```

**结论：**

| 数字 | 含义 | 归因 |
|---|---|---|
| 8160 MB | 游戏看到的**总显存** | 驱动上报的 DEVICE_LOCAL 堆，**Proton 没有扣** |
| 5882 MB | 当前画质设定的**预估占用** | 游戏自己算的（1440p + 光追全开） |
| 2299 MB | 游戏认为**别的软件占了** | `总显存 − QueryVideoMemoryInfo.Budget` |
| 8160−2299 = **5861 MB** | 游戏认为自己**能用的上限** | ← 这就是"只有 5G"的来源 |

**已证实的现象（2026-09-25）：** 默认分配逻辑下**兼容层拿不到完整显存**。装补丁前后实测：

| | 装 v1 前 09-24 21:39 | 装 v1fix 后 09-25 00:41 |
|---|---|---|
| 游戏实测占用 | **5146 MiB**（≈5.0 GiB） | **6364 MiB**（≈6.2 GiB） |
| 游戏看到的额度 | 5861（界面显示） | 6656（= 8192−1536，我们写的常量） |
| 占用率 | 87.8% | 95.6% |

**净增 = +1218 MiB。**

**尚未证实的是"为什么"。** 目前只有两个端点有物证：

- 启动瞬间（游戏**还没分配**）驱动给 **7109** = 8192 − 实测其它占用 1070 —— 日志里有；
- 游戏运行中驱动**原始**的 budget 是多少、怎么随时间变 —— **一次都没采样过。**

所以"额度随游戏分配一路衰减"只是**候选假设**，不是结论。那 800 MB 的归属
（驱动预留 / 采样时机 / DXVK 池化）目前**仍未排除**。唯一的取证手段是
`WBVRAM_DRYRUN=1`（v2.1 提供）：不干预地跑游戏，读运行中的驱动原值序列。

⚠ 方法教训两条：
1. 判定得失必须在"**游戏已分配后**"取数。启动瞬间的 7109 不能当基准 ——
   拿它比会得出"补丁反而扣了 453"的错误结论（我犯过）。
2. 两个端点之间的形状**不可推断**。画图时没采样支撑的曲线必须标注"示意"。

### 21:50 / 21:51 两次尝试为什么没生效（Steam 日志原文）

`~/.local/share/Steam/logs/console-linux.txt`：

```
[2026-09-24 21:50:35] /bin/sh: line 1: dxgi.maxDeviceMemory=7168: command not found
[2026-09-24 21:51:46] /bin/sh: line 1: dxgi.maxDeviceMemory=7168: command not found
```

`console_log.txt` / `gameprocess_log.txt` 里对应的启动命令行显示，
`DXVK_HUD=memory,devinfo` 被写在 `...proton waitforexitandrun '<游戏>/PlayGTAV.exe'`
**之后**，于是它变成了游戏的一个命令行参数，而不是环境变量。

根因：`dxgi.maxDeviceMemory=7168` 里含 `.`，不是合法的 shell 变量名，`sh` 把它当成
"要执行的命令"；而 DXVK 的选项必须包在 `DXVK_CONFIG="..."` 里。

### 正确的启动项写法

```
DXVK_CONFIG="dxgi.maxDeviceMemory=7168" VKD3D_CONFIG="no_upload_hvv" DXVK_HUD=memory,devinfo %command%
```

要抓驱动给的真实预算，再加日志落盘（`results/` 已开放写权限）：

```
DXVK_HUD=memory DXVK_LOG_PATH=/home/ronman/WorkBuddy/proton显存问题探究/results DXVK_LOG_LEVEL=debug VKD3D_CONFIG="no_upload_hvv,log_memory_budget" %command%
```

`DXVK_HUD=memory` 屏幕左上角会显示 `Memory: 已用/预算 MB`，那个"预算"就是
`VK_EXT_memory_budget` 的 `heapBudget` 原值 —— 它和 8160 的差就是"看得见拿不到"的那块。

> **`dxgi.maxDeviceMemory` 不是解药。** 源码 `DxgiAdapter::GetAdapterDesc()` 里它只裁剪
> `DXGI_ADAPTER_DESC` 的静态值；而 `QueryVideoMemoryInfo()` 里 Budget 是
> `pVideoMemoryInfo->Budget += memInfo.heaps[i].memoryBudget;`，**不经过任何选项**。
> 设小它只会让游戏看到的"总显存"变小，上限一点没松。机制见下面 2.5。

**另外**：`settings.xml` 里 `Raytracing_Enabled=true`，RT 阴影/AO/反射/GI 全开，
`RTReflection_FullRes_Enabled=true`，2560×1440 —— 这套设定在 8 GB 卡上本来就贴着上限，
进度条变红属于预期。

## 2.5 为什么上限是 5861 MB 而不是 8192 MB（源码级链条）

```
8192 MiB  物理显存                                    nvidia-smi
  |  驱动保留 32 MiB（Windows 上同样保留）
  v
8160 MiB  Vulkan DEVICE_LOCAL 堆大小                  -> DXGI_ADAPTER_DESC.DedicatedVideoMemory
  |        DXVK: result.deviceMemory = max(heap.size)     不做任何扣减
  |
  |  驱动通过 VK_EXT_memory_budget 给出动态预算：扣除"当前占用 + 自己的余量"
  v
5861 MB   VK_EXT_memory_budget.heapBudget             -> DXGI QueryVideoMemoryInfo().Budget
  |        DXVK: info.heaps[i].memoryBudget = memBudget.heapBudget[i]      原样透传
  |
  v
8160 - 5861 = 2299 MB   <- 游戏界面上「其它软件」那一行
```

**结论：Proton / DXVK / vkd3d 一个字节都没扣。**
`GetAdapterDesc()` 只做"2 的幂次修正"和（默认不触发的）可选项裁剪；
`QueryVideoMemoryInfo()` 里 Budget 是纯累加，没有任何选项参与。
上限 = NVIDIA 驱动给出的 `heapBudget`。

那么 `nvidia-smi` 里那 ~1.5 GB "free" 为什么用不上？
因为它是对**物理 8192** 算的，而 app 的天花板是**驱动算出来的 budget**。
两者之间的差额被三段吃掉：

| 吃掉的部分 | 实测 | 能否拿回来 |
|---|---|---|
| 其它进程真实占用 | 09-25 实测 **1070 MiB**（Xorg 68 + kwin 34 + plasmashell 147 + Steam 101 + wine 附属 325 + …） | 能，关掉即还，budget 随之上升 |
| **游戏自己已分配的显存** | 装 v1fix 后实测 **6364 MiB** | **这就是本补丁要处理的**：它会把 budget 一路压下去 |
| DXVK / vkd3d 自身的池化分配 | 未测 | 驱动把它算进"已用"，**会连带压低 budget**；DXVK 在**上报口径**上已把这块从"你的占用"里减掉，但驱动那一侧的 budget 不受影响 |

**合并结论**：`budget ≈ 8192 − 其它进程占用 − 本进程已分配`，
所以它是一个**会随游戏自己越用越小的动态值**。补丁把它钉成常量 6656，
游戏才敢分配到 6364（95.6%）而不是 5146（87.8%）。
**没有"驱动保守预留 800 MB"这回事** —— 那 800 MB 是游戏自己已经吃掉的那部分。

**唯一还没排除的"兼容层自己吃掉一块"的地方 —— NVAPI 分支：**

`dxvk-nvapi` 的 `NvAPI_GPU_GetMemoryInfo`（`src/nvapi_gpu.cpp`，本机版本 v0.9.2-89）：

```cpp
availableDedicatedVideoMemory    = DedicatedVideoMemory - ReservedVideoMemory;
curAvailableDedicatedVideoMemory = std::min(memoryBudgetInfo.Budget,
                                            DedicatedVideoMemory - ReservedVideoMemory);
```

即 `其它软件 = max(总 - Budget, ReservedVideoMemory)`。
`ReservedVideoMemory` 是 Proton 侧（DXVK / DXVK-NVAPI）自己填的保留值 ——
若它 > 2299 MB，主约束的性质就从"驱动行为"变成"兼容层行为"。
它确切来源（dxvk-nvapi 适配器实现）尚未拿到，是下一步要挖的点。

**可动的杠杆（按性价比排序）：**

1. 关掉 steamwebhelper（单进程 489 MiB）、Wallpaper Engine、浏览器 —— budget 随实际占用回升，零风险。
2. BIOS 关 `Resizable BAR` / `Above 4G Decoding` —— 去掉驱动的 BAR 预留，需 A/B 实测。
3. 保留 `no_upload_hvv`（把 UPLOAD 堆从显存挪到系统内存）。
4. 放弃 `dxgi.maxDeviceMemory` 这条路（源码证明它不碰 Budget）。
5. 画质侧：1440p + 光追全开本身就奔着 5.8 GB 去，这是预期行为，不是 bug。

### 阶段 1 — 主机基线（原生 Vulkan + 驱动）
`scripts/00-host-baseline.sh` 采集：`nvidia-smi` 全部显存字段、BAR1/ReBAR 状态、
内核驱动信息、`vulkaninfo` 的 memoryHeaps、以及空闲时桌面占用的显存。
分别在 **空闲时** 和 **游戏运行时** 各跑一次。

要回答：驱动上报的 DEVICE_LOCAL 堆 = ? `heapBudget` 随占用如何变化？

### 阶段 2 — Proton 兼容容器里的显存探测
`scripts/10-proton-container.sh` 用 GE-Proton 建一个**独立**前缀（不碰游戏存档），
运行 `probe/dxgi_vram_probe.exe`：打印 DXGI 适配器描述、`IDXGIAdapter3::QueryVideoMemoryInfo`
（LOCAL/NON_LOCAL 的 Budget / CurrentUsage）、D3D12 设备创建前后的差值、注册表 qwMemorySize。

要回答：容器内 app 视角的"总显存/预算"分别是多少？与阶段 1 是否一致？

### 阶段 3 — 回到真实游戏
给 GTA V Enhanced 临时加 `VKD3D_CONFIG="log_memory_budget"` + `DXVK_LOG_LEVEL=debug`，
收集日志与游戏内画面设置里显示的显存数值；用 `nvidia-smi -l 1` 采样真实占用曲线。

要回答：游戏自己算出来的预算 = ? 爆满时是谁在涨？

---

## 3.5 方案：给 DXVK 打「LOCAL 预算下限」补丁（**未采用，留作备选**）

> 已被 `winepart/`（自研 dxgi.dll，不改上游源码）取代。保留是因为它记录了
> 当初对 `QueryVideoMemoryInfo` 的定位过程，逻辑与 winepart 版一致。

第一阶段衰减（8192 → 8160）动不了，但第二阶段（8160 → 5861）是**驱动的实时衰减**
（不是固定预留，见 2.5 节）——**可以在兼容层侧把额度钉成常量顶回去**。
`patch/` 目录里已经写好：

| 文件 | 作用 |
|---|---|
| `patch/apply-budget-patch.py` | 在 `src/dxgi/dxgi_adapter.cpp` 的 `QueryVideoMemoryInfo()` 插入「LOCAL 段预算下限」逻辑；幂等，支持 `--revert` |
| `patch/build-dxvk.sh` | clone/checkout DXVK → 打补丁 → mingw 交叉编译 → 替换 Proton 目录里的 `dxgi.dll`（原文件备份为 `.orig`） |
| `patch/selftest/` | 离线自检固件（不参与编译），已实测：插入位置正确 / 重复执行为空操作 / 撤销后与原文件逐字节一致 |

改动内容（在 `Budget` 累加之后）：

```diff
       pVideoMemoryInfo->AvailableForReservation += memInfo.heaps[i].heapSize / 2;
     }
 
+    // --- LOCAL budget floor override -------------------------------------
+    static const VkDeviceSize s_localBudgetFloor = [] () -> VkDeviceSize {
+      const char* env = std::getenv("DXGI_LOCAL_BUDGET_MB");
+      return env ? (VkDeviceSize(std::strtoull(env, nullptr, 10)) << 20) : 0u;
+    } ();
+
+    if (MemorySegmentGroup == DXGI_MEMORY_SEGMENT_GROUP_LOCAL
+     && s_localBudgetFloor > pVideoMemoryInfo->Budget)
+      pVideoMemoryInfo->Budget = s_localBudgetFloor;
+
     // We don't implement reservation, but the observable
```

只抬高、不下调；不设环境变量时与官方行为**完全一致**。

用法：启动项里加 `DXGI_LOCAL_BUDGET_MB=6400 %command%`。

**为什么不是从零写一个代理 DLL**：要拦 `IDXGIAdapter3::QueryVideoMemoryInfo`，必须把
`IDXGIFactory` → `IDXGIAdapter` 整条 COM 链都包装一遍（vtable + 引用计数 + 各版本 IID），
500 行起。而 DXVK 的 `dxgi.dll` 本来就是那个产物 —— 改一行再编一次，产物同样是 `dxgi.dll`，
风险面小一个数量级。

**为什么不能直接设 8160**：见 2.5 的表格 —— 桌面/Steam/wine 真实占用约 1.4 GB，
实际可用天花板约 **6600~6900**。谎报到 8160 只是把「游戏主动缩池」换成
「分配失败 / 帧生成卡顿 / 贴图丢失」。建议 6400 起步，每档 +256 往上试。

**风险与前置**：

- 依赖 `mingw-w64-gcc meson ninja glslang`（`build-dxvk.sh` 顶部有检查和安装提示）。
- 必须替换 **Proton 目录里**的 `dxgi.dll`（`files/lib/wine/dxvk/x86_64-windows/`），
  不是 prefix 里的 —— Proton 每次启动都会把自带 DXVK 重新摊进 prefix，手改 prefix 会被覆盖。
- 回滚：脚本结尾给了命令（把 `*.orig` 挪回去）。
- **建议顺序**：先用 `DXVK_HUD=memory` 跑一次，读驱动给的**真实 `heapBudget`**。
  - HUD 显示预算 ≈5861 → 游戏的「其它软件 2299」= `8160 − Budget`，本补丁直接生效。
  - HUD 显示预算本来就接近 6600 → 那 2299 来自 NVAPI 分支
    （`curAvailableDedicatedVideoMemory = min(Budget, total − ReservedVideoMemory)`），
    补丁要改到 `dxvk-nvapi` 上去。

## 3.6 实际采用的方案：自研 Vulkan 层 `shim/`（wb-vram-shim）

按用户要求改为**自研组件**：纯 C、不动 Wine/DXVK 源码、一条自己算 + 自己传的链路。
详见 `shim/README.md`，这里只放结论。

**复刻的 Windows 逻辑：**

```
可用 = 物理总量(8192) − 所有其它进程的显存占用(NVML 逐进程枚举，cmdline 认自己剔除) − 余量(默认 200 MiB)
```

**为什么挂在 Vulkan 层而不是写一个 `dxgi.dll`：**
（**机制更正**：DXVK 并不是 Linux 原生库直接 dlopen `libvulkan.so.1` —— 实测 DXVK 目录里
一个 `.so` 都没有，它是纯 PE，`d3d11.dll` 直接 `import vulkan-1.dll`。真实链路是
`DXVK(PE) → vulkan-1.dll(PE,Wine builtin) → winevulkan.so → 宿主 libvulkan.so.1`。
结论不变：最后一跳是宿主 loader，所以 Vulkan 层照样生效。）<br>
因此拦截 `vkGetPhysicalDeviceMemoryProperties[2]` 一处，整条链一起变：

```
堆大小(DEVICE_LOCAL) → DXVK DedicatedVideoMemory
heapBudget           → DXVK QueryVideoMemoryInfo().Budget → vkd3d / nvapi → 游戏
```

而且 Wine 的 `winevulkan` 同样走宿主 loader，所以 Wine 里一样生效 —— 一处改动同时覆盖
「Wine 优先」和「DXVK 也要管」。这比手写整条 COM vtable 包装少一个数量级的代码。

**已交付并已离线验证：** 编译零警告；导出表仅两个入口符号；`selftest.sh` 通过
（dlopen、构造函数、入口表）；manifest 被 Vulkan loader 识别。

**过程中的一个真实坑：** `library_path` 含非 ASCII 字符时 loader 会**静默忽略整个层**。
本项目路径含中文，所以 `install.sh` 把 `.so` 复制到
`~/.local/share/vulkan/explicit_layer.d/` 并用相对路径引用。

**尚未验证：** 有 GPU 时的实际数值（沙箱拿不到 `/dev/dri`），需要在本机桌面会话跑
`wb_vram_probe` 与游戏。
（注：同算法的 `winepart/` 版已上机通过，见第 6 节；`shim/` 这个 Vulkan 层形态本身仍未上机。）

> `patch/`（给 DXVK 打补丁那套）**未被采用**，保留作为备选参考，不再作为主路线。

## 3.7 你要的形态：drop-in `dxgi.dll`（`dropin/`）

和 DXVK 一样：**一个 PE 文件复制到游戏根目录就行**，不装层、不改启动项。

```
Grand Theft Auto V Enhanced/dxgi.dll     <- 约 23 KB
```

**工作机制**

1. 主程序目录优先于 `system32` → 顶替 Proton 装在前缀里的 DXVK `dxgi.dll`。
2. 把 `system32\dxgi.dll` 复制到 `%TEMP%` **换个名字**加载（避免与自身同名冲突），
   原样转发 DXVK 的那 5 个导出。
3. 首次转发时，在自己的进程里把 DXVK **适配器类的虚表** 5 个槽位换掉：
   `GetDesc(8)` / `GetDesc1(10)` / `GetDesc2(11)` / `QueryVideoMemoryInfo(14)` / `GetDesc3(16)`。
   每个钩子都是「先调用原实现，再改返回值」。
   因为改的是**进程内唯一**的类虚表，所以游戏和 `vkd3d-proton` 谁拿到适配器都生效。
4. 数值：`total` 读注册表 `HardwareInformation.qwMemorySize`（本机 8192 MiB）；
   `budget = total − reserve`，`reserve` 默认 1536 MiB（照 nvtop 实测的桌面+Steam+Wine 开销定的）。

**构建链（全本机，免 root / 免联网 / 免 mingw）**

| 环节 | 用什么 |
|---|---|
| 编译 | `clang --target=x86_64-pc-windows-gnu` → COFF 目标文件 |
| 链接 | `ld -m i386pep`（本机 binutils 支持 `pei-x86-64`） |
| 导入库 | `/usr/lib/wine/x86_64-windows/libkernel32.a` / `libadvapi32.a`（Wine 自带） |

源码**不 include 任何头文件**（否则要拖 mingw 的 libc），类型与导入全部自带声明。

**已离线验证**：零警告；`file` + `objdump -p` + **`winedump`** 三方确认是合法 PE32+ DLL；
**导出表与 DXVK 完全一致**（5 个）；导入表只有 `advapi32`(4) + `kernel32`(11)。

**⚠️ 反作弊**：GTA Online 带 BattlEye，自定义 DLL 进游戏目录有被判篡改的风险。
建议只在故事模式用，进线上前删除（`./dropin/deploy.sh uninstall`）。

**与 `shim/` 的关系**：两边用的是同一套「Windows 账」算法，只是打包形态不同——
`dropin/` 是你要的单文件 drop-in，`shim/` 是 Vulkan 层版（覆盖所有 Vulkan 程序、
数值可动态算，但要装一次）。

## 3.8 最终形态：作为 **Proton/Wine 的部件**安装（`winepart/`）

要求是"让它作为 Wine 的部件，不改动游戏文件"。查完 Proton 的安装机制后，可行的挂载点只有
一个，而且理由很硬：

**Proton 的 DXVK dxgi 走的是 `n`（只用 native）。** `proton` 脚本 1396 行：

```python
for f in dxvkfiles:
    try_copy(g_proton.arch_pe_dir("wine/dxvk", False) + f + ".dll", "drive_c/windows/system32", ...)
    g_session.dlloverrides[f] = "n"          # <- n = 只查 native
```

再加上脚本 1373 行会把 `dxgi` 放进这个复制名单，以及 2065-2067 行把用户给的
`WINEDLLPATH` **追加在末尾**：

```python
dllpaths = [g_proton.lib_dir + "vkd3d", g_proton.lib_dir + "wine"]
if "WINEDLLPATH" in os.environ:
    dllpaths.append(os.environ["WINEDLLPATH"])     # <- 我们只能排最后
self.env["WINEDLLPATH"] = ':'.join(dllpaths)
```

于是"丢进 WINEDLLPATH 让 Wine 当 builtin 加载"这条路是死的：`dxgi=n` 时 Wine 压根不看
builtin 目录，而且我们的目录排在 Proton 自己的 builtin 目录后面。

**唯一可行且正确的挂载点**，就是 Proton 的 DXVK 源目录 —— 让 Proton **自己**把我们的文件
复制进前缀：

```
<Proton>/files/lib/wine/dxvk/x86_64-windows/dxgi.dll           <- 换成我们的（native，被 Proton 亲手上架）
<Proton>/files/lib/wine/dxvk/x86_64-windows/wbvram_dxvk_dxgi.dll   <- DXVK 原件备份（名字不参与复制名单）
<pfx>/drive_c/windows/system32/wbvram_dxvk_dxgi.dll            <- 原件副本（自愈用）
```

好处：**游戏目录一个字节不动；启动项不用改；前缀里那份不会被覆盖**（因为源头就是它）。

**为什么这不是"改游戏文件"**：改动只落在 Proton 自己的安装目录里，游戏目录干净 ——
BattlEye 那类"比对游戏目录文件完整性"的检查看不到任何异常。

### 与 `dropin/` 相比，还多了两个安全阀

| 安全阀 | 作用 |
|---|---|
| **appid 白名单** | 这份 DLL 对"用这个 Proton 的所有游戏"可见。默认只对 `3240220` 生效，其它游戏**一字节都不改**，只做纯转发。`WBVRAM_ALL=1` 放开。 |
| **虚表健全性校验** | 打补丁前先按原槽位真调一次 `GetDesc` / `QueryVideoMemoryInfo`，数值不合理（DXVK 改了虚表布局、拿错对象）就**放弃改写并记日志**，绝不在没验证过的情况下动指针。 |

### 真实 dxgi 的定位链（自愈）

1. `WBVRAM_REAL_DLL` 环境变量
2. `<system32>\wbvram_dxvk_dxgi.dll`（安装脚本放的，最确定）
3. `STEAM_COMPAT_TOOL_PATHS` 里的每个 Proton 目录 → `Z:\…\files\lib\wine\dxvk\x86_64-windows\wbvram_dxvk_dxgi.dll`
   （前缀重建、副本丢了也能自愈）
4. 交给 Wine 按名找 `wbvram_dxvk_dxgi.dll`
5. 兜底 `system32\dxgi.dll`（`dropin/` 形态下那一份才是 DXVK 的；解析到自身会被丢弃）

### ⚠️ v1 上机失败 + v2 修复（2026-09-24 晚）

v1 装上去后实机日志给出致命一行：

```
CreateDXGIFactory1 失败 hr=80004002          ← E_NOINTERFACE
```

根因：v1 自己拿一个**写死的 IID** 去调 `CreateDXGIFactory1`，而那个值是
`{7b7166ec-…}` = **IDXGIFactory**（不是 IDXGIFactory1）。DXGI 规定该函数只接受
IDXGIFactory1 及其派生接口 → 返回 `E_NOINTERFACE` → 适配器枚举失败 →
**虚表一个指针都没换，数字一个都没改**。所以当时"看起来有点效果"是错觉。

v2 改成**不猜 IID**：转发 `CreateDXGIFactory*` 时先按**调用者自己的 riid** 建工厂，
再拿这个真实工厂 `EnumAdapters(0)` 取适配器打补丁。同时加固：4 个 GetDesc 槽位
逐个放大缓冲探测校验、日志改追加带 pid、新增 `WBVRAM_DRYRUN=1` 纯观测、
候选自检（解析 DXVK 原件时拒绝任何含我们标记的候选）。

> **v2.2–v2.4 曾试过"从驱动预算反推其它占用"，已被实机日志证伪**（游戏 16→2265 MiB 时
> 驱动预算只在 6645→6506 微动，`Budget = total − others − CurrentUsage` 等式不成立），
> **整套已删除**。v3.0 改成直接枚举各进程占用，见下。

**顺带查明**：Wine 的 NVML 垫片（`nvml.dll` PE + `nvml.so` unix 侧，
dlopen 宿主 `libnvidia-ml.so.1`）**只有 GE-Proton11-7 自带**，
Proton Experimental 里没有 → `install.sh` **默认**会把它拷过去。

### ⚠️ 2026-09-25 00:0x 又把游戏搞挂了（v2.1 修）

装完 v2 游戏起不来。日志显示 v2 把 `system32\wbvram_dxvk_dxgi.dll` 当成了 DXVK 原件，
**而那是 28466 字节的 v1 代理 DLL**（官方原件 4882432 字节）。链条：

```
v2(system32\dxgi.dll) → v1(system32\wbvram_dxvk_dxgi.dll) → v2 → v1 → …  互相转发到爆栈
```

**根因在安装脚本**：它用**哈希**判断"当前 dxgi.dll 是不是我们的"。装 v2 时 Proton 里躺着 v1，
哈希对不上 → 把 **v1 误当成"官方原件"备份**，**真原件被覆盖丢了**。

三处修复：① 身份判定改成**内容标记**（我们的 DLL 都含 UTF-16 串 `wb-vram-shim`，
官方没有，跨版本稳定）；② 备份一经登记不再覆盖，发现备份本身是我们自己就**拒绝安装**；
③ DLL 解析原件时**拒绝任何含我们标记的候选**（`[候选拒绝]`），最坏情况安静返回 `E_FAIL`。

**官方原件救回来了**：`compatdata/1493710` 与 `1551360` 两个前缀里各躺着一份
md5 = `182768d50de7624b68975c88a60c1a55`（与官方逐字节一致）的副本，
`winepart/rollback.sh` 会自动优先挑这种"与官方一致"的源来恢复。

### v1fix 对照组（v1 只修 IID）

v1 的源码已被 v2 覆盖，所以对照组直接对 **v1 的原始二进制**做最小补丁：
**仅改 16 字节**（offset `0x3e90`，`IDXGIFactory` → `IDXGIFactory1`），
其余逐字节相同 —— 改动范围可证明，便于归因"到底是不是 v2 的问题"。

### 用法

```bash
./winepart/build.sh             # 编译（本机 clang+ld，零警告）
./winepart/hosttest/run.sh      # 宿主端测试：假 DXVK + 假 NVML + 假 /proc/self/stat，13 个场景
./winepart/selftest.sh          # 安装器自检：假 Steam 树，44 项断言
./winepart/install-v2.sh        # ★ 一键装 v3（默认连 NVML 垫片一起装）
./winepart/install-v2.sh --status
./winepart/install.sh uninstall

./winepart/rollback.sh          # ★ 出问题先跑这个：恢复官方 dxgi.dll 并清掉我们的痕迹
./winepart/rollback.sh --dry-run
./winepart/v1fix/install.sh     # 对照组：v1 + 仅修 IID（A/B 归因用）
```

### 算法（默认，装完即生效，不用写启动项）

```
others = 所有进程的显存占用之和，**剔除本进程自己**（NVML 逐进程枚举）
budget = 物理总量(8192) − others − WBVRAM_MARGIN_MB(默认 200)
```

认自己靠读 `Z:\proc\self\stat` 拿本进程的 Linux PID —— NVML 报的就是 Linux PID。
每 500ms 重算一次，桌面占用变化会实时反映到额度上。

**不在驱动预算上做减法**：实测证明驱动预算不随本进程用量等量下降
（游戏 16→2265 MiB 时预算只在 6645→6506 微动），在错的数上减，怎么减都不好用。

可选的开关（都不用重装，加到启动项 `%command%` 前面）：

| 开关 | 作用 |
|---|---|
| 不加任何东西 | **默认就是这样**：NVML 逐进程枚举 + 自己算 |
| `WBVRAM_MARGIN_MB=200 %command%` | 改安全垫（默认 200） |
| `WBVRAM_DYNAMIC=0 %command%` | 退回固定余量 `8192 − WBVRAM_RESERVE_MB(默认 1536)` |
| `WBVRAM_DRYRUN=1 %command%` | 只观测不改写 |

**已验证（离线）**：干净重建零警告；导出表 5 个与 DXVK 完全一致；导入只有
`advapi32`+`kernel32`；`winedump` 确认合法 PE32+；**宿主端 13/13 场景**（`badriid` 复刻
DXVK 的 IID 校验，`nested`/`all-ours` 复刻 00:0x 那次污染事故，`dynamic`/`default`/`nopid`/
`noprocs` 覆盖主路径与三条兜底）；**安装器 44/44 断言**。

**✅ 已上机通过（2026-09-25 00:35，v1fix 对照组）**：游戏正常启动运行，无崩溃。日志
`results/wbvram-v1fix-ok.log.txt`：

```
探测通过：DedicatedVideoMemory=8160 MiB, Budget=7109 MiB, Usage=0 MiB
已挂钩适配器虚表: GetDesc=… QueryVideoMemoryInfo=…
计算结果: total=8192 MiB  reserve=1536 MiB  => budget=6656 MiB
```

游戏内：分母 `8160 → 8192`、游戏实测占用 `5146 → 6364 MiB`（**+1218**）。

**仍未做（关键）**：
1. **v3.0 没在真机上装过** —— 主路径要 NVML 垫片（默认会装），而垫片是从
   GE-Proton11-7 **移植**到 Proton Experimental，**ABI 兼容性未验证**。
2. 首次上机要确认：游戏能起、日志里有 `NVML 就绪` + `本进程 Linux PID=…`、
   `[NVML] … 除本进程外合计=X` 跟 nvtop 对得上。

> **⚠️ 两个必须知道的失效条件**：① Steam「验证 Proton Experimental 文件完整性」或 Proton
> 自动更新会把 `dxgi.dll` 换回官方版；② 改用另一个 Proton 时这份不会跟着走。
> 两者都用 `./winepart/install.sh status` 一查就知道，重跑 `install` 即可。

## 4. 目录

```
README.md                      本文
winepart/                      ★ 最终形态：作为 Proton/Wine 部件安装的 dxgi.dll（不动游戏目录）
  wb_vram_dxgi.c               源码（纯 C、自带声明、零头文件依赖）
  dxgi.def                     导出定义（与 DXVK 的 dxgi 一致，5 个）
  build.sh                     clang + ld -m i386pep 本地构建
  install.sh                   安装/状态/修复/卸载（自动反查 Proton；身份用内容标记判定）
  rollback.sh                  ★ 回退：恢复官方 dxgi.dll + 清理所有前缀里的痕迹
  selftest.sh                  安装器自检（造一棵假 Steam 树，44 项断言）
  hosttest/                    宿主端测试台（假 DXVK + 假适配器，10 个场景，不需要 GPU）
  v1fix/                       对照组：v1 原始二进制 + 仅修 IID 的 16 字节
  dxgi.dll                     编译产物
dropin/                        上一形态：丢进游戏根目录的 dxgi.dll（仍可用，但会动游戏目录）
shim/                          另一种形态：Vulkan 层版（覆盖所有 Vulkan 程序）
third_party/vulkan, vk_video/  vendored Khronos 头文件
scripts/00-host-baseline.sh    主机基线采集（nvidia-smi / vulkaninfo / BAR1）
scripts/10-proton-container.sh Proton 容器启动器（独立前缀，不碰游戏存档）
probe/dxgi_vram_probe.c        Windows 侧 DXGI 探测器源码（需 mingw，暂未编）
patch/                         给 DXVK 打补丁的方案（未采用，备选参考）
results/                       所有采集输出（我直接读这里）
```

## 5. 注意

* `scripts/*` 与 `probe/*` 由我创建，属主是 root（我的沙箱以 root 运行）。
  如需改动，`sudo chown -R ronman:ronman` 即可。
* 我的执行环境看不到 `/dev/dri`、`/dev/nvidia*`，**无法直接跑 GPU 程序**，
  所有采集需要你在自己的桌面终端里执行。
