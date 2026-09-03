/*
 * nRF54L15 test application:
 *   1. The external 32 MHz crystal (HFXO) is started and kept running forever.
 *   2. The CPU never enters idle / low power (the idle thread is never scheduled,
 *      so WFI is never executed).
 *   3. Status is printed continuously over SEGGER RTT.
 *
 * Build: west build -b nrf54l15dk/nrf54l15/cpuapp
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/onoff.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_clock.h>
#include <nrfx_clock_xo.h>

LOG_MODULE_REGISTER(hfxo_test, LOG_LEVEL_INF);

#define REPORT_PERIOD_MS 1000U

#define CLOCK_DEV DEVICE_DT_GET(DT_NODELABEL(clock))

/* -------------------------------------------------------------------------
 * 1. HFXO: request the high-accuracy HF clock and never release it.
 *
 * On nRF54L the "HF" subsystem of the nordic,nrf-clock driver is the crystal
 * oscillator (TASKS_XOSTART). Taking one onoff reference and never dropping it
 * keeps HFXO running for the lifetime of the application.
 * -------------------------------------------------------------------------
 */
static struct onoff_client hfxo_cli;

static int hfxo_request_forever(void)
{
	struct onoff_manager *mgr;
	int err;
	int res;

	mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (mgr == NULL) {
		LOG_ERR("No onoff manager for HF clock");
		return -ENODEV;
	}

	sys_notify_init_spinwait(&hfxo_cli.notify);

	err = onoff_request(mgr, &hfxo_cli);
	if (err < 0) {
		LOG_ERR("onoff_request() failed: %d", err);
		return err;
	}

	do {
		err = sys_notify_fetch_result(&hfxo_cli.notify, &res);
		if (err == 0 && res != 0) {
			LOG_ERR("HFXO could not be started: %d", res);
			return res;
		}
	} while (err == -EAGAIN);

	if (err != 0) {
		LOG_ERR("sys_notify_fetch_result() failed: %d", err);
		return err;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * 2. Never idle: a thread at the lowest application priority that spins.
 *
 * Every other thread (main, the log processing thread, workqueues) has a
 * higher priority and preempts it, but the idle thread sits below all
 * application priorities, so it is never selected. The CPU therefore always
 * has a runnable thread and never executes WFI.
 * -------------------------------------------------------------------------
 */
#define SPINNER_STACK_SIZE 512

static volatile uint32_t spin_count;

static void spinner_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		spin_count++;
		/* Keeps the loop from being optimised away and stops the
		 * compiler from turning it into a tight no-side-effect loop.
		 */
		__asm__ volatile("nop" ::: "memory");
	}
}

