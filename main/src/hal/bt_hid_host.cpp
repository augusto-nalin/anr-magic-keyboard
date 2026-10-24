#include "hal/bt_hid_host.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <esp_bt_defs.h>
#include <esp_gap_bt_api.h>
#include <esp_gattc_api.h>
#include <esp_hidh.h>
#include <esp_hidh_gattc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "hal/bt_power.h"
#include "utils/log.h"

namespace magic_keyboard::hal::bt {
    namespace {
        constexpr std::string_view TARGET_NAME{"Magic Keyboard"};

        // In 1.28s units, ~10s
        constexpr uint8_t INQUIRY_LENGTH{8};

        // A bonded keyboard connects to us by itself when a key is pressed, but after it has been
        // used with another host it calls that one instead, and then sits waiting for a call. So
        // once it has been gone a while, call it like at boot, and keep calling until it answers.
        // A sleeping keyboard doesn't answer, so this doesn't wake it.
        constexpr uint64_t RETRY_FIRST_US{20'000'000};
        constexpr uint64_t RETRY_INTERVAL_US{15'000'000};

        // How long one call rings, in 0.625ms slots: 2.56s instead of Bluedroid's 5.12s, a call
        // still ringing when the keyboard calls us is what leaves its link half open (below)
        constexpr uint16_t PAGE_TIMEOUT{0x1000};

        // Class of Device: major class in bits 8-12, keyboard flag in bit 6 (minor class bits 2-7)
        constexpr uint32_t COD_MAJOR_PERIPHERAL{0x05};
        constexpr uint32_t COD_KEYBOARD{0x40};

        // Legacy pairing only: typed on the keyboard, followed by Enter
        constexpr char LEGACY_PIN[]{"0000"};

        // Keys input report, which also carries the LED output
        constexpr size_t KEYS_REPORT_ID{1};

        esp_bd_addr_t target_bda{};
        std::atomic<bool> target_found{false};
        std::atomic<bool> bonded{false};
        std::atomic<bool> connected{false};
        std::atomic<esp_hidh_dev_t*> device{nullptr};
        esp_timer_handle_t retry_timer{nullptr};
        TaskHandle_t open_task{nullptr};
        hid_host::report_handler report_callback{};

        // Our own connect at boot can still be paging when the keyboard connects to us. When that
        // page then fails, Bluedroid tears down its state for the keyboard's working link but never
        // disconnects it: the keyboard keeps sending into the void. Such a link never reports an
        // ACL disconnect, so one still up a moment after HID closed gets dropped, and the keyboard
        // connects again cleanly on the next key. After a restart of ours the keyboard calls back
        // just as we call it at boot, and with no HID close then, a link that HID hasn't opened
        // on a while after it came up gets the same treatment.
        constexpr uint16_t NO_LINK{0xFFFF};
        constexpr uint64_t HALF_OPEN_CHECK_US{2'000'000};
        constexpr uint64_t HID_OPEN_CHECK_US{5'000'000};
        std::atomic<uint16_t> keyboard_link{NO_LINK};
        esp_timer_handle_t half_open_timer{nullptr};

        void on_half_open_check(void*) {
            if (connected) {
                return;
            }

            const uint16_t link = keyboard_link.exchange(NO_LINK);
            if (link == NO_LINK) {
                return;
            }

            LOG_WARN("keyboard link 0x%03x left half open, dropping it", link);
            mkb_bt_power_disconnect(link);
        }

        void on_acl_connected(const esp_bt_gap_cb_param_t::acl_conn_cmpl_stat_param& acl) {
            if (acl.stat == ESP_BT_STATUS_SUCCESS && std::memcmp(acl.bda, target_bda, sizeof(esp_bd_addr_t)) == 0) {
                keyboard_link = acl.handle;
                esp_timer_stop(half_open_timer);
                esp_timer_start_once(half_open_timer, HID_OPEN_CHECK_US);
            }
        }

