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
#include <string.h>
#include "protocol.h"

#define COMMAND_START_CAPTURE	0x01
#define COMMAND_STOP_CAPTURE	0x02
#define COMMAND_READ_EEPROM	0x07
#define COMMAND_INIT_BITSTREAM  0x7e
#define COMMAND_SEND_BITSTREAM  0x7f
#define COMMAND_WRITE_REG	0x80
#define COMMAND_READ_REG	0x81
#define COMMAND_READ_TEMP	0x86
#define COMMAND_WRITE_I2C	0x87
#define COMMAND_READ_I2C	0x88
#define COMMAND_WAKE_I2C	0x89
#define COMMAND_READ_FW_VER	0x8b

#define REG_ADC_IDX		0x03
#define REG_ADC_VAL_LSB		0x04
#define REG_ADC_VAL_MSB		0x05
#define REG_LED_RED		0x0f
#define REG_LED_GREEN		0x10
#define REG_LED_BLUE		0x11
#define REG_STATUS		0x40


static void iterate_lfsr(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	uint32_t lfsr = devc->lfsr;
	int i, max;

	max = (lfsr & 0x1f) + 34;
	for (i = 0; i <= max; i++) {
		lfsr = (lfsr >> 1) |		\
			((lfsr ^		\
			  (lfsr >> 1) ^		\
			  (lfsr >> 21) ^	\
			  (lfsr >> 31)		\
			  ) << 31);
	}
	sr_spew("Iterate 0x%08x -> 0x%08x", devc->lfsr, lfsr);
	devc->lfsr = lfsr;
}

static void encrypt(const struct sr_dev_inst *sdi, const uint8_t *in, uint8_t *out, uint16_t len)
{
	struct dev_context *devc = sdi->priv;
	uint32_t lfsr = devc->lfsr;
	uint8_t value, mask;
	int i;

	for (i = 0; i < len; i++) {
		value = in[i];
		mask = lfsr >> (i % 4 * 8);
		if (i == 0)
			value = (value & 0x28) | ((value ^ mask) & ~0x28);
		else
			value = value ^ mask;
		out[i] = value;
	}
	iterate_lfsr(sdi);
}

static void decrypt(const struct sr_dev_inst *sdi, uint8_t *data, uint16_t len)
{
	struct dev_context *devc = sdi->priv;
	uint32_t lfsr = devc->lfsr;
	int i;

	for (i = 0; i < len; i++)
		data[i] ^= (lfsr >> (i % 4 * 8));
	iterate_lfsr(sdi);
}

/*
 * Reseed packet for seed 0 (the seed is XORed with a fixed key in the
 * packet, so this resets the device LFSR to 0). It has no response and does
 * not advance the LFSR.
 */
static const uint8_t reseed_req[] = {0x20, 0x24, 0x4b, 0x35, 0x8e};

/*
 * After a failed transaction the device LFSR may have advanced while ours did
 * not. Any further command would then decrypt to garbage on the device, and
 * the firmware treats unknown commands as fatal (it needs a power cycle to
 * recover). Reseed unconditionally to get both sides back in sync.
 */
static void resync(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	struct sr_usb_dev_inst *usb = sdi->conn;
	uint8_t req[sizeof(reseed_req)];
	int xfer;

	memcpy(req, reseed_req, sizeof(req));
	devc->lfsr = 0;
	if (libusb_bulk_transfer(usb->devhdl, 1, req, sizeof(req), &xfer, 1000) != 0)
		sr_err("Failed to resync command channel.");
}

static int transact(const struct sr_dev_inst *sdi,
		    const uint8_t *req, uint16_t req_len,
		    uint8_t *rsp, uint16_t rsp_len)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	uint8_t *req_enc;
	uint8_t rsp_dummy[1] = {};
	int ret, xfer;

	if (req_len < 2 || req_len > 1024 || rsp_len > 128 ||
	    !req || (rsp_len > 0 && !rsp))
		return SR_ERR_ARG;

	req_enc = g_malloc(req_len);
	encrypt(sdi, req, req_enc, req_len);

	ret = libusb_bulk_transfer(usb->devhdl, 1, req_enc, req_len, &xfer, 1000);
	g_free(req_enc);
	if (ret != 0) {
		sr_dbg("Failed to send request 0x%02x: %s.",
		       req[1], libusb_error_name(ret));
		goto err;
	}
	if (xfer != req_len) {
		sr_dbg("Failed to send request 0x%02x: incorrect length "
		       "%d != %d.", req[1], xfer, req_len);
		goto err;
	}

	if (req[0] == 0x20) { /* Reseed. */
		return SR_OK;
	} else if (rsp_len == 0) {
		rsp = rsp_dummy;
		rsp_len = sizeof(rsp_dummy);
	}

	ret = libusb_bulk_transfer(usb->devhdl, 0x80 | 1, rsp, rsp_len,
				   &xfer, 1000);
	if (ret != 0) {
		sr_dbg("Failed to receive response to request 0x%02x: %s.",
		       req[1], libusb_error_name(ret));
		goto err;
	}
	if (xfer != rsp_len) {
		sr_dbg("Failed to receive response to request 0x%02x: "
		       "incorrect length %d != %d.", req[1], xfer, rsp_len);
		goto err;
	}

	decrypt(sdi, rsp, rsp_len);

	return SR_OK;

err:
	if (req[0] != 0x20)
		resync(sdi);
	return SR_ERR;
}

static int reseed(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;

	devc->lfsr = 0;
	return transact(sdi, reseed_req, sizeof(reseed_req), NULL, 0);
}

static int write_regs(const struct sr_dev_inst *sdi, uint8_t (*regs)[2], uint8_t cnt)
{
	uint8_t req[64];
	int i;

	if (cnt < 1 || cnt > 30)
		return SR_ERR_ARG;

	req[0] = 0x00;
	req[1] = COMMAND_WRITE_REG;
	req[2] = cnt;

	for (i = 0; i < cnt; i++) {
		req[3 + 2 * i] = regs[i][0];
		req[4 + 2 * i] = regs[i][1];
	}

	return transact(sdi, req, 3 + (2 * cnt), NULL, 0);
}

static int write_reg(const struct sr_dev_inst *sdi,
		     uint8_t address, uint8_t value)
{
	uint8_t regs[2] = {address, value};

	return write_regs(sdi, &regs, 1);
}

static int read_regs(const struct sr_dev_inst *sdi,
		     const uint8_t *regs, uint8_t *values,
		     uint8_t cnt)
{
	uint8_t req[33];

	if (cnt < 1 || cnt > 30)
		return SR_ERR_ARG;

	req[0] = 0x00;
	req[1] = COMMAND_READ_REG;
	req[2] = cnt;
	if (cnt)
		memcpy(&req[3], regs, cnt);

	return transact(sdi, req, 3 + cnt, values, cnt);
}

