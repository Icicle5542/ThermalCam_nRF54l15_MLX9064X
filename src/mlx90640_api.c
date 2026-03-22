/**
 * MLX90640 API — Pure C port of the Melexis reference implementation.
 *
 * Original: Copyright (C) 2017 Melexis N.V.  (Apache-2.0)
 * Ported to plain C for Zephyr / nRF54L15.
 *
 * Key changes from the ESP32/Arduino C++ original:
 *   - pow() / sqrt() replaced with powf() / sqrtf() (float, not double)
 *   - x^3 expanded to x*x*x to avoid a pow() call in the hot path
 *   - All C++ class/namespace constructs removed (none were present)
 *   - Compiled as C99
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdint.h>

#include "mlx90640_api.h"
#include "mlx90640_i2c.h"

/* ---- Forward declarations for private helpers ---- */
static void ExtractVDDParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractPTATParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractGainParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractTgcParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractResolutionParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractKsTaParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractKsToParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractAlphaParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractOffsetParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractKtaPixelParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractKvPixelParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractCPParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static void ExtractCILCParameters(uint16_t *eeData, paramsMLX90640 *mlx90640);
static int  ExtractDeviatingPixels(uint16_t *eeData, paramsMLX90640 *mlx90640);
static int  CheckAdjacentPixels(uint16_t pix1, uint16_t pix2);
static int  CheckEEPROMValid(uint16_t *eeData);

/* ============================================================
 * Public API
 * ============================================================ */

int MLX90640_DumpEE(uint8_t slaveAddr, uint16_t *eeData)
{
    return MLX90640_I2CRead(slaveAddr, 0x2400, 832, eeData);
}

int MLX90640_GetFrameData(uint8_t slaveAddr, uint16_t *frameData)
{
    uint16_t statusRegister;
    uint16_t controlRegister1;
    uint16_t dataReady = 0;
    int error;
    uint8_t cnt = 0;

    /* Wait until the sensor raises the data-ready flag. */
    while (dataReady == 0) {
        error = MLX90640_I2CRead(slaveAddr, 0x8000, 1, &statusRegister);
        if (error != 0) {
            return error;
        }
        dataReady = statusRegister & 0x0008;
    }

    /* Clear the flag and read the pixel RAM (up to 5 attempts). */
    while (dataReady != 0 && cnt < 5) {
        error = MLX90640_I2CWrite(slaveAddr, 0x8000, 0x0030);
        if (error == -1) {
            return error;
        }

        error = MLX90640_I2CRead(slaveAddr, 0x0400, 832, frameData);
        if (error != 0) {
            return error;
        }

        error = MLX90640_I2CRead(slaveAddr, 0x8000, 1, &statusRegister);
        if (error != 0) {
            return error;
        }
        dataReady = statusRegister & 0x0008;
        cnt++;
    }

    if (cnt > 4) {
        return -8;
    }

    error = MLX90640_I2CRead(slaveAddr, 0x800D, 1, &controlRegister1);
    if (error != 0) {
        return error;
    }

    /* Store control register and sub-page in the trailer words. */
    frameData[832] = controlRegister1;
    frameData[833] = statusRegister & 0x0001;

    return (int)frameData[833];
}

int MLX90640_ExtractParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    int error = CheckEEPROMValid(eeData);

    if (error == 0) {
        ExtractVDDParameters(eeData, mlx90640);
        ExtractPTATParameters(eeData, mlx90640);
        ExtractGainParameters(eeData, mlx90640);
        ExtractTgcParameters(eeData, mlx90640);
        ExtractResolutionParameters(eeData, mlx90640);
        ExtractKsTaParameters(eeData, mlx90640);
        ExtractKsToParameters(eeData, mlx90640);
        ExtractAlphaParameters(eeData, mlx90640);
        ExtractOffsetParameters(eeData, mlx90640);
        ExtractKtaPixelParameters(eeData, mlx90640);
        ExtractKvPixelParameters(eeData, mlx90640);
        ExtractCPParameters(eeData, mlx90640);
        ExtractCILCParameters(eeData, mlx90640);
        error = ExtractDeviatingPixels(eeData, mlx90640);
    }

    return error;
}

