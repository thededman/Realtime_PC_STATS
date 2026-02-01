#pragma once

#include <Arduino.h>

// Initialize the config portal (call in setup before WiFi)
void configPortalInit();

// Check if configuration exists in NVS
bool configPortalCheck();

// Start the captive portal AP and web server
void configPortalStart();

// Handle portal requests (call in loop when in setup mode)
void configPortalLoop();

// Stop the portal and clean up
void configPortalStop();

// Getters for stored configuration
String getConfigWifiSSID();
String getConfigWifiPass();
String getConfigApiKey();
String getConfigCity();
String getConfigUnits();

// Check if currently in setup mode
bool isInSetupMode();

// Display setup screen on M5 display
void displaySetupScreen();
