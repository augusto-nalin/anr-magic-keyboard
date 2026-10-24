#pragma once
#include <atomic>

#include <driver/gpio.h>
#include <esp_timer.h>

namespace magic_keyboard::hal {
    // The board's single LED: solid while linked, blinking while it isn't
    class status_led {
    private:
        gpio_num_t pin;
        esp_timer_handle_t timer{nullptr};
        std::atomic<bool> linked{false};
        bool lit{false};

        static void on_tick(void* arg);

    public:
        explicit status_led(gpio_num_t pin);

        // The timer holds a pointer to this
        status_led(const status_led&) = delete;

        status_led& operator =(const status_led&) = delete;

        ~status_led();

        // Takes effect on the next tick, within half a second
        void set_linked(bool is_linked);
    };
}
