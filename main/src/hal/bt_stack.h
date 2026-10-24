#pragma once

namespace magic_keyboard::hal::bt {
    // Brings up the controller in dual mode and Bluedroid. Classic talks to the keyboard, BLE to the PC.
    void start_stack();
}
