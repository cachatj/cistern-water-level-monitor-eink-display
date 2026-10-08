# Cistern Water Level Monitor

This repository contains the firmware and hardware documentation for the "Cistern Water Level Monitor" project. This system utilizes a solar-powered outdoor ESP32 sensor node to track cistern water levels and localized rainfall. The data goes to an indoor e-ink dashboard. The dashboard joins the home Wi-Fi network and publishes the readings to Apple Home.

## System Architecture

The project is split into two primary nodes:
*   **Node 1 (Outdoor Publisher):** A solar-powered ELEGOO ESP32. It collects sensor data, spends most of its time in deep sleep to save power, and sends JSON payloads to Node 2 over **ESP-NOW only**. It never joins Wi-Fi, because connecting to a router on every wake would drain the battery, and a sleeping device can't answer HomeKit. During bench testing, a 0.96" OLED shows live readings for debugging.
*   **Node 2 (Indoor Subscriber and Wi-Fi Bridge):** An ELECROW CrowPanel 5.79" E-Ink Display (ESP32-S3), powered over USB and always on. It receives Node 1's ESP-NOW payloads and updates the dashboard. It also stays joined to the home 2.4 GHz IoT Wi-Fi network and publishes the readings to Apple Home through HomeSpan.

## Network Setup (Wi-Fi and ESP-NOW)

Only the CrowPanel joins Wi-Fi. The outdoor node talks to it over ESP-NOW, a direct radio link that doesn't go through the router.

1.  **2.4 GHz only:** ESP32 radios can't use 5 GHz. The IoT network must have a 2.4 GHz band. If the router uses one network name for both bands, that's fine.
2.  **Fixed channel:** In the router, set the 2.4 GHz channel to **1, 6, or 11** instead of auto.
    *   While the CrowPanel is joined to Wi-Fi, its radio stays on the router's channel.
    *   ESP-NOW only works when the sender is on that same channel, so the outdoor node is set to it in firmware.
    *   If a send fails, the outdoor node scans for the network to find its new channel. It does this without joining the network.
3.  **CrowPanel MAC:** The CrowPanel prints its MAC address when it joins Wi-Fi. Enter that address in the outdoor node's firmware as its ESP-NOW peer.
4.  **Credentials stay out of GitHub:** The network name, password, channel and CrowPanel MAC live in `include/secrets.h` in each repo.
    *   That file is listed in `.gitignore`.
    *   Commit a `secrets.example.h` with placeholder values instead.
5.  **Reliable receiving:** On the CrowPanel, turn off Wi-Fi power saving with `WiFi.setSleep(false)`. Otherwise it can miss ESP-NOW packets. It runs on USB power, so the extra draw doesn't matter.
6.  **Smart-home apps:** HomeSpan works with Apple Home only. Google Home support needs a different approach and is an open decision.

## Hardware Manifest

### In Hand

