#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h> // Librería para portal cautivo
#include <WiFiClient.h>
#include <WebServer.h>
#include <uri/UriBraces.h>
#include <FS.h>
#include <SPIFFS.h>
#include <EEPROM.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <AESLib.h>
#include <Stepper.h>
#include "webpage.h"
#include "config.h"
// Estructura para almacenar hasta 3 horarios por día
struct ConfigDiaHorario {
  String dia;
  String horario;
};
std::vector<ConfigDiaHorario> configuraciones; // Máximo 21 elementos
WiFiUDP ntpUDP;
// Zona horaria de México Central: UTC-6 (invierno), UTC-5 (verano)
// Para horario estándar (invierno): offset = -6*3600 = -21600
// Para horario de verano (verano): offset = -5*3600 = -18000
// Aquí se usa -21600 para México Central (ajusta si necesitas horario de verano)
NTPClient timeClient(ntpUDP, "pool.ntp.org", -21600, 60000); // UTC-6, actualiza cada 60s
// Funciones para guardar y cargar configuración en EEPROM (máx 21 pares dia-horario)
void guardarConfiguracionEEPROM(const std::vector<ConfigDiaHorario>& confs) {
  Serial.println("Guardando configuración en EEPROM...");
  EEPROM.begin(512);
  String data = "";
  for (size_t i = 0; i < confs.size(); ++i) {
    data += confs[i].dia + "," + confs[i].horario;
    if (i < confs.size() - 1) data += ";";
  }
  Serial.print("Cadena final a guardar: "); Serial.println(data);
  for (size_t i = 0; i < data.length(); ++i) {
    EEPROM.write(i, data[i]);
  }
  EEPROM.write(data.length(), '\0');
  EEPROM.commit();
  Serial.println("Configuración guardada en EEPROM.");
}

void cargarConfiguracionEEPROM(std::vector<ConfigDiaHorario>& confs) {
  Serial.println("Cargando configuración de EEPROM...");
  EEPROM.begin(512);
  char data[512];
  for (size_t i = 0; i < sizeof(data) - 1; ++i) {
    data[i] = EEPROM.read(i);
    if (data[i] == '\0') break;
  }
  data[sizeof(data) - 1] = '\0';
  String str = String(data);
  Serial.print("Cadena leída de EEPROM: "); Serial.println(str);
  confs.clear();
  int last = 0, idx = 0;
  while ((idx = str.indexOf(';', last)) != -1) {
    String par = str.substring(last, idx);
    int coma = par.indexOf(',');
    if (coma > 0) {
      confs.push_back({par.substring(0, coma), par.substring(coma + 1)});
    }
    last = idx + 1;
  }
  // Último par
  if (str.length() > last) {
    String par = str.substring(last);
    int coma = par.indexOf(',');
    if (coma > 0) {
      confs.push_back({par.substring(0, coma), par.substring(coma + 1)});
    }
  }
  Serial.print("Configuraciones cargadas: ");
  for (auto& c : confs) Serial.printf("[%s,%s] ", c.dia.c_str(), c.horario.c_str());
  Serial.println();
}

// --- Encriptación de credenciales WiFi ---
#define AES_KEY "1234567890123456" // 16 bytes (clave fija, puedes cambiarla)
#define WIFI_EEPROM_ADDR 400 // Dirección EEPROM para credenciales WiFi

String wifi_ssid = "";
String wifi_password = "";

AESLib aesLib;

// --- Encriptar y desencriptar usando AESLib (encrypt64/decrypt64) ---
byte aes_key[16] = {0};
byte aes_iv[16] = {0}; // IV fijo para ejemplo, puedes mejorar esto

void prepareKeyAndIV(const String& keyStr) {
  // Copia la clave al buffer de 16 bytes
  memset(aes_key, 0, 16);
  size_t len = keyStr.length();
  if (len > 16) len = 16;
  memcpy(aes_key, keyStr.c_str(), len);
  // IV en cero (puedes cambiarlo si quieres)
  memset(aes_iv, 0, 16);
}

