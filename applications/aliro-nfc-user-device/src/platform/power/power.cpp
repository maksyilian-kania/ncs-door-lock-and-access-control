/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "power.h"
#include "power_button0.h"
#include "power_button1.h"
#include "power_indicator.h"
#include "power_policy.h"

#include "platform/authorization/authorization_window.h"
#include "platform/nfc/nfc_transport.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/poweroff.h>

LOG_MODULE_REGISTER(aliro_ud_power, CONFIG_ALIRO_UD_POWER_LOG_LEVEL);

namespace AliroUd::Power {
namespace {

Policy sPolicy{};
k_work_delayable sIdleWork{};

/*
 * Monotonic time at which Start() ran (i.e. when this boot became reachable).
 * Used to enforce a time separation between the Button 0 press that woke the
 * DK and the first accepted Button 1 press: without it, that same wake press
 * can cross-trigger the Button 1 handler on this chip family (LATCH
 * ambiguity, see power_button1.cpp) and pin the DK awake forever.
 */
int64_t sBootMs{ 0 };

/*
 * Slack added on top of the authorization window's own remaining time when
 * rescheduling a recheck (see AttemptPowerOff() below): avoids rescheduling
 * exactly at the window's deadline and racing a fresh button-0 press that
 * extends it in the same millisecond.
 */
constexpr uint32_t kWindowRecheckSlackMs{ 50U };

/*
 * Suspends the console before every sys_poweroff() call so no shell/log
 * output is truncated (matches samples/nfc/system_off's pattern; requires
 * CONFIG_PM_DEVICE=y, see prj.conf).
 */
void SuspendConsole()
{
	if (!IS_ENABLED(CONFIG_PM_DEVICE) || !IS_ENABLED(CONFIG_SERIAL)) {
		return;
	}

	const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	if (!device_is_ready(console)) {
		return;
	}

	(void)pm_device_action_run(console, PM_DEVICE_ACTION_SUSPEND);
}

/** @brief The single sys_poweroff() call site: NFC frontend off, awake indicator off, Button 0 armed to wake, console suspended, then power off. */
void DoPowerOff()
{
	LOG_INF("Entering System OFF");

	/* Remove the NFC field wake source: Button 0 is the sole way back from System OFF. */
	AliroUd::Nfc::StopEmulation();
	Indicator::SetAwake(false);
	Button0::ConfigureWakeSense();
	SuspendConsole();

	sys_poweroff();
}

/**
 * @brief Evaluates every pending trigger now and powers off if one is due.
 *
 * If a shutdown is still pending only because the authorization window is
 * open (never because a mutation guard is active - that case is instead
 * resolved by EndMutationGuard() calling this function directly),
 * reschedules a recheck for when that window is expected to expire. There
 * is no other notification path back into this module when Button 0 opens
 * or extends the window, so this is the only way the auto idle-timeout
 * path ever gets a second look after the window closes.
 */
void AttemptPowerOff()
{
	const int64_t now = k_uptime_get();
	const bool windowValid = AliroUd::Authorization::GlobalWindow().IsValid(now);

	if (sPolicy.ShouldPowerOff(now, windowValid)) {
		DoPowerOff();
		return;
	}

	if (sPolicy.IsShutdownPending() && !sPolicy.IsMutationGuardActive() && !sPolicy.IsHoldAwake()) {
		const int64_t remainingMs = AliroUd::Authorization::GlobalWindow().GetRemainingMs(now);
		const uint32_t recheckMs = (remainingMs > 0)
						    ? (static_cast<uint32_t>(remainingMs) + kWindowRecheckSlackMs)
						    : kWindowRecheckSlackMs;
		k_work_reschedule(&sIdleWork, K_MSEC(recheckMs));
	}
}

void IdleWorkHandler(k_work *)
{
	AttemptPowerOff();
}

/**
 * @brief True if this boot resumed from System OFF (a wake reset) rather than
 * a cold power-on/pin reset.
 *
 * Button 0 is the sole System OFF wake source (the NFC frontend is
 * deactivated in DoPowerOff() so an NFC field can no longer wake the DK), so
 * a RESET_LOW_POWER_WAKE reset unambiguously means "Button 0 woke us" and the
 * boot path can open the authorization window with confidence. If the reset
 * cause cannot be read, err on the side of treating it as a wake so the
 * Button 0 wake-to-authorize flow still works.
 */
bool DidWakeFromSystemOff()
{
	uint32_t cause = 0;
	const int err = hwinfo_get_reset_cause(&cause);
	(void)hwinfo_clear_reset_cause();
	if (err != 0) {
		return true;
	}
	return (cause & RESET_LOW_POWER_WAKE) != 0U;
}

} // namespace

void Start()
{
	k_work_init_delayable(&sIdleWork, IdleWorkHandler);
	sBootMs = k_uptime_get();
	Indicator::SetAwake(true);
	Button1::Init();

	/*
	 * Button 0 is the sole wake source and its purpose is
	 * "wake + authorize" in one press. The physical press that woke the DK
	 * is already released by the time this runs, so its GPIO edge cannot be
	 * caught; instead, when this boot resumed from System OFF we open the
	 * authorization window here (equivalent to the Button 0 press having
	 * opened it) and arm the matching auto-sleep for when that window
	 * expires - so a wake that is never followed by a tap returns to System
	 * OFF on its own rather than staying awake forever.
	 */
	if (DidWakeFromSystemOff()) {
		const uint32_t windowMs =
			static_cast<uint32_t>(CONFIG_ALIRO_UD_AUTHORIZATION_WINDOW_SECONDS) * 1000U;
		AliroUd::Authorization::GlobalWindow().Open(sBootMs, windowMs);
		sPolicy.ArmAutoSleep(sBootMs, windowMs + kWindowRecheckSlackMs);
		k_work_reschedule(&sIdleWork, K_MSEC(windowMs + kWindowRecheckSlackMs));
	}
}

void NotifyFieldOn()
{
	sPolicy.NotifyFieldOn();
	k_work_cancel_delayable(&sIdleWork);
}

void NotifyFieldOff()
{
	if (sPolicy.IsHoldAwake()) {
		/* Operator asked to stay awake: field activity must not re-arm auto-sleep. */
		k_work_cancel_delayable(&sIdleWork);
		return;
	}

	sPolicy.NotifyFieldOff(k_uptime_get(),
				static_cast<uint32_t>(CONFIG_ALIRO_UD_SYSTEM_OFF_IDLE_DELAY_S) * 1000U);
	k_work_reschedule(&sIdleWork, K_SECONDS(CONFIG_ALIRO_UD_SYSTEM_OFF_IDLE_DELAY_S));
}

void RequestSleep()
{
	sPolicy.RequestManualSleep();
	AttemptPowerOff();
}

void HandleButton1Press()
{
	/*
	 * Time-separation guard: reject Button 1 presses that arrive too soon
	 * after this boot's wake, so the Button 0 wake press cannot cross-fire
	 * this handler and pin the DK awake indefinitely (see sBootMs above and
	 * power_button1.cpp on the LATCH ambiguity this defends against).
	 */
	const int64_t now = k_uptime_get();
	if ((now - sBootMs) < static_cast<int64_t>(CONFIG_ALIRO_UD_SYSTEM_OFF_BUTTON1_SETTLE_MS)) {
		return;
	}

	if (!sPolicy.IsHoldAwake()) {
		/* First press: hold the DK awake indefinitely (e.g. for provisioning). */
		sPolicy.SetHoldAwake();
		k_work_cancel_delayable(&sIdleWork);
	} else {
		/* Second press: leave hold-awake and power off now. */
		sPolicy.ClearHoldAwake();
		RequestSleep();
	}
}

void BeginMutationGuard()
{
	sPolicy.BeginMutationGuard();
}

void EndMutationGuard()
{
	sPolicy.EndMutationGuard();
	/*
	 * Fires any shutdown that became due while the guard was active
	 * (Button 1 press or an already-elapsed auto idle-timeout) the
	 * instant it is safe to, per docs/system_off_proposal.md: never
	 * ignored, never delayed longer than necessary.
	 */
	AttemptPowerOff();
}

bool IsMutationGuardActive()
{
	return sPolicy.IsMutationGuardActive();
}

bool IsShutdownPending()
{
	return sPolicy.IsShutdownPending();
}

} // namespace AliroUd::Power
