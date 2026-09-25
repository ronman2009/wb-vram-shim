# winepart/ — 把 dxgi.dll 装成 Proton/Wine 的部件

目标：拿到显存改写效果，但**游戏目录一个字节都不动**。

```bash
./winepart/build.sh             # 编译（本机 clang + ld，零警告）
./winepart/hosttest/run.sh      # 宿主端测试：假 DXVK + 假 NVML + 假 /proc/self/stat，13 个场景
./winepart/selftest.sh          # 安装器自检：假 Steam 树，44 项断言
./winepart/install-v2.sh        # ★ 一键装 v3（默认连 NVML 垫片一起装，推荐入口）
./winepart/install-v2.sh --status       # 看现在装的是哪一版（会报 v1fix / v2.x / v3.0）
./winepart/install-v2.sh --dry-run      # 只打印将要执行的命令
./winepart/install-v2.sh rollback       # 急救：恢复官方原件 + 清痕迹

./winepart/install.sh           # 通用安装器（配 --dll 可装任意一份）
./winepart/install.sh status    # 看状态（只读）

./winepart/rollback.sh          # ★ 出问题先跑这个：把 dxgi.dll 恢复成官方原件
./winepart/rollback.sh --dry-run   # 只读预览要动什么

./winepart/v1fix/install.sh     # 对照组：v1 + 仅修 IID（用于 A/B 归因）
```

---

## 0. v2 修了什么（一版真机日志换来的）

v1 装上去之后，实机日志给出：

```
本模块路径: C:\windows\system32\DXGI.DLL          ← 部件加载成功
[已加载 DXVK 原件] system32\wbvram_dxvk_dxgi.dll  ← 转发链通
白名单命中 appid= 3240220
CreateDXGIFactory1 失败 hr=80004002               ← E_NOINTERFACE，补丁从未打上
```

根因：v1 自己拿一个**写死的 IID** 去调 `CreateDXGIFactory1`，而那个值是
`{7b7166ec-21c7-44ae-b21a-e3218c4d1e42}` = **IDXGIFactory**（不是 IDXGIFactory1）。
DXGI 的约定是 `CreateDXGIFactory1` 只接受 IDXGIFactory1 及其派生接口，
于是 DXVK 返回 `E_NOINTERFACE`，适配器枚举失败，**虚表一个指针都没换**。
所以 v1 那个"有点效果"是错觉，数字其实一个都没改。

v2 的做法是**不猜 IID**：转发任何一个 `CreateDXGIFactory*` 时，先按
**调用者自己的 riid** 把工厂建出来，再拿这个真实存在的工厂 `EnumAdapters(0)`
取适配器、打补丁。零 IID 常量、零试探。

同类加固：

| 项 | v1 | v2 |
|---|---|---|
| 取工厂 | 自造 IID 试探，错了整条链死掉 | 用调用者的 riid，**不可能再错** |
| GetDesc 槽位 | 直接挂钩 4 个槽 | 挂钩前逐个用 1024B 放大缓冲**探测校验**，不一致就不挂 |
| 数值方向 | `!=` 就赋值（驱动更高时会被调低） | 只有 `out->Budget < g_budget` 才赋值，**只抬不降** |
| 日志 | `CREATE_ALWAYS`，后启动的进程冲掉前面的 | **追加** + pid + 宿主进程名，全部保留 |
| 观测 | 无 | `WBVRAM_DRYRUN=1` 只读不改，周期打印观测行 |
| 计算 | 只有静态余量 | 可选 `WBVRAM_DYNAMIC=1` 走 NVML 读真实占用 |

> 顺带说一句：日志里那个 `AppData\Local\Temp\wbvram.log` 才是实际落点
> （`GetTempPathW` 在 Wine 下就是它），不是文档早先写的 `users\steamuser\Temp`。

## 0.5 v2.1：一次真实事故（2026-09-25 00:0x 游戏起不来）

### 现场

装完 v2 之后游戏启动不了。日志里 v2 的解析结果是：

