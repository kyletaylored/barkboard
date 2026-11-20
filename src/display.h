#pragma once
#include <Arduino.h>

namespace display {
    void begin();
    void tick();          // call frequently from loop()
    void setBacklight(uint8_t pct);  // 0..100
    // Rotate the panel 180 degrees (true) or back to the default landscape
    // orientation (false), mirror touch to match, and redraw. Call from the
    // core-1 loop only (it touches TFT_eSPI and LVGL). begin() applies
    // storage::getDisplayFlipped() itself, so this is only for live changes.
    void setFlipped(bool flipped);
    bool touched();       // raw touch state — usable before LVGL is running
    // Show a "hold to factory-reset" prompt on the bare TFT (LVGL not required).
    // Returns true if the user held for the full duration.
    bool factoryResetPrompt(uint32_t holdMs = 2000);
}