String encryptString(const String& text, const String& key) {
  prepareKeyAndIV(key);
  char encrypted[256];
  int msgLen = text.length();
  const byte* plainBytes = (const byte*)text.c_str();
  int encLen = aesLib.encrypt64(
    plainBytes,
    msgLen,
    encrypted,
    aes_key,
    16, // keyLen
    aes_iv
  );
  encrypted[encLen] = '\0';
  return String(encrypted);
}

String decryptString(const String& encrypted, const String& key) {
  prepareKeyAndIV(key);
  byte decrypted[256];
  int encLen = strlen(encrypted.c_str());
  int decLen = aesLib.decrypt64(
    (char*)encrypted.c_str(),
    encLen,
    decrypted,
    aes_key,
    16, // keyLen
    aes_iv
  );
  decrypted[decLen] = '\0';
  return String((char*)decrypted);
}

void guardarCredencialesWiFi(const String& ssid, const String& password) {
  String enc_ssid = encryptString(ssid, AES_KEY);
  String enc_pass = encryptString(password, AES_KEY);
  String data = enc_ssid + "," + enc_pass;
  EEPROM.begin(512);
  for (size_t i = 0; i < data.length(); ++i) {
    EEPROM.write(WIFI_EEPROM_ADDR + i, data[i]);
  }
  EEPROM.write(WIFI_EEPROM_ADDR + data.length(), '\0');
  EEPROM.commit();
  wifi_ssid = ssid;
  wifi_password = password;
  Serial.println("Credenciales WiFi guardadas encriptadas en EEPROM.");
}

void cargarCredencialesWiFi() {
  EEPROM.begin(512);
  char data[128];
  size_t i = 0;
  bool soloFF = true;
  for (; i < sizeof(data) - 1; ++i) {
    data[i] = EEPROM.read(WIFI_EEPROM_ADDR + i);
    if (data[i] != 0xFF) soloFF = false;
    if (data[i] == '\0' || data[i] == 0xFF) break;
  }
  data[i] = '\0';
  String str = String(data);
  int coma = str.indexOf(',');
  // Si la EEPROM está vacía o solo contiene 0xFF, usar por defecto
  if (soloFF || coma <= 0 || coma >= (int)str.length() - 1) {
    wifi_ssid = "IoT";
    wifi_password = "24_RunJapan2024.1";
    Serial.println("No hay credenciales WiFi guardadas, usando por defecto.");
    return;
  }
  String enc_ssid = str.substring(0, coma);
  String enc_pass = str.substring(coma + 1);
  wifi_ssid = decryptString(enc_ssid, AES_KEY);
  wifi_password = decryptString(enc_pass, AES_KEY);
  // Validar que el resultado sea imprimible y no vacío
  bool ssidValido = wifi_ssid.length() > 0 && wifi_ssid.length() < 33;
  bool passValido = wifi_password.length() > 0 && wifi_password.length() < 65;
  for (size_t j = 0; j < wifi_ssid.length(); ++j) {
    if (wifi_ssid[j] < 32 || wifi_ssid[j] > 126) ssidValido = false;
  }
  for (size_t j = 0; j < wifi_password.length(); ++j) {
    if (wifi_password[j] < 32 || wifi_password[j] > 126) passValido = false;
  }
  if (!ssidValido || !passValido) {
    wifi_ssid = "IoT";
    wifi_password = "24_RunJapan2024.1";
    Serial.println("Credenciales WiFi corruptas, usando por defecto.");
    return;
  }
  Serial.println("Credenciales WiFi desencriptadas:");
  Serial.println("SSID: " + wifi_ssid);
  Serial.println("PASS: " + wifi_password);
}
// Defining the WiFi channel speeds up the connection:
#define WIFI_CHANNEL 6


WebServer server(80);

const int stepsPerRevolution = 2048;

// ULN2003 Motor Driver Pins
#define IN1 19
#define IN2 18
#define IN3 5
#define IN4 17

// initialize the stepper library
Stepper myStepper(stepsPerRevolution, IN1, IN3, IN2, IN4);

// Ejecuta el ciclo de alimentación completo
void alimentarMotor() {
  myStepper.step(stepsPerRevolution * 4);
  digitalWrite(19, LOW);
  digitalWrite(18, LOW);
  digitalWrite(5, LOW);
  digitalWrite(17, LOW);
}

