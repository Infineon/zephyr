/*
 * Copyright (c) 2019-2020 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "crypto_optiga.h"
#include "optiga_phy.h"
#include "optiga_data.h"

#include <sys/byteorder.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(optiga_data, CONFIG_CRYPTO_LOG_LEVEL);

/* Length in bytes of the fields in a frame, see Figure 3-1 in protocol specification */
#define OPTIGA_DATA_FCTR_LEN 1
#define OPTIGA_DATA_LEN_LEN 2
#define OPTIGA_DATA_FCS_LEN 2

/* Offsets in frame header */
#define OPTIGA_DATA_FCTR_OFFSET 0
#define OPTIGA_DATA_LEN_OFFSET (OPTIGA_DATA_FCTR_OFFSET + OPTIGA_DATA_FCTR_LEN)
#define OPTIGA_DATA_PAYLOAD_OFFSET (OPTIGA_DATA_LEN_OFFSET + OPTIGA_DATA_LEN_LEN)

#define OPTIGA_DATA_FCTR_FTYPE_MASK 0x80
#define OPTIGA_DATA_FCTR_SEQCTR_MASK 0x60
#define OPTIGA_DATA_FCTR_FRNR_MASK 0x0C
#define OPTIGA_DATA_FCTR_ACKNR_MASK 0x03

/* Flags in the FCTR */
enum {
	/* FTYPE field */
	OPTIGA_DATA_FCTR_FTYPE_DATA     = 0x00,
	OPTIGA_DATA_FCTR_FTYPE_CTRL     = 0x80,
	/* SEQCTR field */
	OPTIGA_DATA_FCTR_SEQCTR_ACK     = 0x00,
	OPTIGA_DATA_FCTR_SEQCTR_NAK     = 0x20,
	OPTIGA_DATA_FCTR_SEQCTR_RST     = 0x40
};

/* Length of a sync frame is fixed */
#define OPTIGA_DATA_CRTL_FRAME_LEN (OPTIGA_DATA_FCTR_LEN + OPTIGA_DATA_LEN_LEN + OPTIGA_DATA_FCS_LEN)
#if OPTIGA_PHY_DATA_REG_LEN < OPTIGA_DATA_CRTL_FRAME_LEN
#error "Can't fit smallest frame in send buffer"
#endif

#define DATA_LINK_OVERHEAD (OPTIGA_DATA_HEADER_LEN + OPTIGA_DATA_TRAILER_LEN)

/*
 * Inner function of the FCS, initial seed is 0
 * From Appendix 8.1.2 of the protocol specification
 */
static uint16_t optiga_data_calc_fcs_core(uint16_t seed, uint8_t c)
{
	uint16_t h1 = (seed ^ c) & 0xFF;
	uint16_t h2 = h1 & 0x0F;
	uint16_t h3 = (h2 << 4) ^ h1;
	uint16_t h4 = h3 >> 4;

	return (((((h3 << 1) ^ h4) << 4) ^ h2) << 3) ^ h4 ^ (seed >> 8);
}

/*
 * @brief Calculate the frame check sequence of a frame
 *
 * @param frame_start Beginning of the frame
 * @param len The number of bytes in the frame without the FCS
 * @return Two byte frame check sequence
 */
static uint16_t optiga_data_frame_calc_fcs(const uint8_t *frame_start, size_t len)
{
	/* Initial seed is 0 */
	uint16_t fcs = 0;

	for (size_t i = 0; i < len; i++) {
		fcs = optiga_data_calc_fcs_core(fcs, frame_start[i]);
	}

	return fcs;
}

/*
 * @brief Verify the frame check sequence of a frame
 *
 * @param frame_start Beginning of the frame
 * @param len The number of bytes including the FCS
 * @return true if the FCS is correct, else false
 */
