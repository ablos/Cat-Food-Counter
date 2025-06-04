#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PubSubClient.h>
#include "config.h"

// Structs
struct FeedingMoment 
{
    uint32_t dateTimeValue;

    String dateString() 
    {
        uint16_t date = dateTimeValue / 10000;
        
        if (date == 9999)
            return "?\?-?\?";

        char buffer[7];
        sprintf(buffer, "%02d-%02d", date / 100, date % 100);
        return String(buffer);
    }
    
    String timeString() 
    {
        uint16_t time = dateTimeValue % 10000;
        
        if (time == 9999)
            return "?\?:?\?";

        char buffer[7];
        sprintf(buffer, "%02d:%02d", time / 100, time % 100);
        return String(buffer);
    }
};

struct Memory 
{
    uint32_t crc32;
    uint32_t lastWakeTime;
    uint32_t feedings[4];
    uint8_t feedingCount;
    uint8_t pressCount;
    uint16_t padding;       // Extra padding for memory alignment
};

// Defenitions
#define MULTI_PRESS_WINDOW 1000
#define OLED_POWER_PIN D4
#define VDIV_ENABLE_PIN D5

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

// Function declarations
uint32_t calculateCRC32(const uint8_t *data, size_t length);
bool readMemory(Memory* data);
void writeMemory(Memory* data);
void decideAction(uint8_t pressCount);
float getBatteryVoltage();
void wakeDisplay();
void turnOffDisplay();
void updateDisplay();
bool isDisplayOn();
void waitForDisplayOff();
void addFeedingToMemory(uint32_t dateTimeValue);
uint32_t getLatestFeedingFromMemory();
void removeLatestFeedingFromMemory();
void clearAllFeedingsFromMemory();
bool connectMqtt();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void addFeeding();
void removeFeeding();

// Variables
Memory memoryData;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
unsigned long displayStartTime = 0;
WiFiClient wifi;
PubSubClient mqtt(wifi);
uint32_t currentDateTime = 99999999;
float currentBatteryVoltage = 0;

void setup()
{
    system_phy_set_powerup_option(1);  // Minimal RF during boot
    WiFi.mode(WIFI_OFF);               // Explicit WiFi disable
    WiFi.forceSleepBegin();            // Force radio sleep

    bool validData = readMemory(&memoryData);
    uint32_t currentTime = millis();
    
    // Check if this wake is within multi-press window and update press count accordingly
    if (validData && (currentTime - memoryData.lastWakeTime < MULTI_PRESS_WINDOW)) 
        memoryData.pressCount++;
    else
        memoryData.pressCount = 1;

    // Save the updated count
    memoryData.lastWakeTime = currentTime;
    writeMemory(&memoryData);

    Serial.begin(115200);
    Serial.printf("\n\nPress count: %d\n", memoryData.pressCount);
    
    wakeDisplay();
    
    // Wait to see if more presses are coming
    delay(MULTI_PRESS_WINDOW + 50);
    
    // No more presses came, so reset presscount and execute action
    uint8_t pressCount = memoryData.pressCount;
    memoryData.pressCount = 0;
    writeMemory(&memoryData);
    decideAction(pressCount);

    // Return to deep sleep
    waitForDisplayOff();
    ESP.deepSleep(0);
}

void loop() {}

// Decides an action based on press count
void decideAction(uint8_t pressCount) 
{
    switch (pressCount) 
    {
        // Handle single press
        case 1: 
            Serial.println("Single press detected");
            Serial.println(getBatteryVoltage());
            break;

        // Handle double press
        case 2:
            Serial.println("Double press detected");
            addFeeding();
            break;

        // Handle triple press
        case 3:
            Serial.println("Triple press detected");
            removeFeeding();
            break;

        // Handle quadriple press
        case 4:
            Serial.println("Quadriple press detected");
            clearAllFeedingsFromMemory();
            updateDisplay();
            break;

        // Default behaviour
        default:
            Serial.println("Too many presses - treating as single");
            break;
    }
}

