/*
 * ESP32 Fuel Pump Controller
 * Controls a fuel pump relay based on fuel level sensor input
 * 
 * Hardware:
 * - ESP-WROOM-32 (ESP32 ESP-32S)
 * - Fuel level sensor connected to analog input
 * - Relay module connected to digital output
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <PicoMQTT.h>  // MQTT broker - install "PicoMQTT" from the Arduino Library Manager

// WiFi Configuration
const char* HOSTNAME = "heater-controller";  // Web interface at http://heater-controller.local
const char* WIFI_SSID = "BigMission";
const char* WIFI_PASSWORD = "";  // Set your WiFi password here

// MQTT Broker Configuration
// The controller runs its own MQTT broker, so other applications connect straight to it (no external broker needed).
// Topics use HOSTNAME as the prefix:
//   heater-controller/status   JSON status, same fields as the web interface's /status
//   heater-controller/command  ON, OFF, TOGGLE (manual mode) or AUTO - the same actions as the web interface buttons
const uint16_t MQTT_PORT = 1883;
const unsigned long MQTT_PUBLISH_INTERVAL = 1000;  // 1 second; also publishes immediately on pump/state/mode change

// Pin Definitions
#define FUEL_LEVEL_PIN 34        // Analog input pin (ADC1_CH6, use GPIO 34-39 for analog input only)
#define RELAY_PIN 23             // Digital output pin to control relay
#define LED_PIN 2                // Status LED - blinks at 1Hz (loop running) or 5Hz (pump active)
#define LED_ACTIVE_LOW false     // Set to true if LED is active-low (on when pin is LOW)

// Fuel Level Constants (in volts)
const float FUEL_FULL_VOLTAGE = 0.184;   // Voltage when tank is full
const float FUEL_EMPTY_VOLTAGE = 1.001;  // Voltage when tank is empty

// ESP32 ADC Constants
const float ADC_MAX_VOLTAGE = 3.3;       // ESP32 ADC reference voltage
const int ADC_RESOLUTION = 4095;         // 12-bit ADC (0-4095)

// Timing Constants (in milliseconds)
const unsigned long PUMP_RUN_TIME = 5 * 60 * 1000UL;        // 5 minutes
const unsigned long SHORT_WAIT_TIME = 60 * 60 * 1000UL;     // 1 hour
const unsigned long LONG_WAIT_TIME = 6 * 60 * 60 * 1000UL;  // 6 hours
const unsigned long SENSOR_READ_INTERVAL = 1000;            // 1 second

// Fuel Level Thresholds (percentage)
const float FULL_THRESHOLD = 90.0;       // Consider tank full at 90%
const float REFILL_THRESHOLD = 30.0;     // Start refilling when below 30%

// State Machine States
enum PumpState {
  IDLE,
  PUMPING,
  SHORT_WAIT,
  LONG_WAIT,
  MANUAL  // Manual override mode
};

// Global Variables
PumpState currentState = IDLE;
unsigned long stateStartTime = 0;
int attemptCount = 0;
unsigned long lastSensorRead = 0;
float currentFuelLevel = 0.0;
unsigned long lastLedToggle = 0;
bool ledState = false;
bool manualMode = false;

// Web Server
WebServer server(80);

// MQTT Broker - serviced by networkTask alongside the web server
PicoMQTT::Server mqtt(MQTT_PORT);
String mqttStatusTopic;
String mqttCommandTopic;
unsigned long lastMqttPublish = 0;
bool lastPublishedPumpOn = false;
PumpState lastPublishedState = IDLE;
bool lastPublishedManual = false;

// Commands from the web interface and MQTT. Those run in networkTask, so commands are queued
// and applied by loop(), which owns the pump state machine.
enum CommandType {
  CMD_PUMP_ON,
  CMD_PUMP_OFF,
  CMD_PUMP_TOGGLE,
  CMD_AUTO
};
struct ControlCommand {
  CommandType type;
  const char* source;  // String literal for logging, e.g. "web interface"
};
QueueHandle_t commandQueue = NULL;

// Status snapshot - written by loop(), read by networkTask for /status and MQTT
struct StatusSnapshot {
  float fuelLevel;
  bool pumpOn;
  PumpState state;
  bool manual;
};
StatusSnapshot statusSnapshot = {0.0, false, IDLE, false};
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;

void setup() {
  // Configure pins FIRST - critical for proper operation
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);  // Ensure pump is off initially
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);    // Ensure LED is off initially
  
  // Configure ADC - ESP32 has 12-bit ADC by default (0-4095)
  // Set attenuation for full 0-3.3V range on the specific pin
  analogSetPinAttenuation(FUEL_LEVEL_PIN, ADC_11db);
  
  // Initialize serial communication for debugging
  Serial.begin(115200);
  delay(100);  // Short delay for serial to stabilize
  
  Serial.println();
  Serial.println("========================================");
  Serial.println("ESP32 Fuel Pump Controller Starting...");
  Serial.println("========================================");
  
  Serial.println("Hardware initialization complete");
  Serial.println("Pin Configuration:");
  Serial.print("  Fuel Level Sensor: GPIO ");
  Serial.println(FUEL_LEVEL_PIN);
  Serial.print("  Relay Control: GPIO ");
  Serial.println(RELAY_PIN);
  Serial.print("  Status LED: GPIO ");
  Serial.println(LED_PIN);
  Serial.println();
  
  // Connect to WiFi
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.setHostname(HOSTNAME);  // Must be set before WiFi.mode() to take effect
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Access web interface at: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection failed. Continuing without web interface.");
  }
  Serial.println();
  
  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/toggle", handleToggle);
  server.begin();
  Serial.println("Web server started");

  // Advertise HOSTNAME.local via mDNS (also works if WiFi connects after setup)
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("mqtt", "tcp", MQTT_PORT);
    Serial.print("mDNS started: http://");
    Serial.print(HOSTNAME);
    Serial.println(".local");
  } else {
    Serial.println("mDNS failed to start");
  }

  // Start the MQTT broker (status publishing and commands)
  setupMqtt();
  Serial.println();
  
  // Initial state
  currentState = IDLE;
  stateStartTime = millis();
  attemptCount = 0;
  updateStatusSnapshot();

  // Service the web server and MQTT broker in their own task on core 0, so a slow or vanished client can't
  // hold up the pump state machine in loop() on core 1. Idle priority because the libraries busy-wait on
  // stalled clients; at a higher priority that would starve core 0's idle task and trip the task watchdog.
  commandQueue = xQueueCreate(8, sizeof(ControlCommand));
  if (commandQueue == NULL ||
      xTaskCreatePinnedToCore(networkTask, "network", 8192, NULL, tskIDLE_PRIORITY, NULL, 0) != pdPASS) {
    Serial.println("Failed to start network task - web interface and MQTT unavailable");
  }
}

void loop() {
  unsigned long currentTime = millis();
  
  // Apply commands queued by the web interface and MQTT (see networkTask)
  applyQueuedCommands();
  
  // Read fuel level periodically
  if (currentTime - lastSensorRead >= SENSOR_READ_INTERVAL) {
    currentFuelLevel = readFuelLevel();
    lastSensorRead = currentTime;
  }
  
  // LED blinks at different rates depending on pump status
  // Pump ON (relay active): 5Hz (100ms on, 100ms off) - fast blink
  // Pump OFF: 1Hz (500ms on, 500ms off) - slow heartbeat
  bool pumpIsRunning = (digitalRead(RELAY_PIN) == HIGH);
  unsigned long ledInterval = pumpIsRunning ? 100 : 500;
  
  if (currentTime - lastLedToggle >= ledInterval) {
    ledState = !ledState;
    // Handle active-low or active-high LED
    if (LED_ACTIVE_LOW) {
      digitalWrite(LED_PIN, ledState ? LOW : HIGH);  // Inverted for active-low
    } else {
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);  // Normal active-high
    }
    lastLedToggle = currentTime;
  }
  
  // State machine (only run if not in manual mode)
  if (!manualMode) {
    switch (currentState) {
      case IDLE:
        handleIdleState(currentTime);
        break;
        
      case PUMPING:
        handlePumpingState(currentTime);
        break;
        
      case SHORT_WAIT:
        handleShortWaitState(currentTime);
        break;
        
      case LONG_WAIT:
        handleLongWaitState(currentTime);
        break;
        
      case MANUAL:
        // Manual mode - do nothing, controlled by web interface or MQTT commands
        break;
    }
  }
  
  // Share the latest status with networkTask (after the state machine so changes go out immediately)
  updateStatusSnapshot();

  delay(10);  // Reduced delay for smoother LED blinking
}

// Read fuel level sensor and convert to percentage
float readFuelLevel() {
  int adcValue = analogRead(FUEL_LEVEL_PIN);
  float voltage = (adcValue / (float)ADC_RESOLUTION) * ADC_MAX_VOLTAGE;
  
  // Convert voltage to fuel level percentage
  // Note: Lower voltage = fuller tank, higher voltage = emptier tank
  float fuelPercent = 0.0;
  
  if (voltage <= FUEL_FULL_VOLTAGE) {
    fuelPercent = 100.0;
  } else if (voltage >= FUEL_EMPTY_VOLTAGE) {
    fuelPercent = 0.0;
  } else {
    // Linear interpolation between full and empty
    fuelPercent = 100.0 - ((voltage - FUEL_FULL_VOLTAGE) / (FUEL_EMPTY_VOLTAGE - FUEL_FULL_VOLTAGE) * 100.0);
  }
  
  return fuelPercent;
}

// Check if tank is full
bool isTankFull() {
  return currentFuelLevel >= FULL_THRESHOLD;
}

// Start the pump
void startPump() {
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println(">>> PUMP STARTED <<<");
}

// Stop the pump
void stopPump() {
  digitalWrite(RELAY_PIN, LOW);
  Serial.println(">>> PUMP STOPPED <<<");
}

// Transition to a new state
void changeState(PumpState newState) {
  currentState = newState;
  stateStartTime = millis();
  
  Serial.print("State changed to: ");
  switch (newState) {
    case IDLE:
      Serial.println("IDLE");
      break;
    case PUMPING:
      Serial.println("PUMPING");
      break;
    case SHORT_WAIT:
      Serial.println("SHORT_WAIT (1 hour)");
      break;
    case LONG_WAIT:
      Serial.println("LONG_WAIT (6 hours)");
      break;
  }
}

// Handle IDLE state
void handleIdleState(unsigned long currentTime) {
  static unsigned long lastStatusPrint = 0;
  
  // Print status every 10 seconds
  if (currentTime - lastStatusPrint >= 10000) {
    float voltage = FUEL_FULL_VOLTAGE + (100.0 - currentFuelLevel) / 100.0 * (FUEL_EMPTY_VOLTAGE - FUEL_FULL_VOLTAGE);
    Serial.print("IDLE - Fuel Level: ");
    Serial.print(currentFuelLevel, 1);
    Serial.print("% (");
    Serial.print(voltage, 3);
    Serial.println("V)");
    lastStatusPrint = currentTime;
  }
  
  // Check if tank needs refilling
  if (currentFuelLevel < REFILL_THRESHOLD) {
    Serial.print("Tank below refill threshold (");
    Serial.print(currentFuelLevel, 1);
    Serial.print("% < ");
    Serial.print(REFILL_THRESHOLD, 0);
    Serial.println("%), starting pump cycle");
    attemptCount = 0;
    startPump();
    changeState(PUMPING);
  }
}

// Handle PUMPING state
void handlePumpingState(unsigned long currentTime) {
  unsigned long elapsedTime = currentTime - stateStartTime;
  
  // Print status every 5 seconds while pumping
  static unsigned long lastStatusPrint = 0;
  if (currentTime - lastStatusPrint >= 5000) {
    unsigned long remainingTime = PUMP_RUN_TIME - elapsedTime;
    Serial.print("PUMPING - Fuel Level: ");
    Serial.print(currentFuelLevel, 1);
    Serial.print("%, Time remaining: ");
    Serial.print(remainingTime / 1000);
    Serial.println(" seconds");
    lastStatusPrint = currentTime;
  }
  
  // Check if tank is full
  if (isTankFull()) {
    Serial.print("Tank full detected (");
    Serial.print(currentFuelLevel, 1);
    Serial.println("%)!");
    stopPump();
    attemptCount = 0;
    changeState(IDLE);
    return;
  }
  
  // Check if max pump time reached
  if (elapsedTime >= PUMP_RUN_TIME) {
    stopPump();
    attemptCount++;
    Serial.print("Pump cycle ");
    Serial.print(attemptCount);
    Serial.print(" complete. Tank level: ");
    Serial.print(currentFuelLevel, 1);
    Serial.println("%");
    
    if (attemptCount >= 2) {
      // After 2 attempts, switch to long wait (6 hour intervals)
      Serial.println("2 attempts completed without reaching full. Switching to 6-hour intervals.");
      changeState(LONG_WAIT);
    } else {
      // First attempt, use short wait (1 hour)
      Serial.println("Waiting 1 hour before next attempt.");
      changeState(SHORT_WAIT);
    }
  }
}

// Handle SHORT_WAIT state (1 hour wait)
void handleShortWaitState(unsigned long currentTime) {
  unsigned long elapsedTime = currentTime - stateStartTime;
  
  // Print status every 60 seconds during wait
  static unsigned long lastStatusPrint = 0;
  if (currentTime - lastStatusPrint >= 60000) {
    unsigned long remainingTime = SHORT_WAIT_TIME - elapsedTime;
    Serial.print("SHORT_WAIT - Fuel Level: ");
    Serial.print(currentFuelLevel, 1);
    Serial.print("%, Time remaining: ");
    Serial.print(remainingTime / 60000);
    Serial.println(" minutes");
    lastStatusPrint = currentTime;
  }
  
  // Check if tank somehow became full during wait (external fill)
  if (isTankFull()) {
    Serial.print("Tank full detected during wait (");
    Serial.print(currentFuelLevel, 1);
    Serial.println("%)!");
    attemptCount = 0;
    changeState(IDLE);
    return;
  }
  
  // Check if wait time is over
  if (elapsedTime >= SHORT_WAIT_TIME) {
    Serial.println("Short wait complete. Starting next pump cycle.");
    startPump();
    changeState(PUMPING);
  }
}

// Handle LONG_WAIT state (6 hour wait)
void handleLongWaitState(unsigned long currentTime) {
  unsigned long elapsedTime = currentTime - stateStartTime;
  
  // Print status every 5 minutes during long wait
  static unsigned long lastStatusPrint = 0;
  if (currentTime - lastStatusPrint >= 300000) {
    unsigned long remainingTime = LONG_WAIT_TIME - elapsedTime;
    Serial.print("LONG_WAIT - Fuel Level: ");
    Serial.print(currentFuelLevel, 1);
    Serial.print("%, Time remaining: ");
    Serial.print(remainingTime / 60000);
    Serial.println(" minutes");
    lastStatusPrint = currentTime;
  }
  
  // Check if tank somehow became full during wait (external fill)
  if (isTankFull()) {
    Serial.print("Tank full detected during wait (");
    Serial.print(currentFuelLevel, 1);
    Serial.println("%)!");
    attemptCount = 0;
    changeState(IDLE);
    return;
  }
  
  // Check if wait time is over
  if (elapsedTime >= LONG_WAIT_TIME) {
    Serial.println("Long wait complete. Starting pump cycle (6-hour interval mode).");
    startPump();
    changeState(PUMPING);
  }
}

// Web Server Handlers

// Serve main HTML page
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Fuel Pump Controller</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background: #f0f0f0; }";
  html += ".container { max-width: 600px; margin: 0 auto; background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #333; text-align: center; margin-top: 0; }";
  html += ".status-card { background: #f8f9fa; padding: 20px; border-radius: 8px; margin: 20px 0; }";
  html += ".fuel-level { font-size: 48px; font-weight: bold; text-align: center; margin: 20px 0; }";
  html += ".fuel-bar-container { width: 100%; height: 40px; background: #e0e0e0; border-radius: 20px; overflow: hidden; margin: 20px 0; }";
  html += ".fuel-bar { height: 100%; background: linear-gradient(90deg, #ff4444 0%, #ffaa00 50%, #44ff44 100%); transition: width 0.3s; }";
  html += ".info-row { display: flex; justify-content: space-between; margin: 10px 0; padding: 10px; background: white; border-radius: 5px; }";
  html += ".info-label { font-weight: bold; color: #666; }";
  html += ".info-value { color: #333; }";
  html += ".pump-status { text-align: center; font-size: 24px; font-weight: bold; padding: 15px; border-radius: 8px; margin: 20px 0; }";
  html += ".pump-on { background: #4CAF50; color: white; }";
  html += ".pump-off { background: #f44336; color: white; }";
  html += ".button { width: 100%; padding: 20px; font-size: 20px; font-weight: bold; border: none; border-radius: 8px; cursor: pointer; margin: 10px 0; transition: all 0.3s; }";
  html += ".button:hover { opacity: 0.8; transform: scale(1.02); }";
  html += ".button-on { background: #4CAF50; color: white; }";
  html += ".button-off { background: #f44336; color: white; }";
  html += ".button-auto { background: #2196F3; color: white; }";
  html += ".mode-badge { display: inline-block; padding: 5px 15px; border-radius: 15px; font-size: 14px; margin-left: 10px; }";
  html += ".mode-auto { background: #2196F3; color: white; }";
  html += ".mode-manual { background: #ff9800; color: white; }";
  html += "</style>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>Fuel Pump Controller";
  html += "<span class='mode-badge' id='modeBadge'>AUTO</span>";
  html += "</h1>";
  
  html += "<div class='status-card'>";
  html += "<div class='fuel-level' id='fuelLevel'>--</div>";
  html += "<div class='fuel-bar-container'><div class='fuel-bar' id='fuelBar' style='width: 0%'></div></div>";
  html += "<div class='info-row'><span class='info-label'>Voltage:</span><span class='info-value' id='voltage'>--</span></div>";
  html += "<div class='info-row'><span class='info-label'>State:</span><span class='info-value' id='state'>--</span></div>";
  html += "<div class='info-row'><span class='info-label'>Mode:</span><span class='info-value' id='mode'>--</span></div>";
  html += "</div>";
  
  html += "<div class='pump-status' id='pumpStatus'>PUMP OFF</div>";
  
  html += "<button class='button button-on' id='toggleBtn' onclick='togglePump()'>TURN PUMP ON</button>";
  html += "<button class='button button-auto' onclick='setAutoMode()'>RETURN TO AUTO MODE</button>";
  
  html += "</div>";
  
  html += "<script>";
  html += "function updateStatus() {";
  html += "  fetch('/status').then(r => r.json()).then(data => {";
  html += "    document.getElementById('fuelLevel').textContent = data.fuelLevel.toFixed(1) + '%';";
  html += "    document.getElementById('fuelBar').style.width = data.fuelLevel + '%';";
  html += "    document.getElementById('voltage').textContent = data.voltage.toFixed(3) + 'V';";
  html += "    document.getElementById('state').textContent = data.state;";
  html += "    document.getElementById('mode').textContent = data.manual ? 'MANUAL' : 'AUTO';";
  html += "    document.getElementById('modeBadge').textContent = data.manual ? 'MANUAL' : 'AUTO';";
  html += "    document.getElementById('modeBadge').className = 'mode-badge ' + (data.manual ? 'mode-manual' : 'mode-auto');";
  html += "    var pumpStatus = document.getElementById('pumpStatus');";
  html += "    var toggleBtn = document.getElementById('toggleBtn');";
  html += "    if (data.pumpOn) {";
  html += "      pumpStatus.textContent = 'PUMP ON';";
  html += "      pumpStatus.className = 'pump-status pump-on';";
  html += "      toggleBtn.textContent = 'TURN PUMP OFF';";
  html += "      toggleBtn.className = 'button button-off';";
  html += "    } else {";
  html += "      pumpStatus.textContent = 'PUMP OFF';";
  html += "      pumpStatus.className = 'pump-status pump-off';";
  html += "      toggleBtn.textContent = 'TURN PUMP ON';";
  html += "      toggleBtn.className = 'button button-on';";
  html += "    }";
  html += "  });";
  html += "}";
  html += "function togglePump() {";
  html += "  fetch('/toggle').then(() => updateStatus());";
  html += "}";
  html += "function setAutoMode() {";
  html += "  fetch('/toggle?auto=1').then(() => updateStatus());";
  html += "}";
  html += "setInterval(updateStatus, 500);";  // Poll twice per second
  html += "updateStatus();";
  html += "</script>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

// Build status JSON - shared by the web interface (/status) and MQTT so both report the same data
String buildStatusJson(const StatusSnapshot& status) {
  float voltage = FUEL_FULL_VOLTAGE + (100.0 - status.fuelLevel) / 100.0 * (FUEL_EMPTY_VOLTAGE - FUEL_FULL_VOLTAGE);
  
  String stateStr;
  switch (status.state) {
    case IDLE: stateStr = "IDLE"; break;
    case PUMPING: stateStr = "PUMPING"; break;
    case SHORT_WAIT: stateStr = "SHORT_WAIT"; break;
    case LONG_WAIT: stateStr = "LONG_WAIT"; break;
    case MANUAL: stateStr = "MANUAL"; break;
  }
  
  String json = "{";
  json += "\"fuelLevel\":" + String(status.fuelLevel, 1) + ",";
  json += "\"voltage\":" + String(voltage, 3) + ",";
  json += "\"pumpOn\":" + String(status.pumpOn ? "true" : "false") + ",";
  json += "\"state\":\"" + stateStr + "\",";
  json += "\"manual\":" + String(status.manual ? "true" : "false");
  json += "}";

  return json;
}

// Return status as JSON
void handleStatus() {
  server.send(200, "application/json", buildStatusJson(getStatusSnapshot()));
}

// Toggle pump on/off or return to auto mode
void handleToggle() {
  CommandType type = server.hasArg("auto") ? CMD_AUTO : CMD_PUMP_TOGGLE;
  bool queued = queueCommand(type, "web interface");
  
  if (queued) {
    server.send(200, "text/plain", "OK");
  } else {
    server.send(503, "text/plain", "Busy, try again");
  }
}

// Network Task - services the web server and MQTT broker on core 0, so a slow or vanished client
// can't delay the pump state machine in loop()
void networkTask(void* parameter) {
  for (;;) {
    server.handleClient();
    mqtt.loop();
    publishMqttStatus(millis());
    vTaskDelay(pdMS_TO_TICKS(10));  // Don't spin when there's nothing to do
  }
}

// Queue a command for loop() to apply (called from networkTask). Returns false if it couldn't be queued.
bool queueCommand(CommandType type, const char* source) {
  ControlCommand command = {type, source};
  // loop() drains the queue every ~10 ms, so wait briefly rather than drop the latest command of a burst
  if (xQueueSend(commandQueue, &command, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.print("Command queue full, dropping command from ");
    Serial.println(source);
    return false;
  }
  return true;
}

// Apply commands queued by the web interface and MQTT (called from loop())
void applyQueuedCommands() {
  ControlCommand command;
  while (xQueueReceive(commandQueue, &command, 0) == pdTRUE) {
    switch (command.type) {
      case CMD_PUMP_ON:
        setManualPump(true, command.source);
        break;
      case CMD_PUMP_OFF:
        setManualPump(false, command.source);
        break;
      case CMD_PUMP_TOGGLE:
        setManualPump(digitalRead(RELAY_PIN) != HIGH, command.source);
        break;
      case CMD_AUTO:
        setAutoMode(command.source);
        break;
    }
  }
}

// Copy the current status for networkTask (called from loop())
void updateStatusSnapshot() {
  StatusSnapshot status = {currentFuelLevel, digitalRead(RELAY_PIN) == HIGH, currentState, manualMode};
  portENTER_CRITICAL(&statusMux);
  statusSnapshot = status;
  portEXIT_CRITICAL(&statusMux);
}

// Read the latest status (called from networkTask)
StatusSnapshot getStatusSnapshot() {
  portENTER_CRITICAL(&statusMux);
  StatusSnapshot status = statusSnapshot;
  portEXIT_CRITICAL(&statusMux);
  return status;
}

// Manual Control - applied by loop() for commands from the web interface and MQTT

// Enter manual mode (if not already) and turn the pump on or off
void setManualPump(bool on, const char* source) {
  if (!manualMode) {
    manualMode = true;
    currentState = MANUAL;
    Serial.print("Entered MANUAL mode via ");
    Serial.println(source);
  }

  if (on == (digitalRead(RELAY_PIN) == HIGH)) {
    return;  // Pump is already in the requested state
  }
  if (on) {
    startPump();
  } else {
    stopPump();
  }
  Serial.print("Pump turned ");
  Serial.print(on ? "ON" : "OFF");
  Serial.print(" via ");
  Serial.println(source);
}

// Turn the pump off and return to automatic mode, restarting the refill cycle
void setAutoMode(const char* source) {
  manualMode = false;
  stopPump();
  changeState(IDLE);
  attemptCount = 0;
  Serial.print("Returned to AUTO mode via ");
  Serial.println(source);
}

// MQTT

// Queue a command published to the command topic: ON, OFF, TOGGLE, or AUTO (case-insensitive)
void handleMqttCommand(const char* payload) {
  String command = payload;
  command.trim();
  command.toUpperCase();

  if (command == "ON") {
    queueCommand(CMD_PUMP_ON, "MQTT");
  } else if (command == "OFF") {
    queueCommand(CMD_PUMP_OFF, "MQTT");
  } else if (command == "TOGGLE") {
    queueCommand(CMD_PUMP_TOGGLE, "MQTT");
  } else if (command == "AUTO") {
    queueCommand(CMD_AUTO, "MQTT");
  } else {
    Serial.print("Ignoring unknown MQTT command: ");
    Serial.println(command.substring(0, 20));  // Truncated - loop() waits on Serial too
  }
}

// Start the MQTT broker and listen for commands from clients
void setupMqtt() {
  mqttStatusTopic = String(HOSTNAME) + "/status";
  mqttCommandTopic = String(HOSTNAME) + "/command";

  // Runs inside mqtt.loop() on networkTask. Only queue the command here - never publish from this callback,
  // since the broker is still forwarding the incoming message to other subscribers.
  mqtt.subscribe(mqttCommandTopic, [](const char* topic, const char* payload) {
    handleMqttCommand(payload);
  });

  // Client packets are read synchronously in networkTask, so a client that connects without sending anything,
  // or stalls mid-packet, holds up the web server and other clients (not pump control). This bounds each read;
  // LAN clients respond in milliseconds.
  mqtt.socket_timeout_millis = 1000;
  mqtt.begin();

  Serial.print("MQTT broker started on port ");
  Serial.print(MQTT_PORT);
  Serial.print(", topics: ");
  Serial.print(mqttStatusTopic);
  Serial.print(", ");
  Serial.println(mqttCommandTopic);
}

// Publish status when the pump, state, or mode changes, and at least every MQTT_PUBLISH_INTERVAL
void publishMqttStatus(unsigned long currentTime) {
  StatusSnapshot status = getStatusSnapshot();
  bool changed = status.pumpOn != lastPublishedPumpOn || status.state != lastPublishedState || status.manual != lastPublishedManual;
  if (!changed && currentTime - lastMqttPublish < MQTT_PUBLISH_INTERVAL) {
    return;
  }

  // Sent only to currently subscribed clients - the broker doesn't support retained messages
  mqtt.publish(mqttStatusTopic, buildStatusJson(status));

  lastMqttPublish = currentTime;
  lastPublishedPumpOn = status.pumpOn;
  lastPublishedState = status.state;
  lastPublishedManual = status.manual;
}
