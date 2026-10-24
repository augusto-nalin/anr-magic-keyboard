#include "hal/ble_hid_device.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <utility>

#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include <esp_hidd.h>
#include <esp_hidd_gatts.h>
#include <nvs.h>

#include "utils/log.h"

namespace magic_keyboard::hal::ble {
    namespace {
        constexpr char DEVICE_NAME[]{"Magic Keyboard Bridge"};

        constexpr size_t KEYBOARD_REPORT_ID{1};
        constexpr size_t CONSUMER_REPORT_ID{2};
        constexpr size_t SYSTEM_REPORT_ID{3};

        // Report 1: boot keyboard with LED output, keys up to F24 and the rest of the 0xA4 range.
        // Report 2: a single 16-bit consumer control usage. Report 3: system power down, sleep and
        // wake up bits. Windows reads this when pairing, so a change needs the bridge paired again.
        constexpr uint8_t REPORT_MAP[]{
            0x05, 0x01,         // Usage Page (Generic Desktop)
            0x09, 0x06,         // Usage (Keyboard)
            0xA1, 0x01,         // Collection (Application)
            0x85, 0x01,         //   Report ID (1)
            0x05, 0x07,         //   Usage Page (Keyboard)
            0x19, 0xE0,         //   Usage Minimum (Left Control)
            0x29, 0xE7,         //   Usage Maximum (Right GUI)
            0x15, 0x00,         //   Logical Minimum (0)
            0x25, 0x01,         //   Logical Maximum (1)
            0x75, 0x01,         //   Report Size (1)
            0x95, 0x08,         //   Report Count (8)
            0x81, 0x02,         //   Input (Data, Variable, Absolute): modifiers
            0x95, 0x01,         //   Report Count (1)
            0x75, 0x08,         //   Report Size (8)
            0x81, 0x01,         //   Input (Constant): reserved
            0x95, 0x05,         //   Report Count (5)
            0x75, 0x01,         //   Report Size (1)
            0x05, 0x08,         //   Usage Page (LEDs)
            0x19, 0x01,         //   Usage Minimum (Num Lock)
            0x29, 0x05,         //   Usage Maximum (Kana)
            0x91, 0x02,         //   Output (Data, Variable, Absolute): LEDs
            0x95, 0x01,         //   Report Count (1)
            0x75, 0x03,         //   Report Size (3)
            0x91, 0x01,         //   Output (Constant): padding
            0x95, 0x06,         //   Report Count (6)
            0x75, 0x08,         //   Report Size (8)
            0x15, 0x00,         //   Logical Minimum (0)
            0x26, 0xA4, 0x00,   //   Logical Maximum (164)
            0x05, 0x07,         //   Usage Page (Keyboard)
            0x19, 0x00,         //   Usage Minimum (0)
            0x29, 0xA4,         //   Usage Maximum (164)
            0x81, 0x00,         //   Input (Data, Array): keys
            0xC0,               // End Collection
            0x05, 0x0C,         // Usage Page (Consumer)
            0x09, 0x01,         // Usage (Consumer Control)
            0xA1, 0x01,         // Collection (Application)
            0x85, 0x02,         //   Report ID (2)
            0x15, 0x00,         //   Logical Minimum (0)
            0x26, 0xFF, 0x03,   //   Logical Maximum (1023)
            0x19, 0x00,         //   Usage Minimum (0)
            0x2A, 0xFF, 0x03,   //   Usage Maximum (1023)
            0x75, 0x10,         //   Report Size (16)
            0x95, 0x01,         //   Report Count (1)
            0x81, 0x00,         //   Input (Data, Array): usage
            0xC0,               // End Collection
            0x05, 0x01,         // Usage Page (Generic Desktop)
            0x09, 0x80,         // Usage (System Control)
            0xA1, 0x01,         // Collection (Application)
            0x85, 0x03,         //   Report ID (3)
            0x19, 0x81,         //   Usage Minimum (System Power Down)
            0x29, 0x83,         //   Usage Maximum (System Wake Up)
            0x15, 0x00,         //   Logical Minimum (0)
            0x25, 0x01,         //   Logical Maximum (1)
            0x75, 0x01,         //   Report Size (1)
            0x95, 0x03,         //   Report Count (3)
            0x81, 0x02,         //   Input (Data, Variable, Absolute): power down, sleep, wake up
            0x95, 0x05,         //   Report Count (5)
            0x81, 0x01,         //   Input (Constant): padding
            0xC0,               // End Collection
        };