static int read_reg(const struct sr_dev_inst *sdi,
		    uint8_t address, uint8_t *value)
{
	return read_regs(sdi, &address, value, 1);
}

static int write_adc(const struct sr_dev_inst *sdi,
		     uint8_t address, uint16_t value)
{
	uint8_t regs[][2] = {
		{REG_ADC_IDX, address},
		{REG_ADC_VAL_LSB, value},
		{REG_ADC_VAL_MSB, value >> 8},
	};

	return write_regs(sdi, ARRAY_AND_SIZE(regs));
}

static int read_eeprom(const struct sr_dev_inst *sdi,
		       uint16_t address, uint8_t *data, uint16_t len)
{
	uint8_t req[8] = {
		0x00, COMMAND_READ_EEPROM,
		0x33, 0x81, /* Unknown values */
		address, address >> 8,
		len, len >> 8
	};

	return transact(sdi, req, sizeof(req), data, len);
}

static int read_eeprom_serial(const struct sr_dev_inst *sdi,
			      uint8_t data[8])
{
	return read_eeprom(sdi, 0x08, data, 0x8);
}

static int read_eeprom_magic(const struct sr_dev_inst *sdi,
			     uint8_t data[16])
{
	return read_eeprom(sdi, 0x10, data, 0x10);
}

static int read_temperature(const struct sr_dev_inst *sdi, int8_t *temp)
{
	uint8_t req[2] = {0x00, COMMAND_READ_TEMP};

	return transact(sdi, req, sizeof(req), (uint8_t*)temp, 1);
}

static int get_firmware_version(const struct sr_dev_inst *sdi)
{
	uint8_t req[2] = {0x00, COMMAND_READ_FW_VER};
	uint8_t rsp[128] = {};
	int ret;

	ret = transact(sdi, req, sizeof(req), rsp, sizeof(rsp));
	if (ret == SR_OK) {
		rsp[63] = 0;
		sr_dbg("fw-version: %s", rsp);
	}

	return ret;
}

static int read_i2c(const struct sr_dev_inst *sdi, uint8_t *data, uint8_t len)
{
	uint8_t req[5];
	uint8_t rsp[1 + 128];
	int ret;

	if (len < 1 || len > 128 || !data)
		return SR_ERR_ARG;

	req[0] = 0x00;
	req[1] = COMMAND_READ_I2C;
	req[2] = 0xc0; /* Fixed address */
	req[3] = len;
	req[4] = 0; /* Len MSB? */

	ret = transact(sdi, req, sizeof(req), rsp, 1 + len);
	if (ret != SR_OK)
		return ret;
	if (rsp[0] != 0x02) {
		sr_dbg("Failed to do I2C read (0x%02x).", rsp[0]);
		return SR_ERR;
	}

	memcpy(data, rsp + 1, len);
	return SR_OK;
}

static int write_i2c(const struct sr_dev_inst *sdi, const uint8_t *data, uint8_t len)
{
	uint8_t req[5 + 128];
	uint8_t rsp[1];
	int ret;

	if (len < 1 || len > 128 || !data)
		return SR_ERR_ARG;

	req[0] = 0x00;
	req[1] = COMMAND_WRITE_I2C;
	req[2] = 0xc0; /* Fixed address */
	req[3] = len;
	req[4] = 0; /* Len MSB? */
	memcpy(req + 5, data, len);

	ret = transact(sdi, req, 5 + len, rsp, sizeof(rsp));
	if (ret != SR_OK)
		return ret;
	if (rsp[0] != 0x02) {
		sr_dbg("Failed to do I2C write (0x%02x).", rsp[0]);
		return SR_ERR;
	}

	return SR_OK;
}

static int wake_i2c(const struct sr_dev_inst *sdi)
{
	uint8_t req[] = {0x00, COMMAND_WAKE_I2C};
	uint8_t rsp[1] = {};
	uint8_t i2c_rsp[1 + 1 + 2] = {};
	int ret;

	ret = transact(sdi, req, sizeof(req), rsp, sizeof(rsp));
	if (ret != SR_OK)
		return ret;
	if (rsp[0] != 0x00) {
		sr_dbg("Failed to do I2C wake trigger (0x%02x).", rsp[0]);
		return SR_ERR;
	}

	ret = read_i2c(sdi, i2c_rsp, sizeof(i2c_rsp));
	if (ret != SR_OK) {
		return ret;
	}
	if (i2c_rsp[1] != 0x11) {
		sr_dbg("Failed to do I2C wake read (0x%02x).", i2c_rsp[0]);
		return SR_ERR;
	}

	return SR_OK;
}

static int crypto_random(const struct sr_dev_inst *sdi, uint8_t *data)
{
	uint8_t i2c_req[8] = {0x03, 0x07, 0x1b, 0x00, 0x00, 0x00, 0x24, 0xcd};
	uint8_t i2c_rsp[1 + 32 + 2] = {};
	int ret;

	ret = write_i2c(sdi, i2c_req, sizeof(i2c_req));
	if (ret != SR_OK)
		return ret;

	g_usleep(100000); /* TODO: Poll instead. */

	ret = read_i2c(sdi, i2c_rsp, sizeof(i2c_rsp));
	if (ret != SR_OK)
		return ret;

	if (data)
		memcpy(data, i2c_rsp + 1, 32);

	return SR_OK;
}

static int crypto_nonce(const struct sr_dev_inst *sdi, uint8_t *data)
{
	uint8_t i2c_req[6 + 20 + 2] = {0x03, 0x1b, 0x16, 0x00, 0x00, 0x00};
	uint8_t i2c_rsp[1 + 32 + 2] = {};
	int ret;

	/* CRC */
	i2c_req[26] = 0x7d;
	i2c_req[27] = 0xe0;

	ret = write_i2c(sdi, i2c_req, sizeof(i2c_req));
	if (ret != SR_OK)
		return ret;

	g_usleep(100000); /* TODO: Poll instead. */

	ret = read_i2c(sdi, i2c_rsp, sizeof(i2c_rsp));
	if (ret != SR_OK)
		return ret;

	if (data)
		memcpy(data, i2c_rsp + 1, 32);

	return SR_OK;
}

