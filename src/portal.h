#pragma once
#include <Arduino.h>

namespace portal {
    void begin();      // start HTTP server on device IP for key entry
    void loop();       // process clients
    String currentIP();
}
