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

#ifndef LIBSIGROK_HARDWARE_SALEAE_LOGIC_PRO_PROTOCOL_H
#define LIBSIGROK_HARDWARE_SALEAE_LOGIC_PRO_PROTOCOL_H

#include <stdint.h>
#include <glib.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

#define LOG_PREFIX "saleae-logic-pro"

/* Verified sustainable sample stream over USB 3 (bits per second); 2 Gbit/s overruns. */
#define MAX_BANDWIDTH_BPS	(1000ULL * 1000 * 1000)

#define MAX_ANALOG_CHANNELS	8
#define MAX_SOS_SECTIONS	8
#define MAX_GROUP_DELAY		64

/*
 * Sample stream USB transfers: sized for ~10 ms of data each (16..512 KiB),
 * with enough of them queued for ~0.4 s, so conversion hiccups don't let
 * the device FIFO overrun at high rates.
 */
#define XFER_MIN_SIZE		(16 * 1024)
#define XFER_MAX_SIZE		(512 * 1024)
#define XFER_MIN_COUNT		16
#define XFER_MAX_COUNT		256

/*
 * Largest frame: 16 digital words plus 4 rounds of 8 analog channels with
 * 3 words (8 x 12-bit samples) each.
 */
#define MAX_FRAME_WORDS		(16 + 4 * MAX_ANALOG_CHANNELS * 3)

/* One second-order IIR section, direct form I. */
struct sos {
	float b0, b1, b2, a1, a2;
	float x1, x2, y1, y2;
};

/* Per analog channel calibration, from the vendor calibration file. */
struct analog_cal {
	float vmin;		/* Voltage at code -2048. */
	float vmax;		/* Voltage at code +2047. */
	float scale, offset;	/* volts = code * scale + offset */
	unsigned int group_delay;
	gboolean primed;	/* Filter state initialised. */
	unsigned int num_sos;
	struct sos sos[MAX_SOS_SECTIONS];
};

struct saleae_logic_pro_profile {
	uint16_t pid;
	const char *model;
	unsigned int num_channels;
	unsigned int num_analog;
	const char *fw_name;
	const char *bitstream_name;
	/*
	 * Use the init sequence of the current vendor software (Lattice FPGA
	 * hardware). This also skips the legacy crypto handshake, which changes
	 * the LFSR state on the host side.
	 */
	gboolean logic2_init;
};

struct dev_context {
	const struct saleae_logic_pro_profile *profile;
	unsigned int dig_channel_cnt;
	uint16_t dig_channel_mask;
	uint16_t dig_channel_masks[16];
	uint8_t dig_channel_shift[16];
	uint64_t dig_samplerate;
	uint64_t device_id;

	/* Analog channels. */
	unsigned int ana_channel_cnt;
	uint8_t ana_channel_mask;
	struct sr_channel *ana_channels[MAX_ANALOG_CHANNELS];
	unsigned int ana_hw_index[MAX_ANALOG_CHANNELS];
	char *cal_json;			/* Vendor calibration file, if found. */
	struct analog_cal cal[MAX_ANALOG_CHANNELS];

	/* Stream frame layout, see saleae_logic_pro_prepare(). */
	unsigned int frame_dig_words;
	unsigned int frame_ana_words;
	unsigned int frame_samples;
	uint32_t *work;			/* Carried-over words + one transfer. */
	unsigned int carry_len;
	uint64_t samples_in;		/* Samples received so far. */
	uint64_t emit_pos;		/* Aligned samples emitted so far. */
	unsigned int ana_gd;		/* Analog filter group delay. */

	struct sr_sw_limits limits;
	gboolean acq_done;

	uint64_t capture_ratio;
	struct soft_trigger_logic *stl;
	gboolean trigger_fired;
	unsigned int unitsize;

	uint32_t lfsr;

	unsigned int num_transfers;
	unsigned int submitted_transfers;
	struct libusb_transfer **transfers;

	uint64_t stream_bps;		/* Sample stream, bits per second. */
	size_t xfer_size;
	size_t buf_samples;
	/* Logic delay line: ana_gd history samples, then the new block. */
	uint16_t *logic_buf;
	float *ana_buf[MAX_ANALOG_CHANNELS];
};

SR_PRIV int saleae_logic_pro_init(const struct sr_dev_inst *sdi);
SR_PRIV int saleae_logic_pro_prepare(const struct sr_dev_inst *sdi);
SR_PRIV int saleae_logic_pro_start(const struct sr_dev_inst *sdi);
SR_PRIV int saleae_logic_pro_stop(const struct sr_dev_inst *sdi);
SR_PRIV void LIBUSB_CALL saleae_logic_pro_receive_data(struct libusb_transfer *transfer);
SR_PRIV void saleae_logic_pro_load_calibration(const struct sr_dev_inst *sdi);

#endif