int MLX90640_SetResolution(uint8_t slaveAddr, uint8_t resolution)
{
    uint16_t controlRegister1;
    int error = MLX90640_I2CRead(slaveAddr, 0x800D, 1, &controlRegister1);

    if (error == 0) {
        int value = (controlRegister1 & 0xF3FF) | ((resolution & 0x03) << 10);
        error = MLX90640_I2CWrite(slaveAddr, 0x800D, (uint16_t)value);
    }
    return error;
}

int MLX90640_GetCurResolution(uint8_t slaveAddr)
{
    uint16_t controlRegister1;
    int error = MLX90640_I2CRead(slaveAddr, 0x800D, 1, &controlRegister1);

    if (error != 0) {
        return error;
    }
    return (controlRegister1 & 0x0C00) >> 10;
}

int MLX90640_SetRefreshRate(uint8_t slaveAddr, uint8_t refreshRate)
{
    uint16_t controlRegister1;
    int error = MLX90640_I2CRead(slaveAddr, 0x800D, 1, &controlRegister1);

    if (error == 0) {
        int value = (controlRegister1 & 0xFC7F) | ((refreshRate & 0x07) << 7);
        error = MLX90640_I2CWrite(slaveAddr, 0x800D, (uint16_t)value);
    }
    return error;
}

int MLX90640_GetRefreshRate(uint8_t slaveAddr)
{
    uint16_t controlRegister1;
    int error = MLX90640_I2CRead(slaveAddr, 0x800D, 1, &controlRegister1);

    if (error != 0) {
        return error;
    }
    return (controlRegister1 & 0x0380) >> 7;
}

int MLX90640_SetInterleavedMode(uint8_t slaveAddr)
{
    uint16_t controlRegister1;
    int error = MLX90640_I2CRead(slaveAddr, 0x800D, 1, &controlRegister1);

    if (error == 0) {
        error = MLX90640_I2CWrite(slaveAddr, 0x800D,
                                  (uint16_t)(controlRegister1 & 0xEFFF));
    }
    return error;
}

int MLX90640_SetChessMode(uint8_t slaveAddr)
{
    uint16_t controlRegister1;
    int error = MLX90640_I2CRead(slaveAddr, 0x800D, 1, &controlRegister1);

    if (error == 0) {
        error = MLX90640_I2CWrite(slaveAddr, 0x800D,
                                  (uint16_t)(controlRegister1 | 0x1000));
    }
    return error;
}

int MLX90640_GetCurMode(uint8_t slaveAddr)
{
    uint16_t controlRegister1;
    int error = MLX90640_I2CRead(slaveAddr, 0x800D, 1, &controlRegister1);

    if (error != 0) {
        return error;
    }
    return (controlRegister1 & 0x1000) >> 12;
}

int MLX90640_GetSubPageNumber(uint16_t *frameData)
{
    return (int)frameData[833];
}

float MLX90640_GetVdd(uint16_t *frameData, const paramsMLX90640 *params)
{
    float vdd = (float)frameData[810];

    if (vdd > 32767.0f) {
        vdd -= 65536.0f;
    }

    int resolutionRAM = (frameData[832] & 0x0C00) >> 10;
    float resolutionCorrection = powf(2.0f, (float)params->resolutionEE)
                               / powf(2.0f, (float)resolutionRAM);

    vdd = (resolutionCorrection * vdd - (float)params->vdd25)
          / (float)params->kVdd + 3.3f;

    return vdd;
}

float MLX90640_GetTa(uint16_t *frameData, const paramsMLX90640 *params)
{
    float vdd = MLX90640_GetVdd(frameData, params);

    float ptat = (float)frameData[800];
    if (ptat > 32767.0f) {
        ptat -= 65536.0f;
    }

    float ptatArt = (float)frameData[768];
    if (ptatArt > 32767.0f) {
        ptatArt -= 65536.0f;
    }

    ptatArt = (ptat / (ptat * params->alphaPTAT + ptatArt))
              * powf(2.0f, 18.0f);

    float ta = (ptatArt / (1.0f + params->KvPTAT * (vdd - 3.3f))
                - (float)params->vPTAT25);
    ta = ta / params->KtPTAT + 25.0f;

    return ta;
}