void sendHtml() {
  String response = getWebpageHtml();
  Serial.println("Enviando página web al cliente...");
  server.send(200, "text/html", response);
}



void setup(void) {

    // Servir archivos CSS desde SPIFFS
  // server.serveStatic("/light.css", SPIFFS, "/light.css");
  // server.serveStatic("/light-mc.css", SPIFFS, "/light-mc.css");
  // server.serveStatic("/light-hc.css", SPIFFS, "/light-hc.css");
  // server.serveStatic("/dark.css", SPIFFS, "/dark.css");
  // server.serveStatic("/dark-mc.css", SPIFFS, "/dark-mc.css");
  // server.serveStatic("/dark-hc.css", SPIFFS, "/dark-hc.css");
  // // Route to load style.css file
  // server.on("/style.css", HTTP_GET, [](){
  //   server.send(200, "/style.css", "text/css");
  // });
  server.on("/light,css", HTTP_GET, []() {
    File f = SPIFFS.open("/light,css", "r");
    if (!f) {
      server.send(500, "text/css", "No se encontró wifimanager.html en SPIFFS");
      return;
    }
    String css = f.readString();
    f.close();
    server.send(200, "/style.css", "text/css");
  });

  // Endpoint para mostrar el portal de configuración WiFi (sirve wifimanager.html)
  server.on("/setupwifi", HTTP_GET, []() {
    File f = SPIFFS.open("/wifimanager.html", "r");
    if (!f) {
      server.send(500, "text/plain", "No se encontró wifimanager.html en SPIFFS");
      return;
    }
    String html = f.readString();
    f.close();
    server.send(200, "text/html", html);
  });
  myStepper.setSpeed(10);
  Serial.begin(115200);
  Serial.println();
  Serial.println("Iniciando setup...");

  if (!SPIFFS.begin(true)) {
    Serial.println("Error al montar SPIFFS");
    return;
  }

  EEPROM.begin(512);
  Serial.println("Configuraciones antes de cargar: " + String(configuraciones.size()));
  cargarConfiguracionEEPROM(configuraciones);
  Serial.println("Configuraciones después de cargar: " + String(configuraciones.size()));

  // --- Lógica para decidir si lanzar WiFiManager ---
  cargarCredencialesWiFi();
  bool sinCredenciales = wifi_ssid.length() == 0 || wifi_password.length() == 0;
  bool forzarPortal = false;
  if (server.arg("setupwifi") == "1") {
    forzarPortal = true;
  }

  if (sinCredenciales || forzarPortal) {
    WiFiManager wifiManager;
    wifiManager.setAPCallback([](WiFiManager* wm) {
      Serial.println("Portal WiFiManager activo. Conéctate a la red y abre 192.168.4.1");
    });
    wifiManager.setConfigPortalTimeout(180); // 3 minutos
    wifiManager.setCustomHeadElement("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    // wifiManager.setMenu({"wifi","info","exit"}); // No compatible en todas las versiones
    if (!wifiManager.autoConnect("DogFeeder-Setup")) {
      Serial.println("No se pudo conectar y no se proporcionaron credenciales.");
      ESP.restart();
      delay(1000);
    }
    Serial.println("¡Conectado a WiFi!");
    Serial.println(WiFi.localIP());
    // Guardar credenciales encriptadas si se usó el portal
    guardarCredencialesWiFi(WiFi.SSID(), WiFi.psk());
  } else {
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str(), WIFI_CHANNEL);
    Serial.print("Connecting to WiFi ");
    Serial.print(wifi_ssid);
    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED && intentos < 100) {
      delay(100);
      Serial.print(".");
      intentos++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(" Connected!");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("\nNo se pudo conectar a la red guardada. Lanzando portal WiFiManager.");
      WiFiManager wifiManager;
      wifiManager.setAPCallback([](WiFiManager* wm) {
        Serial.println("Portal WiFiManager activo. Conéctate a la red y abre 192.168.4.1");
      });
      wifiManager.setConfigPortalTimeout(180);
      wifiManager.setCustomHeadElement("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
      // wifiManager.setMenu({"wifi","info","exit"}); // No compatible en todas las versiones
      if (!wifiManager.autoConnect("DogFeeder-Setup")) {
        Serial.println("No se pudo conectar y no se proporcionaron credenciales.");
        ESP.restart();
        delay(1000);
      }
      Serial.println("¡Conectado a WiFi!");
      Serial.println(WiFi.localIP());
      guardarCredencialesWiFi(WiFi.SSID(), WiFi.psk());
    }
  }

  // Inicializar NTP solo si hay WiFi
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.begin();
    if (timeClient.forceUpdate()) {
      Serial.print("Hora actual: ");
      Serial.println(timeClient.getFormattedTime());
    } else {
      Serial.println("No se pudo obtener la hora NTP");
    }
  } else {
    Serial.println("No se inicializa NTPClient porque no hay WiFi");
  }

  // --- Servidor web para funciones del alimentador ---
  server.on("/", HTTP_GET, []() {
    File f = SPIFFS.open("/webpage.html", "r");
    if (!f) {
      server.send(500, "text/plain", "No se encontró webpage.html en SPIFFS");
      return;
    }
    String html = f.readString();
    f.close();
    server.send(200, "text/html", html);
  });

  server.on("/alimentar", HTTP_POST, []() {
    Serial.println("Alimentación manual activada: LED2 encendido");
    alimentarMotor();
    static unsigned long alimentarAhoraMillis = 0;
    alimentarAhoraMillis = millis();
    server.send(200, "text/plain", "¡Alimentación manual activada!");
    extern unsigned long alimentarAhoraMillisGlobal;
    alimentarAhoraMillisGlobal = alimentarAhoraMillis;
  });

  server.on("/configuracion", HTTP_GET, []() {
    String json = "[";
    for (size_t i = 0; i < configuraciones.size(); ++i) {
      json += "{\"dia\":\"" + configuraciones[i].dia + "\",\"horario\":\"" + configuraciones[i].horario + "\"}";
      if (i < configuraciones.size() - 1) json += ",";
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  server.on("/configurar", HTTP_POST, []() {
    String body = server.arg("plain");
    Serial.println("\nConfiguración recibida:");
    Serial.println(body);
    if (body.startsWith("{")) {
      int d1 = body.indexOf("\"dia\"");
      int h1 = body.indexOf("\"horarios\"");
      if (d1 != -1 && h1 != -1) {
        int dval1 = body.indexOf('"', d1+6);
        int dval2 = body.indexOf('"', dval1+1);
        String dia = body.substring(dval1+1, dval2);
        int arrIni = body.indexOf('[', h1);
        int arrFin = body.indexOf(']', arrIni);
        if (arrIni != -1 && arrFin != -1) {
          String horariosStr = body.substring(arrIni+1, arrFin);
          std::vector<String> horarios;
          int last = 0, idx = 0;
          while ((idx = horariosStr.indexOf('"', last)) != -1) {
            int end = horariosStr.indexOf('"', idx+1);
            if (end == -1) break;
            horarios.push_back(horariosStr.substring(idx+1, end));
            last = end+1;
          }
          for (auto it = configuraciones.begin(); it != configuraciones.end(); ) {
            if (it->dia == dia) it = configuraciones.erase(it);
            else ++it;
          }
          for (size_t j = 0; j < horarios.size(); ++j) {
            if (configuraciones.size() < 21)
              configuraciones.push_back({dia, horarios[j]});
          }
        }
      }
    } else if (body.startsWith("[")) {
      configuraciones.clear();
      int pos = 0;
      while (true) {
        int objIni = body.indexOf('{', pos);
        int objFin = body.indexOf('}', objIni);
        if (objIni == -1 || objFin == -1) break;
        String obj = body.substring(objIni, objFin+1);
        int d1 = obj.indexOf("\"dia\"");
        int h1 = obj.indexOf("\"horario\"");
        if (d1 != -1 && h1 != -1) {
          int dval1 = obj.indexOf('"', d1+6);
          int dval2 = obj.indexOf('"', dval1+1);
          int hval1 = obj.indexOf('"', h1+10);
          int hval2 = obj.indexOf('"', hval1+1);
          if (dval1 != -1 && dval2 != -1 && hval1 != -1 && hval2 != -1) {
            String dia = obj.substring(dval1+1, dval2);
            String horario = obj.substring(hval1+1, hval2);
            configuraciones.push_back({dia, horario});
          }
        }
        pos = objFin+1;
        if (configuraciones.size() >= 21) break;
      }
    }
    Serial.print("Configuraciones parseadas: ");
    for (auto& c : configuraciones) Serial.printf("[%s,%s] ", c.dia.c_str(), c.horario.c_str());
    Serial.println();
    guardarConfiguracionEEPROM(configuraciones);
    server.send(200, "text/plain", "¡Configuración guardada en memoria!");
  });

  // --- Endpoint para establecer credenciales WiFi (útil si quieres cambiar la red desde la web) ---
  server.on("/set_wifi", HTTP_POST, []() {
    String body = server.arg("plain");
    int s1 = body.indexOf("\"ssid\"");
    int p1 = body.indexOf("\"password\"");
    if (s1 != -1 && p1 != -1) {
      int sval1 = body.indexOf('"', s1+6);
      int sval2 = body.indexOf('"', sval1+1);
      int pval1 = body.indexOf('"', p1+10);
      int pval2 = body.indexOf('"', pval1+1);
      if (sval1 != -1 && sval2 != -1 && pval1 != -1 && pval2 != -1) {
        String ssid = body.substring(sval1+1, sval2);
        String pass = body.substring(pval1+1, pval2);
        guardarCredencialesWiFi(ssid, pass);
        server.send(200, "text/plain", "¡Credenciales WiFi guardadas!");
        ESP.restart();
        return;
      }
    }
    server.send(400, "text/plain", "Formato inválido. Esperado: {\"ssid\":\"...\",\"password\":\"...\"}");
  });

  server.begin();
  Serial.println("HTTP server started");
}


unsigned long lastFeedMillis = 0;
bool alimentando = false;
unsigned long alimentarAhoraMillisGlobal = 0;
bool alimentandoAhora = false;

void loop(void) {
  server.handleClient();
  // Solo actualizar NTP si hay WiFi
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update(); // Mantener la hora actualizada
  }

  // Verificar solo una vez por minuto
  static int ultimoMinutoVerificado = -1;
  String horaActual = timeClient.getFormattedTime().substring(0,5);
  int minutoActual = horaActual.substring(3,5).toInt();
  // Obtener día actual (0=Domingo, 1=Lunes, ...)
  int diaSemana = (timeClient.getDay() + 6) % 7; // Ajuste para que 0=Lunes, 6=Domingo
  const char* diasCod[] = {"L","M","X","J","V","S","D"};

  if (minutoActual != ultimoMinutoVerificado) {
    ultimoMinutoVerificado = minutoActual;
    // Si no está alimentando, revisa si debe activar
    if (!alimentando) {
      for (size_t i = 0; i < configuraciones.size(); ++i) {
        Serial.printf("Comparando [%s,%s]...\n", configuraciones[i].dia.c_str(), configuraciones[i].horario.c_str());
        if (configuraciones[i].dia == diasCod[diaSemana] && configuraciones[i].horario == horaActual) {
          alimentando = true;
          lastFeedMillis = millis();
          alimentarMotor();
          Serial.println("Alimentación activada");
          break;
        }
      }
    }
  }

  // Si está alimentando, apaga el LED después de 5 segundos
  if (alimentando && millis() - lastFeedMillis >= 5000) {
    //digitalWrite(LED1, LOW);
    alimentando = false;
    Serial.println("Alimentación finalizada: LED1 apagado");
  }

  // Alimentar ahora (LED2)
  if (alimentarAhoraMillisGlobal > 0 && !alimentandoAhora) {
    alimentandoAhora = true;
  }
  if (alimentandoAhora && millis() - alimentarAhoraMillisGlobal >= 5000) {
    //digitalWrite(LED2, LOW);
    alimentandoAhora = false;
    alimentarAhoraMillisGlobal = 0;
    Serial.println("Alimentación manual finalizada: LED2 apagado");
  }

  delay(200); // Evita sobrecarga de CPU
}