        // HID service, 0x1812, as a 128-bit UUID
        uint8_t HID_SERVICE_UUID[]{
            0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
        };

        esp_hid_raw_report_map_t report_maps[]{{REPORT_MAP, sizeof(REPORT_MAP)}};

        esp_hid_device_config_t device_config{
            .vendor_id = 0x303A,            // Espressif
            .product_id = 0x0001,
            .version = 0x0100,
            .device_name = DEVICE_NAME,
            .manufacturer_name = "magic-keyboard",
            .serial_number = "0001",
            .report_maps = report_maps,
            .report_maps_len = 1,
        };

        esp_hidd_dev_t* device{nullptr};
        std::atomic<bool> connected{false};
        hid_device::leds_handler leds_callback{};
        keyboard::pc_report last_report{};

        // The PC switches on the reports it wants pushed (each report's CCCD) once, when pairing,
        // and a bonded PC expects that to survive. esp_hidd only keeps it in RAM, so after a
        // restart it refuses to send ("Indicate Not Enabled") while Windows, rightly, doesn't ask
        // again: connected but no keys. So the writes are kept in NVS and replayed into esp_hidd
        // once the bonded PC is back. Nothing public says which handle is a CCCD, but they are
        // the only 2-byte writes in this report map. All of this runs on the BTC task.
        struct saved_write {
            uint8_t gatts_if;
            uint16_t handle;
            std::array<uint8_t, 2> value;
        };

        constexpr char NVS_NAMESPACE[]{"mkb_ble"};
        constexpr char NVS_KEY[]{"cccd"};
        constexpr size_t MAX_SAVED_WRITES{16};
        std::array<saved_write, MAX_SAVED_WRITES> saved_writes{};
        size_t saved_count{0};

        void load_saved_writes() {
            nvs_handle_t nvs{};
            if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
                return;
            }

            size_t size = sizeof(saved_writes);
            if (nvs_get_blob(nvs, NVS_KEY, saved_writes.data(), &size) == ESP_OK) {
                saved_count = size / sizeof(saved_write);
            }
            nvs_close(nvs);
        }

        void store_saved_writes() {
            nvs_handle_t nvs{};
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
                return;
            }

