/**
 * MLX90640 I2C driver interface for Zephyr.
 *
 * Wraps the Zephyr I2C API to match the MLX90640 low-level driver
 * interface expected by mlx90640_api.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MLX90640_I2C_H
#define MLX90640_I2C_H

#include <stdint.h>
#include <zephyr/device.h>

/**
 * @brief Initialise the I2C driver with the given Zephyr I2C device.
 *
 * Must be called once before any other MLX90640_I2C* function.
 * The device must already be ready (device_is_ready() == true).
 */
void MLX90640_I2CInit(const struct device *i2c_device);

/**
 * @brief Read @p nWordsRead 16-bit words (big-endian on wire) from @p startAddress.
 *
 * @return 0 on success, negative errno on I2C error.
 */
int MLX90640_I2CRead(uint8_t slaveAddr, unsigned int startAddress,
                     unsigned int nWordsRead, uint16_t *data);

/**
 * @brief Write a single 16-bit word to @p writeAddress, then read back to verify.
 *
 * @return  0 on success.
 * @return -1 if the I2C write failed (no ACK).
 * @return -2 if the readback value does not match the written value.
 */
int MLX90640_I2CWrite(uint8_t slaveAddr, unsigned int writeAddress, uint16_t data);

#endif /* MLX90640_I2C_H */
