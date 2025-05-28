#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Structs
struct FeedingTime 
{
    uint8_t hour;
    uint8_t minute;
    
    String toString() 
    {
        char buffer[6];
        sprintf(buffer, "%02d:%02d", hour, minute);
        return String(buffer);
    }
};

struct Memory 
{
    uint32_t crc32;
    uint32_t lastWakeTime;
    uint8_t pressCount;
    uint8_t padding1;
    uint16_t feedings[4];
    uint8_t feedingCount;
    uint8_t padding2;
};

// Defenitions
#define MULTI_PRESS_WINDOW 200
#define OLED_POWER_PIN D4
#define VDIV_ENABLE_PIN D5

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define SCREEN_WAKE_TIME 5000

// Function declarations
uint32_t calculateCRC32(const uint8_t *data, size_t length);
bool readMemory(Memory* data);
void writeMemory(Memory* data);
void decideAction(uint8_t pressCount);
void checkBatteryVoltage();
void wakeDisplay();
void turnOffDisplay();
void updateDisplay();
bool isDisplayOn();
void waitForDisplayOff();
void addFeeding(uint16_t timeValue);
uint16_t getLatestFeeding();
void removeLatestFeeding();
void clearAllFeedings();

// Variables
Memory memoryData;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
unsigned long displayStartTime = 0;

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
    
    // Wait to see if more presses are coming
    delay(MULTI_PRESS_WINDOW + 50);
    
    // No more presses came, so reset presscount and execute action
    uint8_t pressCount = memoryData.pressCount;
    memoryData.pressCount = 0;
    writeMemory(&memoryData);
    wakeDisplay();
    decideAction(pressCount);

    // Return to deep sleep
    waitForDisplayOff();
    ESP.deepSleep(0);
}

void loop() {}

void decideAction(uint8_t pressCount) 
{
    // Decide action based on press count
    switch (pressCount) 
    {
        // Handle single press
        case 1: 
            Serial.println("Single press detected");
            checkBatteryVoltage();
            break;

        // Handle double press
        case 2:
            Serial.println("Double press detected");
            break;
            
        // Handle triple press
        case 3:
            Serial.println("Triple press detected");
            break;
            
        // Default behaviour
        default:
            Serial.println("Too many presses - treating as single");
            break;
    }
}

void checkBatteryVoltage() 
{
    // Enable voltage divider
    pinMode(VDIV_ENABLE_PIN, OUTPUT);
    digitalWrite(VDIV_ENABLE_PIN, HIGH);
    delay(10);

    // Set up analog pin
    pinMode(A0, INPUT);
    
    // Read and convert voltage
    int adcValue = analogRead(A0);
    float adcVoltage = (adcValue / 1024.0) * 3.25;
    float batteryVoltage = adcVoltage * ((330.0 + 680.0) / 680.0);

    Serial.println(adcVoltage);
    Serial.println(batteryVoltage);
    
    // Disable voltage divider
    digitalWrite(VDIV_ENABLE_PIN, LOW);
}

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

void turnOffDisplay() 
{
    // Shut down display
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    display.clearDisplay();
    delay(10);

    // Power down OLED screen
    pinMode(OLED_POWER_PIN, OUTPUT);
    digitalWrite(OLED_POWER_PIN, LOW);
}

void updateDisplay() 
{
    // Clear buffer
    display.clearDisplay();
    
    // Set text values
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    
    // Print text
    display.println("Hello world!");

    // Put on display
    display.display();

    // Start display timer
    displayStartTime = millis();
}

bool isDisplayOn() 
{
    if (millis() - displayStartTime > SCREEN_WAKE_TIME) 
    {
        turnOffDisplay();
        return false;
    }

    return true;
}

void waitForDisplayOff() 
{
    while (isDisplayOn()) 
    {
        yield();
    }
}

void addFeeding(uint16_t timeValue) 
{
    if (memoryData.feedingCount >= 4)
        return;

    // Shift existing feedings right
    for (int i = 3; i > 0; i--)
        memoryData.feedings[i] = memoryData.feedings[i - 1];
    
    // Add new feedings at front
    memoryData.feedings[0] = timeValue;
    if (memoryData.feedingCount < 4)
        memoryData.feedingCount++;

    writeMemory(&memoryData);
}

uint16_t getLatestFeeding() 
{
    if (memoryData.feedingCount > 0)
        return memoryData.feedings[0];

    return 0;
}

void removeLatestFeeding() 
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

void clearAllFeedings() 
{
    memoryData.feedingCount = 0;

    for (int i = 0; i < 4; i++)
        memoryData.feedings[i] = 0;

    writeMemory(&memoryData);
}

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

void writeMemory(Memory* data) 
{
    data->crc32 = calculateCRC32((uint8_t *)&data->lastWakeTime, sizeof(Memory) - sizeof(data->crc32));
    ESP.rtcUserMemoryWrite(0, (uint32_t *)data, sizeof(Memory));
}

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