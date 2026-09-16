# USB 显示滤波、软件触发、BDF+ 保存

2026-09-16。仅修改上位机；固件、协议及原始 USB 采集路径不变，参考工程只读。无需烧录。

## 参考依据

参考根目录：`C:/Users/liuzh/Desktop/iSensys-X-Client`。

| 参考文件（src/isensex 下） | 已核实并移植的行为 |
| --- | --- |
| `utils/filter.py`、`graphSetup.py` | 陷波→高通→低通；Q=30 陷波，二阶 Butterworth 高/低通，lfilter 跨块状态及初态；默认 1 Hz 高通、30 Hz 低通、50 Hz 陷波 |
| `triggerSetup.py`、`mainWindow.py:keyPressEvent/process_data` | 快捷键/事件/注释设置，事件送波形和 BDF；改用安全 JSON 配置 |
| `plot/signalMat.py:update_trigger` | 红色竖排文字与竖线，位于绘图区底部，随扫描覆盖；当前按序号定位并支持重绘恢复 |
| `utils/bdfWrapper.py`、`mainWindow.py:process_data` | BDF+，保存滤波前数据，一秒记录，事件 annotation；当前以数字码值保存，独立有界写入队列 |

参考工程的 W/L 外部触发盒、其他传感器、设备端离线 SD 录制需要其硬件/协议支持，未移植。当前是八通道 ADS1299 在线 BDF 保存与软件触发，不能替代硬件同步触发。

## 启动和操作

工程根目录运行：

```powershell
./host/.venv/Scripts/python.exe -m pip install -r host/requirements-usb.txt
./host/.venv/Scripts/python.exe host/usb_gui.py
```

`host/.venv` 和 `build/gui-venv` 均已安装新增 SciPy 1.18.1、pyEDFlib 0.1.42。

1. COM11 连接，待机应用 ADC 参数，再开始采集。
2. 高通：关闭 / 0.1 / 1 / 5 / 20 Hz；低通：关闭 / 30 / 40 / 70 / 100 / 200 / 500 Hz；陷波：关闭 / 50 / 60 Hz。点击“应用显示滤波”生效，三项关闭即原始显示。截止频率必须低于实际采样率的一半，高通必须低于低通。
3. 滤波仅作用于显示，既不修改 ADC 增益也不修改文件。变更时保留旧页，新数据使用新滤波状态，交界断线；停止后的历史不重新滤波。真实缺口、显示丢弃、新代次重置滤波状态；降低采样率时不合法的旧截止频率自动关闭并同步控件。
4. 选择不存在的 `.bdf` 路径，采集中点击“开始 BDF 保存”。“结束 BDF 保存”只关闭文件；“停止”、正常断开或关闭窗口自动收尾。再次保存换新文件名，已有文件拒绝覆盖。GUI 从点击保存之后收到的数据开始记录；验证脚本支持 START 前开启记录。
5. 触发框填写事件，点击“触发”，或按默认 F1（事件 1）。“触发设置”支持添加/更新/删除快捷键、事件、注释并保存。仅当前窗口采集中触发，输入框和模态对话框内不触发，禁用按住重复；F8 保留。配置保存在 `~/.ads1299-usb/triggers.json`。事件+注释合计 ≤40 UTF-8 字节。控制通道忙时报告本次触发未接受。

软件触发锚定到**接收进程处理请求时最新的已接收样本**，保留代次、序号、ADC 时间戳。它有 GUI/进程调度及 USB 积压延迟，不是物理按键时刻的硬件精度。因果滤波相位延迟不补偿到事件。未录制时触发只显示，录制时另写 BDF annotation 与 JSONL；精确样本序号以 JSONL 为准，BDF 时间可能存在库的量化。

默认滤波会改变内部测试方波的形状，高通使平台回落。检查原始方波时关闭全部显示滤波。

## 文件与完整性

- 串口线程仅向 BDF 有界队列发布采样包，不解码或写 BDF；接收进程的专用线程解码并批量写入。队列上限 2048 包，显示队列不参与保存。
- 八通道 CH1…CH8，单位 uV，原始数字码范围 -8388608…8388607。数字码值无损，未应用显示滤波；标称比例 `4500000/(gain*8388608)` 未经电压校准。BDF 物理范围字段限八字符，头部使用整数 µV 端点，有微小量化，精确标称比例另存 JSONL。
- 生成 `name.bdf` 和 `name.bdf.jsonl`。真实序号缺口、重排、采集代次或配置变化自动生成 `name.part0002.bdf` 等段。每段时间从 0 开始，JSONL 记录原因、首序号、代次、ADC 时间戳、采样率、增益、模式；不把缺口两侧伪装成连续采样。
- 不足一秒的尾段保留全部真实样本，剩余补数字零，写 `INVALID_PADDING` 注释；JSONL 的 `valid_samples` 和 `padding_samples` 定义有效范围。分析须排除补齐样本，不能把 BDF 总样本数当实际接收帧数。
- BDF 队列满/磁盘失败会报告错误和丢弃数，停止该录制，USB 接收继续。每秒 BDF 事件最多 50 条，超限报告录制错误，避免静默丢事件。正常关闭等待排空，关闭超时标记可能不完整；崩溃、强制结束或断电不保证文件完整。
- 可选原始 bin 仍保留，包含 ADS 状态字及完整协议消息；BDF 不是完整 USB 流的替代品。
- 波形页最多保留 256 个标记；显示标记被覆盖不删除 BDF 注释。

## 验证

静态核对上述参考源码及原始接收/BDF/显示隔离。自动测试 24 项通过：参考滤波一致性、跨包状态、缺口重置、标记像素/回绕、BDF 数字回读、中文事件位置、尾段、缺口/代次/配置分段、已有文件拒绝、显示队列满不影响 BDF、BDF 背压/磁盘错误报告，以及原有 USB 与波形回归。

实机 COM11、16000 SPS、增益 1、内部测试、5 秒页、3 mV、默认滤波，同时保存 bin/BDF+ 约 20 秒：**320704 帧**，接收缺口/显示丢弃/bin 丢失/BDF 丢弃/解析错误均为 0。全部 BDF 有效样本回读与 bin 逐值一致，触发注释回读成功。STOP 主动收尾丢弃计数增加 2，报告保留该信息。产物：`build/sweep-hardware-20260916-102356.{json,bin,bdf,png}` 及 `.bdf.jsonl`。

实际 GUI 验证按钮、F1、开始 BDF 保存和 STOP 自动关闭：共收到 **1669265 帧**，较晚开始的录制区间保存 **1374533 帧**、两条事件，无接收缺口/显示丢弃/BDF 丢弃。文件：`build/usb-features-ui-20260916-1027.bdf` 及 `.bdf.jsonl`。

这些是短时结果，不代表所有参数组合、长期运行或任意第三方 BDF 阅读器均已验证。

复测（独占 COM11，会配置设备但不烧录）：

```powershell
./host/.venv/Scripts/python.exe -m unittest discover -s tests -p 'test_usb_*.py' -v
./host/.venv/Scripts/python.exe tests/validate_sweep_hardware.py --port COM11 --seconds 60 --features
```

文件：`host/signal_tools.py`、`host/bdf_recording.py`、`host/trigger_settings.py`、`host/usb_receiver.py`、`host/usb_process.py`、`host/usb_gui.py`、`host/sweep_plot.py`，以及 requirements 和对应 tests。

库依据：[官方 EdfWriter 文档](https://pyedflib.readthedocs.io/en/latest/ref/edfwriter.html) 及本机 0.1.42 源码。旧网页记录时长单位与新版不同，当前使用默认一秒记录。
