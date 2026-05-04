#include <string.h>
#include "menu.h"

// Initialise static instance pointer
Menu *Menu::instance = nullptr;

Menu::Menu(uint8_t p_p, uint8_t s_p, uint8_t n_p, Settings *s, Buzzer *b, RX5808 *r, Api *a, UsbSerial *u)
  : menuIndex(SCAN),
    previous_pin(p_p), select_pin(s_p), next_pin(n_p),
    selectButtonPressTime(0), selectButtonHeld(false),
    showMarkers(true),
    settings(s), buzzer(b), receiver(r), api(a), usb(u),
    u8g2(U8G2_R0, U8X8_PIN_NONE) {
  instance = this;  // Set static instance pointer
}

// Begin menu object
void Menu::begin() {
  initMenus();

  // Can't call in constructor as pulldown overwritten during boot before setup() called
  pinMode(previous_pin, INPUT_PULLDOWN);
  pinMode(select_pin, INPUT_PULLDOWN);
  pinMode(next_pin, INPUT_PULLDOWN);

  u8g2.begin();
  u8g2.clearBuffer();

#ifdef ROTARY_ENCODER_INPUT
  // Initialise encoder positions
  dial_pos = 32768;
  last_dial_pos = 32768;

  // Attach hardware interrupt for rotary encoder
  attachInterrupt(previous_pin, encoderWrapper, CHANGE);
#endif
}

// Static interrupt callback wrapper
void Menu::encoderWrapper() {
  if (instance) instance->doEncoder();
}

// Handle encoder signal changes
void Menu::doEncoder() {
  int encA = digitalRead(previous_pin);
  int encB = digitalRead(next_pin);

  // Determine rotation direction
  if (encA == 0) {
    if (encB == 1 && encoder_state == 2) {
      encoder_state = 0;
      dial_pos -= 1;
    } else if (encB == 0 && encoder_state == 1) {
      encoder_state = 0;
      dial_pos += 1;
    }
  } else {
    encoder_state = encB == 1 ? 1 : 2;
  }
}