```
[已加载 DXVK 原件] system32\wbvram_dxvk_dxgi.dll C:\windows\system32\wbvram_dxvk_dxgi.dll
```

而那个文件是 **28466 字节** —— 是 **v1 的代理 DLL**，不是 DXVK 原件（4882432 字节）。

### 根因链

1. `install.sh` 判断"当前 dxgi.dll 是不是我们的"用的是**哈希对比**。用户装 v2 时，
   Proton 里躺着的是 v1，v1 的哈希 ≠ v2 构建的哈希 → 脚本把 **v1 误判成"官方原件"** 备份，
   **真正那份官方原件被覆盖，丢了**。
2. 于是链变成：`v2(system32\dxgi.dll) → v1(system32\wbvram_dxvk_dxgi.dll) → v2 → v1 → …`
   两条代理互相转发，栈溢出。
3. 第一个中招的是 Proton 自带辅助进程 `xalia.exe`（日志里宿主进程就是它），
   游戏进程随后再没起来。

### 三处修复

| # | 位置 | 修法 |
|---|---|---|
| 1 | `install.sh` 身份判定 | 改成**内容标记**：我们的所有版本都含 UTF-16 串 `wb-vram-shim`，官方 DXVK 没有。跨版本都能认出来，不会再把自己当官方备份 |
| 2 | `install.sh` 备份保护 | 备份一经登记（`wbvram-orig.ok`）不再覆盖；若发现**备份本身是我们的组件**，直接拒绝安装并指向 `rollback.sh` |
| 3 | `dxgi.dll` 候选自检 | 解析"DXVK 原件"时，凡内容含我们标记串的候选一律**拒绝**（`[候选拒绝]`），绝不再链到自己身上。最坏情况安静返回 `E_FAIL`，不崩 |

另外新增 `rollback.sh`：从**别的 compatdata 前缀**里找一份与官方 md5 逐字节一致的
`dxgi.dll` 恢复回去（本机实测 `compatdata/1493710`、`1551360` 里各有一份完美的）。
顺带扫一遍所有前缀，清掉我们留下的痕迹。

### 教训

- **身份判定不能只看哈希。** 版本一变，哈希就对不上，"自己"会被误认成"对方"。
  内容标记才跨版本稳定。
- **有价值的原始文件在覆盖前必须先验明正身。** 那次操作的不可逆损失是一份
  Steam 管理的官方 DLL；能救回来纯属侥幸（别的前缀里正好留了副本）。

## 1. 为什么只能装在这个位置

Proton 的 `dxgi` 用的是 **`n`（只用 native）**。`proton` 脚本 1391-1396 行：

```python
for f in dxvkfiles:
    try_copy(g_proton.arch_pe_dir("wine/dxvk", False) + f + ".dll",
             "drive_c/windows/system32", prefix=self.prefix_dir, ...)
    g_session.dlloverrides[f] = "n"
```

`dxgi` 在 1373-1374 行被放进 `dxvkfiles`。再加上 2065-2067 行：

```python
dllpaths = [g_proton.lib_dir + "vkd3d", g_proton.lib_dir + "wine"]
if "WINEDLLPATH" in os.environ:
    dllpaths.append(os.environ["WINEDLLPATH"])     # 用户给的一律排最后
self.env["WINEDLLPATH"] = ':'.join(dllpaths)
```

推论：

| 想法 | 为什么不行 |
|---|---|
| 丢进 `WINEDLLPATH` 当 builtin | `dxgi=n` 时 Wine 不查 builtin；且我们的目录排在 Proton 自己的后面 |
| 直接改前缀 `system32/dxgi.dll` | Proton 每次启动都 `try_copy`（**先 `os.remove(dst)` 再复制**，308-325 行），手改必被覆盖 |
| 改注册表 override 走 `dxgi=b` | Proton 会干脆不装 DXVK 的 dxgi，而 builtin 搜索仍先命中 Wine 自带那份 |

**剩下唯一正确的位置**，就是让 Proton *自己*把我们的文件搬进去：

