/*

ESP-VFO.  VFO for TR-2200GX receiption from apprx. 44.4333 - 45.1000 MHz  Proof of Concept only.

The desired rxfreq = (Frequency MHz - 10.7 MHz) / 3
Output on CLK0 of Si5351. Using ESP8266 clone to have WiFi access and .91" OLED as extra.

Minimal PoC with control through webserver only. No encoder or knobs.

Sketch will:
- Initialise the Si5351 and generate the appropriate VFO frequency with low output (2MA).
- Create webserver and one page which will display the current frequency, has up/down knobs and a bunch of Presets.
- Change the frequency based on the webserver requests.

- Currently no transmit frequency is generated. Early tests showed some interference and complexity as the 
  frequency generated is frequency MHz / 12  (so the signal will interfere with the receiption) thus the signal needs 
  to be switched on only when the PTT is pressed.
  Also because the TR-2200GX is missing CTCSS and I have other rigs around the need to continue the PoC was not apperent.
  The Busy signal from the TR-2200GX can be used to switch on the TX signal but you need to have a clean(er)
  generated signal to have a good stable signal and FM modulation. So additional filtering around 12 MHz and a robust and
  jitterfree signal needs to be generated.

- To calibration de Si5351 TCXO and thus the VFO a function is available.
  The frequency will be 10 MHz (1000000000) and adjust with the knobs as close as possible.
  Save will store the cal_factor in EEPROM and reboot the VFO to the start frtequency 145.500 MHz.
  Cancel will stop the calibration and return to the last selected VFO frequency.


*/

#include "si5351.h"
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

#define EEPROM_ADDR_CAL  0   // 4 bytes voor int32_t cal_factor

// OLED SSD1306
#define I2C_ADDRESS 0x3c  // initialize with the I2C addr 0x3C Typically eBay OLED's
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// Si5351 parameters
Si5351 si5351;
uint64_t rxfreq = 4493333333ULL;   // 145.500 MHz rx
const uint64_t step = 416666ULL;   // 12.5 kHz step
int32_t cal_factor, cal_factor_start;
bool pendingReboot = false;
unsigned long rebootAt = 0;

// used in calibration routine
uint32_t rx_freq;               // used in calibration routine
const uint32_t calibration_freq = 1000000000ULL;  // 10 MHz, in hundredths of hertz

// WiFi en Webserver
const char* WIFI_SSID     = "YOUR SSID";
const char* WIFI_PASSWORD = "YOUR PASSWORD";
ESP8266WebServer server(80);

// OLED display routine
void showOLED(String line1, String line2 = "", String line3 = "") {
    display.clearDisplay();
    display.setRotation(0);
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);

    String lines[] = {line1, line2, line3};
    int lineCount = 0;
    for (String &l : lines) if (l.length() > 0) lineCount++;

    int lineHeight = 8 * 2;
    int extraSpacing = (lineCount == 2) ? 6 : 2;
    int totalHeight = lineCount * lineHeight + (lineCount - 1) * extraSpacing;
    int yStart = (64 - totalHeight) / 2;

    int16_t x, y; uint16_t w, h;
    for (int i = 0, yPos = yStart; i < lineCount; i++, yPos += lineHeight + extraSpacing) {
        display.getTextBounds(lines[i], 0, 0, &x, &y, &w, &h);
        display.setCursor((128 - w) / 2, yPos);
        display.println(lines[i]);
    }
    display.display();
}

void EEPROM_writeInt(int address, int32_t value) {
  EEPROM.write(address,        (value >> 0)  & 0xFF);
  EEPROM.write(address + 1,    (value >> 8)  & 0xFF);
  EEPROM.write(address + 2,    (value >> 16) & 0xFF);
  EEPROM.write(address + 3,    (value >> 24) & 0xFF);
}

int32_t EEPROM_readInt(int address) {
  int32_t value = 0;
  value |= EEPROM.read(address);
  value |= EEPROM.read(address + 1) << 8;
  value |= EEPROM.read(address + 2) << 16;
  value |= EEPROM.read(address + 3) << 24;
  return value;
}

String safeIOS(String s) {
  if (s.length() > 3) return s.substring(0,3) + "&#8203;" + s.substring(3);
  return s;
}

double getFinalFreqMHz() {
  double f = rxfreq / 100.0;                // freq van SI5351 in Hz
  double finalHz = (f * 3.0) + 10700000.0;  // eindfrequentie in Hz
  return finalHz / 1e6;                     // omzetten naar MHz
}

