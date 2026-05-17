#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include "esp_camera.h"
#include "img_converters.h"  // fmt2jpg(), converts RGB565 to JPEG
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include <WebServer.h>

// WiFi
#define WIFI_SSID     "Alexandra"
#define WIFI_PASSWORD "Alex2702"

// Backend server
#define BACKEND_HOST "10.213.151.82"  // laptop IP on the WiFi network
#define BACKEND_PORT 5000
#define FRAME_ENDPOINT "/api/camera/frame"
#define MOTION_ENDPOINT "/api/motion"
#define AUTOTRACKING_STATUS_ENDPOINT "/api/settings/autotracking/sync"

// Interval between frames (ms)
#define CAPTURE_INTERVAL_MS 200

// JPEG quality for conversion (0-100, higher = better quality, more traffic)
#define JPEG_QUALITY 80

// HTTP timeout (ms)
#define HTTP_TIMEOUT_MS 5000

// ===================== CAMERA PINS ====================
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
String motionUrl;
String autotrackingStatusUrl;
unsigned long lastFrameTime = 0;
unsigned long frameCount = 0;
unsigned long fpsTimer = 0;

// ======================== SENSORS & BUZZER ==========================
#define PIR_PIN 13
#define BUZZER_PIN 4

volatile bool motionDetected = false;

int buzzerState = 0;
unsigned long buzzerStartTime = 0;

Adafruit_VL53L0X lox = Adafruit_VL53L0X();
bool loxInitialized = false;

bool autotrackingEnabled = false;
bool isScanning = false;
int scanPan = 0;
int scanTilt = 0;
int scanPanDir = 10;
int scanTiltStep = 10;
uint16_t minDistance = 8190;
int bestPan = 110;
int bestTilt = 10;
unsigned long lastScanMoveTime = 0;
unsigned long lastScanCompleteTime = 0;

// ======================== WEB SERVER =======================
WebServer server(80);

// ======================== I2C & PCA9685 ========================
#define I2C_SDA 15             // I2C Data pin (SDA)
#define I2C_SCL 14             // I2C Clock pin (SCL)
#define PCA9685_ADDR 0x40      // Hardware I2C address of the PCA9685 module

#define PCA9685_MODE1     0x00 // Main configuration register address (Mode 1)
#define PCA9685_PRESCALE  0xFE // Register address for PWM prescaler
#define LED0_ON_L         0x06 // Start address for channel 0. Each channel occupies 4 consecutive registers.

#define SERVO_MIN   102        // Number of "ticks" for 0 degrees (approx 500us pulse width) out of 4096 ticks per cycle
#define SERVO_MAX   491        // Number of "ticks" for 180 degrees (approx 2400us pulse width)

int currentPan = 110;
int currentTilt = 10; 

void pca9685WriteReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(PCA9685_ADDR); // Start I2C communication targeting the chip's hardware address (0x40)
  Wire.write(reg);                      // Send the internal register address to modify
  Wire.write(value);                    // Send the value (byte) to be written to that register
  Wire.endTransmission();               // Emit STOP signal on I2C and release the data bus
}

uint8_t pca9685ReadReg(uint8_t reg) {
  Wire.beginTransmission(PCA9685_ADDR); // Start I2C communication to set the register pointer
  Wire.write(reg);                      // Write the register address to read
  Wire.endTransmission();               // End the transmission for address setting
  Wire.requestFrom(PCA9685_ADDR, (uint8_t)1); // Take control of the data line and request 1 byte from the chip
  return Wire.read();                   // Read the received byte via I2C and return it
}