void MLX90640_CalculateTo(uint16_t *frameData, const paramsMLX90640 *params,
                          float emissivity, float tr, float *result)
{
    float vdd = MLX90640_GetVdd(frameData, params);
    float ta  = MLX90640_GetTa(frameData, params);

    float ta4  = (ta  + 273.15f) * (ta  + 273.15f) * (ta  + 273.15f) * (ta  + 273.15f);
    float tr4  = (tr  + 273.15f) * (tr  + 273.15f) * (tr  + 273.15f) * (tr  + 273.15f);
    float taTr = tr4 - (tr4 - ta4) / emissivity;

    float alphaCorrR[4];
    alphaCorrR[0] = 1.0f / (1.0f + params->ksTo[0] * 40.0f);
    alphaCorrR[1] = 1.0f;
    alphaCorrR[2] = 1.0f + params->ksTo[2] * (float)params->ct[2];
    alphaCorrR[3] = alphaCorrR[2] * (1.0f + params->ksTo[3]
                    * (float)(params->ct[3] - params->ct[2]));

    /* ---- Gain ---- */
    float gain = (float)frameData[778];
    if (gain > 32767.0f) {
        gain -= 65536.0f;
    }
    gain = (float)params->gainEE / gain;

    /* ---- Compensation pixel IR data ---- */
    uint8_t mode = (uint8_t)((frameData[832] & 0x1000) >> 5);
    uint16_t subPage = frameData[833];

    float irDataCP[2];
    irDataCP[0] = (float)frameData[776];
    irDataCP[1] = (float)frameData[808];

    for (int i = 0; i < 2; i++) {
        if (irDataCP[i] > 32767.0f) {
            irDataCP[i] -= 65536.0f;
        }
        irDataCP[i] *= gain;
    }

    irDataCP[0] -= params->cpOffset[0]
                   * (1.0f + params->cpKta * (ta - 25.0f))
                   * (1.0f + params->cpKv * (vdd - 3.3f));

    if (mode == params->calibrationModeEE) {
        irDataCP[1] -= params->cpOffset[1]
                       * (1.0f + params->cpKta * (ta - 25.0f))
                       * (1.0f + params->cpKv * (vdd - 3.3f));
    } else {
        irDataCP[1] -= (params->cpOffset[1] + params->ilChessC[0])
                       * (1.0f + params->cpKta * (ta - 25.0f))
                       * (1.0f + params->cpKv * (vdd - 3.3f));
    }

    /* ---- Per-pixel temperature calculation ---- */
    for (int pixelNumber = 0; pixelNumber < 768; pixelNumber++) {
        int8_t ilPattern       = (int8_t)(pixelNumber / 32 - (pixelNumber / 64) * 2);
        int8_t chessPattern    = (int8_t)(ilPattern ^ (pixelNumber - (pixelNumber / 2) * 2));
        int8_t conversionPattern = (int8_t)(
            ((pixelNumber + 2) / 4 - (pixelNumber + 3) / 4
             + (pixelNumber + 1) / 4 - pixelNumber / 4)
            * (1 - 2 * ilPattern));

        int8_t pattern = (mode == 0) ? ilPattern : chessPattern;

        if (pattern != (int8_t)frameData[833]) {
            continue;
        }

        float irData = (float)frameData[pixelNumber];
        if (irData > 32767.0f) {
            irData -= 65536.0f;
        }
        irData *= gain;

        irData -= params->offset[pixelNumber]
                  * (1.0f + params->kta[pixelNumber] * (ta - 25.0f))
                  * (1.0f + params->kv[pixelNumber]  * (vdd - 3.3f));

        if (mode != params->calibrationModeEE) {
            irData += params->ilChessC[2] * (2.0f * ilPattern - 1.0f)
                    - params->ilChessC[1] * conversionPattern;
        }

        irData /= emissivity;
        irData -= params->tgc * irDataCP[subPage];

        float alphaCompensated =
            (params->alpha[pixelNumber] - params->tgc * params->cpAlpha[subPage])
            * (1.0f + params->KsTa * (ta - 25.0f));

        /* Stefan-Boltzmann — first estimate of To */
        float Sx = alphaCompensated * alphaCompensated * alphaCompensated
                   * (irData + alphaCompensated * taTr);
        Sx = sqrtf(sqrtf(Sx)) * params->ksTo[1];

        float To = sqrtf(sqrtf(
            irData / (alphaCompensated * (1.0f - params->ksTo[1] * 273.15f) + Sx)
            + taTr)) - 273.15f;

        /* Select temperature range and refine */
        int8_t range;
        if (To < (float)params->ct[1]) {
            range = 0;
        } else if (To < (float)params->ct[2]) {
            range = 1;
        } else if (To < (float)params->ct[3]) {
            range = 2;
        } else {
            range = 3;
        }

        To = sqrtf(sqrtf(
            irData / (alphaCompensated * alphaCorrR[range]
                      * (1.0f + params->ksTo[range] * (To - (float)params->ct[range])))
            + taTr)) - 273.15f;

        result[pixelNumber] = To;
    }
}

