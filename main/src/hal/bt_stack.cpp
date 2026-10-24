#include "hal/bt_stack.h"

#include <esp_bt.h>
#include <esp_bt_main.h>

#include "utils/log.h"

namespace magic_keyboard::hal::bt {
    void start_stack() {
        esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_bt_controller_init(&controller_config));
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));

        esp_bluedroid_config_t bluedroid_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_config));
        ESP_ERROR_CHECK(esp_bluedroid_enable());

        LOG_INFO("bluetooth stack up");
    }
}