static bool optiga_data_frame_check_fcs(const uint8_t *frame_start, size_t len)
{
	__ASSERT(len > OPTIGA_DATA_FCS_LEN, "Not enough bytes");

	uint16_t calc_fcs = optiga_data_frame_calc_fcs(frame_start, len - 2);
	uint16_t recv_fcs = sys_get_be16(&frame_start[len - 2]);

	return calc_fcs == recv_fcs;
}

/*
 * @brief Append the frame check sequence
 *
 * @param frame_start Beginning of the frame
 * @param len Number of bytes of frame header + data
 *
 * @note The provided buffer must be big enough for the additional OPTIGA_DATA_FCS_LEN bytes
 */
static void optiga_data_frame_set_fcs(uint8_t *frame_start, size_t len)
{
	uint16_t fcs = optiga_data_frame_calc_fcs(frame_start, len);

	/* Chapter 3.3 says, order of FCS is: Low Byte || High Byte */
	sys_put_be16(fcs, &frame_start[len]);
}

/*
 * @brief Set LEN field in frame
 *
 * @param frame_start Beginning of the frame
 * @param len_value Value to write into LEN field
 *
 * @note The provided buffer must be big enough to contain the frame header
 */
static void optiga_data_frame_set_len(uint8_t *frame_start, uint16_t len_value)
{
	sys_put_be16(len_value, &frame_start[OPTIGA_DATA_LEN_OFFSET]);
}

/*
 * @brief Get LEN field from frame
 *
 * @param frame_start Beginning of the frame
 * @return Value in LEN field
 *
 * @note The provided buffer must be big enough to contain the frame header
 */
static uint16_t optiga_data_frame_get_len(const uint8_t *frame_start)
{
	return sys_get_be16(&frame_start[OPTIGA_DATA_LEN_OFFSET]);
}

static void optiga_data_frame_set_fctr(uint8_t *frame_start, uint8_t flags, uint8_t frame_nr, uint8_t frame_ack)
{
	/* ensure no bits are written outside their fields */
	__ASSERT(!(flags & ~(OPTIGA_DATA_FCTR_FTYPE_MASK | OPTIGA_DATA_FCTR_SEQCTR_MASK)), "Invalid flags");
	__ASSERT(!(frame_nr & 0xfc), "Invalid frame_nr");
	__ASSERT(!(frame_ack & 0xfc), "Invalid ack_nr");

	frame_start[OPTIGA_DATA_FCTR_OFFSET] = flags | frame_nr << 2 | frame_ack;
}

static bool optiga_data_is_ctrl_frame(const uint8_t *frame_start)
{
	return (frame_start[OPTIGA_DATA_FCTR_OFFSET] & OPTIGA_DATA_FCTR_FTYPE_MASK) == OPTIGA_DATA_FCTR_FTYPE_CTRL;
}

static uint8_t optiga_data_get_seqctr(const uint8_t *frame_start)
{
	return frame_start[OPTIGA_DATA_FCTR_OFFSET] & OPTIGA_DATA_FCTR_SEQCTR_MASK;
}

static uint8_t optiga_data_get_frame_nr(const uint8_t *frame_start)
{
	return (frame_start[OPTIGA_DATA_FCTR_OFFSET] & OPTIGA_DATA_FCTR_FRNR_MASK) >> 2;
}

static uint8_t optiga_data_get_ack_nr(const uint8_t *frame_start)
{
	return frame_start[OPTIGA_DATA_FCTR_OFFSET] & OPTIGA_DATA_FCTR_ACKNR_MASK;
}