void MLX90640_GetImage(uint16_t *frameData, const paramsMLX90640 *params,
                       float *result)
{
    float vdd = MLX90640_GetVdd(frameData, params);
    float ta  = MLX90640_GetTa(frameData, params);

    float gain = (float)frameData[778];
    if (gain > 32767.0f) {
        gain -= 65536.0f;
    }
    gain = (float)params->gainEE / gain;

    uint8_t  mode    = (uint8_t)((frameData[832] & 0x1000) >> 5);
    uint16_t subPage = frameData[833];

    float irDataCP[2];
    irDataCP[0] = (float)frameData[776];
    irDataCP[1] = (float)frameData[808];

    for (int i = 0; i < 2; i++) {
        if (irDataCP[i] > 32767.0f) {
            irDataCP[i] -= 65536.0f;
        }
        irDataCP[i] *= gain;
    }

    irDataCP[0] -= params->cpOffset[0]
                   * (1.0f + params->cpKta * (ta - 25.0f))
                   * (1.0f + params->cpKv * (vdd - 3.3f));
    if (mode == params->calibrationModeEE) {
        irDataCP[1] -= params->cpOffset[1]
                       * (1.0f + params->cpKta * (ta - 25.0f))
                       * (1.0f + params->cpKv * (vdd - 3.3f));
    } else {
        irDataCP[1] -= (params->cpOffset[1] + params->ilChessC[0])
                       * (1.0f + params->cpKta * (ta - 25.0f))
                       * (1.0f + params->cpKv * (vdd - 3.3f));
    }

    for (int pixelNumber = 0; pixelNumber < 768; pixelNumber++) {
        int8_t ilPattern    = (int8_t)(pixelNumber / 32 - (pixelNumber / 64) * 2);
        int8_t chessPattern = (int8_t)(ilPattern ^ (pixelNumber - (pixelNumber / 2) * 2));
        int8_t conversionPattern = (int8_t)(
            ((pixelNumber + 2) / 4 - (pixelNumber + 3) / 4
             + (pixelNumber + 1) / 4 - pixelNumber / 4)
            * (1 - 2 * ilPattern));

        int8_t pattern = (mode == 0) ? ilPattern : chessPattern;

        if (pattern != (int8_t)frameData[833]) {
            continue;
        }

        float irData = (float)frameData[pixelNumber];
        if (irData > 32767.0f) {
            irData -= 65536.0f;
        }
        irData *= gain;

        irData -= params->offset[pixelNumber]
                  * (1.0f + params->kta[pixelNumber] * (ta - 25.0f))
                  * (1.0f + params->kv[pixelNumber]  * (vdd - 3.3f));

        if (mode != params->calibrationModeEE) {
            irData += params->ilChessC[2] * (2.0f * ilPattern - 1.0f)
                    - params->ilChessC[1] * conversionPattern;
        }

        irData -= params->tgc * irDataCP[subPage];

        float alphaCompensated =
            (params->alpha[pixelNumber] - params->tgc * params->cpAlpha[subPage])
            * (1.0f + params->KsTa * (ta - 25.0f));

        result[pixelNumber] = irData / alphaCompensated;
    }
}

/* ============================================================
 * Private calibration extraction helpers
 * ============================================================ */

static void ExtractVDDParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    int16_t kVdd = (int16_t)((eeData[51] & 0xFF00) >> 8);
    if (kVdd > 127) {
        kVdd -= 256;
    }
    kVdd = (int16_t)(kVdd * 32);

    int16_t vdd25 = (int16_t)(eeData[51] & 0x00FF);
    vdd25 = (int16_t)(((vdd25 - 256) << 5) - 8192);

    mlx90640->kVdd  = kVdd;
    mlx90640->vdd25 = vdd25;
}

