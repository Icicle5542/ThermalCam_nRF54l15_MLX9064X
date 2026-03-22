/**
 * MLX90640 API - Pure C port of the Melexis MLX90640 driver.
 *
 * Original: Copyright (C) 2017 Melexis N.V.
 * Ported for Zephyr / nRF54L15.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MLX90640_API_H
#define MLX90640_API_H

#include <stdint.h>

/**
 * @brief Calibration and configuration parameters extracted from the sensor EEPROM.
 *
 * All 768 pixels (24 rows × 32 columns) have individual calibration entries.
 */
typedef struct {
    /* Supply voltage parameters */
    int16_t kVdd;
    int16_t vdd25;

    /* PTAT (proportional-to-absolute-temperature) sensor parameters */
    float KvPTAT;
    float KtPTAT;
    uint16_t vPTAT25;
    float alphaPTAT;

    /* Gain */
    int16_t gainEE;

    /* Thermal gradient coefficient */
    float tgc;

    /* Compensation pixel coefficients */
    float cpKv;
    float cpKta;

    /* Resolution and calibration mode */
    uint8_t resolutionEE;
    uint8_t calibrationModeEE;

    /* Temperature sensitivity coefficients */
    float KsTa;
    float ksTo[4];
    int16_t ct[4];

    /* Per-pixel calibration arrays (768 pixels) */
    float   alpha[768];
    int16_t offset[768];
    float   kta[768];
    float   kv[768];

    /* Compensation pixel data */
    float   cpAlpha[2];
    int16_t cpOffset[2];

    /* Interleaved/Chess correction coefficients */
    float ilChessC[3];

    /* Defective pixel indices (0xFFFF = unused slot) */
    uint16_t brokenPixels[5];
    uint16_t outlierPixels[5];
} paramsMLX90640;

/* ---- I/O functions ---- */

/** Read 832 EEPROM words into eeData. Returns 0 on success, negative on error. */
int MLX90640_DumpEE(uint8_t slaveAddr, uint16_t *eeData);

/**
 * Read one frame of raw IR data (834 words) from the sensor.
 * Returns the sub-page number (0 or 1) on success, negative on error.
 */
int MLX90640_GetFrameData(uint8_t slaveAddr, uint16_t *frameData);

/**
 * Parse EEPROM words into calibration parameters.
 * Returns 0 on success, negative on error.
 */
int MLX90640_ExtractParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);

/* ---- Calculation functions ---- */

/** Return the supply voltage (VDD) measured during the last frame. */
float MLX90640_GetVdd(uint16_t *frameData, const paramsMLX90640 *params);

/** Return the ambient temperature (Ta, °C) measured during the last frame. */
float MLX90640_GetTa(uint16_t *frameData, const paramsMLX90640 *params);

/**
 * Convert raw frame data to absolute pixel temperatures (°C).
 *
 * @param frameData  834-word buffer from MLX90640_GetFrameData().
 * @param params     Calibration parameters from MLX90640_ExtractParameters().
 * @param emissivity Object emissivity (typically 0.95).
 * @param tr         Reflected ambient temperature = Ta - TA_SHIFT.
 * @param result     Output buffer for 768 temperatures (24×32 pixels, °C).
 */
void MLX90640_CalculateTo(uint16_t *frameData, const paramsMLX90640 *params,
                          float emissivity, float tr, float *result);

/**
 * Produce a compensated IR image (without absolute temperature conversion).
 * Useful for relative heat mapping.
 */
void MLX90640_GetImage(uint16_t *frameData, const paramsMLX90640 *params,
                       float *result);

/* ---- Configuration functions ---- */

int MLX90640_SetResolution(uint8_t slaveAddr, uint8_t resolution);
int MLX90640_GetCurResolution(uint8_t slaveAddr);
int MLX90640_SetRefreshRate(uint8_t slaveAddr, uint8_t refreshRate);
int MLX90640_GetRefreshRate(uint8_t slaveAddr);
int MLX90640_GetSubPageNumber(uint16_t *frameData);
int MLX90640_GetCurMode(uint8_t slaveAddr);
int MLX90640_SetInterleavedMode(uint8_t slaveAddr);
int MLX90640_SetChessMode(uint8_t slaveAddr);

#endif /* MLX90640_API_H */
