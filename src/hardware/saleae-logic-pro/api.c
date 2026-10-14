/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2017 Jan Luebbe <jluebbe@lasnet.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <glib.h>
#include <string.h>
#include "protocol.h"

#define BUF_TIMEOUT 1000

static const uint32_t scanopts[] = {
	SR_CONF_CONN,
};

static const uint32_t drvopts[] = {
	SR_CONF_LOGIC_ANALYZER,
};

static const uint32_t devopts[] = {
	SR_CONF_CONTINUOUS,
	SR_CONF_CONN | SR_CONF_GET,
	SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_LIMIT_MSEC | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_TRIGGER_MATCH | SR_CONF_LIST,
	SR_CONF_CAPTURE_RATIO | SR_CONF_GET | SR_CONF_SET,
};

static const int32_t trigger_matches[] = {
	SR_TRIGGER_ZERO,
	SR_TRIGGER_ONE,
	SR_TRIGGER_RISING,
	SR_TRIGGER_FALLING,
	SR_TRIGGER_EDGE,
};

static const char *channel_names[] = {
	"0", "1", "2", "3", "4", "5", "6", "7",
	"8", "9", "10", "11", "12", "13", "14", "15",
};

static const struct saleae_logic_pro_profile profiles[] = {
	{ 0x1005, "Logic Pro 8", 8, 8,
	  "saleae-logicpro8-logic2-fx3.fw",
	  "saleae-logicpro8-logic2-fpga.bitstream", TRUE },
	{ 0x1006, "Logic Pro 16", 16, 0,
	  "saleae-logicpro16-fx3.fw", "saleae-logicpro16-fpga.bitstream",
	  FALSE },
};

static const struct saleae_logic_pro_profile *find_profile(uint16_t vid, uint16_t pid)
{
	unsigned int i;

	if (vid != 0x21a9)
		return NULL;
	for (i = 0; i < ARRAY_SIZE(profiles); i++)
		if (profiles[i].pid == pid)
			return &profiles[i];

	return NULL;
}

/*
 * Digital rates as offered by Logic 2 (500 MHz / (5 * n) with 8-bit n, or
 * 500/250/125 MHz), plus the analog rates; see saleae_logic_pro_prepare().
 * Rates marked (a) also allow analog channels.
 */
static const uint64_t samplerates[] = {
	SR_HZ(781250),		/* (a) */
	SR_MHZ(1),
	SR_HZ(1562500),		/* (a) */
	SR_MHZ(2),
	SR_KHZ(2500),
	SR_HZ(3125000),		/* (a) */
	SR_MHZ(4),
	SR_MHZ(5),
	SR_HZ(6250000),		/* (a) */
	SR_MHZ(10),
	SR_HZ(12500000),	/* (a) */
	SR_MHZ(20),
	SR_MHZ(25),
	SR_MHZ(50),		/* (a) */
	SR_MHZ(100),
	SR_MHZ(125),
	SR_MHZ(250),
	SR_MHZ(500),
};

#define FW_HEADER_SIZE 7
#define FW_MAX_PART_SIZE (4 * 1024)

