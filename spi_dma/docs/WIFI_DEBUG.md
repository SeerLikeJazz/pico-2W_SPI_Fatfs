# Pico 2 W 双核 Wi-Fi 采集调试

默认 Core 0 保持 ADS1299 SPI0 + DMA 采集和 USB 调试，Core 1 创建 `Pico2W_EEG` 热点、DHCP、单客户端 TCP 服务。每个从 ADS 本地队列取出的有效原始帧都提交网络队列，网络不使用 latest 或 25 Hz 预览作为数据源。SD、UART 默认 OFF，Wi-Fi 默认 ON。

本次完成实际编译和主机自动化验证，**双核无线功能尚未实机验证，未自动烧录**。先前用户提供的 ADS 运行日志不能替代新增 Wi-Fi 功能验证。外部 MCLK 频率及正常输入参考跳线仍需硬件确认。

## 快速开始

1. 按 [ADS 调试说明](../README_ADS1299.md) 确认电源、外部 MCLK、SPI 和内部测试采集。默认仍为内部测试、gain=1、标称 250 SPS；MCLK=2.048 MHz 仍标为假设。
2. 自行烧录已构建固件后，连接 USB CDC，查看 `WiFi ... state=2`。state=0 未启动，1 正在初始化/等待重试，2 网络接口及服务已建立，3 重试耗尽。state=2 不替代实际热点可见性和无线收发验证。
3. 电脑连接开放热点 **Pico2W_EEG**，无需密码，IP 自动获取，通常为 192.168.4.16～23，掩码 255.255.255.0。热点没有互联网，系统的“无互联网”提示不影响本地 TCP 接收。避免系统自动切回其他无线网络。
4. 工程根目录运行 Python 3.8 或更新版本，无第三方依赖：

```powershell
python host/eeg_receiver.py
python host/eeg_receiver.py --host 192.168.4.1 --port 5000 --seconds 60 --csv build/eeg.csv --raw build/eeg.bin
python host/eeg_receiver.py --offline build/eeg.bin --csv build/eeg_offline.csv
```

默认每秒统计，不逐样本打印。`--retries 5` 表示首次连接之外，整个运行期间最多再尝试 5 次，成功连接不会重置预算；连接超时 5 秒，重试间隔默认 2 秒。已连接后的 1 秒接收超时只让主循环继续，不将合法停采当成断线。`--seconds` 的截止检查可能被一次连接调用延后最多约 5 秒。Ctrl-C 可结束。

`--raw` 追加保存收到的所有 TCP 字节，包括错误候选包；`--csv` 新建/覆盖 CSV。不同输入/输出必须使用不同路径。CSV 包含连接编号、stream_id、包/采样序号、采样参数、状态和 ch1～ch8 原始码。timestamp_kind 为 recorded_irq 时是包首帧记录值，为 estimated 时是根据周期估算。可在 Excel、LibreOffice 或已有绘图软件导入 CSV，以 timestamp_us/1000000 为横轴、ch1～ch8 为纵轴查看波形；按连接和 stream_id 分段，不能跨缺口连线假装连续。高采样率下逐行 CSV 磁盘输出可能使接收程序变慢，优先保存二进制后离线解析。

5. 保留现有 USB 命令，用 `s` 看采集和网络状态，`p` 预览仍独立控制；停止/启动、读寄存器、切模式的原命令见 ADS 文档。网络仅传数据，输入的 TCP 字节被确认并丢弃，不解释为采样命令。

## 配置与资源

| 文件/项 | 默认值和用途 |
| --- | --- |
| CMake ENABLE_WIFI_STREAM | ON；OFF 时不编译网络源，不启动 Core 1 |
| ENABLE_SD_CARD / ENABLE_UART_LOG | OFF / OFF；避免 SPI1/SD 和 GP0/GP1 日志占用 |
| net/wifi_config.h | SSID、密码、TCP 端口、重试、无进展超时、Core 1 栈、MCLK 假设标记 |
| EEG_AP_PASSWORD | 空字符串→开放调试 AP；8～63 字节→WPA2 AES PSK；其他长度报配置错误 |
| EEG_NO_PROGRESS_US | 10 秒，有待发或未 ACK 数据且无进展时断开慢客户端 |
| EEG_AP_RETRY_LIMIT / EEG_AP_RETRY_US | 整次上电最多 3 次初始化/恢复尝试，间隔 2 秒 |
| net/eeg_stream.h | 512 样本跨核队列、36 样本包、200 ms 组包期限 |
| net/lwipopts.h | lwIP 堆、TCP 和 pbuf 预算 |
| ads1299_config.h | ADS 模式、增益、标称采样率、MCLK 和实际硬件参考配置 |