static void ExtractPTATParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    float KvPTAT = (float)((eeData[50] & 0xFC00) >> 10);
    if (KvPTAT > 31.0f) {
        KvPTAT -= 64.0f;
    }
    KvPTAT /= 4096.0f;

    float KtPTAT = (float)(eeData[50] & 0x03FF);
    if (KtPTAT > 511.0f) {
        KtPTAT -= 1024.0f;
    }
    KtPTAT /= 8.0f;

    int16_t vPTAT25 = (int16_t)eeData[49];
    float alphaPTAT = (float)(eeData[16] & 0xF000) / powf(2.0f, 14.0f) + 8.0f;

    mlx90640->KvPTAT    = KvPTAT;
    mlx90640->KtPTAT    = KtPTAT;
    mlx90640->vPTAT25   = (uint16_t)vPTAT25;
    mlx90640->alphaPTAT = alphaPTAT;
}

static void ExtractGainParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    int16_t gainEE = (int16_t)eeData[48];
    if (gainEE > 32767) {
        gainEE = (int16_t)(gainEE - 65536);
    }
    mlx90640->gainEE = gainEE;
}

static void ExtractTgcParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    float tgc = (float)(eeData[60] & 0x00FF);
    if (tgc > 127.0f) {
        tgc -= 256.0f;
    }
    mlx90640->tgc = tgc / 32.0f;
}

static void ExtractResolutionParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    mlx90640->resolutionEE = (uint8_t)((eeData[56] & 0x3000) >> 12);
}

static void ExtractKsTaParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    float KsTa = (float)((eeData[60] & 0xFF00) >> 8);
    if (KsTa > 127.0f) {
        KsTa -= 256.0f;
    }
    mlx90640->KsTa = KsTa / 8192.0f;
}

static void ExtractKsToParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    int8_t step = (int8_t)(((eeData[63] & 0x3000) >> 12) * 10);

    mlx90640->ct[0] = -40;
    mlx90640->ct[1] = 0;
    mlx90640->ct[2] = (int16_t)((eeData[63] & 0x00F0) >> 4);
    mlx90640->ct[3] = (int16_t)((eeData[63] & 0x0F00) >> 8);
    mlx90640->ct[2] = (int16_t)(mlx90640->ct[2] * step);
    mlx90640->ct[3] = (int16_t)(mlx90640->ct[2] + mlx90640->ct[3] * step);

    int KsToScale = (int)((eeData[63] & 0x000F) + 8);
    KsToScale = 1 << KsToScale;

    mlx90640->ksTo[0] = (float)(eeData[61] & 0x00FF);
    mlx90640->ksTo[1] = (float)((eeData[61] & 0xFF00) >> 8);
    mlx90640->ksTo[2] = (float)(eeData[62] & 0x00FF);
    mlx90640->ksTo[3] = (float)((eeData[62] & 0xFF00) >> 8);

    for (int i = 0; i < 4; i++) {
        if (mlx90640->ksTo[i] > 127.0f) {
            mlx90640->ksTo[i] -= 256.0f;
        }
        mlx90640->ksTo[i] /= (float)KsToScale;
    }
}

