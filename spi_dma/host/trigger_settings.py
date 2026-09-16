"""Reference-style key/event/annotation table; safe JSON persistence."""
import json
from pathlib import Path
from PySide6.QtGui import QKeySequence
from PySide6.QtWidgets import (QDialog, QVBoxLayout, QHBoxLayout, QTableWidget,
    QTableWidgetItem, QKeySequenceEdit, QLineEdit, QPushButton, QLabel, QDialogButtonBox, QHeaderView)

CONFIG = Path.home()/'.ads1299-usb'/'triggers.json'


def load_triggers():
    try:
        data = json.loads(CONFIG.read_text(encoding='utf-8'))
        return {str(k): [str(v[0]), str(v[1])] for k, v in data.items()
                if isinstance(v, list) and len(v) == 2}
    except (OSError, ValueError, AttributeError, TypeError):
        return {'F1': ['1', '']}


class TriggerDialog(QDialog):
    def __init__(self, mapping, parent=None):
        super().__init__(parent)
        self.setWindowTitle('自定义触发：快捷键 / 事件 / 注释')
        self.resize(650,400)
        self.mapping = {k: list(v) for k,v in mapping.items()}
        layout = QVBoxLayout(self)
        self.table = QTableWidget(0,3)
        self.table.setHorizontalHeaderLabels(['快捷键','事件','注释'])
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.table.cellClicked.connect(self.select)
        layout.addWidget(self.table)
        row = QHBoxLayout();layout.addLayout(row)
        self.key = QKeySequenceEdit();self.key.setMaximumSequenceLength(1)
        self.event = QLineEdit();self.event.setPlaceholderText('事件')
        self.note = QLineEdit();self.note.setPlaceholderText('注释')
        for w in (self.key,self.event,self.note): row.addWidget(w)
        row = QHBoxLayout();layout.addLayout(row)
        for label, callback in (('添加 / 更新',self.add),('删除选中',self.remove)):
            button=QPushButton(label);button.clicked.connect(callback);row.addWidget(button)
        self.hint=QLabel('仅当前窗口生效；输入框内不触发。F8 保留。事件和注释合计 ≤40 UTF-8 字节。')
        self.hint.setWordWrap(True);layout.addWidget(self.hint)
        buttons=QDialogButtonBox(QDialogButtonBox.StandardButton.Save|QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.save);buttons.rejected.connect(self.reject);layout.addWidget(buttons)
        self.reload()

    def reload(self):
        self.table.setRowCount(len(self.mapping))
        for row,(key,values) in enumerate(self.mapping.items()):
            for col,value in enumerate([key,*values]):self.table.setItem(row,col,QTableWidgetItem(value))

    def select(self,row,col):
        self.key.setKeySequence(QKeySequence(self.table.item(row,0).text()))
        self.event.setText(self.table.item(row,1).text());self.note.setText(self.table.item(row,2).text())

    def add(self):
        key=self.key.keySequence().toString();event=self.event.text().strip();note=self.note.text().strip()
        if not key or key=='F8' or not event or len((event+' '+note).strip().encode('utf-8'))>40:
            self.hint.setText('无效快捷键或事件；F8 保留，事件与注释不得超过 40 UTF-8 字节。');return
        self.mapping[key]=[event,note];self.reload()

    def remove(self):
        row=self.table.currentRow()
        if row>=0:self.mapping.pop(self.table.item(row,0).text(),None);self.reload()

    def save(self):
        try:
            CONFIG.parent.mkdir(parents=True,exist_ok=True)
            temporary=CONFIG.with_suffix('.tmp')
            temporary.write_text(json.dumps(self.mapping,ensure_ascii=False,indent=2),encoding='utf-8')
            temporary.replace(CONFIG)
        except OSError as exc:self.hint.setText(str(exc));return
        self.accept()