开放热点仅用于当前调试，不是生产配置。WPA2 支持通过集中密码配置选择，日志只报告认证类型，不输出密码。AP IP 在 CMake 的 CYW43 默认 AP 地址及 wifi_stream.c 的地址配置处均为 192.168.4.1；如更改，须同步两处以及接收端地址。

SDK 2.2.0 采用 `pico_cyw43_arch_lwip_poll`、NO_SYS=1 和 raw API。Core 1 完成 CYW43 初始化、轮询、全部 lwIP 调用、DHCP、连接回调及资源释放，Core 0 不调用这些接口；Core 1 不调用 USB/TinyUSB 或 debug_log。统计以原子字段发布，由 Core 0 打印近似快照。

ADS 先在 Core 0 申请 DMA 和 IRQ，再启动 Core 1。SDK CYW43 SPI 后端使用 PIO 和动态申请的 DMA 资源；ADS 同样动态申请 DMA，处理共享 DMA IRQ 时只清除自己的 RX 标志。GPIO/ADS DMA 中断仍在 Core 0。poll 架构的网络任务不借助 Core 0 的 USB 服务；Core 1 主循环短暂等待 250 µs，每轮消费最多 64 帧，避免持续清队列挤占网络轮询。不要从 Core 0 添加 lwIP 调用。

Core 1 使用显式提供的 **8192 字节栈**。SDK 还可能在链接布局中保留默认的 2048 字节 Core 1 栈，该保留区不作为本任务实际栈。实际最大栈深尚需硬件测量，不能把“编译成功”解释成栈高水位验证。

| 静态/配置资源 | 字节数 |
| --- | ---: |
| 跨核队列及计数 | 28696 |
| 协议组包状态（含两个 1024 缓冲） | 2152 |
| Core 1 实际栈 | 8192 |
| lwIP MEM_SIZE | 32768，另有管理/对齐开销 |
| pbuf 池 | 16 × 1536，另有管理开销 |
| TCP MSS | 1460 |
| TCP 发送缓冲上限 | 11680（8 MSS），占用来自 lwIP 分配器 |
| TCP 接收窗口 | 5840（4 MSS） |
| TCP segment 数量 | 64 |

发送缓冲可同时容纳约 11 个应用包字节量，不要求逐包等 ACK；它不是在 lwIP 堆以外再静态增加一份同等内存。双核版本整体 BSS 约 117 KiB，具体 ELF 数值见文末构建记录；还需考虑栈、堆、SDK 保留区，不能用单个队列大小代表全部 RAM 需求。

## 统计与故障定位

原有采集 DRDY、frames、busy、qdrop、超时、bad 等统计保持独立。网络字段累计至重启，32 位计数自然回绕；Core 0 输出的多个字段并非同一时刻的事务快照。

| 字段 | 含义 |
| --- | --- |
| q / peak | 跨核队列当前深度/历史峰值 |
| sample_drop | net_sample_drop；跨核队列满，生产者丢弃新网络样本 |
| no_client | 无 TCP 客户端及接受新连接前，消费丢弃的网络样本 |
| packets / partial | 组装完成的包数/不足 36 帧的包数 |
| disconnect_drop(q/sample/packet) | 断线清理队列中的样本/组包及待发区样本/被清理包数（含未封装非空组包） |
| enqueued | tcp_write 成功 COPY 到 lwIP 的字节数 |
| acked | tcp_sent 报告已获对端 TCP ACK 的字节数 |
| backpressure | 因 sndbuf 不足或 ERR_MEM 未能入队的尝试次数，不是丢包数 |
| connect/disconnect/reject | TCP 接受、断开、拒绝额外连接次数 |
| unacked_on_disconnect | 断线时已入队但未 ACK 的字节，交付状态未知 |
| error | 最近错误；0 成功，负数为 lwIP err_t，1001 密码长度，1002 CYW 初始化，1003 AP/DHCP/TCP 建立，1004 发送无进展超时，1005 AP netif 被关闭 |