static int crypto_sign(const struct sr_dev_inst *sdi, uint8_t *data, uint8_t *crc)
{
	uint8_t i2c_req[8] = {0x03, 0x07, 0x41, 0x80, 0x00, 0x00, 0x28, 0x05};
	uint8_t i2c_rsp[1 + 64 + 2] = {};
	int ret;

	ret = write_i2c(sdi, i2c_req, sizeof(i2c_req));
	if (ret != SR_OK)
		return ret;

	g_usleep(100000); /* TODO: Poll instead. */

	ret = read_i2c(sdi, i2c_rsp, sizeof(i2c_rsp));
	if (ret != SR_OK)
		return ret;

	memcpy(data, i2c_rsp + 1, 64);
	memcpy(crc, i2c_rsp + 1 + 64, 2);

	return SR_OK;
}

static int authenticate(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	uint8_t random[32] = {};
	uint8_t nonce[32] = {};
	uint8_t sig[64] = {};
	uint8_t sig_crc[64] = {};
	uint32_t lfsr;
	int i, ret;

	ret = wake_i2c(sdi);
	if (ret != SR_OK)
		return ret;

	ret = crypto_random(sdi, random);
	if (ret != SR_OK)
		return ret;
	sr_dbg("random: 0x%02x 0x%02x 0x%02x 0x%02x", random[0], random[1], random[2], random[3]);

	ret = crypto_nonce(sdi, nonce);
	if (ret != SR_OK)
		return ret;
	sr_dbg("nonce: 0x%02x 0x%02x 0x%02x 0x%02x", nonce[0], nonce[1], nonce[2], nonce[3]);

	ret = crypto_nonce(sdi, nonce);
	if (ret != SR_OK)
		return ret;
	sr_dbg("nonce: 0x%02x 0x%02x 0x%02x 0x%02x", nonce[0], nonce[1], nonce[2], nonce[3]);

	ret = crypto_sign(sdi, sig, sig_crc);
	if (ret != SR_OK)
		return ret;
	sr_dbg("sig: 0x%02x 0x%02x 0x%02x 0x%02x", sig[0], sig[1], sig[2], sig[3]);
	sr_dbg("sig crc: 0x%02x 0x%02x", sig_crc[0], sig_crc[1]);

	lfsr = 0;
	for (i = 0; i < 28; i++)
		lfsr ^= nonce[i] << (8 * (i % 4));
	lfsr ^= sig_crc[0] | sig_crc[1] << 8;

	sr_dbg("Authenticate 0x%08x -> 0x%08x", devc->lfsr, lfsr);
	devc->lfsr = lfsr;

	return SR_OK;
}

static int upload_bitstream_part(const struct sr_dev_inst *sdi,
				 const uint8_t *data, uint16_t len)
{
	uint8_t req[4 + 1020];
	uint8_t rsp[1];
	int ret;

	if (len < 1 || len > 1020 || !data)
		return SR_ERR_ARG;

	req[0] = 0x00;
	req[1] = COMMAND_SEND_BITSTREAM;
	req[2] = len;
	req[3] = len >> 8;
	memcpy(req + 4, data, len);

	ret = transact(sdi, req, 4 + len, rsp, sizeof(rsp));
	if (ret != SR_OK)
		return ret;
	if (rsp[0] != 0x00) {
		sr_dbg("Failed to do bitstream upload (0x%02x).", rsp[0]);
		return SR_ERR;
	}

	return SR_OK;
}

static int upload_bitstream(const struct sr_dev_inst *sdi,
			    const char *name)
{
	struct drv_context *drvc = sdi->driver->context;
	unsigned char *bitstream = NULL;
	uint8_t req[2];
	uint8_t rsp[1];
	uint8_t reg_val;
	int ret = SR_ERR;
	size_t bs_size, bs_offset = 0, bs_part_size;

	bitstream = sr_resource_load(drvc->sr_ctx, SR_RESOURCE_FIRMWARE,
				     name, &bs_size, 512 * 1024);
	if (!bitstream)
		goto out;

	sr_info("Uploading bitstream '%s'.", name);

	req[0] = 0x00;
	req[1] = COMMAND_INIT_BITSTREAM;

	ret = transact(sdi, req, sizeof(req), rsp, sizeof(rsp));
	if (ret != SR_OK)
		return ret;
	if (rsp[0] != 0x00) {
		sr_err("Failed to start bitstream upload (0x%02x).", rsp[0]);
		ret = SR_ERR;
		goto out;
	}

	while (bs_offset < bs_size) {
		bs_part_size = MIN(bs_size - bs_offset, 1020);
		sr_spew("Uploading %zd bytes.", bs_part_size);
		ret = upload_bitstream_part(sdi, bitstream + bs_offset, bs_part_size);
		if (ret != SR_OK)
			goto out;
		bs_offset += bs_part_size;
	}

	sr_info("Bitstream upload done.");

	/* Check a scratch register? */
	ret = write_reg(sdi, 0x7f, 0xaa);
	if (ret != SR_OK)
		goto out;
	ret = read_reg(sdi, 0x7f, &reg_val);
	if (ret != SR_OK)
		goto out;
	if (reg_val != 0xaa) {
		sr_err("Failed FPGA register read-back (0x%02x != 0xaa).", rsp[0]);
		ret = SR_ERR;
		goto out;
	}

 out:
	g_free(bitstream);

	return ret;
}

#if 0
static int set_led(const struct sr_dev_inst *sdi, uint8_t r, uint8_t g, uint8_t b)
{
	uint8_t regs[][2] = {
		{REG_LED_RED, r},
		{REG_LED_GREEN, g},
		{REG_LED_BLUE, b},
	};

	authenticate(sdi);

	return write_regs(sdi, ARRAY_AND_SIZE(regs));
}
#endif

static int configure_channels(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	struct sr_channel *c;
	const GSList *l;
	uint16_t mask;
	unsigned int hw;

	devc->dig_channel_cnt = 0;
	devc->dig_channel_mask = 0;
	devc->ana_channel_cnt = 0;
	devc->ana_channel_mask = 0;
	for (l = sdi->channels; l; l = l->next) {
		c = l->data;
		if (!c->enabled)
			continue;

		if (c->type == SR_CHANNEL_ANALOG) {
			/* Analog channels follow the digital ones. */
			hw = c->index - devc->profile->num_channels;
			devc->ana_hw_index[devc->ana_channel_cnt] = hw;
			devc->ana_channels[devc->ana_channel_cnt++] = c;
			devc->ana_channel_mask |= 1 << hw;
			continue;
		}

		mask = 1 << c->index;
		devc->dig_channel_shift[devc->dig_channel_cnt] = c->index;
		devc->dig_channel_masks[devc->dig_channel_cnt++] = mask;
		devc->dig_channel_mask |= mask;
	}
	sr_dbg("%d digital channels enabled (0x%04x), %d analog (0x%02x)",
	       devc->dig_channel_cnt, devc->dig_channel_mask,
	       devc->ana_channel_cnt, devc->ana_channel_mask);

	return SR_OK;
}