bool pca9685Init() {
  pca9685WriteReg(PCA9685_MODE1, 0x00); // Write 0x00 to MODE1 to disable special functions and ensure active state (wake up)
  delay(5);                             // Wait 5ms for the PCA9685's internal oscillator to stabilize at startup
  uint8_t mode1 = pca9685ReadReg(PCA9685_MODE1); // Read back MODE1 to test chip presence
  if (mode1 == 0xFF) return false;      // 0xFF (255) means SDA/SCL wires are floating (pulled HIGH), chip not responding

  uint8_t prescale = 121;               // Value calculated to force oscillator to 50Hz (25MHz / (4096 * 50) - 1)
  uint8_t oldMode = pca9685ReadReg(PCA9685_MODE1); // Save existing MODE1 config
  pca9685WriteReg(PCA9685_MODE1, (oldMode & 0x7F) | 0x10); // Force chip into SLEEP mode (bit 4 HIGH), required for frequency change
  pca9685WriteReg(PCA9685_PRESCALE, prescale);             // Write 121 to PRESCALE register
  pca9685WriteReg(PCA9685_MODE1, oldMode);                 // Exit SLEEP by writing bit 4 back to 0
  delay(5);                             // Wait another 5ms for oscillator restart at 50Hz
  pca9685WriteReg(PCA9685_MODE1, oldMode | 0xA0);          // Set Auto-Increment (bit 5) for speed and trigger Restart (bit 7)
  return true;                          // Initialization successful
}

void servoWrite(uint8_t canal, uint16_t ticks) {
  uint8_t reg = LED0_ON_L + (canal * 4); // Calculate ON_L register address for requested channel (multiply by 4, each channel has 4 registers)
  Wire.beginTransmission(PCA9685_ADDR);  // Open I2C write session
  Wire.write(reg);                       // Send start address. With Auto-Increment, next 4 bytes flow directly into adjacent registers.
  Wire.write(0);                         // Write ON_L (low byte of START): Signal starts at 0 in the 20ms cycle
  Wire.write(0);                         // Write ON_H (high byte of START): Remains 0.
  Wire.write(ticks & 0xFF);              // Write OFF_L (low byte of STOP): PWM signal cut at value masked for first 8 bits
  Wire.write(ticks >> 8);                // Write OFF_H (high byte of STOP): Shift to extract remaining bits of 'ticks'
  Wire.endTransmission();                // Close communication, changing the actual output on PCA9685 hardware pins
}

uint16_t degreesToTicks(int grade) {
  // Linear mapping: input range 0-180 degrees to hardware output range 102-491 ticks
  return SERVO_MIN + (uint16_t)((float)grade / 180.0 * (SERVO_MAX - SERVO_MIN));
}

// ======================== INTERRUPTS ==========================
// Function executed on interrupt, kept in RAM (IRAM_ATTR) for speed
void IRAM_ATTR pirInterrupt(void* arg) {
  // Read the state of all GPIO interrupts using the global structure
  if (GPIO.status & (1 << PIR_PIN)) {
    motionDetected = true;
    
    // Write 1 to the clear register at bit 13 to reset the interrupt flag
    GPIO.status_w1tc = (1 << PIR_PIN);
  }
}

// ======================= FUNCTIONS ===========================

/**
 * Initializes the OV2640 camera in RGB565 mode.
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

  config.xclk_freq_hz = 10000000;          // 10MHz
  config.pixel_format = PIXFORMAT_RGB565;  
  config.frame_size   = FRAMESIZE_QVGA;    // 320x240
  config.jpeg_quality = 12;
  config.fb_count     = 1;                 // Single buffer (save RAM)

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Error initialising camera: 0x%x\n", err);
    return false;
  }

  // Adjust sensor for streaming
  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    s->set_brightness(s, 1);    // Slightly increased brightness
    s->set_saturation(s, 0);    // Normal saturation
  }

  Serial.println("Camera initialized successfully!");
  return true;
}

/**
 * Connect to WiFi with retry.
 */
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
  WiFi.mode(WIFI_STA);  // station mode (acts like a phone, not a router = access point)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("\nWiFi FAILED! Will retry...");
  }
}

/**
 * Sends a notification to the backend when the PIR sensor detects motion.
 * Returns true if the backend confirmed receipt.
 */
