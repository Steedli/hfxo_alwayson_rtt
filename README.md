# nRF54L15 - HFXO always on / CPU never idle / RTT log

Minimal test application for `nrf54l15dk/nrf54l15/cpuapp`.

## What it does

| Requirement | Implementation |
|---|---|
| External 32 MHz crystal runs continuously | `onoff_request()` on `CLOCK_CONTROL_NRF_SUBSYS_HF` is taken once at boot and **never released**, so `TASKS_XOSTART` stays latched (`src/main.c`, `hfxo_request_forever()`). |
| MCU never enters low power / idle | `CONFIG_PM=n` plus a `spinner` thread at `K_LOWEST_APPLICATION_THREAD_PRIO` that busy-loops forever. The idle thread sits *below* all application priorities, so it is never scheduled and `WFI` is never executed. |
| Continuous debug output | SEGGER RTT only (`CONFIG_RTT_CONSOLE`, `CONFIG_LOG_BACKEND_RTT`), UART disabled. One report line per second. |

## Build & flash

```sh
west build -b nrf54l15dk/nrf54l15/cpuapp
west flash
```

## View the log

```sh
JLinkRTTViewer            # or
JLinkRTTLogger -Device NRF54L15_XXAA -If SWD -Speed 4000 -RTTChannel 0 rtt.log
```

## Reading the output

```
#12 up=12016 ms | HFXO=RUNNING TUNED (XO.RUN=1 XO.STAT=0x00010001) drv=ON | spin=21319659 loops/s
   cpu cycles: total=12014395 idle=0 (idle must stay 0)
```

* `HFXO=RUNNING` comes from `CLOCK.XO.STAT` bit 16 (`STATE`) - this is the
  definitive "the crystal is oscillating" bit, not just "the start task was
  triggered". `XO.RUN=1` with `STATE=0` means the crystal is **not oscillating
  at all** (hardware problem), not a tuning failure.
* `TUNED` comes from `nrfx_clock_xo_tune_status_check()`. `NOT-TUNED` points at
  load capacitance / crystal parameters - check `XOSC32M.CONFIG.INTCAP`
  (printed in the boot banner; 0x29 on this DK).
* `drv=ON` is the Zephyr clock_control view of the HF subsystem.
* `spin` is the busy-loop rate of the lowest-priority thread. A steady,
  large value proves the CPU had a runnable thread the whole second.
* `idle=0` from `k_thread_runtime_stats_all_get()` is the hard proof that the
  idle thread never ran, i.e. the core never went to sleep.

### Why the periodic line does not print EVENTS_XOSTARTED / XOTUNED

`nrfx_clock_xo_irq_handler()` does `nrf_clock_event_clear()` on every XO event
as it handles it, so polling those registers from the application always reads
back 0 - the driver has already consumed them. They are folded into the
driver's internal `xo_state`, which is what `TUNED` / `NOT-TUNED` reports.

The raw `EVENTS_*` values are still dumped in the boot banner and on failure
paths, because they *are* meaningful when the driver is stuck - e.g. halted in
a debugger while it spins on `EVENTS_XOTUNED`. They are only ever read, never
cleared, so the driver keeps its events.

Do not call `nrfx_clock_xo_tune_error_check()` from the application: it
asserts when an event handler is registered, and Zephyr's clock_control driver
always registers one.

## Notes

* Deferred logging is used on purpose. `CONFIG_LOG_MODE_IMMEDIATE=y` logs
  synchronously from whatever context calls `LOG_*` and is a known source of
  faults on nRF54L.
* `CONFIG_LOG_PROCESS_THREAD_PRIORITY=5` is required: the log thread defaults
  to `K_LOWEST_APPLICATION_THREAD_PRIO`, which is the spinner's priority, and
  would then never get to run.
* Register addresses (secure alias): `CLOCK` @ `0x5010E000`,
  `XO.RUN` +0x408, `XO.STAT` +0x40C, `EVENTS_XOSTARTED` +0x100,
  `EVENTS_XOTUNED` +0x110; `OSCILLATORS.XOSC32M.CONFIG.INTCAP` @ `0x5012071C`.