static void ExtractAlphaParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    int accRow[24];
    int accColumn[32];
    int p;

    uint8_t accRemScale    = (uint8_t)(eeData[32] & 0x000F);
    uint8_t accColumnScale = (uint8_t)((eeData[32] & 0x00F0) >> 4);
    uint8_t accRowScale    = (uint8_t)((eeData[32] & 0x0F00) >> 8);
    uint8_t alphaScale     = (uint8_t)(((eeData[32] & 0xF000) >> 12) + 30);
    int     alphaRef       = (int)eeData[33];

    for (int i = 0; i < 6; i++) {
        p = i * 4;
        accRow[p + 0] = (eeData[34 + i] & 0x000F);
        accRow[p + 1] = (eeData[34 + i] & 0x00F0) >> 4;
        accRow[p + 2] = (eeData[34 + i] & 0x0F00) >> 8;
        accRow[p + 3] = (eeData[34 + i] & 0xF000) >> 12;
    }
    for (int i = 0; i < 24; i++) {
        if (accRow[i] > 7) {
            accRow[i] -= 16;
        }
    }

    for (int i = 0; i < 8; i++) {
        p = i * 4;
        accColumn[p + 0] = (eeData[40 + i] & 0x000F);
        accColumn[p + 1] = (eeData[40 + i] & 0x00F0) >> 4;
        accColumn[p + 2] = (eeData[40 + i] & 0x0F00) >> 8;
        accColumn[p + 3] = (eeData[40 + i] & 0xF000) >> 12;
    }
    for (int i = 0; i < 32; i++) {
        if (accColumn[i] > 7) {
            accColumn[i] -= 16;
        }
    }

    for (int i = 0; i < 24; i++) {
        for (int j = 0; j < 32; j++) {
            p = 32 * i + j;
            mlx90640->alpha[p] = (float)((eeData[64 + p] & 0x03F0) >> 4);
            if (mlx90640->alpha[p] > 31.0f) {
                mlx90640->alpha[p] -= 64.0f;
            }
            mlx90640->alpha[p] *= (float)(1 << accRemScale);
            mlx90640->alpha[p] = (float)(alphaRef
                                  + (accRow[i]    << accRowScale)
                                  + (accColumn[j] << accColumnScale))
                                 + mlx90640->alpha[p];
            mlx90640->alpha[p] /= powf(2.0f, (float)alphaScale);
        }
    }
}

static void ExtractOffsetParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    int occRow[24];
    int occColumn[32];
    int p;

    uint8_t occRemScale    = (uint8_t)(eeData[16] & 0x000F);
    uint8_t occColumnScale = (uint8_t)((eeData[16] & 0x00F0) >> 4);
    uint8_t occRowScale    = (uint8_t)((eeData[16] & 0x0F00) >> 8);

    int16_t offsetRef = (int16_t)eeData[17];
    if (offsetRef > 32767) {
        offsetRef = (int16_t)(offsetRef - 65536);
    }

    for (int i = 0; i < 6; i++) {
        p = i * 4;
        occRow[p + 0] = (eeData[18 + i] & 0x000F);
        occRow[p + 1] = (eeData[18 + i] & 0x00F0) >> 4;
        occRow[p + 2] = (eeData[18 + i] & 0x0F00) >> 8;
        occRow[p + 3] = (eeData[18 + i] & 0xF000) >> 12;
    }
    for (int i = 0; i < 24; i++) {
        if (occRow[i] > 7) {
            occRow[i] -= 16;
        }
    }

    for (int i = 0; i < 8; i++) {
        p = i * 4;
        occColumn[p + 0] = (eeData[24 + i] & 0x000F);
        occColumn[p + 1] = (eeData[24 + i] & 0x00F0) >> 4;
        occColumn[p + 2] = (eeData[24 + i] & 0x0F00) >> 8;
        occColumn[p + 3] = (eeData[24 + i] & 0xF000) >> 12;
    }
    for (int i = 0; i < 32; i++) {
        if (occColumn[i] > 7) {
            occColumn[i] -= 16;
        }
    }

    for (int i = 0; i < 24; i++) {
        for (int j = 0; j < 32; j++) {
            p = 32 * i + j;
            mlx90640->offset[p] = (int16_t)((eeData[64 + p] & 0xFC00) >> 10);
            if (mlx90640->offset[p] > 31) {
                mlx90640->offset[p] = (int16_t)(mlx90640->offset[p] - 64);
            }
            mlx90640->offset[p] = (int16_t)(mlx90640->offset[p]
                                   * (1 << occRemScale));
            mlx90640->offset[p] = (int16_t)((int)offsetRef
                                   + (occRow[i]    << occRowScale)
                                   + (occColumn[j] << occColumnScale)
                                   + (int)mlx90640->offset[p]);
        }
    }
}

