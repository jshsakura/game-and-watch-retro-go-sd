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
continue
python
import gdb
marker = int(gdb.parse_and_eval('&common_emu_bench_complete')) & ~1
if (int(gdb.parse_and_eval('$pc')) & ~1) != marker:
    raise gdb.GdbError('target stopped somewhere other than completion marker')
end
python
import gdb, json
inf = gdb.selected_inferior()
record_addr = int(gdb.parse_and_eval('&snes_device_bench_result'))
record_size = int(gdb.parse_and_eval('sizeof(snes_device_bench_result)'))
audio_addr = int(gdb.parse_and_eval('&audio_buf'))
audio_size = int(gdb.parse_and_eval('sizeof(audio_buf)'))
screen_addr = int(gdb.parse_and_eval('snes_device_bench_result.framebuffer'))
for path, addr, size in [("/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-snes60/build/snes-device-smw-r2-20261002/base-before/capture/record.bin", record_addr, record_size),
                         ("/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-snes60/build/snes-device-smw-r2-20261002/base-before/capture/audio.bin", audio_addr, audio_size),
                         ("/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-snes60/build/snes-device-smw-r2-20261002/base-before/capture/screen.bin", screen_addr, 153600)]:
    with open(path, 'wb') as f:
        f.write(bytes(inf.read_memory(addr, size)))
with open("/home/pi/app/jupyterLab/notebooks/game-and-watch-retro-go-sd-snes60/build/snes-device-smw-r2-20261002/base-before/capture/audio-hardware.json", "w") as f:
    json.dump(dict(sai_cr1=int.from_bytes(bytes(inf.read_memory(0x40015804,4)), "little")), f)
end
disconnect
quit