// Checks the battery voltage
float getBatteryVoltage() 
{
    // Enable voltage divider
    pinMode(VDIV_ENABLE_PIN, OUTPUT);
    digitalWrite(VDIV_ENABLE_PIN, HIGH);
    delay(50);

    // Set up analog pin
    pinMode(A0, INPUT);
    
    // Take multiple readings and average them
    const int numReadings = 10;
    int totalReading = 0;

    for (int i = 0; i < numReadings; i++) 
    {
        totalReading += analogRead(A0);
        delay(10);
    }

    // Read and convert voltage
    int adcValue = totalReading / numReadings;
    float adcVoltage = (adcValue / 1024.0) * MCP_OUTPUT_VOLTAGE;
    float batteryVoltage = adcVoltage * ((330.0 + 680.0) / 680.0);
    
    // Disable voltage divider
    digitalWrite(VDIV_ENABLE_PIN, LOW);

    return batteryVoltage + VOLTAGE_OFFSET;
}

// Turns the display on after giving power with the transistor
void wakeDisplay() 
{
    // Power on OLED screen
    pinMode(OLED_POWER_PIN, OUTPUT);
    digitalWrite(OLED_POWER_PIN, HIGH);
    delay(10);

    // Initialize display
    display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
    
    // Update display
    updateDisplay();
}

// Turns the display off and cuts power with transistor
void turnOffDisplay() 
{
    // Shut down display
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    display.clearDisplay();
    delay(10);

    // Power down OLED screen
    pinMode(OLED_POWER_PIN, OUTPUT);
    digitalWrite(OLED_POWER_PIN, LOW);

    Serial.println("Display turned off");
}

// Updates the display information
void updateDisplay() 
{
    // Clear buffer
    display.clearDisplay();
    
    // Set text values
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    
    // Retrieve latest feeding
    FeedingMoment latestMoment;
    latestMoment.dateTimeValue = getLatestFeedingFromMemory();

    // Print text
    display.println("Latest feeding date: " + latestMoment.dateString());
    display.println("Latest feeding time: " + latestMoment.timeString());
    display.println("Current feeding count: " + String(memoryData.feedingCount));

    // Put on display
    display.display();

    // Start display timer
    displayStartTime = millis();
}

// Checks if the display is on, if it is on for more than the wake time, turn it off
bool isDisplayOn() 
{
    if (millis() - displayStartTime > SCREEN_WAKE_TIME) 
    {
        turnOffDisplay();
        return false;
    }

    return true;
}

// Wait until the display is off
void waitForDisplayOff() 
{
    while (isDisplayOn()) 
    {
        yield();
    }
}

// Add a new feeding moment
void addFeedingToMemory(uint32_t dateTimeValue) 
{
    if (memoryData.feedingCount >= 4)
        return;

    // Shift existing feedings right
    for (int i = 3; i > 0; i--)
        memoryData.feedings[i] = memoryData.feedings[i - 1];
    
    // Add new feedings at front
    memoryData.feedings[0] = dateTimeValue;
    if (memoryData.feedingCount < 4)
        memoryData.feedingCount++;

    writeMemory(&memoryData);
}

// Retrieve the latest feeding moment
uint32_t getLatestFeedingFromMemory() 
{
    if (memoryData.feedingCount > 0)
        return memoryData.feedings[0];

    return 99999999;
}

// Remove the latest feeding moment
void removeLatestFeedingFromMemory() 
{
    if (memoryData.feedingCount == 0)
        return;
        
    // Shift everything left
    for (int i = 0; i < 3; i++)
        memoryData.feedings[i] = memoryData.feedings[i + 1];

    memoryData.feedingCount--;
    memoryData.feedings[memoryData.feedingCount] = 0;

    writeMemory(&memoryData);
}

// Clears all feeding records
void clearAllFeedingsFromMemory() 
{
    memoryData.feedingCount = 0;

    for (int i = 0; i < 4; i++)
        memoryData.feedings[i] = 0;

    writeMemory(&memoryData);
}

// Connects to WiFi and MQTT
bool connectMqtt() 
{
    Serial.println("Connecting to MQTT...");
    // First read battery voltage, since WiFi can create noise on the analog input
    currentBatteryVoltage = getBatteryVoltage();

    // Enable WiFi
    WiFi.forceSleepWake();
    WiFi.mode(WIFI_STA);
    
    // Configure static IP if provided
    if (strlen(STATIC_IP) > 0) 
    {
        IPAddress ip, gateway, subnet, dns;
        ip.fromString(STATIC_IP);
        gateway.fromString(GATEWAY_IP);
        subnet.fromString(SUBNET_MASK);
        dns.fromString(DNS_SERVER);

        WiFi.config(ip, gateway, subnet, dns);
    }

    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Wait for connection with 10 sec timeout
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000)
        delay(10);
        
    // If we failed to connect after timeout return
    if (WiFi.status() != WL_CONNECTED)
        return false;
        
    // Set up MQTT
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    
    // Connect to MQTT (with authentication if present)
    bool connected = false;
    
    if (strlen(MQTT_USER) > 0)
        connected = mqtt.connect(MQTT_NAME, MQTT_USER, MQTT_PASS);
    else
        connected = mqtt.connect(MQTT_NAME);

    Serial.println("Connection successfull: " + String(connected));

    return connected;
}