// Handle navigation between menus
// Manipulates the internal menuIndex variable
void Menu::handleButtons() {
  // Update length of scan menu
  xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
  menus[SCAN].menuItemsLength = (SCAN_FREQUENCY_RANGE / settings->scanInterval.get()) + 1;  // +1 for final number inclusion
  xSemaphoreGive(settings->settingsMutex);

#ifdef ROTARY_ENCODER_INPUT
  int selectPressed = !digitalRead(select_pin);
  if (dial_pos != last_dial_pos) {
    if (selectPressed == HIGH) {
      if ((dial_pos - 4) > last_dial_pos) settings->clearReset();  // Reset is pressed and rotated anti-clockwise
    } else {
      // Move through menu
      menus[menuIndex].menuIndex = (menus[menuIndex].menuIndex + (last_dial_pos - dial_pos) + menus[menuIndex].menuItemsLength) % menus[menuIndex].menuItemsLength;

      // Sound buzzer if necessary
      xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
      if (settings->buzzer.get()) buzzer->buzz();
      xSemaphoreGive(settings->settingsMutex);

      last_dial_pos = dial_pos;
    }
  }
#else
  int selectPressed = digitalRead(select_pin);
  int prevPressed = digitalRead(previous_pin);
  int nextPressed = digitalRead(next_pin);

  // Hidden reset function
  if (prevPressed == HIGH && selectPressed == HIGH && nextPressed == HIGH) {
    settings->clearReset();
  }

  // Toggle monitor mode with PREV+SELECT on scan screen
  if (menuIndex == SCAN && prevPressed == HIGH && selectPressed == HIGH && nextPressed == LOW) {
    receiver->monitorMode = !receiver->monitorMode;
    if (receiver->monitorMode) receiver->monitorIndex = menus[SCAN].menuIndex;
    xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
    if (settings->buzzer.get()) buzzer->doubleBuzz();
    xSemaphoreGive(settings->settingsMutex);
    selectButtonPressTime = 0;
    selectButtonHeld = false;
    delay(DEBOUNCE_DELAY * 2);
    return;
  }

  // Toggle marker overlay with PREV+NEXT simultaneously on scan screen
  if (menuIndex == SCAN && prevPressed == HIGH && nextPressed == HIGH) {
    showMarkers = !showMarkers;
    xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
    if (settings->buzzer.get()) buzzer->buzz();
    xSemaphoreGive(settings->settingsMutex);
    delay(DEBOUNCE_DELAY * 2);
    return;
  }

  // Move between menu items
  if (nextPressed == HIGH || prevPressed == HIGH) {
    int direction = (nextPressed == HIGH) ? 1 : -1;
    menus[menuIndex].menuIndex = (menus[menuIndex].menuIndex + direction + menus[menuIndex].menuItemsLength) % menus[menuIndex].menuItemsLength;

    // Sync monitor index when cursor moves on scan screen in monitor mode
    if (menuIndex == SCAN && receiver->monitorMode) {
      receiver->monitorIndex = menus[SCAN].menuIndex;
    }

    // Sound buzzer on button press if necessary
    xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
    if (settings->buzzer.get()) buzzer->buzz();
    xSemaphoreGive(settings->settingsMutex);

    // Delay for button debouncing
    delay(DEBOUNCE_DELAY);
  }
#endif

  // Handle pressing and holding SELECT to go back
  if (selectPressed == HIGH) {
    if (selectButtonPressTime == 0) {  // Button just pressed so record time
      selectButtonPressTime = millis();

      // Sound buzzer on button press if necessary
      xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
      if (settings->buzzer.get()) buzzer->buzz();
      xSemaphoreGive(settings->settingsMutex);
    } else if (!selectButtonHeld && millis() - selectButtonPressTime > LONG_PRESS_DURATION) {  // Held longer than threshold register long press
      switch (menuIndex) {
        case MAIN: menuIndex = ADVANCED; break;                          // If on main menu, go to advanced
        case SCAN_INTERVAL ... MARKERS: menuIndex = SETTINGS; break;    // If on individual settings menu, go to settings
        case CALIBRATION ... USB_SERIAL: menuIndex = ADVANCED; break;   // If on individual advanced menu, go to advanced
        default: menuIndex = MAIN; break;                                   // Otherwise, go back to main menu
      }

      selectButtonHeld = true;

      // Sound double buzz on back if necessary
      xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
      if (settings->buzzer.get()) buzzer->doubleBuzz();
      xSemaphoreGive(settings->settingsMutex);
    }

    // Delay for button debouncing
    delay(DEBOUNCE_DELAY);

    // Immediately end and wait for next iteration of loop()
    return;
  }

  // If SELECT button was pressed but not held, use as SELECT rather than BACK
  if (selectButtonPressTime > 0 && !selectButtonHeld) {
    switch (menuIndex) {
      case MAIN:  // Handle SELECT on main menu
        switch (menus[MAIN].menuIndex) {
          case 0: menuIndex = SCAN; break;      // Go to scan menu
          case 1: menuIndex = SETTINGS; break;  // Go to settings menu
          case 2: menuIndex = ABOUT; break;     // Go to about menu
        }
        break;
      case SCAN:  // Handle SELECT on scan menu
        xSemaphoreTake(receiver->lowbandMutex, portMAX_DELAY);
        receiver->lowband.set(!receiver->lowband.get());
        xSemaphoreGive(receiver->lowbandMutex);
        break;
      case SETTINGS:  // Handle SELECT on settings menu
        switch (menus[SETTINGS].menuIndex) {
          case 0: menuIndex = SCAN_INTERVAL; break;  // Go to scan interval menu
          case 1: menuIndex = BUZZER; break;         // Go to buzzer menu
#ifdef BATTERY_MONITORING
          case 2: menuIndex = BATTERY_ALARM; break;  // Go to battery alarm menu
          case 3: menuIndex = MARKERS; break;        // Go to markers menu
#else
          case 2: menuIndex = MARKERS; break;        // Go to markers menu
#endif
        }
        break;
      case ADVANCED:  // Handle SELECT on advanced menu
        switch (menus[ADVANCED].menuIndex) {
          case 0: menuIndex = CALIBRATION; break;  // Go to calibration menu
          case 1: menuIndex = WIFI; break;         // Go to Wi-Fi menu
          case 2: menuIndex = USB_SERIAL; break;   // Go to serial menu
        }
        break;
      case SCAN_INTERVAL ... MARKERS:  // Handle SELECT on individual settings options
        switch (menuIndex) {
          case SCAN_INTERVAL:  // Update scan interval setting
            xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
            settings->scanIntervalIndex.set(menus[SCAN_INTERVAL].menuIndex);
            xSemaphoreGive(settings->settingsMutex);
            menus[SCAN].menuIndex = 0;
            break;
          case BUZZER:  // Update buzzer setting
            xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
            settings->buzzerIndex.set(menus[BUZZER].menuIndex);
            xSemaphoreGive(settings->settingsMutex);
            break;
          case BATTERY_ALARM:  // Update battery alarm setting
            xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
            settings->batteryAlarmIndex.set(menus[BATTERY_ALARM].menuIndex);
            xSemaphoreGive(settings->settingsMutex);
            break;
          case MARKERS:  // Update marker count setting
            xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
            settings->markerCountIndex.set(menus[MARKERS].menuIndex);
            xSemaphoreGive(settings->settingsMutex);
            break;
        }
        break;
      case CALIBRATION:  // Handle SELECT on calibration menu
        switch (menus[CALIBRATION].menuIndex) {
          case 0: receiver->calibrate(true); break;   // Calibrate high rssi
          case 1: receiver->calibrate(false); break;  // Calibrate low rssi
        }
        break;
    }
  }

  // Reset SELECT when button released
  selectButtonPressTime = 0;
  selectButtonHeld = false;
}

