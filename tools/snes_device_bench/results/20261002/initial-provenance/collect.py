#!/usr/bin/env python3
"""Wait for one firmware completion breakpoint. Never query progress in a loop."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import threading

FIELDS = '''magic version warmup window completed done start_ms end_ms start_emu end_emu
start_drawn end_drawn audio_calls audio_samples muted_calls start_audio_calls
start_audio_samples start_muted_calls end_audio_calls end_audio_samples end_muted_calls
input_or clock_hz last_drawn_frame framebuffer
rom_crc32 rom_bytes state_resumed state_refused initial_state_crc32 start_state_crc32 end_state_crc32
initial_guest_frame start_guest_frame end_guest_frame start_dma end_dma start_underruns end_underruns
 audio_buffer_samples gapfree speedup'''.split()


def decode(raw):
    if len(raw) != 4 * len(FIELDS):
        raise ValueError('wrong firmware record size')
    r = dict(zip(FIELDS, struct.unpack('<' + 'I' * len(FIELDS), raw)))
    if r['magic'] != 0x53425844 or r['version'] != 1 or r['done'] != 1:
        raise ValueError('no valid firmware completion record')
    if r['completed'] != r['warmup'] + r['window'] or not r['window']:
        raise ValueError('incomplete frame window')
    for label in ['emu', 'drawn', 'audio_calls', 'audio_samples', 'muted_calls']:
        r['delta_' + label] = (r['end_' + label] - r['start_' + label]) & 0xffffffff
    r['elapsed_ms'] = (r['end_ms'] - r['start_ms']) & 0xffffffff
    if r['delta_emu'] != r['window'] or not 0 < r['delta_drawn'] <= r['window']:
        raise ValueError('wrong emulated/presented frame counts')
    if not 0 < r['elapsed_ms'] < 3600000 or not r['clock_hz']:
        raise ValueError('invalid time/clock')
    if r['input_or'] or r['delta_muted_calls'] or r['delta_audio_calls'] != r['window']:
        raise ValueError('input, mute or audio workload differs')
    if r['delta_audio_samples'] < r['window']:
        raise ValueError('missing audio samples')
    if r['state_resumed'] != 1 or r['state_refused']:
        raise ValueError('savestate not resumed')
    if (r['end_guest_frame'] - r['start_guest_frame']) & 0xffffffff != r['window']:
        raise ValueError('wrong guest frame advance')
    if r['audio_buffer_samples'] != 266 or r['delta_audio_samples'] != 266*r['window']:
        raise ValueError('wrong audio sample workload')
    r['delta_dma'] = (r['end_dma'] - r['start_dma']) & 0xffffffff
    r['delta_underruns'] = (r['end_underruns'] - r['start_underruns']) & 0xffffffff
    r['emu_fps'] = r['delta_emu'] * 1000 / r['elapsed_ms']
    r['drawn_fps'] = r['delta_drawn'] * 1000 / r['elapsed_ms']
    r['ratio'] = r['delta_drawn'] / r['delta_emu']
    return r


def collect(elf, output, timeout, port, warmup, smoke=False, completed_from=None):
    output.mkdir(parents=True, exist_ok=False)
    # JSON quoting is GDB string quoting here, never shell command interpolation.
    q = lambda p: json.dumps(str(p.resolve()), ensure_ascii=False)
    script = output / 'collect.gdb'
    start_event = ''
    end_event = ''
    if smoke and completed_from is None:
        start_event = """hbreak common_emu_bench_begin
continue
python
import gdb, time
if (int(gdb.parse_and_eval('$pc')) & ~1) != (int(gdb.parse_and_eval('&common_emu_bench_begin')) & ~1):
    raise gdb.GdbError('wrong initial stop')
window_host_start = time.monotonic()
end
"""
        end_event = f"""python
import json, time
window_host_ms = (time.monotonic() - window_host_start) * 1000
with open({q(output/'host-timer.json')}, 'w') as f:
    json.dump(dict(window_host_ms=window_host_ms), f)
end
"""
    if completed_from is None:
        admission = f"""python
import gdb
if int(gdb.parse_and_eval('snes_device_bench_result.completed')) >= {warmup}:
    raise gdb.GdbError('attach arrived after warmup: measurement refused')
end
hbreak common_emu_bench_complete
{start_event}continue
python
import gdb
marker = int(gdb.parse_and_eval('&common_emu_bench_complete')) & ~1
if (int(gdb.parse_and_eval('$pc')) & ~1) != marker:
    raise gdb.GdbError('target stopped somewhere other than completion marker')
end
{end_event}"""
    else:
        # Recover a finished capture after host file-writing failure, without
        # reflashing, resetting, continuing or measuring a second interval.
        admission = """python
import gdb
if int(gdb.parse_and_eval('snes_device_bench_result.done')) != 1:
    raise gdb.GdbError('recovery requires an already completed firmware record')
