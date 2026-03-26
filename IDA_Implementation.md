# IDA PCIe DMA Driver 实现文档

**版本**：v1.1
**适用硬件**：xczu7eg（开发板）/ xcku5p（商用板）
**操作系统**：Rocky Linux 9（kernel 5.14）

---

## 1. 背景与目标

IDA（Imaging Data Acquisition）系统通过PCIe总线将FPGA采集的雷达数据传输到Host服务器内存，供应用层实时处理。

核心设计目标：
- **高吞吐**：接近PCIe物理带宽极限，不受CPU搬运能力限制
- **零拷贝**：数据由FPGA直接写入Host内存，App直接读取，无任何数据复制
- **低延迟**：FPGA主导传输节奏，Host仅需准备地址表并等待中断

---

## 2. 整体方案：Method A（FPGA主动DMA）

### 2.1 设计思想

Host驱动不发起DMA传输，而是**提前准备好地址表**，把Host内存的地址告诉FPGA。FPGA在采集数据后，自主按地址表将数据写入Host内存，完成后通过MSI-X中断通知Host。

这与传统DMA的区别：

| 对比项 | 传统DMA（Host发起） | 本方案（FPGA发起） |
|---|---|---|
| 谁控制传输 | CPU发描述符，QDMA IP执行 | FPGA读地址表，自主DMA |
| FPGA工作 | 响应PCIe Read请求 | 主动发起PCIe Write |
| Host驱动复杂度 | 高（需管理descriptor ring） | 低（只需准备地址表） |
| 适用场景 | 从FPGA DDR取特定地址数据 | FPGA连续推送数据流 |

### 2.2 IOMMU的作用

Host内存（hugepage）的物理地址通过IOMMU映射为**IOVA（I/O Virtual Address）**。FPGA DMA操作使用的是IOVA而非物理地址，IOMMU负责翻译并提供内存访问隔离和保护。

---

## 3. 内存布局

### 3.1 Host侧内存分配

每个IDA通道分配两块内存，采用不同的分配策略：

| 内存区域 | 大小 | 分配方式 | 页面类型 | 用途 |
|---|---|---|---|---|
| Swath Buffer | ~10GB（上限） | `mmap(MAP_HUGETLB\|MAP_ANONYMOUS)` | HugeTLB 2MB | FPGA将采集数据直接DMA写入此处 |
| Status Area | 4096字节 | `shm_open + mmap` | 普通 4KB | FPGA写入传输状态（行数、CRC、状态码） |

**为什么Swath Buffer用HugeTLB，Status Area用普通页：**

- Swath Buffer是10GB的DMA目标，必须减少sg段数量（见3.4节）。HugeTLB保证物理上使用2MB大页，`pin_user_pages`成功率高，且页面在程序崩溃后自动归还给内核池，重启程序后可立即复用。
- Status Area只有4096字节，使用2MB大页会浪费整个大页99.8%的空间，普通页即可。

### 3.2 Kernel侧（驱动分配）

| 内存区域 | 分配方式 | 用途 |
|---|---|---|
| Coherent Buffer（地址表） | `dma_alloc_coherent` | CPU填写、FPGA读取的IOVA地址表 |

Coherent Buffer是CPU和FPGA均可访问的共享内存，格式为：

```
Entry[0]: { IOVA地址, 长度 }   ← Swath Buffer第1段
Entry[1]: { IOVA地址, 长度 }   ← Swath Buffer第2段
...
Entry[N]: { IOVA地址, 长度 }   ← Swath Buffer第N段
```

### 3.3 Status Area数据结构

```
偏移 +0x00: dmaed_line_count (int64)  ← 本次Swath已传输的行数
偏移 +0x08: crc_error        (int32)  ← CRC错误计数（0=无错误）
偏移 +0x0C: state            (int32)  ← 状态码（-2=正常结束，-3=提前结束）
偏移 +0x10: _pad             (保留至4096字节)
```

### 3.4 HugeTLB对sg段数的影响

10GB Swath Buffer经过`sg_alloc_table_from_pages`后产生的sg段数：

| 页面类型 | 页面大小 | sg段数（理论） | Coherent Buffer大小 |
|---|---|---|---|
| 普通页 | 4KB | ~2,621,440段 | ~40MB |
| HugeTLB | 2MB | ~5,120段 | ~80KB |

使用HugeTLB后，FPGA读取地址表的PCIe事务数从百万级降至千级，显著降低延迟和PCIe总线占用。

---

## 4. BAR0寄存器接口

Host驱动通过PCIe BAR0寄存器将地址信息传递给FPGA。每次启动新的Swath传输时写入一次，之后FPGA自主工作。