// OLED routine for frequentie only with bigger size
void showOLEDFreq(double mhz) {
  display.clearDisplay();
  display.setRotation(0);
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);

  // maak van 145.5000 5.5000 als tekst
  mhz = mhz -140;
  String txt = String(mhz, 4); 
  // centreer 
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  int x = (128 - w) / 2;
  int y = (64 - h) / 2;
  display.setCursor(x, y);
  
  display.print(txt);
  display.display();
}

// Webpage for configuration
String htmlRoot() {
  String html =
  "<!DOCTYPE html><html><head>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<meta name='format-detection' content='telephone=no'>"
  "<style>"
  "body{text-align:center;font-family:Arial;background:#fff;}"
  "button{width:140px;height:45px;font-size:16px;"
  "background:#1fa3ec;color:#fff;border:none;border-radius:10px;margin:5px;}"
  "</style></head><body>";

  double cur = getFinalFreqMHz();
  String freqDisplay = safeIOS(String(cur, 4));
  html += "<h1><a href='/' style='text-decoration: none; color: inherit;'>ESP-VFO TR-2200GX</a></h1>"
  "<h2>" + freqDisplay + " MHz</h2>"
  
  // UP / DOWN knobs
  "<button onclick=\"location.href='/down'\">DOWN</button>"
  "<button onclick=\"location.href='/up'\">UP</button>"
  
  // PRESETS
  "<h2>Presets</h2>";

  const char* presets[] = {
    "144.650","144.800","145.4625","145.575",
    "145.650","145.675","145.700","145.725"
  };

  int i = 0;
  for (auto p : presets) {
    html += "<button onclick=\"location='/preset?mhz=" + String(p) + "'\">" +
            safeIOS(String(p)) + "</button>";
    i++;
    if (i % 2 == 0) html += "<br>";
  }
  
  html += 
  "<h2>Setup</h2>"
  "<button onclick=\"location.href='/calibrate'\">Calibrate</button>"
  "</body></html>";
  return html;
}

void handleRoot() {
    server.send(200, "text/html", htmlRoot());
}

// Calibration page
String htmlCalibrate() {
 String html =
  "<!DOCTYPE html><html><head>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<meta name='format-detection' content='telephone=no'>"
  "<style>"
  "body{text-align:center;font-family:Arial;background:#fff;}"
  "button{width:140px;height:45px;font-size:16px;"
  "background:#1fa3ec;color:#fff;border:none;border-radius:10px;margin:5px;}"
  "</style></head><body>"

  "<h1>Si5351 Calibrate</h1>"
  "<p>Target frequency: " + String(calibration_freq / 1000.0, 3) + " kHz</p>"
  "<p>Current cal_factor: " + String(cal_factor) + "</p>"

  "<form method='POST'>"
  "<button name='step' value='i'>+1 Hz</button>"
  "<button name='step' value='k'>-1 Hz</button><br>"
  "<button name='step' value='o'>+10 Hz</button>"
  "<button name='step' value='l'>-10 Hz</button><br>"
  "<button name='step' value='p'>+100 Hz</button>"
  "<button name='step' value=';'>-100 Hz</button><br><br>"
  "<button name='step' value='save'>Save</button>"
  "<button name='step' value='cancel'>Cancel</button>"
  "</form>"

  "</body></html>";
  return html;
}

