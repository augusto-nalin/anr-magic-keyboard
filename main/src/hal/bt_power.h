#pragma once
#include <stdint.h>

// Power mode of the Classic link to the keyboard, and dropping it. Safe to call from any task.
#ifdef __cplusplus
extern "C" {
#endif

// Once, after the Bluetooth stack is up
void mkb_bt_power_init(void);

// Refuses sniff and hold on the link and wakes it if it's sleeping, reports go out every slot
void mkb_bt_power_keep_active(const uint8_t bda[6]);

// Allows sniff and hold again, the keyboard drops back into sniff by itself
void mkb_bt_power_allow_sniff(const uint8_t bda[6]);

// Asks for a sniff interval of max_slots (0.625ms each, rounded down to even). A sniff that is
// already short enough stays, otherwise the link wakes and sniffs again with the new interval.
// The keyboard may refuse, then it stays active until it asks for sniff itself.
void mkb_bt_power_request_sniff(const uint8_t bda[6], uint16_t max_slots);

// Drops the Classic link with this HCI handle at the controller, also one Bluedroid has already
// forgotten about
void mkb_bt_power_disconnect(uint16_t handle);

#ifdef __cplusplus
}
#endif
