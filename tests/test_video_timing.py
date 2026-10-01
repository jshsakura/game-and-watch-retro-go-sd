"""Run the real player with the existing rig's injected I/O/audio clocks.

This is a native logic test, not a measurement of STM32 JPEG or SD speed.
Compile only the hardware/runtime seams for the host; all playback, demux,
MP3 decoding and resampling code is unchanged. No device is accessed.
"""
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run():
    cc = shlex.split(os.environ.get("CC", "gcc"))
    if not shutil.which(cc[0]) or not shutil.which("ffmpeg"):
        print("SKIP video timing: host C compiler and ffmpeg are required")
        return
    with tempfile.TemporaryDirectory(prefix="video-timing-") as tmp:
        out = Path(tmp)
        tone = out / "tone48.mp3"
        subprocess.run([
            "ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
            "sine=frequency=440:duration=8:sample_rate=48000", "-ac", "1",
            "-b:a", "128k", "-write_xing", "0", "-id3v2_version", "0", str(tone),
        ], check=True)
        (out / "runtime.c").write_text("void rig_timer_init(void) {}\n")
        (out / "tone.S").write_text(
            '.section .rodata\n.global _binary_tone48_mp3_start\n'
            '_binary_tone48_mp3_start:\n'
            f'.incbin "{tone}"\n'
            '.global _binary_tone48_mp3_end\n_binary_tone48_mp3_end:\n'
            '.section .note.GNU-stack,"",%progbits\n'
        )
        command = cc + [
            "-O2", "-fno-strict-aliasing", "-Itests/video_stubs",
            "-ICore/Inc/porting/video", "-ICore/Src/porting/lib",
            "-ICore/Inc/porting/music", "-DSD_CARD=0", "-DRESUME_HOST_STDIO",
            f'-DRESUME_PATH="{out / "resume.txt"}"',
        ]
        sources = [
            "Core/Src/porting/video/video_play.c", "Core/Src/porting/video/avi.c",
            "Core/Src/porting/video/video_decode.c", "Core/Src/porting/video/video_audio.c",
            "Core/Src/porting/video/video_resume.c", "Core/Src/porting/music/music_minimp3.c",
            "tools/m7_qemu_rig/rig_video.c", str(out / "runtime.c"), str(out / "tone.S"),
            "-Wl,--wrap=fopen,--wrap=fread,--wrap=fseek,--wrap=ftell,"
            "--wrap=rewind,--wrap=fclose,--wrap=setvbuf", "-lm",
        ]
        for name, frames, ppm, throughput, decode_us in [
            ("overload", 1000, 0, "0.2", 2000),
            ("normal", 15000, 0, "2.0", 2000),
            ("clock-drift-slow", 15000, 20000, "2.0", 2000),
            ("clock-drift-fast", 15000, -20000, "2.0", 2000),
            ("decode-overload", 1000, 0, "2.0", 40000),
        ]:
            binary = out / name
            build = subprocess.run(command + [
                f"-DRIG_FRAMES={frames}", f"-DAUDIO_PPM={ppm}",
                f"-DTHROUGHPUT_BPUS={throughput}",
                f"-DDECODE_US={decode_us}",
            ] + sources + ["-o", str(binary)], cwd=ROOT, text=True, capture_output=True)
            if build.returncode:
                raise RuntimeError(build.stdout + build.stderr)
            result = subprocess.run([str(binary)], cwd=out, text=True,
                                    capture_output=True, check=True, timeout=30).stdout
            counts = re.search(r"presented=(\d+) attempts=(\d+) drops=(\d+)", result)
            assert counts, result
            presented, attempts, drops = map(int, counts.groups())
            assert attempts == frames and "video_play -> 0 " in result, result
            assert "LATCHED:" not in result, result
            elapsed_us = int(re.search(r"elapsed_us=(\d+)", result)[1])
            expected_us = frames * 24000 / (1 - ppm / 1e6)
            assert abs(elapsed_us - expected_us) < 1000000, result
            if name == "overload":
                # Old code displays only two frames, then keeps reading frames
                # it will discard; 24 seconds of media takes ~52 virtual seconds.
                elapsed = sum(int(x) * 200 for x in re.findall(r"dt=\s*(\d+)us", result))
                assert presented >= 150, result
                assert abs(elapsed - frames * 24000) < 1000000, result
            elif name == "decode-overload":
                assert presented >= 300 and drops > 0, result
            else:
                assert presented == frames and drops == 0, result
            print(f"OK video timing {name}: {presented}/{attempts} presented, {drops} drops")


if __name__ == "__main__":
    run()