            if (nvs_set_blob(nvs, NVS_KEY, saved_writes.data(), saved_count * sizeof(saved_write)) == ESP_OK) {
                nvs_commit(nvs);
            }
            nvs_close(nvs);
        }

        void remember_write(esp_gatt_if_t gatts_if, const esp_ble_gatts_cb_param_t::gatts_write_evt_param& write) {
            if (write.len != 2 || write.is_prep) {
                return;
            }

            const std::array<uint8_t, 2> value{write.value[0], write.value[1]};
            auto* end   = saved_writes.begin() + saved_count;
            auto* entry = std::find_if(saved_writes.begin(), end, [&](const saved_write& saved) {
                return saved.gatts_if == gatts_if && saved.handle == write.handle;
            });

            if (entry != end) {
                if (entry->value == value) {
                    return;
                }
                entry->value = value;
            } else if (saved_count < MAX_SAVED_WRITES) {
                saved_writes[saved_count++] = {gatts_if, write.handle, value};
            } else {
                return;
            }
            store_saved_writes();
        }

        void replay_saved_writes(uint16_t conn_id) {
            for (size_t i = 0; i < saved_count; ++i) {
                auto value = saved_writes[i].value;

                esp_ble_gatts_cb_param_t param{};
                param.write.conn_id = conn_id;
                param.write.handle  = saved_writes[i].handle;
                param.write.len     = static_cast<uint16_t>(value.size());
                param.write.value   = value.data();
                esp_hidd_gatts_event_handler(ESP_GATTS_WRITE_EVT, saved_writes[i].gatts_if, &param);
            }

            if (saved_count > 0) {
                LOG_INFO("restored %zu report subscriptions", saved_count);
            }
        }

        std::atomic<uint16_t> conn_id{0};

        void on_gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t* param) {
            esp_hidd_gatts_event_handler(event, gatts_if, param);

            if (event == ESP_GATTS_CONNECT_EVT) {
                conn_id = param->connect.conn_id;
            } else if (event == ESP_GATTS_WRITE_EVT) {
                remember_write(gatts_if, param->write);
            }
        }

        void start_advertising() {
            esp_ble_adv_params_t params{};
            params.adv_int_min       = 0x20;
            params.adv_int_max       = 0x30;
            params.adv_type          = ADV_TYPE_IND;
            params.own_addr_type     = BLE_ADDR_TYPE_PUBLIC;
            params.channel_map       = ADV_CHNL_ALL;
            params.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

            esp_err_t err = esp_ble_gap_start_advertising(&params);
            if (err != ESP_OK) {
                LOG_ERROR("start advertising failed: %s", esp_err_to_name(err));
            }
        }

        void log_connection(esp_bd_addr_t bda) {
            esp_gap_conn_params_t conn{};
            if (esp_ble_get_current_conn_params(bda, &conn) == ESP_OK) {
                LOG_INFO("pc connection interval %u us, peripheral latency %u, timeout %u ms",
                         conn.interval * 1250U,
                         conn.latency,
                         conn.timeout * 10U);
            }
        }

        // Every report waits for the next connection event, so the interval is the worst case on
        // this side of the bridge. Not the 7.5ms minimum: the keyboard's sniff comes every 15ms,
        // exactly twice that, which can lock both onto colliding slots on the one radio. 8.75ms
        // keeps them drifting past each other. The PC has the final say.
        void request_fast_interval(esp_bd_addr_t bda) {
            esp_ble_conn_update_params_t params{};
            std::memcpy(params.bda, bda, sizeof(esp_bd_addr_t));
            params.min_int = 0x0007;   // 8.75ms, in 1.25ms units
            params.max_int = 0x0007;
            params.latency = 0;
            params.timeout = 400;      // 4s, in 10ms units

            esp_err_t err = esp_ble_gap_update_conn_params(&params);
            if (err != ESP_OK) {
                LOG_WARN("requesting an 8.75ms interval failed: %s", esp_err_to_name(err));
            }
        }

        void on_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
            switch (event) {
                case ESP_GAP_BLE_SEC_REQ_EVT:
                    esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
                    break;
                case ESP_GAP_BLE_NC_REQ_EVT:
                    esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
                    break;
                case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT: {
                    // Interval in 1.25ms units, timeout in 10ms units
                    const auto& conn = param->update_conn_params;
                    LOG_INFO("pc connection updated (status %d): interval %u us, peripheral latency %u, timeout %u ms",
                             conn.status,
                             conn.conn_int * 1250U,
                             conn.latency,
                             conn.timeout * 10U);
                    break;
                }
                case ESP_GAP_BLE_AUTH_CMPL_EVT:
                    if (param->ble_security.auth_cmpl.success) {
                        LOG_INFO("pc paired");
                        replay_saved_writes(conn_id);
                        log_connection(param->ble_security.auth_cmpl.bd_addr);
                        request_fast_interval(param->ble_security.auth_cmpl.bd_addr);
                    } else {
                        LOG_ERROR("pc pairing failed, reason 0x%x", param->ble_security.auth_cmpl.fail_reason);
                    }
                    break;
                default:
                    LOG_DEBUG("ble gap event %d", event);
                    break;
            }
        }

        void on_hid_event(void*, esp_event_base_t, int32_t id, void* event_data) {
            auto* param = static_cast<esp_hidd_event_data_t*>(event_data);

            switch (static_cast<esp_hidd_event_t>(id)) {
                case ESP_HIDD_START_EVENT:
                    LOG_INFO("advertising as '%s'", DEVICE_NAME);
                    start_advertising();
                    break;
                case ESP_HIDD_CONNECT_EVENT:
                    connected = true;
                    LOG_INFO("pc connected");
                    break;
                case ESP_HIDD_DISCONNECT_EVENT:
                    connected = false;
                    LOG_WARN("pc disconnected, reason %d", param->disconnect.reason);
                    start_advertising();
                    break;
                case ESP_HIDD_OUTPUT_EVENT:
                    if (param->output.report_id == KEYBOARD_REPORT_ID && param->output.length >= 1 && leds_callback) {
                        leds_callback(param->output.data[0]);
                    }
                    break;
                default:
                    LOG_DEBUG("hid device event %" PRId32, id);
                    break;
            }
        }
    }

    void hid_device::start(leds_handler on_leds) {
        leds_callback = std::move(on_leds);

        // Just Works: the bridge has no display or keypad of its own to confirm a code with
        esp_ble_auth_req_t auth_req{ESP_LE_AUTH_REQ_SC_BOND};
        esp_ble_io_cap_t io_cap{ESP_IO_CAP_NONE};
        uint8_t keys{ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK};
        uint8_t key_size{16};

        ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req)));
        ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, sizeof(io_cap)));
        ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys)));
        ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys)));
        ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)));

        ESP_ERROR_CHECK(esp_ble_gap_register_callback(on_gap_event));
        ESP_ERROR_CHECK(esp_ble_gap_set_device_name(DEVICE_NAME));

        // Advertising data only holds 31 bytes, so the name goes in the scan response the PC asks
        // for while searching. 7.5-20ms connection interval, the lower end keeps typing latency down.
        esp_ble_adv_data_t adv_data{};
        adv_data.min_interval     = 0x0006;
        adv_data.max_interval     = 0x0010;
        adv_data.appearance       = ESP_HID_APPEARANCE_KEYBOARD;
        adv_data.service_uuid_len = sizeof(HID_SERVICE_UUID);
        adv_data.p_service_uuid   = HID_SERVICE_UUID;
        adv_data.flag             = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
        ESP_ERROR_CHECK(esp_ble_gap_config_adv_data(&adv_data));

        esp_ble_adv_data_t scan_response{};
        scan_response.set_scan_rsp    = true;
        scan_response.include_name    = true;
        scan_response.include_txpower = true;
        ESP_ERROR_CHECK(esp_ble_gap_config_adv_data(&scan_response));

        load_saved_writes();
        ESP_ERROR_CHECK(esp_ble_gatts_register_callback(on_gatts_event));
        ESP_ERROR_CHECK(esp_hidd_dev_init(&device_config, ESP_HID_TRANSPORT_BLE, on_hid_event, &device));
    }

    void hid_device::send(const keyboard::pc_report& report) {
        const keyboard::pc_report previous = last_report;
        last_report = report;

        if (!connected) {
            return;
        }

        if (report.modifiers != previous.modifiers || report.keys != previous.keys) {
            std::array<uint8_t, 8> keys_report{report.modifiers, 0};
            std::ranges::copy(report.keys, keys_report.begin() + 2);
            esp_hidd_dev_input_set(device, 0, KEYBOARD_REPORT_ID, keys_report.data(), keys_report.size());
        }

        if (report.consumer != previous.consumer) {
            std::array<uint8_t, 2> consumer_report{
                static_cast<uint8_t>(report.consumer & 0xFF),
                static_cast<uint8_t>(report.consumer >> 8),
            };
            esp_hidd_dev_input_set(device, 0, CONSUMER_REPORT_ID, consumer_report.data(), consumer_report.size());
        }

        if (report.system != previous.system) {
            uint8_t system_report{report.system};
            esp_hidd_dev_input_set(device, 0, SYSTEM_REPORT_ID, &system_report, sizeof(system_report));
        }
    }

    void hid_device::set_battery(uint8_t percent) {
        if (device != nullptr) {
            esp_hidd_dev_battery_set(device, percent);
        }
    }

    bool hid_device::is_connected() const {
        return connected;
    }

    void hid_device::forget() {
        nvs_handle_t nvs{};
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
            return;
        }

        if (nvs_erase_key(nvs, NVS_KEY) == ESP_OK) {
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
}
