# GUI 监控工具

IDA TDI 相机实时预览界面，只读方式监控各通道采集状态。

## 文件说明

| 文件 | 说明 |
|------|------|
| `ida_monitor.py` | 主监控界面，实时显示 4 个通道的图像和状态 |
| `demo.py` | 演示模式，写入模拟数据后自动启动 `ida_monitor` |

## 依赖

```bash
pip install PyQt5
```

## 运行

**有硬件时**（`ida_app` 正在运行）：

```bash
python3 ida_monitor.py
```

**无硬件时**（演示/开发用）：

```bash
python3 demo.py
```

`demo.py` 会向 `/dev/shm/ida_preview_0~3` 写入模拟图像数据，然后自动启动监控界面。

## 界面说明

启动后显示 4 个并排面板，每个面板对应一个 TDI 通道：

```
┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│  Channel 0   │ │  Channel 1   │ │  Channel 2   │ │  Channel 3   │
│ SEQ 43 ✓CRC Normal│ │ ...     │ │ SEQ 44 ✗CRC Early│ │ ...    │
│ 2048×8192    │ │              │ │ 2048×8192    │ │              │
│  [图像]      │ │  [图像]      │ │  [图像]      │ │  [图像]      │
└──────────────┘ └──────────────┘ └──────────────┘ └──────────────┘
```

状态徽章含义：

| 徽章 | 含义 |
|------|------|
| `✓ CRC OK` | 数据校验通过 |
| `✗ CRC N` | 有 N 个 CRC 错误 |
| `Normal` | FPGA 正常结束扫描 |
| `Early-end` | FPGA 提前结束（state=-3） |
| `ERR N` | DMA 错误码 N |

## 数据来源

监控界面通过 `mmap` 只读方式访问 `/dev/shm/ida_preview_N`，数据由 `ida_app` 每完成一条 Swath 后写入。界面以 200ms（5Hz）轮询 `seq` 字段，`seq` 变化时才刷新图像。
