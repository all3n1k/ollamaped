// ESP8266 AT Command Bridge for Spider Robot
// Responds to ATmega2560 AT commands on Serial2
// Bridges to brain TCP server
// Serial2: 115200 baud - communication with ATmega
// Serial: 115200 baud - debug output

#include <ESP8266WiFi.h>
#include <WiFiClient.h>

// WiFi configuration (will be overridden by ATmega AT commands)
// Credentials live in `secrets.h` (gitignored — copy secrets.example.h).
#include "secrets.h"

// TCP connection to brain
WiFiClient tcpClient;
uint16_t brain_port = 9000;

bool connected = false;
String responseBuffer = "";

void setup() {
  Serial.begin(115200);    // Debug serial
  Serial.begin(115200);   // ATmega communication
  
  Serial.println("");
  Serial.println("ESP8266 AT Bridge Ready");
  Serial.println("Waiting for AT commands from ATmega...");
}

void loop() {
  // Read AT commands from ATmega (Serial2)
  while (Serial.available()) {
    char c = Serial.read();
    Serial.write(c);  // Echo to debug
    
    if (c == '\n') {
      processATCommand(responseBuffer);
      responseBuffer = "";
    } else if (c != '\r') {
      responseBuffer += c;
    }
  }
  
  // If connected to brain, bridge the connection
  if (tcpClient && tcpClient.connected()) {
    // Serial2 -> TCP
    while (Serial.available()) {
      tcpClient.write(Serial.read());
    }
    
    // TCP -> Serial2
    while (tcpClient.available()) {
      Serial.write(tcpClient.read());
      Serial.write(tcpClient.read());  // Debug
    }
  } else if (connected) {
    // Lost connection, try to reconnect
    Serial.println("\r\nDISCONNECTED");
    Serial.println("Connection lost, reconnecting...");
    connectToBrain();
  }
  
  delay(1);
}

void processATCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  
  if (cmd.startsWith("AT")) {
    if (cmd == "AT" || cmd == "AT+RST") {
      Serial.println("\r\nOK");
    }
    else if (cmd.startsWith("AT+CWMODE")) {
      Serial.println("\r\nOK");
    }
    else if (cmd.startsWith("AT+CWJAP")) {
      // Extract SSID and password from command
      String fullCmd = responseBuffer;
      parseAndConnectWiFi(fullCmd);
    }
    else if (cmd.startsWith("AT+CIPMUX")) {
      Serial.println("\r\nOK");
    }
    else if (cmd.startsWith("AT+CIPSERVER")) {
      // We act as client, not server
      Serial.println("\r\nOK");
    }
    else if (cmd.startsWith("AT+CIPSTART")) {
      parseAndConnectTCP(responseBuffer);
    }
    else if (cmd.startsWith("AT+CIPCLOSE")) {
      tcpClient.stop();
      connected = false;
      Serial.println("\r\nCLOSED");
    }
    else if (cmd.startsWith("AT+CIFSR")) {
      IPAddress ip = WiFi.localIP();
      Serial.printf("\r\n%d.%d.%d.%d\r\n", ip[0], ip[1], ip[2], ip[3]);
    }
    else if (cmd.startsWith("AT+GMR")) {
      Serial.println("\r\n0018000902\r\nOK");
    }
    else {
      Serial.println("\r\nOK");
    }
  }
}

void parseAndConnectWiFi(String cmd) {
  // Parse AT+CWJAP_CUR="SSID","PASSWORD"
  int firstQuote = cmd.indexOf('"');
  int secondQuote = cmd.indexOf('"', firstQuote + 1);
  int thirdQuote = cmd.indexOf('"', secondQuote + 1);
  int fourthQuote = cmd.indexOf('"', thirdQuote + 1);
  
  if (firstQuote >= 0 && fourthQuote >= 0) {
    String ssid = cmd.substring(firstQuote + 1, secondQuote);
    String password = cmd.substring(thirdQuote + 1, fourthQuote);
    
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    
    WiFi.begin(ssid.c_str(), password.c_str());
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    Serial.println("");
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\r\nOK");
      Serial.println("WiFi connected");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("\r\nERROR");
      Serial.println("WiFi failed");
    }
  } else {
    Serial.println("\r\nERROR");
  }
}

void parseAndConnectTCP(String cmd) {
  // Parse AT+CIPSTART="TCP","192.168.1.10",9000
  int firstQuote = cmd.indexOf('"');
  int secondQuote = cmd.indexOf('"', firstQuote + 1);
  int thirdQuote = cmd.indexOf('"', secondQuote + 1);
  int fourthQuote = cmd.indexOf('"', thirdQuote + 1);
  int fifthQuote = cmd.indexOf('"', fourthQuote + 1);
  int sixthQuote = cmd.indexOf('"', fifthQuote + 1);
  
  if (firstQuote >= 0 && sixthQuote >= 0) {
    String type = cmd.substring(firstQuote + 1, secondQuote);
    String host = cmd.substring(thirdQuote + 1, fourthQuote);
    String portStr = cmd.substring(fifthQuote + 1, sixthQuote);
    uint16_t port = portStr.toInt();
    
    Serial.print("Connecting to TCP: ");
    Serial.print(host);
    Serial.print(":");
    Serial.println(port);
    
    if (tcpClient.connect(host.c_str(), port)) {
      connected = true;
      Serial.println("\r\nOK");
      Serial.println("Connected to brain!");
    } else {
      Serial.println("\r\nERROR");
      Serial.println("Connection failed");
    }
  } else {
    Serial.println("\r\nERROR");
  }
}

void connectToBrain() {
  if (tcpClient.connect(brain_host, brain_port)) {
    connected = true;
    Serial.println("\r\nOK");
    Serial.println("Reconnected to brain!");
  } else {
    Serial.println("Reconnect failed");
  }
}