// Clear display buffer
void Menu::clearBuffer() {
  u8g2.clearBuffer();
}

// Send data to display buffer
void Menu::sendBuffer() {
  u8g2.sendBuffer();
}

// Draw current menu
void Menu::drawMenu() {
  // Draw title, but not for scan menu
  if (menuIndex != SCAN) {
    u8g2.setFont(u8g2_font_8x13B_tf);
    const char *title = menus[menuIndex].title;
    u8g2.drawStr(textCentreX(title, 8), 13, title);
    u8g2.setFont(u8g2_font_7x13_tf);
  }

  // Update in-memory icons for individual settings options
  if (menuIndex >= SCAN_INTERVAL && menuIndex <= MARKERS) {
    xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
    updateSettingsOptionIcons(&menus[SCAN_INTERVAL], settings->scanIntervalIndex.get());
    updateSettingsOptionIcons(&menus[BUZZER], settings->buzzerIndex.get());
    updateSettingsOptionIcons(&menus[BATTERY_ALARM], settings->batteryAlarmIndex.get());
    updateSettingsOptionIcons(&menus[MARKERS], settings->markerCountIndex.get());
    xSemaphoreGive(settings->settingsMutex);
  }

  // Call appropriate draw function
  switch (menuIndex) {
    case SCAN:  // Draw scan menu
      receiver->startScan();
      drawScanMenu();
      break;
    case ABOUT:  // Draw about menu
      drawAboutMenu();
      break;
    case WIFI:  // Draw Wi-Fi menu
      receiver->startScan();
      api->startWifi();
      drawWifiMenu();
      break;
    case USB_SERIAL:  // Draw serial menu
      receiver->startScan();
      usb->listen();
      drawSerialMenu();
      break;
    default:  // Draw selection menu with options
      receiver->stopScan();
      api->stopWifi();
      usb->flushIncoming();
      drawSelectionMenu();
      break;
  }
}

// Display battery voltage in bottom corner of main menu
void Menu::drawBatteryVoltage(int voltage) {
  // Draw voltage only display if on main menu
  if (menuIndex == MAIN) {
    // Format voltage reading
    char formattedVoltage[5];
    snprintf(formattedVoltage, sizeof(formattedVoltage), "%d.%dv", voltage / 10, voltage % 10);

    // Set font colour to inverted if selected bottom item
    u8g2.setDrawColor(menus[MAIN].menuIndex == 2 ? 0 : 1);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(109, DISPLAY_HEIGHT, formattedVoltage);
    u8g2.setDrawColor(1);
  }
}

