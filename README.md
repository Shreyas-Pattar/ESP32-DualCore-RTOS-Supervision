# Dual-Core Real-Time Telemetry & Fault Supervision on ESP32

FreeRTOS firmware for the ESP32 (Arduino core, built with PlatformIO). A periodic task on Core 0 produces simulated sensor samples and profiles its own execution time, a logger task on Core 1 prints them from a queue, and the Task Watchdog Timer (TWDT) recovers the system when the producer hangs. After the reboot, the firmware reports the reset reason.

## What it demonstrates

- **Core pinning:** the sensor task is pinned to Core 0 and the logger to Core 1 (`xTaskCreatePinnedToCore`).
- **Fixed-period scheduling:** the sensor task uses `vTaskDelayUntil()` for a 50 ms (20 Hz) period, which avoids cumulative drift.
- **Queue decoupling:** the sensor task never prints. It pushes `TelemetryPacket_t` structs into a 16-deep queue.
- **Shared serial output:** a mutex protects `Serial` between tasks.
- **Cycle profiling:** each sample records its own execution cost using the CPU cycle counter (`xthal_get_ccount()`).
- **Fault injection and recovery:** after 200 iterations (about 10 s) the sensor task is deliberately hung. The TWDT (3 s timeout) aborts and reboots the chip, and `esp_reset_reason()` reports the cause on the next boot.

## Architecture

```text
 Core 0                                   Core 1
┌────────────────────────┐   queue(16)   ┌──────────────────────┐
│ SensorAcq  (prio 2)    │ ────────────▶ │ TelemLog  (prio 1)   │
│ 50 ms period           │ TelemetryPkt  │ blocks on queue,     │
│ sample + cycle count   │               │ prints via printMutex│
└──────────┬─────────────┘               └──────────────────────┘
           │ esp_task_wdt_reset() every loop
           ▼
   Task Watchdog (3 s) ──▶ abort ──▶ reboot ──▶ CheckBootReason()
```

| Task | Core | Priority | Stack | Behaviour |
|---|---|---|---|---|
| `SensorAcq` | 0 | 2 | 4096 B | Runs every 50 ms, enqueues one packet, resets the watchdog |
| `TelemLog` | 1 | 1 | 4096 B | Blocks on the queue and prints each packet |

Packet fields: `timestamp_ms`, `sensor_raw` (12-bit), `execution_cycles`, `core_id`.

## Fault injection

After 200 loops, `SensorAcq` stops yielding and spins in an infinite loop. This is a **task hang that starves Core 0**, not a mutex deadlock. Because the task never calls `esp_task_wdt_reset()` and also starves `IDLE0`, the TWDT reports both tasks and aborts after the 3 s timeout.

## Results

Captured over UART at 115200 baud (PlatformIO serial monitor) on an ESP32 WROOM-32 dev board (ESP32-D0WD-V3). The capture contains a cold boot followed by **4 consecutive injected faults, all recovered automatically**. Excerpt from one cycle:

```text
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)

============================================
[SYSTEM BOOT] Reset Reason: POWER-ON RESET (Normal cold boot)
============================================

[T=001039 ms | Core 0] Sample: 0xE72 | Cost: 33761 cycles
[T=001089 ms | Core 0] Sample: 0xB4D | Cost: 27370 cycles
[T=001139 ms | Core 0] Sample: 0x747 | Cost: 27370 cycles

... (steady 27,370 cycles per sample, 50 ms apart) ...

[T=010939 ms | Core 0] Sample: 0x18A | Cost: 27370 cycles

[FAULT INJECTION] Inducing artificial task deadlock...
[FAULT INJECTION] SensorTask halted. Watchdog starvation starting!
[T=010989 ms | Core 0] Sample: 0x361 | Cost: 27370 cycles
E (28051) task_wdt: Task watchdog got triggered. The following tasks did not reset the watchdog in time:
E (28051) task_wdt:  - IDLE0 (CPU 0)
E (28051) task_wdt:  - SensorAcq (CPU 0)
E (28051) task_wdt: Tasks currently running:
E (28051) task_wdt: CPU 0: SensorAcq
E (28051) task_wdt: CPU 1: IDLE1
E (28051) task_wdt: Aborting.

abort() was called at PC 0x400d94e5 on core 0

Rebooting...
rst:0xc (SW_CPU_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)

============================================
[SYSTEM BOOT] Reset Reason: CRITICAL FAULT: TASK WATCHDOG TIMEOUT!
System recovered automatically from task starvation.
============================================

[T=001039 ms | Core 0] Sample: 0x1BF | Cost: 33761 cycles
[T=001089 ms | Core 0] Sample: 0x558 | Cost: 27370 cycles
```

| Measurement | Value |
|---|---|
| Steady-state cost per sample | 27,370 cycles (about 114 µs at 240 MHz) |
| First sample after every boot (power-on and watchdog reset) | 33,761 cycles (about 141 µs), i.e. +6,391 cycles (about 27 µs); likely a cold-cache effect |
| Injected faults / automatic recoveries | 4 / 4 |
| Fault location (PC) reported by the watchdog abort | `0x400d94e5` in all 4 cycles |

The workload is a fixed 1,500-iteration loop, so a constant cost is expected.

## Limitations

- The sensor is **simulated** with `esp_random()`. No real sensor is read.
- The measured cost is for a **synthetic loop**, not real peripheral access.
- **Period jitter is not measured yet.** `vTaskDelayUntil()` sets a fixed period, but the actual interval between samples is not logged.
- `xQueueSend` is non-blocking, so a full queue silently drops packets, and there is no drop counter.
- The reset count is not persisted across reboots.
- This is a fault-supervision demo, not yet a scheduling benchmark.

## Build and run

Requirements: PlatformIO, an ESP32 dev board (`esp32dev`).

```bash
pio run -t upload
pio device monitor      # 115200 baud
```

The firmware builds against both ESP-IDF v4.x and v5.x watchdog APIs (selected at compile time with `ESP_IDF_VERSION`).

```text
├── platformio.ini
└── src/
    └── main.cpp
```

## Roadmap

- [ ] Log actual period between samples and report min / max / jitter
- [ ] Add more tasks at different priorities and report CPU load (`vTaskGetRunTimeStats`)
- [ ] Add a drop counter for the queue
- [ ] Persist a reset counter in RTC memory
- [ ] Add a true mutex-deadlock fault case alongside the busy-loop hang