// Disconnects from MQTT and WiFi
void disconnectMqtt() 
{
    mqtt.disconnect();
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
}

// This is called when a MQTT message is received
void mqttCallback(char* topic, byte* payload, unsigned int length) 
{
    // We receive the current time, parse it and store it
    currentDateTime = 0;
    for (unsigned int i = 0; i < length; i++)
    {
        if (payload[i] >= '0' && payload[i] <= '9')
            currentDateTime = currentDateTime * 10 + (payload[i] - '0');
    }

    Serial.printf("Received datetime: %d\n", currentDateTime);
}

// Sends the latest data over MQTT
void sendUpdate() 
{
    if (mqtt.connected()) 
    {
        Serial.println("Sending update...");

        String json = "{\"count\":" + String(memoryData.feedingCount) + ", \"datetime\":" + String(getLatestFeedingFromMemory()) + ", \"battery-voltage\":" + String(currentBatteryVoltage) + "}";

        mqtt.publish(MQTT_SEND, json.c_str());
    }
}

// Adds a feeding moment, both synced to MQTT and to memory
void addFeeding() 
{
    Serial.println("Adding feeding...");

    // Try to connect to MQTT
    bool connected = connectMqtt();
    if (connected)
    {
        // Subscribe to the time topic
        mqtt.subscribe(MQTT_RECV);
        
        // Wait for the retained message with 3 sec timeout
        unsigned long start = millis();
        while (millis() - start < 3000 && currentDateTime == 99999999) 
        {
            mqtt.loop();
            yield();
        }
    }
    
    // Check if current date is still as last feeding date, if not reset feedings
    if (memoryData.feedingCount > 0 && currentDateTime / 10000 != getLatestFeedingFromMemory() / 10000)
        clearAllFeedingsFromMemory();

    // Add feeding with current time to memory
    addFeedingToMemory(currentDateTime);
    
    // Update the display
    updateDisplay();
    
    // Send update over MQTT
    sendUpdate();

    // Disconnect MQTT
    disconnectMqtt();
}

// Removes the last feeding moment both from MQTT and memory
void removeFeeding() 
{
    Serial.println("Removing feeding...");

    // Remove the latest feeding from memory
    removeLatestFeedingFromMemory();
    
    // Update the display
    updateDisplay();
    
    // Try to connect to MQTT and send update
    if (connectMqtt())
        sendUpdate();
        
    // Disconnect MQTT
    disconnectMqtt();
}

// Reads RTC memory
bool readMemory(Memory* data) 
{
    if (ESP.rtcUserMemoryRead(0, (uint32_t*)data, sizeof(Memory))) 
    {
        uint32_t crcOfData = calculateCRC32((uint8_t *)&data->lastWakeTime, sizeof(Memory) - sizeof(data->crc32));
        
        if (crcOfData == data->crc32)
            return true;
    }
    
    // Initialize with defaults if invalid
    data->lastWakeTime = 0;
    data->pressCount = 0;
    return false;
}

// Writes RTC memory
void writeMemory(Memory* data) 
{
    data->crc32 = calculateCRC32((uint8_t *)&data->lastWakeTime, sizeof(Memory) - sizeof(data->crc32));
    ESP.rtcUserMemoryWrite(0, (uint32_t *)data, sizeof(Memory));
}

// Calculates CRC for data validity
uint32_t calculateCRC32(const uint8_t *data, size_t length) 
{
    uint32_t crc = 0xffffffff;
    
    while (length--) 
    {
        uint8_t c = *data++;

        for (uint32_t i = 0x80; i > 0; i >>= 1) 
        {
            bool bit = crc & 0x80000000;
            
            if (c & i)
                bit = !bit;

            crc <<= 1;
            
            if (bit)
                crc ^= 0x04c11db7;
        }
    }

    return crc;
}