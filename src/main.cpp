/*
  Bestia Control (ESP32) — Wi-Fi provisioning integrat, Sense NTP/WireGuard
  - Wi-Fi: intenta xarxes guardades; si falla, aixeca AP "BESTIA-SETUP"
  - Portal /wifi sense auth (per facilitar onboarding en AP)
  - WebServer (Basic Auth + Cookies) per la UI principal
  - Servo: PRESS (2s) i HOLD (11s)
  - Wake-on-LAN: paquet màgic a broadcast
  - Dispositius WOL en NVS (Preferences)
  - Xarxes Wi-Fi en NVS (Preferences)

  Requisits:
    - ESP32 core
    - ESP32Servo.h
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <WiFiUdp.h>
#include <Preferences.h>

// ====== Config Wi-Fi i credencials app ======
const char* www_username = "quim";
const char* www_password = "quim";

// ====== Maquinari ======
WebServer server(80);
Servo     myservo;
WiFiUDP   udp;
Preferences prefs;

// Pins
const int servoPin      = 13;
const int bootButtonPin = 0;   // BOOT amb PULLUP intern

// ====== WOL: dispositius guardats a NVS ======
struct Device {
  String name;
  String mac;
  bool   used;
};
static const uint8_t MAX_DEVICES = 10;
Device devices[MAX_DEVICES];

// ====== Wi-Fi: llista de xarxes guardades ======
struct Net {
  String ssid;
  String pass;
  bool   used;
};
static const int MAX_NETS = 8;
Net nets[MAX_NETS];

// AP de fallback
const char* AP_SSID = "BESTIA-SETUP";
const char* AP_PASS = "12345678";

// ====== Cookies d'autenticació (UI principal) ======
const char* cookieNames[]  = {"ESP32Auth1", "ESP32Auth2", "ESP32Auth3"};
const char* cookieValues[] = {"authenticated1", "authenticated2", "authenticated3"};

// ====== Prototips ======
// Auth
bool isAuthenticated();
bool checkAuthOrAsk();

// UI principal
void handleRoot();
void sendHTML();
void sendFeedbackPage(const String& msg, bool ok, bool refreshHome = true);

// Accions servo i WOL
void handlePress();
void handleHold();
void handleWake();
void handleWakeSaved();
void handleAdd();
void handleDelete();
void handleNotFound();

// Helpers WOL/MAC
bool parseMAC(const String& macStr, uint8_t mac[6]);
bool isValidMAC(const String& mac);
bool isValidName(const String& name);
bool wolSend(const uint8_t mac[6], IPAddress bcast = IPAddress(255,255,255,255), uint16_t port = 9);

// Persistència Bestia (WOL)
void loadDevices();
void saveDevices();
int  findDeviceSlotByName(const String& name);
int  findFreeSlot();

// Wi-Fi provisioning
void loadNets();
void saveNets();
bool tryConnect(const char* ssid, const char* pass, uint32_t msTimeout=10000);
bool connectKnown();
void startAP();

// Portal /wifi
String htmlHeader();
void pageWifi();
void routesWifi();

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  delay(100);

  // Servo i botó físic
  myservo.attach(servoPin);
  myservo.write(0);
  pinMode(bootButtonPin, INPUT_PULLUP);

  // Carrega llistes de WOL i Wi-Fi
  prefs.begin("bestia", false);
  loadDevices();
  prefs.end();
  loadNets();

  // Connexió a xarxes conegudes; si falla, AP de fallback
  if (!connectKnown()) {
    startAP();
  }

  // UDP (per WOL)
  udp.begin(0);

  // Rutes UI principal (amb auth)
  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/press",       HTTP_POST, handlePress);
  server.on("/hold",        HTTP_POST, handleHold);
  server.on("/wake",        HTTP_POST, handleWake);
  server.on("/wake_saved",  HTTP_POST, handleWakeSaved);
  server.on("/add",         HTTP_POST, handleAdd);
  server.on("/delete",      HTTP_POST, handleDelete);

  // Rutes portal Wi-Fi (sense auth per onboarding en AP)
  routesWifi();

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Servidor HTTP iniciat (port 80)");
}

void loop() {
  server.handleClient();

  // Botó físic: activació ràpida del servo
  if (digitalRead(bootButtonPin) == LOW) {
    myservo.write(120);
    delay(2000);
    myservo.write(0);
    delay(300); // debounce
  }
}

// ====== Auth ======
bool isAuthenticated() {
  if (server.hasHeader("Cookie")) {
    String cookie = server.header("Cookie");
    for (uint8_t i = 0; i < 3; ++i) {
      if (cookie.indexOf(String(cookieNames[i]) + "=" + String(cookieValues[i])) != -1) {
        return true;
      }
    }
  }
  return false;
}

bool checkAuthOrAsk() {
  if (!isAuthenticated() && !server.authenticate(www_username, www_password)) {
    server.requestAuthentication();
    return false;
  }
  if (!isAuthenticated()) {
    for (uint8_t i = 0; i < 3; ++i) {
      server.sendHeader("Set-Cookie", String(cookieNames[i]) + "=" + String(cookieValues[i]) + "; Path=/; Max-Age=7776000;");
    }
  }
  return true;
}

// ====== HTML principal ======
void handleRoot() {
  if (!checkAuthOrAsk()) return;
  sendHTML();
}

void sendHTML() {
  // Estat de xarxa
  String netLine;
  if (WiFi.getMode() & WIFI_STA && WiFi.status() == WL_CONNECTED) {
    netLine = "SSID: " + WiFi.SSID() + " — IP: " + WiFi.localIP().toString();
  } else if (WiFi.getMode() & WIFI_AP) {
    netLine = "AP: " + String(AP_SSID) + " — IP: " + WiFi.softAPIP().toString();
  } else {
    netLine = "Sense connexió.";
  }

  String html;
  html.reserve(7500);
  html += F(
    "<!DOCTYPE html><html lang='es'><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>WAKE-ON-LAN / Bestia</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Arial,sans-serif;background:#1a1a1a;color:#e0e0e0;line-height:1.6;padding:20px}"
    ".container{max-width:720px;margin:0 auto}"
    "h1{text-align:center;margin-bottom:30px;color:#fff;font-weight:300;font-size:2rem}"
    ".form{background:#2d2d2d;padding:20px;border-radius:8px;margin-bottom:20px;border:1px solid #404040}"
    ".input{width:100%;padding:12px;margin-bottom:10px;background:#1a1a1a;border:1px solid #404040;border-radius:4px;color:#e0e0e0;font-size:14px}"
    ".input:focus{outline:none;border-color:#4a9eff}"
    ".btn{padding:12px 20px;background:#4a9eff;color:#fff;border:none;border-radius:4px;cursor:pointer;font-size:14px;width:100%;transition:background .2s}"
    ".btn:hover{background:#357abd}"
    ".btn-danger{background:#e74c3c}.btn-danger:hover{background:#c0392b}"
    ".btn-small{padding:8px 12px;width:auto;margin-right:8px;font-size:12px}"
    ".device{background:#2d2d2d;padding:15px;margin-bottom:10px;border-radius:6px;border:1px solid #404040}"
    ".device-name{font-weight:500;margin-bottom:5px}"
    ".device-mac{font-family:monospace;color:#aaa;font-size:13px;margin-bottom:10px}"
    ".device-actions{display:flex;gap:8px;flex-wrap:wrap}"
    ".row{display:flex;gap:10px;flex-wrap:wrap}"
    ".row .input{flex:1;min-width:180px}"
    ".section-title{margin:10px 0 12px 0;font-weight:300;color:#ddd}"
    ".muted{color:#9aa}"
    "</style>"
    "</head><body><div class='container'>"
    "<h1>WAKE-ON-LAN · Control Bestia</h1>"
  );

  // Estat de xarxa i link a /wifi
  html += "<div class='form'><div class='section-title'>Xarxa</div><p class='muted'>" + netLine + "</p>";
  html += F("<div class='row' style='margin-top:10px'>"
            "<form method='GET' action='/wifi'><button class='btn' type='submit'>Configurar Wi-Fi</button></form>"
            "</div></div>");

  // Control manual
  html += F("<div class='form'><div class='section-title'>Control manual</div><div class='row'>");
  html += F("<form method='POST' action='/press'><button class='btn'>PRESS (2 s)</button></form>");
  html += F("<form method='POST' action='/hold'><button class='btn btn-danger'>HOLD (11 s)</button></form>");
  html += F("</div></div>");

  // WOL ràpid
  html += F("<div class='form'><div class='section-title'>Wake-on-LAN instantani</div>"
            "<form method='POST' action='/wake'>"
            "<div class='row'>"
            "<input class='input' name='mac' placeholder='AA:BB:CC:DD:EE:FF' required>"
            "<button class='btn' type='submit'>Despertar</button>"
            "</div>"
            "</form></div>");

  // Dispositius guardats
  html += F("<div class='form'><div class='section-title'>Dispositius guardats</div>");
  bool any = false;
  for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
    if (!devices[i].used) continue;
    any = true;
    html += F("<div class='device'>");
    html += "<div class='device-name'>" + devices[i].name + "</div>";
    html += "<div class='device-mac'>"  + devices[i].mac  + "</div>";
    html += F("<div class='device-actions'>");
    html += "<form method='POST' action='/wake_saved'><input type='hidden' name='mac' value='" + devices[i].mac + "'><button class='btn btn-small' type='submit'>Despertar</button></form>";
    html += "<form method='POST' action='/delete'><input type='hidden' name='name' value='" + devices[i].name + "'><button class='btn btn-danger btn-small' type='submit'>Eliminar</button></form>";
    html += F("</div></div>");
  }
  if (!any) {
    html += F("<p style='color:#999;font-style:italic'>No hi ha dispositius guardats.</p>");
  }

  // Afegir dispositiu
  html += F("<hr style='border:0;border-top:1px solid #404040;margin:14px 0'>"
            "<div class='section-title'>Afegir dispositiu</div>"
            "<form method='POST' action='/add'>"
            "<input class='input' name='name' placeholder='Nom del dispositiu' required>"
            "<input class='input' name='mac'  placeholder='AA:BB:CC:DD:EE:FF' required>"
            "<button class='btn' type='submit'>Guardar dispositiu</button>"
            "</form></div>");

  html += F("</div></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void sendFeedbackPage(const String& msg, bool ok, bool refreshHome) {
  String html;
  html.reserve(2000);
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  if (refreshHome) html += F("<meta http-equiv='refresh' content='2;url=/'/>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Resultat</title>"
            "<style>body{background:#1a1a1a;color:#e0e0e0;font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif;display:flex;align-items:center;justify-content:center;height:100vh}"
            ".box{background:#2d2d2d;border:1px solid #404040;border-radius:8px;padding:20px;max-width:480px;text-align:center}"
            ".ok{color:#27ae60}.ko{color:#e74c3c}</style></head><body><div class='box'>");
  html += "<h3 class='" + String(ok ? "ok" : "ko") + "'>" + msg + "</h3>";
  if (refreshHome) html += F("<p>Tornant a l'inici…</p>");
  html += F("</div></body></html>");
  server.send(ok ? 200 : 400, "text/html; charset=utf-8", html);
}

// ====== Handlers Bestia ======
void handlePress() {
  if (!checkAuthOrAsk()) return;
  myservo.write(120);
  delay(2000);
  myservo.write(0);
  sendFeedbackPage("Botó premut (2 s).", true);
}

void handleHold() {
  if (!checkAuthOrAsk()) return;
  myservo.write(120);
  delay(11000);
  myservo.write(0);
  sendFeedbackPage("Botó mantingut (11 s).", true);
}

void handleWake() {
  if (!checkAuthOrAsk()) return;
  if (!server.hasArg("mac")) { sendFeedbackPage("Falta MAC.", false); return; }
  String mac = server.arg("mac"); mac.trim();
  if (!isValidMAC(mac)) { sendFeedbackPage("MAC no vàlida.", false); return; }

  uint8_t macb[6];
  if (!parseMAC(mac, macb)) { sendFeedbackPage("Error parsejant la MAC.", false); return; }

  bool ok = wolSend(macb);
  sendFeedbackPage(ok ? ("Dispositiu " + mac + " despertat.") : "Error enviant paquet WOL.", ok);
}

void handleWakeSaved() {
  if (!checkAuthOrAsk()) return;
  if (!server.hasArg("mac")) { sendFeedbackPage("Falta MAC.", false); return; }
  String mac = server.arg("mac"); mac.trim();
  if (!isValidMAC(mac)) { sendFeedbackPage("MAC no vàlida.", false); return; }

  uint8_t macb[6];
  if (!parseMAC(mac, macb)) { sendFeedbackPage("Error parsejant la MAC.", false); return; }

  bool ok = wolSend(macb);
  sendFeedbackPage(ok ? ("Dispositiu " + mac + " despertat.") : "Error enviant paquet WOL.", ok);
}

void handleAdd() {
  if (!checkAuthOrAsk()) return;
  if (!server.hasArg("name") || !server.hasArg("mac")) {
    sendFeedbackPage("Falten camps.", false); return;
  }
  String name = server.arg("name"); name.trim();
  String mac  = server.arg("mac");  mac.trim();

  if (!isValidName(name) || !isValidMAC(mac)) {
    sendFeedbackPage("Nom o MAC no vàlids.", false); return;
  }
  int idx = findDeviceSlotByName(name);
  if (idx < 0) {
    idx = findFreeSlot();
    if (idx < 0) { sendFeedbackPage("Límit de dispositius assolit.", false); return; }
  }
  devices[idx].name = name;
  devices[idx].mac  = mac;
  devices[idx].used = true;
  saveDevices();
  sendFeedbackPage("Dispositiu guardat.", true);
}

void handleDelete() {
  if (!checkAuthOrAsk()) return;
  if (!server.hasArg("name")) { sendFeedbackPage("Falta 'name'.", false); return; }
  String name = server.arg("name"); name.trim();
  int idx = findDeviceSlotByName(name);
  if (idx < 0) { sendFeedbackPage("No s'ha trobat el dispositiu.", false); return; }
  devices[idx].used = false;
  devices[idx].name = "";
  devices[idx].mac  = "";
  saveDevices();
  sendFeedbackPage("Dispositiu eliminat.", true);
}

void handleNotFound() {
  server.send(404, "text/plain; charset=utf-8", "No trobat");
}

// ====== WOL helpers ======
bool isHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

bool isValidMAC(const String& mac) {
  if (mac.length() != 17) return false;
  for (int i = 0; i < 17; ++i) {
    if ((i + 1) % 3 == 0) {
      if (mac[i] != ':' && mac[i] != '-') return false;
    } else {
      if (!isHexDigit(mac[i])) return false;
    }
  }
  return true;
}

bool parseMAC(const String& macStr, uint8_t mac[6]) {
  String hex; hex.reserve(12);
  for (size_t i = 0; i < macStr.length(); ++i) {
    char c = macStr[i];
    if (c == ':' || c == '-') continue;
    if (!isHexDigit(c)) return false;
    hex += (char)toupper(c);
  }
  if (hex.length() != 12) return false;

  for (int i = 0; i < 6; ++i) {
    char b1 = hex[i*2];
    char b2 = hex[i*2 + 1];
    uint8_t v1 = (b1 <= '9') ? (b1 - '0') : (b1 - 'A' + 10);
    uint8_t v2 = (b2 <= '9') ? (b2 - '0') : (b2 - 'A' + 10);
    mac[i] = (v1 << 4) | v2;
  }
  return true;
}

bool wolSend(const uint8_t mac[6], IPAddress bcast, uint16_t port) {
  uint8_t packet[102];
  for (int i = 0; i < 6; ++i) packet[i] = 0xFF;
  for (int i = 0; i < 16; ++i) memcpy(packet + 6 + i*6, mac, 6);
  if (!udp.beginPacket(bcast, port)) return false;
  udp.write(packet, sizeof(packet));
  return udp.endPacket() == 1;
}

// ====== Persistència Bestia (WOL) ======
void loadDevices() {
  for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
    String keyN = "dname" + String(i);
    String keyM = "dmac"  + String(i);
    String n = prefs.getString(keyN.c_str(), "");
    String m = prefs.getString(keyM.c_str(), "");
    devices[i].name = n;
    devices[i].mac  = m;
    devices[i].used = (n.length() > 0 && m.length() > 0);
  }
}

void saveDevices() {
  prefs.begin("bestia", false);
  for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
    String keyN = "dname" + String(i);
    String keyM = "dmac"  + String(i);
    if (devices[i].used) {
      prefs.putString(keyN.c_str(), devices[i].name);
      prefs.putString(keyM.c_str(), devices[i].mac);
    } else {
      prefs.putString(keyN.c_str(), "");
      prefs.putString(keyM.c_str(), "");
    }
  }
  prefs.end();
}

int findDeviceSlotByName(const String& name) {
  for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
    if (devices[i].used && devices[i].name == name) return i;
  }
  return -1;
}

int findFreeSlot() {
  for (uint8_t i = 0; i < MAX_DEVICES; ++i) {
    if (!devices[i].used) return i;
  }
  return -1;
}

bool isValidName(const String& name) {
  if (name.length() == 0 || name.length() > 32) return false;
  for (size_t i = 0; i < name.length(); ++i) {
    char c = name[i];
    if (!(isalnum(c) || c==' ' || c=='-' || c=='_')) return false;
  }
  return true;
}

// ====== Wi-Fi provisioning ======
void loadNets() {
  prefs.begin("wifi", false);
  for (int i=0;i<MAX_NETS;i++){
    String kS = "s"+String(i), kP="p"+String(i);
    nets[i].ssid = prefs.getString(kS.c_str(), "");
    nets[i].pass = prefs.getString(kP.c_str(), "");
    nets[i].used = nets[i].ssid.length()>0;
  }
  prefs.end();
}

void saveNets() {
  prefs.begin("wifi", false);
  for (int i=0;i<MAX_NETS;i++){
    String kS = "s"+String(i), kP="p"+String(i);
    if (nets[i].used) {
      prefs.putString(kS.c_str(), nets[i].ssid);
      prefs.putString(kP.c_str(), nets[i].pass);
    } else {
      prefs.putString(kS.c_str(), "");
      prefs.putString(kP.c_str(), "");
    }
  }
  prefs.end();
}

bool tryConnect(const char* ssid, const char* pass, uint32_t msTimeout){
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  uint32_t t0=millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-t0<msTimeout) { delay(200); }
  if (WiFi.status()==WL_CONNECTED) {
    Serial.printf("WiFi OK -> %s, IP: %s\n", ssid, WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.printf("WiFi FAIL -> %s\n", ssid);
  return false;
}

bool connectKnown() {
  WiFi.mode(WIFI_STA);
  for (int i=0;i<MAX_NETS;i++){
    if (!nets[i].used) continue;
    if (tryConnect(nets[i].ssid.c_str(), nets[i].pass.c_str(), 10000)) return true;
  }
  return false;
}

void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("AP %s engegat. IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

// ====== Portal /wifi (sense auth) ======
String htmlHeader() {
  return F("<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Wi-Fi Setup</title><style>"
           "body{background:#1a1a1a;color:#e0e0e0;font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif;padding:20px}"
           ".box{max-width:720px;margin:0 auto;background:#2d2d2d;border:1px solid #404040;border-radius:8px;padding:20px}"
           "input,button{padding:10px;border-radius:6px;border:1px solid #404040;background:#1a1a1a;color:#e0e0e0}"
           "button{background:#4a9eff;border:none;cursor:pointer}"
           "table{width:100%;border-collapse:collapse;margin-top:10px}"
           "td,th{border-bottom:1px solid #404040;padding:8px;text-align:left}"
           "a{color:#9cf;text-decoration:none}"
           "</style>");
}

void pageWifi() {
  String s = htmlHeader();
  s += "<div class='box'><h1>Configuració Wi-Fi</h1>";
  // Estat
  if (WiFi.getMode() & WIFI_STA && WiFi.status()==WL_CONNECTED) {
    s += "<p>Connectat a <b>"+WiFi.SSID()+"</b> — IP "+WiFi.localIP().toString()+"</p>";
  } else if (WiFi.getMode() & WIFI_AP) {
    s += "<p>Mode AP <b>"+String(AP_SSID)+"</b> — IP "+WiFi.softAPIP().toString()+"</p>";
  } else {
    s += "<p>Sense connexió.</p>";
  }

  s += "<form method='POST' action='/wifi/scan'><button>Escanejar xarxes</button></form><br>";

  s += "<h3>Afegir xarxa</h3><form method='POST' action='/wifi/add'>"
       "SSID:<br><input name='ssid' required><br>Contrasenya:<br><input name='pass' type='password'><br><br>"
       "<button>Guardar</button></form><hr><h3>Xarxes guardades</h3>";

  s += "<table><tr><th>Prioritat</th><th>SSID</th><th>Accions</th></tr>";
  for (int i=0;i<MAX_NETS;i++){
    if (!nets[i].used) continue;
    s += "<tr><td>"+String(i+1)+"</td><td>"+nets[i].ssid+"</td><td>";
    s += "<form style='display:inline' method='POST' action='/wifi/test'><input type='hidden' name='i' value='"+String(i)+"'><button>Provar</button></form> ";
    if (i>0) s += "<form style='display:inline' method='POST' action='/wifi/up'><input type='hidden' name='i' value='"+String(i)+"'><button>Pujar</button></form> ";
    if (i<MAX_NETS-1) s += "<form style='display:inline' method='POST' action='/wifi/down'><input type='hidden' name='i' value='"+String(i)+"'><button>Baixar</button></form> ";
    s += "<form style='display:inline' method='POST' action='/wifi/del'><input type='hidden' name='i' value='"+String(i)+"'><button>Eliminar</button></form>";
    s += "</td></tr>";
  }
  s += "</table><br><form method='POST' action='/wifi/clear'><button>Esborrar totes</button></form>";
  s += "<br><a href='/'>Tornar a la pàgina principal</a>";
  s += "</div>";
  server.send(200, "text/html; charset=utf-8", s);
}

void routesWifi() {
  server.on("/wifi", HTTP_GET, pageWifi);

  server.on("/wifi/add", HTTP_POST, []{
    if (!server.hasArg("ssid")) { server.send(400,"text/plain","Falta ssid"); return; }
    String ssid = server.arg("ssid"); String pass = server.arg("pass");
    // Primer slot lliure o sobreescriu si ja existeix amb mateix SSID
    int slot = -1;
    for (int i=0;i<MAX_NETS;i++){
      if (nets[i].used && nets[i].ssid == ssid) { slot = i; break; }
    }
    if (slot < 0) {
      for (int i=0;i<MAX_NETS;i++) if (!nets[i].used){ slot = i; break; }
    }
    if (slot < 0) { server.send(400,"text/plain","Límit de xarxes assolit"); return; }
    nets[slot] = {ssid, pass, true};
    saveNets();
    server.sendHeader("Location","/wifi"); server.send(302,"text/plain","");
  });

  server.on("/wifi/scan", HTTP_POST, []{
    // En mode AP pur no pot escanejar; opcionalment podríem passar a AP+STA
    if (WiFi.getMode() == WIFI_AP) {
      WiFi.mode(WIFI_AP_STA); // permet escanejar en AP+STA
    }
    int n = WiFi.scanNetworks();
    String s = htmlHeader(); s += "<div class='box'><h1>Resultat escaneig</h1><ul>";
    for (int i=0;i<n;i++) {
      s += "<li>"+WiFi.SSID(i)+" ("+String(WiFi.RSSI(i))+" dBm)"+(WiFi.encryptionType(i)==WIFI_AUTH_OPEN?" [OPEN]":"")+"</li>";
    }
    s += "</ul><a href='/wifi'>Tornar</a></div>";
    server.send(200,"text/html; charset=utf-8",s);
    WiFi.scanDelete();
  });

  auto swapRows = [&](int a,int b){ Net tmp=nets[a]; nets[a]=nets[b]; nets[b]=tmp; saveNets(); };

  server.on("/wifi/up", HTTP_POST, [&]{
    int i = server.arg("i").toInt(); if (i>0 && nets[i].used && nets[i-1].used) swapRows(i,i-1);
    server.sendHeader("Location","/wifi"); server.send(302,"text/plain","");
  });
  server.on("/wifi/down", HTTP_POST, [&]{
    int i = server.arg("i").toInt(); if (i<MAX_NETS-1 && nets[i].used && nets[i+1].used) swapRows(i,i+1);
    server.sendHeader("Location","/wifi"); server.send(302,"text/plain","");
  });
  server.on("/wifi/del", HTTP_POST, []{
    int i = server.arg("i").toInt(); if (i>=0 && i<MAX_NETS){ nets[i].used=false; nets[i].ssid=""; nets[i].pass=""; saveNets(); }
    server.sendHeader("Location","/wifi"); server.send(302,"text/plain","");
  });
  server.on("/wifi/test", HTTP_POST, []{
    int i = server.arg("i").toInt();
    bool ok=false;
    if (i>=0 && i<MAX_NETS && nets[i].used) ok = tryConnect(nets[i].ssid.c_str(), nets[i].pass.c_str(), 8000);
    String s = htmlHeader(); s += String("<div class='box'><h1>Prova connexió</h1>") +
      (ok? "Connexió OK a "+nets[i].ssid+" — IP: "+WiFi.localIP().toString() : "Connexió fallida.") +
      "<br><a href='/wifi'>Tornar</a></div>";
    server.send(ok?200:400,"text/html; charset=utf-8",s);
    // Si ha connectat i estàvem en AP, quedem en STA (opcional)
    if (ok && (WiFi.getMode() & WIFI_AP)) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
    }
  });

  server.on("/wifi/clear", HTTP_POST, []{
    for (int i=0;i<MAX_NETS;i++){ nets[i].used=false; nets[i].ssid=""; nets[i].pass=""; }
    saveNets();
    server.sendHeader("Location","/wifi"); server.send(302,"text/plain","");
  });
}