| 寄存器 | 偏移 | 方向 | 内容 |
|---|---|---|---|
| REG_TABLE_ADDR_LOW | 0x00 | Host→FPGA | 地址表IOVA低32位 |
| REG_TABLE_ADDR_HIGH | 0x04 | Host→FPGA | 地址表IOVA高32位 |
| REG_TABLE_COUNT | 0x08 | Host→FPGA | 地址表条目数 |
| REG_STATUS_ADDR_LOW | 0x0C | Host→FPGA | Status Area IOVA低32位 |
| REG_STATUS_ADDR_HIGH | 0x10 | Host→FPGA | Status Area IOVA高32位 |
| REG_CTRL | 0x14 | Host→FPGA | 控制命令（1=START，2=STOP，4=RESET） |

> **注**：以上偏移值为占位符，需EE提供最终寄存器地址表后修改`ida_hw.h`。

---

## 5. 传输流程

### 5.1 初始化（一次性，驱动加载后执行）

```
App                          Kernel Driver                    IOMMU
 │                                │                             │
 │  mmap(MAP_HUGETLB)             │                             │
 │  分配 Swath Buffer (10GB)       │                             │
 │  shm_open + mmap               │                             │
 │  分配 Status Area (4KB)        │                             │
 │                                │                             │
 │──── ioctl CMD_INIT ──────────→ │                             │
 │                                │── pin_user_pages ──────────→│
 │                                │   锁定物理页，防止swap        │
 │                                │                             │
 │                                │── dma_map_sg ─────────────→│
 │                                │   建立 PA→IOVA 映射          │
 │                                │←──────── 返回IOVA列表 ───────│
 │                                │                             │
 │                                │── dma_alloc_coherent        │
 │                                │   分配地址表(coherent buffer)│
 │                                │   填入各段IOVA和长度          │
 │←──── 返回成功 ─────────────────│                             │
```

### 5.2 每次Swath传输

```
App          Kernel Driver        BAR0         FPGA            Host Memory
 │                │                │             │                  │
 │─CMD_START──→  │                │             │                  │
 │               │── writel ──→   │             │                  │
 │               │   写地址表IOVA  │             │                  │
 │               │── writel ──→   │             │                  │
 │               │   写StatusIOVA │             │                  │
 │               │── writel ──→   │             │                  │
 │               │   写START命令   │──触发────→  │                  │
 │               │                │             │── 读地址表 ──→   │
 │               │                │             │← 返回IOVA列表 ──│
 │               │                │             │                  │
 │─CMD_WAIT──→  │                │             │                  │
 │  (阻塞等待)   │                │             │─────DMA写────→  │
 │               │                │             │   按IOVA写入     │
 │               │                │             │   Swath Buffer   │
 │               │                │             │                  │
 │               │                │             │── DMA写 ──→     │
 │               │                │             │   写Status Area  │
 │               │                │             │   (行数/CRC/状态) │
 │               │                │             │                  │
 │               │                │             │── MSI-X ──→     │
 │               │◄── ISR唤醒 ────────────────────                  │
 │               │   设transfer_done=true        │                  │
 │               │   wake_up()                  │                  │
 │◄─返回结果────│                │             │                  │
 │  (行数/字节数) │                │             │                  │
 │               │                │             │                  │
 │ 直接读        │                │             │                  │
 │ Swath Buffer  │                │             │                  │
 │ （零拷贝）    │                │             │                  │
 │ 直接读        │                │             │                  │
 │ Status Area   │                │             │                  │
```

### 5.3 多通道轮流工作

4个IDA通道轮流传输，同一时刻只有1个通道活跃。通道切换时直接对下一个通道调用CMD_START即可，无需重新初始化（地址表在CMD_INIT时已建立好，每个通道独立保存）。

---

## 6. 软件架构

### 6.1 文件结构

```
ida/
├── include/
│   └── ida_uapi.h        ← 内核/用户空间共享接口（ioctl定义、结构体）
├── kernel/
│   ├── ida_hw.h          ← 硬件常量（PCI ID、BAR偏移、寄存器定义）
│   ├── ida_driver.h      ← 内核内部数据结构
│   ├── ida_driver.c      ← 主模块（probe/remove/ioctl/ISR）
│   ├── ida_dma.c         ← DMA管理（pin/map/coherent buffer）
│   └── Makefile
└── app/
    ├── ida_app.c         ← 用户空间采集程序
    ├── ida_app.conf      ← 应用配置（通道数、每行字节数）
    └── Makefile
```

### 6.2 ioctl接口

| 命令 | 方向 | 时机 | 作用 |
|---|---|---|---|
| CMD_INIT | App→Kernel | 启动时，一次 | Pin页面、建立IOMMU映射、构建地址表 |
| CMD_START | App→Kernel | 每个Swath开始 | 写BAR0寄存器，触发FPGA开始DMA |
| CMD_WAIT_DONE | App←Kernel | 阻塞等待 | 等Swath End中断，返回行数/字节数 |
| CMD_CLEANUP | App→Kernel | 退出时 | 撤销IOMMU映射，释放pinned pages |

