# Pico 2 W / 单片 ADS1299 调试说明

本工程默认上电打开 ADS1299 电源，用 SPI0 + RX/TX DMA 采集 8 通道，USB CDC 输出诊断信息。默认 **内部测试信号、增益 1、250 SPS（以 2.048 MHz MCLK 为前提）**。SD、UART 日志、25 Hz 抽样预览默认关闭。新增 Core 1 Wi-Fi 热点和 TCP 数据流默认开启，见 [Wi-Fi 调试说明](docs/WIFI_DEBUG.md) 和 [协议 V1](docs/PROTOCOL_V1.md)；`ENABLE_WIFI_STREAM=OFF` 可恢复单核采集构建。没有加入屏幕或板端数据存盘。

**验证状态：已编译验证，尚未实机验证。未烧录硬件。** 软件单元测试通过不代表电源、SPI 时序、外部时钟或模拟性能已通过验证。

## 文件和配置

USB 运行中设置采样率/增益：`:rate 1000`、`:gain 24` 后按 Enter。完整选项、SPI 自动调速及失败处理见 [USB 参数调试](docs/USB_PARAMETERS.md)。

| 文件 | 用途 |
| --- | --- |
| `ads1299_config.h` | 固定引脚、主时钟假设、SPI 时钟、默认模式/增益/采样率、正常输入参考选择、等待时间和队列容量 |
| `ads1299_registers.h` | TI 命令、寄存器地址和使用到的位定义 |
| `ads1299.h` / `ads1299.c` | 单实例驱动、上电、寄存器校验、DRDY/DMA、帧队列、超时和恢复 |
| `ads1299_format.h` / `ads1299_format.c` | 平台无关的 27 字节帧解析、24 位符号扩展、采样率/增益编码和带宽检查 |
| `debug_console.h` / `debug_console.c` | USB CDC 有界日志队列、非阻塞接收、可选 UART 发送 |
| `spi_dma.c` | 启动流程、每秒统计、调试命令、可选抽样预览 |
| `sd_legacy.c` | 保存原 SD/BDF 100 MiB 测速入口，改为手动调用 |
| `tests/test_ads1299_format.c` | 本机可执行的解析和配置边界测试 |

原 `bdf_writer.c/.h`、`pico_fatfs_reference` 保留且未修改。外部 STM32 参考文件未修改。参考驱动的 8 kSPS / 16 kSPS 未赋值分支已替换为完整查表；不保留 STM32 HAL、EXTI、lwrb 等依赖。

## 接线和上电

| Pico GPIO | ADS 信号 | 配置 |
| --- | --- | --- |
| GP16 | DOUT | SPI0 RX，输入 |
| GP17 | CS# | 软件 GPIO，低有效 |
| GP18 | SCLK | SPI0 SCK，空闲低 |
| GP19 | DIN | SPI0 TX |
| GP20 | DRDY# | GPIO 下降沿中断 |
| GP21 | 5V_EN | 上电尽早输出高 |

`ads1299_power_on()` 在 USB 初始化前执行。GP16～GP20 起初设为无内部上拉的输入，随后 GP21 拉高，避免 ADS 未供电时从 SPI 引脚反向供电。等待完成后，CS 设为高，再启用 SPI。

原理图中 RESET#/PWDN# 上拉到 DVDD、START 下拉，采用 SPI RESET/START/STOP 命令，不存在可由固件控制的额外复位/启动引脚。

**CLKSEL 下拉，外部 U12 必须输出 MCLK。U12 的型号/频率没有标出，当前 `ADS_MCLK_HZ=2048000` 是待测假设。** CONFIG1 的 CLK_EN 保持 0，不会通过该位改变 CLKSEL 硬件选择。没有 MCLK 时，应检查硬件，而不是仅修改 CONFIG1。

上电等待至少覆盖：电源/时钟稳定裕量 100 ms + `2^18 / fCLK`，并取不低于 `ADS_ANALOG_STARTUP_MS=1000 ms` 的等待。原理图 VCAP1 使用 100 µF；必须实测 VCAP1 > 1.1 V 后才允许 RESET。软件没有电源良好或 VCAP1 检测输入，1 秒只是可调的板级裕量，不是稳定性保证。参考缓冲使能后另等 150 ms。

