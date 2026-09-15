> 2026-09-14：关闭 5001 每 2 秒周期参数查询/回传，保留首次同步、手动查询及操作回执。数据协议 version=2，无 CRC，需要同步更新固件与接收端。固件 SPI 固定 15 MHz，关闭固件预览/解码，上位机正常显示。详见 [优化说明](../spi_dma/docs/PERFORMANCE.md)。

# Pico 2 W 八通道 Wi-Fi 上位机

## 使用步骤

1. 烧录本次 `Code/build-firmware/spi_dma.uf2`。新上位机需要 **WFC2 固件**，必须与固件一起升级。
2. 电脑连接 `Pico2W_EEG` 热点，双击 `Code/start.bat`。默认 IP `192.168.4.1`，数据端口 `5000`，控制端口 `5001`。
3. 点击“连接”，等待查询完成，状态变为“待机”。上电和连接均不启动 ADC，没有自动波形。
4. 待机时选择采样率、统一增益和模式，分别点击“应用”。下拉框是请求值，下方回执是设备确认值，两者不会混淆。
5. 点击“开始显示”：发送 START，设备确认后才显示对应采集代次的数据。点击“停止显示”：发送 STOP，确认停采后保持现有波形。
6. 采集中禁止修改参数；停止后再修改。主动断开和关闭窗口会先发送 STOP，再关闭数据连接。意外断线会冻结画面并标记故障；重新连接仍待机。

Python 3.11+，依赖 PySide6 和 NumPy。`start.bat` 首次创建 `.venv` 并安装依赖。当前已验证 Python 3.14.2、PySide6 6.11.2、NumPy 2.5.3。

```powershell
# 在 Code 目录手动启动
python -m venv .venv
.venv/Scripts/python.exe -m pip install -r requirements.txt
.venv/Scripts/python.exe main.py
```

当前固件默认配置仍为内部测试、250 SPS、增益 1，但处于停止状态。设置不写 Flash，重启恢复编译默认值。热点默认无密码，可修改 `spi_dma/net/wifi_config.h` 后重新构建。数据端口只允许一个客户端。

## 四种模式及硬件边界

| 界面模式 | 协议 mode | 实际寄存器/通路 |
| --- | --- | --- |
| 正常采集模式 | 2 | 正常差分输入；使用 ADS_NORMAL_SRB1/SRB2/BIAS 板级配置，关闭内部测试和 LOFF 激励 |
| 阻抗采集模式 | 3 | 正常差分 MUX；LOFF=0x02，LOFF_SENSP=0xFF，LOFF_SENSN=0xFF，LOFF_FLIP=0；独立 INxP/INxN 激励，SRB1/SRB2/BIAS 路由关闭 |
| 内部短路模式 | 1 | MUX=1，内部短路输入；内部测试、LOFF、SRB/BIAS 路由关闭 |
| 内部测试模式 | 0 | MUX=5，CONFIG2 内部测试源开启；LOFF、SRB/BIAS 路由关闭 |

所有模式保持请求的统一增益，不暗中修改增益或采样率。每次配置重建完整寄存器镜像并读回校验，离开阻抗模式会关闭 LOFF 激励，不遗留上个模式的设置。CONFIG4=0，保持连续转换配置、关闭 DC 脱落比较器。

