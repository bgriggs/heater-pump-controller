# Diesel Heater Auxiliary Tank Pump Controller
Diesel heater aux tank pump controller. This manages an aux tank's fuel level keeping a small tank full from an RV tank that is closer to the heater. The small pump that feeds the heater is often insufficient to lift fuel from the main RV tank and is more susceptible to freezing.

## Overview
This project implements an automatic fuel pump controller for an ESP-WROOM-32 (ESP32 ESP-32S) device. The controller monitors a fuel level sensor and automatically activates a relay-controlled fuel pump to maintain a full tank while implementing intelligent retry logic to prevent excessive pump cycling.

### Features
- **Automatic fuel level monitoring** with configurable thresholds
- **Intelligent retry logic** to prevent pump damage
- **WiFi web interface** for remote monitoring and manual control
- **Real-time status updates** via web dashboard (2 updates per second)
- **Manual pump override** via web interface or MQTT
- **Hard-coded fail safe** that disables the pump until a power cycle if it runs for 4 minutes continuously
- **Built-in MQTT broker** so other applications on the local network can read status and send the same commands as the web interface
- **Visual LED feedback** for system status

## Hardware Requirements
- **Microcontroller**: ESP-WROOM-32 (ESP32 ESP-32S) (https://www.amazon.com/dp/B08D5ZD528?ref_=ppx_hzsearch_conn_dt_b_fed_asin_title_9)
- **Fuel Level Sensor**: Analog sender (0.184V = Full, 1.001V = Empty) with 560 ohm reistor for voltage divider (https://www.amazon.com/dp/B0F93SSVCB)
- **Relay Module**: 3.3V compatible relay module (https://www.amazon.com/dp/B0B1ZHXXXD)
- **Fuel Pump**: Controlled by the relay (https://www.amazon.com/dp/B08PY7V2MM)
- **Power Supply**: Appropriate for ESP32 and pump requirements (https://www.amazon.com/dp/B08RBWX2GL)
- **WiFi Network**: 2.4GHz WiFi network for web interface access

## Pin Configuration
- **GPIO 34** (ADC1_CH6): Fuel level sensor analog input
- **GPIO 23**: Relay control output (3.3V HIGH to activate)
- **GPIO 2**: Status LED - blinks at 1 Hz (loop running) or 5 Hz (pump active), solid when the fail safe has tripped
  - **Note**: If LED appears to blink erratically, GPIO 2 may have WiFi interference. Change `LED_PIN` to GPIO 4, 5, 16, 17, 18, or 19 in the code and use an external LED.

### Why GPIO 34 for Analog Input?
GPIO 34-39 on the ESP32 are input-only pins designed specifically for analog readings (ADC1). They do not have internal pull-up/pull-down resistors, making them ideal for analog sensors.

### Status LED Behavior
The single status LED on GPIO 2 provides visual feedback:
- **Slow blink (1 Hz)**: System is running, pump is OFF
- **Fast blink (5 Hz)**: Pump is actively running (relay energized)
- **Solid on**: Fail safe tripped - pump disabled until power is cycled

## Fuel Level Sensing
The system uses the ESP32's 12-bit ADC (0-4095) to read the fuel level sensor:
- **Full tank**: 0.184V (90%+ fuel level activates "full" state)
- **Empty tank**: 1.001V (0% fuel level)
- **Refill trigger**: Pumping starts when fuel level drops below 30%
- **Linear interpolation**: Used to calculate fuel percentage between empty and full

## Operating Logic

### State Machine
The controller operates in the following states:

1. **IDLE**: Monitoring mode when tank is full
   - Checks fuel level every second
   - LED blinks at 1 Hz (slow heartbeat - pump OFF)
   - When level drops below 30%, initiates pumping cycle
   
2. **PUMPING**: Active pump operation
   - Relay activated (GPIO 23 HIGH)
   - LED blinks at 5 Hz (fast blink - pump ON)
   - Maximum run time: 3 minutes per cycle
   - Continuously monitors for full tank condition
   - Stops immediately if tank reaches full (90%+)

3. **SHORT_WAIT**: 1-hour wait period
   - Activated after first unsuccessful pump cycle
   - Relay deactivated (GPIO 23 LOW)
   - LED blinks at 1 Hz (slow heartbeat - pump OFF)
   - After 1 hour, retries pumping

4. **LONG_WAIT**: 6-hour wait period
   - Activated after 2 unsuccessful pump cycles
   - Continues indefinitely with 6-hour intervals
   - LED blinks at 1 Hz (slow heartbeat - pump OFF)
   - Each interval: up to 3 minutes pumping, 6 hours waiting

5. **FAILSAFE**: Pump ran continuously for 4 minutes
   - Pump stopped; the state machine and all web and MQTT commands are disabled
   - LED on solid
   - Cleared only by a power cycle (see [Fail Safe](#fail-safe))

### Retry Logic
1. **First attempt**: Pump for up to 3 minutes
   - If full: Return to IDLE
   - If not full: Wait 1 hour, then retry
   
2. **Second attempt**: Pump for up to 3 minutes
   - If full: Return to IDLE
   - If not full: Enter 6-hour interval mode
   
3. **Subsequent attempts**: Continue indefinitely
   - Pump for up to 3 minutes every 6 hours
   - If tank ever reaches full: Return to IDLE and reset attempt counter

### Safety Features
- Maximum 3-minute pump run time per automatic cycle prevents pump damage
- Hard-coded 4-minute fail safe that disables the pump until a power cycle (see below)
- Automatic pump shutoff when tank is full
- Monitors for external filling (manual fill) even during wait periods
- Low CPU usage with strategic delays

### Fail Safe
As a hard-coded backstop, if the pump runs continuously for 4 minutes (`PUMP_FAILSAFE_TIME`) for any reason - in practice a manual `ON` left running or a logic fault, since automatic cycles stop at 3 minutes - the controller:
- Stops the pump and enters the `FAILSAFE` state
- Reports `"state":"FAILSAFE"` and `"failsafe":true` in the status (web and MQTT), shows a banner in the web interface, and turns the status LED on solid
- Ignores all web interface and MQTT commands (the web interface's `/toggle` returns HTTP 409)

Nothing on the network can clear it - cycle power to reset. The trip is stored in RTC memory, so it also survives software, crash, and watchdog resets; only a power-on reset clears it. Pressing the board's EN button, uploading firmware, or a computer on the USB port resetting the board (some serial monitors do this when they connect) also counts as a power-on reset. The fail safe watches the relay output directly, independent of the state machine, and if `loop()` ever hangs for 5 seconds the loop watchdog reboots the controller, which turns the relay off.

The automatic run limit (`PUMP_RUN_TIME`, 3 minutes) is kept below the fail safe time - the build fails if it isn't - so a fill attempt that runs out of time stops and retries later instead of tripping the fail safe. The automatic limit also counts time the relay was already on when the cycle started (for example, returning to automatic mode while the pump is running), so the relay never stays on for more than 3 minutes in automatic mode.

## Serial Monitoring
The controller outputs detailed status information via Serial (115200 baud):
- Startup configuration
- Current fuel level percentage and voltage
- State transitions
- Pump status (started/stopped)
- Time remaining in each state
- Attempt counts

### Example Serial Output
```
ESP32 Fuel Pump Controller Starting...
Initialization complete
Pin Configuration:
  Fuel Level Sensor: GPIO 34
  Relay Control: GPIO 23
  Status LED: GPIO 2

IDLE - Fuel Level: 28.5% (0.769V)
Tank below refill threshold (28.5% < 30%), starting pump cycle
>>> PUMP STARTED <<<
State changed to: PUMPING
PUMPING - Fuel Level: 45.2%, Time remaining: 175 seconds
PUMPING - Fuel Level: 78.1%, Time remaining: 110 seconds
PUMPING - Fuel Level: 88.3%, Time remaining: 65 seconds
Tank full detected (90.4%)!
>>> PUMP STOPPED <<<
State changed to: IDLE
```

## Configuration

### WiFi Settings
Before uploading, configure your WiFi credentials in the code:

```cpp
const char* HOSTNAME = "heater-controller"; // Network name for the device
const char* WIFI_SSID = "BigMission";      // Your WiFi network name
const char* WIFI_PASSWORD = "";            // Your WiFi password
```

The hostname is reported to the router via DHCP and advertised over mDNS, so the web interface is reachable at `http://heater-controller.local` as well as by IP address.

**Note**: The ESP32 supports only 2.4GHz WiFi networks, not 5GHz.

### MQTT
The controller runs its own MQTT broker on port 1883 (`MQTT_PORT`), so other applications on the local network connect straight to `heater-controller.local` (or its IP address) - no separate broker is needed. The broker is advertised over mDNS as `_mqtt._tcp` and has no authentication, so anything on the network can read status and send commands.

Topics use the hostname as a prefix:

| Topic | Direction | Payload |
|---|---|---|
| `heater-controller/status` | Published by the controller | JSON status (see below) |
| `heater-controller/command` | Sent by clients | `ON`, `OFF`, `TOGGLE`, or `AUTO` (see below) |

Status is published every second (`MQTT_PUBLISH_INTERVAL`) and immediately whenever the pump, state, or mode changes. The payload is identical to the web interface's `http://heater-controller.local/status` endpoint:

```json
{"fuelLevel":85.3,"voltage":0.304,"pumpOn":false,"state":"IDLE","manual":false,"failsafe":false}
```

| Field | Description |
|---|---|
| `fuelLevel` | Fuel level percentage (0-100) |
| `voltage` | Fuel sender voltage |
| `pumpOn` | `true` when the relay is energized |
| `state` | `IDLE`, `PUMPING`, `SHORT_WAIT`, `LONG_WAIT`, `MANUAL`, or `FAILSAFE` |
| `manual` | `true` when manual override is active (from the web interface or MQTT) |
| `failsafe` | `true` when the fail safe has tripped - the pump is disabled until power is cycled |

Commands do the same thing as the web interface buttons and are case-insensitive:

| Command | Action |
|---|---|
| `ON` | Switch to manual mode and turn the pump on |
| `OFF` | Switch to manual mode and turn the pump off |
| `TOGGLE` | Switch to manual mode and toggle the pump (the web interface's pump button) |
| `AUTO` | Turn the pump off and return to automatic mode, restarting the refill cycle (the "Return to Auto Mode" button) |

**Note**: In manual mode the pump stays in the commanded state - the 3-minute run limit and full-tank shutoff only apply in automatic mode, but the 4-minute [fail safe](#fail-safe) always applies. Because `AUTO` restarts the refill cycle, sending it while already in automatic mode interrupts any pump run or wait in progress. While the fail safe is tripped, all commands are ignored.

The broker is built on the [PicoMQTT](https://github.com/mlesniew/PicoMQTT) library, which keeps it small but has some limits:
- **No retained messages** - a new subscriber receives status on the next publish, within a second
- **QoS 0 delivery** - subscribers always receive messages at QoS 0. QoS 1 and 2 commands are acknowledged, but a command the client resends may be applied twice, so automations should send `ON`/`OFF` rather than `TOGGLE`
- **No last will** - clients detect that the controller has gone offline when their connection drops
- The web server and MQTT broker run in a background task separate from pump control, so network problems don't hold up the pump. They do share that task with each other: a subscriber that disappears without disconnecting (such as a sleeping phone) can pause the web interface and MQTT for about 10 seconds, and a client that stops partway through sending a request can leave them unresponsive for an extended period

To watch status and send a command from another machine (use the IP address if it can't resolve `.local` names):
```
mosquitto_sub -h heater-controller.local -t "heater-controller/status"
mosquitto_pub -h heater-controller.local -t "heater-controller/command" -m AUTO
```

### Adjustable Parameters
All timing and threshold values can be adjusted in the code:

```cpp
// Timing (in milliseconds)
const unsigned long PUMP_RUN_TIME = 3 * 60 * 1000UL;        // 3 minutes (must be less than PUMP_FAILSAFE_TIME, 4 minutes)
const unsigned long SHORT_WAIT_TIME = 60 * 60 * 1000UL;     // 1 hour
const unsigned long LONG_WAIT_TIME = 6 * 60 * 60 * 1000UL;  // 6 hours

// Fuel Level Constants
const float FUEL_FULL_VOLTAGE = 0.184;   // Voltage when tank is full
const float FUEL_EMPTY_VOLTAGE = 1.001;  // Voltage when tank is empty
const float FULL_THRESHOLD = 90.0;       // Consider full at 90%
const float REFILL_THRESHOLD = 30.0;     // Start refilling below 30%
```

## Installation

### Arduino IDE Setup
1. Install ESP32 board support in Arduino IDE:
   - Go to File → Preferences
   - Add to "Additional Board Manager URLs": 
     `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Go to Tools → Board → Boards Manager
   - Search for "esp32" and install "ESP32 by Espressif Systems"

2. Select your board:
   - Tools → Board → ESP32 Arduino → ESP32 Dev Module

3. Configure board settings:
   - Upload Speed: 115200
   - Flash Frequency: 80MHz
   - Flash Mode: QIO
   - Flash Size: 4MB
   - Partition Scheme: Default
   - Core Debug Level: None (or "Info" for debugging)

4. Install the MQTT broker library:
   - Go to Sketch → Include Library → Manage Libraries
   - Search for "PicoMQTT" and install it
   - **Note**: If PicoMQTT fails to compile with ESP32 core 3.1.x, update the core to 3.2 or later, or uncomment `#define PICOMQTT_EXTRA_CONNECT_METHODS` in the library's `config.h`

### Upload Instructions
1. **Configure WiFi**: Edit the WiFi credentials in the code
2. Connect ESP32 to computer via USB
3. Open the `heater-controller` folder in Arduino IDE (or open `heater-controller.ino` directly)
4. Select the correct COM port in Tools → Port
5. Click Upload button (or press Ctrl+U)
6. Open Serial Monitor (Tools → Serial Monitor) at 115200 baud
7. Note the IP address displayed after "WiFi connected!"
8. Access the web interface at http://[IP_ADDRESS]

**Note**: The Arduino IDE requires sketch files to be in a folder with the same name as the .ino file.

### PlatformIO (VS Code)
The repo root contains a `platformio.ini` (board `esp32dev`), so the project can also be built from VS Code with the PlatformIO extension (it installs the PicoMQTT library automatically):
1. **Configure WiFi**: Edit the WiFi credentials and hostname in `heater-controller/heater-controller.ino`
2. Connect ESP32 to computer via USB. If no COM port appears, install the Silicon Labs CP210x USB to UART driver.
3. Click **Upload** (→) in the PlatformIO toolbar
4. Click **Serial Monitor** (plug icon) to view output at 115200 baud
5. Access the web interface at http://heater-controller.local (or the IP address printed after "WiFi connected!")

## Wiring Diagram

### Fuel Level Sensor
```
Fuel Sender → GPIO 34 (ESP32)
Fuel Sender Ground → GND (ESP32)
```

### Relay Module
```
VCC → 3.3V (ESP32)
GND → GND (ESP32)
IN/Signal → GPIO 23 (ESP32)
```

### Status LED (Optional - GPIO 2 is built-in on most boards)
```
LED Anode (+) → GPIO 2 (ESP32) through 220Ω resistor
LED Cathode (-) → GND (ESP32)
```
**Note**: GPIO 2 is typically connected to the built-in LED on most ESP32 dev boards, so an external LED may not be necessary.

### Relay to Pump
```
Common (COM) → Power Supply +
Normally Open (NO) → Fuel Pump +
Fuel Pump - → Power Supply -
```

## Troubleshooting

### ESP32 doesn't start when powered without computer connection
**Solution**: The code has been optimized to initialize hardware (pins and ADC) before serial communication to ensure proper startup even when not connected via USB. If you experience issues:
- Ensure stable power supply (at least 500mA @ 5V or 3.3V depending on your board)
- Verify the power supply can handle WiFi initialization (WiFi can draw 200-400mA during connection)
- Check that GPIO 2 (LED) and GPIO 23 (relay) are not being pulled down by external circuitry during boot
- If WiFi network is unavailable, the device will still operate after a 10-second timeout

### Pump doesn't activate
- Check GPIO 23 wiring to relay module
- Verify relay module is powered (3.3V or 5V depending on module)
- Monitor Serial output to confirm state changes
- Use multimeter to verify GPIO 23 outputs 3.3V when pumping

### Fuel level reading incorrect
- Verify fuel sender is connected to GPIO 34
- Check Serial output for voltage readings
- Adjust FUEL_FULL_VOLTAGE and FUEL_EMPTY_VOLTAGE constants if needed
- Ensure ADC attenuation is set correctly (ADC_11db for 0-3.3V range)

### Pump runs continuously
- Verify fuel level sensor is working correctly
- Check that voltage decreases as tank fills
- Ensure FULL_THRESHOLD is achievable with your sensor
- The fail safe stops the pump after 4 minutes of continuous running (see [Fail Safe](#fail-safe))

### MQTT client can't connect or commands are ignored
- Check the Serial output for `MQTT broker started on port 1883`
- Connect by IP address if the client machine can't resolve `heater-controller.local`
- Commands must be published to `heater-controller/command` with a payload of `ON`, `OFF`, `TOGGLE`, or `AUTO`; anything else is logged as `Ignoring unknown MQTT command`
- While the fail safe is tripped, all commands are ignored until power is cycled

### Fail safe tripped (LED solid, state `FAILSAFE`)
- The pump ran continuously for 4 minutes. Automatic cycles stop at 3 minutes, so the usual cause is the pump left on in manual mode (`ON`, `TOGGLE`, or the web interface's pump button); otherwise check the Serial log for what kept it running
- Fix the cause, then cycle power to reset - web and MQTT commands can't clear it

## License
See LICENSE file for details.
