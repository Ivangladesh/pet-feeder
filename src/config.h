#pragma once
#include <vector>
#include <Arduino.h>

struct ConfiguracionAlimentador {
  std::vector<String> dias;
  std::vector<String> horarios;
};

extern ConfiguracionAlimentador configActual;
void guardarConfiguracionEEPROM(const ConfiguracionAlimentador& config);
void cargarConfiguracionEEPROM(ConfiguracionAlimentador& config);
