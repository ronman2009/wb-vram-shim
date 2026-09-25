# dropin —— 丢进游戏根目录的 `dxgi.dll`

DXVK 那种形态：**一个 PE 文件复制到游戏目录，完事**。不用装层、不用改启动项。
默认参数就能跑；要调再用几个环境变量。

```
Grand Theft Auto V Enhanced/
├── GTA5_Enhanced.exe
├── dxgi.dll            <- 这个（约 23 KB）
└── ...
```

> **⚠️ 反作弊提醒**：GTA Online 带 BattlEye。往游戏目录塞自定义 DLL 属于改动游戏进程，
> 有被判定为篡改的风险。建议**只在故事模式用**，进线上前删掉。
> （风险由你判断，我只负责把它说清楚。）
>
> **为什么它看得见**：BattlEye 在 Proton 下确实是能工作的，但形态是"用户态"实现——
> Steam 工具 `Proton BattlEye Runtime`（appid 1161040）里是一对
> `beclient_x64.dll`（PE，"for WINE"）+ `beclient_x64.so`（ELF，导出 `__wine_unix_call_funcs`），
> 也就是 Wine 的 builtin DLL 双体架构，和 Wine 自带的 `vulkan-1.dll` + `winevulkan.so` 同构。
> **Windows 上那个 ring-0 的 `BEDaisy.sys` 在 Proton 下不存在**（Valve 官方：
> "Kernel-space solutions are not currently supported and are not recommended"）。
> 所以：它**能枚举游戏进程已加载的模块**、很可能比对游戏目录文件的完整性——
> 这才是本方案的风险点；反过来，内核级的完整性校验/反调试它做不了。

---

## 1. 它做了什么

1. Windows 的 DLL 搜索顺序里，**主程序所在目录优先于 `system32`**，
   所以这个 `dxgi.dll` 会顶替掉 Proton 装在前缀里的 DXVK 版 `dxgi.dll`。
2. 本 DLL 把那 5 个 DXGI 导出原样转发给真正的 DXVK——先把
   `C:\windows\system32\dxgi.dll` **复制到 `%TEMP%` 换个名字**再加载，
   避免与自己同名冲突。
3. 第一次转发时，在**自己的进程里**找到 DXVK 那个 `IDXGIAdapter` 类的虚表，
   把 5 个槽位换成我们的版本：

   | 虚表槽 | 方法 | 改写内容 |
   |---|---|---|
   | 8 | `IDXGIAdapter::GetDesc` | `DedicatedVideoMemory` → 物理总量（8192 MiB） |
   | 10 | `IDXGIAdapter1::GetDesc1` | 同上 |
   | 11 | `IDXGIAdapter2::GetDesc2` | 同上 |
   | 14 | `IDXGIAdapter3::QueryVideoMemoryInfo` | `Budget` → 我们算的可用值 |
   | 16 | `IDXGIAdapter4::GetDesc3` | 同 GetDesc |

   每个钩子都是「先调用原实现，再改返回值」，所以不会偏离 DXVK 的行为。

   **为什么只挂一处就够**：改的是 DXVK 适配器**类的虚表**，整个进程只有一份。
   无论 `dxgi.dll` 是游戏加载的还是 `vkd3d-proton` 的 `d3d12.dll` 加载的，
   也无论从 `IDXGIAdapter` 还是 `IDXGIAdapter3/4` 哪个视角拿到的适配器，全都一起变。

## 2. 数值怎么来的

相当于把 Windows 的账搬过来：

```
total  = HKLM\SYSTEM\CurrentControlSet\Control\Video\*\0000\HardwareInformation.qwMemorySize
         （本机实测 8192 MiB；取的是物理完整值，不是驱动报的 8160）
         取不到时退回  GetDesc 原值 + 32 MiB
budget = total - reserve          reserve 默认 1536 MiB
```

`reserve` 默认 1536 MiB，是按本机实测的「桌面 + Steam + Wine 附属进程」开销定的
（nvtop 实测约 1369 MiB，留了点余量）。驱动原本给的是 5861 MiB，
本方案算出来是 **6656 MiB**，也就是把驱动多留的那约 800 MiB 顶回去。

> **诚实说明**：`reserve` 是**静态配置**，不是动态读 NVML。
> 原因是约束本身——"单文件 + 纯 C 的 PE"拿不到 Linux 侧的 NVML；
> 要动态必须配一个 Unix 侧组件，那就不是"丢一个文件"了。
> 折中：默认值按实测给，可用 `WBVRAM_RESERVE_MB` 覆盖。

