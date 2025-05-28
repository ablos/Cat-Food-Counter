#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Memory data
struct Memory 
{
    uint32_t crc32;
    uint32_t lastWakeTime;
    uint8_t pressCount;
};

// Defenitions
#define MULTI_PRESS_WINDOW 200 // In ms
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
void checkBatteryVoltage();
void wakeDisplay();

// Variables
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void setup()
{
    system_phy_set_powerup_option(1);  // Minimal RF during boot
    WiFi.mode(WIFI_OFF);               // Explicit WiFi disable
    WiFi.forceSleepBegin();            // Force radio sleep

    Memory data;
    bool validData = readMemory(&data);
    uint32_t currentTime = millis();
    
    // Check if this wake is within multi-press window and update press count accordingly
    if (validData && (currentTime - data.lastWakeTime < MULTI_PRESS_WINDOW)) 
        data.pressCount++;
    else
        data.pressCount = 1;

    // Save the updated count
    data.lastWakeTime = currentTime;
    writeMemory(&data);

    Serial.begin(115200);
    Serial.printf("\n\nPress count: %d\n", data.pressCount);
    
    // Wait to see if more presses are coming
    delay(MULTI_PRESS_WINDOW + 50);
    
    // No more presses came, so reset presscount and execute action
    uint8_t pressCount = data.pressCount;
    data.pressCount = 0;
    writeMemory(&data);
    decideAction(pressCount);

    // Return to deep sleep
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
            wakeDisplay();
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
    Serial.println("Turning on display");
    pinMode(OLED_POWER_PIN, OUTPUT);
    digitalWrite(OLED_POWER_PIN, HIGH);
    delay(10);

    // Initialize display
    if (display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) 
    {
        Serial.println("Printing...");
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
    }
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