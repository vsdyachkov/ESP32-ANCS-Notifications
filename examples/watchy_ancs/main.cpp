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
constexpr uint16_t kButtonDebounceMs = 250;
constexpr uint16_t kNotificationVibrationMs = 90;

struct TrackedNotificationState {
  uint32_t uuid = 0;
  uint32_t eventFlags = 0;
  time_t time = 0;
  uint32_t sequence = 0;
  char title[160] = "";
  char message[512] = "";
  char positiveLabel[64] = "";
  char negativeLabel[64] = "";
};

BLENotifications notifications;
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;
SemaphoreHandle_t notificationStateMutex = nullptr;

char statusText[48] = "Advertising. Pair from iPhone.";
char titleText[160] = "No notification yet";
char messageText[512] = "Send a visible iPhone notification after pairing.";
char appText[64] = "";

volatile bool redrawRequested = true;
volatile bool restartAdvertisingRequested = false;
volatile bool vibrationRequested = false;
bool vibrationActive = false;
uint32_t restartAdvertisingAtMs = 0;
uint32_t vibrationStopAtMs = 0;
uint32_t currentNotificationUUID = 0;
bool hasCurrentNotification = false;
TrackedNotificationState trackedNotifications[kMaxTrackedNotifications] = {};
uint8_t trackedNotificationCount = 0;
uint8_t currentNotificationIndex = 0;
uint32_t notificationSequenceCounter = 0;
uint8_t previousButtonMask = 0;
uint32_t lastButtonActionAtMs = 0;

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

int8_t trackedNotificationIndex(uint32_t uuid) {
  for (uint8_t index = 0; index < trackedNotificationCount; index++) {
    if (trackedNotifications[index].uuid == uuid) {
      return static_cast<int8_t>(index);
    }
  }

  return -1;
}

void copyTrackedNotificationToScreen(const TrackedNotificationState &notification) {
  hasCurrentNotification = notification.uuid != 0;
  currentNotificationUUID = notification.uuid;
  copyCString(titleText, sizeof(titleText), notification.title);
  copyCString(messageText, sizeof(messageText), notification.message);
}

void selectTrackedNotification(uint8_t index) {
  if (trackedNotificationCount == 0 || index >= trackedNotificationCount) {
    hasCurrentNotification = false;
    currentNotificationUUID = 0;
    currentNotificationIndex = 0;
    copyCString(titleText, sizeof(titleText), "No notification yet");
    copyCString(messageText, sizeof(messageText), "Send a visible iPhone notification after pairing.");
    return;
  }

  currentNotificationIndex = index + 1;
  copyTrackedNotificationToScreen(trackedNotifications[index]);
}

bool isNewerNotification(const TrackedNotificationState &left, const TrackedNotificationState &right) {
  if (left.time > 0 && right.time > 0 && left.time != right.time) {
    return left.time > right.time;
  }
  return left.sequence > right.sequence;
}

void sortTrackedNotifications() {
  for (uint8_t index = 1; index < trackedNotificationCount; index++) {
    TrackedNotificationState current = trackedNotifications[index];
    int8_t sortedIndex = index - 1;
    while (sortedIndex >= 0 && isNewerNotification(current, trackedNotifications[sortedIndex])) {
      trackedNotifications[sortedIndex + 1] = trackedNotifications[sortedIndex];
      sortedIndex--;
    }
    trackedNotifications[sortedIndex + 1] = current;
  }
}

void storeTrackedNotification(uint8_t index, const ArduinoNotification *notification, bool isNewNotification) {
  trackedNotifications[index].uuid = notification->uuid;
  trackedNotifications[index].eventFlags = notification->eventFlags;
  trackedNotifications[index].time = notification->time;
  if (isNewNotification || trackedNotifications[index].sequence == 0) {
    notificationSequenceCounter++;
    trackedNotifications[index].sequence = notificationSequenceCounter;
  }
  copyString(trackedNotifications[index].title, sizeof(trackedNotifications[index].title), notification->title);
  copyString(trackedNotifications[index].message, sizeof(trackedNotifications[index].message), notification->message);
  copyString(
      trackedNotifications[index].positiveLabel,
      sizeof(trackedNotifications[index].positiveLabel),
      notification->positiveActionLabel);
  copyString(
      trackedNotifications[index].negativeLabel,
      sizeof(trackedNotifications[index].negativeLabel),
      notification->negativeActionLabel);
}

