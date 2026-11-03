/* 
A script to send data from this esp32 to another esp32 with a camera to further pass
Jan 2026 
Kyle Hudson
Live laugh love
*/

#include <DHT.h>               // For DHT module
#include <RTClib.h>            // Clock module
#include <Wire.h>              // I2C protocol
#include <esp_now.h>           // Esp now communication
#include <esp_wifi.h> 
#include <WiFi.h>              // Connect to wifi
#include <Adafruit_GFX.h>      // Digital display
#include <Adafruit_SSD1306.h>  // Digital display

#define DHTPIN 19  // For DHT
#define DHTTYPE DHT22

// For digital display
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1  // No reset pin
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

RTC_DS3231 rtc;
DHT dht(DHTPIN, DHTTYPE);

const unsigned char wifiOn[] PROGMEM = {
  0b00111100,
  0b01111110,
  0b11111111,
  0b11111111,
  0b11111111,
  0b01111110,
  0b00111100,
  0b00000000
};
const unsigned char wifiOff[] PROGMEM = { /* same but all 0 */ };

void showWifiIcon(bool connected) {
  display.drawBitmap(110, 0, connected ? wifiOn : wifiOff,
                     8, 8, SSD1306_WHITE);
  display.display();
}

// Replace with your ESP32-CAM MAC address
uint8_t partnerMac[] = { 0x88, 0x57, 0x21, 0xC1, 0x98, 0x40 };

typedef struct {
  float temperature;
  float humidity;
  uint32_t unixTime;
} sensor_packet_t;

sensor_packet_t packet;

bool oledOK = false;  // to allow oled to fail

void setup() {
  Serial.begin(115200);

  // Start I2C on custom pins
  Wire.begin(15, 16);  // SDA, SCL. For clock module and OLED

  dht.begin();

  // To allow serial monitor to configure
  unsigned long start = millis();
  while (!Serial && millis() - start < 2000) {
    delay(10);
  }

  Serial.println("Starting WiFi...");
  // ESP-NOW setup
  WiFi.mode(WIFI_STA);
  Serial.print("WiFi Channel: ");
  Serial.println(WiFi.channel());

  esp_wifi_set_channel(11, WIFI_SECOND_CHAN_NONE);  // Match cam's WiFi channel

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  /* Only need this to get mac address once
  // Wait for WiFi to start
  while (WiFi.macAddress() == "00:00:00:00:00:00") {
    delay(100);
  }
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  */

  Serial.println("Attempting RTC init...");
  // Check we can find clock module
  if (!rtc.begin()) {
    Serial.println("Couldn't find DS3231");
    while (1)
      ;
  }
  // Only uncomment this to reset time to computer system
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  if (rtc.lostPower()) {
    Serial.println("RTC lost power, syncing to compile time");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, partnerMac, 6);
  peerInfo.channel = 11;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }

  // Initialize OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("OLED allocation failed");
    oledOK = false;
  } else {
    oledOK = true;

    // Initial display before data
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("ESP32 Sender");
    display.println("Live laugh love");
    display.display();
    delay(1500);
  }

  delay(200);  // Minor delay to let modules stablize.
}

void loop() {

  DateTime now = rtc.now();

  packet.temperature = dht.readTemperature();
  packet.humidity = dht.readHumidity();
  packet.unixTime = now.unixtime();

  if (isnan(packet.temperature) || isnan(packet.humidity)) {
    Serial.println("DHT read failed");
    delay(4000);
    return;
  }

  if (oledOK) {  // Wrapped in if incase OLED fails
    display.clearDisplay();

    // TIME (big font)
    display.setTextSize(2);

    // Build time string
    char timeStr[6];  // "HH:MM"
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", now.hour(), now.minute());

    // Calculate text width (6 px per char × text size)
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);

    // Center horizontally
    int16_t x = (SCREEN_WIDTH - w) / 2;

    display.setCursor(x, 0);
    display.print(timeStr);

    // Separator on the LED ----
    display.setTextSize(1);    // Back to normal size
    display.setCursor(0, 24);  // Move down below large time
    display.println("---------------------");

    // Temp & Humidity
    display.print("Temp: ");
    display.print(packet.temperature, 1);
    display.println(" C");

    display.print("Hum:  ");
    display.print(packet.humidity, 1);
    display.println(" %");

    display.display();
  }

  esp_err_t result = esp_now_send(partnerMac,
                                  (uint8_t *)&packet,
                                  sizeof(packet));

  if (result == ESP_OK) {
    // erase previous icon
    display.fillRect(110, 0, 8, 8, 0);                        // clear icon area
    display.drawBitmap(110, 0, wifiOn, 8, 8, SSD1306_WHITE);  // draw icon
    display.display();                                        // single refresh
    delay(1000);                                              // keep icon on
    display.fillRect(110, 0, 8, 8, 0);                        // erase again
    display.display();                                        // single refresh
  }

  if (result == ESP_OK) {
    Serial.println("ESP-NOW send OK");
  } else {
    Serial.println("ESP-NOW send FAIL");
  }

  // All just to print in the serial monitor
  Serial.print(now.year());
  Serial.print("-");
  Serial.print(now.month());
  Serial.print("-");
  Serial.print(now.day());
  Serial.print(" ");
  Serial.print(now.hour());
  Serial.print(":");
  Serial.print(now.minute());
  Serial.print(":");
  Serial.print(now.second());
  Serial.println();
  Serial.print("Temp: ");
  Serial.println(packet.temperature);
  Serial.print("Hum: ");
  Serial.println(packet.humidity);
  Serial.println("--------------------");
  delay(15000);
}