static void ExtractKtaPixelParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    int8_t KtaRC[4];

    int8_t KtaRoCo = (int8_t)((eeData[54] & 0xFF00) >> 8);
    if (KtaRoCo > 127) { KtaRoCo = (int8_t)(KtaRoCo - 256); }
    KtaRC[0] = KtaRoCo;

    int8_t KtaReCo = (int8_t)(eeData[54] & 0x00FF);
    if (KtaReCo > 127) { KtaReCo = (int8_t)(KtaReCo - 256); }
    KtaRC[2] = KtaReCo;

    int8_t KtaRoCe = (int8_t)((eeData[55] & 0xFF00) >> 8);
    if (KtaRoCe > 127) { KtaRoCe = (int8_t)(KtaRoCe - 256); }
    KtaRC[1] = KtaRoCe;

    int8_t KtaReCe = (int8_t)(eeData[55] & 0x00FF);
    if (KtaReCe > 127) { KtaReCe = (int8_t)(KtaReCe - 256); }
    KtaRC[3] = KtaReCe;

    uint8_t ktaScale1 = (uint8_t)(((eeData[56] & 0x00F0) >> 4) + 8);
    uint8_t ktaScale2 = (uint8_t)(eeData[56] & 0x000F);

    for (int i = 0; i < 24; i++) {
        for (int j = 0; j < 32; j++) {
            int p = 32 * i + j;
            int split = 2 * (p / 32 - (p / 64) * 2) + p % 2;

            mlx90640->kta[p] = (float)((eeData[64 + p] & 0x000E) >> 1);
            if (mlx90640->kta[p] > 3.0f) {
                mlx90640->kta[p] -= 8.0f;
            }
            mlx90640->kta[p] *= (float)(1 << ktaScale2);
            mlx90640->kta[p] = ((float)KtaRC[split] + mlx90640->kta[p])
                               / powf(2.0f, (float)ktaScale1);
        }
    }
}

static void ExtractKvPixelParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    int8_t KvT[4];

    int8_t KvRoCo = (int8_t)((eeData[52] & 0xF000) >> 12);
    if (KvRoCo > 7) { KvRoCo = (int8_t)(KvRoCo - 16); }
    KvT[0] = KvRoCo;

    int8_t KvReCo = (int8_t)((eeData[52] & 0x0F00) >> 8);
    if (KvReCo > 7) { KvReCo = (int8_t)(KvReCo - 16); }
    KvT[2] = KvReCo;

    int8_t KvRoCe = (int8_t)((eeData[52] & 0x00F0) >> 4);
    if (KvRoCe > 7) { KvRoCe = (int8_t)(KvRoCe - 16); }
    KvT[1] = KvRoCe;

    int8_t KvReCe = (int8_t)(eeData[52] & 0x000F);
    if (KvReCe > 7) { KvReCe = (int8_t)(KvReCe - 16); }
    KvT[3] = KvReCe;

    uint8_t kvScale = (uint8_t)((eeData[56] & 0x0F00) >> 8);

    for (int i = 0; i < 24; i++) {
        for (int j = 0; j < 32; j++) {
            int p = 32 * i + j;
            int split = 2 * (p / 32 - (p / 64) * 2) + p % 2;
            mlx90640->kv[p] = (float)KvT[split]
                              / powf(2.0f, (float)kvScale);
        }
    }
}

static void ExtractCPParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    uint8_t alphaScale = (uint8_t)(((eeData[32] & 0xF000) >> 12) + 27);

    float offsetSP[2];
    offsetSP[0] = (float)(eeData[58] & 0x03FF);
    if (offsetSP[0] > 511.0f) {
        offsetSP[0] -= 1024.0f;
    }
    offsetSP[1] = (float)((eeData[58] & 0xFC00) >> 10);
    if (offsetSP[1] > 31.0f) {
        offsetSP[1] -= 64.0f;
    }
    offsetSP[1] += offsetSP[0];

    float alphaSP[2];
    alphaSP[0] = (float)(eeData[57] & 0x03FF);
    if (alphaSP[0] > 511.0f) {
        alphaSP[0] -= 1024.0f;
    }
    alphaSP[0] /= powf(2.0f, (float)alphaScale);

    alphaSP[1] = (float)((eeData[57] & 0xFC00) >> 10);
    if (alphaSP[1] > 31.0f) {
        alphaSP[1] -= 64.0f;
    }
    alphaSP[1] = (1.0f + alphaSP[1] / 128.0f) * alphaSP[0];

    float cpKta = (float)(eeData[59] & 0x00FF);
    if (cpKta > 127.0f) {
        cpKta -= 256.0f;
    }
    uint8_t ktaScale1 = (uint8_t)(((eeData[56] & 0x00F0) >> 4) + 8);
    mlx90640->cpKta = cpKta / powf(2.0f, (float)ktaScale1);

    float cpKv = (float)((eeData[59] & 0xFF00) >> 8);
    if (cpKv > 127.0f) {
        cpKv -= 256.0f;
    }
    uint8_t kvScale = (uint8_t)((eeData[56] & 0x0F00) >> 8);
    mlx90640->cpKv = cpKv / powf(2.0f, (float)kvScale);

    mlx90640->cpAlpha[0] = alphaSP[0];
    mlx90640->cpAlpha[1] = alphaSP[1];
    mlx90640->cpOffset[0] = (int16_t)offsetSP[0];
    mlx90640->cpOffset[1] = (int16_t)offsetSP[1];
}

