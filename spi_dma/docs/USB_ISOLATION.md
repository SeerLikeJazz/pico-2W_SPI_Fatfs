# USB 二进制采集与 Core 0 隔离验证

当前波形视图已改为 iSensys 固定时间页扫描、回绕暂留和跨通道显示；最新操作、对照截图与 16000 SPS 实测见 [SWEEP_DISPLAY](SWEEP_DISPLAY.md)。下方保留 USB 隔离及此前显示版本的调试记录。

2026-09-15。默认工程改为 ADS1299 → Core 0 → USB CDC；不启动 Core 1，不链接 Wi-Fi、lwIP、DHCP、SD、USB stdio 或日志控制台。旧 net/、debug_console 和文本命令源码保留，不参与默认固件。上位机使用 host/usb_receiver.py 与 host/usb_gui.py；原 Code Wi-Fi GUI 不用于本固件。

## 构建、运行

### COM11 写入超时修复（2026-09-15）

实机复现：Windows 能枚举 COM11（VID:PID 2E8A:0009），pyserial 能打开端口，但发送 24 字节 QUERY 出现 Write timeout，GUI 同样报错。
根因是原 CFG_TUD_CDC_RX_BUFSIZE=256 小于 CFG_TUD_CDC_EP_BUFSIZE=512。
本地 Pico SDK 2.2.0 的 TinyUSB `cdc_device.c::_prep_out_transaction()` 要求 RX FIFO 剩余空间至少为 EP_BUFSIZE，否则不提交 OUT 接收；原配置即使 FIFO 为空也永远无法接收。
修复将 RX FIFO 增至 1024，保留 512 字节批量传输，并增加编译期约束；只增加 768 字节 RAM，无协议变更。
GUI 连接失败时明确提示端口已释放，可重新连接。
固件编译及 C/Python 回归通过。用户重新下载后，COM11 实机 QUERY/START/STOP 和 GUI 连接成功。实测结果见下节。

### COM11 实机复核及上位机隔离（2026-09-15）

- 16000 SPS，SPI 回报 15000000 Hz，内部测试模式，增益 1。
- 无绘图约 60 秒：966869 帧，序号/包缺口 0，解析异常/丢字节/存储丢失 0。设备采集队列丢帧、DMA 冲突、发送队列丢帧为 0；STOP 清理计数增加 2（包含停止时未完成/未消费样本），不计作运行中缺口。
- 原同进程 GUI 在绘图时复现缺口；仅增加 Windows 接收缓冲仍未消除。记录 `build/com11-gui-validation.bin` 最终 2689592 帧，缺口 720 帧、解析丢弃 17680 字节。这一异常保留，不作为通过结果。
- GUI 改用 `host/usb_process.py`：独立进程负责串口读取、连续性统计和原始记录，命令独立 Pipe，显示快照 Queue 有界且非阻塞；队列满只计显示丢弃。Windows 请求 1 MiB 接收缓冲。绘图点数另按窗口宽度限制；显示抽样不改变原始文件。
- 独立接收进程同时绘图及记录：1596234 帧，序号/包缺口、解析异常、丢字节、尾部残留均为 0。此轮仍有显示队列丢弃，随后进一步限制绘图调用数量；不能混同为原始采样丢失。
- 最终显示快照按 20 Hz 发布（不查询设备），低于 GUI 30 Hz 消费频率，避免 Windows Pipe 分批交付造成持续积压。最终 GUI 实测 823885 帧：序号缺口、显示队列丢弃、绘图断线和解析异常均为 0，STOP 回执成功，保持 COM11 已连接待机。此次未开启原始文件记录；此前 1596234 帧记录用于存储验证。
- 连接/按需查询回执同步实际采样率、增益和模式，避免控件保留上次默认值。
- 对应原始文件及 `*-summary.json` 在 build，另有 `com11-16k-validation.txt`。这些是本机本轮短时观察，未证明所有采样率、长期负载和异常断连下均无缺帧；ADC 边沿是否全部捕获仍需逻辑分析仪证据。

本机已有测试隔离环境 build/gui-venv（Python 3.12、pyserial、PySide6、numpy）。以下命令在 spi_dma 根目录运行：

```powershell
# 正常固件，详细计时关闭；不烧录
./build_usb.ps1
# 诊断固件，独立目录 build/usb-profile，默认产物不被覆盖
./build_usb.ps1 -Profile
# 上位机端口列表
./build/gui-venv/Scripts/python.exe host/usb_receiver.py --list
# 无绘图连续性基准：按实际端口替换 COM7，原始文件必须不存在
./build/gui-venv/Scripts/python.exe host/usb_receiver.py --port COM7 --rate 16000 --seconds 60 --raw build/capture-16k.bin
# USB 波形界面；连接后手动开始，可以先配置采样率/增益/模式
./build/gui-venv/Scripts/python.exe host/usb_gui.py
# 离线检查
./build/gui-venv/Scripts/python.exe host/usb_receiver.py --offline build/capture-16k.bin
# C 驱动/协议和 Python 接收/GUI 回归
./tests/run_usb_tests.ps1
```

