/**
 * ThermalCam — MLX90640 thermal imaging on nRF54L15.
 *
 * Wiring: I2C20 on P1.2 (SCL) / P1.3 (SDA).  See app.overlay.
 * Sensor I2C address: 0x33 (MLX90640 default).
 *
 * The main() function configures the sensor once, then starts a periodic
 * thread that reads a thermal frame every MLX90640_READ_INTERVAL_S seconds
 * and renders it as an ASCII heat map on the RTT log.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include "mlx90640_api.h"
#include "mlx90640_i2c.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/* ---- Sensor settings ---- */
#define MLX90640_ADDR           0x33    /* Default 7-bit I2C address */
#define MLX90640_EMISSIVITY     0.95f   /* Typical for most surfaces */
#define MLX90640_TA_SHIFT       8.0f    /* °C shift applied to Ta for TR */
#define MLX90640_REFRESH_RATE   0x05    /* 16 Hz (register value) */
#define MLX90640_READ_INTERVAL_S 5      /* Seconds between frame captures */

/* ---- Image geometry ---- */
#define MLX90640_ROWS    24
#define MLX90640_COLS    32
#define MLX90640_PIXELS  (MLX90640_ROWS * MLX90640_COLS)  /* 768 */

/*
 * Large buffers are static/global to keep them off the thread stack.
 * Combined they consume ~9.5 KB of BSS.
 */
static uint16_t       s_ee_data[832];
static uint16_t       s_frame_data[834];
static float          s_temp_image[MLX90640_PIXELS];
static paramsMLX90640 s_mlx90640_params;

/* ---- Thermal thread ---- */
#define THERMAL_STACK_SIZE  8192
#define THERMAL_PRIORITY    5

K_THREAD_STACK_DEFINE(s_thermal_stack, THERMAL_STACK_SIZE);
static struct k_thread s_thermal_thread;

/*
 * 7-level thermal palette — cold to hot:
 *   ' '  background / very cold
 *   '.'  cool
 *   '+'  below ambient
 *   '*'  near ambient
 *   'o'  warm
 *   '#'  hot
 *   '@'  very hot
 */
static const char ASCII_PALETTE[] = " .+*o#@";
#define ASCII_LEVELS ((int)(sizeof(ASCII_PALETTE) - 1))  /* 7 */

/* ============================================================
 * mlx90640_configure
 *
 * Obtains the I2C20 device, initialises the low-level driver,
 * dumps the sensor EEPROM, extracts calibration data, and sets
 * the refresh rate.
 *
 * Returns 0 on success, negative errno on failure.
 * ============================================================ */
static int mlx90640_configure(void)
{
    const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c21));

    if (!device_is_ready(i2c_dev)) {
        LOG_ERR("I2C21 device not ready");
        return -ENODEV;
    }

    MLX90640_I2CInit(i2c_dev);
    LOG_INF("I2C21 initialised (SCL=P1.11 SDA=P1.12, 400 kHz)");

    /* Read the 832-word factory calibration EEPROM. */
    int ret = MLX90640_DumpEE(MLX90640_ADDR, s_ee_data);
    if (ret != 0) {
        LOG_ERR("Failed to dump EEPROM (err %d)", ret);
        return ret;
    }
    LOG_INF("EEPROM read OK");

    /* Parse EEPROM into per-pixel calibration parameters. */
    ret = MLX90640_ExtractParameters(s_ee_data, &s_mlx90640_params);
    if (ret != 0) {
        LOG_ERR("Parameter extraction failed (err %d)", ret);
        return ret;
    }
    LOG_INF("Calibration parameters extracted");

    /* Set hardware refresh rate to 16 Hz (register value 0x05). */
    ret = MLX90640_SetRefreshRate(MLX90640_ADDR, MLX90640_REFRESH_RATE);
    if (ret != 0) {
        LOG_ERR("Failed to set refresh rate (err %d)", ret);
        return ret;
    }
    LOG_INF("MLX90640 configured: 16 Hz refresh, emissivity=0.95");

    return 0;
}

/* ============================================================
 * mlx90640_read_image
 *
 * Reads two consecutive sub-frames (interleaved/chess mode).
 * After both sub-frames, s_temp_image[] holds temperatures for
 * all 768 pixels (24 rows × 32 columns), in °C.
 *
 * Returns 0 on success, negative on I2C or sensor error.
 * ============================================================ */