// Generic function for drawing menus with multiple options
void Menu::drawSelectionMenu() {
  int totalItems = menus[menuIndex].menuItemsLength;
  int selectedIdx = menus[menuIndex].menuIndex;

  // Calculate scroll offset: show 3 items at a time, keep selection in view
  int startIndex = max(0, min(selectedIdx - 1, totalItems - 3));

  for (int i = startIndex; i < min(startIndex + 3, totalItems); i++) {
    int drawPos = i - startIndex;
    if (i == selectedIdx) {
      u8g2.drawBox(0, 16 + (drawPos * 16), DISPLAY_WIDTH, 16);
      u8g2.setDrawColor(0);
      u8g2.drawXBMP(10, 17 + (drawPos * 16), 14, 14, menus[menuIndex].menuItems[i].icon);
      u8g2.drawStr(30, 28 + (drawPos * 16), menus[menuIndex].menuItems[i].name);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawXBMP(10, 17 + (drawPos * 16), 14, 14, menus[menuIndex].menuItems[i].icon);
      u8g2.drawStr(30, 28 + (drawPos * 16), menus[menuIndex].menuItems[i].name);
    }
  }

  // Draw extra text for calibration menu
  if (menuIndex == CALIBRATION) {
    u8g2.setFont(u8g2_font_5x7_tf);
    const char *text = "Set to 5800MHz (F4)";
    u8g2.drawStr(textCentreX(text, 5), 60, text);
  }
}

// Draw graph of scanned rssi values
void Menu::drawScanMenu() {
  // Get scan parameters
  xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
  float interval = settings->scanInterval.get();
  int minRssi = settings->lowCalibratedRssi.get();
  int maxRssi = settings->highCalibratedRssi.get();
  xSemaphoreGive(settings->settingsMutex);
  int numScannedValues = (SCAN_FREQUENCY_RANGE / interval) + 1;

  // Calculate bar dimensions
  int barWidth = 1;
  while ((barWidth + 1) * numScannedValues <= DISPLAY_WIDTH) barWidth++;
  int padding = (int)floor((DISPLAY_WIDTH - (barWidth * numScannedValues)) / 2);

  // Get lowband state
  xSemaphoreTake(receiver->lowbandMutex, portMAX_DELAY);
  bool lowband = receiver->lowband.get();
  xSemaphoreGive(receiver->lowbandMutex);
  int min_freq = lowband ? LOWBAND_MIN_FREQUENCY : HIGHBAND_MIN_FREQUENCY;

  // Read all RSSI values at once for a consistent display frame
  int barHeights[MAX_FREQUENCIES_SCANNED];
  int cursorRssi;
  xSemaphoreTake(receiver->scanMutex, portMAX_DELAY);
  for (int i = 0; i < numScannedValues; i++) {
    int rssi = std::clamp(receiver->rssiValues.get(i), minRssi, maxRssi);
    barHeights[i] = map(rssi, minRssi, maxRssi, 0, BAR_Y_MAX - BAR_Y_MIN);
  }
  cursorRssi = receiver->rssiValues.get(menus[SCAN].menuIndex);
  xSemaphoreGive(receiver->scanMutex);

  // Collect marker info
  int markerCount = 0;
  int markerIndices[MAX_MARKERS];
  int markerFreqs[MAX_MARKERS];
  bool cursorOnMarker = false;
  for (int m = 0; m < MAX_MARKERS; m++) markerIndices[m] = -1;

  if (showMarkers) {
    xSemaphoreTake(settings->settingsMutex, portMAX_DELAY);
    markerCount = settings->markerCount.get();
    xSemaphoreGive(settings->settingsMutex);
    if (markerCount > 0) {
      xSemaphoreTake(receiver->markersMutex, portMAX_DELAY);
      for (int m = 0; m < markerCount; m++) {
        markerIndices[m] = receiver->markers[m].index;
        markerFreqs[m] = (markerIndices[m] >= 0)
          ? (int)round(markerIndices[m] * interval + min_freq)
          : -1;
        if (markerIndices[m] == menus[SCAN].menuIndex) cursorOnMarker = true;
      }
      xSemaphoreGive(receiver->markersMutex);
    }
  }

  // Bottom row: marker frequencies when active, otherwise band reference labels
  u8g2.setFont(u8g2_font_5x7_tf);
  if (showMarkers && markerCount > 0) {
    for (int m = 0; m < markerCount; m++) {
      if (markerFreqs[m] < 0) continue;
      char label[12];
      if (markerCount <= 2) {
        snprintf(label, sizeof(label), "M%d:%dMHz", m + 1, markerFreqs[m]);
      } else {
        snprintf(label, sizeof(label), "M%d:%d", m + 1, markerFreqs[m]);
      }
      int x = m * DISPLAY_WIDTH / markerCount;
      x = min(x, (int)(DISPLAY_WIDTH - (int)strlen(label) * 5));
      u8g2.drawStr(x, DISPLAY_HEIGHT, label);
    }
  } else {
    if (lowband) {
      u8g2.drawStr(0, DISPLAY_HEIGHT, "5345");
      u8g2.drawStr(55, DISPLAY_HEIGHT, "5495");
      u8g2.drawStr(109, DISPLAY_HEIGHT, "5645");
    } else {
      u8g2.drawStr(0, DISPLAY_HEIGHT, "5645");
      u8g2.drawStr(55, DISPLAY_HEIGHT, "5795");
      u8g2.drawStr(109, DISPLAY_HEIGHT, "5945");
    }
  }

  // Header left: band or monitor label
  u8g2.setFont(u8g2_font_7x13_tf);
  if (receiver->monitorMode) u8g2.drawStr(0, 13, "MON");
  else if (lowband)          u8g2.drawStr(0, 13, "LOW");
  else                       u8g2.drawStr(0, 13, "HIGH");

  // Header center: cursor frequency (* when on a marker)
  char currentFrequency[9];
  int cursorFreq = (int)round(menus[SCAN].menuIndex * interval + min_freq);
  if (cursorOnMarker)
    snprintf(currentFrequency, sizeof(currentFrequency), "*%dMHz", cursorFreq);
  else
    snprintf(currentFrequency, sizeof(currentFrequency), "%dMHz", cursorFreq);
  u8g2.drawStr(textCentreX(currentFrequency, 7), 13, currentFrequency);

  // Header right: cursor RSSI%
  cursorRssi = std::clamp(cursorRssi, minRssi, maxRssi);
  char percentageStr[5];
  snprintf(percentageStr, sizeof(percentageStr), "%d%%", map(cursorRssi, minRssi, maxRssi, 0, 100));
  u8g2.drawStr(DISPLAY_WIDTH - (strlen(percentageStr) * 7) + 1, 13, percentageStr);

  // Draw spectrum bars (original box-per-bar style)
  for (int i = 0; i < numScannedValues; i++) {
    int h = barHeights[i];
    int x = i * barWidth + padding;
    bool isSelected = (i == menus[SCAN].menuIndex);

    if (isSelected) {
      u8g2.drawBox(x, BAR_Y_MIN, barWidth, BAR_Y_MAX - BAR_Y_MIN);
      u8g2.setDrawColor(0);
      u8g2.drawBox(x, BAR_Y_MAX - h, barWidth, h);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawBox(x, BAR_Y_MAX - h, barWidth, h);
    }
  }

  // Draw marker cap indicators (XOR so visible on both filled and empty bars)
  if (showMarkers && markerCount > 0) {
    u8g2.setDrawColor(2);
    for (int m = 0; m < markerCount; m++) {
      int mIdx = markerIndices[m];
      if (mIdx >= 0 && mIdx < numScannedValues) {
        u8g2.drawBox(mIdx * barWidth + padding, BAR_Y_MIN, barWidth, 2);
      }
    }
    u8g2.setDrawColor(1);
  }
}