```
<Proton>/files/lib/wine/dxvk/x86_64-windows/dxgi.dll              <- 我们的（Proton 亲手上架到前缀，native 载入）
<Proton>/files/lib/wine/dxvk/x86_64-windows/wbvram_dxvk_dxgi.dll   <- DXVK 原件备份
<pfx>/drive_c/windows/system32/wbvram_dxvk_dxgi.dll               <- 原件副本（自愈用）
```

第二条的文件名不参与 Proton 的复制名单，所以不会被 `try_copy` 碰。
`install.sh` 靠前缀里 `drive_c/windows/explorer.exe` 这条**软链**反查该 appid 实际用的
Proton（本机实测：指向 `Proton - Experimental`），不需要手工指定。

## 2. 安全阀

| 安全阀 | 作用 |
|---|---|
| **作用范围（v3.2）** | 默认对**所有游戏**生效——泛化已在 GTA V Enhanced（+1.2 GiB）与 Forza Horizon 5（峰值 6.65→7.51 GiB）实测。要限定，设 `WBVRAM_APPS`；拿不到 appid 的辅助进程（xalia 等）始终不碰。 |
| **虚表健全性校验** | 挂钩前对 4 个 GetDesc 槽位各做一次真实调用（用 1024 字节放大缓冲，避免写坏调用者的结构），要求 `DedicatedVideoMemory` 落在 64 MiB~1 TiB 且各槽位一致；`QueryVideoMemoryInfo` 要求 `Budget ≥ CurrentUsage` 且 ≤ 1 TiB。任一不满足就**放弃该槽位并记日志**。 |
| **只抬不降** | 只在驱动给的预算更低时才改写。 |
| **`WBVRAM_ENABLE=0`** | 整体关闭（仍正常转发）。 |
| **`WBVRAM_DRYRUN=1`** | 挂上钩子但只读不改，纯观测。 |

## 3. 真实 DXVK 的定位链（自愈）

`resolve_real_dll()` 按顺序试，命中即用：

1. `WBVRAM_REAL_DLL` 环境变量（绝对路径）
2. `C:\windows\system32\wbvram_dxvk_dxgi.dll`（安装脚本放的，最确定）
3. `STEAM_COMPAT_TOOL_PATHS` 里每个 Proton 目录 → `Z:\…\files\lib\wine\dxvk\x86_64-windows\wbvram_dxvk_dxgi.dll`
4. 交给 Wine 按名找 `wbvram_dxvk_dxgi.dll`
5. 兜底 `C:\windows\system32\dxgi.dll`（`dropin/` 形态下那一份才是 DXVK 的）

解析到"自己"时会被丢弃（比较模块句柄），不会递归。

## 4. 算法：NVML 逐进程枚举，剔除自己，从物理总量减

```
others = Σ(所有进程的显存占用) − 本进程
budget = 物理总量(8192) − others − WBVRAM_MARGIN_MB(默认 200)
```

数据源：**NVML 的 `nvmlDeviceGetGraphicsRunningProcesses_v2`** —— 它直接给出
每个进程（Linux PID + usedGpuMemory）。每 500ms 重算一次，桌面占用变化实时反映。

### "认自己"：/proc/<pid>/cmdline 匹配（v3.1 的核心修复）

**Wine 下 `Z:\proc\self` 指向的是 wineserver，不是调用进程。** 这不是猜测——
`probe/wineprobe.c` 在 GE-Proton 的 wine 下实测：

```
Linux 侧真实 PID          : 331729  Name: wineprobe.exe
探针内读 /proc/self/stat : 331677 (wineserver)     ← 两种写法都是它
```

`Z:\proc\self\stat` 和 `\\?\unix\proc\self\stat` **都指向 wineserver** ——
这就是 v3.0-旧"三个进程读到同一个 PID、剔除形同虚设"的根因。

**可靠的办法**（探针同时验证了可行性）：
1. `GetModuleFileNameW(0)` 拿自己的 exe 名（如 `gta5_enhanced.exe`）；
2. 对 NVML 列表里的每个 pid，读 `Z:\proc\<pid>\cmdline`（实测 Wine 把
   Windows 路径原样写进 cmdline，与 GetModuleFileNameW 一致）；