K_THREAD_DEFINE(spinner_tid, SPINNER_STACK_SIZE, spinner_fn, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

/* -------------------------------------------------------------------------
 * 3. Reporting
 * -------------------------------------------------------------------------
 */
static const char *clk_status_str(enum clock_control_status s)
{
	switch (s) {
	case CLOCK_CONTROL_STATUS_STARTING:
		return "STARTING";
	case CLOCK_CONTROL_STATUS_OFF:
		return "OFF";
	case CLOCK_CONTROL_STATUS_ON:
		return "ON";
	default:
		return "UNKNOWN";
	}
}

static bool hfxo_is_running(void)
{
	return ((NRF_CLOCK->XO.STAT & CLOCK_XO_STAT_STATE_Msk) >>
		CLOCK_XO_STAT_STATE_Pos) == CLOCK_XO_STAT_STATE_Running;
}

/* Is the crystal tuned, according to the nrfx clock driver?
 *
 * The EVENTS_XOTUNED / EVENTS_XOTUNEFAILED flags themselves cannot be polled
 * from here: nrfx_clock_xo_irq_handler() clears every XO event as it handles
 * it, so they read back as 0 within microseconds. The driver folds them into
 * its own xo_state instead, and that is what this exposes. (Do not call
 * nrfx_clock_xo_tune_error_check() - it asserts when an event handler is
 * registered, which Zephyr's clock_control driver always does.)
 */
static const char *hfxo_tune_str(void)
{
#if NRFX_CHECK(NRF_CLOCK_HAS_XO_TUNE)
	return nrfx_clock_xo_tune_status_check() ? "TUNED" : "NOT-TUNED";
#else
	return "n/a";
#endif
}

/* Raw HFXO registers, for the boot banner and for failure paths only.
 *
 * Read-only on purpose: the EVENTS_* flags belong to the nrfx clock driver,
 * which clears them in its ISR, so clearing them here would steal events.
 * A 0 in any EVENTS_* below is therefore normal - it means the driver already
 * consumed the event, not that the event never happened. They are only
 * meaningful when the driver is stuck (e.g. halted in a debugger while it
 * spins on EVENTS_XOTUNED).
 */
static void hfxo_dump_registers(void)
{
	LOG_INF("  CLOCK.XO.RUN         = %u", NRF_CLOCK->XO.RUN);
	LOG_INF("  CLOCK.XO.STAT        = 0x%08x (%s)", NRF_CLOCK->XO.STAT,
		hfxo_is_running() ? "Running" : "NotRunning");
	LOG_INF("  EVENTS_XOSTARTED     = %u", NRF_CLOCK->EVENTS_XOSTARTED);
	LOG_INF("  EVENTS_XOTUNED       = %u", NRF_CLOCK->EVENTS_XOTUNED);
	LOG_INF("  EVENTS_XOTUNEERROR   = %u", NRF_CLOCK->EVENTS_XOTUNEERROR);
	LOG_INF("  EVENTS_XOTUNEFAILED  = %u", NRF_CLOCK->EVENTS_XOTUNEFAILED);
	LOG_INF("  XOSC32M.CONFIG.INTCAP= 0x%02x",
		NRF_OSCILLATORS->XOSC32M.CONFIG.INTCAP);
}

int main(void)
{
	uint32_t prev_spin;
	uint32_t seq = 0;
	int err;

	LOG_INF("=== nRF54L15 HFXO always-on / never-idle / RTT test ===");
	LOG_INF("HFXO from DT: %u Hz, startup-time %u us",
		DT_PROP(DT_NODELABEL(hfxo), clock_frequency),
		DT_PROP(DT_NODELABEL(hfxo), startup_time_us));
	LOG_INF("Spinner thread priority %d (idle thread must never run)",
		K_LOWEST_APPLICATION_THREAD_PRIO);

	err = hfxo_request_forever();
	if (err != 0) {
		LOG_ERR("HFXO start FAILED (%d) - dumping registers", err);
		hfxo_dump_registers();
		return err;
	}

	LOG_INF("HFXO started and permanently requested, tune state: %s",
		hfxo_tune_str());
	hfxo_dump_registers();

	prev_spin = spin_count;

	while (true) {
		k_thread_runtime_stats_t stats;
		uint32_t spin_now;
		uint32_t spin_delta;
		bool running;

		k_msleep(REPORT_PERIOD_MS);

		spin_now = spin_count;
		spin_delta = spin_now - prev_spin;
		prev_spin = spin_now;

		running = hfxo_is_running();

		LOG_INF("#%u up=%llu ms | HFXO=%s %s (XO.RUN=%u XO.STAT=0x%08x) "
			"drv=%s | spin=%u loops/s",
			++seq, k_uptime_get(), running ? "RUNNING" : "STOPPED",
			hfxo_tune_str(), NRF_CLOCK->XO.RUN, NRF_CLOCK->XO.STAT,
			clk_status_str(clock_control_get_status(
				CLOCK_DEV, CLOCK_CONTROL_NRF_SUBSYS_HF)),
			spin_delta);

		if (!running) {
			LOG_ERR("HFXO is NOT running any more!");
			hfxo_dump_registers();
		}

#if NRFX_CHECK(NRF_CLOCK_HAS_XO_TUNE)
		if (!nrfx_clock_xo_tune_status_check()) {
			LOG_ERR("HFXO is not tuned - check load capacitors / "
				"INTCAP (0x%02x)",
				NRF_OSCILLATORS->XOSC32M.CONFIG.INTCAP);
		}
#endif

		if (k_thread_runtime_stats_all_get(&stats) == 0) {
			LOG_INF("   cpu cycles: total=%llu idle=%llu "
				"(idle must stay 0)",
				stats.execution_cycles, stats.idle_cycles);
		}
	}

	return 0;
}