uint8_t upsertTrackedNotification(const ArduinoNotification *notification) {
  int8_t existingIndex = trackedNotificationIndex(notification->uuid);
  if (existingIndex >= 0) {
    uint8_t index = static_cast<uint8_t>(existingIndex);
    storeTrackedNotification(index, notification, false);
    sortTrackedNotifications();
    int8_t sortedIndex = trackedNotificationIndex(notification->uuid);
    return sortedIndex >= 0 ? static_cast<uint8_t>(sortedIndex) : 0;
  }

  if (trackedNotificationCount >= kMaxTrackedNotifications) {
    for (uint8_t index = 1; index < kMaxTrackedNotifications; index++) {
      trackedNotifications[index - 1] = trackedNotifications[index];
    }
    uint8_t index = kMaxTrackedNotifications - 1;
    storeTrackedNotification(index, notification, true);
    sortTrackedNotifications();
    int8_t sortedIndex = trackedNotificationIndex(notification->uuid);
    return sortedIndex >= 0 ? static_cast<uint8_t>(sortedIndex) : 0;
  }

  uint8_t index = trackedNotificationCount;
  storeTrackedNotification(index, notification, true);
  trackedNotificationCount++;
  sortTrackedNotifications();
  int8_t sortedIndex = trackedNotificationIndex(notification->uuid);
  return sortedIndex >= 0 ? static_cast<uint8_t>(sortedIndex) : 0;
}