先后顺序为：开电 → 等待 → SPI 初始化 → RESET → SDATAC → ID → 配置并读回 → RDATAC → START → 开启 DRDY 采集。默认 GPIO/DMA 中断都在 core 0，禁止从另一个 core 调用驱动 API。

## 寄存器与默认结果

以 2.048 MHz、250 SPS、增益 1、内部测试为例：

| 寄存器 | 期望值 | 说明 |
| --- | --- | --- |
| ID | 常见 0x3E | 校验低 5 位为 0x1E，保留版本位差异；只接受 8 通道器件 |
| CONFIG1 | 0xD6 | 保留位符合要求，DAISY_EN=1 独立器件多次读回模式，CLK_EN=0，DR=6 |
| CONFIG2 | 0xD0 | 内部测试、幅度档 0、频率 `fCLK / 2^21` |
| CONFIG3 | 0xE8 | 内部参考开启、内部中点 BIASREF，BIAS 缓冲默认关闭 |
| CH1SET～CH8SET | 0x05 | 增益 1、测试输入、SRB2 断开 |
| BIAS_SENSP/N、LOFF 相关可写寄存器 | 0x00 | 默认不参与偏置推导或脱落检测 |
| GPIO | 低四位 0xF | ADS GPIO 全部输入，高四位为实际引脚电平 |
| MISC1、MISC2、CONFIG4 | 0x00 | SRB1 断开、保留寄存器为 0、连续转换 |

CONFIG3 读回忽略只读 BIAS_STAT 位，GPIO 读回仅比较方向位；其他被配置的寄存器逐一比较。读写均遵守 CS 保持及字节解码等待。任何 SPI 等待都有控制事务超时，错误不会伪装成读到 0xFF。

`ads1299_read_reg(s)` / `ads1299_write_reg(s)` 只允许在停止状态调用。写入拒绝只读地址、越界和非法保留位值，并使 configured 失效；必须 `ads1299_configure()` 后才能重新启动，避免软件采样周期与手工修改的寄存器不一致。`configure()` 在正在采集时先停止，但完成后保持停止，由调用者决定是否 `start()`。

默认正常输入模式为独立 INxP/INxN 差分输入，SRB1、SRB2、BIAS 均关闭。根据 H2/H5 实际参考跳线配置 `ADS_NORMAL_SRB1`、`ADS_NORMAL_SRB2`、`ADS_NORMAL_BIAS` 和 BIAS 通道掩码；禁止同时开启 SRB1 与 SRB2。本次不会猜测跳线连接。内部测试/短接模式始终关闭 SRB/BIAS 参与，便于隔离外部输入影响。

## DMA、时序与统计含义

SPI0 为 Mode 1（CPOL=0、CPHA=1），8 位、MSB first。默认请求 1 MHz，实际分频结果以启动日志为准。每个 DRDY 下降沿先启动 RX DMA，再启动 TX DMA；TX 重复发送 0x00 空操作产生时钟，RX 接收 **27 字节 = 3 状态 + 8 × 3 通道 = 216 SCLK**。RDATAC 每帧没有额外的 RDATA 命令。

CS 在整个连续采集期间保持低，帧之间通过 DRDY/时钟间隙区分。RX 完成后还检查 TX DMA 和 SPI busy，不在中断中等待尾位。只有停止时，才在 SPI 空闲后等待至少 4 tCLK 再释放 CS，后续保持高至少 2 tCLK。SDK GPIO NVIC 和 DMA NVIC 均显式启用。

DMA 写入独立的 active 缓冲；完成帧复制到静态 64 帧队列。队列满时丢弃新帧，不覆盖消费者未读的数据。主循环在短临界区复制出原始帧，然后解析；生产者也通过临界区与主循环协调。队列容量必须是 2 的幂，支持 32 位计数器自然回绕。默认容量约覆盖 256 ms 的 250 SPS 数据，不能替代长期存储。

采样率编码完整支持标称 250、500、1000、2000、4000、8000、16000 SPS。实际值为：

`fDR = fCLK / (128 × 2^DR_code)`，其中 DR_code 从 0（16000）到 6（250）。

默认 MCLK 下，250 SPS 周期 4 ms；8 kSPS 为 125 µs；16 kSPS 为 62.5 µs。1 MHz 下单帧约 216 µs。驱动要求帧传输小于周期的 75%，为 IRQ 延迟与数据更新时间留出余量。新增动态配置会为更高采样率自动选择更快的 SPI 请求（默认上限 8 MHz），并在停采后用 SDK 实际分频结果复核；低采样率保留约 1 MHz。具体档位见 USB 参数文档。仍需用逻辑分析仪测量余量，带宽检查通过不代表中断延迟已获得硬件保证。