/*
 * Analog calibration.
 *
 * The vendor software keeps a per-device calibration file (JSON) with the
 * full-scale voltage range of every analog channel (codes -2048..+2047) and,
 * per analog samplerate, IIR filters (cascaded biquads, unity DC gain) that
 * flatten the front-end frequency response. It is looked up as firmware
 * resource "saleae-logic-pro-<device id>.cal", then in Logic 2's cache
 * ($XDG_CONFIG_HOME/Logic/calibrations/<device id>-*.cal). Without one,
 * nominal ranges are used and no filtering is done.
 */

#define NOMINAL_VMIN	(-10.45f)
#define NOMINAL_VMAX	(10.65f)

/* Find "key": at or after p, return a pointer just past the colon. */
static const char *json_key(const char *p, const char *key)
{
	char pat[64];

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	if (!p || !(p = strstr(p, pat)))
		return NULL;
	p += strlen(pat);
	while (*p == ' ' || *p == ':')
		p++;

	return p;
}

static const char *json_double(const char *p, const char *key, double *val)
{
	char *end;

	if (!(p = json_key(p, key)))
		return NULL;
	*val = g_ascii_strtod(p, &end);

	return end == p ? NULL : end;
}

static char *load_calibration_file(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	struct drv_context *drvc = sdi->driver->context;
	char name[64], prefix[32], *buf, *text, *best;
	const char *entry;
	size_t size;
	gchar *dir, *path;
	GDir *gdir;

	/* Logic 2 cache first: newest "<id>-<timestamp>-<n>.cal" wins. */
	snprintf(prefix, sizeof(prefix), "%" PRIu64 "-", devc->device_id);
	dir = g_build_filename(g_get_user_config_dir(), "Logic",
			       "calibrations", NULL);
	best = NULL;
	if ((gdir = g_dir_open(dir, 0, NULL))) {
		while ((entry = g_dir_read_name(gdir))) {
			if (!g_str_has_prefix(entry, prefix) ||
			    !g_str_has_suffix(entry, ".cal"))
				continue;
			if (!best || strcmp(entry, best) > 0) {
				g_free(best);
				best = g_strdup(entry);
			}
		}
		g_dir_close(gdir);
	}
	text = NULL;
	if (best) {
		path = g_build_filename(dir, best, NULL);
		if (g_file_get_contents(path, &text, NULL, NULL))
			sr_info("Using analog calibration '%s'.", path);
		g_free(path);
		g_free(best);
	}
	g_free(dir);
	if (text)
		return text;

	/* Then a copy in the firmware directories. */
	snprintf(name, sizeof(name), "saleae-logic-pro-%" PRIu64 ".cal",
		 devc->device_id);
	buf = sr_resource_load(drvc->sr_ctx, SR_RESOURCE_FIRMWARE, name,
			       &size, 1024 * 1024);
	if (!buf)
		return NULL;
	text = g_strndup(buf, size);
	g_free(buf);
	sr_info("Using analog calibration '%s'.", name);

	return text;
}

static void update_scale(struct dev_context *devc)
{
	struct analog_cal *cal;
	unsigned int i;

	for (i = 0; i < MAX_ANALOG_CHANNELS; i++) {
		cal = &devc->cal[i];
		cal->scale = (cal->vmax - cal->vmin) / 4095;
		cal->offset = cal->vmin + 2048 * cal->scale;
	}
}

SR_PRIV void saleae_logic_pro_load_calibration(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	const char *p;
	double vmin, vmax;
	unsigned int i;

	g_free(devc->cal_json);
	devc->cal_json = NULL;
	for (i = 0; i < MAX_ANALOG_CHANNELS; i++) {
		devc->cal[i].vmin = NOMINAL_VMIN;
		devc->cal[i].vmax = NOMINAL_VMAX;
		devc->cal[i].num_sos = 0;
		devc->cal[i].group_delay = 0;
	}
	if (!devc->profile->num_analog)
		return;

	devc->cal_json = load_calibration_file(sdi);
	if (!devc->cal_json) {
		update_scale(devc);
		sr_warn("No analog calibration for device %" PRIu64 ", using "
			"nominal ranges. Copy Logic 2's calibration file to the "
			"firmware directory as saleae-logic-pro-%" PRIu64 ".cal.",
			devc->device_id, devc->device_id);
		return;
	}

	p = json_key(devc->cal_json, "fullScaleVoltageRanges");
	for (i = 0; p && i < devc->profile->num_analog; i++) {
		if (!(p = json_double(p, "minimumVoltage", &vmin)) ||
		    !(p = json_double(p, "maximumVoltage", &vmax)))
			break;
		devc->cal[i].vmin = vmin;
		devc->cal[i].vmax = vmax;
		sr_dbg("A%u: %.4f .. %.4f V", i, vmin, vmax);
	}
	if (i != devc->profile->num_analog)
		sr_warn("Incomplete full-scale ranges in calibration file.");
	update_scale(devc);
}

/* Load the frequency-response filters for one analog samplerate. */
static void select_calibration_rate(struct dev_context *devc, uint64_t rate)
{
	static const char *const coef[] = { "b0", "b1", "b2", "a1", "a2" };
	const char *p, *close;
	char key[32], *end;
	double v[5];
	unsigned int i, j, k;
	struct analog_cal *cal;

	for (i = 0; i < MAX_ANALOG_CHANNELS; i++) {
		devc->cal[i].num_sos = 0;
		devc->cal[i].group_delay = 0;
	}
	if (!devc->cal_json)
		return;

	snprintf(key, sizeof(key), "%" PRIu64, rate);
	p = json_key(json_key(devc->cal_json, "calibrationSets"), key);
	if (!p) {
		sr_warn("No calibration filters for %" PRIu64 " Hz.", rate);
		return;
	}

	for (i = 0; i < devc->profile->num_analog; i++) {
		cal = &devc->cal[i];
		if (!(p = json_key(p, "groupDelay")))
			break;
		cal->group_delay = MIN(strtoul(p, &end, 10), MAX_GROUP_DELAY);
		if (!(p = json_key(p, "df1SosCoefficients")) ||
		    !(close = strchr(p, ']')))
			break;
		for (j = 0; j < MAX_SOS_SECTIONS; j++) {
			const char *q = strstr(p, "\"b0\"");
			if (!q || q > close)
				break;
			for (k = 0; k < 5; k++)
				if (!(q = json_double(q, coef[k], &v[k])))
					break;
			if (k != 5)
				break;
			cal->sos[j] = (struct sos){ v[0], v[1], v[2], v[3], v[4],
						    0, 0, 0, 0 };
			p = q;
		}
		cal->num_sos = j;
		p = close;
	}
	sr_dbg("Loaded calibration filters for %" PRIu64 " Hz.", rate);
}

