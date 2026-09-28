#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

void initActuators();
void stopActuators(const char* reason);
void serviceActuators(bool cloudConnected);
void handleActuatorCommand(JsonObjectConst command);
String actuatorsJson();
bool actuatorReportPending();
void actuatorReportSent();