static int upload_firmware(struct sr_context *ctx, libusb_device *dev, const char *name)
{
	struct libusb_device_handle *hdl = NULL;
	unsigned char *firmware = NULL;
	int ret = SR_ERR;
	size_t fw_size, fw_offset = 0;
	uint32_t part_address = 0;
	uint16_t part_size = 0;
	uint8_t part_final = 0;

	firmware = sr_resource_load(ctx, SR_RESOURCE_FIRMWARE,
				    name, &fw_size, 256 * 1024);
	if (!firmware)
		goto out;

	sr_info("Uploading firmware '%s'.", name);

	if (libusb_open(dev, &hdl) != 0)
		goto out;

	while ((fw_offset + FW_HEADER_SIZE) <= fw_size) {
		part_size = GUINT16_FROM_LE(*(uint16_t*)(firmware + fw_offset));
		part_address = GUINT32_FROM_LE(*(uint32_t*)(firmware + fw_offset + 2));
		part_final = *(uint8_t*)(firmware + fw_offset + 6);
		if (part_size > FW_MAX_PART_SIZE) {
			sr_err("Part too large (%d).", part_size);
			goto out;
		}
		fw_offset += FW_HEADER_SIZE;
		if ((fw_offset + part_size) > fw_size) {
			sr_err("Truncated firmware file.");
			goto out;
		}
		ret = libusb_control_transfer(hdl, LIBUSB_REQUEST_TYPE_VENDOR |
					      LIBUSB_ENDPOINT_OUT, 0xa0,
					      part_address & 0xffff, part_address >> 16,
					      firmware + fw_offset, part_size,
					      100);
		if (ret < 0) {
			sr_err("Unable to send firmware to device: %s.",
			       libusb_error_name(ret));
			ret = SR_ERR;
			goto out;
		}
		if (part_size)
			sr_spew("Uploaded %d bytes.", part_size);
		else
			sr_info("Started firmware at 0x%x.", part_address);
		fw_offset += part_size;
	}

	if ((!part_final) || (part_size != 0)) {
		sr_err("Missing final part.");
		goto out;
	}

	ret = SR_OK;

	sr_info("Firmware upload done.");

 out:
	if (hdl)
		libusb_close(hdl);

	g_free(firmware);

	return ret;
}

static gboolean scan_firmware(libusb_device *dev)
{
	struct libusb_device_descriptor des;
	struct libusb_device_handle *hdl;
	gboolean ret;
	unsigned char strdesc[64];

	hdl = NULL;
	ret = FALSE;

	libusb_get_device_descriptor(dev, &des);

	if (libusb_open(dev, &hdl) != 0)
		goto out;

	if (libusb_get_string_descriptor_ascii(hdl,
		des.iManufacturer, strdesc, sizeof(strdesc)) < 0)
		goto out;
	if (strcmp((const char *)strdesc, "Saleae"))
		goto out;

	if (libusb_get_string_descriptor_ascii(hdl,
		des.iProduct, strdesc, sizeof(strdesc)) < 0)
		goto out;
	if (strcmp((const char *)strdesc, "Logic Pro"))
		goto out;

	ret = TRUE;

out:
	if (hdl)
		libusb_close(hdl);

	return ret;
}

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	struct drv_context *drvc;
	struct dev_context *devc;
	struct sr_dev_inst *sdi;
	GSList *devices, *conn_devices;
	libusb_device **devlist;
	struct libusb_device_descriptor des;
	const char *conn;
	char connection_id[64];
	const struct saleae_logic_pro_profile *prof;
	gboolean fw_loaded = FALSE;

	devices = NULL;
	conn_devices = NULL;
	drvc = di->context;
	drvc->instances = NULL;

	conn = NULL;
	for (GSList *l = options; l; l = l->next) {
		struct sr_config *src = l->data;

		switch (src->key) {
		case SR_CONF_CONN:
			conn = g_variant_get_string(src->data, NULL);
			break;
		}
	}

	libusb_get_device_list(drvc->sr_ctx->libusb_ctx, &devlist);
	for (unsigned int i = 0; devlist[i]; i++) {
		libusb_get_device_descriptor(devlist[i], &des);

		if (!(prof = find_profile(des.idVendor, des.idProduct)))
			continue;

		if (!scan_firmware(devlist[i])) {
			sr_info("Found a %s device (no firmware loaded).",
				prof->model);
			if (upload_firmware(drvc->sr_ctx, devlist[i],
					    prof->fw_name) != SR_OK) {
				sr_err("Firmware upload failed, name %s.",
				       prof->fw_name);
				continue;
			};
			fw_loaded = TRUE;
		}

	}
	if (fw_loaded) {
		/* Wait (up to 5 s) for the device to re-enumerate. */
		for (unsigned int t = 0; t < 50; t++) {
			gboolean back = FALSE;

			libusb_free_device_list(devlist, 1);
			g_usleep(100 * 1000);
			libusb_get_device_list(drvc->sr_ctx->libusb_ctx, &devlist);
			for (unsigned int i = 0; devlist[i]; i++) {
				libusb_get_device_descriptor(devlist[i], &des);
				if (find_profile(des.idVendor, des.idProduct) &&
				    scan_firmware(devlist[i]))
					back = TRUE;
			}
			if (back)
				break;
		}
	}
	if (conn)
		conn_devices = sr_usb_find(drvc->sr_ctx->libusb_ctx, conn);
	for (unsigned int i = 0; devlist[i]; i++) {
		if (conn_devices) {
			struct sr_usb_dev_inst *usb = NULL;
			GSList *l;

			for (l = conn_devices; l; l = l->next) {
				usb = l->data;
				if (usb->bus == libusb_get_bus_number(devlist[i])
				    && usb->address == libusb_get_device_address(devlist[i]))
					break;
			}
			if (!l)
				/* This device matched none of the ones that
				 * matched the conn specification. */
				continue;
		}

		libusb_get_device_descriptor(devlist[i], &des);

		if (!(prof = find_profile(des.idVendor, des.idProduct)))
			continue;

		if (usb_get_port_path(devlist[i], connection_id, sizeof(connection_id)) < 0)
			continue;

		sdi = g_malloc0(sizeof(struct sr_dev_inst));
		sdi->status = SR_ST_INITIALIZING;
		sdi->vendor = g_strdup("Saleae");
		sdi->model = g_strdup(prof->model);
		sdi->connection_id = g_strdup(connection_id);

		for (unsigned int j = 0; j < prof->num_channels; j++)
			sr_channel_new(sdi, j, SR_CHANNEL_LOGIC, TRUE,
				       channel_names[j]);
		for (unsigned int j = 0; j < prof->num_analog; j++) {
			char name[16];
			snprintf(name, sizeof(name), "A%u", j);
			sr_channel_new(sdi, prof->num_channels + j,
				       SR_CHANNEL_ANALOG, FALSE, name);
		}

		sr_dbg("Found a %s device.", prof->model);
		sdi->status = SR_ST_INACTIVE;
		sdi->inst_type = SR_INST_USB;
		sdi->conn = sr_usb_dev_inst_new(libusb_get_bus_number(devlist[i]),
						libusb_get_device_address(devlist[i]), NULL);

		devc = g_malloc0(sizeof(struct dev_context));
		devc->profile = prof;
		sr_sw_limits_init(&devc->limits);
		sdi->priv = devc;
		devices = g_slist_append(devices, sdi);

	}
	g_slist_free_full(conn_devices, (GDestroyNotify)sr_usb_dev_inst_free);
	libusb_free_device_list(devlist, 1);

	return std_scan_complete(di, devices);
}

