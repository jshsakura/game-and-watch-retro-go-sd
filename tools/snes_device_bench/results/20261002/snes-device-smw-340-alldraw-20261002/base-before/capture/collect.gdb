set pagination off
set confirm off
set remotetimeout 10
file "/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-snes60/build/snes-device-pair/A/gw_retro_go.elf"
target extended-remote localhost:3333
monitor halt
python
import gdb
if int(gdb.parse_and_eval('snes_device_bench_result.completed')) >= 120:
    raise gdb.GdbError('attach arrived after warmup: measurement refused')
end
hbreak common_emu_bench_complete
hbreak common_emu_bench_begin
continue
python
import gdb, time
if (int(gdb.parse_and_eval('$pc')) & ~1) != (int(gdb.parse_and_eval('&common_emu_bench_begin')) & ~1):
    raise gdb.GdbError('wrong initial stop')
gdb.execute('set variable forced_draw_ratio = 1')
gdb.execute('set variable frame_integrator = 0')
gdb.execute('set variable skip_streak = 0')
gdb.execute('set variable common_emu_state.skip_frames = 0')
gdb.execute('set variable common_emu_state.last_sync_time = uwTick')
window_host_start = time.monotonic()
end
continue
python
import gdb
marker = int(gdb.parse_and_eval('&common_emu_bench_complete')) & ~1
if (int(gdb.parse_and_eval('$pc')) & ~1) != marker:
    raise gdb.GdbError('target stopped somewhere other than completion marker')
end
python
import json, time
window_host_ms = (time.monotonic() - window_host_start) * 1000
with open("/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-snes60/build/snes-device-smw-340-alldraw-20261002/base-before/capture/host-timer.json", 'w') as f:
    json.dump(dict(window_host_ms=window_host_ms), f)
end
python
import gdb, json
inf = gdb.selected_inferior()
record_addr = int(gdb.parse_and_eval('&snes_device_bench_result'))
record_size = int(gdb.parse_and_eval('sizeof(snes_device_bench_result)'))
audio_addr = int(gdb.parse_and_eval('&audio_buf'))
audio_size = int(gdb.parse_and_eval('sizeof(audio_buf)'))
screen_addr = int(gdb.parse_and_eval('snes_device_bench_result.framebuffer'))
for path, addr, size in [("/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-snes60/build/snes-device-smw-340-alldraw-20261002/base-before/capture/record.bin", record_addr, record_size),
                         ("/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-snes60/build/snes-device-smw-340-alldraw-20261002/base-before/capture/audio.bin", audio_addr, audio_size),
                         ("/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-snes60/build/snes-device-smw-340-alldraw-20261002/base-before/capture/screen.bin", screen_addr, 153600)]:
    with open(path, 'wb') as f:
        f.write(bytes(inf.read_memory(addr, size)))
with open("/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-snes60/build/snes-device-smw-340-alldraw-20261002/base-before/capture/audio-hardware.json", "w") as f:
    json.dump(dict(sai_cr1=int.from_bytes(bytes(inf.read_memory(0x40015804,4)), "little"),
                   forced_draw_ratio=int(gdb.parse_and_eval('forced_draw_ratio')),
                   stretch_step=int(gdb.parse_and_eval("'snes_audio_stretch.c'::step")),
                   stretch_base=int(gdb.parse_and_eval("'snes_audio_stretch.c'::base")),
                   stretch_fill=int(gdb.parse_and_eval("'snes_audio_stretch.c'::fill")),
                   stretch_pitch_conf=int(gdb.parse_and_eval("'snes_audio_stretch.c'::pitch_conf"))), f)
end
disconnect
quit
