# AUSB v1：USB CDC 二进制协议，无应用层校验

不是旧 EEG1/WFC2 协议。所有整数均为小端。USB 是连续字节流，read 与消息边界无关；数据、回执、按需状态使用不同 type。无日志、无启动公告、无周期状态推送。

## 公共头（16 字节）

| 偏移 | 长度 | 定义 |
| --- | --- | --- |
| 0 | 4 | ASCII AUSB |
| 4 | 1 | version=1 |
| 5 | 1 | type：1 数据，2 回执，3 状态，16 命令 |
| 6 | 2 | 消息总长度，含头 |
| 8 | 4 | 数据：当前代次包序号；回执/状态：请求 ID；命令：主机请求 ID |
| 12 | 4 | 数据/回执/状态：采集 generation；命令必须为 0 |

消息中没有 CRC、校验和或包尾。解析先搜索 magic，再验证 version/type/length 与数据元信息，等足 length。错误候选只跳一个字节再搜；不以 raw 中的 magic 强行截包。载荷位错误无法由应用协议检测，长度/头损坏后的重同步也不具备校验级别保证。USB 硬件自身 CRC 保留。

## 命令（type=16，总长 24）

16..19 为 op(uint32)，20..23 为 value(uint32)。一个请求对应一个二进制回执；主机保持一个在途请求，ID 自然回绕。QUERY/START/STOP 的 value 必须为 0。

| op | 行为 |
| --- | --- |
| 0 QUERY | 一次按需状态 type=3，不改变采集 |
| 1 START | 启动；已运行时幂等；配置故障时显式初始化重试 |
| 2 STOP | 停采并结束待发部分包；回执在先前数据之后 |
| 3 RATE | value=250/500/1000/2000/4000/8000/16000，只允许待机 |
| 4 GAIN | value=1/2/4/6/8/12/24，只允许待机 |
| 5 MODE | value=0测试、1短接、2正常、3阻抗激励，只允许待机 |

运行中修改参数返回 result=2，不停采，不修改配置。非法 op/value 返回失败。回执不是 ACK 成功的同义词：必须检查 result 与 configured/running。命令没有周期参数检查，也没有自动心跳。断连通过 DTR/unmount 处理，停止读取通过发送无进展超时处理。

## 数据（type=1，总长 40+27×count，67～1012 字节）

| 偏移 | 长度 | 定义 |
| --- | --- | --- |
| 16 | 4 | 首样本软件 DRDY 序号 |
| 20 | 8 | 首样本 GPIO ISR 时间戳，微秒 |
| 28 | 4 | 配置 MCLK Hz（默认 2048000，未经实测） |
| 32 | 2 | 标称采样率 SPS |
| 34 | 1 | 增益 |
| 35 | 1 | 模式 |
| 36 | 2 | count=1..36 |
| 38 | 2 | reserved=0 |
| 40 | 27×count | 原始 ADS 帧：3 字节状态+8×3 字节通道，大端24位二补码 |

同包必须连续序号、同代次、同配置，序号模 2^32 回绕。首次真实 START 的 generation=1，每次真实启动加1，设备重启会归零，不能跨重启当全球唯一 ID。包序号在 generation 内从0递增。软件未观察到的 DRDY 不会反映在序号中。

只传首帧实测 IRQ 时间。包内第 i 帧估算时间为 first_timestamp+i×1e6/(rate×mclk/2048000)。时间戳不是转换器内部精确采样时刻，不是 UTC。固件不解码通道。

## 回执/状态（type=2/3，总长176）

payload 为下列40个 uint32。QUERY 返回 type=3，其他命令返回 type=2。result：0 成功，1 非法命令，2 当前状态不允许，3 ADC 操作失败。error 为 ads1299_error_t：0 OK、1 参数、2 状态、3 SPI超时、4 ID、5 寄存器读回、6 DMA资源、7 SPI预算/非精确15MHz、8 DRDY超时、9 DMA超时、10 DMA/SPI硬件错误、11 帧格式、12 DMA终止失败。

error 是驱动保留的最近错误，不保证成功 QUERY 后清零；result 才表示该命令结果。stop_reason：0 无/运行，1 显式STOP，2 断连，3 发送停滞，4 ADC故障。最低成本计数跨启停保留，重启清零，uint32 自然回绕。tx_bytes 由两个字拼成 uint64，表示被 TinyUSB FIFO 接受，不是主机解析/落盘字节。

| payload 字号 | 消息偏移 | 字段 |
| --- | --- | --- |
| 0 | 16 | op |
| 1 | 20 | result |
| 2 | 24 | error |
| 3 | 28 | configured |
| 4 | 32 | running |
| 5 | 36 | rate |
| 6 | 40 | gain |
| 7 | 44 | mode |
| 8 | 48 | mclk |
| 9 | 52 | spi |
| 10 | 56 | drdy |
| 11 | 60 | frames |
| 12 | 64 | busy_drdy |
| 13 | 68 | adc_queue_drops |
| 14 | 72 | bad_frames |
| 15 | 76 | drdy_timeouts |
| 16 | 80 | dma_timeouts |
| 17 | 84 | dma_errors |
| 18 | 88 | recoveries |
| 19 | 92 | stop_discard |
| 20 | 96 | adc_queue_peak |
| 21 | 100 | tx_sample_drops |
| 22 | 104 | tx_queue_peak |
| 23 | 108 | tx_bytes_low |
| 24 | 112 | tx_bytes_high |
| 25 | 116 | disconnects |
| 26 | 120 | stall_stops |
| 27 | 124 | stop_reason |
| 28 | 128 | profile_enabled |
| 29 | 132 | drdy_irq_max |
| 30 | 136 | dma_irq_max |
| 31 | 140 | poll_gap_max |
| 32 | 144 | adc_age_max |
| 33 | 148 | tainted_frames |
| 34 | 152 | usb_task_max |
| 35 | 156 | main_max |
| 36 | 160 | tx_queued |
| 37 | 164 | disconnect_discard |
| 38 | 168 | malformed_commands |
| 39 | 172 | fatal |

profile_enabled=0 时详细耗时字段为0；正常固件不会执行逐帧/逐IRQ计时。诊断构建耗时是微秒墙钟区间，不是独占 CPU 周期，可能包含抢占、量化与仪器自身开销。

frames 为合法且未 taint 的完成帧数，包含队列满后被丢的帧；adc_queue_drops 是其中未能进入采集队列的数量。busy_drdy 是旧传输未结束时观察到的新DRDY数，tainted_frames 是因这种冲突最终丢弃的旧传输数，两者不应直接相加推导硬件样本总损失。stop_discard 包含停止时未消费及在途帧。tx_sample_drops 独立表示采集已取出但发送队列放不下的样本。disconnect_discard 是未完成应用消息的保守样本计数，不包含所有 TinyUSB 内部已接受字节的命运。
