/*
 * Copyright (c) 2019-2020 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_CRYPTO_OPTIGA_OPTIGA_PHY_H_
#define ZEPHYR_DRIVERS_CRYPTO_OPTIGA_OPTIGA_PHY_H_

#include <device.h>

/* 1 byte for register address on writes */
#define OPTIGA_PHY_HEADER_LEN 1

#define OPTIGA_PHY_DATA_REG_LEN (CONFIG_OPTIGA_HOST_BUFFER_SIZE - OPTIGA_PHY_HEADER_LEN)

struct physical_layer {
	uint16_t data_reg_len; /* DATA_REG_LEN negotiated with OPTIGA */
	uint8_t host_buf[CONFIG_OPTIGA_HOST_BUFFER_SIZE];
};

/* Flags in I2C_STATE from protocol specification Table 2-4 */
enum {
	OPTIGA_I2C_STATE_FLAG_BUSY              = 0x80,
	OPTIGA_I2C_STATE_FLAG_RESP_READY        = 0x40,
	OPTIGA_I2C_STATE_FLAG_SOFT_RESET        = 0x08,
	OPTIGA_I2C_STATE_FLAG_CONT_READ         = 0x04,
	OPTIGA_I2C_STATE_FLAG_REP_START         = 0x02,
	OPTIGA_I2C_STATE_FLAG_CLK_STRETCHING    = 0x01
};

int optiga_phy_init(struct device *dev);
uint8_t *optiga_phy_frame_buf(struct device *dev, size_t *len);
int optiga_phy_write_frame(struct device *dev, size_t len);
int optiga_phy_read_frame(struct device *dev, size_t *len);
int optiga_phy_get_i2c_state(struct device *dev, uint16_t *read_len, uint8_t *state_flags);

#endif /* ZEPHYR_DRIVERS_CRYPTO_OPTIGA_OPTIGA_PHY_H_ */