## 3. 环境变量（全部可选）

| 变量 | 默认 | 说明 |
|---|---|---|
| `WBVRAM_ENABLE` | 1 | 设 0 = 完全放行（等价于删掉文件但保留它） |
| `WBVRAM_RESERVE_MB` | 1536 | 预留余量 MiB |
| `WBVRAM_TOTAL_MB` | 空 | 强制指定物理总量，覆盖注册表读数 |
| `WBVRAM_NOLOG` | 0 | 设 1 = 不写日志文件 |

游戏启动项里加（放进 `%command%` 之前），例如只改余量：

```
WBVRAM_RESERVE_MB=1600 %command%
```

## 4. 日志

每次运行都会在 **DLL 所在目录**（也就是游戏根目录）写一份 `wbvram.log`：

```
===== wb-vram-dropin (dxgi.dll) pid=1234 =====
已挂钩适配器虚表: GetDesc=7ff... QueryVideoMemoryInfo=7ff...
计算结果: total=8192 MiB  reserve=1536 MiB  => budget=6656 MiB
GetDesc: DedicatedVideoMemory 8160 -> 8192 MiB
QueryVideoMemoryInfo(LOCAL): Budget 5861 -> 6656 MiB   (usage=120 MiB)
```

看到这两行 `->` 就说明生效了。用它对照 `nvidia-smi` 和游戏设置页的「视频内存 / 其它软件」。

## 5. 构建

不需要 root、不需要联网、不需要 mingw：

```bash
./dropin/build.sh          # -> dropin/dxgi.dll
./dropin/build.sh clean
```

工具链全部是本机现成的：

| 环节 | 用什么 |
|---|---|
| 编译 | `clang --target=x86_64-pc-windows-gnu`（产出 COFF 目标文件） |
| 链接 | `ld -m i386pep`（本机 binutils 支持 `pei-x86-64`） |
| 导入库 | `/usr/lib/wine/x86_64-windows/libkernel32.a`、`libadvapi32.a`（Wine 自带） |
| 导出定义 | `dropin/dxgi.def` |

源码 **刻意不 include 任何头文件**（Windows 头会把 mingw 的 libc 一起拖进来），
所需类型与导入全部自己声明，所以整套流程零外部依赖。

## 6. 部署 / 卸载

```bash
./dropin/deploy.sh              # 复制到游戏目录
./dropin/deploy.sh uninstall    # 删掉
```

或手动：

```bash
cp dropin/dxgi.dll ~/.local/share/Steam/steamapps/common/'Grand Theft Auto V Enhanced'/
```

## 7. 验证状态

**已在本机离线验证：**

* 编译**零警告**；
* 产物是合法 PE32+ x86-64 DLL（`file` + `objdump -p` + **`winedump`** 三方确认）；
* **导出表与 DXVK 的 `dxgi.dll` 完全一致**：`CreateDXGIFactory`、`CreateDXGIFactory1`、
  `CreateDXGIFactory2`、`DXGIDeclareAdapterRemovalSupport`、`DXGIGetDebugInterface1`；
* 导入表只有 `advapi32.dll`(4) 与 `kernel32.dll`(11)，全部按名字解析。

**尚未验证（需要你在有 GPU 的桌面会话里跑）：**

* 真实运行时的改写效果（沙箱里既没有 `/dev/dri`，`wine` 建前缀也会挂住）；
* Proton 下 DLL 搜索顺序是否如预期优先加载本文件（理论如此，需实测）；
* `vkd3d-proton` 是否会另加载一份 `dxgi.dll`（**即使如此也不影响结果**——
  虚表是进程内共享的，钩子对所有拿到适配器的调用方都生效）。

## 8. 已知限制

* 只编译了 x86_64。32 位游戏需要另出一份 `i686` 的。
* 依赖 `system32\dxgi.dll` 是真实文件（Proton 装 DXVK 后就是），
  否则初始化会中止并保持驱动原值——日志里会写明。
* 不处理共享段（`NON_LOCAL` / `SharedSystemMemory`），只动专用显存这一侧。
* **谎报预算不产生显存**：天花板仍是 `物理总量 − 真实其它占用`。
  默认 1536 MiB 的余量就是照着这个留的，别压到 0。
