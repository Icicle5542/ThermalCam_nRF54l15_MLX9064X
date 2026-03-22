/**
 * MLX90640 I2C driver — Zephyr TWIM implementation.
 *
 * Implements the low-level I2C transfers required by mlx90640_api.c using
 * Zephyr's I2C API (i2c_write_read / i2c_write).
 *
 * The MLX90640 uses 16-bit register addresses and sends data MSB-first
 * over I2C. Byte order is fixed up after each read.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

#include "mlx90640_i2c.h"

LOG_MODULE_REGISTER(mlx90640_i2c, LOG_LEVEL_DBG);

static const struct device *s_i2c_dev;

void MLX90640_I2CInit(const struct device *i2c_device)
{
    s_i2c_dev = i2c_device;
}

int MLX90640_I2CRead(uint8_t slaveAddr, unsigned int startAddress,
                     unsigned int nWordsRead, uint16_t *data)
{
    uint8_t addr_buf[2] = {
        (uint8_t)(startAddress >> 8),
        (uint8_t)(startAddress & 0xFF)
    };

    /* Single combined write-then-read transfer (repeated START on the wire). */
    int ret = i2c_write_read(s_i2c_dev, (uint16_t)slaveAddr,
                             addr_buf, sizeof(addr_buf),
                             (uint8_t *)data, nWordsRead * 2U);
    if (ret != 0) {
        LOG_ERR("I2C read from addr 0x%04X failed: %d", startAddress, ret);
        return ret;
    }

    /*
     * The sensor sends each 16-bit word MSB-first (big-endian).
     * After the raw byte read, data[i] holds bytes [MSB, LSB] in memory,
     * which a little-endian CPU interprets as (LSB | MSB<<8) — wrong.
     * sys_be16_to_cpu() byte-swaps to give the correct value.
     */
    for (unsigned int i = 0; i < nWordsRead; i++) {
        data[i] = sys_be16_to_cpu(data[i]);
    }

    return 0;
}

int MLX90640_I2CWrite(uint8_t slaveAddr, unsigned int writeAddress, uint16_t data)
{
    uint8_t buf[4] = {
        (uint8_t)(writeAddress >> 8),
        (uint8_t)(writeAddress & 0xFF),
        (uint8_t)(data >> 8),
        (uint8_t)(data & 0xFF)
    };

    int ret = i2c_write(s_i2c_dev, buf, sizeof(buf), (uint16_t)slaveAddr);
    if (ret != 0) {
        LOG_ERR("I2C write to addr 0x%04X failed: %d", writeAddress, ret);
        return -1;
    }

    /* Verify the write by reading back and comparing. */
    uint16_t data_check;

    ret = MLX90640_I2CRead(slaveAddr, writeAddress, 1, &data_check);
    if (ret != 0) {
        return ret;
    }

    if (data_check != data) {
        /* A mismatch on a volatile/write-clear register (e.g. 0x8000) is
         * expected — the sensor updates it autonomously between the write
         * and the readback.  The caller decides whether -2 is fatal. */
        LOG_DBG("Write verify at 0x%04X: wrote 0x%04X, read 0x%04X (volatile reg, normal)",
                writeAddress, data, data_check);
        return -2;
    }

    return 0;
}