bool notifyMotion() {
  HTTPClient http;
  http.begin(motionUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(HTTP_TIMEOUT_MS);

  int httpCode = http.POST("{\"detected\": true, \"source\": \"PIR_ESP32\"}");
  bool success = false;

  if (httpCode == 200) {
    Serial.println("Backend notified successfully.");
    success = true;
  } else if (httpCode > 0) {
    Serial.printf("Backend responded with error: %d\n", httpCode);
  } else {
    Serial.printf("Connection error: %s\n", http.errorToString(httpCode).c_str());
  }
  
  http.end();
  return success;
}

/**
 * Captures an RGB565 frame, converts it to JPEG with fmt2jpg(),
 * and sends it via HTTP POST to the backend.
 * Returns true if the frame was sent successfully.
 */
bool captureAndSendFrame() {
  // Capture frame (raw RGB565)
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Error: could not capture frame!");
    return false;
  }

  // Convert RGB565 to JPEG
  uint8_t *jpeg_buf = NULL;
  size_t jpeg_len = 0;

  bool converted = fmt2jpg(
    fb->buf, fb->len,  // the pixels and the length in bytes
    fb->width, fb->height,  // the dimensions of the image
    PIXFORMAT_RGB565,  // the format of the pixels
    JPEG_QUALITY,  // the quality of the JPEG
    &jpeg_buf, &jpeg_len  // the JPEG buffer and the length in bytes
  );

  // Release the camera buffer as soon as we have the JPEG
  esp_camera_fb_return(fb);

  if (!converted || jpeg_buf == NULL) {
    Serial.println("JPEG compression error!");
    if (jpeg_buf) free(jpeg_buf);
    return false;
  }

  // Send JPEG via HTTP POST
  HTTPClient http;
  http.begin(backendUrl);
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(HTTP_TIMEOUT_MS);

  int httpCode = http.POST(jpeg_buf, jpeg_len);

  // Free the JPEG memory allocated by fmt2jpg
  free(jpeg_buf);

  // Check response
  if (httpCode == 200) {
    return true;
  } else {
    if (httpCode > 0) {
      Serial.printf("HTTP POST failed: code %d\n", httpCode);
    } else {
      Serial.printf("HTTP POST connection error: %s\n",
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

  // --- PIR sensor configuration ---
  
  // Set base GPIO function (not used for SD card), enable input and pull-down in 
  // the multiplexer for pin 13
  // For IO_MUX there is no GPIO structure, so I use direct pointer dereferencing:
  *((volatile uint32_t *)IO_MUX_GPIO13_REG) = (2 << MCU_SEL_S) | FUN_IE | FUN_PD;
  
  // Disable output for pin 13 by writing 1 at the corresponding position
  GPIO.enable_w1tc = (1 << PIR_PIN);
  
  // Read current pin 13 configuration into a temporary variable
  uint32_t pin_config = GPIO.pin[PIR_PIN].val;
  
  // Clear bits controlling interrupt type
  pin_config &= ~GPIO_PIN_INT_TYPE_M;
  
  // Insert value for rising edge trigger shifted to the correct register position
  pin_config |= (GPIO_INTR_POSEDGE << GPIO_PIN_INT_TYPE_S);
  
  // Write modified configuration back to pin 13 register
  GPIO.pin[PIR_PIN].val = pin_config;
  
  // Initialize global GPIO interrupt service
  gpio_install_isr_service(0);
  
  // Attach previously created handler function to pin 13 events
  gpio_isr_handler_add((gpio_num_t)PIR_PIN, pirInterrupt, NULL);

  // --- Buzzer configuration ---
  // Set IO MUX for pin 4: GPIO function (MCU_SEL = 0), enable pull-down (FUN_PD),
  // and set drive capability to value 3 (bit 10).
  *((volatile uint32_t *)IO_MUX_GPIO4_REG) = (0 << MCU_SEL_S) | (3 << 10) | FUN_PD;

  // Enable output for pin 4
  GPIO.enable_w1ts = (1 << BUZZER_PIN);

  // Buzzer OFF (LOW state)
  GPIO.out_w1tc = (1 << BUZZER_PIN);

  // Build backend URLs
  backendUrl = String("http://") + BACKEND_HOST + ":" +
               String(BACKEND_PORT) + FRAME_ENDPOINT;
  motionUrl = String("http://") + BACKEND_HOST + ":" +
              String(BACKEND_PORT) + MOTION_ENDPOINT;
  autotrackingStatusUrl = String("http://") + BACKEND_HOST + ":" +
                          String(BACKEND_PORT) + AUTOTRACKING_STATUS_ENDPOINT;

  // Initialize camera
  if (!initCamera()) {
    Serial.println("Camera is still off! Restart in 5s...");
    delay(5000);
    ESP.restart();
  }

  // Initialize PCA9685
  Wire.begin(I2C_SDA, I2C_SCL);
  if (pca9685Init()) {
    Serial.println("PCA9685 initializat cu succes.");
    // Set initial positions of servos
    servoWrite(0, degreesToTicks(currentPan));
    servoWrite(1, degreesToTicks(currentTilt));
  } else {
    Serial.println("Error initializing PCA9685!");
  }

  if (!lox.begin(0x29, false, &Wire)) {
    Serial.println(F("Error initializing VL53L0X!"));
    loxInitialized = false;
  } else {
    Serial.println(F("VL53L0X initialized successfully."));
    loxInitialized = true;
  }

  connectWiFi();

  // Configure HTTP Server on ESP32
  server.on("/buzzer", HTTP_POST, []() {
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    Serial.println("Buzzer command received from backend!");
    
    if (buzzerState == 0) {
      buzzerState = 1;
      buzzerStartTime = millis();
      GPIO.out_w1ts = (1 << BUZZER_PIN); // Set HIGH
    }
  });

  server.on("/move", HTTP_POST, []() {
    if (server.hasArg("plain") == false) {
      server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"No body\"}");
      return;
    }
    String body = server.arg("plain");
    Serial.println("Move command received: " + body);
    
    int step = 10;
    if (body.indexOf("up") != -1) {
      currentTilt -= step; 
    } else if (body.indexOf("down") != -1) {
      currentTilt += step;
    } else if (body.indexOf("left") != -1) {
      currentPan += step; 
    } else if (body.indexOf("right") != -1) {
      currentPan -= step;
    }
    Serial.printf("currentPan: %d, currentTilt: %d\n", currentPan, currentTilt);

    // Make sure we stay within physical limits (0-180 degrees)
    if (currentPan < 0) currentPan = 0;
    if (currentPan > 180) currentPan = 180;
    if (currentTilt < 0) currentTilt = 0;
    if (currentTilt > 180) currentTilt = 180;

    servoWrite(0, degreesToTicks(currentPan));
    servoWrite(1, degreesToTicks(currentTilt));
    
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/autotracking", HTTP_POST, []() {
    if (server.hasArg("plain") == false) {  // check if there is a body
      server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"No body\"}");
      return;
    }
    String body = server.arg("plain");  // get body
    
    if (body.indexOf("true") != -1) {  // if body contains "true"
      Serial.println("Autotracking is ON");
      autotrackingEnabled = true;
      isScanning = true;
      scanPan = 0;
      scanTilt = 0;
      scanPanDir = 10;
      minDistance = 8190;
      bestPan = 110;
      bestTilt = 10;
      currentPan = scanPan;
      currentTilt = scanTilt;
      servoWrite(0, degreesToTicks(currentPan));
      servoWrite(1, degreesToTicks(currentTilt));
      lastScanMoveTime = millis();
    } else {
      Serial.println("Autotracking is OFF");
      autotrackingEnabled = false;
      isScanning = false;
    }
    
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/notifications", HTTP_POST, []() {
    if (server.hasArg("plain") == false) {
      server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"No body\"}");
      return;
    }
    String body = server.arg("plain");
    
    if (body.indexOf("true") != -1) {
      Serial.println("PIR Interrupts ENABLED");
      gpio_intr_enable((gpio_num_t)PIR_PIN);
    } else {
      Serial.println("PIR Interrupts DISABLED");
      gpio_intr_disable((gpio_num_t)PIR_PIN);  // disable interrupts
    }
    
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.begin();
  Serial.println("HTTP Server started on port 80!");

  fpsTimer = millis();
  Serial.println("Setup completed, start streaming!\n");
}

// ========================= LOOP ============================
void loop() {
  // Process HTTP requests to ESP32 
  server.handleClient();

  // --- Buzzer processing (Non-Blocking) ---
  if (buzzerState > 0) {
    unsigned long currentMillis = millis();
    if (buzzerState == 1 && currentMillis - buzzerStartTime >= 30) {
      GPIO.out_w1tc = (1 << BUZZER_PIN); // Set LOW
      buzzerState = 2;
      buzzerStartTime = currentMillis;
    } else if (buzzerState == 2 && currentMillis - buzzerStartTime >= 10) {
      GPIO.out_w1ts = (1 << BUZZER_PIN); // Set HIGH
      buzzerState = 3;
      buzzerStartTime = currentMillis;
    } else if (buzzerState == 3 && currentMillis - buzzerStartTime >= 30) {
      GPIO.out_w1tc = (1 << BUZZER_PIN); // Set LOW
      buzzerState = 0; // Done
    }
  }

  // --- Autotracking processing ---
  if (autotrackingEnabled) {
    unsigned long currentMillis = millis();
    
    if (!isScanning) {
      if (currentMillis - lastScanCompleteTime >= 5000) { // 5s pause between scans
        isScanning = true;
        scanPan = 0;
        scanTilt = 0;
        scanPanDir = 10;
        minDistance = 8190;
        bestPan = 110;
        bestTilt = 10;
        currentPan = scanPan;
        currentTilt = scanTilt;
        servoWrite(0, degreesToTicks(currentPan));
        servoWrite(1, degreesToTicks(currentTilt));
        lastScanMoveTime = currentMillis;
      }
    } else {
      if (currentMillis - lastScanMoveTime >= 150) { // 150ms per step for stable reading and mechanical movement
        lastScanMoveTime = currentMillis;
        
        // Sensor reading
        if (loxInitialized) {
          VL53L0X_RangingMeasurementData_t measure;
          lox.rangingTest(&measure, false);
          if (measure.RangeStatus != 4 && measure.RangeMilliMeter > 0) {
            if (measure.RangeMilliMeter < minDistance) {
              minDistance = measure.RangeMilliMeter;
              bestPan = currentPan;
              bestTilt = currentTilt;
            }
          }
        }
        
        // Motor movement
        scanPan += scanPanDir;
        if (scanPan > 180 || scanPan < 0) {
          scanPanDir = -scanPanDir;
          scanPan += scanPanDir; // Correction to stay within 0-180
          scanTilt += scanTiltStep;
          
          if (scanTilt > 30) {
            // End of scan
            isScanning = false;
            autotrackingEnabled = false; // Completely stop autotracking
            lastScanCompleteTime = currentMillis;
            
            if (minDistance == 8190) {
              currentPan = 110;
              currentTilt = 10;
            } else {
              currentPan = bestPan - 10;
              currentTilt = bestTilt;
            }
            servoWrite(0, degreesToTicks(currentPan));
            servoWrite(1, degreesToTicks(currentTilt));
            Serial.printf("Finished scanning. MinDist: %d, Pan: %d, Tilt: %d\n", minDistance, currentPan, currentTilt);
            
            // Notify backend
            HTTPClient http;
            http.begin(autotrackingStatusUrl);
            http.addHeader("Content-Type", "application/json");
            http.setTimeout(1000);
            http.POST("{\"enabled\": false}");
            http.end();
          } else {
            currentTilt = scanTilt;
            servoWrite(1, degreesToTicks(currentTilt));
          }
        } else {
          currentPan = scanPan;
          servoWrite(0, degreesToTicks(currentPan));
        }
      }
    }
  }

  // --- PIR Sensor processing ---
  static unsigned long lastMotionDetectionTime = 0;
  if (motionDetected) {
    unsigned long currentMillis = millis();
    // Check cooldown period (3s)
    if (currentMillis - lastMotionDetectionTime >= 3000) {
      Serial.println("Motion detected! Notify backend...");
      notifyMotion();
      lastMotionDetectionTime = currentMillis;
    }
    // reset flag and waut for the next interrupt
    motionDetected = false;
  }

  // Reconectare WiFi daca s-a deconectat
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, retry...");
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      delay(2000);
      return;
    }
  }

  unsigned long now = millis();
  if (now - lastFrameTime < CAPTURE_INTERVAL_MS) {
    delay(1); // short pause
    return;
  }
  lastFrameTime = now;

  // Take a picture and send it
  if (captureAndSendFrame()) {
    frameCount++;
  }

  // Print FPS every 5 seconds
  if (now - fpsTimer >= 5000) {
    float fps = (float)frameCount / ((now - fpsTimer) / 1000.0);
    Serial.printf("FPS: %.1f\n", fps);
    frameCount = 0;
    fpsTimer = now;
  }
}
