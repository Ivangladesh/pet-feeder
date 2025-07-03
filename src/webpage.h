#pragma once
#include <Arduino.h>

String getWebpageHtml() {
  File file = SPIFFS.open("/webpage.html", "r");
  if (!file) {
    Serial.printf("[getWebpageHtml] Error: No se pudo abrir /webpage.html\n");
    return "<html><body>Error: No se pudo abrir webpage.html</body></html>";
  }
  String html = file.readString();
  file.close();
  Serial.printf("[getWebpageHtml] HTML cargado correctamente. Tamaño: %d bytes\n", html.length());
  return html;
}