无客户端时 no_client 持续增长是预期行为；网络 queue 满只增加 sample_drop，不应使 ADS qdrop 或 busy 增长。USB logdrop 表示日志队列丢弃，不能当作采样丢失；网络新增日志也可能使 logdrop 增长。ACK 数不等于上位机已保存字节数。

热点不可见：先看 state/attempt/error、电源和目标板 pico2_w 配置。state=2 仅能确认 SDK 接口/服务状态；驱动对 AP 的 link_status 不能按 STA 已关联状态解释，当前实现不会因此无限重启无线模块。重试耗尽继续保持采集和 USB。

已连热点但 TCP 失败：检查电脑是否取得 192.168.4.x 地址、是否自动切换网络、端口是否 5000，以及是否已有另一接收程序连接。DHCP 当前支持 8 个地址租约，24 小时租期；断开 Wi-Fi 不一定立即释放租约。TCP 数据客户端仍仅允许一个。

有连接但无数据：检查 ADS run、ID、DRDY、是否停采；没有样本不会发空包。持续 backpressure 或 q 上升：检查接收程序是否读取、CSV/磁盘是否过慢、无线质量；慢连接有界缓冲，最终会丢弃新网络样本或因 10 秒无进展而断开。close 遇 ERR_MEM 直接使用 abort 作为有界回退；RST/FIN 均清理本连接，错误回调不使用已被 lwIP 释放的 PCB。

CRC/长度错误：检查上位机是否按 TCP 字节流累积，是否错误地发送 C struct，是否混入文本；接收程序会重新同步，不能用忽略 CRC 来掩盖问题。发现样本缺口时同时记录本地采集、跨核丢弃和断线统计。软件无异常也无法排除完全未被 MCU 捕获的硬件 DRDY 边沿，必要时用逻辑分析仪独立计数。

## 构建与自动化验证

在工程根目录 PowerShell 执行（本机安装路径；其他机器按实际工具位置替换）：

```powershell
$cmakeExe = "$env:USERPROFILE/.pico-sdk/cmake/v3.31.5/bin/cmake.exe"
$ninjaExe = "$env:USERPROFILE/.pico-sdk/ninja/v1.12.1/ninja.exe"
$sdkPath = "$env:USERPROFILE/.pico-sdk/sdk/2.2.0"
$pythonExe = 'C:/ncs/toolchains/2d382dcd92/opt/bin/python.exe'
& $cmakeExe -S . -B build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninjaExe" "-DPICO_SDK_PATH=$sdkPath" "-DPython3_EXECUTABLE=$pythonExe" -DPICO_BOARD=pico2_w -DENABLE_WIFI_STREAM=ON -DENABLE_SD_CARD=OFF -DENABLE_UART_LOG=OFF
& $cmakeExe --build build --parallel 4
& $cmakeExe -S . -B build/no_wifi -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninjaExe" "-DPICO_SDK_PATH=$sdkPath" "-DPython3_EXECUTABLE=$pythonExe" -DPICO_BOARD=pico2_w -DENABLE_WIFI_STREAM=OFF -DENABLE_SD_CARD=OFF -DENABLE_UART_LOG=OFF
& $cmakeExe --build build/no_wifi --parallel 4
powershell -ExecutionPolicy Bypass -File tests/run_tests.ps1
```

产物：默认双核 `build/spi_dma.uf2`、`.elf`、`.elf.map`；单核 `build/no_wifi/spi_dma.uf2`、`.elf`、`.elf.map`。测试脚本可用 `-Python`、`-CC` 覆盖 Python 和 MinGW GCC 路径。

2026-09-11 本机实际构建结果：SDK 2.2.0、pico2_w、Arm GCC 14.2.1；ON/OFF 均成功，维护源的 `-Wall -Wextra -Werror` 检查通过。`arm-none-eabi-size` 报告：

