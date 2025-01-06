#include <Wire.h>
#include <LiquidCrystal_I2C.h>  // Library for I2C LCD
#include <WiFi.h>
#include "time.h"
#include <WebSocketsClient.h>
#include <SD.h>                 // Include SD card library
#include <SPI.h>                // SPI library for SD card communication
#include <stdbool.h>
#include <string.h>

// Wi-Fi credentials, SHOULD BE READ FROM SD CARD
const char* ssid           = "...";
const char* password       = "12345678";

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 7 * 3600; // TIMEZONE UTC+7
const int daylightOffset_sec = 0;

// Pin Definitions
int angleSensorPin = 34;      // Pin connected to CJMCU sensor, for analog reading of steering sensor
int difflockSwitchPin = 27;      // Pin connected to the switch, for enable/disable difflock
int buzzerAlerterPin = 26;    // Pin connected to the solenoid, which will interact (push/pull) the buzzerPin component
int speaker = 14; // Pin connected to the buzzer
int angleSensorValue = 0;     // Store sensor value
float angle = 0;         // Store calculated angle
bool isDifflockSwitchOn = false; // Track switch state
bool isBuzzerAlerterActive = false; // Track solenoid state
bool isDifflockSwitchOnNext = true; // Track switch previous state
bool isBuzzerAlerterActiveNext = true; // Track solenoid previous state

// RTC DS1302 Pins
const int PIN_RST = 5;
const int PIN_CLK = 18;
const int PIN_DAT = 19;

//Ds1302 rtc(PIN_RST, PIN_CLK, PIN_DAT); // Initialize DS1302 RTC

// Initialize LCD (I2C address 0x27, 16 characters, 2 lines)
LiquidCrystal_I2C lcd(0x27, 16, 2); //21 SDA //22 SCL

// Custom SD Card SPI Pins
const int CUSTOM_CS = 15;   // Chip Select pin for SD card
const int CUSTOM_SCK = 16;  // Serial Clock pin
const int CUSTOM_MOSI = 17; // Master Out Slave In pin
const int CUSTOM_MISO = 23; // Master In Slave Out pin

SPIClass spi = SPIClass(VSPI);  // Using VSPI bus for SD card

File myFile;

// Debounce variables
const unsigned long debounceDelay = 50; // 50 ms debounce time

// State variables
unsigned long lastSwitchTime = 0;
static unsigned long lastDataSaveTime = 0;
static unsigned long lastReconnectAttempt = 0;
bool isConnected = false; // Track connection status
bool isUnitError = false;

// SHOULD BE READ FROM SD CARD TO PREVENT HARD CODE
const char* UNIT_ID = "buzzergd";
const char* UNIT_NAME = "BuzzerGD";
String WEBSOCKET_URL = "faceted-supreme-oak.glitch.me";
const uint16_t WEBSOCKET_PORT = 80;
String WEBSOCKET_PATH = String("/units/") + UNIT_ID + String("?mode=sender");
const unsigned long WEBSOCKET_RECONNECT_DELAY = 5000;

// Create WebSocket client object
WebSocketsClient webSocket;

String jsonData = "{\"name\":\"" + String(UNIT_NAME) + "\",\"status\":{\"is_switch_on\":" + String(isDifflockSwitchOn ? "true" : "false") + ",\"is_difflock_on\":" + String(isBuzzerAlerterActive ? "true" : "false") + "}}";

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.println("WebSocket Disconnected! Attempting to reconnect...");
            isConnected = false; // Update the connection status
            lastReconnectAttempt = millis(); // Set the last reconnect attempt time
            break;
        case WStype_CONNECTED:
            Serial.println("Connected to WebSocket server!");
            isConnected = true; // Update the connection status
              // Send initial data
            webSocket.sendTXT(jsonData);
            Serial.println("Sent data: " + jsonData);
            break;
    }
}

void setup() {
    Serial.begin(115200);           // Start serial communication
    pinMode(angleSensorPin, INPUT);      // Set sensor pin as input
    pinMode(difflockSwitchPin, INPUT_PULLUP); // Set switch pin as input with pull-up
    pinMode(buzzerAlerterPin, OUTPUT);   // Set solenoid pin as output
    pinMode(speaker, OUTPUT); // Set buzzer pin as output
    digitalWrite(speaker, HIGH);  // Turn off buzzer after the delay                        

    lcd.begin();                    // Initialize LCD
    lcd.backlight();

    // Connect to Wi-Fi
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("Connected to WiFi");

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    lcd.setCursor(0, 0);
    lcd.print("System Ready");
    delay(2000);
    lcd.clear();

    // Initialize SPI for SD card
    spi.begin(CUSTOM_SCK, CUSTOM_MISO, CUSTOM_MOSI, CUSTOM_CS);

    // Initialize SD card
    if (!SD.begin(CUSTOM_CS, spi)) {
        Serial.println("SD card initialization failed!");
        return;
    }
    Serial.println("SD card initialized.");

    // Open or create a file for writing
    myFile = SD.open("/log.txt", FILE_WRITE);

    if (myFile) {
        Serial.println("Writing to log.txt...");
        myFile.println("This is data stored from ESP32 to SD card.");
        myFile.println("You can add more data here.");
        myFile.close();
        Serial.println("Writing complete.");
    } else {
        Serial.println("Failed to open log.txt.");
    }

    // Setup WebSocket path
    Serial.println("Initializing WebSocket connection...");
    Serial.println(WEBSOCKET_URL + WEBSOCKET_PATH);

    // Connect to WebSocket
    connectToServer();
}

