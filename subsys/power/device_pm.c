/*
 * Copyright (c) 2018 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <kernel.h>
#include <device.h>
#include <sys/__assert.h>

#define LOG_LEVEL CONFIG_SYS_PM_LOG_LEVEL /* From power module Kconfig */
#include <logging/log.h>
LOG_MODULE_DECLARE(power);

/* Device PM request type */
#define DEVICE_PM_SYNC			(1 << 0)
#define DEVICE_PM_ASYNC			(1 << 1)
#define DEVICE_PM_UNDEFERED		(1 << 2)

static void pm_work_handler(struct k_work *work);

static void device_pm_callback(struct device *dev,
			       int retval, void *context, void *arg)
{
	__ASSERT(retval == 0, "Device set power state failed");

	/* Set the fsm_state */
	if (*((u32_t *)context) == DEVICE_PM_ACTIVE_STATE) {
		atomic_set(&dev->pm->fsm_state,
			   DEVICE_PM_FSM_STATE_ACTIVE);
	} else {
		atomic_set(&dev->pm->fsm_state,
			   DEVICE_PM_FSM_STATE_SUSPENDED);
	}

	/**
	 * Typically this would be submitted as a work queue item.
	 * However because of the above state setting, the implementation
	 * never does anything. To allow undeferred operations to complete
	 * normally, run the function directly here, instead of indirectly
	 * on the system workqueue.
	 */
	pm_work_handler(&dev->pm->work);
}

static void pm_work_handler(struct k_work *work)
{
	struct device_pm *pm = CONTAINER_OF(work,
					struct device_pm, work);
	struct device *dev = pm->dev;
	int ret = 0;
	u8_t pm_state;

	switch (atomic_get(&dev->pm->fsm_state)) {
	case DEVICE_PM_FSM_STATE_ACTIVE:
		if ((atomic_get(&dev->pm->usage) == 0) &&
					dev->pm->enable) {
			atomic_set(&dev->pm->fsm_state,
				   DEVICE_PM_FSM_STATE_SUSPENDING);
			ret = device_set_power_state(dev,
						DEVICE_PM_SUSPEND_STATE,
						device_pm_callback, NULL);
		} else {
			pm_state = DEVICE_PM_ACTIVE_STATE;
			goto fsm_out;
		}
		break;
	case DEVICE_PM_FSM_STATE_SUSPENDED:
		if ((atomic_get(&dev->pm->usage) > 0) ||
					!dev->pm->enable) {
			atomic_set(&dev->pm->fsm_state,
				   DEVICE_PM_FSM_STATE_RESUMING);
			ret = device_set_power_state(dev,
						DEVICE_PM_ACTIVE_STATE,
						device_pm_callback, NULL);
		} else {
			pm_state = DEVICE_PM_SUSPEND_STATE;
			goto fsm_out;
		}
		break;
	case DEVICE_PM_FSM_STATE_SUSPENDING:
	case DEVICE_PM_FSM_STATE_RESUMING:
		/* Do nothing: We are waiting for device_pm_callback() */
		break;
	default:
		LOG_ERR("Invalid FSM state!!\n");
	}

	__ASSERT(ret == 0, "Set Power state error");

	return;

fsm_out:
	k_poll_signal_raise(&dev->pm->signal, pm_state);
}

static int device_pm_request(struct device *dev,
			     u32_t target_state, u32_t pm_flags)
{
	int result, signaled = 0;

	__ASSERT((target_state == DEVICE_PM_ACTIVE_STATE) ||
			(target_state == DEVICE_PM_SUSPEND_STATE),
			"Invalid device PM state requested");

	if (target_state == DEVICE_PM_ACTIVE_STATE) {
		if (atomic_inc(&dev->pm->usage) < 0) {
			return 0;
		}
	} else {
		if (atomic_dec(&dev->pm->usage) > 1) {
			return 0;
		}
	}

	if ((pm_flags & DEVICE_PM_SYNC) && (&k_sys_work_q.thread == _current)) {
		LOG_WRN("sync pm operation within system workqueue");
		/* Swap to undeferred to avoid deadlock */
		pm_flags = DEVICE_PM_UNDEFERED;
	}

	if (pm_flags & DEVICE_PM_UNDEFERED) {
		/* Perform the request immediately instead of defering to
		 * the system workqueue.
		 */
		pm_work_handler(&dev->pm->work);
		k_poll_signal_check(&dev->pm->signal, &signaled, &result);
		dev->pm->event.state = K_POLL_STATE_NOT_READY;
		k_poll_signal_reset(&dev->pm->signal);

		__ASSERT(signaled, "Work did not raise signal");
		return result == target_state ? 0 : -EIO;
	}

	k_work_submit(&dev->pm->work);

	/* Return in case of Async request */
	if (pm_flags & DEVICE_PM_ASYNC) {
		return 0;
	}

	/* Incase of Sync request wait for completion event */
	do {
		(void)k_poll(&dev->pm->event, 1, K_FOREVER);
		k_poll_signal_check(&dev->pm->signal,
						&signaled, &result);
	} while (!signaled);

	dev->pm->event.state = K_POLL_STATE_NOT_READY;
	k_poll_signal_reset(&dev->pm->signal);


	return result == target_state ? 0 : -EIO;
}

int device_pm_get(struct device *dev)
{
	return device_pm_request(dev,
			DEVICE_PM_ACTIVE_STATE, DEVICE_PM_ASYNC);
}

int device_pm_get_sync(struct device *dev)
{
	return device_pm_request(dev,
			DEVICE_PM_ACTIVE_STATE, DEVICE_PM_SYNC);
}

int device_pm_get_undefered(struct device *dev)
{
	return device_pm_request(dev,
			DEVICE_PM_ACTIVE_STATE, DEVICE_PM_UNDEFERED);
}

int device_pm_put(struct device *dev)
{
	return device_pm_request(dev,
			DEVICE_PM_SUSPEND_STATE, DEVICE_PM_ASYNC);
}

int device_pm_put_sync(struct device *dev)
{
	return device_pm_request(dev,
			DEVICE_PM_SUSPEND_STATE, DEVICE_PM_SYNC);
}

int device_pm_put_undefered(struct device *dev)
{
	return device_pm_request(dev,
			DEVICE_PM_SUSPEND_STATE, DEVICE_PM_UNDEFERED);
}

void device_pm_enable(struct device *dev)
{
	k_sem_take(&dev->pm->lock, K_FOREVER);
	dev->pm->enable = true;

	/* During the driver init, device can set the
	 * PM state accordingly. For later cases we need
	 * to check the usage and set the device PM state.
	 */
	if (!dev->pm->dev) {
		dev->pm->dev = dev;
		atomic_set(&dev->pm->fsm_state,
			   DEVICE_PM_FSM_STATE_SUSPENDED);
		k_work_init(&dev->pm->work, pm_work_handler);
	} else {
		k_work_submit(&dev->pm->work);
	}
	k_sem_give(&dev->pm->lock);
}

void device_pm_disable(struct device *dev)
{
	k_sem_take(&dev->pm->lock, K_FOREVER);
	dev->pm->enable = false;
	/* Bring up the device before disabling the Idle PM */
	k_work_submit(&dev->pm->work);
	k_sem_give(&dev->pm->lock);
}
