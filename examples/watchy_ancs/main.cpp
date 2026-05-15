#include <Arduino.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Watchy.h>
#include <Wire.h>

#include "esp32notifications.h"

namespace {

constexpr char kDeviceName[] = "Watchy ANCS 2";
constexpr uint16_t kScreenWidth = 200;
constexpr uint16_t kScreenHeight = 200;
constexpr uint8_t kMaxTrackedNotifications = 32;

BLENotifications notifications;
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;
SemaphoreHandle_t notificationStateMutex = nullptr;

char statusText[48] = "Advertising. Pair from iPhone.";
char titleText[160] = "No notification yet";
char messageText[512] = "Send a visible iPhone notification after pairing.";
char appText[64] = "";

volatile bool redrawRequested = true;
volatile bool restartAdvertisingRequested = false;
uint32_t restartAdvertisingAtMs = 0;
uint32_t incomingCallNotificationUUID = 0;
uint32_t currentNotificationUUID = 0;
bool hasCurrentNotification = false;
uint32_t trackedNotificationUUIDs[kMaxTrackedNotifications] = {};
uint8_t trackedNotificationCount = 0;
uint8_t currentNotificationIndex = 0;

int8_t trackedNotificationIndex(uint32_t uuid) {
  for (uint8_t index = 0; index < trackedNotificationCount; index++) {
    if (trackedNotificationUUIDs[index] == uuid) {
      return static_cast<int8_t>(index);
    }
  }

  return -1;
}

void trackNotification(uint32_t uuid) {
  int8_t existingIndex = trackedNotificationIndex(uuid);
  if (existingIndex >= 0) {
    currentNotificationIndex = static_cast<uint8_t>(existingIndex + 1);
    return;
  }

  if (trackedNotificationCount >= kMaxTrackedNotifications) {
    for (uint8_t index = 1; index < kMaxTrackedNotifications; index++) {
      trackedNotificationUUIDs[index - 1] = trackedNotificationUUIDs[index];
    }
    trackedNotificationUUIDs[kMaxTrackedNotifications - 1] = uuid;
    currentNotificationIndex = kMaxTrackedNotifications;
    return;
  }

  trackedNotificationUUIDs[trackedNotificationCount] = uuid;
  trackedNotificationCount++;
  currentNotificationIndex = trackedNotificationCount;
}

bool untrackNotification(uint32_t uuid) {
  int8_t existingIndex = trackedNotificationIndex(uuid);
  if (existingIndex < 0) {
    return false;
  }

  for (uint8_t index = static_cast<uint8_t>(existingIndex) + 1; index < trackedNotificationCount; index++) {
    trackedNotificationUUIDs[index - 1] = trackedNotificationUUIDs[index];
  }

  trackedNotificationCount--;
  if (trackedNotificationCount < kMaxTrackedNotifications) {
    trackedNotificationUUIDs[trackedNotificationCount] = 0;
  }

  if (hasCurrentNotification) {
    int8_t currentIndex = trackedNotificationIndex(currentNotificationUUID);
    currentNotificationIndex = currentIndex >= 0 ? static_cast<uint8_t>(currentIndex + 1) : 0;
  } else {
    currentNotificationIndex = 0;
  }

  return true;
}

void copyString(char *destination, size_t destinationSize, const String &source) {
  if (destinationSize == 0) {
    return;
  }

  source.toCharArray(destination, destinationSize);
  destination[destinationSize - 1] = '\0';

  size_t used = strlen(destination);
  if (used == 0) {
    return;
  }

  size_t firstByte = used - 1;
  while (firstByte > 0 && (static_cast<uint8_t>(destination[firstByte]) & 0xC0) == 0x80) {
    firstByte--;
  }

  uint8_t leadByte = static_cast<uint8_t>(destination[firstByte]);
  uint8_t expectedLength = 1;
  if ((leadByte & 0xE0) == 0xC0) {
    expectedLength = 2;
  } else if ((leadByte & 0xF0) == 0xE0) {
    expectedLength = 3;
  } else if ((leadByte & 0xF8) == 0xF0) {
    expectedLength = 4;
  } else if (leadByte >= 0x80) {
    destination[firstByte] = '\0';
    return;
  }

  if (firstByte + expectedLength > used) {
    destination[firstByte] = '\0';
  }
}

void copyCString(char *destination, size_t destinationSize, const char *source) {
  if (destinationSize == 0) {
    return;
  }

  strncpy(destination, source, destinationSize - 1);
  destination[destinationSize - 1] = '\0';
}

void updateState(const char *status, const String *title, const String *message, const String *app) {
  if (notificationStateMutex != nullptr) {
    xSemaphoreTake(notificationStateMutex, portMAX_DELAY);
  }

  if (status != nullptr) {
    copyCString(statusText, sizeof(statusText), status);
  }
  if (title != nullptr) {
    copyString(titleText, sizeof(titleText), *title);
  }
  if (message != nullptr) {
    copyString(messageText, sizeof(messageText), *message);
  }
  if (app != nullptr) {
    copyString(appText, sizeof(appText), *app);
  }

  redrawRequested = true;

  if (notificationStateMutex != nullptr) {
    xSemaphoreGive(notificationStateMutex);
  }
}

bool updateNotificationIfNew(const ArduinoNotification *notification) {
  char newTitle[sizeof(titleText)];
  char newMessage[sizeof(messageText)];
  copyString(newTitle, sizeof(newTitle), notification->title);
  copyString(newMessage, sizeof(newMessage), notification->message);

  if (notificationStateMutex != nullptr) {
    xSemaphoreTake(notificationStateMutex, portMAX_DELAY);
  }

  bool shouldRedraw = !hasCurrentNotification || currentNotificationUUID != notification->uuid ||
                      strcmp(titleText, newTitle) != 0 || strcmp(messageText, newMessage) != 0;
  trackNotification(notification->uuid);
  if (shouldRedraw) {
    hasCurrentNotification = true;
    currentNotificationUUID = notification->uuid;
    copyCString(titleText, sizeof(titleText), newTitle);
    copyCString(messageText, sizeof(messageText), newMessage);
    appText[0] = '\0';
    redrawRequested = true;
  }

  if (notificationStateMutex != nullptr) {
    xSemaphoreGive(notificationStateMutex);
  }

  return shouldRedraw;
}

uint8_t utf8CharLength(uint8_t leadByte) {
  if (leadByte < 0x80) {
    return 1;
  }
  if ((leadByte & 0xE0) == 0xC0) {
    return 2;
  }
  if ((leadByte & 0xF0) == 0xE0) {
    return 3;
  }
  if ((leadByte & 0xF8) == 0xF0) {
    return 4;
  }
  return 0;
}

bool isUtf8Continuation(uint8_t value) {
  return (value & 0xC0) == 0x80;
}

bool hasValidUtf8Continuation(const String &text, uint16_t start, uint8_t length) {
  if (length == 0 || start + length > text.length()) {
    return false;
  }

  for (uint8_t offset = 1; offset < length; offset++) {
    if (!isUtf8Continuation(static_cast<uint8_t>(text.charAt(start + offset)))) {
      return false;
    }
  }

  return true;
}

uint16_t nextUtf8Index(const String &text, uint16_t index) {
  if (index >= text.length()) {
    return index;
  }

  uint8_t length = utf8CharLength(static_cast<uint8_t>(text.charAt(index)));
  if (!hasValidUtf8Continuation(text, index, length)) {
    return index + 1;
  }

  return index + length;
}

int16_t textWidth(const String &text) {
  return u8g2Fonts.getUTF8Width(text.c_str());
}

String normalizedForDisplay(String text) {
  String normalized;
  normalized.reserve(text.length());

  bool previousWasSpace = false;
  for (uint16_t index = 0; index < text.length();) {
    uint8_t character = static_cast<uint8_t>(text.charAt(index));
    if (character < 32 || character == 127) {
      if (!previousWasSpace) {
        normalized += ' ';
      }
      previousWasSpace = true;
      index++;
      continue;
    }

    if (character < 0x80) {
      if (character == ' ') {
        if (!previousWasSpace) {
          normalized += ' ';
        }
        previousWasSpace = true;
      } else {
        normalized += static_cast<char>(character);
        previousWasSpace = false;
      }
      index++;
      continue;
    }

    uint8_t length = utf8CharLength(character);
    if (!hasValidUtf8Continuation(text, index, length)) {
      if (!previousWasSpace) {
        normalized += ' ';
      }
      previousWasSpace = true;
      index++;
    } else {
      normalized += text.substring(index, index + length);
      previousWasSpace = false;
      index += length;
    }
  }

  normalized.trim();
  return normalized;
}

uint16_t fittingUtf8Length(const String &text, int16_t maxWidth) {
  uint16_t length = 0;
  while (length < text.length()) {
    uint16_t nextLength = nextUtf8Index(text, length);
    String candidate = text.substring(0, nextLength);
    if (textWidth(candidate) > maxWidth) {
      break;
    }
    length = nextLength;
  }

  return length;
}

uint16_t wrappedLineLength(const String &text, int16_t maxWidth) {
  uint16_t length = 0;
  uint16_t lastSpace = 0;

  while (length < text.length()) {
    uint16_t nextLength = nextUtf8Index(text, length);
    String candidate = text.substring(0, nextLength);
    if (textWidth(candidate) > maxWidth) {
      break;
    }

    if (text.charAt(length) == ' ') {
      lastSpace = length;
    }
    length = nextLength;
  }

  if (length == text.length()) {
    return length;
  }

  if (lastSpace > 0) {
    return lastSpace;
  }

  return fittingUtf8Length(text, maxWidth);
}

uint8_t printClippedBlock(String text, int16_t x, int16_t y, int16_t maxWidth, int16_t lineHeight, uint8_t maxLines) {
  text = normalizedForDisplay(text);
  uint8_t line = 0;
  while (text.length() > 0 && line < maxLines) {
    uint16_t length = wrappedLineLength(text, maxWidth);
    if (length == 0) {
      break;
    }

    String currentLine = text.substring(0, length);
    currentLine.trim();
    u8g2Fonts.setCursor(x, y + line * lineHeight);
    u8g2Fonts.print(currentLine);

    text = text.substring(length);
    text.trim();
    line++;
  }

  return line;
}

void drawNotificationScreen() {
  char localTitle[sizeof(titleText)];
  char localMessage[sizeof(messageText)];
  uint8_t localNotificationCount = 0;
  uint8_t localNotificationIndex = 0;

  if (notificationStateMutex != nullptr) {
    xSemaphoreTake(notificationStateMutex, portMAX_DELAY);
  }

  copyCString(localTitle, sizeof(localTitle), titleText);
  copyCString(localMessage, sizeof(localMessage), messageText);
  localNotificationCount = trackedNotificationCount;
  localNotificationIndex = currentNotificationIndex;
  redrawRequested = false;

  if (notificationStateMutex != nullptr) {
    xSemaphoreGive(notificationStateMutex);
  }

  Watchy::display.setFullWindow();
  Watchy::display.epd2.asyncPowerOn();
  Watchy::display.fillScreen(GxEPD_WHITE);
  Watchy::display.setTextColor(GxEPD_BLACK);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setFontDirection(0);
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

  if (localNotificationCount > 0 && localNotificationIndex > 0) {
    u8g2Fonts.setFont(u8g2_font_6x10_tf);
    String counterText = String(localNotificationIndex) + "/" + String(localNotificationCount);
    int16_t counterX = kScreenWidth - 4 - u8g2Fonts.getUTF8Width(counterText.c_str());
    u8g2Fonts.setCursor(counterX, 10);
    u8g2Fonts.print(counterText);
  }

  u8g2Fonts.setFont(u8g2_font_10x20_t_cyrillic);
  constexpr int16_t titleY = 31;
  constexpr int16_t titleLineHeight = 22;
  constexpr int16_t titleMessageGap = 7;
  uint8_t titleLines = printClippedBlock(String(localTitle), 4, titleY, 192, titleLineHeight, 2);

  u8g2Fonts.setFont(u8g2_font_9x15_t_cyrillic);
  constexpr int16_t messageLineHeight = 18;
  int16_t messageY = titleY + titleLines * titleLineHeight + titleMessageGap;
  uint8_t messageLines =
      kScreenHeight > messageY + 4 ? (kScreenHeight - messageY - 4) / messageLineHeight : 1;
  if (messageLines == 0) {
    messageLines = 1;
  }
  printClippedBlock(String(localMessage), 4, messageY, 192, messageLineHeight, messageLines);

  Watchy::display.display(false);
  Watchy::display.hibernate();
}

void onBLEStateChanged(BLENotifications::State state) {
  switch (state) {
    case BLENotifications::StateConnected:
      break;
    case BLENotifications::StateDisconnected:
      restartAdvertisingRequested = true;
      restartAdvertisingAtMs = millis() + 1000;
      break;
  }
}

void onNotificationArrived(const ArduinoNotification *notification, const Notification *rawNotificationData) {
  (void)rawNotificationData;

  updateNotificationIfNew(notification);

  if (notification->category == CategoryIDIncomingCall) {
    incomingCallNotificationUUID = notification->uuid;
  } else {
    incomingCallNotificationUUID = 0;
  }
}

void onNotificationRemoved(const ArduinoNotification *notification, const Notification *rawNotificationData) {
  (void)rawNotificationData;

  if (notificationStateMutex != nullptr) {
    xSemaphoreTake(notificationStateMutex, portMAX_DELAY);
  }

  bool shouldRedraw = untrackNotification(notification->uuid);
  if (shouldRedraw) {
    redrawRequested = true;
  }

  if (notificationStateMutex != nullptr) {
    xSemaphoreGive(notificationStateMutex);
  }
}

void setupButtons() {
  pinMode(MENU_BTN_PIN, INPUT_PULLUP);
  pinMode(BACK_BTN_PIN, INPUT_PULLUP);
  pinMode(UP_BTN_PIN, INPUT_PULLUP);
  pinMode(DOWN_BTN_PIN, INPUT_PULLUP);
}

}  // namespace

void setup() {
  delay(300);

  notificationStateMutex = xSemaphoreCreateMutex();

  Wire.begin(SDA, SCL);
  setupButtons();
  Watchy::display.epd2.initWatchy();
  u8g2Fonts.begin(Watchy::display);
  drawNotificationScreen();

  notifications.setConnectionStateChangedCallback(onBLEStateChanged);
  notifications.setNotificationCallback(onNotificationArrived);
  notifications.setRemovedCallback(onNotificationRemoved);
  notifications.begin(kDeviceName);
}

void loop() {
  if (restartAdvertisingRequested && static_cast<int32_t>(millis() - restartAdvertisingAtMs) >= 0) {
    restartAdvertisingRequested = false;
    notifications.startAdvertising();
  }

  if (redrawRequested) {
    drawNotificationScreen();
  }

  if (incomingCallNotificationUUID > 0) {
    if (digitalRead(MENU_BTN_PIN) == LOW) {
      notifications.actionPositive(incomingCallNotificationUUID);
      incomingCallNotificationUUID = 0;
    } else if (digitalRead(BACK_BTN_PIN) == LOW) {
      notifications.actionNegative(incomingCallNotificationUUID);
      incomingCallNotificationUUID = 0;
    }
  }

  delay(50);
}
