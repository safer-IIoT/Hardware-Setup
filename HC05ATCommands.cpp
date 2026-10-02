#include <SoftwareSerial.h>
SoftwareSerial hc05(4, 3);   // HC-05 TXD->pin4, RXD->pin3

void setup() {
  Serial.begin(9600);        // Serial Monitor at 9600 for THIS sketch
  hc05.begin(38400);         // HC-05 AT-mode baud is 38400
  Serial.println("AT mode ready - type commands");
}
void loop() {
  if (hc05.available()) Serial.write(hc05.read());
  if (Serial.available()) hc05.write(Serial.read());
}
/*
1. Remove the VCC connection from HC-05, hold the button on the HC-05, and turn it back on to enter AT mode (slow blink).
2. Open the Arduino Serial Monitor (Both NL & CR) and run these clean reset commands:
	• Type: AT+ORGL (Resets everything to factory default)
	• Type: AT+ROLE=0 (Ensures the HC-05 is set to standard Slave mode)
	• Type: AT+CMODE=1 (Allows it to accept a connection from any master device)
        • Type: AT+PSWD? (Returns PIN)
        • Type: AT+ADDR? (Returns the MAC address)
3. Connect VCC connection from HC-05 to the same row where it was plugged. The HC-05 will blink fast, waiting passively for a master to command it.
*/