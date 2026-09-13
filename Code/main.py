"""Pico 2 W Wi-Fi eight-channel acquisition dashboard."""
import queue
import sys
import threading
import time

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (QApplication, QComboBox, QDoubleSpinBox, QGridLayout,
                               QHBoxLayout, QLabel, QLineEdit, QMainWindow,
                               QPushButton, QSpinBox, QVBoxLayout, QWidget)
from network import Receiver, control, endpoint, MODES
from protocol import RATES, GAINS
from waveform import Waveform


class Window(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('Pico 2 W · 八通道 Wi-Fi 采集')
        self.resize(1280, 860)
        self.receiver = None
        self.displaying = False
        self.state = '未连接'
        self.device_session = 0
        self.expected_stream = None
        self.run_requested = False
        self.synced = False
        self.query_needed = False
        self.force_stop = False
        self.disconnect_requested = False
        self.closing = False
        self.next_query = 0
        self.results = queue.Queue(maxsize=1)
        self.control_thread = None
        self.background_request = False
        self.pending_action = None
        self.request_id = 0
        self.session = 0
        self.latest = None
        self.latest_at = 0
        root = QWidget()
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)
        title = QLabel('PICO 2 W   /   EEG ACQUISITION')
        title.setStyleSheet('font-size: 23px; font-weight: 600; color: #55dfc5; padding: 10px 0;')
        layout.addWidget(title)
        row = QHBoxLayout()
        self.host = QLineEdit('192.168.4.1')
        self.host.setMaximumWidth(170)
        self.data_port, self.control_port = QSpinBox(), QSpinBox()
        for widget, value in ((self.data_port, 5000), (self.control_port, 5001)):
            widget.setRange(1, 65535); widget.setValue(value)
        self.connect_button = QPushButton('连接')
        self.connect_button.clicked.connect(self.connect_device)
        self.disconnect_button = QPushButton('断开')
        self.disconnect_button.clicked.connect(self.disconnect_device)
        for name, widget in (('设备 IP', self.host), ('数据端口', self.data_port), ('控制端口', self.control_port)):
            row.addWidget(QLabel(name)); row.addWidget(widget)
        row.addWidget(self.connect_button); row.addWidget(self.disconnect_button); row.addStretch()
        layout.addLayout(row)
        self.connection_label = QLabel('未连接 · 请先将电脑连接到 Pico2W_EEG 热点')
        layout.addWidget(self.connection_label)
        grid = QGridLayout()
        self.rate, self.gain = QComboBox(), QComboBox()
        self.rate.addItems([str(v) for v in sorted(RATES)])
        self.gain.addItems([str(v) for v in sorted(GAINS)])
        self.rate_button, self.gain_button, self.query_button = (QPushButton(s) for s in ('应用采样率', '应用统一增益', '查询设备'))
        self.rate_button.clicked.connect(lambda: self.send_control(1, int(self.rate.currentText())))
        self.gain_button.clicked.connect(lambda: self.send_control(2, int(self.gain.currentText())))
        self.query_button.clicked.connect(lambda: self.send_control(0, 0))
        grid.addWidget(QLabel('请求采样率 / 标称 SPS'), 0, 0); grid.addWidget(self.rate, 0, 1); grid.addWidget(self.rate_button, 0, 2)
        grid.addWidget(QLabel('请求增益 / 所有 8 通道'), 0, 3); grid.addWidget(self.gain, 0, 4); grid.addWidget(self.gain_button, 0, 5)
        grid.addWidget(self.query_button, 0, 6)
        self.mode = QComboBox()
        for value, name in MODES.items(): self.mode.addItem(name, value)
        self.mode_button = QPushButton('应用模式')
        self.mode_button.clicked.connect(lambda: self.send_control(3, self.mode.currentData()))
        grid.addWidget(QLabel('请求模式'), 1, 0); grid.addWidget(self.mode, 1, 1, 1, 4)
        grid.addWidget(self.mode_button, 1, 5)
        layout.addLayout(grid)
        self.control_label = QLabel('控制回执：尚未查询；旧版固件须升级后才能通过 Wi-Fi 设置参数')
        self.control_label.setWordWrap(True)
        self.actual_label = QLabel('数据包实际参数：未知')
        self.actual_label.setWordWrap(True)
        layout.addWidget(self.control_label); layout.addWidget(self.actual_label)
        self.device_label = QLabel('设备状态：尚未同步')
        self.device_label.setWordWrap(True)
        layout.addWidget(self.device_label)
        controls = QHBoxLayout()
        self.display_button = QPushButton('开始显示')
        self.display_button.clicked.connect(self.toggle_display)
        controls.addWidget(self.display_button)
        clear = QPushButton('清空波形'); controls.addWidget(clear)
        self.seconds = QDoubleSpinBox(); self.seconds.setRange(.5, 10); self.seconds.setValue(5); self.seconds.setSuffix(' s')
        self.amplitude = QDoubleSpinBox(); self.amplitude.setDecimals(0); self.amplitude.setRange(1, 8388608)
        self.amplitude.setValue(1000000); self.amplitude.setSingleStep(10000)
        controls.addWidget(QLabel('时间窗口')); controls.addWidget(self.seconds)
        controls.addWidget(QLabel('每通道纵轴 ±原始码')); controls.addWidget(self.amplitude)
        controls.addStretch(); controls.addWidget(QLabel('CH01 → CH08 · 未换算为 μV'))
        layout.addLayout(controls)
        self.plot = Waveform(); layout.addWidget(self.plot, 1)
        clear.clicked.connect(self.plot.clear)
        self.seconds.valueChanged.connect(self.scale_changed); self.amplitude.valueChanged.connect(self.scale_changed)
        self.stats_label = QLabel('接收 0 帧')
        self.stats_label.setWordWrap(True); layout.addWidget(self.stats_label)
        footer = QLabel('开始/停止显示同步控制设备采集。阻抗模式使用交流激励，仅显示原始码；待机时可修改参数。')
        footer.setWordWrap(True)
        footer.setStyleSheet('color: #89a9bb;'); layout.addWidget(footer)
        self.setStyleSheet('''QMainWindow, QWidget {background:#0c2434; color:#d8e8ee; font:10pt "Microsoft YaHei";}
            QLineEdit,QSpinBox,QDoubleSpinBox,QComboBox {background:#071b29; padding:5px; border:1px solid #315568; border-radius:4px;}
            QPushButton {background:#184557; padding:7px 12px; border:1px solid #367086; border-radius:4px;}
            QPushButton:hover {background:#24596d;} QPushButton:disabled {color:#56717e; border-color:#29414e;}
            QLabel {background:transparent;}''')
        self.timer = QTimer(self); self.timer.setInterval(33); self.timer.timeout.connect(self.refresh); self.timer.start()

    def scale_changed(self):
        self.plot.seconds, self.plot.amplitude = self.seconds.value(), self.amplitude.value()
        self.plot.update()

    def connect_device(self):
        if self.receiver and self.receiver.is_alive():
            return
        try:
            self.receiver = Receiver(self.host.text(), self.data_port.value())
        except ValueError as exc:
            self.connection_label.setText(f'地址错误：{exc}'); return
        self.session += 1
        self.pending_action = None
        self.device_label.setText('设备状态：尚未同步')
        self.device_session = 0; self.synced = False; self.query_needed = True
        self.disconnect_requested = False; self.run_requested = False; self.force_stop = False
        self.expected_stream = None
        self.set_state('连接中')
        self.latest = None; self.plot.clear()
        self.actual_label.setText('数据包实际参数：未知')
        self.control_label.setText('控制回执：尚未查询本次连接')
        self.receiver.start()

    def disconnect_device(self):
        self.pending_action = None
        self.disconnect_requested = True
        self.run_requested = False
        if self.receiver and not self.receiver.connected:
            self.receiver.stop()
        self.set_state('停止中')

    def toggle_display(self):
        if self.state == '待机': self.send_control(4, 0)
        elif self.state == '采集中': self.send_control(5, 0)

    def set_state(self, state):
        self.state = state
        self.displaying = state == '采集中'
        self.display_button.setText('停止显示' if state == '采集中' else '开始显示')
        if self.receiver:
            if self.displaying: self.receiver.expecting_data.set()
            else: self.receiver.expecting_data.clear()

    def busy(self):
        return bool((self.control_thread and self.control_thread.is_alive()) or not self.results.empty())

    def send_control(self, op, value, *, background=False):
        if not self.receiver or not self.receiver.connected:
            return
        if op in (1, 2, 3, 4) and self.state != '待机': return
        if self.busy():
            # Firmware permits one transaction. Keep the user's click instead
            # of disabling controls or silently discarding it during polling.
            if self.background_request and not background and self.pending_action is None:
                self.pending_action = (op, value)
                self.control_label.setText('操作已接收，等待当前状态查询结束…')
            return
        self.background_request = background
        if op == 4:
            self.run_requested = True; self.set_state('启动中')
        elif op == 5:
            self.run_requested = False; self.set_state('停止中')
        try:
            host, port = endpoint(self.host.text(), self.control_port.value())
        except ValueError as exc:
            self.control_label.setText(str(exc)); return
        self.request_id = (self.request_id + 1) & 0xffffffff
        request_id, session, device_session = self.request_id, self.session, self.device_session
        if not background:
            self.control_label.setText(f'请求 #{request_id}：{("查询", "采样率", "增益", "模式", "启动", "停止")[op]} {value if op in (1,2,3) else ""}；等待设备确认…')
        def work():
            try:
                result = control(host, port, request_id, op, value, session=device_session)
            except (OSError, ValueError) as exc:
                result = f'未确认：{exc}。请查询设备；本软件需要 WFC2 固件。'
            self.results.put((session, request_id, op, result, background))
        self.control_thread = threading.Thread(target=work, name='pico-control', daemon=True)
        self.control_thread.start()

    def refresh(self):
        active = bool(self.receiver and self.receiver.is_alive())
        busy = self.busy()
        ui_busy = self.pending_action is not None or (busy and not self.background_request)
        for w in (self.host, self.data_port, self.control_port): w.setEnabled(not active and not busy)
        self.connect_button.setEnabled(not active and not busy)
        self.disconnect_button.setEnabled(active and not self.disconnect_requested)
        connected = bool(active and self.receiver.connected and not self.receiver.stop_event.is_set())
        for w in (self.rate, self.gain, self.mode, self.rate_button, self.gain_button, self.mode_button):
            w.setEnabled(connected and not ui_busy and self.state == '待机' and not self.disconnect_requested)
        self.query_button.setEnabled(connected and not ui_busy and not self.disconnect_requested)
        self.display_button.setEnabled(connected and not ui_busy and self.state in ('待机', '采集中') and not self.disconnect_requested)
        if self.receiver:
            packets, status, connected, stats, dropped = self.receiver.snapshot()
            if not active:
                self.pending_action = None
                self.set_state('未连接' if self.disconnect_requested else '故障')
                self.synced = False
            self.connection_label.setText(f'{self.state} · {status}')
            for packet in packets:
                if self.receiver.stop_event.is_set(): break
                if self.displaying and packet.stream_id == self.expected_stream:
                    self.latest, self.latest_at = packet, time.monotonic()
                    self.plot.append(packet)
            if self.latest:
                p = self.latest
                stale = '历史波形，当前未显示新数据' if not self.displaying else '最近收到，当前已过期' if not connected or time.monotonic() - self.latest_at > 5 else '最新有效数据'
                clock = '主时钟未经实测' if p.flags & 8 else '主时钟由固件报告'
                self.actual_label.setText(f'数据包实际参数（{stale}）：{p.nominal_rate} SPS × MCLK {p.mclk_hz} / 2048000 = '
                                          f'{p.sample_rate:g} SPS（推导） | 增益 ×{p.gain} | '
                                          f'{MODES[p.mode]} | {clock}')
            self.stats_label.setText(f'{stats} | 显示队列丢弃 {dropped} 帧 | 波形连续性重置 {self.plot.discontinuities}')
        try:
            session, request_id, op, result, background = self.results.get_nowait()
            if session == self.session and active and self.receiver.connected and not self.receiver.stop_event.is_set():
                self.next_query = time.monotonic() + 2
                if isinstance(result, str):
                    if background:
                        self.device_label.setText(f'设备状态查询失败：{result}')
                        if self.pending_action:
                            self.control_label.setText('待执行操作已取消：设备状态未确认，请先查询设备。')
                    else:
                        self.control_label.setText(f'请求 #{request_id} {result}')
                    self.pending_action = None
                    self.set_state('故障')
                    self.query_needed = op != 0 and not self.disconnect_requested
                    self.synced = False
                else:
                    self.device_session = result.session
                    self.synced = True
                    self.query_needed = False
                    validity = '设备确认配置' if result.configured else '未配置/故障，参数不可视为生效'
                    status_text = (f'{validity}：{result.rate} SPS / ×{result.gain} / {MODES[result.mode]}'
                                   f' | 采集 {"运行" if result.running else "停止"}')
                    if self.device_label.text() != status_text:
                        self.device_label.setText(status_text)
                    if not background:
                        self.control_label.setText(f'回执 #{request_id}：{result.description} | {validity}：'
                                               f'{result.rate} SPS / ×{result.gain} / {MODES[result.mode]} | 采集 {"运行" if result.running else "停止"}'
                                               + (' | 6 nA，MCLK/65536 Hz，独立差分输入，未校准原始码' if result.mode == 3 else ''))
                    if not result.result or not result.configured:
                        self.set_state('故障')
                    elif result.running and self.run_requested:
                        if self.expected_stream != result.stream_id:
                            self.plot.clear(); self.latest = None
                        self.expected_stream = result.stream_id
                        self.set_state('采集中')
                    elif result.running:
                        self.force_stop = True; self.set_state('停止中')
                    else:
                        self.run_requested = False; self.set_state('待机')
                if op == 5 and self.disconnect_requested:
                    self.receiver.stop()  # FIN also triggers firmware stop if reply was lost.
                elif self.disconnect_requested and isinstance(result, str) and not self.device_session:
                    self.receiver.stop()
        except queue.Empty:
            pass
        if connected and not self.busy() and not self.receiver.stop_event.is_set():
            if (self.disconnect_requested or self.force_stop) and self.device_session:
                self.pending_action = None
                self.force_stop = False; self.send_control(5, 0)
            elif self.pending_action is not None:
                op, value = self.pending_action
                self.pending_action = None
                if op in (1, 2, 3, 4) and self.state != '待机':
                    self.control_label.setText('待执行操作已取消：设备已不处于待机状态。')
                else:
                    self.send_control(op, value)
            elif self.query_needed or (self.synced and time.monotonic() >= self.next_query):
                background = self.synced and not self.query_needed
                self.query_needed = False; self.send_control(0, 0, background=background)
        if self.displaying: self.plot.update()
        if self.closing and not active and not self.busy(): self.close()

    def closeEvent(self, event):
        if (self.receiver and self.receiver.is_alive()) or self.busy():
            self.closing = True
            self.disconnect_device()
            event.ignore()  # Complete STOP asynchronously, then close the window.
        else:
            self.timer.stop(); event.accept()


def main():
    app = QApplication(sys.argv)
    window = Window(); window.show()
    return app.exec()


if __name__ == '__main__':
    raise SystemExit(main())
