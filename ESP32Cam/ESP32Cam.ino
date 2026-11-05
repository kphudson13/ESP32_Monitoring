/*
A script to send pictures from an esp32-cam via email
You need a credentials file to make this work
This esp32 also collects data from another module via ESP-NOW
Jan 2026
Kyle Hudson
Live laugh love
*/

#include "esp_camera.h"       // include library for the camera
#include <WiFi.h>             // for wifi connection
#include <ESP_Mail_Client.h>  // for mail client
#include <esp_now.h>          // To talk to other esp32
#include <RTClib.h>           // To convert data packet from unix time
#include "credentials.h"      // personal credentials file, not tracked with Git
#include <SD_MMC.h>           // to use SD card
#include <WebServer.h>        // for web server
#include <LittleFS.h>         // to save csv on board hardware

WebServer server(80);

// set pin configurations - #define replaces all the first value with the second before it hits the compiler
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

#define TEMP_LOG_FILE "/templog.csv"
#define MAX_LOG_POINTS 144  // 24hrs at 10min intervals

unsigned long lastLogTime = 0;
const unsigned long LOG_INTERVAL = 600000;  // 10 minutes in ms

String cachedSVG = "";

// The four target hours for photos
const uint8_t PHOTO_HOURS[4] = { 0, 6, 12, 18 };
const uint8_t EMAIL_HOUR = 18;  // Send email after 6pm photo

// Simple Mail Transfer Protocol objects
SMTPSession smtp;
ESP_Mail_Session session;

// Replace with the partner ESP32 MAC address
//uint8_t partnerMac[] = { 0xE4, 0xB0, 0x63, 0xB4, 0x2F, 0xE8 };

// structure for data from other esp
typedef struct {
  float temperature;
  float humidity;
  uint32_t unixTime;
} sensor_packet_t;

sensor_packet_t lastPacket;
bool hasPacket = false;

// Track which photo slots have been taken today
bool photoTaken[4] = { false, false, false, false };  // 12am, 6am, 12pm, 6pm
bool emailSentToday = false;

// Sensor readings saved at each photo time
float savedTemps[4] = { NAN, NAN, NAN, NAN };
float savedHums[4] = { NAN, NAN, NAN, NAN };
uint32_t savedTimes[4] = { 0, 0, 0, 0 };

int lastCheckedDay = -1;  // To detect day rollover

// Camera function
bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;  // Allows only one frame cached
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  return esp_camera_init(&config) == ESP_OK;
}

// WiFi function
uint8_t WiFiConnect(const char* nSSID, const char* nPassword) {

  Serial.print("Connecting to ");
  Serial.println(nSSID);
  WiFi.begin(nSSID, nPassword);

  uint8_t i = 0;
  while (WiFi.status() != WL_CONNECTED && i++ < 50) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();  // Just make a new line after loading dots

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed");
    return false;
  }
  Serial.println("WiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  Serial.println("ESP-NOW packet received");
  memcpy(&lastPacket, data, sizeof(lastPacket));
  hasPacket = true;
}

// Save photo to SD, returns filename or empty string on fail
String savePhotoToSD(camera_fb_t* fb, uint8_t hour) {
  char filename[32];
  snprintf(filename, sizeof(filename), "/photo_%02dh.jpg", hour);

  File file = SD_MMC.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
    return "";
  }

  file.write(fb->buf, fb->len);
  file.close();

  Serial.print("Photo saved to SD: ");
  Serial.println(filename);
  return String(filename);
}

// Discard stale frame, capture fresh one
camera_fb_t* captureFreshPhoto() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);
  delay(100);
  return esp_camera_fb_get();
}

char attNames[4][32];  // Declare above the loop