// Draw static content on about menu
void Menu::drawAboutMenu() {
  const char *info = "5.8GHz scanner";
  u8g2.drawStr(textCentreX(info, 7), 28, info);

  u8g2.drawStr(textCentreX(VERSION, 7), 44, VERSION);

  u8g2.drawStr(textCentreX(AUTHOR, 7), 60, AUTHOR);
}

// Draw static content on Wi-Fi menu
void Menu::drawWifiMenu() {
  // Draw SSID
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(11, 28, "ID");
  u8g2.setFont(u8g2_font_7x13_tf);
  u8g2.drawStr(30, 28, WIFI_SSID);

  // Draw password
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(4, 44, "PWD");
  u8g2.setFont(u8g2_font_7x13_tf);
  u8g2.drawStr(30, 44, WIFI_PASSWORD);

  // Draw IP
  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(11, 60, "IP");
  if (strlen(WIFI_IP) < 15) {  // If not 15 characters use regular font
    u8g2.setFont(u8g2_font_7x13_tf);
    u8g2.drawStr(30, 60, WIFI_IP);
  } else {  // If 15 characters use smaller font, otherwise last digit off screen
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(30, 59, WIFI_IP);
  }
}

// Draw static content on serial menu
void Menu::drawSerialMenu() {
  char baudString[13];
  snprintf(baudString, sizeof(baudString), "%d Baud", USB_SERIAL_BAUD);
  u8g2.drawStr(textCentreX(baudString, 7), 28, baudString);

  const char *info = "Use client program";
  u8g2.drawStr(textCentreX(info, 7), 44, info);
}