/* Apply the frequency-response filter to a block of voltages, in place. */
static void filter_block(struct analog_cal *cal, float *v, size_t n)
{
	unsigned int i;
	size_t k;
	struct sos *s;
	float x, y, x1, x2, y1, y2;

	if (!n || !cal->num_sos)
		return;

	if (!cal->primed) {
		/* Start each section in its DC steady state: no transient. */
		x = v[0];
		for (i = 0; i < cal->num_sos; i++) {
			s = &cal->sos[i];
			s->x1 = s->x2 = x;
			x *= (s->b0 + s->b1 + s->b2) / (1 + s->a1 + s->a2);
			s->y1 = s->y2 = x;
		}
		cal->primed = TRUE;
	}

	/* Section by section over the whole block (cache friendly). */
	for (i = 0; i < cal->num_sos; i++) {
		s = &cal->sos[i];
		x1 = s->x1; x2 = s->x2; y1 = s->y1; y2 = s->y2;
		for (k = 0; k < n; k++) {
			x = v[k];
			y = s->b0 * x + s->b1 * x1 + s->b2 * x2 -
			    s->a1 * y1 - s->a2 * y2;
			x2 = x1; x1 = x;
			y2 = y1; y1 = y;
			v[k] = y;
		}
		s->x1 = x1; s->x2 = x2; s->y1 = y1; s->y2 = y2;
	}
}

/*
 * Post-bitstream init as done by Logic 2 for the Logic Pro 8 (hardware with
 * a Lattice FPGA), taken from a USB capture.
 */
static int logic2_post_bitstream_init(const struct sr_dev_inst *sdi)
{
	static const struct { uint8_t reg; uint16_t val; } adc[] = {
		{0x00, 0x0001}, {0x0f, 0x0000}, {0x11, 0x0777}, {0x12, 0x4444},
		{0x2a, 0x1111}, {0x2b, 0x1111}, {0x42, 0x8040}, {0x46, 0x8204},
	};
	uint8_t val;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(adc); i++)
		if ((ret = write_adc(sdi, adc[i].reg, adc[i].val)) != SR_OK)
			return ret;

	if ((ret = write_reg(sdi, 0x00, 0x04)) != SR_OK ||
	    (ret = write_reg(sdi, 0x00, 0x00)) != SR_OK ||
	    (ret = read_reg(sdi, REG_STATUS, &val)) != SR_OK)
		return ret;
	sr_dbg("status after reset: 0x%02x", val);
	if ((ret = write_reg(sdi, 0x00, 0x08)) != SR_OK ||
	    (ret = write_reg(sdi, 0x00, 0x00)) != SR_OK ||
	    (ret = write_reg(sdi, 0x15, 0x00)) != SR_OK ||
	    (ret = write_adc(sdi, 0x0f, 0x0100)) != SR_OK ||
	    (ret = write_reg(sdi, 0x00, 0x00)) != SR_OK ||
	    (ret = write_reg(sdi, 0x12, 0x04)) != SR_OK)
		return ret;

	return SR_OK;
}

SR_PRIV int saleae_logic_pro_init(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	uint8_t reg_val;
	uint8_t dummy[8];
	uint8_t serial[8];
	uint8_t magic[16];
	int8_t temperature;
	int ret, i;

	ret = reseed(sdi);
	if (ret != SR_OK)
		return ret;

	ret = get_firmware_version(sdi);
	if (ret != SR_OK)
		return ret;

	sr_dbg("read serial");
	ret = read_eeprom_serial(sdi, serial);
	if (ret != SR_OK)
		return ret;
	devc->device_id = 0;
	for (i = 7; i >= 0; i--)
		devc->device_id = (devc->device_id << 8) | serial[i];
	sr_info("Device ID: 0x%016" PRIx64 ".", devc->device_id);
	saleae_logic_pro_load_calibration(sdi);

	/* Check if we need to upload the bitstream. */
	ret = read_reg(sdi, 0x7f, &reg_val);
	if (ret != SR_OK)
		return ret;
	if (reg_val == 0xaa) {
		sr_info("Skipping bitstream upload.");
	} else {
		ret = upload_bitstream(sdi, devc->profile->bitstream_name);
		if (ret != SR_OK)
			return ret;
	}

	if (devc->profile->logic2_init)
		return logic2_post_bitstream_init(sdi);

	/* Reset the ADC? */
	sr_dbg("reset ADC");
	ret = write_reg(sdi, 0x00, 0x00);
	if (ret != SR_OK)
		return ret;
	ret = write_reg(sdi, 0x00, 0x80);
	if (ret != SR_OK)
		return ret;

	sr_dbg("init ADC");
	ret = write_adc(sdi, 0x11, 0x0444);
	if (ret != SR_OK)
		return ret;
	ret = write_adc(sdi, 0x12, 0x0777);
	if (ret != SR_OK)
		return ret;
	ret = write_adc(sdi, 0x25, 0x0000);
	if (ret != SR_OK)
		return ret;
	ret = write_adc(sdi, 0x45, 0x0000);
	if (ret != SR_OK)
		return ret;
	ret = write_adc(sdi, 0x2a, 0x1111);
	if (ret != SR_OK)
		return ret;
	ret = write_adc(sdi, 0x2b, 0x1111);
	if (ret != SR_OK)
		return ret;
	ret = write_adc(sdi, 0x46, 0x0004);
	if (ret != SR_OK)
		return ret;
	ret = write_adc(sdi, 0x50, 0x0000);
	if (ret != SR_OK)
		return ret;
	ret = write_adc(sdi, 0x55, 0x0020);
	if (ret != SR_OK)
		return ret;
	ret = write_adc(sdi, 0x56, 0x0000);
	if (ret != SR_OK)
		return ret;

	ret = write_reg(sdi, 0x15, 0x00);
	if (ret != SR_OK)
		return ret;

	ret = write_adc(sdi, 0x0f, 0x0100);
	if (ret != SR_OK)
		return ret;

	/* Resets? */
	sr_dbg("resets");
	ret = write_reg(sdi, 0x00, 0x02); /* bit 1 */
	if (ret != SR_OK)
		return ret;
	ret = write_reg(sdi, 0x00, 0x00);
	if (ret != SR_OK)
		return ret;
	ret = write_reg(sdi, 0x00, 0x04); /* bit 2 */
	if (ret != SR_OK)
		return ret;
	ret = write_reg(sdi, 0x00, 0x00);
	if (ret != SR_OK)
		return ret;
	ret = write_reg(sdi, 0x00, 0x08); /* bit 3 */
	if (ret != SR_OK)
		return ret;
	ret = write_reg(sdi, 0x00, 0x00);
	if (ret != SR_OK)
		return ret;

	sr_dbg("read dummy");
	for (i = 0; i < 8; i++) {
		ret = read_reg(sdi, 0x41 + i, &dummy[i]);
		if (ret != SR_OK)
			return ret;
	}

	/* Read and write back magic EEPROM value. */
	sr_dbg("read/write magic");
	ret = read_eeprom_magic(sdi, magic);
	if (ret != SR_OK)
		return ret;
	for (i = 0; i < 16; i++) {
		ret = write_reg(sdi, 0x17, magic[i]);
		if (ret != SR_OK)
			return ret;
	}

	ret = read_temperature(sdi, &temperature);
	if (ret != SR_OK)
		return ret;
	sr_dbg("temperature = %d", temperature);

	/* Setting the LED doesn't work yet. */
	/* set_led(sdi, 0x00, 0x00, 0xff); */

	return SR_OK;
}