| 日志字段 | 含义 |
| --- | --- |
| DRDY | 软件实际处理的下降沿数量 |
| frames / fps | 状态头通过且未受忙冲突污染的完整帧数量 / 统计窗口内速率，包含后来因队列满被丢弃的帧 |
| busy | 前一帧尚未结束又收到 DRDY 的次数；前一帧标为污染，不交付 |
| qdrop / qpeak | 完成帧入队失败数 / 队列历史最高占用 |
| stopdiscard | 停止、切换模式或恢复时主动丢弃的队列帧及在途帧数量 |
| timeout(drdy/dma/spi) | 分别为无 DRDY、DMA 不完成、SPI 控制事务超时 |
| dmaerr / bad | DMA AHB 或 SPI RX 溢出故障次数 / 状态头不是 0xC 的帧数 |
| recovery | 自动恢复尝试次数，显式初始化之间最多 2 次 |
| logdrop / logbytes | USB 日志整条未入队次数 / 已丢弃字节，包括未连接、队列满和断开时排队字节 |
| uartdrop | 可选 UART 独立队列的日志丢弃次数 |
| last | 最近错误；成功自动恢复后也保留触发故障名称，结合 run 判断当前状态 |

DRDY 超时为 10 个采样周期 + 10 ms，覆盖首次数字滤波稳定时间；DMA 超时为 max(一个采样周期, 两倍预计传输时间 + 100 µs)。连续 3 帧状态头错误会触发恢复。恢复由主循环停止 DMA、复位 ADS、重新配置并启动，最多两次；再次失败后保持停止。输入 `i` 可显式重试并打开新的恢复预算。

停止时先禁止 DRDY 和本驱动的 DMA 中断源，等待在途传输，必要时做有界 abort。abort 之前清除两通道 EN，考虑 RP2350-E5；通过不触发传输的 AL1_CTRL 别名恢复 EN，避免启动旧传输。若 DMA 无法在超时内静止，标记 fatal 并禁止重用 DMA 缓冲，需复位主控；不会无限重试或自动掉电重启。

**ADS 数据没有硬件帧序号。** seq 是软件观察到的 DRDY 计数，时间戳是中断服务时刻。长时间关中断可能让多个边沿合并；全零错误计数不能证明硬件完全无漏帧，必须结合逻辑分析仪的 DRDY 数量和服务延迟评估。

## USB 调试与波形预览

连接 Pico USB 数据口，打开枚举出的串口，启用 DTR（大多数串口终端默认如此）。USB CDC 传输不依赖终端波特率，可选择 115200。固件不等 USB 主机；后来打开串口时会重新输出配置、ID、寄存器校验和帮助。USB 未连接时的 logdrop 增长属于正常诊断行为。

输入单个字符，不必回车：

| 命令 | 操作 |
| --- | --- |
| `?` | 帮助 |
| `s` | 配置、ID、寄存器最近校验和当前错误；每秒统计自动输出 |
| `r` | 安全暂停，读 0x00～0x17，原来正在运行则成功后恢复 |
| `x` / `g` | 停止 / 启动，g 不重置恢复预算；未配置时使用 i |
| `i` | 重新复位、配置并启动；清空自动恢复预算，fatal abort 除外 |
| `t` / `h` / `n` | 切换内部测试 / 内部短接 / 正常输入并启动 |
| `p` | 切换最多 25 Hz 的最新帧预览，默认关闭 |

预览行格式：`sample,seq,time_us,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8`。这是抽样显示，不是原始采样率数据流；seq 跳跃属于预期。终端保存日志后筛选 `sample,` 行可作为 CSV 绘图，横轴用 time_us。默认测试频率约 0.9765625 Hz，周期约 1.024 s，25 Hz 预览足以初步观察测试脉冲。停采后不会反复输出旧预览。

日志只在主循环格式化和入队，USB 每轮最多提交 64 字节，不等待主机腾出空间。未连接或队列满会丢日志，独立于采集 qdrop。TinyUSB 的后台 task IRQ 被关闭，由主循环调用零等待任务接口；不要另开 core 或后台 task 同时访问 CDC。采集 GPIO/DMA IRQ 中无 printf、阻塞 SPI 或等待 DMA。

