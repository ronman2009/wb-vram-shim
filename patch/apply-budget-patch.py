#!/usr/bin/env python3
"""给 DXVK 增加「LOCAL 段显存预算下限覆盖」。

问题
----
Proton 下 app 拿到的显存上限经过两段衰减：

    8192 MiB  物理显存
      -> 8160 MiB   Vulkan DEVICE_LOCAL 堆（驱动保留 32 MiB）
      -> 5861 MB    VK_EXT_memory_budget.heapBudget（驱动又留了约 800 MiB 余量）

DXVK 把 heapBudget 原样转成 DXGI 的 `QueryVideoMemoryInfo().Budget`
（src/dxgi/dxgi_adapter.cpp：

    pVideoMemoryInfo->Budget += memInfo.heaps[i].memoryBudget;   // 无任何选项参与
），

而现代 DX12 游戏用这个 Budget 反推「其它软件占了多少」，然后自我限流。
DXVK 现有的选项里没有抬高它的办法（`dxgi.maxDeviceMemory` 只裁剪
`GetAdapterDesc()` 的静态值，不碰 Budget），所以只能改源码。

改法
----
在 QueryVideoMemoryInfo() 里给 LOCAL 段加一个「预算下限」：
环境变量 DXGI_LOCAL_BUDGET_MB 有值时，Budget 至少是该值（单位 MiB）。
只抬高、不下调；不设置则与官方行为完全一致。

用稳定锚点做文本插入，不用 .patch —— 不同 DXVK 版本的上下文行会漂移。
可重复执行（幂等），支持 --revert。
"""

import argparse
import pathlib
import sys

TARGET = "src/dxgi/dxgi_adapter.cpp"
ANCHOR = "    // We don't implement reservation, but the observable"
MARKER = "DXGI_LOCAL_BUDGET_MB"

BLOCK = """    // --- LOCAL budget floor override -------------------------------------
    // Rationale: on Linux the NVIDIA driver announces a
    // VK_EXT_memory_budget heapBudget that sits noticeably below
    // "device-local heap size minus actual usage by other clients".
    // DX12 titles derive their usable-VRAM figure from it (GTA V Enhanced
    // exposes it as the "other software" row), so they limit themselves to
    // less than the GPU could actually hand out.
    // This only ever *raises* the reported LOCAL budget, never lowers it.
    // Unit: MiB. Unset => stock behaviour.
    static const VkDeviceSize s_localBudgetFloor = [] () -> VkDeviceSize {
      const char* env = std::getenv("DXGI_LOCAL_BUDGET_MB");
      return env ? (VkDeviceSize(std::strtoull(env, nullptr, 10)) << 20) : 0u;
    } ();

    if (MemorySegmentGroup == DXGI_MEMORY_SEGMENT_GROUP_LOCAL
     && s_localBudgetFloor > pVideoMemoryInfo->Budget)
      pVideoMemoryInfo->Budget = s_localBudgetFloor;

"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dxvk_root", help="DXVK 源码根目录")
    ap.add_argument("--revert", action="store_true", help="撤销改动")
    args = ap.parse_args()

    path = pathlib.Path(args.dxvk_root) / TARGET
    if not path.is_file():
        print(f"找不到 {path}", file=sys.stderr)
        return 1

    text = path.read_text(encoding="utf-8")
    patched = MARKER in text

    if args.revert:
        if not patched:
            print("未打过补丁，无需撤销")
            return 0
        start = text.index(BLOCK) if BLOCK in text else -1
        if start < 0:
            print("补丁块与当前文件不匹配，请手工撤销", file=sys.stderr)
            return 1
        path.write_text(text[:start] + text[start + len(BLOCK):], encoding="utf-8")
        print(f"已撤销: {path}")
        return 0

    if patched:
        print("补丁已在树中，跳过")
        return 0

    if ANCHOR not in text:
        print("锚点行不存在 —— DXVK 版本差异过大，请手工插入", file=sys.stderr)
        return 1

    text = text.replace(ANCHOR, BLOCK + ANCHOR, 1)
    path.write_text(text, encoding="utf-8")
    print(f"已打补丁: {path}")
    print("（检查一下 QueryVideoMemoryInfo() 里的插入位置）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