SR_PRIV int saleae_logic_pro_prepare(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	uint8_t regs_unknown[][2] = {
		{0x03, 0x0f},
		{0x04, 0x00},
		{0x05, 0x00},
	};
	uint8_t regs_config[][2] = {
		{0x00, 0x00},
		{0x08, 0x00}, /* Analog channel mask (LSB) */
		{0x09, 0x00}, /* Analog channel mask (MSB) */
		{0x06, 0x01}, /* Digital channel mask (LSB) */
		{0x07, 0x00}, /* Digital channel mask (MSB) */
		{0x0a, 0x00}, /* Analog sample rate? */
		{0x0b, 0x64}, /* Digital sample rate? */
		{0x0c, 0x00},
		{0x0d, 0x00}, /* Analog mux rate? */
		{0x0e, 0x01}, /* Digital mux rate? */
		{0x12, 0x04},
		{0x13, 0x00},
		{0x14, 0xff}, /* Pre-divider? */
	};
	uint8_t start_req[] = {0x00, 0x01};
	uint8_t start_rsp[2] = {};
	uint64_t rate, div, dec, bw, throttle, ana_div;
	unsigned int i;

	configure_channels(sdi);

	/* Digital channel mask and muxing */
	regs_config[3][1] = devc->dig_channel_mask;
	regs_config[4][1] = devc->dig_channel_mask >> 8;
	regs_config[9][1] = devc->dig_channel_cnt;

	if (devc->dig_channel_cnt == 0 && devc->ana_channel_cnt == 0) {
		sr_err("No channels enabled.");
		return SR_ERR_ARG;
	}

	/*
	 * Digital samplerate, from a 500 MHz sample clock:
	 * - legacy bitstream: decimation by 5 * n, 16-bit n in regs
	 *   0x0b/0x0c, or n = 0 for 500 MHz;
	 * - Logic 2 bitstream: either by 5 * n with an 8-bit n in reg 0x0b,
	 *   or (reg 0x0b = 0) by 2^dec with dec in reg 0x1a for 500, 250
	 *   and 125 MHz.
	 */
	rate = devc->dig_samplerate;
	if (!rate || SR_MHZ(500) % rate) {
		sr_err("Unsupported samplerate %" PRIu64 ".", rate);
		return SR_ERR_ARG;
	}
	div = SR_MHZ(500) / rate;
	dec = 0;
	if (devc->profile->logic2_init && (div == 1 || div == 2 || div == 4)) {
		dec = div == 1 ? 0 : div == 2 ? 1 : 2;
		div = 0;
	} else if (div == 1) {
		div = 0;
	} else if (div % 5 ||
		   div / 5 > (devc->profile->logic2_init ? 0xff : 0xffff)) {
		sr_err("Unsupported samplerate %" PRIu64 ".", rate);
		return SR_ERR_ARG;
	} else {
		div /= 5;
	}
	regs_config[6][1] = div;
	regs_config[7][1] = div >> 8;

	/*
	 * Analog channels run at the same samplerate as the digital ones:
	 * 50 MHz / ana_div, for the rates the vendor calibrates (50 MHz down
	 * to 781.25 kHz). The register holds ana_div, or ana_div - 1 below 4.
	 */
	ana_div = 0;
	if (devc->ana_channel_cnt) {
		ana_div = SR_MHZ(50) / rate;
		if (SR_MHZ(50) % rate || (ana_div != 1 && (ana_div < 4 ||
		    ana_div > 64 || (ana_div & (ana_div - 1))))) {
			sr_err("Analog channels need one of the samplerates "
			       "781.25 kHz, 1.5625/3.125/6.25/12.5/50 MHz.");
			return SR_ERR_ARG;
		}
		if (ana_div < 4)
			ana_div--;
	}

	devc->ana_gd = 0;

	/*
	 * Stream frame (vendor DeviceCalculateMixRate() for equal rates):
	 * per 32 samples, one 32-bit word per digital channel, followed by
	 * 4 rounds of one group per analog channel (ascending), each group
	 * 3 words holding 8 x 12-bit samples. Without digital channels a
	 * frame is a single round (8 samples).
	 */
	devc->frame_dig_words = devc->dig_channel_cnt;
	if (devc->dig_channel_cnt) {
		devc->frame_samples = 32;
		devc->frame_ana_words = 12 * devc->ana_channel_cnt;
	} else {
		devc->frame_samples = 8;
		devc->frame_ana_words = 3 * devc->ana_channel_cnt;
	}

	/*
	 * USB bandwidth throttle, as computed by the vendor software:
	 * ceil(100e6 / (bits per second / 32)), saturated to 8 bits.
	 */
	bw = rate * (devc->dig_channel_cnt + 12 * devc->ana_channel_cnt);
	devc->stream_bps = bw;
	if (bw > MAX_BANDWIDTH_BPS) {
		sr_err("Samplerate too high for %u digital and %u analog "
		       "channels (max %" PRIu64 " bit/s over USB).",
		       devc->dig_channel_cnt, devc->ana_channel_cnt,
		       (uint64_t)MAX_BANDWIDTH_BPS);
		return SR_ERR_ARG;
	}
	/* Rounded up, as the vendor does (5.33 -> 6 for 50 MS/s analog). */
	throttle = (SR_MHZ(100) * 32 + bw - 1) / bw;
	regs_config[12][1] = MIN(throttle, 0xff);

	if (devc->profile->logic2_init) {
		/* Register block as Logic 2 writes it for a capture. */
		uint8_t regs_l2[][2] = {
			{0x00, 0x00},
			{0x08, devc->ana_channel_mask},	/* Analog channel mask */
			{0x06, devc->dig_channel_mask},	/* Digital channel mask */
			{0x0a, ana_div},		/* Analog divider */
			{0x1b, 0x00},
			{0x0b, devc->dig_channel_cnt ? regs_config[6][1] : 0},
			{0x0c, 0x00},
			{0x1a, devc->dig_channel_cnt ? dec : 0},
			{0x0d, devc->frame_ana_words},	/* Analog mux */
			{0x0e, devc->frame_dig_words},	/* Digital mux */
			{0x12, 0x04},			/* IO voltage option */
			{0x13, 0x00},
			{0x14, regs_config[12][1]},	/* USB throttle */
		};
		if (devc->ana_channel_cnt) {
			select_calibration_rate(devc, rate);
			/*
			 * Compensate the filter delay only when there is
			 * logic data to align the analog data with.
			 */
			for (i = 0; devc->dig_channel_cnt &&
			     i < devc->ana_channel_cnt; i++)
				devc->ana_gd = MAX(devc->ana_gd,
					devc->cal[devc->ana_hw_index[i]].group_delay);
		}
		write_reg(sdi, 0x15, 0x03);
		write_regs(sdi, ARRAY_AND_SIZE(regs_unknown));
		write_regs(sdi, ARRAY_AND_SIZE(regs_l2));
	} else {
		authenticate(sdi);

		write_reg(sdi, 0x15, 0x03);
		write_regs(sdi, ARRAY_AND_SIZE(regs_unknown));
		write_regs(sdi, ARRAY_AND_SIZE(regs_config));
	}

	transact(sdi, start_req, sizeof(start_req), start_rsp, sizeof(start_rsp));

	return SR_OK;
}

