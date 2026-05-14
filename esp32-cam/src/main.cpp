#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_VL53L0X.h>
#include "esp_camera.h"
#include "img_converters.h"  // fmt2jpg() - conversie RGB565 -> JPEG
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include <WebServer.h>

// ======================= CONFIGURARE =======================
// WiFi
#define WIFI_SSID     "Alexandra"
#define WIFI_PASSWORD "Alex2702"

// Backend server URL (IP-ul laptopului pe reteaua WiFi)
#define BACKEND_HOST "10.213.151.82"
#define BACKEND_PORT 5000
#define FRAME_ENDPOINT "/api/camera/frame"
#define MOTION_ENDPOINT "/api/motion"
#define AUTOTRACKING_STATUS_ENDPOINT "/api/settings/autotracking/sync"

// Interval intre cadre (ms)
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
String motionUrl;
String autotrackingStatusUrl;
unsigned long lastFrameTime = 0;
unsigned long frameCount = 0;
unsigned long fpsTimer = 0;

// ======================== SENZORI & BUZZER ==========================
#define PIR_PIN 13
#define BUZZER_PIN 4

volatile bool motionDetected = false;

int buzzerState = 0;
unsigned long buzzerStartTime = 0;

// ======================== WEB SERVER =======================
WebServer server(80);

// ======================== AUTOTRACKING & SENZOR DISTANTA ========================
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

// ======================== I2C & PCA9685 ========================
#define I2C_SDA 15             // Pinul de Date I2C (Serial Data) al ESP32
#define I2C_SCL 14             // Pinul de Ceas I2C (Serial Clock) al ESP32
#define PCA9685_ADDR 0x40      // Adresa I2C hardware implicita a modulului PCA9685

#define PCA9685_MODE1     0x00 // Adresa registrului principal de configurare (Mode 1)
#define PCA9685_PRESCALE  0xFE // Adresa registrului pentru prescaler PWM
#define LED0_ON_L         0x06 // Adresa de start pentru canalul 0. Fiecare canal ocupa cate 4 registri succesivi.

#define SERVO_MIN   102        // Numarul de pasi "ticks" pentru 0 grade (aprox 500us latime puls) din cei 4096 pasi ai ciclului
#define SERVO_MAX   491        // Numarul de "ticks" pentru 180 grade (aprox 2400us latime puls)

int currentPan = 110;
int currentTilt = 10; 


void pca9685WriteReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(PCA9685_ADDR); // Initiaza comunicarea I2C tintind adresa hardware a cipului (0x40)
  Wire.write(reg);                      // Trimite pe bus adresa registrului intern pe care vrem sa il modificam
  Wire.write(value);                    // Trimite valoarea (octetul) ce va fi scrisa efectiv in acel registru
  Wire.endTransmission();               // Emite semnalul de STOP pe I2C si elibereaza magistrala de date
}

uint8_t pca9685ReadReg(uint8_t reg) {
  Wire.beginTransmission(PCA9685_ADDR); // Initiaza comunicarea I2C pentru a seta pointerul de registru
  Wire.write(reg);                      // Scrie adresa registrului pe care vrem sa il citim
  Wire.endTransmission();               // Incheie transmisia preliminara de setare a adresei
  Wire.requestFrom(PCA9685_ADDR, (uint8_t)1); // Preia controlul liniei de date si cere cipului 1 byte inapoi
  return Wire.read();                   // Citeste acel byte primit prin I2C si il returneaza
}