static int optiga_send_ack_frame(struct device *dev)
{
	size_t buf_len = 0;
	uint8_t *frame = optiga_phy_frame_buf(dev, &buf_len);

	__ASSERT(buf_len >= OPTIGA_DATA_CRTL_FRAME_LEN, "Send buffer too small for ACK frame");

	/*
	 * Sending an ack frame would destroy the read buffer, so we backup
	 * the relevant bytes before sending and restore them afterwards.
	 * This works, because PHY doesn't have trailing bytes in the frame.
	 */
	uint8_t frame_bak[OPTIGA_DATA_CRTL_FRAME_LEN];

	memcpy(frame_bak, frame, OPTIGA_DATA_CRTL_FRAME_LEN);

	/* Assemble frame data */
	struct optiga_data *driver = dev->driver_data;

	optiga_data_frame_set_fctr(frame, OPTIGA_DATA_FCTR_FTYPE_CTRL | OPTIGA_DATA_FCTR_SEQCTR_ACK, 0, driver->data.frame_rx_nr);
	optiga_data_frame_set_len(frame, 0);
	optiga_data_frame_set_fcs(frame, OPTIGA_DATA_PAYLOAD_OFFSET);

	int err = optiga_phy_write_frame(dev, OPTIGA_DATA_CRTL_FRAME_LEN);

	/* restore previous frame content */
	memcpy(frame, frame_bak, OPTIGA_DATA_CRTL_FRAME_LEN);
	return err;
}

static int optiga_data_is_ctrl_frame_available(struct device *dev)
{
	uint16_t read_len = 0;
	int res = optiga_phy_get_i2c_state(dev, &read_len, NULL);

	if (res != 0) {
		return res;
	}

	if (read_len == OPTIGA_DATA_CRTL_FRAME_LEN) {
		return 1;
	}

	return 0;
}

static int optiga_data_recv_common(struct device *dev, uint8_t **recv_frame, size_t *recv_frame_len)
{
	int err = optiga_phy_read_frame(dev, recv_frame_len);

	if (err != 0) {
		LOG_ERR("Failed to read I2C_STATE");
		return err;
	}

	LOG_DBG("Frame len: %d", *recv_frame_len);

	if (*recv_frame_len < OPTIGA_DATA_CRTL_FRAME_LEN) {
		LOG_ERR("Invalid frame");
		return -EIO;
	}

	uint8_t *frame = optiga_phy_frame_buf(dev, NULL);

	/* Check FCS */
	bool fcs_good = optiga_data_frame_check_fcs(frame, *recv_frame_len);

	if (!fcs_good) {
		LOG_ERR("FCS error");
		return -EIO;
	}

	/* Frame header parsing */
	uint8_t seqctr = optiga_data_get_seqctr(frame);

	if (seqctr != OPTIGA_DATA_FCTR_SEQCTR_ACK) {
		LOG_ERR("Packet not acked");
		return -EIO;
	}

	/* check ack matches sent frame */
	uint8_t ack_nr = optiga_data_get_ack_nr(frame);
	struct optiga_data *driver = dev->driver_data;

	if (driver->data.frame_tx_nr == ack_nr) {
		/* frame nr was acknowledged, increase frame number for next send action */
		driver->data.frame_tx_nr = (driver->data.frame_tx_nr + 1) % 4;
		/* make this ack our last received one */
		driver->data.frame_tx_ack = ack_nr;
	} else if (driver->data.frame_tx_ack == ack_nr) {
		LOG_DBG("Received same ACK twice");
	} else {
		LOG_ERR("Wrong frame acknowledged");
		return -EIO;
	}

	*recv_frame = frame;
	return 0;
}

static int optiga_data_recv_ctrl_frame(struct device *dev)
{
	size_t ctrl_frame_len = 0;
	uint8_t *ctrl_frame_buf = NULL;
	int res = optiga_data_recv_common(dev, &ctrl_frame_buf, &ctrl_frame_len);

	if (res != 0) {
		return res;
	}

	if (ctrl_frame_len != OPTIGA_DATA_CRTL_FRAME_LEN) {
		LOG_ERR("Invalid control frame length");
		return -EIO;
	}

	bool ctrl_frame = optiga_data_is_ctrl_frame(ctrl_frame_buf);
	uint16_t frame_len = optiga_data_frame_get_len(ctrl_frame_buf);

	if (!ctrl_frame || frame_len != 0) {
		LOG_ERR("Invalid control frame");
		return -EIO;
	}

	return 0;
}