String buildTempSVG(uint32_t sinceUnix = 0) {
  const int W = 500;
  const int H = 200;
  const int PAD = 40;

  uint32_t times[MAX_LOG_POINTS];
  float temps[MAX_LOG_POINTS];
  int count = 0;

  File f = LittleFS.open(TEMP_LOG_FILE, FILE_READ);
  if (!f) return "<p>No data yet</p>";

  while (f.available() && count < MAX_LOG_POINTS) {
    String line = f.readStringUntil('\n');
    line.trim();

    int comma = line.indexOf(',');
    if (comma <= 0 || comma >= line.length() - 1) continue;

    uint32_t t = (uint32_t)line.substring(0, comma).toInt();
    float temp = line.substring(comma + 1).toFloat();

    if (t == 0 || !isfinite(temp)) continue;
    if (sinceUnix > 0 && t < sinceUnix) continue;

    times[count] = t;
    temps[count] = temp;
    count++;
  }
  f.close();

  if (count < 2) return "<p>Not enough data yet</p>";

  float tMin = temps[0];
  float tMax = temps[0];

  for (int i = 1; i < count; i++) {
    if (temps[i] < tMin) tMin = temps[i];
    if (temps[i] > tMax) tMax = temps[i];
  }

  tMin -= 1.0f;
  tMax += 1.0f;

  uint32_t xMin = times[0];
  uint32_t xMax = times[count - 1];

  String svg;
  svg.reserve(6000);

  svg = "<svg xmlns='http://www.w3.org/2000/svg' ";
  svg += "width='500' height='200' viewBox='0 0 500 200'>";
  svg += "<rect width='100%' height='100%' fill='#f9f9f9' rx='8'/>";

  // Axes
  svg += "<line x1='40' y1='10' x2='40' y2='160' "
         "stroke='#333' stroke-width='1'/>";
  svg += "<line x1='40' y1='160' x2='490' y2='160' "
         "stroke='#333' stroke-width='1'/>";

  // ---- Compact data  ----
  String points;
  points.reserve(count * 14);

  for (int i = 0; i < count; i++) {
    float xRatio = (float)(times[i] - xMin) / (float)(xMax - xMin);
    float yRatio = (temps[i] - tMin) / (tMax - tMin);

    int px = PAD + (int)(xRatio * (W - PAD - 10));
    int py = (H - PAD) - (int)(yRatio * (H - PAD - 10));

    px = constrain(px, PAD, W - 10);
    py = constrain(py, 10, H - PAD);

    points += String(px) + "," + String(py) + " ";
  }

  svg += "<polyline points='" + points + "' fill='none' stroke='#e05c2a' stroke-width='1.5'/>";

  svg += "<text x='2' y='160' font-size='10' fill='#666'>" + String(tMin, 1) + "C</text>";
  svg += "<text x='2' y='18' font-size='10' fill='#666'>" + String(tMax, 1) + "C</text>";

  DateTime startTime(xMin);
  DateTime endTime(xMax);

  char startLabel[12];
  char endLabel[12];

  snprintf(startLabel, sizeof(startLabel), "%02d:%02d",
           startTime.hour(), startTime.minute());
  snprintf(endLabel, sizeof(endLabel), "%02d:%02d",
           endTime.hour(), endTime.minute());

  svg += "<text x='40' y='192' font-size='10' fill='#666'>" + String(startLabel) + "</text>";
  svg += "<text x='462' y='192' font-size='10' fill='#666'>" + String(endLabel) + "</text>";

  svg += "</svg>";
  return svg;
}