        void on_acl_disconnected(const esp_bt_gap_cb_param_t::acl_disconn_cmpl_stat_param& acl) {
            uint16_t link = acl.handle;
            keyboard_link.compare_exchange_strong(link, NO_LINK);
        }

#if CONFIG_MKB_KEYBOARD_POWER_IDLE
        constexpr uint64_t IDLE_TIMEOUT_US{CONFIG_MKB_KEYBOARD_IDLE_SECONDS * 1000000ULL};
        esp_timer_handle_t idle_timer{nullptr};
        std::atomic<bool> link_active{false};

        void on_idle(void*) {
            link_active = false;
            LOG_INFO("keyboard idle, power saving allowed");
            mkb_bt_power_allow_sniff(target_bda);
        }
#endif

#if CONFIG_MKB_KEYBOARD_SNIFF_SLOTS > 0
        // Bluedroid and the keyboard sort sniff out among themselves around connecting, and the
        // keyboard asks for its own 15ms again whenever it re-enters sniff, so ask after it settles
        constexpr uint64_t SNIFF_SETTLE_US{1'000'000};
        esp_timer_handle_t sniff_timer{nullptr};

        void on_sniff_timer(void*) {
            mkb_bt_power_request_sniff(target_bda, CONFIG_MKB_KEYBOARD_SNIFF_SLOTS);
        }

        void schedule_sniff_request() {
            if (esp_timer_is_active(sniff_timer)) {
                esp_timer_restart(sniff_timer, SNIFF_SETTLE_US);
            } else {
                esp_timer_start_once(sniff_timer, SNIFF_SETTLE_US);
            }
        }
#endif

        // Any report: keep the link out of sniff, and allow it again once the keyboard goes quiet
        void on_activity() {
#if CONFIG_MKB_KEYBOARD_POWER_IDLE
            if (!link_active.exchange(true)) {
                LOG_INFO("keyboard in use, keeping the link active");
                mkb_bt_power_keep_active(target_bda);
            }

            if (esp_timer_is_active(idle_timer)) {
                esp_timer_restart(idle_timer, IDLE_TIMEOUT_US);
            } else {
                esp_timer_start_once(idle_timer, IDLE_TIMEOUT_US);
            }
#endif
        }

        void on_link_closed() {
#if CONFIG_MKB_KEYBOARD_POWER_IDLE
            esp_timer_stop(idle_timer);
            link_active = false;
#endif
        }

        std::string to_hex(const uint8_t* data, size_t length) {
            std::string hex{};
            hex.reserve(length * 3);

            char byte[4]{};
            for (size_t i = 0; i < length; ++i) {
                std::snprintf(byte, sizeof(byte), "%02x ", data[i]);
                hex += byte;
            }

            if (!hex.empty()) {
                hex.pop_back();
            }
            return hex;
        }

