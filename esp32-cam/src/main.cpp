#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_camera.h"
#include "img_converters.h"  // fmt2jpg() - conversie RGB565 -> JPEG

// ======================= CONFIGURARE =======================
// WiFi
#define WIFI_SSID     "Alexandra"
#define WIFI_PASSWORD "Alex2702"

// Backend server URL (IP-ul laptopului pe reteaua WiFi)
#define BACKEND_HOST "10.213.151.82"
#define BACKEND_PORT 5000
#define FRAME_ENDPOINT "/api/camera/frame"

// Interval intre cadre (ms). 200ms ≈ 5 FPS — compromis bun
// intre fluiditate si ce poate duce ESP32-ul cu conversie RGB->JPEG.
#define CAPTURE_INTERVAL_MS 200

// Calitate JPEG la conversie (0-100, mai mare = mai buna calitate, mai mult traffic)
#define JPEG_QUALITY 80

// Timeout HTTP (ms)
#define HTTP_TIMEOUT_MS 5000

// ===================== PINII AI THINKER ====================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ======================== GLOBALS ==========================
String backendUrl;
unsigned long lastFrameTime = 0;
unsigned long frameCount = 0;
unsigned long fpsTimer = 0;

// ======================= FUNCTII ===========================

/**
 * Initializeaza camera OV2640 in modul RGB565.
 * Frame-urile brute vor fi convertite in JPEG cu fmt2jpg() inainte de trimitere.
 */
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;

  config.xclk_freq_hz = 10000000;          // 10MHz — viteza scazuta pentru stabilitate
  config.pixel_format = PIXFORMAT_RGB565;   // Format brut — convertim in JPEG manual
  config.frame_size   = FRAMESIZE_QVGA;    // 320x240 — bun pentru streaming cu conversie soft
  config.jpeg_quality = 12;                // Necesar in config chiar daca nu e JPEG nativ
  config.fb_count     = 1;                 // Single buffer (RGB565 e mare, economisim RAM)

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Eroare la initializarea camerei: 0x%x\n", err);
    return false;
  }

  // Ajustari senzor pentru streaming
  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    s->set_brightness(s, 1);    // Luminozitate usor crescuta
    s->set_saturation(s, 0);    // Saturatie normala
  }

  Serial.println("Camera initializata cu succes!");
  return true;
}

/**
 * Conectare la WiFi cu retry.
 */
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("Conectare la WiFi '%s'", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectat!");
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("\nWiFi ESUAT! Se va reincerca...");
  }
}

/**
 * Captura un frame RGB565, il converteste in JPEG cu fmt2jpg(),
 * si il trimite prin HTTP POST la backend.
 * Returneaza true daca frame-ul a fost trimis cu succes.
 */
bool captureAndSendFrame() {
  // Captura frame (RGB565 brut)
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Eroare: nu am putut captura frame!");
    return false;
  }

  // Converteste RGB565 -> JPEG
  uint8_t *jpeg_buf = NULL;
  size_t jpeg_len = 0;

  bool converted = fmt2jpg(
    fb->buf, fb->len,
    fb->width, fb->height,
    PIXFORMAT_RGB565,
    JPEG_QUALITY,
    &jpeg_buf, &jpeg_len
  );

  // Eliberam buffer-ul camerei imediat ce avem JPEG-ul
  esp_camera_fb_return(fb);

  if (!converted || jpeg_buf == NULL) {
    Serial.println("Eroare la compresia JPEG!");
    if (jpeg_buf) free(jpeg_buf);
    return false;
  }

  // Trimite JPEG prin HTTP POST
  HTTPClient http;
  http.begin(backendUrl);
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(HTTP_TIMEOUT_MS);

  int httpCode = http.POST(jpeg_buf, jpeg_len);

  // Eliberam memoria JPEG alocata de fmt2jpg
  free(jpeg_buf);

  // Verifica raspuns
  if (httpCode == 200) {
    return true;
  } else {
    if (httpCode > 0) {
      Serial.printf("HTTP POST esuat: cod %d\n", httpCode);
    } else {
      Serial.printf("HTTP POST eroare conexiune: %s\n",
                     http.errorToString(httpCode).c_str());
    }
    http.end();
    return false;
  }
}

// ========================= SETUP ===========================
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  delay(1000);
  Serial.println("\n\n=== PET-CAM ESP32 ===");

  // Construim URL-ul backend
  backendUrl = String("http://") + BACKEND_HOST + ":" +
               String(BACKEND_PORT) + FRAME_ENDPOINT;
  Serial.printf("Backend URL: %s\n", backendUrl.c_str());

  // Initializeaza camera
  if (!initCamera()) {
    Serial.println("Camera nu a pornit! Restart in 5s...");
    delay(5000);
    ESP.restart();
  }

  // Conectare WiFi
  connectWiFi();

  fpsTimer = millis();
  Serial.println("Setup complet - incep streaming-ul!\n");
}

// ========================= LOOP ============================
void loop() {
  // Reconectare WiFi daca s-a deconectat
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi deconectat — reincerc...");
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      delay(2000);
      return;
    }
  }

  // Respecta intervalul de captura
  unsigned long now = millis();
  if (now - lastFrameTime < CAPTURE_INTERVAL_MS) {
    delay(1); // pauza scurta 
    return;
  }
  lastFrameTime = now;

  // Captura si trimite
  if (captureAndSendFrame()) {
    frameCount++;
  }

  // Afiseaza FPS la fiecare 5 secunde
  if (now - fpsTimer >= 5000) {
    float fps = (float)frameCount / ((now - fpsTimer) / 1000.0);
    Serial.printf("FPS: %.1f | Free heap: %u bytes | RSSI: %d dBm\n",
                  fps, ESP.getFreeHeap(), WiFi.RSSI());
    frameCount = 0;
    fpsTimer = now;
  }
}