static void ExtractCILCParameters(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    uint8_t calibrationModeEE = (uint8_t)((eeData[10] & 0x0800) >> 4);
    calibrationModeEE ^= 0x80;

    float ilChessC0 = (float)(eeData[53] & 0x003F);
    if (ilChessC0 > 31.0f) { ilChessC0 -= 64.0f; }
    ilChessC0 /= 16.0f;

    float ilChessC1 = (float)((eeData[53] & 0x07C0) >> 6);
    if (ilChessC1 > 15.0f) { ilChessC1 -= 32.0f; }
    ilChessC1 /= 2.0f;

    float ilChessC2 = (float)((eeData[53] & 0xF800) >> 11);
    if (ilChessC2 > 15.0f) { ilChessC2 -= 32.0f; }
    ilChessC2 /= 8.0f;

    mlx90640->calibrationModeEE = calibrationModeEE;
    mlx90640->ilChessC[0] = ilChessC0;
    mlx90640->ilChessC[1] = ilChessC1;
    mlx90640->ilChessC[2] = ilChessC2;
}

static int ExtractDeviatingPixels(uint16_t *eeData, paramsMLX90640 *mlx90640)
{
    uint16_t brokenPixCnt  = 0;
    uint16_t outlierPixCnt = 0;
    int warn = 0;

    for (int i = 0; i < 5; i++) {
        mlx90640->brokenPixels[i]  = 0xFFFF;
        mlx90640->outlierPixels[i] = 0xFFFF;
    }

    for (uint16_t pixCnt = 0;
         pixCnt < 768 && brokenPixCnt < 5 && outlierPixCnt < 5;
         pixCnt++) {

        if (eeData[pixCnt + 64] == 0) {
            mlx90640->brokenPixels[brokenPixCnt++] = pixCnt;
        } else if ((eeData[pixCnt + 64] & 0x0001) != 0) {
            mlx90640->outlierPixels[outlierPixCnt++] = pixCnt;
        }
    }

    if (brokenPixCnt > 4) {
        return -3;
    }
    if (outlierPixCnt > 4) {
        return -4;
    }
    if ((brokenPixCnt + outlierPixCnt) > 4) {
        return -5;
    }

    for (uint16_t i = 0; i < brokenPixCnt; i++) {
        for (uint16_t j = (uint16_t)(i + 1); j < brokenPixCnt; j++) {
            warn = CheckAdjacentPixels(mlx90640->brokenPixels[i],
                                       mlx90640->brokenPixels[j]);
            if (warn != 0) { return warn; }
        }
    }
    for (uint16_t i = 0; i < outlierPixCnt; i++) {
        for (uint16_t j = (uint16_t)(i + 1); j < outlierPixCnt; j++) {
            warn = CheckAdjacentPixels(mlx90640->outlierPixels[i],
                                       mlx90640->outlierPixels[j]);
            if (warn != 0) { return warn; }
        }
    }
    for (uint16_t i = 0; i < brokenPixCnt; i++) {
        for (uint16_t j = 0; j < outlierPixCnt; j++) {
            warn = CheckAdjacentPixels(mlx90640->brokenPixels[i],
                                       mlx90640->outlierPixels[j]);
            if (warn != 0) { return warn; }
        }
    }

    return warn;
}

static int CheckAdjacentPixels(uint16_t pix1, uint16_t pix2)
{
    int diff = (int)pix1 - (int)pix2;

    if (diff > -34 && diff < -30) { return -6; }
    if (diff > -2  && diff <  2)  { return -6; }
    if (diff >  30 && diff <  34) { return -6; }

    return 0;
}

static int CheckEEPROMValid(uint16_t *eeData)
{
    return (eeData[10] & 0x0040) ? -7 : 0;
}
