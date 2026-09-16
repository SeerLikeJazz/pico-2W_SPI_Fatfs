"""Render the real read-only reference class and USB plot with identical inputs."""
import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import sys
sys.dont_write_bytecode = True  # No __pycache__ in the reference project.
from pathlib import Path
import importlib.util
import numpy as np
from PySide6.QtWidgets import QApplication
from PySide6.QtGui import QImage, QPainter, QColor, QFont, QFontDatabase
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'host'))
from sweep_plot import Plot

app = QApplication.instance() or QApplication([])
QFontDatabase.addApplicationFont('C:/Windows/Fonts/msyh.ttc')
source = Path('C:/Users/liuzh/Desktop/iSensys-X-Client/src/isensex/plot/signalMat.py')
spec = importlib.util.spec_from_file_location('reference_signal_mat', source)
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)

old = reference.Plot(fs=1000, ch_num=8, background='#FDFEF6', color='#3D2E03')
old.update_y_scale(100)
old.update_y_labels({i:f'CH{i+1}' for i in range(8)})
old.update_x_ticks(1)
new = Plot(fs=1000)
new.set_time_scale(1)
for widget in (old,new):
    widget.resize(1000,600);widget.show()
app.processEvents()
old.draw();new.draw()
out=ROOT/'build/sweep-comparison';out.mkdir(exist_ok=True)
for start in range(0,1300,50):
    t=np.arange(start,start+50)/1000
    values=np.array([65*np.sin(2*np.pi*(i+2)*t) for i in range(8)]).T
    values[:,2] *= 4  # Deliberate excursion across neighbouring channel lanes.
    if start>=1000:values *= .4
    old.update_data(values);old.update_plot()
    new.feed(values);new.flush()
    if start in (450,950,1250):
        name={450:'half-page',950:'full-page',1250:'wrap-retention'}[start]
        old.grab().save(str(out/f'{name}-reference.png'))
        new.grab().save(str(out/f'{name}-usb.png'))
        pair=QImage(2000,630,QImage.Format.Format_RGB32);pair.fill(QColor('white'))
        p=QPainter(pair);p.setPen(QColor('black'));p.setFont(QFont('Microsoft YaHei',11))
        p.drawText(20,20,'Reference: actual signalMat.py (read only)')
        p.drawText(1020,20,'ADS1299 USB: same input, scan and scale')
        p.drawImage(0,30,old.grab().toImage());p.drawImage(1000,30,new.grab().toImage());p.end()
        pair.save(str(out/f'{name}-comparison.png'))
old.close();new.close()
print(out)