模式切换、寄存器命令、恢复本身是有界的控制流程，期间采集暂停，参考稳定等待会造成 USB 短暂延迟；这些不是无缝采样操作。主循环只维护软件超时监测，没有新增硬件 watchdog。

## 实机调试顺序

1. **检查供电。** 示波器/万用表测 GP21 应为高。依次测 5V-IN 升压输出（原理图目标约 5.27 V）、AVDD（约 5 V）和 DVDD（约 3.3 V），并确认 GND/AGND 单点连接。测量上升过程、纹波和 VCAP1 到 1.1 V 的时间，必要时增加等待参数。5V-IN 与 ADS 的 AVDD 是不同节点。
2. **检查 MCLK。** 在 U12 输出或 ADS CLK 测量频率、幅度和连续性，确认 CLKSEL 为低。将测得的频率写入 ADS_MCLK_HZ，再编译；检查 RESET#/PWDN# 为高、START 为低。
3. **检查寄存器通信。** 逻辑分析仪接 GP17/18/19/16 和数字地，Mode 1、MSB first、8 bit，采样频率至少为 SCLK 的 10 倍。观察 RESET/SDATAC、RREG/WREG 和完整 CS 事务，USB ID 常见 0x3E，所有期望寄存器比较通过。
4. **验证内部测试。** 输入 t，再输入 p。增加 DRDY 探头 GP20，250 SPS 应约每 4 ms 下降一次；每个正常读取突发严格 216 个时钟，1 MHz 时约 216 µs，DIN 为 0，帧前三字节是状态。CS 连续保持低，因此逻辑分析仪需按 DRDY/突发边界分帧，不要把整个 CS 低区间当一帧。状态最高四位为 0xC，随后通道按 CH1→CH8 排列。各通道应观察到同频测试脉冲；跳变附近允许数字滤波过渡。
5. **验证内部短接。** 输入 h。原始码应在偏移附近小幅变化，不能要求严格为零；比较通道离散程度并检查是否饱和。默认增益 1 用于初次联通检查，需要测噪声性能时按数据手册选择增益、带宽及足够样本数，25 Hz 预览不能替代全速噪声测量。
6. **验证正常输入。** 先确认 H2/H5、参考电极和 INxN/INxP 的真实连接，并相应修改 SRB/BIAS 配置。输入 n，用已知受控的小信号源验证极性、幅值及共模范围。当前默认差分模式不能将悬空的 INxN 自动当成公共参考。
7. **持续运行。** 默认模式先运行至少 10 分钟，记录 frames/fps、busy、qdrop、bad、timeout 和 recovery；开关预览、断开/重新连接终端，核对采集统计。比较分析仪的 DRDY 总数、传输突发数及最差服务延迟，再逐级提高采样率。寄存器 dump/模式切换产生的主动停采应单独记录。

内部参考默认约 4.5 V；通道原始码换算输入电压可用 `code × VREF / (gain × 2^23)`，实际以测得的 VREF 和增益为准。测试信号 CAL_AMP=0 的幅度标度按手册为 VREF/2400（约 1.875 mV），应结合手册测试波形定义和滤波过渡判断，而不是把单个瞬时码作为完整幅度验证。

## 常见现象

| 现象 | 排查方向 |
| --- | --- |
| ID 全 0 | 供电、MCLK、RESET/PWDN、CS、MISO 接错或短路；检查共地 |
| ID 全 1 | MISO 悬空、CS 未选中、器件未上电、连线断开；以波形确认而非只看读值 |
| ID 正常、配置读回失败 | 是否退出 RDATAC、SPI Mode 1、字节解码间隔、保留位、实际主时钟 |
| 无 DRDY | MCLK 缺失、START 未生效、PWDN/RESET 电平、GP20 连线；看 run 和恢复计数 |
| 帧错位 / bad 增长 | 每帧是否多/少时钟、错误添加 RDATA、SPI 相位、MISO 电平、前一帧是否跨过下一 DRDY |
| 测试频率不符 | 实测 MCLK 与配置不同，CONFIG2 CAL_FREQ 或 MUX 未设置；不要用每秒摘要重建波形 |
| 只有部分通道异常 | CHnSET、输入连接、参考跳线或模拟前端；先用内部测试隔离外部输入 |
| busy 或 DMA timeout | SPI 太慢、ISR 延迟、DMA DREQ/完成状态、FIFO 溢出、外部时钟引发采样率变化 |
| qdrop 增长但 DMA 正常 | 主循环消费不及时；检查新增阻塞代码，区别日志丢弃与采集队列丢弃 |
| run=0 且 recovery=2 | 自动预算耗尽；排除硬件问题后输入 i；fatal abort 必须复位主控 |
| USB 没输出 | 数据线、枚举、DTR；固件可能已在无日志状态下运行，重连会重发配置 |

