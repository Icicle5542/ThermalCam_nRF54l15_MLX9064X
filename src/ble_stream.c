/**
 * ble_stream.c — BLE NUS thermal frame streaming for ThermalCam.
 *
 * See ble_stream.h for the wire-protocol description.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <bluetooth/services/nus.h>
#include <zephyr/logging/log.h>

#include "ble_stream.h"

LOG_MODULE_REGISTER(ble_stream, LOG_LEVEL_INF);

/* ---- Frame geometry ---- */
#define FRAME_ROWS    24
#define FRAME_COLS    32
#define FRAME_PIXELS  (FRAME_ROWS * FRAME_COLS)              /* 768  */
#define FRAME_BUF_SZ  (2 + 2 + 2 + FRAME_PIXELS + 2)        /* 776  */

/* ---- Module-level state ---- */
static struct bt_conn *s_conn;          /* NULL when not connected */
static bool           s_notify_enabled; /* true once peer writes CCCD = 1 */
static K_SEM_DEFINE(s_bt_ready, 0, 1); /* signalled by bt_ready_cb */

/* ---- Advertising / scan-response data ---------------------------------- */

/*
 * Primary advertisement: flags + NUS service UUID.
 * The NUS UUID in the ad payload lets the Python bleak scanner (and nRF
 * Connect for Mobile) identify the device by service, not just name.
 */
static const struct bt_data s_adv[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

/*
 * Scan response: device name.
 * Splitting name into the scan response keeps the primary ad short while
 * still letting the PC identify the device by the "ThermalCam" name.
 */
static const struct bt_data s_sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE,
            CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* ---- BT controller-ready callback -------------------------------------- */

static void bt_ready_cb(int err)
{
    if (err) {
        LOG_ERR("bt_enable failed (err %d)", err);
        return;
    }
    k_sem_give(&s_bt_ready);
}

/* ---- Connection event callbacks ---------------------------------------- */

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed (err 0x%02x)", err);
        return;
    }
    s_conn = bt_conn_ref(conn);
    LOG_INF("BLE peer connected");
}

static void nus_send_enabled_cb(enum bt_nus_send_status status)
{
    s_notify_enabled = (status == BT_NUS_SEND_STATUS_ENABLED);
    LOG_INF("NUS notifications %s",
            s_notify_enabled ? "enabled" : "disabled");
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("BLE peer disconnected (reason 0x%02x)", reason);

    s_notify_enabled = false;

    if (s_conn) {
        bt_conn_unref(s_conn);
        s_conn = NULL;
    }

    /* Restart advertising so the PC can reconnect. */
    int ret = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
                              s_adv, ARRAY_SIZE(s_adv),
                              s_sd,  ARRAY_SIZE(s_sd));
    if (ret) {
        LOG_ERR("Advertising restart failed (err %d)", ret);
    } else {
        LOG_INF("Re-advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = connected_cb,
    .disconnected = disconnected_cb,
};

/* ============================================================
 * ble_stream_init
 * ============================================================ */
int ble_stream_init(void)
{
    /* Enable the BT controller; bt_ready_cb fires when ready. */
    int ret = bt_enable(bt_ready_cb);
    if (ret) {
        LOG_ERR("bt_enable failed (err %d)", ret);
        return ret;
    }
    k_sem_take(&s_bt_ready, K_FOREVER);
    LOG_INF("BT stack ready");

    /* Register NUS callbacks. */
    static struct bt_nus_cb nus_cb = {
        .send_enabled = nus_send_enabled_cb,
    };
    ret = bt_nus_init(&nus_cb);
    if (ret) {
        LOG_ERR("NUS init failed (err %d)", ret);
        return ret;
    }

    /* Start advertising. */
    ret = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
                          s_adv, ARRAY_SIZE(s_adv),
                          s_sd,  ARRAY_SIZE(s_sd));
    if (ret) {
        LOG_ERR("Advertising start failed (err %d)", ret);
        return ret;
    }

    LOG_INF("Advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
    return 0;
}

/* ============================================================
 * ble_stream_send_frame
 * ============================================================ */
void ble_stream_send_frame(const float *temp_image, int n_pixels)
{
    if (!s_conn) {
        LOG_DBG("No BLE peer connected — skipping frame");
        return;
    }

    if (!s_notify_enabled) {
        LOG_DBG("NUS notifications not yet enabled — skipping frame");
        return;
    }

    /* ---- Compute min / max over all pixels ---- */
    float t_min = temp_image[0];
    float t_max = temp_image[0];

    for (int i = 1; i < n_pixels; i++) {
        if (temp_image[i] < t_min) { t_min = temp_image[i]; }
        if (temp_image[i] > t_max) { t_max = temp_image[i]; }
    }

    float t_span = t_max - t_min;
    if (t_span < 0.1f) {
        t_span = 0.1f;   /* guard against a completely flat scene */
    }

    /* ---- Assemble the 776-byte frame (static — kept off the stack) ---- */
    static uint8_t buf[FRAME_BUF_SZ];

    /* SOF */
    buf[0] = 0xFF;
    buf[1] = 0xFE;

    /* t_min × 10 and t_max × 10 as big-endian int16_t */
    int16_t t_min_x10 = (int16_t)(t_min * 10.0f);
    int16_t t_max_x10 = (int16_t)(t_max * 10.0f);

    buf[2] = (uint8_t)((uint16_t)t_min_x10 >> 8);
    buf[3] = (uint8_t)((uint16_t)t_min_x10 & 0xFF);
    buf[4] = (uint8_t)((uint16_t)t_max_x10 >> 8);
    buf[5] = (uint8_t)((uint16_t)t_max_x10 & 0xFF);

    /* Greyscale pixels: 0 = coldest, 255 = hottest */
    for (int i = 0; i < n_pixels; i++) {
        float v = (temp_image[i] - t_min) / t_span * 255.0f;

        if (v <   0.0f) { v =   0.0f; }
        if (v > 255.0f) { v = 255.0f; }

        buf[6 + i] = (uint8_t)v;
    }

    /* EOF at fixed offset 774 */
    buf[6 + n_pixels]     = 0xFF;
    buf[6 + n_pixels + 1] = 0xFD;

    /* ---- Send in MTU-sized chunks via NUS TX notifications ---- */
    uint32_t mtu = bt_nus_get_mtu(s_conn);
    if (mtu == 0) {
        mtu = 20;  /* conservative fallback if MTU exchange has not occurred */
    }

    for (size_t offset = 0; offset < FRAME_BUF_SZ; ) {
        size_t chunk = MIN(mtu, (uint32_t)(FRAME_BUF_SZ - offset));
        int ret = bt_nus_send(s_conn, buf + offset, (uint16_t)chunk);

        if (ret) {
            LOG_ERR("bt_nus_send failed at offset %zu (err %d)", offset, ret);
            return;
        }
        offset += chunk;
    }

    LOG_INF("BLE frame sent (%d B)  min=%.1f C  max=%.1f C",
            FRAME_BUF_SZ, (double)t_min, (double)t_max);
}
