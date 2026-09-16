"""Explicit real-device validation: --port COM11 --seconds 60 (never flashes).

Runs the production independent receiver and real Qt sweep canvas together.
Creates a new raw file and JSON report, never overwrites previous captures.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'host'))


def main():
    from PySide6.QtCore import QTimer
    from PySide6.QtWidgets import QApplication
    from sweep_plot import Plot
    from usb_process import ProcessReceiver
    from usb_receiver import checked
    from usb_protocol import QUERY,START,STOP,RATE,GAIN,MODE,Decoder,Continuity
    parser=argparse.ArgumentParser()
    parser.add_argument('--port',required=True)
    parser.add_argument('--seconds',type=float,default=60)
    parser.add_argument('--rate',type=int,default=16000)
    parser.add_argument('--gain',type=int,default=1)
    parser.add_argument('--mode',type=int,default=0)
    parser.add_argument('--time-page',type=int,default=5)
    parser.add_argument('--scale',type=float,default=3000)
    parser.add_argument('--features',action='store_true',help='Enable reference filters, BDF and software trigger')
    args=parser.parse_args()
    prefix=ROOT/'build'/('sweep-hardware-'+datetime.now().strftime('%Y%m%d-%H%M%S'))
    app=QApplication([])
    plot=Plot();plot.resize(1200,650);plot.setWindowTitle('ADS1299 USB sweep — hardware validation')
    # Internal test at gain 1 exceeds 100 µV; use reference 3 mV display preset.
    plot.set_voltage_scale(args.scale);plot.set_time_scale(args.time_page)
    plot.show();app.processEvents()
    if args.features:plot.set_filter(1,30,50)
    receiver=ProcessReceiver(args.port,raw_path=str(prefix)+'.bin')
    report={'port':args.port,'requested_seconds':args.seconds,'raw':str(prefix)+'.bin',
            'settings':vars(args)}
    timer=QTimer();timer.setInterval(33)
    try:
        checked(receiver,QUERY);checked(receiver,STOP)
        for op,value in ((RATE,args.rate),(GAIN,args.gain),(MODE,args.mode)):checked(receiver,op,value)
        report['baseline']=checked(receiver,QUERY)
        if args.features:receiver.request('bdf_start',str(prefix)+'.bdf')
        checked(receiver,START)
        began=time.monotonic();paint_times=[]
        def update():
            try:
                stamp=time.perf_counter()
                stats,packets=receiver.snapshot(take_display=True)
                for packet in packets:plot.append(packet)
                plot.flush()
                if args.features and 'marker' not in report and time.monotonic()-began>1:
                    report['marker']=receiver.request('marker','E1 hardware-test')
                    plot.add_marker(report['marker']['anchor'],'E1')
                paint_times.append((time.perf_counter()-stamp)*1000)
                report['host']=stats
                if stats.get('error'):raise RuntimeError(stats['error'])
                if time.monotonic()-began>=args.seconds:
                    timer.stop();app.quit()
            except Exception as exc:
                report['failure']=repr(exc);timer.stop();app.quit()
        timer.timeout.connect(update);timer.start();app.exec()
        report['elapsed_seconds']=time.monotonic()-began
        report['final']=checked(receiver,STOP)
        if args.features:report['bdf']=receiver.request('bdf_stop',None)
        # Drain remaining display snapshots; sampling is already stopped.
        for _ in range(5):
            time.sleep(.06)
            report['host'],packets=receiver.snapshot(take_display=True)
            for packet in packets:plot.append(packet)
            plot.flush()
        plot.grab().save(str(prefix)+'.png')
        report['display_breaks']=plot.breaks
        report['timestamp_discontinuities']=plot.timestamp_discontinuities
        report['display_tick_max_ms']=max(paint_times,default=0)
    finally:
        timer.stop()
        receiver.close()
        plot.close()
    decoder,tracker=Decoder(),Continuity()
    with open(str(prefix)+'.bin','rb') as file:
        while chunk:=file.read(65536):
            for msg in decoder.feed(chunk):
                if msg.kind==1:tracker.accept(msg)
    report['offline']={k:v for k,v in vars(tracker).items() if k!='last'}
    report['offline'].update(malformed=decoder.malformed,discarded_bytes=decoder.discarded,
                             trailing=len(decoder.buffer))
    if args.features:
        import pyedflib
        import numpy as np
        from signal_tools import digital_samples
        events=[json.loads(line) for line in Path(str(prefix)+'.bdf.jsonl').read_text(encoding='utf-8').splitlines()]
        report['bdf_manifest']=events
        assert report['bdf']['bdf_samples']==tracker.samples and not report['bdf']['bdf_error'],report
        # Direct digital readback against the independent raw-wire recording.
        with pyedflib.EdfReader(str(prefix)+'.bdf') as bdf:
            saved=np.array([bdf.readSignal(ch,digital=True) for ch in range(8)]).T
            raw_decoder=Decoder();offset=0
            with open(str(prefix)+'.bin','rb') as raw:
                while chunk:=raw.read(65536):
                    for msg in raw_decoder.feed(chunk):
                        if msg.kind==1:
                            np.testing.assert_array_equal(saved[offset:offset+msg.count],digital_samples(msg))
                            offset+=msg.count
            report['bdf_readback_exact_samples']=offset
            onset,duration,text=bdf.readAnnotations()
            assert 'E1 hardware-test' in text
    with open(str(prefix)+'.json','x',encoding='utf-8') as file:json.dump(report,file,indent=2)
    print(json.dumps(report,indent=2))
    assert not report.get('failure'),report
    assert not any(report['offline'][k] for k in ('sample_gaps','packet_gaps','reorders','malformed','discarded_bytes','trailing')),report
    assert not report['host'].get('storage_dropped_bytes') and not report['host'].get('storage_error'),report


if __name__=='__main__':main()