| 构建 | text | data | bss |
| --- | ---: | ---: | ---: |
| Wi-Fi ON | 339080 | 0 | 119348 |
| Wi-Fi OFF | 43432 | 0 | 14476 |

text 含 CYW43 固件只读镜像，不能视为 RAM 使用量。符号检查确认 OFF 版不包含 CYW43、Wi-Fi 服务和 Core 1 入口；默认关闭 SD/UART。自动化结果为 C 协议/队列测试通过、ADS 全域测试通过、Python 8 项测试通过。尚未进行真实 AP/DHCP/TCP 通信、Core 1 栈高水位和长期并发运行验证。

测试直接调用实际 C 协议组包、SPSC 队列、发送偏移状态机及 Python 解析器：CRC 标准向量/C-Python 交叉验证、满包/部分包/补零、字段偏移、24 位边界、全部 1025 个拆包位置、逐字节接收、多包粘连、数据区嵌入标记、错误候选包重新同步、序号回绕/缺口、配置/代次变化、显式停止、队列满、200000 帧双线程并发、背压重试和断线半包清理。Python 使用受控 socket 替身执行实际接收主循环，验证有限重连、重连丢弃旧半包、CSV 连接编号；它不替代真实 lwIP/RF 测试。原 ADS 帧解析测试覆盖全部 2^24 个符号扩展输入。DHCP 选项解析有越界/截断测试，实际 DHCP 租约收发尚需硬件测试。

## 实机持续测试清单

1. 无客户端运行至少 30 分钟：记录 ADS 帧率/异常和 no_client，确认不积累历史数据；随后连接应收到当前时间戳。
2. 内部测试接收至少 1 小时：确认约 250 SPS、满包间隔约 144 ms，CRC 为 0 错误，检查 8 通道波形；MCLK 测量确认后才将假设位清零。
3. 停止读取客户端 socket（或在调试接收器中暂停 recv）制造慢客户端：观察 ACK 停滞、背压、队列满/丢新帧、约 10 秒无进展断开；注意操作系统 TCP 接收缓存可能延迟背压出现。
4. 反复强制关闭接收进程、断开 Wi-Fi、重连至少 100 次：检查新连接从完整包开始、旧数据不回放、拒绝第二客户端；确认 RAM/服务未逐次耗尽。
5. USB 停采/启采、读寄存器、切换内部测试/短接/正常输入：旧包提前封装，新代次变化，包内无混合配置。切换正常输入前确认外部参考/跳线。
6. USB 每秒日志与 p 预览、TCP 同时运行至少 24 小时：分别记录采集丢帧、网络丢弃、日志丢弃，检查帧率、队列峰值、错误计数及 Core 1 栈余量。正常前提下不能承诺所有高采样率和慢客户端条件下无丢失。

## 本次文件修改范围

- `net/eeg_stream.*`：硬件无关协议编码、CRC、跨核队列、发送偏移状态机。
- `net/wifi_stream.*`、`wifi_config.h`、`lwipopts.h`：Core 1 AP/TCP、采集代次桥接、资源配置和原子统计。
- `net/dhcpserver.*`、`dhcp_options.h`：DHCP 服务及有界选项解析。保留原 MIT 许可，源自 Raspberry Pi pico-examples 的 `pico_w/wifi/access_point/dhcpserver`，本次修改为 raw UDP 定向 AP 接口、边界校验、租约处理且不输出 Core 1 日志。
- `ads1299.c/.h`：Core 0 停止/启动生命周期回调；`spi_dma.c`：逐有效帧提交及网络统计；`CMakeLists.txt`：开关、依赖和源文件；`.gitignore`：Python 缓存忽略。
- `host/eeg_receiver.py`、`tests/test_eeg_stream.c`、`tests/test_eeg_receiver.py`、`tests/run_tests.ps1`：接收、保存、自动化验证。
- 本文、`PROTOCOL_V1.md`、`protocol_v1_example.hex`、主 ADS README：协议、步骤和兼容说明。

外部参考工程未改动；SD/BDF 源码保留，未添加屏幕、HTTP、网络控制采样或板端数据存盘。