阻抗模式真实配置 ADS1299 交流导联检测：标称 6 nA，频率 fCLK/65536；2.048 MHz 时为 31.25 Hz。依据 [TI ADS1299 数据手册 SBAS499C，9.3.2.4.3.2 和 9.6.1.5](https://www.ti.com/lit/ds/symlink/ads1299.pdf)。此模式显示的是激励下的八通道原始 ADC 码，未计算电极阻抗，不标注 Ω/kΩ。停止转换不等于物理断开输入，模式配置决定激励寄存器的状态。

**尚缺硬件信息：** 当前项目目录未找到可复核原理图；现有文档未确认 H2/H5 跳线、INxN 是否接公共参考、实际回流及共模偏置路径、输入保护电阻/电容和主时钟实测值。当前阻抗配置针对独立差分端口，不能当作已经验证的公共参考电极方案。须用已知阻值负载核查响应、激励电流及通道耦合，确认并校准整个模拟输入网络后，才能增加阻抗数值换算。若实际硬件需要公共参考/SRB，应先据真实接线调整方案。

## 状态与故障处理

界面区分未连接、连接中、待机、启动中、采集中、停止中、故障。每次只有一个控制事务，防止重复点击。固件端也拒绝采集中修改采样率、增益、模式，不能通过绕过界面强行修改。

START/STOP 不再是画面冻结开关。配置成功保持待机；失败时展示 FAILED 和错误，不能将无效寄存器配置报告为生效。START 回执返回采集代次，GUI 只绘制匹配的包，丢弃旧缓存和在确认前到达的数据。停止后保留画面，重新 START 时清空旧波形。

发生控制超时，结果可能未知，软件不把请求值当成成功结果，会查询实际状态；查询也失败则保持故障，可手动点击“查询设备”。后台每两秒查询设备状态；正常轮询不覆盖操作回执、不禁用按钮或打断下拉选择，设备状态文字仅在变化时更新。轮询期间点击的操作会在当前查询结束后串行执行一次；若查询失败或设备不再符合操作条件，则取消待执行操作并明确提示。发现停止或故障及时同步；若发现未经本界面请求的运行状态，会请求 STOP，避免连接后自动显示。

数据连接每秒发送心跳字节 K；固件五秒未收到客户端数据即关闭会话，并通知 Core 0 停采。FIN、TCP 错误或 AP 关闭也触发停采，不等待控制连接。物理断网不能瞬时发现，最坏停采延迟约为心跳期限加主循环/停止事务处理时间。主动 STOP 若回执丢失，断开数据连接仍会触发固件停采，界面不假称已收到停采成功回执。

## 数据协议与绘图

保留 `EEG1` 固定 1024 字节结构：44 字节头 + 36×27 字节采样区 + 4 字节保留零 + 4 字节包尾（version=2，无 CRC），空余样本补零。新增 mode=3 表示阻抗激励采集。详细偏移见 `spi_dma/docs/PROTOCOL_V1.md`。

每帧：3 字节状态字，再按 CH1～CH8 排列的 24 位大端二补码。状态高四位应为 0xC。纵轴始终为原始码，范围 -8388608～8388607，不换算微伏。支持全部拆包位置、粘包、CRC/头/尾/填充验证和错误重同步。

标称采样率：250、500、1000、2000、4000、8000、16000 SPS；统一增益：1、2、4、6、8、12、24。高采样率仍受 SPI 预算限制，设备可返回 SPI_BUDGET。

推导速率 = 标称 SPS × MCLK / 2048000。首帧时间来自 MCU 中断，其余时间按配置推导，非 UTC 或实测帧率；界面提示未经实测的时钟。横轴为相对最近样本时间。序号缺口、乱序、代次/配置变化会重置波形，防止跨缺口连线。重连后建立新连续性基线，不能估计断网期间全部损失。

接收和解析在后台线程，最多 256 包的显示队列；队满丢弃最旧显示包并统计。Qt 每 33 ms 刷新，320000 帧固定 NumPy 环形缓冲约 23 MB。时间窗口 0.5～10 秒，每通道纵轴范围可调；高采样率采用最小值/最大值包络保留尖峰。

## WFC2 控制协议

TCP 5001，每连接一个请求和回执。整数全部为小端 uint32。数据端口 5000 不解释采样命令，仅接收心跳。控制来源 IP 必须与当前数据连接 IP 相同。

请求固定 **20 字节**，Python `<4s4I`：

| 偏移 | 字段 |
| --- | --- |
| 0 | magic：WFC2 |
| 4 | request_id |
| 8 | operation：0 查询、1 标称采样率、2 统一增益、3 模式、4 START、5 STOP |
| 12 | value：配置值；查询/START/STOP 必须为 0 |
| 16 | data_session：查询可传 0；所有修改命令必须回传最新查询中的会话编号 |

回执固定 **48 字节**，Python `<4s11I`：

| 偏移 | 字段 |
| --- | --- |
| 0 / 4 | WFR2 / request_id 回显 |
| 8 | result：0 FAILED、1 UNCHANGED（查询也使用）、2 APPLIED |
| 12 | error：ads1299_error_t |
| 16 / 20 | 标称 SPS / 统一增益 |
| 24 / 28 | running / configured，各为 0 或 1；fatal 时 configured=0 |
| 32 / 36 | MCLK Hz / mode |
| 40 | stream_id：当前或最近采集代次，仅实际启动后产生新代次 |
| 44 | data_session：奇数表示连接；每次数据连接生命周期变化更新 |

错误编号 0～12：OK、ARGUMENT、STATE、SPI_TIMEOUT、ID、READBACK、DMA_RESOURCE、SPI_BUDGET、DRDY_TIMEOUT、DMA_TIMEOUT、DMA_HW、FRAME、ABORT。configured=0 时参数不能视为有效；查询返回固件状态快照，不额外停采读寄存器。

Core 1 累积完整请求并通过 release/acquire 原子邮箱交给 Core 0；所有 ADS 调用仍在 Core 0。会话改变时优先停采；START/配置执行前验证会话，执行后再次检查，覆盖慢事务期间断线的情况。旧会话命令被拒绝，同会话并发事务被拒绝。服务端控制期限五秒，客户端四秒。控制连接断线不撤销已经提交的事务，request_id 不是跨连接幂等键，应查询后判断状态。

兼容性：WFC1/WFR1 不支持新的启停与会话语义，本版明确拒绝，不自动降级为“冻结显示”。旧固件返回旧回执或关闭连接时，上位机提示升级，不启动显示。旧 EEG1 解码器通常拒绝 mode=3，需要同步更新。项目原命令行 `host/eeg_receiver.py` 已支持 mode=3 和心跳，但不自动 START，连接后需手动控制启动。

## 文件与构建

新增及主要修改代码均在 Code：

- `main.py`：模式选择、异步启停、状态及窗口关闭流程。
- `network.py`：WFC2/WFR2 校验、会话匹配和数据心跳。
- `protocol.py`、`waveform.py`：四模式数据解析和有界八通道绘图。
- `firmware/wifi_control.c/.h`、`control_wire.h`：跨核控制与会话保护。
- `firmware/ads_mode_registers.h`：生产驱动与原生测试共用的模式寄存器镜像生成逻辑。
- `simulator.py`、`tests/`：待机/启停模拟器、Python 和原生 C 测试。

现有固件修改：`ads1299.h/.c` 新增模式；`ads1299_control.h/.c` 支持模式事务；`spi_dma.c` 取消上电及 USB i/t/h/n 的自动 START；`net/wifi_stream.c/.h` 管理数据会话、心跳超时和采集代次；构建文件接入 Code 下固件源码。参考工程保持不变。USB g 仍是明确启动命令，i 仅重新初始化后待机，t/h/n 仅切换配置后待机。

```powershell
# 项目根目录，使用本机 Pico SDK 2.2.0 / ARM GCC 14.2 / CMake / Ninja
powershell -ExecutionPolicy Bypass -File Code/build_firmware.ps1
powershell -ExecutionPolicy Bypass -File Code/build_firmware.ps1 -NoWifi
```

Wi-Fi 产物：`Code/build-firmware/spi_dma.uf2`。非 Wi-Fi 构建也上电待机，USB g 手动启动。Code/firmware 是驱动的必要依赖，构建时必须保留。Wi-Fi 开启及关闭两种构建均通过应用代码 -Wall -Wextra -Werror 检查。本次仅构建，没有烧录设备。

## 验证

```powershell
Code/.venv/Scripts/python.exe -m unittest discover -s Code/tests -v
# 可选开发依赖，仅运行原生 C 测试需要
Code/.venv/Scripts/python.exe -m pip install ziglang==0.16.0
powershell -ExecutionPolicy Bypass -File Code/tests/run_native.ps1
# 本地模拟；界面 IP 改为 127.0.0.1，连接后仍需点击开始显示
Code/.venv/Scripts/python.exe Code/simulator.py
```

模拟器支持 `--rate 16000`、`--corrupt-every 5`、`--disconnect-after 10`，仅监听本机。所有模拟波形不代表硬件信号。

已验证：20 项 Python 测试（含实际 Qt 事件循环）、生产 C 模式寄存器与控制邮箱/回调测试、原 USB 控制回归测试；覆盖四种模式、连接后零采样、手动启停、冻结保留、禁止运行中修改、旧会话拒绝、断线/心跳失联停采、START 回执丢失后查询、断线后迟到回执隔离、失败配置、旧协议拒绝、全部 1025 个拆包位置、包头/包尾/填充重同步、24 位极值和缓存上限。界面截图 `ui-simulation.png` 明确标为本地模拟。

**实机待验证：** 各模式寄存器读回和真实信号、阻抗模式的独立差分接线及已知电阻响应、主时钟、无线心跳断线停采时延、最高采样率吞吐和长期丢包情况。原生测试模拟了 lwIP/ADC 硬件边界，不能证明真实电气行为。
