# v1fix/ —— 对照组：v1 只修 IID 那个 bug

用来回答一个问题：**线上那次"游戏起不来"是不是 v2 引入的？**

## 它是什么

```
dxgi-v1-vanilla.dll   v1 的原始二进制（28466 字节，md5 13e9669078e8e0944cfca9469df67c5c）
                      —— 从被污染的备份里抢救出来的，未经改动，留作对照基准
dxgi.dll              打上 IID 补丁的版本（改变 16 字节）
make-iidfix.py        生成脚本（幂等、带校验）
build.sh              跑上面的脚本
install.sh            包装 ../install.sh --dll ...
```

## 为什么用二进制补丁而不是源码

v1 的源码在工作区里已经被 v2 覆盖了。与其**重写一份"差不多的 v1"**，
不如直接对 v1 的原始二进制做最小补丁 —— 这样"改动范围"是可证明的：

```
改动字节数 : 16
改动范围   : offset 0x3e90 - 0x3e9f（连续）
               {7b7166ec-21c7-44ae-b21a-e3218c4d1e42}  IDXGIFactory
            →  {770aae78-f26f-4dba-a829-253c83d1b387}  IDXGIFactory1
PE CheckSum : 0（本来就不校验，不用改）
导出表     : 5 个，与 DXVK 的 dxgi 完全一致
```

除了这 16 字节，它和 v1 **逐字节相同**：同样的日志格式（`CREATE_ALWAYS`）、
同样的 `!=` 就赋值、同样没有虚表探测、没有 DRYRUN、没有 NVML、没有候选自检。

## 用法

```bash
./winepart/v1fix/build.sh            # 生成 dxgi.dll（已生成，可跳过）
./winepart/v1fix/install.sh          # 装
./winepart/v1fix/install.sh status   # 看状态
./winepart/v1fix/install.sh uninstall
```

安装走的是 `../install.sh --dll <这份>`，所以备份/身份判定/回退逻辑完全一样。

## 前提

**必须先有一份干净的官方 dxgi.dll** 能给代理转发。所以顺序是：

```bash
./winepart/rollback.sh       # 1) 先把 Proton 恢复干净
./winepart/v1fix/install.sh  # 2) 再装对照组
```

如果直接装，`install.sh` 会拒绝并提示先跑 rollback（这是故意的，见下）。

## ⚠️ 它没有 v2.1 的候选自检

v2.1 在解析"DXVK 原件"时会拒绝任何**含我们标记串**的候选文件，避免我们的两份
代理互相转发直到爆栈。v1fix 是刻意保持 v1 原样的对照组，**没有这道自检** ——
所以它必须装在干净的基础上；如果备份被污染了，它会一路转发下去。

这也是对照组的意义：只差那 16 字节，行为和 v1 一致，便于定位问题归因。