3. 同名（大小写不敏感）的就是自己。认到一次就缓存，不再读文件。

### 三次翻车的教训（都在这同一个函数附近）

| 版本 | 做法 | 为什么错 |
|---|---|---|
| v3.0-旧 | `/proc/self` 认自己 | Wine 下指向 wineserver，剔除形同虚设 → others 3311（含游戏自己 2427） |
| v3.0 | `others = 整卡已用 − CurrentUsage` | **CurrentUsage 不是物理占用**：DXVK 源码里它是 `max(驱动heapUsage, DXVK申请量) − DXVK申请量 + 实际交出量`，与 NVML 的整卡 used 不可比 → 游戏跑起来后 CurrentUsage(4805) > 整卡(2919)，others 算成 0，budget 虚高到 7992（界面"其它软件"显示 200） |
| v3.0 | NVML v1 结构 `{total,used,free}` | 官方顺序是 `{total,free,used}`，把"空闲"当"已用"读 |

### 兜底链

| 情况 | 行为 |
|---|---|
| 正常 | 枚举 → cmdline 认出自己 → 剔除 → 求和 |
| 认不出自己 | others 偏大（含自己）→ **额度偏保守**（安全方向），日志明确警告 |
| 枚举接口失败 / NVML 不可用 | 退回静态 `total − WBVRAM_RESERVE_MB(默认 1536)` = 6656 |

**三种结果都有界**：最好的情况是精确的 7352 上下，最差也是 6656 ——
不会再出现"其它软件 200 / budget 7992"这种虚高。

### 三条要求满足情况

| 要求 | 实现 |
|---|---|
| ① 显存总量读取 | ✅ 注册表 `HardwareInformation.qwMemorySize`（本机 8192） |
| ② 显存占用读取 | ✅ **真读**：NVML 逐进程枚举每进程的物理占用 |
| ③ 报给游戏的预算 | ✅ 每 500ms 现算，无硬编码 |

### NVML 垫片从哪来 —— 以及为什么必须用 GE-Proton

Wine 侧要读 NVML，得有 `nvml.dll`(PE) + `nvml.so`(unix) 这一对垫片，
而且**必须是由同一个 Wine 构建产出的**（unixlib 接口表要自洽）。

**Proton Experimental 用不了它。** 实测（2026-09-25 11:4x）：

```
NVML 垫片加载失败（nvml.dll），GetLastError=…
  → 动态模式退回静态 reserve
```

把 GE-Proton 的那对垫片挪过去，位置和依赖都没问题
（`nvml.dll` 是合规的 `PE32+ for WINE`，只依赖 `kernel32/ntdll/ucrtbase`，
摆在 `files/lib/wine/x86_64-windows/` 也落在 `WINEDLLPATH` 里），**但就是加载不起来**。
原因是两件事叠在一起：

1. **Experimental 的 proton 脚本里完全没有 nvml 逻辑** —— 它没有 `nvidia-libs` 目录，
   也不会 prepend `WINEDLLPATH`（GE-Proton 的脚本 2251-2253、1505-1506 行才有）。
2. **跨 Proton 移植的 unixlib 对，ABI 不匹配** —— PE 侧能加载，但 `nvml.so` 起不来。

**所以正确做法是让游戏跑 GE-Proton 系列**：

- GE-Proton 在 N 卡上会 `compat_config.add("nvml")`（脚本 2251-2253），
  并把 `wine/nvidia-libs/nvml/wine` **prepend** 进 `WINEDLLPATH`（1505-1506）；
- 那对垫片是同源构建，ABI 自洽。

`install.sh` 会识别这种情况：**Proton 自带 `nvidia-libs/nvml` 时不再移植**，
`status` 里会显示「由 Proton 自带」。不想要就 `--no-nvml`。

## 5. 环境变量