在其他电脑使用 Python 3.10+ 创建 venv，pip install -r host/requirements-usb.txt。无绘图接收仅依赖 pyserial，协议离线解析只需 Python 标准库。GUI 与命令行不能同时占用同一串口。串口 baudrate=115200 只是 CDC 线编码，不限制 USB 实际波特率；DTR 必须为真。

正常固件：build/spi_dma.uf2；可选诊断固件：build/usb-profile/spi_dma.uf2。未自动烧录，尚无实机连续性结论。

## Core 0 审查结果

- 同核 GPIO/DMA IRQ 均为 0x40，彼此不抢占；USB 保持 SDK 默认 IRQ 优先级。RX 先启用，TX 后产生时钟；只启用 RX 完成 IRQ。
- RX DMA 完成不等于 SPI 最后一位已结束。finish_frame 仍检查 RX/TX busy 与 SPI BSY；未结束由主循环短临界区收尾，也可在下一次 DRDY 开始前收尾。不会等硬件自旋于 IRQ 中。
- 活跃 DMA 缓冲与采集队列分离。IRQ 校验状态高 nibble、复制一次 raw_frame_t，再经 DMB 发布 head；主循环借用队列槽，直接将 27 字节 raw 复制进 USB 组包槽，然后经 DMB 发布 tail。没有整帧中间复制或通道解码。状态 nibble 检查属于 ADC 格式诊断，不是载荷校验和。
- 新 DRDY 到来但旧 DMA 未结束时，标记旧帧 tainted，并跳过当前读出；分别统计 busy_drdy 和 tainted_frames，旧帧不作为有效数据发布。
- 看门狗每 100 us 检查状态快照，超时计算放在关中断区之外；提交故障前重新核对 DMA 开始时间/DRDY 时间，避免 IRQ 已恢复后误停。
- 上一轮的逐 IRQ 计时、逐帧队列年龄和主循环耗时测量默认全部关闭。按需状态查询仍保留最小计数。
- 上一轮故障恢复会 STOP → RESET → 150 ms 参考稳定等待 → START，新代次可能表现为波形重置。本轮关闭自动恢复，故障保持停采；用户显式 START 才重新初始化重试。
- 没有静态证据证明 Core 0 是上一轮 Wi-Fi 卡顿的根因，也不能证明真实硬件不漏 DRDY。软件序号只能反映已观察到的 IRQ，GPIO 边沿合并/完整漏边沿必须用逻辑分析仪验证。

## USB 实现依据与限制

直接链接当前 SDK 2.2.0 的 tinyusb_device，自定义单 CDC 描述符；不注册任何 stdio 输出驱动，不启用 1200-baud 重启接口。USB 硬件 IRQ 收集事件，Core 0 主循环 tud_task_ext(0,false) 处理事件；该 API 会处理队列中的事件，不是严格的恒定时间调用。没有随意提升 USB 优先级。诊断构建测 usb_task_max/main_max，正常构建不做这些计时。

核对本地 TinyUSB：cdc_device.c 的 write 返回实际 FIFO 接受字节数；write_flush 在端点忙时返回，不等待主机；TX complete 回调继续 flush。配置 4096 字节发送 FIFO、512 字节 endpoint transfer buffer，DCD 按 64 字节 FS 包拆分传输。Pico OSAL 队列存在短临界区，ADC 队列仍需吸收 USB 服务长尾。

CDC 在 DTR=false 时允许覆盖 TX FIFO，因此应用仅在 tud_cdc_connected（DTR=true）时发送；DTR 下降/USB unmount 会锁存断连事件，主循环停止 ADC 并清队列。tud_cdc_write_clear 不能追回已经在 endpoint/主机的字节，重开时接收端按包头重同步；会话以首次查询/START 回执及 generation 为基准，不承诺旧 USB 缓冲从未出现。

## 缓冲与生命周期

