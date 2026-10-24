// The one place that reaches into Bluedroid internals. ESP-IDF has no public API for a link's
// power mode or link policy, and the internal functions have to run on the BTU task like the rest
// of the stack, or they race with its own power manager. So this registers a tiny BTA subsystem
// and does the work from its event handler, which BTU runs. Expect to revisit on IDF upgrades.
#include "hal/bt_power.h"

#include <string.h>

#include <esp_log.h>

#include "common/bt_target.h"
#include "bta/bta_sys.h"
#include "osi/allocator.h"
#include "stack/btm_api.h"
#include "stack/hcidefs.h"

// From stack/hcimsgs.h, whose macros don't build with -Wcast-align=strict
BOOLEAN btsnd_hcic_disconnect(UINT16 handle, UINT8 reason);

// Printer client, not part of ESP-IDF, so its slot is free to borrow
#define BRIDGE_ID BTA_ID_PR

#define EVT_KEEP_ACTIVE ((UINT16)(BRIDGE_ID << 8))
#define EVT_ALLOW_SNIFF ((UINT16)((BRIDGE_ID << 8) | 1))
#define EVT_SET_SNIFF   ((UINT16)((BRIDGE_ID << 8) | 2))
#define EVT_DISCONNECT  ((UINT16)((BRIDGE_ID << 8) | 3))

// Slots the keyboard listens for at each sniff anchor (2 * attempt - 1), which has to stay below
// half the interval, and the extra slots it keeps listening after receiving something
#define SNIFF_ATTEMPT 2
#define SNIFF_TIMEOUT 1

static const char* TAG = "bt_power";

// Both make a report wait for the next slot
#define SLEEP_POLICY (HCI_ENABLE_SNIFF_MODE | HCI_ENABLE_HOLD_MODE)

typedef struct {
    BT_HDR hdr;
    BD_ADDR bda;
    UINT16 max_slots;
    UINT16 handle;
} power_msg_t;

// Runs on BTU. Clearing sniff from the policy also brings a sleeping link back to active.
static BOOLEAN on_event(BT_HDR* p_msg) {
    power_msg_t* msg = (power_msg_t*)p_msg;

    if (msg->hdr.event == EVT_KEEP_ACTIVE) {
        bta_sys_clear_policy(BTA_ID_HH, SLEEP_POLICY, msg->bda);
    } else if (msg->hdr.event == EVT_ALLOW_SNIFF) {
        bta_sys_set_policy(BTA_ID_HH, SLEEP_POLICY, msg->bda);
    } else if (msg->hdr.event == EVT_SET_SNIFF) {
        tBTM_PM_PWR_MD mode;
        memset(&mode, 0, sizeof(mode));
        mode.mode    = BTM_PM_MD_SNIFF;
        mode.max     = msg->max_slots;
        mode.min     = msg->max_slots;
        mode.attempt = SNIFF_ATTEMPT;
        mode.timeout = SNIFF_TIMEOUT;

        // 0 already fine, 1 started: waking the link first, then sniff with the new interval
        tBTM_STATUS status = BTM_SetPowerMode(BTM_PM_SET_ONLY_ID, msg->bda, &mode);
        ESP_LOGI(TAG, "sniff interval %u slots requested, status %d", msg->max_slots, status);
    } else if (msg->hdr.event == EVT_DISCONNECT) {
        // Straight to the controller: Bluedroid may no longer have a record of this link
        btsnd_hcic_disconnect(msg->handle, HCI_ERR_PEER_USER);
    }

    // BTA frees the message
    return TRUE;
}

static void on_disable(void) {}

static const tBTA_SYS_REG registration = {on_event, on_disable};

static power_msg_t* new_msg(UINT16 event) {
    power_msg_t* msg = (power_msg_t*)osi_malloc(sizeof(power_msg_t));
    if (msg != NULL) {
        memset(msg, 0, sizeof(power_msg_t));
        msg->hdr.event = event;
    }
    return msg;
}

static void post(UINT16 event, const uint8_t bda[6], UINT16 max_slots) {
    power_msg_t* msg = new_msg(event);
    if (msg == NULL) {
        return;
    }

    memcpy(msg->bda, bda, BD_ADDR_LEN);
    msg->max_slots = max_slots;
    bta_sys_sendmsg(msg);
}

void mkb_bt_power_init(void) {
    bta_sys_register(BRIDGE_ID, &registration);
}

void mkb_bt_power_keep_active(const uint8_t bda[6]) {
    post(EVT_KEEP_ACTIVE, bda, 0);
}

void mkb_bt_power_allow_sniff(const uint8_t bda[6]) {
    post(EVT_ALLOW_SNIFF, bda, 0);
}

void mkb_bt_power_request_sniff(const uint8_t bda[6], uint16_t max_slots) {
    // The controller wants an even interval, and two slots is the shortest there is
    uint16_t even = (uint16_t)(max_slots & ~1u);
    post(EVT_SET_SNIFF, bda, even < 2 ? 2 : even);
}

void mkb_bt_power_disconnect(uint16_t handle) {
    power_msg_t* msg = new_msg(EVT_DISCONNECT);
    if (msg == NULL) {
        return;
    }

    msg->handle = handle;
    bta_sys_sendmsg(msg);
}
