/*
 * Copyright (c) 2019-2020 Infineon Technologies AG
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_CRYPTO_OPTIGA_OPTIGA_NETTRAN_H_
#define ZEPHYR_DRIVERS_CRYPTO_OPTIGA_OPTIGA_NETTRAN_H_

#include <device.h>

/* 1 byte PCTR */
#define OPTIGA_NETTRAN_HEADER_LEN 1

/* network and transport */
struct nettran_layer {
	bool presence_flag;
};

int optiga_nettran_init(struct device *dev);
int optiga_nettran_send_apdu(struct device *dev, const uint8_t *data, size_t len);
int optiga_nettran_recv_apdu(struct device *dev, uint8_t *data, size_t *len);
void optiga_nettran_presence_enable(struct device *dev);
bool optiga_nettran_presence_get(struct device *dev);

#endif /* ZEPHYR_DRIVERS_CRYPTO_OPTIGA_OPTIGA_NETTRAN_H_ */