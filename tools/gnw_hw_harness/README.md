# GNW SD hardware-contract harness

This is the shared, reproducible hardware contract for the STM32H7B0VB
Game & Watch SD build. It prevents a model or engineer from silently replacing
the real device with a remembered specification.

It makes three deliberately separate claims:

1. **Static layout:** the GNU ld map is authoritative for ITCM, DTCM heap,
   framebuffer, RAM_EMU overlays, AHB statics/heap, and the audio reservation.
2. **Runtime memory:** a device profile supplies facts a map cannot contain,
   especially DTCM bytes already consumed when emulator initialization begins.
   The profile is SHA-256-bound to the exact map and optionally the ELF.
3. **Timing:** QEMU M7 supplies executed ARM instructions, never absolute time.
   `timing_oracle.py` converts those work units with DWT measurements bound to
   the same device profile and reports the calibration error/status.

There is no claim that `mps2-an500` emulates STM32H7 caches, OSPI, SD-SPI DMA,
LCD DMA, or bus arbitration. A future custom QEMU machine may improve address
and fault fidelity, but silicon remains the timing oracle.

## Canonical facts and their authority

| Fact | Authority |
|---|---|
| ITCM/DTCM/RAM_EMU/AHB sizes and section use | current linker map |
| DTCM use at emulator entry | device allocator/DWT log |
| clock (`clk=`) | device profile log |
| ARM instruction count | QEMU `mps2-an500 -icount shift=0` |
| framebuffer/audio/state correctness | identical host/QEMU/device hashes |
| absolute fps | device, or an explicitly labelled empirical estimate |

The regression golden values are 64 KiB ITCM, 82,944-byte DTCM heap,
74,728 bytes consumed at emulator entry, 8,216 bytes effectively free,
741,376-byte RAM_EMU, 122,880 bytes of AHB before the 8 KiB audio reservation,
and a measured `profile2` clock of 312,000,000 Hz. These values test the parser;
real work must use a newly bound profile rather than copying them.

## Quick start

Run the self-tests, including the two hardware regressions:

```sh
tools/gnw_hw_harness/run_tests.sh
```

Build the canonical SD firmware, then extract the static contract:

```sh
tools/gnw_hw_harness/run.sh
```

Without a device profile, DTCM effective free space and clock are intentionally
reported as unknown. The tool will not substitute the nominal 81 KiB heap or a
clock remembered from documentation.

Create a profile from a captured device log. The log may contain `clk=312000000`
and `HEAP OOM: need=... used=74728/82944`; values not present can be supplied
explicitly:

```sh
python3 tools/gnw_hw_harness/gnw_hw.py profile-from-log \
  --log /path/to/device.log \
  --map build/gw_retro_go.map --elf build/gw_retro_go.elf \
  --config-manifest tools/gnw_hw_harness/config/sd_mario.json \
  --dtcm-used 74728 --output /path/to/device-profile.json
```

Extract and gate proposed allocations:

```sh
tools/gnw_hw_harness/run.sh --profile /path/to/device-profile.json \
  --config tools/gnw_hw_harness/config/sd_mario.json \
  --proposal dtcm:61440:rc_dispatch \
  --proposal ahb:86016:Draw2FB
```

The DTCM proposal fails against the observed 8,216-byte effective free space.
The AHB result follows the current map: the historical fixture with 43,200
bytes already reserved fails, while a corrected linker layout may pass.

## Poison allocator

`alloc_model.c` is a reusable bump-pool shim for device-shaped host harnesses.
Initialize a pool with both its capacity and the pre-consumed byte count, then
route the core's allocation seam through `gnw_alloc_malloc` and
`gnw_alloc_calloc`. Plain allocations contain `0xAA`; only calloc is cleared.
This exposes device-only bugs hidden by a conveniently zero-filled host heap.

The model returns `NULL` on overflow so a harness can assert the expected
failure. Firmware `_sbrk`/AHB assertions may abort instead; the budget decision
is the same.

## Empirical timing oracle

Calibration JSON associates named QEMU work units with the same components'
DWT cycles. Fit and predict only with the exact bound device profile:

```sh
python3 tools/gnw_hw_harness/timing_oracle.py fit \
  --calibration tools/gnw_hw_harness/examples/timing_calibration.json \
  --device-profile /path/to/device-profile.json --output /tmp/timing-model.json

python3 tools/gnw_hw_harness/timing_oracle.py predict \
  --model /tmp/timing-model.json \
  --workload tools/gnw_hw_harness/examples/timing_workload.json \
  --device-profile /path/to/device-profile.json
```

A single calibration sample is labelled `single-sample-no-error-bound` and
does not produce an fps interval. Two or more samples report their maximum
residual and an fps range. This prevents one convenient ROM from being treated
as a universal cache/OSPI model.

## Reproducible container

First build the repository's GCC 15.2.rel1 image if it is not already present:

```sh
make docker_build
tools/gnw_hw_harness/container.sh --build
```

The harness image builds QEMU 8.2.2 from source and refuses to run with a
different ARM GCC/QEMU version. It also contains `mtools`, `dosfstools`, and
`jq` for real FAT image workflows. Network is disabled when the container is
run:

```sh
tools/gnw_hw_harness/container.sh
tools/gnw_hw_harness/container.sh tools/m7_qemu_rig/run_snes.sh ROM 120
```

## Remaining integration work

- Add a small on-device exporter for allocator entry watermarks, all clock
  profiles, cache geometry, SD hardware type, and per-region DWT microbenches.
- Route each core's existing host harness allocation seams through
  `alloc_model.c`; the shared model and regression proof are implemented, but
  cores must opt in rather than relying on global linker interposition.
- Add a QEMU plugin that counts instruction fetch/data accesses by linker
  region. Those counters can become timing-oracle features after device
  calibration; they still must not be described as native QEMU timing.