void connectToServer() {
    webSocket.begin(WEBSOCKET_URL, WEBSOCKET_PORT, WEBSOCKET_PATH);
    webSocket.onEvent(webSocketEvent); // Set WebSocket event handler 
}

void loop() {
    // Call this method to keep the WebSocket connection alive
    webSocket.loop();

    // Check if we need to reconnect
    if (!isConnected) {
        // If not connected, check if enough time has passed to retry
        if (millis() - lastReconnectAttempt >= WEBSOCKET_RECONNECT_DELAY) {
            connectToServer(); // Try to reconnect
            lastReconnectAttempt = millis(); // Update the last reconnect attempt time
        }
    }

    // DELAY LOCAL LOGGING FOR 1 SECOND
    if (millis() - lastDataSaveTime < 1000) {
      return;
    }

    String json = "";
    char timestamp_now[30];
    char time_now[30];
    
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        // Convert timeinfo to timestamp format
        strftime(timestamp_now, sizeof(timestamp_now), "%Y-%m-%d %H:%M:%S", &timeinfo); // Format as "YYYY-MM-DD HH:MM:SS"
        strftime(time_now, sizeof(time_now), "%H:%M:%S", &timeinfo); // Format as "HH:MM:SS"
    } else {
        Serial.println("Failed to obtain time!");
    }

    if (!isUnitError) {
      // Display solenoid state and time on LCD
      lcd.setCursor(0, 0);
      lcd.print("Diff-lock: ");
      lcd.print(isBuzzerAlerterActive ? "ON " : "OFF");
  
      lcd.setCursor(0, 1);
      lcd.print("Time: ");
      lcd.print(time_now);
    }
    
    // Read sensor data
    angleSensorValue = analogRead(angleSensorPin);
    angle = (-0.000007 * (angleSensorValue * angleSensorValue)) + (0.0849 * angleSensorValue) + 0.0000000000005;

    // Debounce switch check, to detect difflockSwitch switch status if it is really on/off
    if (millis() - lastSwitchTime > debounceDelay) {
        lastSwitchTime = millis();
        int switchState = digitalRead(difflockSwitchPin);

        // Switch pressed
        if (switchState == LOW) {
            isDifflockSwitchOn = !isDifflockSwitchOn;
            isBuzzerAlerterActive = isDifflockSwitchOn;
            delay(800);
        }

        // Control solenoid based on angle
        if (isBuzzerAlerterActive) {
            if (angle >= 80.0 && angle <= 100.0) {
                // ONLY SEND IF DATA IS UPDATED
                if (isBuzzerAlerterActive == isBuzzerAlerterActiveNext) {
                    if (isUnitError) {
                        Serial.println("STATUS: Recovering from error...");
                        isDifflockSwitchOn = false;
                        isBuzzerAlerterActive = false;
                        isDifflockSwitchOnNext = !isDifflockSwitchOn;
                        isBuzzerAlerterActiveNext = !isBuzzerAlerterActive;
                        isUnitError = false;

                        // Append timeinfo to String
                        json = "{\"status\":{\"is_switch_on\":" + String(isDifflockSwitchOn ? "true" : "false") + ",\"is_difflock_on\":false},\"error\":{\"end_time\":\"" + String(timestamp_now) + "\"}}";
                       
                        webSocket.sendTXT(json); // Send error message to WebSocket
                        Serial.println(String("Data sent: " + String(json)));
                        
                        // Display solenoid state and time on LCD
                        lcd.clear();
                        
                        digitalWrite(buzzerAlerterPin, LOW);
                        digitalWrite(speaker, HIGH);  // Turn off buzzer after the delay
                    } else {
                        Serial.println("STATUS: Diff-lock ON");
                        isUnitError = false;
                        isDifflockSwitchOnNext = !isDifflockSwitchOn;
                        isBuzzerAlerterActiveNext = !isBuzzerAlerterActive;
                        digitalWrite(buzzerAlerterPin, HIGH);
//                        digitalWrite(speaker, HIGH);  // Turn off buzzer after the delay                        
                    }
                    webSocket.sendTXT("{\"status\":{\"is_switch_on\":" + String(isDifflockSwitchOn ? "true" : "false") + ",\"is_difflock_on\":" + String(isBuzzerAlerterActive ? "true" : "false") + "}}");
                    Serial.println(String("Data sent: {\"status\":{\"is_switch_on\":" + String(isDifflockSwitchOn ? "true" : "false") + ",\"is_difflock_on\":" + String(isBuzzerAlerterActive ? "true" : "false") + "}}"));  
                }
            } else {
                if (isUnitError) {
                  Serial.println("STATUS: Recovering from error without fixing steering angle...");
                  isDifflockSwitchOn = false;
                  isBuzzerAlerterActive = false;
                  isDifflockSwitchOnNext = !isDifflockSwitchOn;
                  isBuzzerAlerterActiveNext = !isBuzzerAlerterActive;
                  isUnitError = false;

                  // Append timeinfo to String
                  json = "{\"status\":{\"is_switch_on\":" + String(isDifflockSwitchOn ? "true" : "false") + ",\"is_difflock_on\":false},\"error\":{\"end_time\":\"" + String(timestamp_now) + "\"}}";
                 
                  webSocket.sendTXT(json); // Send error message to WebSocket
                  Serial.println(String("Data sent: " + String(json)));
                  
                  // Display solenoid state and time on LCD
                  lcd.clear();

                  digitalWrite(buzzerAlerterPin, LOW); // Deactivate the solenoid
                  digitalWrite(speaker, HIGH);  // Turn off buzzer after the delay
                } else {
                  // ERROR HANDLING FOR ANGLE OUT OF RANGE
                  isUnitError = true;
  
                  // Append timeinfo to String
                  json = "{\"status\":{\"is_switch_on\":" + String(isDifflockSwitchOn ? "true" : "false") + ",\"is_difflock_on\":false},\"error\":{\"message\":\"Improper Angle!\",\"start_time\":\"" + String(timestamp_now) + "\"}}";
  
                  digitalWrite(buzzerAlerterPin, LOW); // Deactivate the solenoid
                  isDifflockSwitchOn = false;
                  isBuzzerAlerterActive = false;
                  Serial.println("ERROR: Steering angle is out of range!");
                  webSocket.sendTXT(json); // Send error message to WebSocket
                  Serial.println(String("Data sent: " + String(json)));
                  isDifflockSwitchOnNext = !isDifflockSwitchOn;
                  isBuzzerAlerterActiveNext = !isBuzzerAlerterActive;
  
                  // Display solenoid state and time on LCD
                  lcd.clear();
                  lcd.setCursor(0, 0);
                  lcd.print("ERROR: Steering");
                  lcd.setCursor(0, 1);
                  lcd.print("Sync Active");
  
                  // Activate the buzzer when error occurs
                  digitalWrite(speaker, LOW); // Turn on buzzer  
                }
            }
        } else {
            if (isBuzzerAlerterActive == isBuzzerAlerterActiveNext) {
                Serial.println("STATUS: Diff-lock OFF (Inactive)");
                digitalWrite(buzzerAlerterPin, LOW);
                digitalWrite(speaker, HIGH);  // Turn off buzzer after the delay
                webSocket.sendTXT("{\"status\":{\"is_switch_on\":" + String(isDifflockSwitchOn ? "true" : "false") + ",\"is_difflock_on\":false}}");
                Serial.println(String("Data sent: {\"status\":{\"is_switch_on\":" + String(isDifflockSwitchOn ? "true" : "false") + ",\"is_difflock_on\":false}}"));
                isDifflockSwitchOn = !isDifflockSwitchOn;
                isBuzzerAlerterActiveNext = !isBuzzerAlerterActive;
            }
        }
    }

    // Print sensor value and angle to Serial Monitor
    Serial.print("Angle Sensor Value: ");
    Serial.print(angleSensorValue);
    Serial.print(", Angle: ");
    Serial.print(angle);
    Serial.print(", Switch State: ");
    Serial.println(isBuzzerAlerterActive ? "Active" : "Inactive");
    
    // Save data to SD card
    saveDataToSD(angleSensorValue, angle, isBuzzerAlerterActive, timestamp_now);
    lastDataSaveTime = millis(); // Update last save time
}

void saveDataToSD(int angleSensorValue, float angle, bool isBuzzerAlerterActive, char* timestamp_now) {
    File dataFile = SD.open("/dataLog.txt", FILE_APPEND);
    if (dataFile) {
        dataFile.print("Sensor Value: ");
        dataFile.print(angleSensorValue);
        dataFile.print(", Angle: ");
        dataFile.print(angle);
        dataFile.print(", Solenoid: ");
        dataFile.print(isBuzzerAlerterActive ? "Active" : "Inactive");
        dataFile.print(", Time: ");
        dataFile.print(timestamp_now);
        dataFile.close();
        Serial.println("Data logged to SD card.");
    } else {
        Serial.println("Failed to open dataLog.txt.");
    }
}