- 采集队列 64 帧；16 kSPS 时约 4 ms。满时丢新帧，adc_queue_drops 增加，不覆盖旧槽。
- USB 消息队列 64×1024 字节槽；组包直接使用下一空槽，无整包 memcpy。最多 36 帧（1012 字节），首帧入组包 4 ms 后即触发部分包，发送实际长度，不补满包。
- 采样最多使用 62 个已提交槽，保留控制回执余量；满时继续消费采集队列并增加 tx_sample_drops。连续序号缺口会切包，接收端可识别缺失。生产/消费均在主循环，不需要跨核原子操作。
- 控制请求每次只处理一个；先保证回执空间，再执行命令副作用。主机应保持一个在途命令。每轮取至多 64 帧、解析至多 64 命令字节、向 TinyUSB 交付至多 4096 字节，不等 USB 可写。
- STOP 停止 ADC，未消费的采集队列/在途采集计入 stop_discard；已组包和已排入 USB 的数据保留，部分包先结束，再排 STOP 回执。回执到达表示前面的完整应用消息已交给主机读取顺序，不代表已落盘。
- START 幂等：已运行则不重复启动；真正启动 generation++。停止后重新 START 不截断前次待发消息，新旧代次分别携带元数据。
- 连续 2 秒有发送积压且无法向 TinyUSB 交付新字节，停止 ADC，stall_stops++，stop_reason=3。已有消息继续等待读出，不自动重启。总缓冲有限，2 秒前仍可能溢出，必须查看计数。这个阈值测 FIFO 接受进展，不是对端应用落盘确认。
- 断连停止并丢弃应用未完成传输的样本，disconnect_discard 统计包含部分已接受消息的全部样本（保守计数），不能代表准确的主机丢失量。TinyUSB 已接受字节只计入 tx_bytes，不声称已交付。

## 如何验证与归因

先不绘图、不存盘，依次运行 250/500/1000/2000/4000/8000/16000 SPS，每档至少 60 s；最高档再延长至 10 min。CLI 启动前查询 BASELINE，结束 STOP 回执 DEVICE_FINAL，输出设备计数差分。期间每秒仅打印本机接收统计，不查询固件。随后分别开启 raw 存盘、GUI，比较指标。

| 现象 | 优先检查 |
| --- | --- |
| busy_drdy / tainted_frames / DMA 错误增加 | DRDY 响应、DMA/SPI 完成时序与中断延迟 |
| adc_queue_drops 增加 | Core 0 消费停顿、USB task 长尾、关中断时间 |
| tx_sample_drops 增加 | USB/主机消费能力不足或短时背压 |
| 上位机 sample_gaps 增加 | 与前述设备计数核对；也可能是应用帧损坏或解析重同步 |
| display_dropped_samples 增加、sample_gaps=0 | 仅绘图消费落后；完整接收未丢 |
| storage_dropped_bytes / storage_error 非零 | 存盘跟不上或失败，录制不完整；接收继续 |
| generation 变化 | 明确 START；异常自动重启已禁用 |

上位机接收线程只解析头和统计连续性，不解码通道。存盘线程有独立 8 MiB 上限队列，满时显式计数，不阻塞接收；文件存在时拒绝覆盖。GUI 显示队列 128 包，满时丢旧显示包并独立计数；显示抽样最多约 1000 SPS/通道，缺口插断线，历史保留，只有新采集代次清图。显示抽样不是抗混叠滤波，不能用其判断高频信号幅值。

另做主机暂停读取、拔线、DTR 关闭、反复启停、运行中改参数（应拒绝）、故障后手动 START、命令/数据拆包实验。用逻辑分析仪确认全部档位实际 SPI=15 MHz、DRDY周期以及最后 SCLK 到下一 DRDY 余量。默认 MCLK=2.048 MHz 是配置假设，DVDD=3.3 V 与连线信号质量须实测。15 MHz 时每帧总线时间 14.4 us；16 kSPS 满包应用速率约 449778 B/s，实际 FS USB 吞吐必须实测。

详细协议见 USB_PROTOCOL.md。本次软件测试通过不能替代上述硬件结论。

## 本轮软件验证结果

默认与 ENABLE_ACQ_PROFILE=ON 两种 Release 固件均构建通过。生产 ADC 驱动测试覆盖 DMA 先后顺序、SPI 尾部、重复发布、污染帧丢弃、队满、环回、超时快照竞态与硬件错误；生产命令测试覆盖查询、幂等启停、运行中拒绝改参、显式故障重试和重连事件。USB 组包测试覆盖部分写入、零进展、队列满、回执预留、STOP 顺序与断连清理。上位机 8 项协议/接收/存盘测试和 2 项 GUI 测试通过。

默认 ELF 符号检查未发现 cyw43、TCP、Wi-Fi、Core 1 启动、USB stdio/debug_log 或通道解码函数。默认固件 ELF text=33844 字节、BSS=76364 字节（含 USB 消息队列和 TinyUSB FIFO）；这只是构建占用，不是实机 CPU 或吞吐测量。