        std::string_view device_name(const esp_bt_gap_cb_param_t::disc_res_param& result) {
            for (int i = 0; i < result.num_prop; ++i) {
                const auto& prop = result.prop[i];

                if (prop.type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                    return {static_cast<const char*>(prop.val), static_cast<size_t>(prop.len)};
                }

                if (prop.type == ESP_BT_GAP_DEV_PROP_EIR) {
                    auto* eir = static_cast<uint8_t*>(prop.val);
                    uint8_t length{};

                    uint8_t* name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &length);
                    if (name == nullptr) {
                        name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &length);
                    }

                    if (name != nullptr) {
                        return {reinterpret_cast<const char*>(name), length};
                    }
                }
            }
            return {};
        }

        uint32_t device_cod(const esp_bt_gap_cb_param_t::disc_res_param& result) {
            for (int i = 0; i < result.num_prop; ++i) {
                const auto& prop = result.prop[i];

                if (prop.type == ESP_BT_GAP_DEV_PROP_COD) {
                    uint32_t cod{};
                    std::memcpy(&cod, prop.val, sizeof(cod));
                    return cod;
                }
            }
            return 0;
        }

        bool is_keyboard(uint32_t cod) {
            return ((cod >> 8) & 0x1F) == COD_MAJOR_PERIPHERAL && (cod & COD_KEYBOARD) != 0;
        }

        void start_discovery() {
            LOG_INFO("scanning for '%.*s'...", static_cast<int>(TARGET_NAME.size()), TARGET_NAME.data());

            esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, INQUIRY_LENGTH, 0);
            if (err != ESP_OK) {
                LOG_ERROR("start discovery failed: %s", esp_err_to_name(err));
            }
        }

        void schedule_retry(uint64_t delay_us) {
            esp_timer_stop(retry_timer);
            esp_timer_start_once(retry_timer, delay_us);
        }

        void on_retry(void*) {
            if (bonded && !connected) {
                xTaskNotifyGive(open_task);
            }
        }

        void on_discovery_result(const esp_bt_gap_cb_param_t::disc_res_param& result) {
            const auto name = device_name(result);
            const auto cod  = device_cod(result);

            LOG_INFO("found " ESP_BD_ADDR_STR " cod=0x%06" PRIx32 "%s name='%.*s'",
                     ESP_BD_ADDR_HEX(result.bda),
                     cod,
                     is_keyboard(cod) ? " (keyboard)" : "",
                     static_cast<int>(name.size()),
                     name.empty() ? "" : name.data());

            if (target_found || !name.starts_with(TARGET_NAME)) {
                return;
            }

            std::memcpy(target_bda, result.bda, sizeof(esp_bd_addr_t));
            target_found = true;
            esp_bt_gap_cancel_discovery();
        }

        void on_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
            switch (event) {
                case ESP_BT_GAP_DISC_RES_EVT:
                    on_discovery_result(param->disc_res);
                    break;
                case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
                    if (param->disc_st_chg.state != ESP_BT_GAP_DISCOVERY_STOPPED) {
                        break;
                    }

                    if (target_found) {
                        xTaskNotifyGive(open_task);
                    } else if (!connected) {
                        start_discovery();
                    }
                    break;
                case ESP_BT_GAP_KEY_NOTIF_EVT:
                    LOG_WARN(">>> type %06" PRIu32 " on the Magic Keyboard, then press Enter <<<",
                             param->key_notif.passkey);
                    break;
                case ESP_BT_GAP_CFM_REQ_EVT:
                    LOG_INFO("accepting numeric comparison %06" PRIu32, param->cfm_req.num_val);
                    esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
                    break;
                case ESP_BT_GAP_PIN_REQ_EVT: {
                    LOG_WARN(">>> legacy pairing: type %s on the Magic Keyboard, then press Enter <<<", LEGACY_PIN);

                    esp_bt_pin_code_t pin{};
                    std::memcpy(pin, LEGACY_PIN, sizeof(LEGACY_PIN) - 1);
                    esp_bt_gap_pin_reply(param->pin_req.bda, true, sizeof(LEGACY_PIN) - 1, pin);
                    break;
                }
                case ESP_BT_GAP_AUTH_CMPL_EVT:
                    if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                        LOG_INFO("paired with '%s'", reinterpret_cast<const char*>(param->auth_cmpl.device_name));
                    } else {
                        LOG_ERROR("pairing failed, status %d", param->auth_cmpl.stat);
                    }
                    break;
                case ESP_BT_GAP_MODE_CHG_EVT:
                    // 0 active, 2 sniff. Sniff is where the first keypress after a pause gets slow.
                    LOG_INFO("power mode %d", param->mode_chg.mode);
#if CONFIG_MKB_KEYBOARD_SNIFF_SLOTS > 0
                    if (param->mode_chg.mode == ESP_BT_PM_MD_SNIFF) {
                        schedule_sniff_request();
                    }