## 构建和可选功能

在工程目录的 PowerShell 运行，沿用已安装 Pico 工具链：

```powershell
$adsCmake = "$env:USERPROFILE/.pico-sdk/cmake/v3.31.5/bin/cmake.exe"
& $adsCmake -S . -B build -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$env:USERPROFILE/.pico-sdk/ninja/v1.12.1/ninja.exe" `
  "-DPICO_SDK_PATH=$env:USERPROFILE/.pico-sdk/sdk/2.2.0" `
  "-DPython3_EXECUTABLE=C:/ncs/toolchains/2d382dcd92/opt/bin/python.exe" `
  -DENABLE_WIFI_STREAM=ON -DENABLE_SD_CARD=OFF -DENABLE_UART_LOG=OFF
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
& $adsCmake --build build -j 8
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
```

产物：`build/spi_dma.uf2`、`build/spi_dma.elf`、`build/spi_dma.bin`。仅构建，不自动烧录。此机 PATH 中的 Rye Python shim 无法用于全新 CMake 配置，因此示例显式指定原 build 缓存已使用的 Python 3.12.4；换电脑时改成有效 Python 解释器路径。

需要编入旧 SD 测速时将 ENABLE_SD_CARD 设为 ON，再输入 b 手动运行；默认启动仍是 ADS，b 会先停止 ADS，执行原来的同步 SD/BDF 测速，结束后保持停止，使用 g 手动恢复。此旧测试不是实时数据存储路径，不受 ADS 日志队列约束，执行时 USB 可能延迟。关闭该开关时不加入 FatFs/PIO-SPI 子目录，不编译 sd_legacy 或 bdf_writer，也不会初始化 SPI1。

ENABLE_UART_LOG=ON 仅额外启用 UART0 TX GP0、115200 日志，使用独立队列；**GP0 与原理图屏幕 MOSI 冲突，仅在确认屏幕接线影响后启用**。不配置 GP1，不从 UART 接收命令。默认关闭时固件无 UART 初始化。保留了 Pico VS Code 扩展配置块。

主机测试命令（此机已安装 MinGW GCC）：

```powershell
& 'C:/MinGW/bin/gcc.exe' -std=c11 -O2 -Wall -Wextra -Werror -I . `
  ads1299_format.c tests/test_ads1299_format.c -o build/test_ads1299_format.exe
if ($LASTEXITCODE -eq 0) { & './build/test_ads1299_format.exe' }
```

测试覆盖全部 16,777,216 个 24 位输入值、8 通道布局、状态头、原始数据/时间戳保留、合法/非法采样率及增益、外部时钟周期换算和 SPI 带宽边界。维护的应用源文件以 `-Wall -Wextra -Werror` 编译，第三方代码保持原编译选项。

ADS 驱动阶段曾完成默认及 `build/optional` 中 SD/UART 可选构建。新增 Wi-Fi 后实际验证了 Wi-Fi ON/OFF、SD/UART 均 OFF 的两个构建及主机测试，详见 [Wi-Fi 验证记录](docs/WIFI_DEBUG.md)。硬件 IRQ、DMA、USB 背压、参考稳定和电气时序仍需按上述步骤实测。

## 依据

- 硬件：用户提供 `C:/Users/liuzh/Desktop/Mailbox/Schematic.pdf`，第 1 页主控、第 2 页供电、第 3 页 ADS1299。
- 迁移参考：用户提供的 `ADS1299_Definitions.h`、`ADS1299_Library.h/.c`。
- [TI ADS1299 数据手册 SBAS499C](https://www.ti.com/lit/ds/symlink/ads1299.pdf)：7.6 串行时序、9.4 转换模式、9.5 SPI 命令、9.6 寄存器、11.1 上电顺序。
- 本机 Pico SDK 2.2.0 的 GPIO、SPI、DMA 和 USB stdio 实现；DMA abort 注释中的 RP2350-E5 处理要求。
