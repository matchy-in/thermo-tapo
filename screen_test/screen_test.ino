// Screen finder for the matrixbit. Upload this on its own, open the Serial
// Monitor at 115200 baud, and watch the screen: it cycles through display
// settings and shows "OPTION n" when a setting works. Report the option number
// shown on the screen (and the serial log) so the main sketch can use it.

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

struct Bus {
    int sda, scl;
};
const Bus kBuses[] = {{23, 22}, {21, 22}, {22, 23}, {4, 15}};

U8G2_SH1106_128X64_NONAME_F_HW_I2C sh1106(U8G2_R0, U8X8_PIN_NONE);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C ssd1306(U8G2_R0, U8X8_PIN_NONE);

struct Option {
    const Bus* bus;
    uint8_t addr;
    bool isSh1106;
};
Option options[32];
int optionCount = 0;

void startBus(const Bus& b) {
    Wire.end();
    Wire.begin(b.sda, b.scl);
    Wire.setClock(100000);
}

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("\n=== SCREEN FINDER ===");

    for (const Bus& b : kBuses) {
        startBus(b);
        Serial.printf("I2C scan SDA=%d SCL=%d:", b.sda, b.scl);
        bool any = false;
        for (uint8_t a = 1; a < 127; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() != 0) continue;
            Serial.printf(" 0x%02X", a);
            any = true;
            if ((a == 0x3C || a == 0x3D) && optionCount < 30) {
                options[optionCount++] = {&b, a, true};
                options[optionCount++] = {&b, a, false};
            }
        }
        Serial.println(any ? "" : " (nothing)");
    }

    if (optionCount == 0) {
        Serial.println("No screen address (0x3C/0x3D) answered; trying the usual settings blind.");
        options[optionCount++] = {&kBuses[0], 0x3C, true};
        options[optionCount++] = {&kBuses[0], 0x3C, false};
        options[optionCount++] = {&kBuses[1], 0x3C, true};
        options[optionCount++] = {&kBuses[1], 0x3C, false};
    }
}

void show(int n, const Option& o) {
    U8G2& d = o.isSh1106 ? static_cast<U8G2&>(sh1106) : static_cast<U8G2&>(ssd1306);
    startBus(*o.bus);
    d.setI2CAddress(o.addr * 2);
    d.begin();
    d.clearBuffer();
    d.drawFrame(0, 0, 128, 64);
    d.setFont(u8g2_font_helvB14_tf);
    char buf[24];
    snprintf(buf, sizeof(buf), "OPTION %d", n);
    d.drawStr(8, 24, buf);
    d.setFont(u8g2_font_6x10_tf);
    snprintf(buf, sizeof(buf), "%s 0x%02X", o.isSh1106 ? "SH1106" : "SSD1306", o.addr);
    d.drawStr(8, 42, buf);
    snprintf(buf, sizeof(buf), "SDA %d SCL %d", o.bus->sda, o.bus->scl);
    d.drawStr(8, 56, buf);
    d.sendBuffer();
}

void loop() {
    for (int i = 0; i < optionCount; i++) {
        const Option& o = options[i];
        Serial.printf("Showing OPTION %d: %s at 0x%02X, SDA=%d SCL=%d\n", i + 1, o.isSh1106 ? "SH1106" : "SSD1306",
                      o.addr, o.bus->sda, o.bus->scl);
        show(i + 1, o);
        delay(5000);
    }
}
