#include <Arduino.h>
#include <WiFi.h>
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

#define WIFI_SSID ""
#define WIFI_PASSWORD ""
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
  
  myStepper.setSpeed(10);

  server.on("/alimentar", HTTP_POST, []() {
    Serial.println("Alimentación manual activada: LED2 encendido");
    // Apagar después de 5 segundos (no bloqueante)
    alimentarMotor();
    static unsigned long alimentarAhoraMillis = 0;
    alimentarAhoraMillis = millis();
    server.send(200, "text/plain", "¡Alimentación manual activada!");
    // Guardar el tiempo para apagar en loop
    extern unsigned long alimentarAhoraMillisGlobal;
    alimentarAhoraMillisGlobal = alimentarAhoraMillis;
  });
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

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  Serial.print("Connecting to WiFi ");
  Serial.print(WIFI_SSID);
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.print(".");
  }
  Serial.println(" Connected!");

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Inicializar NTP
  timeClient.begin();
  if (timeClient.forceUpdate()) {
    Serial.print("Hora actual: ");
    Serial.println(timeClient.getFormattedTime());
  } else {
    Serial.println("No se pudo obtener la hora NTP");
  }

  server.on("/", sendHtml);

  // Ruta para obtener la configuración guardada en formato JSON (array de objetos)
  server.on("/configuracion", HTTP_GET, []() {
    String json = "[";
    for (size_t i = 0; i < configuraciones.size(); ++i) {
      json += "{\"dia\":\"" + configuraciones[i].dia + "\",\"horario\":\"" + configuraciones[i].horario + "\"}";
      if (i < configuraciones.size() - 1) json += ",";
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  // Ruta para recibir la configuración del alimentador (array de objetos o formato antiguo)
  server.on("/configurar", HTTP_POST, []() {
    String body = server.arg("plain");
    Serial.println("\nConfiguración recibida:");
    Serial.println(body);
    // Si el body es un array de objetos [{"dia":"L","horario":"08:00"}, ...] (eliminar)
    // Nuevo formato: {"dia":"L","horarios":["08:00","12:00"]}
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
          // Eliminar todas las configuraciones previas de ese día
          for (auto it = configuraciones.begin(); it != configuraciones.end(); ) {
            if (it->dia == dia) it = configuraciones.erase(it);
            else ++it;
          }
          // Agregar los nuevos horarios para ese día
          for (size_t j = 0; j < horarios.size(); ++j) {
            if (configuraciones.size() < 21)
              configuraciones.push_back({dia, horarios[j]});
          }
        }
      }
    } else if (body.startsWith("[")) {
      // Compatibilidad: array de objetos
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

  server.begin();
  Serial.println("HTTP server started");
}


unsigned long lastFeedMillis = 0;
bool alimentando = false;
unsigned long alimentarAhoraMillisGlobal = 0;
bool alimentandoAhora = false;

void loop(void) {
  server.handleClient();
  timeClient.update(); // Mantener la hora actualizada

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