static int mlx90640_read_image(void)
{
    for (int subframe = 0; subframe < 2; subframe++) {
        int ret = MLX90640_GetFrameData(MLX90640_ADDR, s_frame_data);
        if (ret < 0) {
            LOG_ERR("GetFrameData failed (sub %d, err %d)", subframe, ret);
            return ret;
        }

        float vdd = MLX90640_GetVdd(s_frame_data, &s_mlx90640_params);
        float ta  = MLX90640_GetTa(s_frame_data,  &s_mlx90640_params);
        float tr  = ta - MLX90640_TA_SHIFT;   /* reflected temperature */

        MLX90640_CalculateTo(s_frame_data, &s_mlx90640_params,
                             MLX90640_EMISSIVITY, tr, s_temp_image);

        LOG_DBG("Sub-frame %d: VDD=%.2fV  Ta=%.1f°C  TR=%.1f°C",
                subframe, (double)vdd, (double)ta, (double)tr);
    }

    return 0;
}

/* ============================================================
 * mlx90640_display_ascii
 *
 * Maps the 768-pixel temperature array onto a 92-level greyscale
 * ASCII ramp and prints it via the UART logging back-end.
 *
 * Statistics printed:
 *   min  – coldest pixel temperature (°C)
 *   max  – hottest pixel temperature (°C)
 *   mean – average pixel temperature (°C)
 *   span – max − min (°C)
 *
 * Layout: 24 rows × 32 characters, bordered with '|' pipes.
 * ============================================================ */
static void mlx90640_display_ascii(void)
{
    /* ---- Compute statistics ---- */
    float t_min  = s_temp_image[0];
    float t_max  = s_temp_image[0];
    float t_sum  = 0.0f;

    for (int i = 0; i < MLX90640_PIXELS; i++) {
        float t = s_temp_image[i];
        if (t < t_min) { t_min = t; }
        if (t > t_max) { t_max = t; }
        t_sum += t;
    }

    float t_mean  = t_sum / (float)MLX90640_PIXELS;
    float t_span  = t_max - t_min;
    if (t_span < 0.1f) {
        t_span = 0.1f;   /* guard against flat scenes */
    }

    /* ---- Header with statistics ---- */
    LOG_INF("+--------------------------------+");
    LOG_INF("| MLX90640  %3d x %2d  greyscale  |",
            MLX90640_COLS, MLX90640_ROWS);
    LOG_INF("| min  %6d.%01d C                  |",
            (int)t_min,  (int)((t_min  < 0 ? -t_min  : t_min)  * 10) % 10);
    LOG_INF("| max  %6d.%01d C                  |",
            (int)t_max,  (int)((t_max  < 0 ? -t_max  : t_max)  * 10) % 10);
    LOG_INF("| mean %6d.%01d C                  |",
            (int)t_mean, (int)((t_mean < 0 ? -t_mean : t_mean) * 10) % 10);
    LOG_INF("| span %6d.%01d C                  |",
            (int)t_span, (int)(t_span * 10) % 10);
    LOG_INF("+--------------------------------+");

    /* ---- Image rows ---- */
    /* Row buffer: '|' + 32 chars + '|' + '\0' */
    char row_buf[MLX90640_COLS + 3];
    row_buf[0]                 = '|';
    row_buf[MLX90640_COLS + 1] = '|';
    row_buf[MLX90640_COLS + 2] = '\0';

    for (int row = 0; row < MLX90640_ROWS; row++) {
        for (int col = 0; col < MLX90640_COLS; col++) {
            float t = s_temp_image[row * MLX90640_COLS + col];
            int level = (int)((t - t_min) / t_span * (float)(ASCII_LEVELS - 1));

            if (level < 0)             { level = 0; }
            if (level >= ASCII_LEVELS) { level = ASCII_LEVELS - 1; }

            row_buf[col + 1] = ASCII_PALETTE[level];
        }
        LOG_INF("%s", row_buf);
    }

    LOG_INF("+--------------------------------+");
}

/* ============================================================
 * Thermal thread entry point
 * ============================================================ */
static void thermal_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (true) {
        int ret = mlx90640_read_image();

        if (ret == 0) {
            mlx90640_display_ascii();
        } else {
            LOG_ERR("Image read failed (err %d) — retrying in %d s",
                    ret, MLX90640_READ_INTERVAL_S);
        }

        k_sleep(K_SECONDS(MLX90640_READ_INTERVAL_S));
    }
}

/* ============================================================
 * main
 * ============================================================ */
int main(void)
{
    LOG_INF("ThermalCam starting (nRF54L15 + MLX90640)");

    int ret = mlx90640_configure();
    if (ret != 0) {
        LOG_ERR("Sensor configuration failed (err %d) — halting", ret);
        return ret;
    }

    k_thread_create(&s_thermal_thread,
                    s_thermal_stack, K_THREAD_STACK_SIZEOF(s_thermal_stack),
                    thermal_thread_fn, NULL, NULL, NULL,
                    THERMAL_PRIORITY, 0, K_NO_WAIT);

    k_thread_name_set(&s_thermal_thread, "thermal");

    LOG_INF("Thermal imaging thread started (reads every %d s)",
            MLX90640_READ_INTERVAL_S);

    return 0;
}

