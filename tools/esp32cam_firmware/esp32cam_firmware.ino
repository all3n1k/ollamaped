/**
 * Spider Robot — ESP32-CAM Firmware
 * ====================================
 * Connects to WiFi, serves JPEG snapshots on /capture,
 * and supports OTA so you never need the CH340G adapter again
 * after this first flash.
 *
 * Brain vision module calls:  GET http://<camera_ip>/capture
 * OTA updates via:            Arduino IDE or arduino-cli on same network
 *
 * Board:     AI Thinker ESP32-CAM  (esp32:esp32:esp32cam)
 * Partition: Minimal SPIFFS (1.9MB APP with OTA)
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>

// ── WiFi credentials ────────────────────────────────────────────────────────
// Credentials live in `secrets.h` (gitignored — copy secrets.example.h).
#include "secrets.h"

// ── OTA hostname (shows up in arduino-cli board list on the network) ────────
const char* OTA_HOSTNAME  = "spider-cam";

// ── AI Thinker ESP32-CAM pin definitions ────────────────────────────────────
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

// ── Onboard flash LED (GPIO 4) ───────────────────────────────────────────────
#define LED_PIN 4

WebServer server(80);

// ── RSSI scan state (async — never blocks camera or stream) ──────────────────
static String  g_rssi_json    = "{\"networks\":[],\"connected_rssi\":0}";
static bool    g_scan_running = false;
static unsigned long g_last_scan_ms = 0;
static const unsigned long RSSI_SCAN_INTERVAL_MS = 60000;  // re-scan every 60s

void rssi_start_scan() {
    WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
    g_scan_running  = true;
    g_last_scan_ms  = millis();
}

void rssi_poll() {
    if (!g_scan_running) {
        // Kick off a new scan if interval elapsed
        if (millis() - g_last_scan_ms >= RSSI_SCAN_INTERVAL_MS) {
            rssi_start_scan();
        }
        return;
    }
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;   // still in progress

    // Scan finished (n >= 0) or failed (n == WIFI_SCAN_FAILED)
    g_scan_running = false;
    if (n > 0) {
        String json = "{\"networks\":[";
        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            // Escape SSID quotes to avoid broken JSON
            String ssid = WiFi.SSID(i);
            ssid.replace("\"", "\\\"");
            json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
        }
        json += "],\"connected_rssi\":" + String(WiFi.RSSI()) + "}";
        g_rssi_json = json;
        WiFi.scanDelete();
        Serial.printf("RSSI scan done: %d networks\n", n);
    }
}


// ── Camera init ──────────────────────────────────────────────────────────────
bool init_camera() {
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
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    // Use PSRAM if available for higher resolution
    if (psramFound()) {
        config.frame_size   = FRAMESIZE_VGA;   // 640x480 — good balance for vision
        config.jpeg_quality = 12;              // 0-63, lower = better quality
        config.fb_count     = 2;
    } else {
        config.frame_size   = FRAMESIZE_CIF;   // 352x288 fallback
        config.jpeg_quality = 15;
        config.fb_count     = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return false;
    }

    // Flip image if camera is mounted upside down on the robot
    sensor_t* s = esp_camera_sensor_get();
    s->set_vflip(s, 1);     // set to 0 if image appears upside down
    s->set_hmirror(s, 0);   // set to 1 if image appears mirrored

    Serial.println("Camera ready");
    return true;
}


// ── HTTP handlers ─────────────────────────────────────────────────────────────

// GET /capture — returns a single JPEG frame
// This is what the brain's vision module calls
void handle_capture() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        server.send(503, "text/plain", "Camera capture failed");
        return;
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
}

// GET / — simple status page
void handle_root() {
    String ip = WiFi.localIP().toString();
    String html = "<html><body style='font-family:monospace;background:#111;color:#0f0;padding:20px'>";
    html += "<h2>Spider-CAM</h2>";
    html += "<p>IP: " + ip + "</p>";
    html += "<p>PSRAM: ";
    html += psramFound() ? "yes" : "no";
    html += "</p>";
    html += "<p><a href='/capture' style='color:#0f0'>Snapshot</a></p>";
    html += "<p><a href='/stream' style='color:#0f0'>Stream</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
}

// GET /stream — MJPEG stream for viewing in browser
void handle_stream() {
    WiFiClient client = server.client();
    String boundary = "frame";
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: multipart/x-mixed-replace; boundary=" + boundary);
    client.println();

    while (client.connected()) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) break;

        client.println("--" + boundary);
        client.println("Content-Type: image/jpeg");
        client.println("Content-Length: " + String(fb->len));
        client.println();
        client.write(fb->buf, fb->len);
        client.println();

        esp_camera_fb_return(fb);
        delay(50);   // ~20 fps max
    }
}

// GET /rssi_scan — returns last completed WiFi RSSI scan as JSON
// Scan runs in background; this never blocks camera or stream.
// Response: {"networks":[{"ssid":"Home","rssi":-55},...], "connected_rssi":-40}
void handle_rssi_scan() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", g_rssi_json);
}

// GET /led?state=on|off — toggle flash LED
void handle_led() {
    String state = server.arg("state");
    if (state == "on") {
        digitalWrite(LED_PIN, HIGH);
        server.send(200, "text/plain", "LED on");
    } else {
        digitalWrite(LED_PIN, LOW);
        server.send(200, "text/plain", "LED off");
    }
}


// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\nSpider-CAM booting...");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Init camera
    if (!init_camera()) {
        // Blink fast = camera error
        while (true) {
            digitalWrite(LED_PIN, HIGH); delay(100);
            digitalWrite(LED_PIN, LOW);  delay(100);
        }
    }

    // Connect to WiFi
    Serial.printf("Connecting to %s ", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    WiFi.setSleep(false);   // disable WiFi sleep for lower latency
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi failed — rebooting in 5s");
        delay(5000);
        ESP.restart();
    }

    Serial.println();
    Serial.printf("Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Snapshot URL: http://%s/capture\n", WiFi.localIP().toString().c_str());

    // Flash LED twice = WiFi connected
    for (int i = 0; i < 2; i++) {
        digitalWrite(LED_PIN, HIGH); delay(200);
        digitalWrite(LED_PIN, LOW);  delay(200);
    }

    // OTA setup — after this first flash you can update wirelessly
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.onStart([]() {
        Serial.println("OTA update starting...");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("OTA done. Rebooting.");
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA error [%u]\n", error);
    });
    ArduinoOTA.begin();
    Serial.printf("OTA ready — hostname: %s\n", OTA_HOSTNAME);

    // Register HTTP routes
    server.on("/",          HTTP_GET, handle_root);
    server.on("/capture",   HTTP_GET, handle_capture);
    server.on("/stream",    HTTP_GET, handle_stream);
    server.on("/led",       HTTP_GET, handle_led);
    server.on("/rssi_scan", HTTP_GET, handle_rssi_scan);
    server.begin();
    Serial.println("HTTP server started");

    // Kick off first RSSI scan in background
    rssi_start_scan();
}


// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    ArduinoOTA.handle();
    server.handleClient();
    rssi_poll();   // check async RSSI scan; re-triggers every 60s automatically

}