| Part                                                                          | Qty     | Role                                   | Notes                                                                                                                                                          |
| ----------------------------------------------------------------------------- | ------- | -------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| ELEGOO ESP-32 Development Board (USB-C, CP2102)                               | 3       | Outdoor publisher (Node 1)             | 1 in use, 2 spares                                                                                                                                             |
| ELECROW CrowPanel ESP32 5.79" E-Paper HMI (272×792, SPI)                      | 1       | Indoor subscriber (Node 2)             | ESP32-S3, USB-C powered                                                                                                                                        |
| ELEGOO 0.96" OLED (SSD1306, I2C, white)                                       | 3       | Bench-test debug display               | Not deployed outdoors (power draw)                                                                                                                             |
| JSN-SR04T-V3.0 Waterproof Ultrasonic Module (21–600 cm)                       | 2       | Distance to water surface              | 1 spare. ~21–25 cm blind zone. Run at 5 V.                                                                                                                     |
| MISOL Tipping-Bucket Rain Gauge (weather station spare part)                  | 1       | Localized rainfall                     | Reed switch on an RJ11 plug; vendor spec 0.3 mm per tip                                                                                                        |
| EverExceed 5W 5V USB Solar Panel (IP65, 9.8 ft cable, 360° bracket)           | 1       | Charging source                        | Security-camera panel; ends in a USB plug                                                                                                                      |
| TP4056 Type-C Li-ion Charging Module **with protection** (5 V in, 1 A charge) | 5       | Charges the 18650 cells from the panel | 1 in use, 4 spares. B+/B− go to the cells; OUT+/OUT− carry battery voltage (3.0–4.2 V) through the protection circuit to the load.                             |
| RTHIEAI IP67 Junction Box, 150×100×70 mm                                      | 1       | Enclosure                              | Hinged clear cover, mounting plate, wall brackets                                                                                                              |
| PIONFYNES PG7 Nylon Cable Glands (3–6.5 mm cable)                             | 20      | Cable entries                          | 3 needed: solar, ultrasonic probe, rain gauge                                                                                                                  |
| EPLZON Solderable Mini Breadboard PCB, 2.0" × 1.5" (17 columns, rows A–J)     | 12      | Field junction board                   | One board holds the resistors and the shared 5V, 3V3 and GND connections (see Assembly). 11 spares.                                                            |
| ESP32 30-pin screw-terminal adapter                                           | 1       | Holds the ESP32                        | Every pin has a labeled screw terminal, so field wires clamp in without Dupont connectors. Each block also has one unlabeled screw at the top. Leave it empty. |
| DEYUE 1% Metal Film Resistor Kit, 0 Ω–1 MΩ                                    | 605 pcs | Dividers and pull-ups                  | Supplies the 1 kΩ, 2.2 kΩ, 10 kΩ, 22 kΩ, and 100 kΩ values used below. It has no 2 kΩ, so the 2 kΩ is made from 2.2 kΩ and 22 kΩ in parallel.                  |
| TODOELEC 360-pc Dupont Jumper Kit (M-F / M-M / F-F, 10/20/30 cm)              | 1       | Bench wiring                           |                                                                                                                                                                |

### Ordered (Awaiting Delivery)

