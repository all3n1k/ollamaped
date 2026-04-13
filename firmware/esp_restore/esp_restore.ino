// Simple ESP8266 WiFi bridge - restores ESP to working state
// Connects to WiFi and TCP server, bridges Serial <-> TCP

#include <ESP8266WiFi.h>

// Credentials live in `secrets.h` (gitignored — copy secrets.example.h).
#include "secrets.h"

void setup() {
  Serial.begin(115200);  // Hardware Serial
  delay(10);
  
  Serial.println("");
  Serial.println("Starting...");
  
  // Connect to WiFi
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println("");
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed!");
  }
}

void loop() {
  // Try to connect to brain
  WiFiClient client;
  if (!client.connect(host, port)) {
    Serial.println("Connecting to brain...");
    delay(2000);
    return;
  }
  
  Serial.println("Connected to brain!");
  
  // Bridge Serial <-> TCP continuously
  while (client.connected()) {
    // Serial to TCP
    while (Serial.available()) {
      char c = Serial.read();
      client.print(c);
    }
    
    // TCP to Serial
    while (client.available()) {
      char c = client.read();
      Serial.print(c);
    }
    
    delay(1);
  }
  
  Serial.println("Disconnected");
  client.stop();
  delay(1000);
}