// Update icons for selected settings options
void Menu::updateSettingsOptionIcons(menuStruct *menu, int selectedIndex) {
  for (int i = 0; i < menu->menuItemsLength; i++) {
    if (i == selectedIndex) {
      menu->menuItems[i].icon = bitmap_Selected;
    } else {
      menu->menuItems[i].icon = bitmap_Blank;
    }
  }
}

// Initialise menu structures
void Menu::initMenus() {
  // Main menu
  mainMenuItems[0] = { "Scan", bitmap_Scan };
  mainMenuItems[1] = { "Settings", bitmap_Settings };
  mainMenuItems[2] = { "About", bitmap_About };

  // Settings menu
  settingsMenuItems[0] = { "Scan interval", bitmap_Interval };
  settingsMenuItems[1] = { "Buzzer", bitmap_Buzzer };
  settingsMenuItems[2] = { "Bat. alarm", bitmap_Alarm };
  settingsMenuItems[3] = { "Markers", bitmap_Calibration };

  // Markers menu
  markersMenuItems[0] = { "Off", bitmap_Blank };
  markersMenuItems[1] = { "1", bitmap_Blank };
  markersMenuItems[2] = { "2", bitmap_Blank };
  markersMenuItems[3] = { "3", bitmap_Blank };
  markersMenuItems[4] = { "4", bitmap_Blank };

  // Scan Interval menu
  scanIntervalMenuItems[0] = { "2.5MHz", bitmap_Blank };
  scanIntervalMenuItems[1] = { "5MHz", bitmap_Blank };
  scanIntervalMenuItems[2] = { "10MHz", bitmap_Blank };

  // Buzzer menu
  buzzerMenuItems[0] = { "On", bitmap_Blank };
  buzzerMenuItems[1] = { "Off", bitmap_Blank };

  // Battery Alarm menu
  batteryAlarmMenuItems[0] = { "3.6v", bitmap_Blank };
  batteryAlarmMenuItems[1] = { "3.3v", bitmap_Blank };
  batteryAlarmMenuItems[2] = { "3.0v", bitmap_Blank };

  // Advanced menu
  advancedMenuItems[0] = { "Calibration", bitmap_Calibration };
  advancedMenuItems[1] = { "Wi-Fi", bitmap_Wifi };
  advancedMenuItems[2] = { "USB Serial", bitmap_Serial };

  // Calibration menu
  calibrationMenuItems[0] = { "Calib. high", bitmap_Wifi };
  calibrationMenuItems[1] = { "Calib. low", bitmap_WifiLow };

  // Settings menu length varies by build configuration
#ifdef BATTERY_MONITORING
  int settingsLength = 4;
#else
  int settingsLength = 3;
#endif

  // Menus
  menus[MAIN] = { "Hertz Hunter", mainMenuItems, 3, 0 };
  menus[SCAN] = { "Scan", nullptr, MAX_FREQUENCIES_SCANNED, 0 };
  menus[SETTINGS] = { "Settings", settingsMenuItems, settingsLength, 0 };
  menus[ABOUT] = { "About", nullptr, 1, 0 };
  menus[ADVANCED] = { "Advanced", advancedMenuItems, 3, 0 };
  menus[SCAN_INTERVAL] = { "Scan interval", scanIntervalMenuItems, 3, 0 };
  menus[BUZZER] = { "Buzzer", buzzerMenuItems, 2, 0 };
  menus[BATTERY_ALARM] = { "Bat. alarm", batteryAlarmMenuItems, 3, 0 };
  menus[MARKERS] = { "Markers", markersMenuItems, 5, 0 };
  menus[CALIBRATION] = { "Calibration", calibrationMenuItems, 2, 0 };
  menus[WIFI] = { "Wi-Fi", nullptr, 1, 0 };
  menus[USB_SERIAL] = { "USB Serial", nullptr, 1, 0 };
}

// Calculate x position of text to centre it on screen
int Menu::textCentreX(const char *text, int fontCharWidth) {
  // +1 to include blank space pixel on right edge of final character
  return (DISPLAY_WIDTH - (strlen(text) * fontCharWidth)) / 2 + 1;
}