| Part                                                 | Qty | Role                                                                                                             | Check on Arrival                                                                                                                                                                                                                 |
| ---------------------------------------------------- | --- | ---------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 18650 Li-ion cells, same model and same charge state | 2   | Energy storage (wired in parallel, 3.0–4.2 V)                                                                    | Each cell should read within ~0.1 V of the other before they go in the holder together.                                                                                                                                          |
| 2-slot 18650 holder wired in **parallel**            | 1   | Holds the cells; connects to TP4056 B+/B−                                                                        | With both cells installed, the leads must read ≤4.2 V. A series-wired holder reads ~7–8.4 V and would destroy the TP4056.                                                                                                        |
| Pololu U3V16F5 5V Step-Up Regulator                  | 1   | Makes a true 5 V rail from the 3.0–4.2 V battery for the ESP32 VIN pin and the JSN-SR04T (see Design Changes #2) | The header pins come loose, so solder them on first. The board has no reverse-polarity protection, so check VIN/GND polarity before applying power. With 3.7–4.2 V on VIN and nothing on the output, VOUT should read 4.8–5.2 V. |

### Still Needed

| Part                            | Qty | Why                                         |
| ------------------------------- | --- | ------------------------------------------- |
| 830-point solderless breadboard | 1   | Bench testing (skip if you already own one) |

### Removed From the Original Design

*   **1N5819 Schottky diode:** Not needed. When the panel stops producing, the TP4056 enters sleep mode and battery drain back through it is under 2 µA (per the TP4056 datasheet). The diode would only cost ~0.3 V of charging headroom.

## Design Changes vs. the Original Schematic

The earlier wiring schematics (`wiring schematic.jpeg`, `wiring schematic 2.jpeg`) are out of date on the points below.

1.  **JSN-SR04T Echo needs a voltage divider.** Powered at 5 V, the Echo pin outputs 5 V pulses. ESP32 GPIOs are 3.3 V only. Echo now passes through a 1 kΩ / 2 kΩ divider (≈3.3 V) before D18 (GPIO 18). The kit has no 2 kΩ, so it is a 2.2 kΩ and a 22 kΩ in parallel (exactly 2.0 kΩ). A 2.2 kΩ alone would put about 3.44–3.57 V on the pin, too close to the ESP32's 3.6 V limit. Trig can be driven directly from D5 (GPIO 5), because 3.3 V reads as HIGH.
2.  **The battery cannot feed the ESP32's VIN pin directly.** TP4056 OUT+ is the battery voltage (3.0–4.2 V), not 5 V. On the VIN pin it goes through the dev board's AMS1117 3.3 V regulator, which needs ~1 V of headroom, so the ESP32 browns out under Wi-Fi load. The JSN-SR04T on the same "5V" pin would also only see 3–4 V, where its range and reliability drop. Fix: a Pololu U3V16F5 boost regulator between TP4056 OUT+ and the 5 V rail. The 5 V rail then behaves the same in the field as it does on USB during bench testing.
3.  **Solar input.** The EverExceed panel is a 5 V USB camera panel. Its plug will not pass through a PG7 gland, so cut it off, pass the cable through the gland, and solder it to TP4056 IN+/IN−. The 1N5819 diode has been removed (see above).
4.  **Board layout.** The EPLZON boards are 2.0" × 1.5", so the single-board layout in the old schematic will not fit. The ESP32 stays on its screw-terminal adapter, and the TP4056 and U3V16F5 mount as modules. One EPLZON board becomes a junction board that holds the resistors and the shared 5V, 3V3 and GND connections (see Assembly).
5.  **Rain gauge connector and pull-up.** The gauge ends in an RJ11 plug, and the reed switch is on the two center conductors. Add an external 10 kΩ pull-up to 3.3 V on D27 (GPIO 27) so the input stays defined in deep sleep.
6.  **OLED added** as a bench-only debug display on I2C (D21/D22).

## Pin Map (Outdoor ESP32)

The wiring guides use the **board labels** printed next to each pin on the ELEGOO board (30-pin layout). On this board, D*n* is GPIO *n*: D18 is GPIO 18, D5 is GPIO 5, and so on. Firmware uses the GPIO number, for example `pinMode(18, INPUT)`.

The screw-terminal adapter prints the same labels next to its terminals, in the same order. This was checked against a photo of the adapter. Each terminal block has one extra screw at the top with no label. Leave it empty.

Position assumes the adapter is held with the antenna end at the top and USB-C at the bottom. It counts labeled terminals from the top, skipping the unlabeled screw. L4 means the left block, 4th labeled terminal (D34).

| Board label | GPIO (in code) | Position | Connects to              | Notes                                                                              |
| ----------- | -------------- | -------- | ------------------------ | ---------------------------------------------------------------------------------- |
| D5          | 5              | R8       | JSN-SR04T Trig           | Direct connection. Use a 20 µs trigger pulse (not the HC-SR04's 10 µs).            |
| D18         | 18             | R7       | JSN-SR04T Echo           | Through a 1 kΩ / 2 kΩ divider (2 kΩ = 2.2 kΩ ∥ 22 kΩ)                              |
| D27         | 27             | L10      | Rain gauge               | 10 kΩ pull-up to 3.3 V. RTC GPIO, so it can wake the board from deep sleep (ext0). |
| D34         | 34             | L4       | Battery divider midpoint | Input-only, ADC1 (works while Wi-Fi is on)                                         |
| D21         | 21             | R5       | OLED SDA                 | Bench only                                                                         |
| D22         | 22             | R2       | OLED SCL                 | Bench only                                                                         |
| VIN         | —              | L15      | 5 V rail                 | USB 5 V comes out here on the bench. In the field the U3V16F5 feeds 5 V in here.   |
| 3V3         | —              | R15      | 3.3 V rail               | Output of the board's regulator                                                    |
| GND         | —              | L14, R14 | Ground                   | Either GND pin                                                                     |

The other labels on the board are also GPIOs:
*   VP = GPIO 36 and VN = GPIO 39 (both input-only).
*   TX0/RX0 = GPIO 1/3. These carry USB serial, so leave them free.
*   TX2/RX2 = GPIO 17/16.
*   EN is the reset line, not a GPIO.

## Wiring Guide: Bench Test (USB-Powered, Buildable With Parts in Hand)

Power the ESP32 from your computer over USB-C. Keep it on its screw-terminal adapter beside the breadboard, and run jumper wires from its terminals to the breadboard. On the breadboard, use one pair of edge strips for 5V and GND, and the other pair for 3V3 and GND. Join the two GND strips with a short jumper.

**Breadboard Rails:**
*   5 V rail ← ESP32 **VIN** pin (USB 5 V)
*   3.3 V rail ← ESP32 **3V3** pin
*   GND rail ← ESP32 **GND**

**JSN-SR04T (Water Level):**
*   5V → 5 V rail, GND → GND rail, Trig → **D5**
*   Echo → 1 kΩ → junction → **D18**
*   Junction → 2.2 kΩ → GND, plus a 22 kΩ from the junction to GND (in parallel, together 2.0 kΩ)

**Rain Gauge:** Cut off the RJ11 plug. While tipping the bucket, use a multimeter on continuity to confirm which two conductors are the switch (normally the center pair).
*   Switch wire A → GND
*   Switch wire B → **D27**, plus 10 kΩ from D27 to the 3.3 V rail

**OLED (Debug Display):** VCC → 3.3 V rail, GND → GND, SDA → **D21**, SCL → **D22** (I2C address 0x3C)

**Battery Divider (Bench Stand-In):** 5 V rail → 100 kΩ → junction → **D34**, and junction → 100 kΩ → GND.
*   Before connecting D34, measure the junction with a multimeter. It should read ≈2.4–2.5 V and **must be under 3.3 V**.
*   Firmware should report ≈5.0 V (ADC reading × 2), which validates the ADC math before a battery exists.
*   In the field, the top of the divider moves to TP4056 B+.


## Wiring Guide: Field Power (After "Still Needed" Parts Arrive)

**Power Pipeline:**
1.  Solar panel (+) → TP4056 IN+ and Solar panel (−) → TP4056 IN−, using the input solder pads beside the USB-C port (labeled IN+/IN− or +/−). Cut the plug off first. Identify polarity with a multimeter (normally red = +, black = −) and insulate any unused data wires.
2.  TP4056 B+ / B− → parallel 18650 holder
3.  TP4056 OUT+ → U3V16F5 VIN and TP4056 OUT− → U3V16F5 GND
4.  U3V16F5 VOUT → **5 V rail** (ESP32 VIN pin + JSN-SR04T 5V)
5.  All grounds common

**Battery Telemetry:** TP4056 B+ → 100 kΩ → **D34** → 100 kΩ → GND (4.2 V max → 2.1 V at the pin)

Sensor wiring is identical to the bench test.

**Power Budget (Estimate, Verify on the Bench):** In deep sleep, the dev board's AMS1117 regulator, its power LED, and the JSN-SR04T (~5 mA idle) all stay powered. Expect roughly 15–20 mA from the battery while asleep, not microamps. Two 18650 cells (~5–6 Ah total) give roughly 10–15 days with no sun. The 5 W panel recharges on sunny days.

## Software Requirements

### Outdoor Node (ESP32)
*   **ESP-NOW (`esp_now.h`, `esp_wifi.h`):** Sends readings straight to the CrowPanel on the router's channel. The node never joins Wi-Fi.
*   **ArduinoJSON:** For packing sensor data into a structured payload.
*   **Adafruit_SSD1306 + Adafruit_GFX:** Bench-test OLED only.

**Firmware Notes:**
*   **JSN-SR04T:** Hold Trig HIGH for 20 µs. Discard readings under ~25 cm (blind zone).
*   **Rain Gauge:**
    *   Wake on GPIO 27 going LOW (ext0) and count the tip in an `RTC_DATA_ATTR` variable.
    *   Wait for the pin to return HIGH before sleeping again, because each closure lasts hundreds of milliseconds.
    *   Start at 0.3 mm per tip, then calibrate by pouring a measured volume of water through the gauge.
*   **Battery:** `analogReadMilliVolts(34) * 2`.

### Indoor Node (CrowPanel ESP32-S3)
*   **GxEPD2:** For driving the e-ink display.
*   **Adafruit_GFX:** For drawing UI elements, shapes, and text.
*   **ESP-NOW:** Asynchronous receiver callbacks.
*   **WiFi (Arduino core):** Joins the home 2.4 GHz IoT network, with power saving off.
*   **HomeSpan:** Publishes the readings to Apple Home. HomeKit has no water-level or rainfall sensor type, so how they appear in the Home app is still to be decided.

## Setup & Deployment

1.  **Bench Testing (Now, USB-Powered):**
    *   Set the multimeter to continuity and check each terminal you will use (D5, D18, D27, D34, VIN, 3V3, GND) against its pin on the ESP32 itself. Each pair should beep.
    *   Build the bench-test wiring above.
    *   Verify JSN-SR04T distances against a tape measure (targets ≥25 cm away).
    *   Count rain-gauge tips by hand against the firmware count.
    *   Confirm the battery-divider stand-in reads <3.3 V at the junction before connecting D34.
2.  **Network and ESP-NOW Link:** Follow the Network Setup section above.
    *   Fix the router's 2.4 GHz channel.
    *   Flash the CrowPanel so it joins Wi-Fi, and record the channel and MAC address it prints.
    *   Put the channel and MAC in the outdoor node's `secrets.h`.
    *   Confirm that ESP-NOW packets arrive.
3.  **Power-Stage Bench Test (When Parts Arrive):** Check each item *before* connecting the ESP32:
    *   Holder leads read ≤4.2 V with both cells installed (confirms parallel wiring).
    *   Panel open-circuit voltage in full sun is ≤6.0 V (TP4056 absolute max is 6.5 V).
    *   U3V16F5 output reads 4.8–5.2 V.
    *   Then connect the ESP32 and measure deep-sleep current with the multimeter in series with the battery.
4.  **Assembly (One Junction Board):**
    *   The ESP32 stays on its screw-terminal adapter. The TP4056 and U3V16F5 are finished modules, so mount them directly on the enclosure's mounting plate.
    *   One EPLZON board is the junction board. On these boards, holes A–E in each numbered column are connected to each other, and so are holes F–J. Each 5-hole strip works as a small rail. Before soldering, check with continuity mode that two holes in the same strip beep.
    *   Hold the board component side up with column 1 on the left and row J at the top. Solder the resistors flat with their legs 4 holes apart:

        | Part         | Holes     | Bands (1% metal film)          |
        | ------------ | --------- | ------------------------------ |
        | R1 · 1 kΩ    | 1A – 5A   | brown black black brown brown  |
        | R2a · 2.2 kΩ | 5B – 9B   | red red black brown brown      |
        | R2b · 22 kΩ  | 5C – 9C   | red red black red brown        |
        | R3 · 10 kΩ   | 13A – 17A | brown black black red brown    |
        | R4 · 100 kΩ  | 1J – 5J   | brown black black orange brown |
        | R5 · 100 kΩ  | 5I – 9I   | brown black black orange brown |
        | Jumper wire  | 9E – 9F   | Joins the two GND strips       |

    *   Solder the wires:

        | Strip   | Hole | Wire to                             |
        | ------- | ---- | ----------------------------------- |
        | 5V      | 13F  | U3V16F5 VOUT                        |
        | 5V      | 13G  | ESP32 VIN terminal                  |
        | 5V      | 13H  | JSN-SR04T 5V                        |
        | 3V3     | 17C  | ESP32 3V3 terminal                  |
        | GND     | 9J   | TP4056 OUT−                         |
        | GND     | 9H   | U3V16F5 GND                         |
        | GND     | 9G   | ESP32 GND terminal                  |
        | GND     | 9D   | JSN-SR04T GND                       |
        | GND     | 9A   | Rain gauge wire A                   |
        | B+      | 1H   | TP4056 B+ (second wire on that pad) |
        | Battery | 5G   | ESP32 D34 terminal                  |
        | Echo in | 1C   | JSN-SR04T Echo                      |
        | Echo    | 5D   | ESP32 D18 terminal                  |
        | Rain    | 13E  | Rain gauge wire B                   |
        | Rain    | 13D  | ESP32 D27 terminal                  |

    *   These connections skip the junction board and are wired directly:
        *   JSN-SR04T Trig → ESP32 D5 terminal
        *   TP4056 OUT+ → U3V16F5 VIN
        *   Solar panel → TP4056 IN+/IN−
        *   Cell holder → TP4056 B+/B−
    *   Use red wire for 5V, orange for 3V3 and black for GND, so the strips are easy to tell apart.
    *   The hole-by-hole drawing is `junction-board-layout.png`.
5.  **Mounting:** Mount the JSN-SR04T transducer at least 25 cm above the highest (overflow) water level, pointing straight down and clear of pipes and walls.
6.  **Weatherproofing:**
    *   Drill 1/2" (12.5 mm) holes for three PG7 glands: solar, ultrasonic probe, and rain gauge.
    *   Mount the ESP32 adapter, junction board, TP4056, U3V16F5 and cell holder on the junction box's mounting plate.
    *   Seal all cable gland entries with silicone sealant.