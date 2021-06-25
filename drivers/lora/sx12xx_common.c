/*
 * Copyright (c) 2019 Manivannan Sadhasivam
 * Copyright (c) 2020 Grinn
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <drivers/gpio.h>
#include <drivers/lora.h>
#include <logging/log.h>
#include <zephyr.h>

/* LoRaMac-node specific includes */
#include <radio.h>

#include "sx12xx_common.h"

LOG_MODULE_REGISTER(sx12xx_common, CONFIG_LORA_LOG_LEVEL);

static struct sx12xx_data {
	const struct device *dev;
	struct k_sem data_sem;
	struct k_sem tx_sem;
	RadioEvents_t events;
	struct lora_modem_config tx_cfg;
	lora_recv_cb rx_cb;
	uint8_t *rx_buf;
	uint8_t rx_len;
	int8_t snr;
	int16_t rssi;
} dev_data;

int __sx12xx_configure_pin(const struct device * *dev, const char *controller,
			   gpio_pin_t pin, gpio_flags_t flags)
{
	int err;

	*dev = device_get_binding(controller);
	if (!(*dev)) {
		LOG_ERR("Cannot get pointer to %s device", controller);
		return -EIO;
	}

	err = gpio_pin_configure(*dev, pin, flags);
	if (err) {
		LOG_ERR("Cannot configure gpio %s %d: %d", controller, pin,
			err);
		return err;
	}

	return 0;
}

static void sx12xx_ev_rx_done(uint8_t *payload, uint16_t size, int16_t rssi,
			      int8_t snr)
{
	/* Asynchronous reception */
	if (dev_data.rx_cb) {
		/* Run user callback */
		dev_data.rx_cb(dev_data.dev, payload, size, rssi, snr);
		/* If the callback didn't cancel RX, start the radio again */
		if (dev_data.rx_cb) {
			Radio.Rx(0);
		}
	}
	/* Synchronous reception */
	else {
		Radio.Sleep();

		dev_data.rx_buf = payload;
		dev_data.rx_len = size;
		dev_data.rssi = rssi;
		dev_data.snr = snr;

		k_sem_give(&dev_data.data_sem);
	}
}

static void sx12xx_ev_tx_done(void)
{
	Radio.Sleep();
	k_sem_give(&dev_data.tx_sem);
}

int sx12xx_lora_send(const struct device *dev, uint8_t *data,
		     uint32_t data_len)
{
	uint32_t air_time;
	int rc;

	/* Clear any previous state */
	k_sem_take(&dev_data.tx_sem, K_NO_WAIT);

	Radio.SetMaxPayloadLength(MODEM_LORA, data_len);

	Radio.Send(data, data_len);

	/* Calculate expected airtime of the packet */
	air_time = Radio.TimeOnAir(MODEM_LORA,
				   dev_data.tx_cfg.bandwidth,
				   dev_data.tx_cfg.datarate,
				   dev_data.tx_cfg.coding_rate,
				   dev_data.tx_cfg.preamble_len,
				   0, data_len, true);
	LOG_DBG("Expected airtime of %d bytes = %dms", data_len, air_time);

	/* Wait for the packet to finish transmitting.
	 * The additional wiggle room is provided to ensure that
	 * we are actually detecting a failed transmission, instead of
	 * some minor timing variation between modem and driver.
	 */
	air_time += (air_time >> 3) + 1;
	rc = k_sem_take(&dev_data.tx_sem, K_MSEC(air_time));
	if (rc < 0) {
		LOG_ERR("Packet transmission failed!");
		/* Put radio back to sleep */
		Radio.Sleep();
	}
	return rc;
}

int sx12xx_lora_recv(const struct device *dev, uint8_t *data, uint8_t size,
		     k_timeout_t timeout, int16_t *rssi, int8_t *snr)
{
	int ret;

	/* Validate asynchronous RX not in progress */
	if (dev_data.rx_cb) {
		return -EINVAL;
	}

	Radio.SetMaxPayloadLength(MODEM_LORA, 255);
	Radio.Rx(0);

	ret = k_sem_take(&dev_data.data_sem, timeout);
	if (ret < 0) {
		LOG_INF("Receive timeout");
		/* Manually transition to sleep mode on timeout */
		Radio.Sleep();
		return ret;
	}

	/* Only copy the bytes that can fit the buffer, drop the rest */
	if (dev_data.rx_len > size) {
		dev_data.rx_len = size;
	}

	/*
	 * FIXME: We are copying the global buffer here, so it might get
	 * overwritten inbetween when a new packet comes in. Use some
	 * wise method to fix this!
	 */
	memcpy(data, dev_data.rx_buf, dev_data.rx_len);

	if (rssi != NULL) {
		*rssi = dev_data.rssi;
	}

	if (snr != NULL) {
		*snr = dev_data.snr;
	}

	return dev_data.rx_len;
}

int sx12xx_lora_recv_async(const struct device *dev, lora_recv_cb cb)
{
	/* Handle RX cancel */
	if (!cb) {
		if (dev_data.rx_cb) {
			/* Put radio to sleep */
			Radio.Sleep();
			dev_data.rx_cb = NULL;
		}
		return 0;
	}

	/* Handle reception already running */
	if (dev_data.rx_cb) {
		dev_data.rx_cb = cb;
		return 0;
	}

	/* Store the callback */
	dev_data.rx_cb = cb;

	/* Enable the radio */
	Radio.SetMaxPayloadLength(MODEM_LORA, 255);
	Radio.Rx(0);
	return 0;
}

int sx12xx_lora_config(const struct device *dev,
		       struct lora_modem_config *config)
{
	Radio.SetChannel(config->frequency);

	if (config->tx) {
		/* Store TX config locally for airtime calculations */
		memcpy(&dev_data.tx_cfg, config, sizeof(dev_data.tx_cfg));
		/* Configure radio driver */
		Radio.SetTxConfig(MODEM_LORA, config->tx_power, 0,
				  config->bandwidth, config->datarate,
				  config->coding_rate, config->preamble_len,
				  false, true, 0, 0, false, 4000);
	} else {
		/* TODO: Get symbol timeout value from config parameters */
		Radio.SetRxConfig(MODEM_LORA, config->bandwidth,
				  config->datarate, config->coding_rate,
				  0, config->preamble_len, 10, false, 0,
				  false, 0, 0, false, true);
	}

	return 0;
}

int sx12xx_lora_test_cw(const struct device *dev, uint32_t frequency,
			int8_t tx_power,
			uint16_t duration)
{
	Radio.SetTxContinuousWave(frequency, tx_power, duration);
	return 0;
}

int sx12xx_init(const struct device *dev)
{
	dev_data.dev = dev;

	k_sem_init(&dev_data.data_sem, 0, K_SEM_MAX_LIMIT);
	k_sem_init(&dev_data.tx_sem, 0, 1);

	dev_data.events.TxDone = sx12xx_ev_tx_done;
	dev_data.events.RxDone = sx12xx_ev_rx_done;
	Radio.Init(&dev_data.events);

	/*
	 * Automatically place the radio into sleep mode upon boot.
	 * The required `lora_config` call before transmission or reception
	 * will bring the radio out of sleep mode before it is used. The radio
	 * is automatically placed back into sleep mode upon TX or RX
	 * completion.
	 */
	Radio.Sleep();

	return 0;
}