void sendDailyEmail() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, attempting reconnect...");
    WiFiConnect(ssid, password);
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Reconnect failed, skipping email");
      return;
    }
  }

  session.server.host_name = "smtp.gmail.com";
  session.server.port = 465;
  session.login.email = smtp_email;
  session.login.password = smtp_password;
  session.login.user_domain = "";

  SMTP_Message message;
  message.sender.name = "ESP32-CAM";
  message.sender.email = smtp_email;
  message.subject = "Daily Apt. Report";
  message.addRecipient("Kyle", recip_email);

  uint32_t cutoff = lastPacket.unixTime - 86400;
  String svgChart = buildTempSVG(cutoff);

  String emailBody = "<h2>Daily Room Report</h2>";
  emailBody += "<p><b>Temperature at send time:</b> " + String(lastPacket.temperature, 1) + " &deg;C</p>";
  emailBody += "<p><b>Humidity:</b> " + String(lastPacket.humidity, 1) + " %</p>";
  emailBody += "<h3>Temperature - Last 24hrs</h3>";
  emailBody += svgChart;

  message.html.content = emailBody.c_str();
  message.html.charSet = "utf-8";

  const char* labels[4] = { "12:00 AM", "6:00 AM", "12:00 PM", "6:00 PM" };
  const char* photoHourLabels[4] = { "12am", "6am", "12pm", "6pm" };

  // Attach photos from SD
  SMTP_Attachment atts[4];
  uint8_t* bufs[4] = { nullptr, nullptr, nullptr, nullptr };  // Track for cleanup

  for (int i = 0; i < 4; i++) {
    char filename[32];
    snprintf(filename, sizeof(filename), "/photo_%02dh.jpg", PHOTO_HOURS[i]);

    if (SD_MMC.exists(filename)) {
      File f = SD_MMC.open(filename);
      if (f) {
        size_t fileSize = f.size();
        bufs[i] = (uint8_t*)malloc(fileSize);
        if (bufs[i]) {
          f.read(bufs[i], fileSize);
          f.close();
          snprintf(attNames[i], sizeof(attNames[i]), "photo_%s.jpg", photoHourLabels[i]);
          atts[i].descr.filename = attNames[i];
          atts[i].descr.mime = "image/jpeg";
          atts[i].blob.data = bufs[i];
          atts[i].blob.size = fileSize;
          atts[i].descr.transfer_encoding = Content_Transfer_Encoding::enc_base64;
          message.addAttachment(atts[i]);
          Serial.print("Attached: ");
          Serial.println(attNames[i]);
        } else {
          Serial.println("malloc failed for photo attachment");
          f.close();
        }
      }
    } else {
      Serial.print("Photo not found on SD: ");
      Serial.println(filename);
    }
  }

  if (!smtp.connect(&session)) {
    Serial.println("SMTP connect failed");
    for (int i = 0; i < 4; i++)
      if (bufs[i]) free(bufs[i]);
    return;
  }

  if (!MailClient.sendMail(&smtp, &message))
    Serial.println(smtp.errorReason());
  else
    Serial.println("Daily email sent successfully");

  smtp.closeSession();

  for (int i = 0; i < 4; i++)
    if (bufs[i]) free(bufs[i]);
}