SR_PRIV int saleae_logic_pro_start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	unsigned int i;

	devc->carry_len = 0;
	devc->samples_in = 0;
	devc->emit_pos = 0;
	for (i = 0; i < MAX_ANALOG_CHANNELS; i++)
		devc->cal[i].primed = FALSE;

	write_reg(sdi, 0x00, 0x01);

	return SR_OK;
}

SR_PRIV int saleae_logic_pro_stop(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	uint8_t stop_req[] = {0x00, 0x02};
	uint8_t stop_rsp[2] = {};
	uint8_t status;
	int ret;

	write_reg(sdi, 0x00, 0x00);
	transact(sdi, stop_req, sizeof(stop_req), stop_rsp, sizeof(stop_rsp));

	ret = read_reg(sdi, 0x40, &status);
	if (devc->profile->logic2_init) {
		/* Like Logic 2: power the ADC down again after a capture. */
		write_reg(sdi, 0x15, 0x00);
		write_adc(sdi, 0x0f, 0x0100);
	}
	if (ret != SR_OK)
		return ret;
	/* Bit 7 is set in normal operation with the Logic 2 bitstream. */
	if ((status & 0x7f) != 0x20) {
		sr_err("Capture error (status reg = 0x%02x).", status);
		return SR_ERR;
	}

	return SR_OK;
}

static void saleae_logic_pro_send_data(const struct sr_dev_inst *sdi,
				      void *data, size_t length, size_t unitsize)
{
	const struct sr_datafeed_logic logic = {
		.length = length,
		.unitsize = unitsize,
		.data = data
	};

	const struct sr_datafeed_packet packet = {
		.type = SR_DF_LOGIC,
		.payload = &logic
	};

	sr_session_send(sdi, &packet);
}

static void saleae_logic_pro_send_analog(const struct sr_dev_inst *sdi,
					 struct sr_channel *ch, float *data,
					 size_t num_samples)
{
	struct sr_datafeed_analog analog;
	struct sr_analog_encoding encoding;
	struct sr_analog_meaning meaning;
	struct sr_analog_spec spec;
	struct sr_datafeed_packet packet;

	sr_analog_init(&analog, &encoding, &meaning, &spec, 3);
	analog.meaning->channels = g_slist_append(NULL, ch);
	analog.meaning->mq = SR_MQ_VOLTAGE;
	analog.meaning->unit = SR_UNIT_VOLT;
	analog.meaning->mqflags = 0;
	analog.num_samples = num_samples;
	analog.data = data;
	packet.type = SR_DF_ANALOG;
	packet.payload = &analog;
	sr_session_send(sdi, &packet);
	g_slist_free(analog.meaning->channels);
}

/*
 * nibble_lanes[n]: the 4 bits of n (MSB = earliest sample) spread over four
 * 16-bit lanes (lane 0 = earliest sample in memory order), each 0 or 1.
 */
static const uint64_t nibble_lanes[16] = {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define L(n) ((((n) >> 3) & 1ULL) | ((((n) >> 2) & 1ULL) << 16) | \
	      ((((n) >> 1) & 1ULL) << 32) | (((n) & 1ULL) << 48))
#else
#define L(n) (((((n) >> 3) & 1ULL) << 48) | ((((n) >> 2) & 1ULL) << 32) | \
	      ((((n) >> 1) & 1ULL) << 16) | ((n) & 1ULL))
#endif
	L(0), L(1), L(2), L(3), L(4), L(5), L(6), L(7),
	L(8), L(9), L(10), L(11), L(12), L(13), L(14), L(15),
#undef L
};

/*
 * Convert complete stream frames (see saleae_logic_pro_prepare()) into
 * 16-bit logic samples and per-channel analog voltages. Words of a partial
 * frame are carried over to the next transfer. Returns the number of
 * sample periods converted.
 */