/* send a packet with the correct framing */
int optiga_data_send_packet(struct device *dev, size_t len)
{
	size_t max_frame_len = 0;
	uint8_t *const frame = optiga_phy_frame_buf(dev, &max_frame_len);

	if ((len + DATA_LINK_OVERHEAD) > max_frame_len) {
		LOG_ERR("Packet too big");
		return -EINVAL;
	}

	struct optiga_data *driver = dev->driver_data;
	uint8_t *const frame_nr = &driver->data.frame_tx_nr;
	uint8_t *const frame_ack = &driver->data.frame_rx_nr;

	/* Assemble frame header */
	optiga_data_frame_set_fctr(frame, OPTIGA_DATA_FCTR_FTYPE_DATA | OPTIGA_DATA_FCTR_SEQCTR_ACK, *frame_nr, *frame_ack);
	optiga_data_frame_set_len(frame, len);
	optiga_data_frame_set_fcs(frame, OPTIGA_DATA_PAYLOAD_OFFSET + len);

	int res = optiga_phy_write_frame(dev, len + DATA_LINK_OVERHEAD);

	if (res != 0) {
		LOG_ERR("Can't send data to phy");
		return res;
	}

	res = optiga_data_is_ctrl_frame_available(dev);
	if (res < 0) {
		LOG_ERR("Can't check for control frame");
		return res;
	}

	if (res == 1) {
		LOG_INF("Ctrl frame available, receiving");
		return optiga_data_recv_ctrl_frame(dev);
	}

	LOG_INF("No Ctrl frame available");
	return 0;
}

int optiga_data_recv_packet(struct device *dev, size_t *data_len)
{
	size_t rx_data_len = 0;
	uint8_t *rx_data_buf = NULL;

	int res = optiga_data_recv_common(dev, &rx_data_buf, &rx_data_len);
	if (res != 0) {
		return res;
	}

	const bool ctrl_frame = optiga_data_is_ctrl_frame(rx_data_buf);
	if (ctrl_frame) {
		LOG_ERR("Unexpected control frame");
		return -EIO;
	}

	LOG_DBG("Data frame");
	/* Ensure frame length matches */
	const uint16_t frame_len = optiga_data_frame_get_len(rx_data_buf);
	if ((frame_len + DATA_LINK_OVERHEAD) != rx_data_len) {
		LOG_ERR("Invalid frame length");
		return -EIO;
	}

	*data_len = frame_len;

	/* Acknowledge this frame */
	struct optiga_data *driver = dev->driver_data;

	driver->data.frame_rx_nr = optiga_data_get_frame_nr(rx_data_buf);
	return optiga_send_ack_frame(dev);
}

int optiga_data_init(struct device *dev)
{
	/* OPTIGA should be in synchronized state after reset */

	struct optiga_data *driver = dev->driver_data;

	driver->data.frame_tx_nr = 0;
	driver->data.frame_tx_ack = 0;
	driver->data.frame_rx_nr = 0;

	LOG_DBG("Data Link init successful");

	return 0;
}

/*
 * @brief Get a pointer to the packet space of the send/receive buffer
 *
 * @param dev Device to get the buffer
 * @param len If not NULL, the length of the buffer is returned
 * @return A pointer to the send/receive packet buffer of the device
 *
 * @note Use this way to access the send/receive buffer to avoid copying of the data.
 */
uint8_t *optiga_data_packet_buf(struct device *dev, size_t *len)
{
	size_t res_len = 0;
	uint8_t *res_buf = optiga_phy_frame_buf(dev, &res_len);

	__ASSERT(res_len > DATA_LINK_OVERHEAD, "PHY layer buffer too small");
	res_buf += OPTIGA_DATA_HEADER_LEN;
	res_len -= DATA_LINK_OVERHEAD;

	if (len) {
		*len = res_len;
	}

	return res_buf;
}