bool pca9685Init() {
  pca9685WriteReg(PCA9685_MODE1, 0x00); // Scrie 0x00 in MODE1 pentru a opri functiile speciale si a asigura starea activa (trezit)
  delay(5);                             // Asteapta 5ms pentru ca oscilatorul intern al PCA9685 sa se stabilizeze la pornire
  uint8_t mode1 = pca9685ReadReg(PCA9685_MODE1); // Citeste inapoi registrul MODE1 pentru a testa prezenta cipului
  if (mode1 == 0xFF) return false;      // 0xFF (255) semnifica ca firele SDA/SCL sunt libere (trase HIGH), cipul nu raspunde

  uint8_t prescale = 121;               // Valoarea calculata pentru a forta oscilatorul la frecventa de 50Hz (25MHz / (4096 * 50) - 1)
  uint8_t oldMode = pca9685ReadReg(PCA9685_MODE1); // Salveaza configuratia existenta a registrului MODE1
  pca9685WriteReg(PCA9685_MODE1, (oldMode & 0x7F) | 0x10); // Forteaza cipul in mod SLEEP (bit 4 HIGH), o conditie fizica impusa pt. schimbarea frecventei
  pca9685WriteReg(PCA9685_PRESCALE, prescale);             // Scrie divizorul de 121 in registrul hardware PRESCALE
  pca9685WriteReg(PCA9685_MODE1, oldMode);                 // Iese din modul SLEEP prin rescrierea bitului 4 inapoi in 0
  delay(5);                             // Asteapta din nou 5ms pentru restartarea corecta a oscilatorului intern la 50Hz
  pca9685WriteReg(PCA9685_MODE1, oldMode | 0xA0);          // Seteaza Auto-Increment (bit 5) pt viteza si declanseaza Restart (bit 7)
  return true;                          // Initializarea s-a terminat cu succes
}

void servoWrite(uint8_t canal, uint16_t ticks) {
  uint8_t reg = LED0_ON_L + (canal * 4); // Calculeaza adresa registrului ON_L pt canalul cerut (se inmulteste cu 4 pt ca fiecare canal are 4 registri de cate 8 biti)
  Wire.beginTransmission(PCA9685_ADDR);  // Deschide sesiunea de scriere I2C
  Wire.write(reg);                       // Trimite adresa de pornire. Pt ca am activat Auto-Increment, urmatorii 4 bytes curg direct in pachetele adiacente.
  Wire.write(0);                         // Scrie ON_L (low byte de START): Semnalul porneste la momentul 0 din ciclul de 20ms
  Wire.write(0);                         // Scrie ON_H (high byte de START): Ramane 0.
  Wire.write(ticks & 0xFF);              // Scrie OFF_L (low byte de STOP): Taie semnalul PWM la valoarea extrasa prin aplicarea mastii pentru primii 8 biti
  Wire.write(ticks >> 8);                // Scrie OFF_H (high byte de STOP): Shiftare pt a extrage bitii ramasi ai valorii 'ticks' care guverneaza stingerea semnalului
  Wire.endTransmission();                // Inchide comunicarea, modificand curentul efectiv scos pe pinii hardware PCA9685
}

uint16_t gradeLaTicks(int grade) {
  // Executa maparea liniara a valorilor: din plaja de intrari 0-180 grade in plaja hardware de iesiri 102-491 ticks
  return SERVO_MIN + (uint16_t)((float)grade / 180.0 * (SERVO_MAX - SERVO_MIN));
}

// ======================== INTRERUPERI ==========================
// Functia executata la intrerupere, tinuta in RAM (IRAM_ATTR) pentru viteza
void IRAM_ATTR pirInterrupt(void* arg) {
  // Citim starea tuturor intreruperilor GPIO folosind structura globala
  if (GPIO.status & (1 << PIR_PIN)) {
    motionDetected = true;
    
    // Scriem 1 in registrul de clear la pozitia bitului 13 pentru a reseta flag-ul intreruperii
    GPIO.status_w1tc = (1 << PIR_PIN);
  }
}

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
 * Trimite o notificare catre backend cand senzorul PIR detecteaza miscare.
 * Returneaza true daca backend-ul a confirmat primirea.
 */