static size_t saleae_logic_pro_convert_data(const struct sr_dev_inst *sdi,
					    size_t nwords)
{
	struct dev_context *devc = sdi->priv;
	unsigned int frame_words, n_dig, n_ana, w, i, k, grp, ch;
	uint32_t word, *buf;
	size_t f, nframes, base;
	uint16_t *logic;
	int code;

	n_dig = devc->frame_dig_words;
	n_ana = devc->ana_channel_cnt;
	frame_words = n_dig + devc->frame_ana_words;

	/* work[] holds the carried-over words followed by the new ones. */
	buf = devc->work;
	nframes = nwords / frame_words;

	logic = devc->logic_buf + devc->ana_gd;
	for (f = 0; f < nframes; f++) {
		const uint32_t *fr = buf + f * frame_words;
		base = f * devc->frame_samples;

		if (n_dig) {
			/* 32 samples, MSB first, as 8 groups of 4 x 16 bits. */
			uint64_t acc[8] = { 0 };
			for (w = 0; w < n_dig; w++) {
				word = fr[w];
				for (i = 0; i < 8; i++)
					acc[i] |= nibble_lanes[(word >> (28 - 4 * i)) & 0xf]
						  << devc->dig_channel_shift[w];
			}
			memcpy(logic + base, acc, sizeof(acc));
		}

		/*
		 * Analog groups: 3 little-endian words = 8 x 12-bit two's
		 * complement samples, LSB first. Convert to volts here, filter
		 * per channel below.
		 */
		for (grp = 0; grp * 3 < devc->frame_ana_words; grp++) {
			const uint32_t *g = fr + n_dig + grp * 3;
			uint32_t raw[8];
			struct analog_cal *cal;
			float *dst;

			ch = grp % n_ana;
			cal = &devc->cal[devc->ana_hw_index[ch]];
			dst = devc->ana_buf[ch] + base + (grp / n_ana) * 8;
			raw[0] = g[0];
			raw[1] = g[0] >> 12;
			raw[2] = (g[0] >> 24) | (g[1] << 8);
			raw[3] = g[1] >> 4;
			raw[4] = g[1] >> 16;
			raw[5] = (g[1] >> 28) | (g[2] << 4);
			raw[6] = g[2] >> 8;
			raw[7] = g[2] >> 20;
			for (k = 0; k < 8; k++) {
				/* Sign-extend the low 12 bits. */
				code = (int32_t)(raw[k] << 20) >> 20;
				dst[k] = code * cal->scale + cal->offset;
			}
		}
	}
	/* Keep the words of a partial frame for the next transfer. */
	devc->carry_len = nwords - nframes * frame_words;
	memmove(buf, buf + nframes * frame_words,
		devc->carry_len * sizeof(uint32_t));

	for (ch = 0; ch < n_ana; ch++)
		filter_block(&devc->cal[devc->ana_hw_index[ch]],
			     devc->ana_buf[ch], nframes * devc->frame_samples);

	return nframes * devc->frame_samples;
}

static void free_transfer(struct libusb_transfer *transfer)
{
	const struct sr_dev_inst *sdi = transfer->user_data;
	struct dev_context *devc = sdi->priv;
	unsigned int i;

	for (i = 0; i < devc->num_transfers; i++) {
		if (devc->transfers[i] == transfer) {
			devc->transfers[i] = NULL;
			break;
		}
	}
	g_free(transfer->buffer);
	libusb_free_transfer(transfer);
	devc->submitted_transfers--;
}

SR_PRIV void LIBUSB_CALL saleae_logic_pro_receive_data(struct libusb_transfer *transfer)
{
	const struct sr_dev_inst *sdi = transfer->user_data;
	struct dev_context *devc = sdi->priv;
	uint64_t remain, in0, avail;
	size_t block, samples, skip, lpos, nwords, i;
	unsigned int c, gd;
	uint8_t *data;
	int trigger_offset, pre_trigger_samples;
	gboolean exceeded;
	int ret;

	switch (transfer->status) {
	case LIBUSB_TRANSFER_COMPLETED:
	case LIBUSB_TRANSFER_TIMED_OUT: /* We may have received some data though. */
		break;
	default:
		if (transfer->status != LIBUSB_TRANSFER_CANCELLED)
			sr_dbg("Transfer failed: %s.",
			       libusb_error_name(transfer->status));
		devc->acq_done = TRUE;
		free_transfer(transfer);
		return;
	}

	if (devc->acq_done) {
		free_transfer(transfer);
		return;
	}

	/*
	 * Copy the data out and give the buffer back to the device right
	 * away: at high analog rates the device FIFO overruns if the queue of
	 * transfers runs dry while we are converting.
	 */
	nwords = MIN((size_t)transfer->actual_length, devc->xfer_size) / 4;
	memcpy(devc->work + devc->carry_len, transfer->buffer,
	       nwords * sizeof(uint32_t));
	nwords += devc->carry_len;
	if ((ret = libusb_submit_transfer(transfer)) != LIBUSB_SUCCESS) {
		sr_err("Failed to resubmit transfer: %s.", libusb_error_name(ret));
		devc->acq_done = TRUE;
		free_transfer(transfer);
	}

	block = saleae_logic_pro_convert_data(sdi, nwords);

	/*
	 * Logic and analog are emitted over one aligned range per block, with
	 * equal sample counts. The analog calibration filter delays its output
	 * by ana_gd samples, so the logic stream is delayed by the same amount:
	 * aligned sample n is logic sample n and analog filter output n + gd.
	 */
	gd = devc->ana_gd;
	in0 = devc->samples_in;
	avail = in0 + block > gd ? in0 + block - gd : 0;
	samples = avail - devc->emit_pos;
	lpos = devc->emit_pos + gd - in0;	/* Also the index into ana_buf. */

	/* Logic samples are converted as 16 bits; pack to the unitsize. */
	data = (uint8_t *)(devc->logic_buf + lpos);
	if (devc->dig_channel_cnt && devc->unitsize == 1) {
		for (i = 0; i < samples; i++)
			data[i] = devc->logic_buf[lpos + i];
	}

	skip = 0;
	if (!devc->trigger_fired && samples) {
		trigger_offset = soft_trigger_logic_check(devc->stl, data,
			samples * devc->unitsize, &pre_trigger_samples);
		if (trigger_offset < 0) {
			skip = samples;
		} else {
			/* Pre-trigger samples were sent by the soft trigger. */
			sr_sw_limits_update_samples_read(&devc->limits,
							 pre_trigger_samples);
			devc->trigger_fired = TRUE;
			skip = trigger_offset;
		}
	}
	data += skip * devc->unitsize;
	samples -= skip;

	sr_sw_limits_get_remain(&devc->limits, &remain, NULL, NULL, &exceeded);
	if (devc->limits.limit_samples && samples > remain)
		samples = remain;
	if (samples) {
		if (devc->dig_channel_cnt)
			saleae_logic_pro_send_data(sdi, data,
				samples * devc->unitsize, devc->unitsize);
		for (c = 0; c < devc->ana_channel_cnt; c++)
			saleae_logic_pro_send_analog(sdi, devc->ana_channels[c],
				devc->ana_buf[c] + lpos + skip,
				samples);
		sr_sw_limits_update_samples_read(&devc->limits, samples);
	}

	devc->emit_pos = avail;
	devc->samples_in += block;
	/* Keep the last gd logic samples (not yet emitted) as history. */
	if (gd)
		memmove(devc->logic_buf, devc->logic_buf + block,
			gd * sizeof(uint16_t));

	/* Transfers still queued are reaped once acq_done is seen. */
	if (sr_sw_limits_check(&devc->limits))
		devc->acq_done = TRUE;
}
