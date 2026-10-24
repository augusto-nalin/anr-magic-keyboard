#include "hal/status_led.h"

namespace magic_keyboard::hal {
    namespace {
        constexpr uint64_t BLINK_HALF_PERIOD_US{500'000};
    }

    status_led::status_led(const gpio_num_t pin) : pin{pin} {
        ESP_ERROR_CHECK(gpio_reset_pin(pin));
        ESP_ERROR_CHECK(gpio_set_direction(pin, GPIO_MODE_OUTPUT));
        ESP_ERROR_CHECK(gpio_set_level(pin, 0));

        esp_timer_create_args_t timer_args{};
        timer_args.callback = on_tick;
        timer_args.arg      = this;
        timer_args.name     = "status_led";
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &this->timer));
        ESP_ERROR_CHECK(esp_timer_start_periodic(this->timer, BLINK_HALF_PERIOD_US));
    }

    status_led::~status_led() {
        esp_timer_stop(this->timer);
        esp_timer_delete(this->timer);
    }

    void status_led::on_tick(void* arg) {
        auto* led = static_cast<status_led*>(arg);

        // Linked keeps it on, otherwise every tick flips it
        led->lit = led->linked || !led->lit;
        gpio_set_level(led->pin, led->lit);
    }

    void status_led::set_linked(const bool is_linked) {
        this->linked = is_linked;
    }
}
