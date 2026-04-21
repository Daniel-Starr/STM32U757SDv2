# STM32 串口到 SD NAND 日志记录器

这是一个基于 STM32U5 的串口日志采集工程。程序通过 `UART4` 接收上位机发送的文本数据，并通过 `SDMMC2` 将数据按块写入外部存储器，同时支持串口回读、清空和状态查询。

这个项目更适合被理解为一个“原始块设备日志记录器”，而不是带文件系统的 SD 卡读写例程。当前代码没有使用 FATFS，而是直接调用 HAL 的块读写接口对存储器进行顺序访问。

## 项目功能

固件启动后，会通过串口输出欢迎信息和命令提示。支持以下几种操作：

- 发送任意文本并回车：将这一帧数据写入存储器
- 发送 `READ`：回读当前已保存的全部数据
- 发送 `ERASE`：清空逻辑数据
- 发送 `STATUS`：查看当前存储状态

从 [main.c](C:\Users\wang2\Desktop\test\sd32v2\Core\Src\main.c) 的实现来看，这个工程的核心目标是：

- 验证 `UART4` 接收链路
- 验证 `SDMMC2` 原始块读写
- 实现掉电后可恢复的简单日志存储

## 一个需要先说明的问题

仓库名是 `STM32U757SDv2`，但当前工程实际配置的目标芯片并不是 `STM32U757`，而是 `STM32U575` 系列。

可以从以下文件看出来：

- [sd32v2.ioc](C:\Users\wang2\Desktop\test\sd32v2\sd32v2.ioc) 中 MCU 为 `STM32U575VGT6`
- [sd32v2.uvprojx](C:\Users\wang2\Desktop\test\sd32v2\MDK-ARM\sd32v2.uvprojx) 中设备为 `STM32U575VGTx`
- 工程宏定义使用的是 `STM32U575xx`

如果后续准备迁移到 `STM32U757`，需要重新检查：

- 芯片型号是否正确
- 引脚复用是否一致
- 时钟树配置是否匹配
- HAL / Device Pack 是否需要切换
- UART 和 SDMMC 外设实例是否仍然适用

## 硬件接口

### UART4

串口初始化位于 [usart.c](C:\Users\wang2\Desktop\test\sd32v2\Core\Src\usart.c)。

当前配置：

- 波特率：`115200`
- 数据位：`8`
- 停止位：`1`
- 校验位：`None`
- 模式：`TX / RX`

引脚定义：

- `PA0` -> `UART4_TX`
- `PA1` -> `UART4_RX`

### SDMMC2

SDMMC2 初始化位于 [sdmmc.c](C:\Users\wang2\Desktop\test\sd32v2\Core\Src\sdmmc.c)。

当前连接如下：

- `PB14` -> `SDMMC2_D0`
- `PB15` -> `SDMMC2_D1`
- `PB3` -> `SDMMC2_D2`
- `PB4` -> `SDMMC2_D3`
- `PD6` -> `SDMMC2_CK`
- `PD7` -> `SDMMC2_CMD`

当前代码配置为：

- 总线宽度：`1-bit`
- 时钟分频：`10`
- 时钟边沿：上升沿

虽然引脚已经把 `D0-D3` 全部接出，但代码仍然以 `1-bit` 模式初始化。如果后续想切换到 `4-bit` 模式，还需要进一步确认初始化流程和硬件稳定性。

## 软件工作流程

程序启动流程大致如下：

1. 执行 `HAL_Init()`
2. 初始化系统时钟 `SystemClock_Config()`
3. 初始化 GPIO、ICACHE、UART4
4. 延时约 2 秒，等待存储器稳定
5. 初始化 `SDMMC2`
6. 读取卡信息，获取总块数
7. 从 `Block 0` 加载元数据
8. 启动 UART 中断接收

启动完成后，设备会在串口输出命令说明。

## 串口接收逻辑

接收逻辑同样集中在 [main.c](C:\Users\wang2\Desktop\test\sd32v2\Core\Src\main.c) 中。

当前实现方式是：

- 使用中断逐字节接收
- 每收到 1 个字节就放入接收缓冲区
- 如果收到换行符 `\n`，则认为一帧接收完成
- 如果一段时间没有继续收到数据，超过 `100 ms`，也会强制判定当前帧结束

这种做法的优点是实现简单，适合：

- 串口助手发送一行一行的文本
- 人工手动输入命令并回车
- 短报文调试

## 命令解析逻辑

主循环会把当前帧内容拿出来做简单命令匹配。

支持的命令有：

- `READ`
- `ERASE`
- `STATUS`

如果接收到的内容不是上述命令，就会把整帧当作普通数据写入存储器。

需要注意：

- 命令是区分大小写的
- 普通文本默认会连同回车换行一起写入

## 存储设计

### 块划分

程序中定义：

- `SD_BLOCK_SIZE = 512`
- `META_BLOCK = 0`
- `DATA_START_BLOCK = 1`

也就是说：

- `Block 0` 用于保存元数据
- 从 `Block 1` 开始依次保存用户数据

### 元数据结构

元数据结构如下：

```c
typedef struct {
    uint32_t magic;
    uint32_t next_block;
    uint32_t total_bytes;
    uint32_t total_frames;
} SD_Meta_t;
```