| 变量 | 默认 | 作用 |
|---|---|---|
| `WBVRAM_ENABLE` | `1` | `0` = 只转发不改写 |
| `WBVRAM_DRYRUN` | 关 | `1` = 只观测不改写 |
| `WBVRAM_APPS` | 空 | 逗号分隔 appid = 只对这些游戏生效（默认空 = **全部游戏**） |

| **`WBVRAM_DYNAMIC`** | **开** | `0` = 关掉 NVML 动态计算、退回静态余量 |
| `WBVRAM_MARGIN_MB` | `200` | 安全垫 |
| `WBVRAM_RESERVE_MB` | `1536` | 静态模式余量（仅 `WBVRAM_DYNAMIC=0` 时用） |
| `WBVRAM_TOTAL_MB` | 读注册表 | 强制指定物理总量 |
| `WBVRAM_REAL_DLL` | — | 直接指定 DXVK 原件 |
| `WBVRAM_LOG_FILE` | `%TEMP%\wbvram.log` | 日志路径 |
| `WBVRAM_NOLOG` | 关 | `1` = 不写日志 |

**默认什么都不用设。** 改这些不需要重装，加到启动项 `%command%` 前面即可。

日志（实测路径）：

```
<pfx>/drive_c/users/steamuser/AppData/Local/Temp/wbvram.log
```

生效时应该是这一串：

```
===== wb-vram-shim v3 (Wine 部件版 dxgi.dll) pid=…
本模块: C:\windows\system32\DXGI.DLL
宿主进程: …\GTA5_Enhanced.exe
[已加载 DXVK 原件] system32\wbvram_dxvk_dxgi.dll <- …
白名单命中 appid 3240220
NVML 就绪，设备数=1
本进程 Linux PID=…
[结果] total=8192 MiB ; 算法=NVML 枚举各进程占用 → total − 其它 − margin => budget=…
[NVML] 枚举进程=N 个（其中无用量=0）除本进程外合计=X MiB ; margin=200 => budget=Y MiB
探测通过：DedicatedVideoMemory=8160 MiB, Budget=…, Usage=… MiB
已挂钩适配器虚表: GetDesc=… QueryVideoMemoryInfo=… GetDesc3=挂钩
GetDesc: DedicatedVideoMemory 8160 -> 8192 MiB
QueryVideoMemoryInfo(LOCAL): Budget … -> Y MiB   (usage=… MiB)
```

**游戏内可肉眼验证**：设置→图形里「视频内存 x/8160」应变成 `/8192`，
「其它软件」那一行 = `8192 − Y`。

**想先只看数据不改**：`WBVRAM_DRYRUN=1 %command%`，日志里会周期性打印 `[观测]` 行。

## 6. 已知失效条件

1. **Steam 验证 Proton 文件完整性 / Proton 自动更新** → 官方 `dxgi.dll` 被换回来，
   备份也可能被清掉。`install.sh status` 一查即知，重跑 `install` 即可。
2. **换用别的 Proton 启动同一个游戏** → 对新 Proton 再 install 一次
   （`install.sh --proton "…"`）。GE-Proton 这类第三方工具 Steam 不接管，反而更稳。
3. **32 位游戏不受影响**：只改了 `x86_64-windows`，`i386-windows` 那份没动。
4. **NVML 垫片被清掉** → 会自动退回静态 reserve（日志里会写 `NVML 不可用 → 退回静态`），
   重跑 `install.sh` 补回来即可。
4. **反作弊**：改动落在 Proton 目录，游戏目录干净。但 BattlEye 是**用户态**组件，
   它在游戏进程里枚举已加载模块是本职工作 —— 它会**看到**这个外来 `dxgi.dll`。
   它没有 ring-0 能力、比对不了磁盘文件，但后端能动态下发检测规则。
   **建议只在故事模式用，进线上前 `uninstall`。**

## 7. 验证状态

**宿主端（Linux，不需要 GPU）** —— `./winepart/hosttest/run.sh`，**14 个场景全通过**：

