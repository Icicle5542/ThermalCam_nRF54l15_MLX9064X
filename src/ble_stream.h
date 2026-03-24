/**
 * ble_stream.h — BLE Nordic UART Service (NUS) thermal frame streaming.
 *
 * Call ble_stream_init() once from main(), then call
 * ble_stream_send_frame() after each captured thermal image.
 *
 * Wire protocol (776 bytes per frame, big-endian integers):
 *
 *   Offset  Size  Content
 *        0     2  SOF  0xFF 0xFE
 *        2     2  t_min × 10  (int16_t BE)  — e.g. 227 = 22.7 °C
 *        4     2  t_max × 10  (int16_t BE)
 *        6   768  pixels[0..767]  uint8  0 = coldest, 255 = hottest
 *      774     2  EOF  0xFF 0xFD
 *
 * EOF is checked at the fixed offset 774 (not searched), so pixel data
 * that happens to contain the SOF byte pattern does not cause framing
 * errors on the receiver.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_STREAM_H
#define BLE_STREAM_H

/**
 * @brief Initialise the BT stack, register NUS, and start advertising
 *        as CONFIG_BT_DEVICE_NAME ("ThermalCam").
 *
 * Blocks until the BT controller signals readiness.  Safe to call once
 * from main() before starting the thermal thread.
 *
 * @return 0 on success, negative errno on failure.
 */
int ble_stream_init(void);

/**
 * @brief Encode and transmit one 32 × 24 thermal frame over BLE NUS.
 *
 * Normalises pixel temperatures to uint8 greyscale 0–255, prepends a
 * 4-byte header (t_min, t_max, both × 10 as int16_t BE), wraps in
 * SOF/EOF markers, and sends in MTU-sized chunks.  No-op when no BLE
 * peer is connected.
 *
 * @param temp_image  Array of @p n_pixels float temperatures in °C,
 *                    stored row-major (32 columns × 24 rows).
 * @param n_pixels    Number of pixels — must be 768 (MLX90640_PIXELS).
 */
void ble_stream_send_frame(const float *temp_image, int n_pixels);

#endif /* BLE_STREAM_H */