字段含义：

- `magic`：用于判断元数据是否有效
- `next_block`：下一次写入的块号
- `total_bytes`：累计写入字节数
- `total_frames`：累计写入帧数

魔数定义为：

```c
#define META_MAGIC 0x53444C47
```

### 写入方式

写入逻辑位于 `Data_Write()` 中，工作方式如下：

- 将收到的一帧数据按 `512` 字节分块
- 不足一块的部分用 `0` 填充
- 每写完一帧后更新元数据
- 将元数据重新写回 `Block 0`

这套方案的优点是简单直接，掉电后容易恢复；缺点也比较明显：

- 没有文件系统
- 没有磨损均衡
- 没有记录每一帧的独立边界信息
- 没有更强的数据完整性校验

因此当前 `READ` 命令做的事情，本质上是：

- 根据累计写入字节数
- 按块顺序把原始数据流重新通过串口发出来

它并不是“按记录格式解析并逐条输出”。

## 串口命令说明

### 1. 保存数据

在串口中发送任意文本并回车，例如：

```text
hello world
```

设备会把这帧内容写入存储器，并输出类似：

```text
[#1] Saved 13 bytes
```

### 2. 读取全部数据

发送：

```text
READ
```

设备会：

- 先输出总帧数
- 再输出总字节数
- 之后连续输出所有已保存的数据
- 最后输出 `--- End ---`

### 3. 清空逻辑数据

发送：

```text
ERASE
```

设备会重置元数据，使存储区在逻辑上变为空。

注意这里的“清空”并不是逐块物理擦除，而是把写指针和统计信息恢复到初始状态，后续新数据会从头覆盖旧数据。

### 4. 查看状态

发送：

```text
STATUS
```

设备会输出：

- 当前累计帧数
- 当前累计字节数
- 已使用块数
- 剩余块数

## 工程结构

建议重点阅读以下文件：

- [main.c](C:\Users\wang2\Desktop\test\sd32v2\Core\Src\main.c)
- [sdmmc.c](C:\Users\wang2\Desktop\test\sd32v2\Core\Src\sdmmc.c)
- [usart.c](C:\Users\wang2\Desktop\test\sd32v2\Core\Src\usart.c)
- [stm32u5xx_it.c](C:\Users\wang2\Desktop\test\sd32v2\Core\Src\stm32u5xx_it.c)
- [sd32v2.ioc](C:\Users\wang2\Desktop\test\sd32v2\sd32v2.ioc)
- [sd32v2.uvprojx](C:\Users\wang2\Desktop\test\sd32v2\MDK-ARM\sd32v2.uvprojx)

目录说明：

- `Core/`：应用层源码
- `Drivers/`：HAL 和 CMSIS 驱动
- `MDK-ARM/`：Keil 工程文件
- `sd32v2.ioc`：STM32CubeMX 配置文件

## 构建环境

从当前工程配置来看，主要使用的是以下工具链和环境：

- STM32CubeMX
- Keil MDK-ARM
- ARMCLANG
- STM32Cube FW_U5

已知信息包括：

- Device Family Pack：`Keil.STM32U5xx_DFP.3.2.0`
- 固件包：`STM32Cube FW_U5 V1.8.0`

## 当前代码的特点与局限

### 优点

- 逻辑清晰，适合入门和调试
- 不依赖文件系统，验证原始块读写比较直接
- 适合作为 UART + SDMMC 联调原型
- 支持掉电后恢复写入位置

### 局限

- 当前读写路径是阻塞式的，不适合高吞吐场景
- `SD_WriteBlock_Safe()` 对 HAL 返回值的处理比较保守，更多依赖卡状态判断
- 没有帧格式、帧头、帧尾或校验字段
- 没有更完整的异常恢复机制
- 仓库里包含较多 CMSIS 示例源码，体积偏大
- [sd32v2.uvprojx](C:\Users\wang2\Desktop\test\sd32v2\MDK-ARM\sd32v2.uvprojx) 中还残留了旧的绝对路径，比如 `Downloads/files (1)`，后续建议清理

## 适用场景

这个工程比较适合以下场景：

- 串口日志落盘验证
- STM32 原始块设备读写实验
- 串口文本采集与掉电恢复验证
- 嵌入式数据记录原型开发

如果后续要继续扩展成正式项目，通常还需要补充：

- 文件系统支持，例如 FATFS
- 更规范的数据帧格式
- 数据完整性校验
- 更稳健的异常处理和状态机
- 硬件连接图与文档说明

## 快速使用

1. 用 Keil 打开 [sd32v2.uvprojx](C:\Users\wang2\Desktop\test\sd32v2\MDK-ARM\sd32v2.uvprojx)
2. 编译并下载到目标板
3. 打开串口工具，设置为 `115200 8N1`
4. 给设备上电，等待启动输出
5. 发送测试命令：

```text
hello
STATUS
READ
ERASE
```

## 后续建议

如果你准备继续维护这个仓库，比较值得优先做的事情有：

1. 修正 Keil 工程中的旧绝对路径
2. 精简不必要的 CMSIS 示例代码
3. 给 README 增加一张硬件连接图
4. 给串口协议增加帧格式和校验
5. 如果目标是通用文件存储，可以考虑接入 FATFS
