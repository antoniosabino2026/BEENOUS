#include <LittleFS.h>
#include <WiFi.h>

const char ssid* = "";
const char pass* = "";








void setup() {
  Serial.begin(112500);
  Serial.println("==========================================");
  Serial.println("============BEENOUS INICIANDO=============");
  Serial.println("==========================================");
  WiFi.begin(ssid, pass);


}

void loop() {
  // put your main code here, to run repeatedly:

}
  