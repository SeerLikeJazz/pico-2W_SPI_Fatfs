"""USB acquisition UI. Reception/storage run independently of the 30 Hz display."""
import os
from pathlib import Path
import queue
import sys
import threading
from PySide6.QtCore import QTimer
from PySide6.QtGui import QFont, QFontDatabase, QShortcut, QKeySequence
from PySide6.QtWidgets import (QApplication, QComboBox, QHBoxLayout, QLabel,
    QLineEdit, QMainWindow, QPushButton, QVBoxLayout, QWidget, QFileDialog, QDialog)
from usb_receiver import checked
from usb_process import ProcessReceiver as Receiver
from usb_protocol import QUERY, START, STOP, RATE, GAIN, MODE
from sweep_plot import Plot, TIME_SCALES, VOLTAGE_SCALES
from signal_tools import HIGH, LOW, NOTCH
from trigger_settings import TriggerDialog, load_triggers


class Window(QMainWindow):
    def __init__(self):
        super().__init__()
        if os.name == 'nt':
            font_path = Path(os.environ.get('WINDIR', 'C:/Windows')) / 'Fonts/msyh.ttc'
            if font_path.exists():
                QFontDatabase.addApplicationFont(str(font_path))
                self.setFont(QFont('Microsoft YaHei', 10))
        self.setWindowTitle('ADS1299 · USB Raw · Core 0')
        self.resize(1200,820)
        self.receiver = None; self.busy=False; self.results=queue.Queue(); self.device={}
        self.closing=False
        self.bdf_active=False;self.pending_markers=[];self.shortcuts=[]
        self.trigger_mapping=load_triggers()
        root=QWidget(); self.setCentralWidget(root); layout=QVBoxLayout(root)
        title=QLabel('ADS1299 → Core 0 → USB  |  无应用层校验 · 不轮询设备参数')
        layout.addWidget(title)
        row=QHBoxLayout(); layout.addLayout(row)
        self.port=QComboBox(); self.port.setEditable(True)
        try:
            from serial.tools import list_ports
            for item in list_ports.comports(): self.port.addItem(item.device)
        except ImportError: self.port.addItem('COM7')
        row.addWidget(self.port)
        self.connect_button=QPushButton('连接'); self.connect_button.clicked.connect(self.connect_device); row.addWidget(self.connect_button)
        self.disconnect_button=QPushButton('断开'); self.disconnect_button.clicked.connect(self.disconnect_device); row.addWidget(self.disconnect_button)
        self.raw=QLineEdit(); self.raw.setPlaceholderText('可选：新建原始数据文件路径（连接前填写）'); row.addWidget(self.raw)
        row=QHBoxLayout(); layout.addLayout(row)
        self.rate=QComboBox(); self.rate.addItems(['250','500','1000','2000','4000','8000','16000'])
        self.gain=QComboBox(); self.gain.addItems(['1','2','4','6','8','12','24'])
        self.mode=QComboBox(); self.mode.addItems(['内部测试','输入短接','正常输入','阻抗激励'])
        self.apply_button=QPushButton('应用参数（待机）'); self.apply_button.clicked.connect(self.apply)
        self.start_button=QPushButton('开始'); self.start_button.clicked.connect(lambda:self.operation(START))
        self.stop_button=QPushButton('停止'); self.stop_button.clicked.connect(lambda:self.operation(STOP))
        self.query_button=QPushButton('查询状态'); self.query_button.clicked.connect(lambda:self.operation(QUERY))
        for widget in (self.rate,self.gain,self.mode,self.apply_button,self.start_button,self.stop_button,self.query_button): row.addWidget(widget)
        self.device_label=QLabel('未连接'); self.device_label.setWordWrap(True); layout.addWidget(self.device_label)
        row=QHBoxLayout();layout.addLayout(row)
        self.filters=[]
        for label,choices,default in (('高通',HIGH,1),('低通',LOW,30),('陷波',NOTCH,50)):
            row.addWidget(QLabel(label));combo=QComboBox()
            for value in choices:combo.addItem('关闭' if value is None else f'{value:g} Hz',value)
            combo.setCurrentIndex(combo.findData(default));row.addWidget(combo);self.filters.append(combo)
        self.filter_button=QPushButton('应用显示滤波');self.filter_button.clicked.connect(self.apply_filter);row.addWidget(self.filter_button)
        self.trigger_text=QLineEdit('1');self.trigger_text.setMaximumWidth(140)
        self.trigger_text.setToolTip('软件事件；定位到接收进程处理触发时的最新样本，不是硬件精确触发')
        row.addWidget(self.trigger_text)
        self.trigger_button=QPushButton('触发');self.trigger_button.clicked.connect(lambda:self.trigger(self.trigger_text.text()));row.addWidget(self.trigger_button)
        self.trigger_setup=QPushButton('触发设置');self.trigger_setup.clicked.connect(self.edit_triggers);row.addWidget(self.trigger_setup)
        row=QHBoxLayout();layout.addLayout(row)
        self.bdf_path=QLineEdit();self.bdf_path.setPlaceholderText('新建 BDF+ 文件（保存未滤波的 8 通道数据）');row.addWidget(self.bdf_path)
        self.bdf_browse=QPushButton('选择文件');self.bdf_browse.clicked.connect(self.choose_bdf);row.addWidget(self.bdf_browse)
        self.bdf_start=QPushButton('开始 BDF 保存');self.bdf_start.clicked.connect(lambda:self.local_operation('bdf_start',self.bdf_path.text().strip()));row.addWidget(self.bdf_start)
        self.bdf_stop=QPushButton('结束 BDF 保存');self.bdf_stop.clicked.connect(lambda:self.local_operation('bdf_stop',None));row.addWidget(self.bdf_stop)
        self.feature_label=QLabel('');self.feature_label.setWordWrap(True);layout.addWidget(self.feature_label)
        self.plot=Plot(); layout.addWidget(self.plot,1)
        self.plot.set_filter(1,30,50)
        row=QHBoxLayout(); layout.addLayout(row)
        row.addWidget(QLabel('时间页：'))
        self.time_page=QComboBox()
        for seconds in TIME_SCALES: self.time_page.addItem(f'{seconds}s', seconds)
        self.time_page.setCurrentText('5s')
        self.time_page.currentIndexChanged.connect(lambda:self.plot.set_time_scale(self.time_page.currentData()))
        row.addWidget(self.time_page)
        row.addWidget(QLabel('显示量程：'))
        self.amplitude=QComboBox(); self.amplitude.addItem('Auto',None)
        for value in VOLTAGE_SCALES:
            self.amplitude.addItem(f'{value} µV' if value<1000 else f'{value//1000} mV',value)
        self.amplitude.setCurrentText('100 µV')
        self.amplitude.setToolTip('只改变显示量程，按内部参考标称 4.5 V 换算 µV，未经电压校准；BDF 保存滤波前数据。')
        self.amplitude.currentIndexChanged.connect(self.set_display_scale)
        row.addWidget(self.amplitude)
        self.scale_label=QLabel('扫描暂留 · 显示滤波 · 双击通道标签聚焦/还原')
        self.plot.scale_changed.connect(lambda v:self.scale_label.setText(f'Auto {v:g} µV · 双击通道标签聚焦/还原'))
        row.addStretch(); row.addWidget(self.scale_label)
        self.stats_label=QLabel(); self.stats_label.setWordWrap(True); layout.addWidget(self.stats_label)
        self.timer=QTimer(self); self.timer.setInterval(33); self.timer.timeout.connect(self.refresh); self.timer.start()
        self.install_triggers()

    def apply_filter(self):
        try:
            self.plot.set_filter(*(combo.currentData() for combo in self.filters))
            self.feature_label.setText('显示滤波已应用；旧页保留，新数据使用新参数。BDF 不受影响。')
        except ValueError as exc:self.feature_label.setText(str(exc))

    def choose_bdf(self):
        path,_=QFileDialog.getSaveFileName(self,'新建 BDF+ 文件',self.bdf_path.text(),'BDF (*.bdf)')
        if path:self.bdf_path.setText(path)

    def local_operation(self,action,value):
        receiver=self.receiver
        if not receiver or self.busy:return
        if action=='bdf_start' and not value:
            self.feature_label.setText('请先选择新的 BDF 文件路径');return
        def run():
            try:return ('feature',action,receiver.request(action,value))
            except Exception as exc:return ('feature_error',str(exc))
        self.work(run)

    def trigger(self,text):
        if not self.device.get('running'):return
        if self.busy:
            self.feature_label.setText('控制通道忙，本次触发未接受');return
        self.local_operation('marker',text)

    def hotkey_trigger(self,text):
        if isinstance(QApplication.focusWidget(),QLineEdit) or QApplication.activeModalWidget():return
        self.trigger(text)

    def install_triggers(self):
        for shortcut in self.shortcuts:shortcut.setEnabled(False);shortcut.deleteLater()
        self.shortcuts=[]
        for key,(event,note) in self.trigger_mapping.items():
            shortcut=QShortcut(QKeySequence(key),self);shortcut.setAutoRepeat(False)
            text=(event+' '+note).strip()
            shortcut.activated.connect(lambda text=text:self.hotkey_trigger(text))
            self.shortcuts.append(shortcut)

    def edit_triggers(self):
        dialog=TriggerDialog(self.trigger_mapping,self)
        if dialog.exec()==QDialog.DialogCode.Accepted:
            self.trigger_mapping=dialog.mapping;self.install_triggers()

    def work(self, fn):
        if self.busy: return
        self.busy=True
        def run():
            try: result=fn()
            except Exception as exc: result=exc
            self.results.put(result)
        threading.Thread(target=run,daemon=True).start()

    def set_display_scale(self):
        self.scale_label.setText('扫描暂留 · 双击通道标签聚焦/还原')
        self.plot.set_voltage_scale(self.amplitude.currentData())

    def connect_device(self):
        port,path=self.port.currentText(),self.raw.text().strip()
        def connect():
            receiver=Receiver(port,raw_path=path or None,display=True)
            try: state=checked(receiver,QUERY)
            except Exception: receiver.close(); raise
            return ('connected',receiver,state)
        self.work(connect)

    def operation(self, op):
        receiver=self.receiver
        if receiver:
            def run():
                state=checked(receiver,op)
                if op==STOP:
                    recording=receiver.request('bdf_stop',None)
                    return ('stopped',state,recording)
                return ('state',state)
            self.work(run)

    def apply(self):
        receiver=self.receiver
        if not receiver:return
        values=((RATE,int(self.rate.currentText())),(GAIN,int(self.gain.currentText())),(MODE,self.mode.currentIndex()))
        def configure():
            for op,value in values: state=checked(receiver,op,value)
            return ('state',state)
        self.work(configure)

    def disconnect_device(self):
        receiver=self.receiver
        if not receiver:return
        def close():
            try: checked(receiver,STOP)
            except Exception: pass
            try: recording=receiver.request('bdf_stop',None)
            except Exception as exc: recording={'bdf_error':str(exc)}
            finally: receiver.close()
            return ('closed',recording)
        self.work(close)

    def refresh(self):
        try:
            result=self.results.get_nowait(); self.busy=False
            if isinstance(result,Exception):
                self.device={}
                if self.receiver is None:
                    self.device_label.setText(f'连接失败，端口已释放：{result}；请检查固件后重新连接')
                else:
                    self.device_label.setText(f'状态未确认：{result}；请查询或断开')
            elif result[0]=='connected':
                _,self.receiver,self.device=result; self.plot.begin_connection()
            elif result[0]=='state': self.device=result[1]
            elif result[0]=='stopped':
                self.device=result[1];self.bdf_active=False
                saved=result[2]
                self.feature_label.setText(f"采集停止 · BDF 保存 {saved.get('bdf_samples',0)} 帧 · "
                    f"{saved.get('bdf_path','')} {saved.get('bdf_error','')}")
            elif result[0]=='feature_error':self.feature_label.setText(result[1])
            elif result[0]=='feature':
                _,action,value=result
                if action=='marker':
                    self.pending_markers.append(value)
                    self.feature_label.setText(f"触发 {value['text']}，采样序号 {value['anchor']['sequence']}")
                else:
                    self.bdf_active=value.get('bdf_active',False)
                    self.feature_label.setText(f"BDF {'录制中' if self.bdf_active else '已结束'}：{value.get('bdf_path','')} {value.get('bdf_error','')}")
            elif result[0]=='closed':
                self.receiver=None;self.device={};self.bdf_active=False;self.pending_markers.clear()
                self.device_label.setText('已断开')
                saved=result[1]
                self.feature_label.setText(f"BDF 保存 {saved.get('bdf_samples',0)} 帧 · "
                    f"{saved.get('bdf_path','')} {saved.get('bdf_error','')}")
            if self.device:
                d=self.device
                self.rate.setCurrentText(str(d['rate']))
                self.gain.setCurrentText(str(d['gain']))
                self.mode.setCurrentIndex(d['mode'])
                self.device_label.setText(f"设备确认：运行={d['running']} 配置={d['configured']} SPI={d['spi']} Hz  "
                    f"采集丢帧={d['adc_queue_drops']} DMA冲突={d['busy_drdy']} 发送丢帧={d['tx_sample_drops']} "
                    f"队列峰值={d['adc_queue_peak']}/{d['tx_queue_peak']} 停止原因={d['stop_reason']} 错误={d['error']}")
        except queue.Empty: pass
        connected=self.receiver is not None and not self.receiver.done.is_set()
        running=bool(self.device.get('running'))
        self.connect_button.setEnabled(not self.receiver and not self.busy)
        self.disconnect_button.setEnabled(self.receiver is not None and not self.busy)
        self.port.setEnabled(not self.receiver and not self.busy); self.raw.setEnabled(not self.receiver and not self.busy)
        for w in (self.rate,self.gain,self.mode,self.apply_button): w.setEnabled(connected and not running and not self.busy and bool(self.device))
        self.start_button.setEnabled(connected and not running and not self.busy and bool(self.device))
        self.stop_button.setEnabled(connected and not self.busy)
        self.query_button.setEnabled(connected and not self.busy)
        self.trigger_button.setEnabled(connected and running and not self.busy)
        self.bdf_start.setEnabled(connected and running and not self.bdf_active and not self.busy)
        self.bdf_stop.setEnabled(connected and self.bdf_active and not self.busy)
        self.bdf_path.setEnabled(not self.bdf_active);self.bdf_browse.setEnabled(not self.bdf_active)
        if self.receiver:
            stats,packets=self.receiver.snapshot(take_display=True)
            for packet in packets:self.plot.append(packet)
            if packets:
                # Reflect any cutoff disabled after an ADC sample-rate reduction.
                for combo,value in zip(self.filters,self.plot.filter_settings):
                    if value is None and combo.currentData() is not None and combo.currentData()>=self.plot.fs/2:
                        combo.setCurrentIndex(combo.findData(None))
            remaining=[]
            for marker in self.pending_markers:
                if not self.plot.add_marker(marker['anchor'],marker['text']):remaining.append(marker)
            self.pending_markers=remaining[-64:]
            self.stats_label.setText(f"接收 {stats['samples']} 帧 / {stats['bytes']} 字节  | "
                f"真实序号缺口 {stats['sample_gaps']}  | 显示队列丢弃 {stats['display_dropped_samples']} 帧  | "
                f"绘图断线 {self.plot.breaks}  | 存储丢字节 {stats.get('storage_dropped_bytes',0)}  | "
                f"解析异常 {stats['malformed']}  {stats['error']} {stats.get('storage_error','')}  | "
                f"BDF {stats.get('bdf_samples',0)} 帧 / 丢弃 {stats.get('bdf_dropped_samples',0)} / 分段 {stats.get('bdf_parts',0)} {stats.get('bdf_error','')}")
            self.plot.flush()
        if self.closing and not self.busy:
            if self.receiver:self.disconnect_device()
            else:self.close()

    def closeEvent(self,event):
        if self.receiver or self.busy:
            self.closing=True;event.ignore()
        else:self.timer.stop();event.accept()


def main():
    app=QApplication(sys.argv);window=Window();window.show();return app.exec()

if __name__=='__main__':raise SystemExit(main())