bool notifyMotion() {
  HTTPClient http;
  http.begin(motionUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(HTTP_TIMEOUT_MS);

  int httpCode = http.POST("{\"detected\": true, \"source\": \"PIR_ESP32\"}");
  bool success = false;

  if (httpCode == 200) {
    Serial.println("Backend notificat cu succes.");
    success = true;
  } else if (httpCode > 0) {
    Serial.printf("Backend a raspuns cu eroare: %d\n", httpCode);
  } else {
    Serial.printf("Eroare la conectare: %s\n", http.errorToString(httpCode).c_str());
  }
  
  http.end();
  return success;
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

  // --- Configurare senzor PIR ---
  
  // Setam functia de baza GPIO, activam functionalitatea de input si pull-down in multiplexorul pinului 13
  // Pentru IO_MUX nu exista o structura similara, asa ca procedam pur cu pointeri dereferentiati:
  *((volatile uint32_t *)IO_MUX_GPIO13_REG) = (2 << MCU_SEL_S) | FUN_IE | FUN_PD;
  
  // Oprim driver-ul de output pentru pinul 13 scriind 1 la pozitia aferenta
  GPIO.enable_w1tc = (1 << PIR_PIN);
  
  // Citim starea curenta a configuratiei pinului 13 intr-o variabila temporara
  uint32_t pin_config = GPIO.pin[PIR_PIN].val;
  
  // Stergem bitii care controleaza tipul de intrerupere aplicand o masca logica AND NOT
  pin_config &= ~GPIO_PIN_INT_TYPE_M;
  
  // Inseram valoarea pentru declansare pe front crescator shiftata pe pozitia corecta din registru
  pin_config |= (GPIO_INTR_POSEDGE << GPIO_PIN_INT_TYPE_S);
  
  // Scriem configuratia modificata inapoi in registrul pinului 13
  GPIO.pin[PIR_PIN].val = pin_config;
  
  // Initializam serviciul global de gestionare a intreruperilor pe GPIO
  gpio_install_isr_service(0);
  
  // Atasam functia handler creata anterior la evenimentele pinului 13
  gpio_isr_handler_add((gpio_num_t)PIR_PIN, pirInterrupt, NULL);

  // --- Configurare Buzzer (VCC control prin GPIO4) ---
  // Setam IO MUX pentru pinul 4: functia GPIO (MCU_SEL = 0), activam pull-down (FUN_PD),
  // si setam drive capability la valoarea 3 (pozitionat pe bitul 10).
  *((volatile uint32_t *)IO_MUX_GPIO4_REG) = (0 << MCU_SEL_S) | (3 << 10) | FUN_PD;

  // Activam driver-ul de output pentru pinul 4
  GPIO.enable_w1ts = (1 << BUZZER_PIN);

  // Buzzer OPRIT (stare LOW) - Scriem 1 in registrul W1TC
  GPIO.out_w1tc = (1 << BUZZER_PIN);

  // Construim URL-ul backend
  backendUrl = String("http://") + BACKEND_HOST + ":" +
               String(BACKEND_PORT) + FRAME_ENDPOINT;
  motionUrl = String("http://") + BACKEND_HOST + ":" +
              String(BACKEND_PORT) + MOTION_ENDPOINT;
  autotrackingStatusUrl = String("http://") + BACKEND_HOST + ":" +
                          String(BACKEND_PORT) + AUTOTRACKING_STATUS_ENDPOINT;
  Serial.printf("Backend URL: %s\n", backendUrl.c_str());
  Serial.printf("Motion URL: %s\n", motionUrl.c_str());

  // Initializeaza camera
  if (!initCamera()) {
    Serial.println("Camera nu a pornit! Restart in 5s...");
    delay(5000);
    ESP.restart();
  }

  // Initializare PCA9685
  Wire.begin(I2C_SDA, I2C_SCL);
  if (pca9685Init()) {
    Serial.println("PCA9685 initializat cu succes.");
    // Seteaza pozitiile initiale
    servoWrite(0, gradeLaTicks(currentPan));
    servoWrite(1, gradeLaTicks(currentTilt));
  } else {
    Serial.println("Eroare la initializare PCA9685!");
  }

  if (!lox.begin(0x29, false, &Wire)) {
    Serial.println(F("Eroare la initializare VL53L0X!"));
    loxInitialized = false;
  } else {
    Serial.println(F("VL53L0X initializat cu succes."));
    loxInitialized = true;
  }

  // Conectare WiFi
  connectWiFi();

  // Configurare Server HTTP pe ESP32
  server.on("/buzzer", HTTP_POST, []() {
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    Serial.println("Comanda buzzer primita de la backend!");
    
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
    Serial.println("Comanda move primita: " + body);
    
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

    // Asigura-te ca ramanem in limitele fizice (0-180 grade)
    if (currentPan < 0) currentPan = 0;
    if (currentPan > 180) currentPan = 180;
    if (currentTilt < 0) currentTilt = 0;
    if (currentTilt > 180) currentTilt = 180;

    servoWrite(0, gradeLaTicks(currentPan));
    servoWrite(1, gradeLaTicks(currentTilt));
    
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/autotracking", HTTP_POST, []() {
    if (server.hasArg("plain") == false) {
      server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"No body\"}");
      return;
    }
    String body = server.arg("plain");
    
    if (body.indexOf("true") != -1) {
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
      servoWrite(0, gradeLaTicks(currentPan));
      servoWrite(1, gradeLaTicks(currentTilt));
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
      gpio_intr_disable((gpio_num_t)PIR_PIN);
    }
    
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.begin();
  Serial.println("HTTP Server pornit pe portul 80!");

  fpsTimer = millis();
  Serial.println("Setup complet - incep streaming-ul!\n");
}

// ========================= LOOP ============================
void loop() {
  // Procesare cereri HTTP catre ESP32 (ex: /buzzer)
  server.handleClient();

  // --- Procesare Buzzer (Non-Blocking) ---
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
      buzzerState = 0; // Finalizat
    }
  }

  // --- Procesare Autotracking ---
  if (autotrackingEnabled) {
    unsigned long currentMillis = millis();
    
    if (!isScanning) {
      if (currentMillis - lastScanCompleteTime >= 5000) { // Pauza de 5s intre scanari
        isScanning = true;
        scanPan = 0;
        scanTilt = 0;
        scanPanDir = 10;
        minDistance = 8190;
        bestPan = 110;
        bestTilt = 10;
        currentPan = scanPan;
        currentTilt = scanTilt;
        servoWrite(0, gradeLaTicks(currentPan));
        servoWrite(1, gradeLaTicks(currentTilt));
        lastScanMoveTime = currentMillis;
      }
    } else {
      if (currentMillis - lastScanMoveTime >= 150) { // 150ms per pas pt citire stabila si miscare mecanica
        lastScanMoveTime = currentMillis;
        
        // Citire senzor
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
        
        // Miscare motoare
        scanPan += scanPanDir;
        if (scanPan > 180 || scanPan < 0) {
          scanPanDir = -scanPanDir;
          scanPan += scanPanDir; // Corectie pentru a ramane in 0-180
          scanTilt += scanTiltStep;
          
          if (scanTilt > 40) {
            // Sfarsit scanare
            isScanning = false;
            autotrackingEnabled = false; // Opreste autotracking-ul complet
            lastScanCompleteTime = currentMillis;
            
            if (minDistance == 8190) {
              currentPan = 110;
              currentTilt = 10;
            } else {
              currentPan = bestPan;
              currentTilt = bestTilt;
            }
            servoWrite(0, gradeLaTicks(currentPan));
            servoWrite(1, gradeLaTicks(currentTilt));
            Serial.printf("Sfarsit scanare. MinDist: %d, Pan: %d, Tilt: %d\n", minDistance, currentPan, currentTilt);
            
            // Anunta backend-ul
            HTTPClient http;
            http.begin(autotrackingStatusUrl);
            http.addHeader("Content-Type", "application/json");
            http.setTimeout(1000);
            http.POST("{\"enabled\": false}");
            http.end();
          } else {
            currentTilt = scanTilt;
            servoWrite(1, gradeLaTicks(currentTilt));
          }
        } else {
          currentPan = scanPan;
          servoWrite(0, gradeLaTicks(currentPan));
        }
      }
    }
  }

  // --- Procesare Senzor PIR ---
  static unsigned long lastMotionDetectionTime = 0;
  if (motionDetected) {
    unsigned long currentMillis = millis();
    // Verificam perioada de cooldown de 3 secunde (3000 ms)
    if (currentMillis - lastMotionDetectionTime >= 3000) {
      Serial.println("S-a detectat miscare! Trimit notificare catre backend...");
      notifyMotion();
      lastMotionDetectionTime = currentMillis;
    }
    // Reseteaza mereu flag-ul pentru a astepta urmatoarea intrerupere
    motionDetected = false;
  }

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
    Serial.printf("FPS: %.1f\n", fps);
    frameCount = 0;
    fpsTimer = now;
  }
}
