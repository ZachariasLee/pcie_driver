#!/usr/bin/env python3
"""
demo.py - 向 /dev/shm 写入模拟数据，用于预览 ida_monitor 界面效果。
运行后直接启动 ida_monitor。
"""

import os
import struct
import math
import subprocess
import sys

# 与 ida_preview.h 保持一致
PREVIEW_MAGIC    = 0x49444150
PREVIEW_MAX_W    = 2048
PREVIEW_MAX_H    = 8192
_HDR_FMT         = '<IIIIQQiii20s'
_HDR_SIZE        = struct.calcsize(_HDR_FMT)   # 64
PREVIEW_SHM_SIZE = _HDR_SIZE + PREVIEW_MAX_W * PREVIEW_MAX_H
SHM_DIR          = '/dev/shm'

# 示例通道参数
CHANNELS = [
    dict(width=2048, height=8192, orig_lines=8192,  orig_bytes=8192*3072,  crc=0, state=-2, error=0),
    dict(width=2048, height=8192, orig_lines=8192,  orig_bytes=8192*3072,  crc=0, state=-2, error=0),
    dict(width=2048, height=8192, orig_lines=8192,  orig_bytes=8192*3072,  crc=3, state=-3, error=0),
    dict(width=2048, height=8192, orig_lines=8192,  orig_bytes=8192*3072,  crc=0, state=-2, error=0),
]


def make_image(width: int, height: int, ch_id: int) -> bytes:
    """生成示例灰度图：竖向渐变 + 横向条纹 + 噪点，模拟 TDI 推扫图像。"""
    buf = bytearray(width * height)
    for y in range(height):
        # 竖向慢变化背景（模拟地物亮度随推扫变化）
        base = int(60 + 140 * (0.5 + 0.5 * math.sin(2 * math.pi * y / height)))
        for x in range(width):
            # 横向周期性条纹（模拟相机响应不均匀性）
            stripe = int(20 * math.sin(2 * math.pi * x / 256))
            # 细节纹理（模拟地表特征）
            detail = int(15 * math.sin(2 * math.pi * x / 32) *
                             math.cos(2 * math.pi * y / 128))
            # 通道间亮度差异
            ch_offset = ch_id * 15
            v = base + stripe + detail + ch_offset
            buf[y * width + x] = max(0, min(255, v))
    return bytes(buf)


def write_channel(ch_id: int, params: dict):
    w   = params['width']
    h   = params['height']
    seq = 42 + ch_id   # 非零，GUI 才会渲染

    pixels = make_image(w, h, ch_id)

    hdr = struct.pack(
        _HDR_FMT,
        PREVIEW_MAGIC,
        seq,
        w,
        h,
        params['orig_lines'],
        params['orig_bytes'],
        params['crc'],
        params['state'],
        params['error'],
        b'\x00' * 20,
    )

    path = os.path.join(SHM_DIR, f'ida_preview_{ch_id}')
    with open(path, 'wb') as f:
        f.write(hdr)
        f.write(pixels)
        # 补齐到固定大小（GUI 按 PREVIEW_SHM_SIZE 打开 mmap）
        remaining = PREVIEW_SHM_SIZE - _HDR_SIZE - len(pixels)
        if remaining > 0:
            f.write(b'\x00' * remaining)

    print(f'  CH{ch_id}: {w}×{h} → {path}')


def main():
    print('写入示例预览数据…')
    for i, params in enumerate(CHANNELS):
        write_channel(i, params)
    print('启动 ida_monitor…\n')
    script = os.path.join(os.path.dirname(__file__), 'ida_monitor.py')
    os.execv(sys.executable, [sys.executable, script])


if __name__ == '__main__':
    main()