// Calibration routine live interaction with webpage
void handleCalibrate() {
  if (rx_freq == 0) rx_freq = calibration_freq;
  cal_factor = (int32_t)(calibration_freq - rx_freq) + cal_factor_start;

  si5351.set_correction(cal_factor, SI5351_PLL_INPUT_XO);
  si5351.set_pll(SI5351_PLL_FIXED, SI5351_PLLA);
  si5351.set_freq(calibration_freq, SI5351_CLK0);
  si5351.set_clock_pwr(SI5351_CLK0, 1);
  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_2MA);

  showOLED("CALIBRATE", String(calibration_freq));

  // Process POST requests
  if (server.method() == HTTP_POST && server.hasArg("step")) {
    String step = server.arg("step");

    if (step == "save") {
      EEPROM_writeInt(EEPROM_ADDR_CAL, cal_factor);
      EEPROM.commit();

      server.sendHeader("Location", "/");
      server.send(303);        // POST → GET redirect
      showOLED("REBOOT");
      // Plan reboot in 1 seconde
      pendingReboot = true;
      rebootAt = millis() + 1000;

      return;
    } 

    if (step == "cancel") {
      server.sendHeader("Location", "/");
      server.send(303);        // POST → GET redirect
  
      si5351.set_freq(rxfreq, SI5351_CLK0);
      showOLEDFreq(getFinalFreqMHz());
      return;
    } 

    // step adjustment
    char c = step.charAt(0);
    int32_t stepVal = 0;
    switch (c) {
      case 'i': stepVal = 100; break;
      case 'k': stepVal = -100; break;
      case 'o': stepVal = 1000; break;
      case 'l': stepVal = -1000; break;
      case 'p': stepVal = 10000; break;
      case ';': stepVal = -10000; break;
    }

    rx_freq += stepVal;
    int32_t diff = (int32_t)(calibration_freq - rx_freq);
    cal_factor = diff + cal_factor_start;

    si5351.set_correction(cal_factor, SI5351_PLL_INPUT_XO);
    si5351.set_pll(SI5351_PLL_FIXED, SI5351_PLLA);
    si5351.pll_reset(SI5351_PLLA);
    si5351.set_freq(calibration_freq, SI5351_CLK0);
  }

  // Always send fresh HTML (safe for iPhone Safari)
  String html = htmlCalibrate();
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "text/html", html);
}

void wifiConnect() {
  // Wifi in STA mode
  WiFi.mode(WIFI_STA);
  wifi_set_sleep_type(NONE_SLEEP_T);
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  showOLED("CONNECT", "WiFi");

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 40) {
      delay(250);
      Serial.print(".");
      timeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected!");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      showOLED("WiFi OK");
  } else {
      Serial.println("\nWiFi FAILED!");
      showOLED("WiFi FAIL");
  }
  delay(1000);
}

void setup() {
  Serial.begin(115200);
  delay(300);
 
  Serial.println("Initialise SSD1306...");
  if (!display.begin(SSD1306_SWITCHCAPVCC, I2C_ADDRESS)) {
    Serial.println(F("Failure at SSD1306-init!"));
    for (;;);
  }

  // Read cal_factor
  EEPROM.begin(64);   
  cal_factor = EEPROM_readInt(EEPROM_ADDR_CAL);
  cal_factor_start = cal_factor;

  // Init met 25 MHz kristal
  if (!si5351.init(SI5351_CRYSTAL_LOAD_10PF, 25000000UL, 0)) {
    Serial.println("Si5351 init FAIL!");
    while (1);
  }
  // 2MA is enough on CLK0. you can put a 47 ohm over the output pins for balancing
  // calibration factor based on individual Si5351 TCXO
  si5351.set_correction(cal_factor, SI5351_PLL_INPUT_XO);
  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_2MA);  // SI5351_DRIVE_8MA=5 mw / SI5351_DRIVE_2MA= 1mw/ SI5351_DRIVE_4MA= 2mw
  si5351.output_enable(SI5351_CLK0, 1);
  si5351.set_freq(rxfreq, SI5351_CLK0);

  // switch off other outputs for savety
  si5351.output_enable(SI5351_CLK1, 0);
  si5351.output_enable(SI5351_CLK2, 0);
  Serial.println("Si5351 Ready");

  wifiConnect();

  // Start WebServer and point to the individual pages 
  server.on("/", handleRoot);

  server.on("/calibrate", handleCalibrate);

  server.on("/up", []() {
    rxfreq += step;
    si5351.set_freq(rxfreq, SI5351_CLK0);
    showOLEDFreq(getFinalFreqMHz());
    server.send(200, "text/html", htmlRoot());
  });

  server.on("/down", []() {
    rxfreq -= step;
    si5351.set_freq(rxfreq, SI5351_CLK0);
    showOLEDFreq(getFinalFreqMHz());
    server.send(200, "text/html", htmlRoot());
  });

  server.on("/preset", [](){
    double mhz = server.arg("mhz").toDouble();
    rxfreq = (uint64_t)(((mhz*1e6 - 10700000.0)/3.0)*100.0);
    si5351.set_freq(rxfreq, SI5351_CLK0);
    showOLEDFreq(getFinalFreqMHz());
    server.send(200,"text/html",htmlRoot());
  });

  server.begin();
  Serial.println("Webserver started");

  showOLED("TR-2200GX", "RX VFO", "ANG-V1.0");
  delay(2000);
  showOLEDFreq(getFinalFreqMHz());
}

void loop() {
  server.handleClient();

  if (pendingReboot && millis() > rebootAt) {
      ESP.restart();
  }
}
