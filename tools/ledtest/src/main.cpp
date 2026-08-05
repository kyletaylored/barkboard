// Copied verbatim from witnessmenow/ESP32-Cheap-Yellow-Display's own
// Examples/Basics/6-LEDTest/6-LEDTest.ino (mirrored under
// _reference/ESP32-Cheap-Yellow-Display/) — the board author's own minimal
// RGB LED sanity check, only renamed .ino -> .cpp so PlatformIO picks it up
// without needing an Arduino.h include of its own (framework=arduino adds
// that automatically for .cpp files under src/).
/*******************************************************************
    Testing the RGB LED of the CYD.

    https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display

    If you find what I do useful and would like to support me,
    please consider becoming a sponsor on Github
    https://github.com/sponsors/witnessmenow/

    Written by Brian Lough
    YouTube: https://www.youtube.com/brianlough
    Twitter: https://twitter.com/witnessmenow
 *******************************************************************/
#include <Arduino.h>

#define CYD_LED_BLUE 17
#define CYD_LED_RED 4
#define CYD_LED_GREEN 16

// Added to test a specific real-world claim: that GPIO4/red only lights via
// PWM (ledcWrite), not plain digitalWrite, supposedly because pulling it LOW
// has to fight an onboard pull-up. That reasoning doesn't hold up physically
// (PWM's LOW phase is driven by the exact same output transistor as a static
// digitalWrite(LOW) — toggling frequency doesn't change drive strength), and
// the specific pull-up claim (GPIO4 tied to the microSD slot's HSPI_HD/Data1
// line) doesn't match witnessmenow/ESP32-Cheap-Yellow-Display's own PINS.md,
// which puts the microSD slot on GPIO 5/18/19/23 (VSPI), not GPIO4. Testing
// the actual empirical claim anyway — "does PWM light it when digitalWrite
// doesn't" is checkable regardless of whether the proposed reason is right.
#define RED_PWM_CHANNEL 0
#define RED_PWM_FREQ    5000
#define RED_PWM_RES_BITS 8

// Added on top of the original upstream sketch: after every phase, read
// each pin back with digitalRead() and print it. On the ESP32, digitalRead()
// on a pin currently in OUTPUT mode reports the level the chip is actually
// driving right now, not just "whatever GPIO mode it's in" — so this proves
// whether the SoC itself is really pulling a pin LOW, independent of
// whether any LED visibly lights. If the log says LOW but a color still
// doesn't show, that pins down the fault to the LED/resistor/solder joint,
// not the code or the chip's GPIO output.
static void logPins(const char* phase) {
  Serial.printf("[ledtest] %-6s  RED(gpio%d)=%s  GREEN(gpio%d)=%s  BLUE(gpio%d)=%s\n",
                phase,
                CYD_LED_RED,   digitalRead(CYD_LED_RED)   == LOW ? "LOW(on)"  : "HIGH(off)",
                CYD_LED_GREEN, digitalRead(CYD_LED_GREEN) == LOW ? "LOW(on)"  : "HIGH(off)",
                CYD_LED_BLUE,  digitalRead(CYD_LED_BLUE)  == LOW ? "LOW(on)"  : "HIGH(off)");
}

void setup() {

  Serial.begin(115200);
  delay(200);
  Serial.println("\n[ledtest] booted");

  pinMode(CYD_LED_RED, OUTPUT);
  pinMode(CYD_LED_GREEN, OUTPUT);
  pinMode(CYD_LED_BLUE, OUTPUT);

}

// Detach GPIO4 from LEDC and hand it back to plain digitalWrite — mixing
// the two modes on an attached pin is unreliable (same reasoning as
// src/main.cpp's setSolidRGB()/setBreathingRB() split in the main firmware).
static void redDigitalWrite(uint8_t level) {
  ledcDetachPin(CYD_LED_RED);
  pinMode(CYD_LED_RED, OUTPUT);
  digitalWrite(CYD_LED_RED, level);
}

// duty 0 = fully LOW (on, active-low) at RED_PWM_FREQ/RED_PWM_RES_BITS —
// the specific claim under test is that THIS, not redDigitalWrite(LOW),
// lights the red channel.
static void redPwmOn() {
  ledcSetup(RED_PWM_CHANNEL, RED_PWM_FREQ, RED_PWM_RES_BITS);
  ledcAttachPin(CYD_LED_RED, RED_PWM_CHANNEL);
  ledcWrite(RED_PWM_CHANNEL, 0);
}

void loop() {
  //Turn LED Off
  redDigitalWrite(HIGH); // The LEDs are "active low", meaning HIGH == off, LOW == on
  digitalWrite(CYD_LED_GREEN, HIGH);
  digitalWrite(CYD_LED_BLUE, HIGH);
  logPins("off");

  delay(1000);

  // Red LED via plain digitalWrite
  redDigitalWrite(LOW);
  digitalWrite(CYD_LED_GREEN, HIGH);
  digitalWrite(CYD_LED_BLUE, HIGH);
  logPins("red(digitalWrite)");

  delay(1000);

  // Red LED via PWM/ledcWrite at full duty — the claim under test
  redPwmOn();
  digitalWrite(CYD_LED_GREEN, HIGH);
  digitalWrite(CYD_LED_BLUE, HIGH);
  logPins("red(PWM)");

  delay(1000);

  // Green LED
  redDigitalWrite(HIGH);
  digitalWrite(CYD_LED_GREEN, LOW);
  digitalWrite(CYD_LED_BLUE, HIGH);
  logPins("green");

  delay(1000);

  // Blue LED
  redDigitalWrite(HIGH);
  digitalWrite(CYD_LED_GREEN, HIGH);
  digitalWrite(CYD_LED_BLUE, LOW);
  logPins("blue");

  delay(1000);

}