end
"""
        if smoke:
            timer = json.loads((completed_from/'host-timer.json').read_text())
            (output/'host-timer.json').write_text(json.dumps(timer)+'\n')
    script.write_text(f'''set pagination off
set confirm off
set remotetimeout 10
file {q(elf)}
target extended-remote localhost:{port}
monitor halt
{admission}python
import gdb, json
inf = gdb.selected_inferior()
record_addr = int(gdb.parse_and_eval('&snes_device_bench_result'))
record_size = int(gdb.parse_and_eval('sizeof(snes_device_bench_result)'))
audio_addr = int(gdb.parse_and_eval('&audio_buf'))
audio_size = int(gdb.parse_and_eval('sizeof(audio_buf)'))
screen_addr = int(gdb.parse_and_eval('snes_device_bench_result.framebuffer'))
for path, addr, size in [({q(output/'record.bin')}, record_addr, record_size),
                         ({q(output/'audio.bin')}, audio_addr, audio_size),
                         ({q(output/'screen.bin')}, screen_addr, 153600)]:
    with open(path, 'wb') as f:
        f.write(bytes(inf.read_memory(addr, size)))
with open({q(output/"audio-hardware.json")}, "w") as f:
    json.dump(dict(sai_cr1=int.from_bytes(bytes(inf.read_memory(0x40015804,4)), "little")), f)
end
disconnect
quit
''')
    ready = threading.Event()
    errors = []
    with (output / 'openocd.log').open('w') as log:
        server = subprocess.Popen(['openocd', '-f', 'interface/stlink-dap.cfg',
            '-f', 'target/stm32h7x.cfg', '-c', 'adapter speed 4000',
            '-c', 'stm32h7x.cpu0 configure -work-area-size 0',
            '-c', f'gdb_port {port}', '-c', 'tcl_port disabled',
            '-c', 'telnet_port disabled'], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True)
        def receive():
            # Blocking reads of process output, not repeated file/state checks.
            try:
                for line in server.stdout:
                    log.write(line)
                    if f'Listening on port {port} for gdb' in line:
                        ready.set()
            except Exception as e:
                errors.append(str(e))
        reader = threading.Thread(target=receive)
        reader.start()
        try:
            if not ready.wait(15):
                raise RuntimeError('no OpenOCD readiness event; no reset/retry performed')
            with (output / 'gdb.log').open('w') as trace:
                subprocess.run(['gdb-multiarch', '-q', '-batch', '-x', str(script)],
                               stdout=trace, stderr=subprocess.STDOUT,
                               timeout=timeout, check=True)
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
            reader.join()
    if errors:
        raise RuntimeError(errors[0])
    report = decode((output / 'record.bin').read_bytes())
    report["sai_cr1"] = json.loads((output/"audio-hardware.json").read_text())["sai_cr1"]
    report["audio_dma_stopped"] = (report["sai_cr1"] & 0x30000) == 0
    if smoke:
        external=json.loads((output/'host-timer.json').read_text())['window_host_ms']
        if abs(external-report['elapsed_ms']) > max(100, external*0.01):
            raise ValueError('HAL_GetTick differs from external window time')
        report['host_window_ms']=external
        report['wall_timer_smoke']='matched within max(100 ms, 1 percent)'
    for name in ['audio', 'screen']:
        data = (output / (name + '.bin')).read_bytes()
        if not data or (name == 'screen' and len(data) != 153600):
            raise ValueError('incomplete ' + name + ' dump')
        report[name + '_sha256'] = hashlib.sha256(data).hexdigest()
    report['elf_sha256'] = hashlib.sha256(elf.read_bytes()).hexdigest()
    report['completion'] = 'GDB stop at resident marker; no progress polling'
    if completed_from is not None:
        report['recovered_from'] = str(completed_from)
        report['recovery'] = 'already completed record; no flash/reset/continue; original smoke timer reused'
    report['limitations'] = ['endpoint framebuffer/audio probes, not full-stream correctness',
                            'timing includes identical finite measurement hooks in both arms; final frame forced drawn']
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--timeout', type=int, default=240)
    p.add_argument('--warmup', type=int, default=1200)
    p.add_argument('--port', type=int, default=3333)
    p.add_argument('--smoke', action='store_true', help='also wait for the initial window marker to validate the timer')
    a = p.parse_args()
    if a.timeout <= 0 or a.warmup <= 0 or not 1 <= a.port <= 65535:
        p.error('positive timeout/warmup and valid port required')
    if a.output.exists():
        p.error('output already exists; preserve prior evidence')
    try:
        r = collect(a.elf.resolve(strict=True), a.output.resolve(), a.timeout, a.port, a.warmup, a.smoke)
        print(json.dumps({'status':'completed', 'drawn_fps':r['drawn_fps'],
                          'report':str(a.output / 'report.json')}))
    except Exception as e:
        a.output.mkdir(parents=True, exist_ok=True)
        (a.output / 'failure.json').write_text(json.dumps({'status':'failed',
            'error':str(e),'automatic_reset_or_retry':False}, indent=2) + '\n')
        raise SystemExit(str(e))

if __name__ == '__main__':
    main()
