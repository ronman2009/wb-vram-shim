#!/usr/bin/env python3
"""
make-iidfix.py —— 从 v1 的原始二进制生成「只修了 IID 这个 bug」的对照组

背景：v1 拿 {7b7166ec-…}（= IDXGIFactory）去调 CreateDXGIFactory1，
而该函数只接受 IDXGIFactory1（{770aae78-…}），于是 DXVK 返回
E_NOINTERFACE(0x80004002)，适配器枚举失败、虚表从未被打补丁。

v1 的源码在工作区里已被 v2 覆盖，所以对照组直接用 v1 的**原始二进制**，
只把那 16 字节常量改对。改动范围可证明最小 —— 便于判定"是不是 v2 的问题"。

用法:
    ./build.sh           # 或 python3 make-iidfix.py
"""

import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "dxgi-v1-vanilla.dll")
OUT = os.path.join(HERE, "dxgi.dll")

# v1 原始二进制（从被污染的备份里抢救出来的那一份）
VANILLA_MD5 = "13e9669078e8e0944cfca9469df67c5c"

# {7b7166ec-21c7-44ae-b21a-e3218c4d1e42} = IDXGIFactory      （v1 用的，错的）
WRONG = bytes([0xEC, 0x66, 0x71, 0x7B, 0xC7, 0x21, 0xAE, 0x44,
               0xB2, 0x1A, 0xE3, 0x21, 0x8C, 0x4D, 0x1E, 0x42])
# {770aae78-f26f-4dba-a829-253c83d1b387} = IDXGIFactory1    （正确的）
RIGHT = bytes([0x78, 0xAE, 0x0A, 0x77, 0x6F, 0xF2, 0xBA, 0x4D,
               0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87])


def main():
    if not os.path.isfile(SRC):
        print("缺少 %s" % SRC)
        return 1

    data = open(SRC, "rb").read()
    got = hashlib.md5(data).hexdigest()
    if got != VANILLA_MD5:
        print("输入不是预期的 v1 原始二进制：md5 %s != %s" % (got, VANILLA_MD5))
        return 1

    n_wrong = data.count(WRONG)
    n_right = data.count(RIGHT)
    if n_wrong != 1:
        print("错的 IID 出现 %d 次，预期恰好 1 次 —— 输入不对" % n_wrong)
        return 1
    if n_right != 0:
        print("输入里已经含正确的 IID（%d 处）—— 可能已经打过补丁了" % n_right)
        return 1

    off = data.index(WRONG)
    out = data[:off] + RIGHT + data[off + 16:]

    diff = [i for i in range(len(data)) if data[i] != out[i]]
    if len(diff) != 16 or diff[0] != off or diff[-1] != off + 15:
        print("补丁范围异常：%d 处差异" % len(diff))
        return 1
    if out.count(WRONG) != 0 or out.count(RIGHT) != 1:
        print("补丁后常量校验失败")
        return 1
    if len(out) != len(data):
        print("文件大小变了")
        return 1

    open(OUT, "wb").write(out)
    print("已生成 %s" % OUT)
    print("  源    : dxgi-v1-vanilla.dll   md5=%s  %d 字节" % (got, len(data)))
    print("  产物  : dxgi.dll               md5=%s  %d 字节"
          % (hashlib.md5(out).hexdigest(), len(out)))
    print("  改动  : 仅 offset %s 起 16 字节 —— IDXGIFactory -> IDXGIFactory1"
          % hex(off))
    return 0


if __name__ == "__main__":
    sys.exit(main())