bool untrackNotification(uint32_t uuid) {
  int8_t existingIndex = trackedNotificationIndex(uuid);
  if (existingIndex < 0) {
    return false;
  }

  for (uint8_t index = static_cast<uint8_t>(existingIndex) + 1; index < trackedNotificationCount; index++) {
    trackedNotifications[index - 1] = trackedNotifications[index];
  }

  trackedNotificationCount--;
  if (trackedNotificationCount < kMaxTrackedNotifications) {
    trackedNotifications[trackedNotificationCount] = TrackedNotificationState();
  }

  if (trackedNotificationCount == 0) {
    selectTrackedNotification(0);
    return true;
  }

  if (currentNotificationUUID == uuid) {
    uint8_t nextIndex = static_cast<uint8_t>(existingIndex);
    if (nextIndex >= trackedNotificationCount) {
      nextIndex = trackedNotificationCount - 1;
    }
    selectTrackedNotification(nextIndex);
  } else {
    int8_t currentIndex = trackedNotificationIndex(currentNotificationUUID);
    currentNotificationIndex = currentIndex >= 0 ? static_cast<uint8_t>(currentIndex + 1) : 0;
  }

  return true;
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
  if (notificationStateMutex != nullptr) {
    xSemaphoreTake(notificationStateMutex, portMAX_DELAY);
  }

  int8_t existingIndex = trackedNotificationIndex(notification->uuid);
  uint8_t storedIndex = upsertTrackedNotification(notification);
  bool shouldRedraw = existingIndex < 0;
  if (shouldRedraw) {
    selectTrackedNotification(storedIndex);
    appText[0] = '\0';
    vibrationRequested = true;
    redrawRequested = true;
  } else if (hasCurrentNotification) {
    int8_t currentIndex = trackedNotificationIndex(currentNotificationUUID);
    currentNotificationIndex = currentIndex >= 0 ? static_cast<uint8_t>(currentIndex + 1) : 0;
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

void printClippedLine(String text, int16_t x, int16_t y, int16_t maxWidth, bool rightAlign) {
  text = normalizedForDisplay(text);
  uint16_t length = fittingUtf8Length(text, maxWidth);
  text = text.substring(0, length);
  text.trim();
  if (text.length() == 0) {
    return;
  }

  int16_t cursorX = x;
  if (rightAlign) {
    cursorX = x + maxWidth - u8g2Fonts.getUTF8Width(text.c_str());
  }
  u8g2Fonts.setCursor(cursorX, y);
  u8g2Fonts.print(text);
}

void drawNotificationScreen() {
  char localTitle[sizeof(titleText)];
  char localMessage[sizeof(messageText)];
  char localPositiveLabel[sizeof(trackedNotifications[0].positiveLabel)] = "";
  char localNegativeLabel[sizeof(trackedNotifications[0].negativeLabel)] = "";
  uint32_t localEventFlags = 0;
  uint8_t localNotificationCount = 0;
  uint8_t localNotificationIndex = 0;

  if (notificationStateMutex != nullptr) {
    xSemaphoreTake(notificationStateMutex, portMAX_DELAY);
  }

  copyCString(localTitle, sizeof(localTitle), titleText);
  copyCString(localMessage, sizeof(localMessage), messageText);
  localNotificationCount = trackedNotificationCount;
  localNotificationIndex = currentNotificationIndex;
  if (localNotificationIndex > 0 && localNotificationIndex <= localNotificationCount) {
    const TrackedNotificationState &currentNotification = trackedNotifications[localNotificationIndex - 1];
    localEventFlags = currentNotification.eventFlags;
    copyCString(localPositiveLabel, sizeof(localPositiveLabel), currentNotification.positiveLabel);
    copyCString(localNegativeLabel, sizeof(localNegativeLabel), currentNotification.negativeLabel);
  }
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
  constexpr int16_t actionLabelTop = 184;
  constexpr int16_t messageLineHeight = 18;
  int16_t messageY = titleY + titleLines * titleLineHeight + titleMessageGap;
  uint8_t messageLines =
      actionLabelTop > messageY ? (actionLabelTop - messageY) / messageLineHeight : 1;
  if (messageLines == 0) {
    messageLines = 1;
  }
  printClippedBlock(String(localMessage), 4, messageY, 192, messageLineHeight, messageLines);

  u8g2Fonts.setFont(u8g2_font_6x13_t_cyrillic);
  constexpr int16_t actionLabelY = 198;
  constexpr int16_t actionLabelWidth = 92;
  if ((localEventFlags & ANCS::EventFlagPositiveAction) != 0) {
    printClippedLine(String(localPositiveLabel), 4, actionLabelY, actionLabelWidth, false);
  }
  if ((localEventFlags & ANCS::EventFlagNegativeAction) != 0) {
    printClippedLine(String(localNegativeLabel), 104, actionLabelY, actionLabelWidth, true);
  }

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
}

void onNotificationRemoved(const ArduinoNotification *notification, const Notification *rawNotificationData) {
  (void)rawNotificationData;

  if (notificationStateMutex != nullptr) {
    xSemaphoreTake(notificationStateMutex, portMAX_DELAY);
  }

  untrackNotification(notification->uuid);

  if (notificationStateMutex != nullptr) {
    xSemaphoreGive(notificationStateMutex);
  }
}

void selectRelativeNotification(int8_t delta) {
  if (notificationStateMutex != nullptr) {
    xSemaphoreTake(notificationStateMutex, portMAX_DELAY);
  }

  if (trackedNotificationCount > 1) {
    uint8_t currentIndex = currentNotificationIndex > 0 ? currentNotificationIndex - 1 : 0;
    if (delta < 0) {
      currentIndex = currentIndex == 0 ? trackedNotificationCount - 1 : currentIndex - 1;
    } else {
      currentIndex = currentIndex + 1 >= trackedNotificationCount ? 0 : currentIndex + 1;
    }
    selectTrackedNotification(currentIndex);
    redrawRequested = true;
  }

  if (notificationStateMutex != nullptr) {
    xSemaphoreGive(notificationStateMutex);
  }
}

void performCurrentNotificationAction(bool positiveAction) {
  uint32_t uuid = 0;
  bool actionAvailable = false;

  if (notificationStateMutex != nullptr) {
    xSemaphoreTake(notificationStateMutex, portMAX_DELAY);
  }

  if (currentNotificationIndex > 0 && currentNotificationIndex <= trackedNotificationCount) {
    const TrackedNotificationState &currentNotification = trackedNotifications[currentNotificationIndex - 1];
    uuid = currentNotification.uuid;
    uint32_t actionFlag = positiveAction ? ANCS::EventFlagPositiveAction : ANCS::EventFlagNegativeAction;
    actionAvailable = (currentNotification.eventFlags & actionFlag) != 0;
  }

  if (notificationStateMutex != nullptr) {
    xSemaphoreGive(notificationStateMutex);
  }

  if (uuid == 0 || !actionAvailable) {
    return;
  }

  if (positiveAction) {
    notifications.actionPositive(uuid);
  } else {
    notifications.actionNegative(uuid);
  }
}

uint8_t readButtonMask() {
  uint8_t buttonMask = 0;
  if (digitalRead(MENU_BTN_PIN) == LOW) {
    buttonMask |= 0x01;
  }
  if (digitalRead(BACK_BTN_PIN) == LOW) {
    buttonMask |= 0x02;
  }
  if (digitalRead(UP_BTN_PIN) == LOW) {
    buttonMask |= 0x04;
  }
  if (digitalRead(DOWN_BTN_PIN) == LOW) {
    buttonMask |= 0x08;
  }
  return buttonMask;
}

void handleButtonPresses() {
  uint8_t currentButtonMask = readButtonMask();
  uint8_t pressedButtons = currentButtonMask & ~previousButtonMask;
  previousButtonMask = currentButtonMask;
  if (pressedButtons == 0) {
    return;
  }

  uint32_t now = millis();
  if (now - lastButtonActionAtMs < kButtonDebounceMs) {
    return;
  }
  lastButtonActionAtMs = now;

  if ((pressedButtons & 0x01) != 0) {
    performCurrentNotificationAction(true);
  } else if ((pressedButtons & 0x02) != 0) {
    selectRelativeNotification(-1);
  } else if ((pressedButtons & 0x04) != 0) {
    selectRelativeNotification(1);
  } else if ((pressedButtons & 0x08) != 0) {
    performCurrentNotificationAction(false);
  }
}

void setupButtons() {
  pinMode(MENU_BTN_PIN, INPUT_PULLUP);
  pinMode(BACK_BTN_PIN, INPUT_PULLUP);
  pinMode(UP_BTN_PIN, INPUT_PULLUP);
  pinMode(DOWN_BTN_PIN, INPUT_PULLUP);
}

void setupVibration() {
  pinMode(VIB_MOTOR_PIN, OUTPUT);
  digitalWrite(VIB_MOTOR_PIN, LOW);
}

void handleVibration() {
  uint32_t now = millis();
  if (vibrationActive && static_cast<int32_t>(now - vibrationStopAtMs) >= 0) {
    digitalWrite(VIB_MOTOR_PIN, LOW);
    vibrationActive = false;
  }

  if (!vibrationActive && vibrationRequested) {
    vibrationRequested = false;
    vibrationActive = true;
    vibrationStopAtMs = now + kNotificationVibrationMs;
    digitalWrite(VIB_MOTOR_PIN, HIGH);
  }
}

}  // namespace

void setup() {
  delay(300);

  notificationStateMutex = xSemaphoreCreateMutex();

  Wire.begin(SDA, SCL);
  setupButtons();
  setupVibration();
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

  handleVibration();

  if (redrawRequested && !vibrationActive && !vibrationRequested) {
    drawNotificationScreen();
  }

  handleButtonPresses();

  delay(20);
}