static int dev_open(struct sr_dev_inst *sdi)
{
	struct sr_dev_driver *di = sdi->driver;
	struct drv_context *drvc = di->context;
	struct dev_context *devc = sdi->priv;
	struct sr_usb_dev_inst *usb = sdi->conn;
	int ret;

	if (sr_usb_open(drvc->sr_ctx->libusb_ctx, usb) != SR_OK)
		return SR_ERR;

	if ((ret = libusb_claim_interface(usb->devhdl, 0))) {
		sr_err("Failed to claim interface: %s.", libusb_error_name(ret));
		return SR_ERR;
	}

	/*
	 * Like the vendor software, clear any halt left by a previous
	 * (possibly aborted) session.
	 */
	libusb_clear_halt(usb->devhdl, 1 | LIBUSB_ENDPOINT_IN);

	/* Configure default samplerate. */
	if (devc->dig_samplerate == 0)
		devc->dig_samplerate = SR_MHZ(10);

	return saleae_logic_pro_init(sdi);
}

static int dev_close(struct sr_dev_inst *sdi)
{
	sr_usb_close(sdi->conn);

	return SR_OK;
}

static int config_get(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct sr_usb_dev_inst *usb;
	struct dev_context *devc;

	(void)cg;

	switch (key) {
	case SR_CONF_CONN:
		if (!sdi || !sdi->conn)
			return SR_ERR_ARG;
		usb = sdi->conn;
		*data = g_variant_new_printf("%d.%d", usb->bus, usb->address);
		break;
	case SR_CONF_SAMPLERATE:
		if (!sdi)
			return SR_ERR;
		devc = sdi->priv;
		*data = g_variant_new_uint64(devc->dig_samplerate);
		break;
	case SR_CONF_LIMIT_SAMPLES:
	case SR_CONF_LIMIT_MSEC:
		if (!sdi)
			return SR_ERR;
		devc = sdi->priv;
		return sr_sw_limits_config_get(&devc->limits, key, data);
	case SR_CONF_CAPTURE_RATIO:
		if (!sdi)
			return SR_ERR;
		devc = sdi->priv;
		*data = g_variant_new_uint64(devc->capture_ratio);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_set(uint32_t key, GVariant *data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;

	(void)cg;

	devc = sdi->priv;

	switch (key) {
	case SR_CONF_SAMPLERATE:
		devc->dig_samplerate = g_variant_get_uint64(data);
		break;
	case SR_CONF_LIMIT_SAMPLES:
	case SR_CONF_LIMIT_MSEC:
		return sr_sw_limits_config_set(&devc->limits, key, data);
	case SR_CONF_CAPTURE_RATIO:
		devc->capture_ratio = g_variant_get_uint64(data);
		if (devc->capture_ratio > 100)
			return SR_ERR_ARG;
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_list(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	switch (key) {
	case SR_CONF_SCAN_OPTIONS:
	case SR_CONF_DEVICE_OPTIONS:
		return STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, devopts);
	case SR_CONF_SAMPLERATE:
		*data = std_gvar_samplerates(ARRAY_AND_SIZE(samplerates));
		break;
	case SR_CONF_TRIGGER_MATCH:
		*data = std_gvar_array_i32(ARRAY_AND_SIZE(trigger_matches));
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

/* Can the hardware capture at this rate with these channels? */
static gboolean samplerate_ok(const struct dev_context *devc, uint64_t rate,
			      unsigned int n_dig, unsigned int n_ana)
{
	uint64_t div, d;

	if (!rate || SR_MHZ(500) % rate)
		return FALSE;
	div = SR_MHZ(500) / rate;
	if (!(div == 1 ||
	      (devc->profile->logic2_init && (div == 2 || div == 4)) ||
	      (div % 5 == 0 &&
	       div / 5 <= (devc->profile->logic2_init ? 0xff : 0xffff))))
		return FALSE;
	if (n_ana) {
		if (SR_MHZ(50) % rate)
			return FALSE;
		d = SR_MHZ(50) / rate;
		if (d != 1 && d != 4 && d != 8 && d != 16 && d != 32 && d != 64)
			return FALSE;
	}

	return rate * (n_dig + 12 * n_ana) <= MAX_BANDWIDTH_BPS;
}

/*
 * Front ends start from any listed samplerate, but analog channels and the
 * USB bandwidth restrict it. Instead of failing, use the closest possible
 * rate (the highest one not above the requested rate), and tell the session
 * via a meta packet. Returns TRUE if the rate was changed.
 */
static gboolean coerce_samplerate(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	const struct sr_channel *ch;
	const GSList *l;
	unsigned int n_dig = 0, n_ana = 0, i;
	uint64_t want, best = 0, lowest = 0;
	char *a, *b;

	for (l = sdi->channels; l; l = l->next) {
		ch = l->data;
		if (!ch->enabled)
			continue;
		if (ch->type == SR_CHANNEL_ANALOG)
			n_ana++;
		else
			n_dig++;
	}

	want = devc->dig_samplerate;
	if (samplerate_ok(devc, want, n_dig, n_ana))
		return FALSE;

	for (i = 0; i < ARRAY_SIZE(samplerates); i++) {
		if (!samplerate_ok(devc, samplerates[i], n_dig, n_ana))
			continue;
		if (!lowest)
			lowest = samplerates[i];
		if (samplerates[i] <= want)
			best = samplerates[i];
	}
	if (!best)
		best = lowest;
	if (!best)
		return FALSE;	/* Nothing fits; prepare() reports it. */

	a = sr_samplerate_string(want);
	b = sr_samplerate_string(best);
	sr_warn("%s is not possible with %u digital and %u analog channels, "
		"using %s.", a, n_dig, n_ana, b);
	g_free(a);
	g_free(b);
	devc->dig_samplerate = best;

	return TRUE;
}

static void free_buffers(struct dev_context *devc)
{
	unsigned int i;

	g_free(devc->logic_buf);
	devc->logic_buf = NULL;
	g_free(devc->work);
	devc->work = NULL;
	for (i = 0; i < MAX_ANALOG_CHANNELS; i++) {
		g_free(devc->ana_buf[i]);
		devc->ana_buf[i] = NULL;
	}
}

static void clear_helper(struct dev_context *devc)
{
	g_free(devc->cal_json);
}

static int dev_clear(const struct sr_dev_driver *di)
{
	return std_dev_clear_with_callback(di, (std_dev_clear_callback)clear_helper);
}

static void dev_acquisition_abort(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	unsigned int i;

	for (i = 0; i < devc->num_transfers; i++) {
		if (devc->transfers[i])
			libusb_cancel_transfer(devc->transfers[i]);
	}
}

static int dev_acquisition_handle(int fd, int revents, void *cb_data)
{
	struct sr_dev_inst *sdi = cb_data;
	struct dev_context *devc = sdi->priv;
	struct drv_context *drvc = sdi->driver->context;
	struct timeval tv = ALL_ZERO;

	(void)fd;

	libusb_handle_events_timeout(drvc->sr_ctx->libusb_ctx, &tv);

	/* Handle timeout or reached limits. */
	if (!revents || devc->acq_done) {
		sr_dbg("Stopping: %s.", devc->acq_done ? "done" : "timeout");
		sr_dev_acquisition_stop(sdi);
	}

	return TRUE;
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	struct drv_context *drvc = sdi->driver->context;
	struct libusb_transfer *transfer;
	struct sr_usb_dev_inst *usb;
	struct sr_trigger *trigger;
	uint8_t *buf;
	unsigned int i, ret;
	uint64_t num_xfers;
	gboolean rate_changed;

	rate_changed = coerce_samplerate(sdi);

	ret = saleae_logic_pro_prepare(sdi);
	if (ret != SR_OK)
		return ret;

	usb = sdi->conn;

	/* The stream endpoint may be halted after an aborted session. */
	libusb_clear_halt(usb->devhdl, 2 | LIBUSB_ENDPOINT_IN);

	/*
	 * Conversion buffers: one transfer plus carried-over words, at most
	 * one sample period per frame sample.
	 */
	/* ~10 ms of data per transfer, a multiple of 16 KiB. */
	devc->xfer_size = devc->stream_bps / 8 / 100;
	devc->xfer_size = CLAMP(devc->xfer_size, XFER_MIN_SIZE, XFER_MAX_SIZE);
	devc->xfer_size -= devc->xfer_size % XFER_MIN_SIZE;
	/* Enough queued transfers for ~0.4 s. */
	num_xfers = devc->stream_bps / 8 * 4 / 10 / devc->xfer_size;
	num_xfers = CLAMP(num_xfers, XFER_MIN_COUNT, XFER_MAX_COUNT);

	devc->buf_samples = ((devc->xfer_size / 4 + MAX_FRAME_WORDS) /
		(devc->frame_dig_words + devc->frame_ana_words) + 1) *
		devc->frame_samples;
	/* Plus room for the logic delay line (analog filter group delay). */
	devc->logic_buf = g_malloc0((devc->buf_samples + MAX_GROUP_DELAY) *
				    sizeof(uint16_t));
	devc->work = g_malloc(devc->xfer_size + MAX_FRAME_WORDS * sizeof(uint32_t));
	for (i = 0; i < devc->ana_channel_cnt; i++)
		devc->ana_buf[i] = g_malloc0(devc->buf_samples * sizeof(float));
	devc->acq_done = FALSE;
	devc->submitted_transfers = 0;
	sr_sw_limits_acquisition_start(&devc->limits);

	/* Saleae hardware has no trigger unit; triggers are done on the host. */
	devc->unitsize = logic_channel_unitsize(sdi->channels);
	devc->trigger_fired = TRUE;
	devc->stl = NULL;
	if ((trigger = sr_session_trigger_get(sdi->session))) {
		int pre_trigger_samples = 0;
		if (!devc->dig_channel_cnt) {
			sr_err("Triggers need at least one digital channel.");
			free_buffers(devc);
			return SR_ERR_ARG;
		}
		if (devc->limits.limit_samples)
			pre_trigger_samples = devc->capture_ratio *
				devc->limits.limit_samples / 100;
		if (pre_trigger_samples && devc->ana_channel_cnt) {
			sr_warn("Pre-trigger data is not supported with analog "
				"channels; capture starts at the trigger.");
			pre_trigger_samples = 0;
		}
		devc->stl = soft_trigger_logic_new(sdi, trigger,
						   pre_trigger_samples);
		if (!devc->stl) {
			free_buffers(devc);
			return SR_ERR_MALLOC;
		}
		devc->trigger_fired = FALSE;
	}

	devc->num_transfers = num_xfers;
	devc->transfers = g_malloc0(sizeof(*devc->transfers) * num_xfers);
	for (i = 0; i < devc->num_transfers; i++) {
		buf = g_malloc(devc->xfer_size);
		transfer = libusb_alloc_transfer(0);
		libusb_fill_bulk_transfer(transfer, usb->devhdl,
			2 | LIBUSB_ENDPOINT_IN, buf, devc->xfer_size,
			saleae_logic_pro_receive_data, (void *)sdi, 0);
		if ((ret = libusb_submit_transfer(transfer)) != 0) {
			sr_err("Failed to submit transfer: %s.",
			       libusb_error_name(ret));
			libusb_free_transfer(transfer);
			g_free(buf);
			dev_acquisition_abort(sdi);
			return SR_ERR;
		}
		devc->transfers[i] = transfer;
		devc->submitted_transfers++;
	}

	usb_source_add(sdi->session, drvc->sr_ctx, BUF_TIMEOUT, dev_acquisition_handle, (void *)sdi);

	std_session_send_df_header(sdi);
	if (rate_changed)
		sr_session_send_meta(sdi, SR_CONF_SAMPLERATE,
				     g_variant_new_uint64(devc->dig_samplerate));

	saleae_logic_pro_start(sdi);
	if (ret != SR_OK)
		return ret;

	return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	struct drv_context *drvc = sdi->driver->context;
	struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
	unsigned int i;

	/* Already stopped (e.g. limits reached, then stopped by the session). */
	if (!devc->transfers)
		return SR_OK;

	devc->acq_done = TRUE;

	saleae_logic_pro_stop(sdi);

	/* Cancel and reap all streaming transfers before freeing buffers. */
	dev_acquisition_abort(sdi);
	for (i = 0; devc->submitted_transfers && i < 50; i++)
		libusb_handle_events_timeout(drvc->sr_ctx->libusb_ctx, &tv);
	if (devc->submitted_transfers)
		sr_warn("%u transfers still pending.", devc->submitted_transfers);

	std_session_send_df_end(sdi);

	usb_source_remove(sdi->session, drvc->sr_ctx);

	g_free(devc->transfers);
	devc->transfers = NULL;
	devc->num_transfers = 0;
	free_buffers(devc);
	if (devc->stl) {
		soft_trigger_logic_free(devc->stl);
		devc->stl = NULL;
	}

	return SR_OK;
}

static struct sr_dev_driver saleae_logic_pro_driver_info = {
	.name = "saleae-logic-pro",
	.longname = "Saleae Logic Pro",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = dev_clear,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = dev_open,
	.dev_close = dev_close,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = dev_acquisition_stop,
	.context = NULL,
};
SR_REGISTER_DEV_DRIVER(saleae_logic_pro_driver_info);