#endif
                    break;
                case ESP_BT_GAP_QOS_CMPL_EVT:
                    LOG_INFO("poll interval %" PRIu32 " slots, status %d", param->qos_cmpl.t_poll, param->qos_cmpl.stat);
                    break;
                case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
                    on_acl_connected(param->acl_conn_cmpl_stat);
                    break;
                case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
                    on_acl_disconnected(param->acl_disconn_cmpl_stat);
                    break;
                default:
                    LOG_DEBUG("gap event %d", event);
                    break;
            }
        }

        bool find_bonded_target();

        void log_device(esp_hidh_dev_t* dev) {
            const char* name = esp_hidh_dev_name_get(dev);

            LOG_INFO("connected to '%s' vid=0x%04x pid=0x%04x version=0x%04x",
                     name != nullptr ? name : "",
                     esp_hidh_dev_vendor_id_get(dev),
                     esp_hidh_dev_product_id_get(dev),
                     esp_hidh_dev_version_get(dev));

            // No esp_hidh_dev_dump: it reads report_maps[0] unchecked, and a reconnect right after a
            // failed one opens without a report map. That crashed with LoadProhibited.
            size_t num_maps{};
            esp_hid_raw_report_map_t* maps{nullptr};
            if (esp_hidh_dev_report_maps_get(dev, &num_maps, &maps) == ESP_OK && maps != nullptr) {
                for (size_t i = 0; i < num_maps; ++i) {
                    LOG_DEBUG("report map %zu (%u bytes): %s", i, maps[i].len, to_hex(maps[i].data, maps[i].len).c_str());
                }
            }
        }

        void on_hid_event(void*, esp_event_base_t, int32_t id, void* event_data) {
            auto* param = static_cast<esp_hidh_event_data_t*>(event_data);

            switch (static_cast<esp_hidh_event_t>(id)) {
                case ESP_HIDH_OPEN_EVENT:
                    if (param->open.status != ESP_OK) {
                        LOG_ERROR("connect failed: %s", esp_err_to_name(param->open.status));

                        // A bonded keyboard connects back by itself on a key press, scanning would
                        // only keep the radio busy. Unless it rejected our key, then Bluedroid has
                        // deleted the bond and it needs pairing again.
                        if (bonded && find_bonded_target()) {
                            LOG_INFO("waiting for the keyboard, press a key on it");
                            schedule_retry(RETRY_INTERVAL_US);
                            break;
                        }

                        bonded       = false;
                        target_found = false;
                        start_discovery();
                        break;
                    }

                    bonded = true;
                    esp_timer_stop(retry_timer);
#if CONFIG_MKB_KEYBOARD_POLL_SLOTS > 0
                    esp_bt_gap_set_qos(target_bda, CONFIG_MKB_KEYBOARD_POLL_SLOTS);
#endif

                    connected = true;
                    device    = param->open.dev;
                    log_device(param->open.dev);

                    // It connects because a key was pressed
                    on_activity();
                    break;
                case ESP_HIDH_INPUT_EVENT: {
                    const auto& input = param->input;
                    LOG_DEBUG("input map=%u id=%u %s len=%u: %s",
                              input.map_index,
                              input.report_id,
                              esp_hid_usage_str(input.usage),
                              input.length,
                              to_hex(input.data, input.length).c_str());

                    if (report_callback) {
                        report_callback(input.report_id, {input.data, input.length});
                    }
                    on_activity();
                    break;
                }
                case ESP_HIDH_FEATURE_EVENT: {
                    const auto& feature = param->feature;
                    LOG_INFO("feature map=%u id=%u %s len=%u: %s",
                             feature.map_index,
                             feature.report_id,
                             esp_hid_usage_str(feature.usage),
                             feature.length,
                             to_hex(feature.data, feature.length).c_str());
                    break;
                }
                case ESP_HIDH_BATTERY_EVENT:
                    LOG_INFO("battery %u%%", param->battery.level);
                    break;
                case ESP_HIDH_CLOSE_EVENT:
                    connected = false;
                    device    = nullptr;
                    on_link_closed();
                    LOG_WARN("disconnected, press a key on the keyboard to reconnect");
                    esp_timer_stop(half_open_timer);
                    esp_timer_start_once(half_open_timer, HALF_OPEN_CHECK_US);
                    schedule_retry(RETRY_FIRST_US);
                    break;
                default:
                    LOG_DEBUG("hid event %" PRId32, id);
                    break;
            }
        }

        [[noreturn]] void open_target(void*) {
            while (true) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                LOG_INFO("connecting to " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(target_bda));
                esp_hidh_dev_open(target_bda, ESP_HID_TRANSPORT_BT, 0);
            }
        }

        std::unique_ptr<esp_bd_addr_t[]> bonded_devices(int& count) {
            count = esp_bt_gap_get_bond_device_num();
            if (count <= 0) {
                count = 0;
                return nullptr;
            }

            auto devices = std::make_unique<esp_bd_addr_t[]>(static_cast<size_t>(count));
            ESP_ERROR_CHECK(esp_bt_gap_get_bond_device_list(&count, devices.get()));
            return devices;
        }

        void forget_bonds() {
            int count{};
            auto devices = bonded_devices(count);

            for (int i = 0; i < count; ++i) {
                LOG_WARN("forgetting " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(devices[i]));
                esp_bt_gap_remove_bond_device(devices[i]);
            }
        }

        bool find_bonded_target() {
            int count{};
            auto devices = bonded_devices(count);
            if (count == 0) {
                return false;
            }

            // Only the keyboard is ever paired, the first bond is it
            std::memcpy(target_bda, devices[0], sizeof(esp_bd_addr_t));
            return true;
        }
    }

    void hid_host::start(report_handler on_report) {
        report_callback = std::move(on_report);

        mkb_bt_power_init();

        esp_timer_create_args_t half_open_args{};
        half_open_args.callback = on_half_open_check;
        half_open_args.name     = "keyboard_half_open";
        ESP_ERROR_CHECK(esp_timer_create(&half_open_args, &half_open_timer));

        esp_timer_create_args_t retry_args{};
        retry_args.callback = on_retry;
        retry_args.name     = "keyboard_retry";
        ESP_ERROR_CHECK(esp_timer_create(&retry_args, &retry_timer));
#if CONFIG_MKB_KEYBOARD_SNIFF_SLOTS > 0
        esp_timer_create_args_t sniff_args{};
        sniff_args.callback = on_sniff_timer;
        sniff_args.name     = "keyboard_sniff";
        ESP_ERROR_CHECK(esp_timer_create(&sniff_args, &sniff_timer));
#endif
#if CONFIG_MKB_KEYBOARD_POWER_IDLE
        esp_timer_create_args_t timer_args{};
        timer_args.callback = on_idle;
        timer_args.name     = "keyboard_idle";
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &idle_timer));
#endif

        // DisplayYesNo against a KeyboardOnly keyboard ends up as passkey entry: we show, it types
        esp_bt_io_cap_t io_cap{ESP_BT_IO_CAP_IO};
        ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &io_cap, sizeof(io_cap)));

        esp_bt_pin_code_t pin{};
        ESP_ERROR_CHECK(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin));

        ESP_ERROR_CHECK(esp_bt_gap_register_callback(on_gap_event));

        // A bonded keyboard pages us when a key is pressed, so stay connectable
        ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE));
        ESP_ERROR_CHECK(esp_bt_gap_set_page_timeout(PAGE_TIMEOUT));

        // With BLE enabled esp_hidh_init also brings up the BLE host, which blocks until its GATT
        // client app registers. That reply only arrives through this handler.
        ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

        esp_hidh_config_t hid_config{
            .callback = on_hid_event,
            .event_stack_size = 4096,
            .callback_arg = nullptr,
        };
        ESP_ERROR_CHECK(esp_hidh_init(&hid_config));

        xTaskCreate(open_target, "bt_hid_open", 4096, nullptr, 2, &open_task);

        if (find_bonded_target()) {
            LOG_INFO("bonded to " ESP_BD_ADDR_STR ", press a key if it doesn't connect", ESP_BD_ADDR_HEX(target_bda));
            bonded       = true;
            target_found = true;
            xTaskNotifyGive(open_task);
            return;
        }

        LOG_WARN("not paired: remove the keyboard from other hosts, switch it off and on, and keep it close");
        start_discovery();
    }

    bool hid_host::is_connected() const {
        return connected;
    }

    void hid_host::forget() {
        forget_bonds();
    }

    void hid_host::set_leds(uint8_t leds) {
        esp_hidh_dev_t* dev = device;
        if (dev == nullptr) {
            return;
        }

        esp_err_t err = esp_hidh_dev_output_set(dev, 0, KEYS_REPORT_ID, &leds, sizeof(leds));
        if (err != ESP_OK) {
            LOG_WARN("setting keyboard leds failed: %s", esp_err_to_name(err));
        }
    }
}