void handleRoot() {
  String html = "<html><head>";
  html += "<meta http-equiv='refresh' content='30'>";
  html += "<style>body{font-family:sans-serif;padding:20px;} h1{color:#333;}</style>";
  html += "</head><body>";
  html += "<h1>Room Monitor</h1>";

  if (hasPacket) {
    DateTime now(lastPacket.unixTime);
    char timeStr[32];
    snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute());
    html += "<p><b>Last update:</b> " + String(timeStr) + "</p>";
    html += "<p><b>Current Temp:</b> " + String(lastPacket.temperature, 1) + " &deg;C</p>";
    html += "<p><b>Humidity:</b> " + String(lastPacket.humidity, 1) + " %</p>";
    html += "<h2>Temperature - Last 24hrs</h2>";
    html += cachedSVG.length() > 0 ? cachedSVG : "<p>Not enough data yet</p>";
  } else {
    html += "<p>Waiting for sensor data...</p>";
  }

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void logTemperature(float temp, uint32_t unixTime) {
  // Count existing lines to enforce 144 point rolling window
  int lineCount = 0;
  File f = LittleFS.open(TEMP_LOG_FILE, FILE_READ);
  if (f) {
    while (f.available()) {
      if (f.read() == '\n') lineCount++;
    }
    f.close();
  }

  // If at max, rewrite file without the first line
  if (lineCount >= MAX_LOG_POINTS) {
    File fin = LittleFS.open(TEMP_LOG_FILE, FILE_READ);
    String newContent = "";
    bool firstLine = true;
    while (fin.available()) {
      String line = fin.readStringUntil('\n');
      if (firstLine) {
        firstLine = false;
        continue;
      }  // skip oldest
      newContent += line + "\n";
    }
    fin.close();
    File fout = LittleFS.open(TEMP_LOG_FILE, FILE_WRITE);
    fout.print(newContent);
    fout.close();
  }

  // Append new point
  File fa = LittleFS.open(TEMP_LOG_FILE, FILE_APPEND);
  if (fa) {
    fa.printf("%u,%.1f\n", unixTime, temp);
    fa.close();
  }

  cachedSVG = buildTempSVG();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Setup started!");
  delay(500);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  } else {
    cachedSVG = buildTempSVG();
  }

  Serial.println("Starting camera initialization...");
  delay(500);
  if (!initCamera()) {
    Serial.println("Camera init failed");
    return;
  }
  Serial.println("Camera initialized!");

  if (!SD_MMC.begin()) {
    Serial.println("SD card mount failed");
    // Not fatal, photos just won't save
  } else {
    Serial.println("SD card mounted");
  }

  WiFi.mode(WIFI_STA);
  if (!WiFiConnect(ssid, password)) return;

  while (WiFi.macAddress() == "00:00:00:00:00:00") delay(100);
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("WiFi Channel: ");
  Serial.println(WiFi.channel());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onReceive);

  server.on("/", handleRoot);
  server.begin();
  Serial.println("Web server started");
  Serial.print("Visit: http://");
  Serial.println(WiFi.localIP());

  delay(500);

  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);

  Serial.println("Waiting for first ESP-NOW time packet...");
}

void loop() {

  server.handleClient();  // Launch server

  // Log every 10 minutes
  if (hasPacket && (lastLogTime == 0 || millis() - lastLogTime >= LOG_INTERVAL)) {
    logTemperature(lastPacket.temperature, lastPacket.unixTime);
    lastLogTime = millis();
  }

  // Can't do anything without a time reference
  if (!hasPacket) {
    Serial.println("No time data yet, waiting...");
    delay(5000);
    return;
  }

  // Non-blocking 30 second check
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 30000) return;
  lastCheck = millis();

  DateTime now(lastPacket.unixTime);
  int today = now.day();
  uint8_t currentHour = now.hour();
  uint8_t currentMinute = now.minute();

  // Reset daily flags on day rollover
  if (today != lastCheckedDay) {
    Serial.println("New day detected, resetting daily flags");
    for (int i = 0; i < 4; i++) {
      photoTaken[i] = false;
      savedTemps[i] = NAN;
      savedHums[i] = NAN;
      savedTimes[i] = 0;
    }
    emailSentToday = false;
    lastCheckedDay = today;
  }

  // Check each photo slot
  for (int i = 0; i < 4; i++) {
    if (photoTaken[i]) continue;
    if (currentHour != PHOTO_HOURS[i]) continue;

    // Within the first 5 minutes of the target hour
    if (currentMinute > 5) continue;
    Serial.print("Taking scheduled photo for hour: ");
    Serial.println(PHOTO_HOURS[i]);
    camera_fb_t* fb = captureFreshPhoto();

    if (!fb) {
      Serial.println("Camera capture failed");
      continue;
    }

    savePhotoToSD(fb, PHOTO_HOURS[i]);
    esp_camera_fb_return(fb);

    // Save sensor data at this time
    savedTemps[i] = lastPacket.temperature;
    savedHums[i] = lastPacket.humidity;
    savedTimes[i] = lastPacket.unixTime;
    photoTaken[i] = true;
    Serial.println("Photo slot complete");
  }

  // Send email after 6pm photo is taken
  if (!emailSentToday && photoTaken[3] && currentHour == EMAIL_HOUR) {
    Serial.println("Sending daily email...");
    sendDailyEmail();
    emailSentToday = true;
  }
}