---

## 7. 与EE的接口约定

以下内容需要与FPGA工程师对齐：

### 7.1 FPGA侧职责

1. **读地址表**：收到BAR0 START命令后，从`coherent_iova`地址读取条目列表
2. **按顺序DMA写入**：按地址表顺序，将采集数据写入各条目指定的IOVA地址
3. **写Status Area**：传输完成后，将`dmaed_line_count`、`crc_error`、`state`写入`status_iova`地址
4. **发MSI-X中断**：写完Status Area后，触发Swath End中断（vector 0）

### 7.2 需EE提供的信息

| 信息 | 用途 |
|---|---|
| PCI Device ID（ZU7EG和KU5P） | `ida_hw.h`中的设备匹配 |
| BAR0寄存器偏移表 | 替换`ida_hw.h`中的占位偏移值 |
| CTRL_CMD_START的值 | REG_CTRL寄存器的触发命令字 |
| MSI-X Swath End向量号 | 确认使用哪个vector |
| State字段的状态码定义 | -2/-3的具体含义 |

### 7.3 数据格式

- 每行数据：**3072字节**（= 2048采样点 × 1.5 字节/采样点）
- actual_bytes = dmaed_line_count × 3072
- 每个Swath的行数由FPGA决定，Host按实际`dmaed_line_count`处理数据

---

## 8. 系统部署要求

### 8.1 运行前配置（一次性，需root）

```bash
# 1. 预留 HugeTLB 2MB 大页
#    每个通道需要：10GB / 2MB = 5120 页，加余量建议 5500 页/通道
#    1个通道测试：
echo 5500 > /proc/sys/vm/nr_hugepages
#    4个通道生产：
#    echo 22000 > /proc/sys/vm/nr_hugepages

#    永久生效（写入 sysctl.conf）：
echo "vm.nr_hugepages = 5500" >> /etc/sysctl.conf
sysctl -p

#    验证分配成功：
grep HugePages /proc/meminfo
#    HugePages_Total 必须等于设置值
#    HugePages_Free  必须充足（>= 5120 × 通道数）

# 2. 启用IOMMU（在GRUB中添加参数，然后重启）
#    编辑 /etc/default/grub：
#    GRUB_CMDLINE_LINUX="... intel_iommu=on iommu=pt"
grub2-mkconfig -o /boot/grub2/grub.cfg
# reboot

# 3. 验证IOMMU已启用
dmesg | grep -i iommu
# 应看到：DMAR: IOMMU enabled
```

### 8.2 驱动加载

```bash
# 编译
cd kernel && make

# 加载
insmod ida.ko

# 确认设备文件创建
ls /dev/ida_dma
```

### 8.3 应用配置文件

```ini
# /etc/ida_app.conf
IDA_COUNT       = 1        # 通道数（测试用1个）
BYTES_PER_LINE  = 3072     # 每行字节数
```

### 8.4 HugeTLB崩溃恢复说明

HugeTLB页面由内核池统一管理，不属于任何进程。应用程序崩溃后：

1. Linux内核自动回收进程持有的mmap映射
2. 占用的HugeTLB页面归还到内核池
3. 重新运行应用程序时，`mmap(MAP_HUGETLB)`直接从池中分配，无需任何手动操作

因此，**崩溃后重启程序无需任何额外处理**，只要内核池配置（`nr_hugepages`）保持不变即可。

---

## 9. 关键设计决策说明

**为什么Swath Buffer用HugeTLB而不是THP**：THP（Transparent Huge Page）是内核自动决策的，不保证100%分配到2MB大页，在内存碎片较多时可能退化为4KB小页，导致sg段数剧增。HugeTLB是显式预分配的大页，使用`MAP_HUGETLB`强制保证页面大小，sg段数固定可预测（约5120段）。此外，HugeTLB在程序崩溃后自动归还给内核池，行为比THP更可靠。

**为什么Status Area不用HugeTLB**：Status Area只有4096字节，如果使用2MB HugeTLB页，99.8%的空间会被浪费。普通页面完全满足需求。

**为什么Status Area放Host内存**：FPGA直接DMA写入Host内存，App直接读取，零拷贝。相比放在FPGA BRAM再由Host通过BAR读取，省去一次PCIe读操作，且App可以直接cast访问结构体。

**为什么用Coherent Buffer而不是普通内存**：Coherent Buffer由`dma_alloc_coherent`分配，同时具有CPU可用的虚拟地址和FPGA可用的IOVA，且保证cache一致性。普通`kmalloc`内存FPGA无法直接访问。

**Swath End中断是唯一完成信号**：不依赖任何字节计数或超时，完全由FPGA控制节奏。FPGA可以在任意时刻发送Swath End（提前结束或正常结束），Host通过`dmaed_line_count`知道实际传输量。
