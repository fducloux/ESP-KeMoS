#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include "cfg_svc.h"

LOG_MODULE_REGISTER(cfg, LOG_LEVEL_INF);

// 5a6f0001-6b76-4d48-8f2e-7d9c1b2a3c4d (service), ...0002 state, ...0003 command
#define CFG_UUID(n) BT_UUID_128_ENCODE(0x5a6f0000 + (n), 0x6b76, 0x4d48, 0x8f2e, 0x7d9c1b2a3c4d)
static const struct bt_uuid_128 uuid_svc = BT_UUID_INIT_128(CFG_UUID(1));
static const struct bt_uuid_128 uuid_state = BT_UUID_INIT_128(CFG_UUID(2));
static const struct bt_uuid_128 uuid_cmd = BT_UUID_INIT_128(CFG_UUID(3));

static char snap[480];
static size_t snap_len;

static ssize_t read_state(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t off)
{
    if (!cfg_access_ok(conn)) return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    if (off == 0) snap_len = cfg_status(snap, sizeof(snap));      // a long read continues on the same snapshot
    return bt_gatt_attr_read(conn, attr, buf, len, off, snap, snap_len);
}

// commands run on the work queue, not in the Bluetooth thread
static char pending[64];
static struct k_work cmd_work;
static void cmd_fn(struct k_work *w) { char c[sizeof(pending)]; strcpy(c, pending); cfg_command(c); }

static ssize_t write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t off, uint8_t flags)
{
    if (!cfg_access_ok(conn)) return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
    if (off || len == 0 || len >= sizeof(pending)) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    if (k_work_busy_get(&cmd_work)) return BT_GATT_ERR(BT_ATT_ERR_PREPARE_QUEUE_FULL);
    memcpy(pending, buf, len);
    pending[len] = 0;
    k_work_submit(&cmd_work);
    return len;
}

BT_GATT_SERVICE_DEFINE(cfg_svc,
    BT_GATT_PRIMARY_SERVICE(&uuid_svc),
    BT_GATT_CHARACTERISTIC(&uuid_state.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_state, NULL, NULL),
    BT_GATT_CHARACTERISTIC(&uuid_cmd.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE, NULL, write_cmd, NULL),
);

static int cfg_init(void) { k_work_init(&cmd_work, cmd_fn); return 0; }
SYS_INIT(cfg_init, APPLICATION, 90);
