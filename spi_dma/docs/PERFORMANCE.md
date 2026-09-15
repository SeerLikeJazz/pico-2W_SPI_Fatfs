# 采集与传输优化（2026-09-14）

## 已实施

1. 轻量计时：DRDY/RX DMA IRQ、Core 0 采集服务、辅助服务、Core 1 网络轮询及组包发送；记录队列年龄、服务间隔、关键区和 ACK 延迟上限。计时只累积，不在 IRQ 中打印。
2. 删除固件和接收端 CRC。EEG1 magic 保留，version=2，1016～1019 为保留零。新旧协议互相拒绝，GUI、模拟器、命令行接收端同步升级。
3. 固件 get_frame 仅返回 raw、序号、时间戳，不解码通道，不输出预览；USB p 只提示功能关闭。上位机仍可解码和显示。
4. RX 完成 IRQ 是唯一 DMA 完成 IRQ；TX DMA 仍负责生成时钟，错误与忙状态仍检查。SPI 尾部由短轮询收尾。
5. 单次网络服务最多消费 64 样本、调用 4 次 tcp_write，并设 200 us 循环预算；新包立即尝试发送。单次 SDK 调用可能超过预算。仅新写入或输出失败待重试时调用 tcp_output。
6. Core 1 有进展立即继续，空闲/背压以 WFE 等待。使用 Core 1 独立硬件 alarm，250 us 后最迟重新服务；生产者 SEV 提前唤醒，不使用 Core 0 默认 alarm pool。需要一个空闲硬件 alarm。
7. Core 0 取帧采用同核 SPSC 发布屏障，复制时不关中断；看门狗完整检查限制为每 100 us 一次，超时计算在状态快照临界区之外并在故障提交前重验。USB/控制辅助服务限制为 1 kHz。USB 未连接且 UART 关闭时不做日志格式化。
8. 组包双缓冲交换指针；跨核消费借用连续槽并批量发布 tail，遇 STOP 游标或环回截断；生产端直接填入预留槽，消除 source.raw 中转。队列满仍丢弃新样本，不覆盖未消费槽。eeg_stream_t 初始化后不可按值复制（内部自引用指针）。
9. TCP 缓冲保留 8×MSS=11680 字节；新增 EEG_TCP_SND_MSS 构建配置入口。尚无实机 ACK/RTT 和积压数据，未把估计当实测、未盲目扩容。

所有采样率 ADS_SPI_HZ=ADS_SPI_MAX_HZ=15000000。150 MHz clk_peri 分频 10 得到精确 15 MHz；实际分频结果不等于 15 MHz 则配置失败。硬件要求 DVDD=3.3 V，并验证 SCLK/DOUT 信号时序。MCLK 与采样率保持原设置。

5001 不再由 GUI 每 2 秒查询。保留连接首次同步、失败操作后的一次状态同步、手动查询、参数设置与 START/STOP 回执；5000 心跳保留。固件 wifi_control_poll 是邮箱和连接服务，不是周期参数查询，不能删除。

## 指标解释

每秒 USB 状态输出 perf 行（USB 未连接则跳过格式化）：

- IRQ avg/max：回调函数墙钟耗时，不包括 IRQ 分发入口/退出；IRQ 每次测量。
- acquisition avg/max：Core 0 每 64 轮抽样一次，包含抢占，max 只是抽样最大值。
- auxiliary avg/max：1 kHz USB/控制服务墙钟时间。日志报告本身不在该计时范围内。
- critical_max：常规快照/收尾临界区包围计时，可能包含恢复中断后的抢占，作为上界观察值；不是所有 SDK 临界区的全局最大值。
- pollgap_max：运行期间两次 ADS poll 入口间隔；每次 START 重置间隔基线，最大值跨运行保留。
- Core 0 qage_max：DRDY 时间戳到主循环取帧。
- Core 1 qage_max：DRDY 时间戳到进入组包，包含前级排队，不能与前者直接相加。
- ack_delay_max：一个 TCP 接受字节位置到 ACK 越过它的时间，包含 lwIP 排队、重传和对端延迟，不是纯无线 RTT。
- inflight_max：tcp_write 已接受但尚未 ACK 的最大字节量。

计数/累计 us 为 uint32，按模 2^32 回绕；长时间实验应分段记录或重启后测量。微秒计时会产生量化和自身开销，不能据此声称精确 CPU 利用率。状态报告、单次 SDK 内部阻塞仍可能产生长尾，需要实机观察。

## 实机验收与第 9 项调参

依次测试 250/500/1000/2000/4000/8000/16000 SPS，记录满速、USB 接入/拔出、弱信号、停止/重启、断线/重连和长期传输。比较 DRDY/frames/busy/qdrop、网络 sample_drop、序号缺口、perf 最大值和队列年龄。逻辑分析仪确认所有档 SCLK=15 MHz、DRDY 到 SCLK 的延迟和最后一位到下一次 DRDY 的余量。无 CRC 后不能靠包校验判定 SPI 载荷正确，应结合测试源/短接数据验证。

16 kSPS 协议速率约 455111 B/s，512 帧网络队列约 32 ms，64 帧采集队列约 4 ms。发送缓冲需求可先按协议速率×实测 ACK 延迟估算，再留抖动余量；同时观察 TCP 段数和 MEM_SIZE，不能只增大 TCP_SND_BUF。若发送长期慢于采集，扩容只能延后溢出。第 9 项最终数值需要实机数据后确定。

构建：cmake --build build；产物 build/spi_dma.uf2。主机回归：tests/run_tests.ps1；GUI/模拟器测试：python -m unittest discover -s ../Code/tests -v。本次没有自动烧录。

## 本次验证结果

- Wi-Fi ON/OFF 两种 Release 固件均构建通过，维护的应用源码启用 -Wall -Wextra -Werror。
- 4 个原生 C 测试程序通过：USB/配置、采样格式、组包/跨核队列、控制邮箱/会话。包含 20 万样本并发批量队列、STOP/环回、背压、部分发送以及双缓冲不覆盖检查。
- 29 项 Python 测试通过：命令行协议 9 项，GUI/模拟器/网络 20 项。GUI 验证待机和运行时均不发周期查询，手动查询和启停仍正常。
- 最终 ELF 符号检查未发现 ads1299_decode、ads1299_signed24、eeg_crc32；固件采集路径不再包含解码/应用 CRC。
- 无实机计时、无线 RTT/吞吐或 15 MHz 电气测量结果；未烧录。第 9 项缓冲最终调参仍需实机日志。