| 场景 | 断言要点 |
|---|---|
| `patch` | 工厂建成、两个槽位被挂钩、GetDesc 8160→8192、静态预算 6656 |
| `nowhitelist` | `WBVRAM_APPS` 限定后名单外游戏：槽位不动、数值不动、但转发照常 |
| `noappid` | 拿不到 SteamGameId 的辅助进程（xalia 等）：保守跳过，不改写 |
| `dryrun` | 挂钩但只读、数值不变、观测行打印 |
| `badriid` | **故意用错的 IID**（复刻 v1 的 bug）：错误码原样透传、不瞎挂；换正确 IID 后立刻生效 |
| `reserve` | `WBVRAM_RESERVE_MB=1000` → 7192 |
| **`dynamic`** | **主路径**：枚举 5 进程、靠 cmdline 认出自己(4321)剔除 → `8192−640−200=7352`；其它进程涨到 890 → 额度**实时**降到 7102（动态调节成立） |
| **`default`** | **一个开关都不设**也自动算出 7352（验证"默认就开"） |
| **`noself`** | cmdline 认不出自己 → others=1140、额度 6852（**偏保守**，日志警告） |
| **`noprocs`** | 枚举接口失败 → 退回静态 6656（不再出现虚高 7992） |
| `nvml-missing` | 没有 nvml.dll 时自动退回静态 6656 |
| `toolpath` | system32 副本缺失时顺着 `STEAM_COMPAT_TOOL_PATHS` 自愈成功 |
| `nested` | **复刻 00:0x 事故**：候选 2 是我们自己的旧版 → 被拒绝，退到候选 3 成功 |
| `all-ours` | 所有候选都是我们自己 → 安静返回 `E_FAIL`、记明原因、**不崩** |

测试台的假 dxgi **刻意复刻了 DXVK 的 IID 校验**，假 NVML 也带**进程枚举接口和假
`/proc/self/stat`** —— 上面那三类回归都能抓住。

**PE 侧**：干净重建零警告；导出表 5 个与 DXVK 完全一致；`winedump` 确认合法 PE32+。

**安装器** —— `./winepart/selftest.sh`，**43 项全通过**（含幂等、模拟 Steam 更新后备份刷新、
repair、逐字节还原、**NVML 垫片默认安装**与卸载清理、**污染状态下拒绝安装**、
**rollback.sh 从别的前缀恢复官方原件**、**v1fix 走 `--dll` 装/卸**、
**版本识别能把 v1fix / v2.x / v3.0 分开**、**`install-v2.sh --dry-run` 不改文件**）。

**真机已通过（v1fix 对照组，2026-09-25 00:35）**：

```
探测通过：DedicatedVideoMemory=8160 MiB, Budget=7109 MiB, Usage=0 MiB
已挂钩适配器虚表: GetDesc=… QueryVideoMemoryInfo=…
计算结果: total=8192 MiB  reserve=1536 MiB  => budget=6656 MiB
```

游戏内：分母 `8160 → 8192` ✔，"其它软件" `2299 → 1536` ✔，游戏实测占用 `5146 → 6364 MiB` ✔。
游戏可正常启动运行，无崩溃。日志转码本：`results/wbvram-v1fix-ok.log.txt`。

**仍未做（关键）**：

1. **v3.0 没在真机上装过。** 主路径要 NVML 垫片（`install.sh` 默认会装），
   而垫片是从 GE-Proton11-7 **移植**到 Proton Experimental，**ABI 兼容性未验证**。
2. **垫片移植后 nvmlDeviceGetMemoryInfo 的 32/64 位结构、unixlib 接口表**都还是未知数 ——
   跑起来才知。跑之前建议先 `install.sh status` 看一眼垫片在不在。
3. **驱动运行中的原始预算**仍只有启动瞬间那一个采样点（这条对 v3.0 已不重要，因为不再参考它）。

**首次上机要确认的三件事**：

1. 游戏能正常启动、不崩；
2. 日志里有 `NVML 就绪` 和 `本进程 Linux PID=…`（后者说明 `/proc/self/stat` 读通了）；
3. `[NVML] … 除本进程外合计=X`，把 X 跟 nvtop 里非游戏进程的合计对一下。
