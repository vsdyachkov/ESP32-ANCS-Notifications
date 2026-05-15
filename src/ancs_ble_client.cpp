// Based on the ANCS work of https://github.com/S-March and the CarWatch project

#include "ble_security.h"
#include "ancs_ble_client.h"
#include "ancs_notification_queue.h"

#include "BLEAddress.h"
#include "BLEDevice.h"
#include "BLEClient.h"
#include "BLEUtils.h"
#include "BLE2902.h"

#include <Arduino.h>

// Fixed service IDs for the Apple ANCS service
const BLEUUID notificationSourceCharacteristicUUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
const BLEUUID controlPointCharacteristicUUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
const BLEUUID dataSourceCharacteristicUUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
const BLEUUID ancsServiceUUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");

static ANCSBLEClient *sharedInstance;

static int parseTwoDigits(const std::string &value, size_t offset)
{
	if (offset + 1 >= value.length())
	{
		return -1;
	}
	if (value[offset] < '0' || value[offset] > '9' || value[offset + 1] < '0' || value[offset + 1] > '9')
	{
		return -1;
	}
	return (value[offset] - '0') * 10 + (value[offset + 1] - '0');
}

static int parseFourDigits(const std::string &value, size_t offset)
{
	int high = parseTwoDigits(value, offset);
	int low = parseTwoDigits(value, offset + 2);
	if (high < 0 || low < 0)
	{
		return -1;
	}
	return high * 100 + low;
}

static time_t parseNotificationDate(const std::string &value)
{
	if (value.length() < 15 || value[8] != 'T')
	{
		return 0;
	}

	int year = parseFourDigits(value, 0);
	int month = parseTwoDigits(value, 4);
	int day = parseTwoDigits(value, 6);
	int hour = parseTwoDigits(value, 9);
	int minute = parseTwoDigits(value, 11);
	int second = parseTwoDigits(value, 13);
	if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
			minute < 0 || minute > 59 || second < 0 || second > 59)
	{
		return 0;
	}

	tm notificationTime = {};
	notificationTime.tm_year = year - 1900;
	notificationTime.tm_mon = month - 1;
	notificationTime.tm_mday = day;
	notificationTime.tm_hour = hour;
	notificationTime.tm_min = minute;
	notificationTime.tm_sec = second;
	notificationTime.tm_isdst = -1;
	return mktime(&notificationTime);
}

static void dataSourceNotifyCallback(
		BLERemoteCharacteristic *pDataSourceCharacteristic,
		uint8_t *pData,
		size_t length,
		bool isNotify)
{
	sharedInstance->onDataSourceNotify(pDataSourceCharacteristic, pData, length, isNotify);
}

static void notificationSourceNotifyCallback(
		BLERemoteCharacteristic *pNotificationSourceCharacteristic,
		uint8_t *pData,
		size_t length,
		bool isNotify)
{
	sharedInstance->onNotificationSourceNotify(pNotificationSourceCharacteristic, pData, length, isNotify);
}

ANCSBLEClient::ANCSBLEClient()
		: notificationCB(nullptr), removedCB(nullptr), pControlPointCharacteristic(nullptr)
{
	assert(sharedInstance == nullptr);
	sharedInstance = this;
	notificationQueue = new ANCSNotificationQueue();
}

ANCSBLEClient::~ANCSBLEClient()
{
	sharedInstance = nullptr;
}

void ANCSBLEClient::startClientTask(void *params)
{
	const BLEAddress *address = (BLEAddress *)params;
	sharedInstance->setup(address);

	ANCSNotificationQueue *queue = sharedInstance->notificationQueue;
	while (1)
	{
		if (queue->pendingNotificationExists())
		{
			Notification pending = queue->getNextPendingNotification();
			sharedInstance->retrieveExtraNotificationData(pending);
		}
		delay(500);
	}
}

void ANCSBLEClient::setup(const BLEAddress *address)
{
#ifdef ENABLE_IOS_SETTINGS_PAIRING_HELPER
	// Let iOS finish HID pairing/bonding before we connect back to its ANCS service.
	delay(12000);
#endif

	BLEClient *pClient = BLEDevice::createClient();
	BLEDevice::setSecurityCallbacks(new NotificationSecurityCallbacks()); // @todo memory leak?

	BLESecurity *pSecurity = new BLESecurity();
	pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
	pSecurity->setCapability(ESP_IO_CAP_NONE);
	pSecurity->setKeySize(16);
	pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
	pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
	// Connect to the remote BLE Server.
	if (!pClient->connect(*address))
	{
		return;
	}

	/** BEGIN ANCS SERVICE **/
	// Obtain a reference to the service we are after in the remote BLE server.
	BLERemoteService *pAncsService = pClient->getService(ancsServiceUUID);
	if (pAncsService == nullptr)
	{
		return;
	}
	// Obtain a reference to the characteristic in the service of the remote BLE server.
	BLERemoteCharacteristic *pNotificationSourceCharacteristic = pAncsService->getCharacteristic(notificationSourceCharacteristicUUID);
	if (pNotificationSourceCharacteristic == nullptr)
	{
		return;
	}
	// Obtain a reference to the characteristic in the service of the remote BLE server.
	pControlPointCharacteristic = pAncsService->getCharacteristic(controlPointCharacteristicUUID);
	if (pControlPointCharacteristic == nullptr)
	{
		return;
	}
	// Obtain a reference to the characteristic in the service of the remote BLE server.
	BLERemoteCharacteristic *pDataSourceCharacteristic = pAncsService->getCharacteristic(dataSourceCharacteristicUUID);
	if (pDataSourceCharacteristic == nullptr)
	{
		return;
	}

	const uint8_t v[] = {0x1, 0x0};
	pDataSourceCharacteristic->registerForNotify(dataSourceNotifyCallback);
	pDataSourceCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t *)v, 2, true);
	pNotificationSourceCharacteristic->registerForNotify(notificationSourceNotifyCallback);
	pNotificationSourceCharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t *)v, 2, true);
}

BLEUUID ANCSBLEClient::getAncsServiceUUID()
{
	return ancsServiceUUID;
}

void ANCSBLEClient::setNotificationArrivedCallback(ble_notification_arrived_t cbNotification)
{
	notificationCB = cbNotification;
}

void ANCSBLEClient::setNotificationRemovedCallback(ble_notification_removed_t cbNotification)
{
	removedCB = cbNotification;
}

void ANCSBLEClient::retrieveExtraNotificationData(Notification &pending)
{
	uint8_t uuid[4];
	uint32_t notifyUUID = pending.uuid;
	uuid[0] = notifyUUID;
	uuid[1] = notifyUUID >> 8;
	uuid[2] = notifyUUID >> 16;
	uuid[3] = notifyUUID >> 24;

	bool notificationAlreadyInQueue = notificationQueue->contains(pending.uuid);
	if (notificationAlreadyInQueue)
	{
		return;
	}
	notificationQueue->addNotification(notifyUUID, pending, isIncomingCall(pending));

	const uint8_t vIdentifier[] = {0x0, uuid[0], uuid[1], uuid[2], uuid[3], ANCS::NotificationAttributeIDAppIdentifier};
	pControlPointCharacteristic->writeValue((uint8_t *)vIdentifier, 6, true);
	if ((pending.eventFlags & ANCS::EventFlagPositiveAction) != 0)
	{
		const uint8_t vPositiveLabel[] = {0x0, uuid[0], uuid[1], uuid[2], uuid[3], ANCS::NotificationAttributeIDPositiveActionLabel};
		pControlPointCharacteristic->writeValue((uint8_t *)vPositiveLabel, 6, true);
	}
	if ((pending.eventFlags & ANCS::EventFlagNegativeAction) != 0)
	{
		const uint8_t vNegativeLabel[] = {0x0, uuid[0], uuid[1], uuid[2], uuid[3], ANCS::NotificationAttributeIDNegativeActionLabel};
		pControlPointCharacteristic->writeValue((uint8_t *)vNegativeLabel, 6, true);
	}
	const uint8_t vTitle[] = {0x0, uuid[0], uuid[1], uuid[2], uuid[3], ANCS::NotificationAttributeIDTitle, 0x0, 0x10};
	pControlPointCharacteristic->writeValue((uint8_t *)vTitle, 8, true);
	const uint8_t vMessage[] = {0x0, uuid[0], uuid[1], uuid[2], uuid[3], ANCS::NotificationAttributeIDMessage, 0x0, 0x10};
	pControlPointCharacteristic->writeValue((uint8_t *)vMessage, 8, true);
	const uint8_t vDate[] = {0x0, uuid[0], uuid[1], uuid[2], uuid[3], ANCS::NotificationAttributeIDDate};
	pControlPointCharacteristic->writeValue((uint8_t *)vDate, 6, true);
}

void ANCSBLEClient::onDataSourceNotify(
		BLERemoteCharacteristic *pNotificationSourceCharacteristic,
		uint8_t *pData,
		size_t length,
		bool isNotify)
{
	std::string message;

	uint32_t messageId = pData[4];
	messageId = messageId << 8 | pData[3];
	messageId = messageId << 16 | pData[2];
	messageId = messageId << 24 | pData[1];

	for (int i = 8; i < length; i++)
	{
		message += (char)pData[i];
	}

	Notification *notification = notificationQueue->getNotification(messageId);
	bool visibleDataChanged = false;

	switch (pData[5])
	{
	case ANCS::NotificationAttributeIDAppIdentifier:
		notification->type = message;
		break;
	case 0x1:
		visibleDataChanged = notification->title != message;
		notification->title = message;
		notification->titleReceived = true;
		break;
	case 0x3:
		visibleDataChanged = notification->message != message;
		notification->message = message;
		notification->messageReceived = true;
		break;
	case ANCS::NotificationAttributeIDPositiveActionLabel:
		visibleDataChanged = notification->positiveActionLabel != message;
		notification->positiveActionLabel = message;
		break;
	case ANCS::NotificationAttributeIDNegativeActionLabel:
		visibleDataChanged = notification->negativeActionLabel != message;
		notification->negativeActionLabel = message;
		break;
	case ANCS::NotificationAttributeIDDate:
	{
		time_t notificationTime = parseNotificationDate(message);
		visibleDataChanged = notificationTime != 0 && notification->time != notificationTime;
		if (notificationTime != 0)
		{
			notification->time = notificationTime;
		}
		break;
	}
	}
	if (notification->titleReceived && notification->messageReceived && (!notification->title.empty() || !notification->message.empty()))
	{
		if (notificationCB && (!notification->isComplete || visibleDataChanged))
		{
			const ArduinoNotification arduinoNotification = ArduinoNotification(*notification);
			notificationCB(&arduinoNotification, notification);
		}
		notification->isComplete = true;
	}
}

bool ANCSBLEClient::isIncomingCall(const Notification &notification) const
{
	// @todo detect if it is a FaceTime or call by app? Or is category sufficient?
	return notification.category == CategoryIDIncomingCall;
}

void ANCSBLEClient::onNotificationSourceNotify(
		BLERemoteCharacteristic *pNotificationSourceCharacteristic,
		uint8_t *pData,
		size_t length,
		bool isNotify)
{
	uint32_t messageId;

	messageId = pData[7];
	messageId = messageId << 8 | pData[6];
	messageId = messageId << 16 | pData[5];
	messageId = messageId << 24 | pData[4];

	if (pData[0] == ANCS::EventIDNotificationRemoved)
	{
		Notification *notification = notificationQueue->getNotification(messageId);

		if (isIncomingCall(*notification))
		{
			notificationQueue->addNotification(messageId, *notification, false);
			notificationQueue->removeCallNotification();
		}

		if (removedCB)
		{
			const ArduinoNotification arduinoNotification = ArduinoNotification(*notification);
			removedCB(&arduinoNotification, notification);
		}
	}
	else if (pData[0] == ANCS::EventIDNotificationAdded || pData[0] == ANCS::EventIDNotificationModified)
	{
		if (notificationQueue->contains(messageId))
		{
			return;
		}

		Notification pending;
		pending.uuid = messageId;
		pending.eventFlags = pData[1];
		pending.category = NotificationCategory(pData[2]);
		pending.categoryCount = pData[3];
		notificationQueue->addPendingNotification(pending);
	}
}

void ANCSBLEClient::performAction(uint32_t notifyUUID, uint8_t actionID)
{
	uint8_t uuid[4];
	uuid[0] = notifyUUID;
	uuid[1] = notifyUUID >> 8;
	uuid[2] = notifyUUID >> 16;
	uuid[3] = notifyUUID >> 24;

	const uint8_t vPerformAction[] = {ANCS::CommandIDPerformNotificationAction, uuid[0], uuid[1], uuid[2], uuid[3], actionID};
	pControlPointCharacteristic->writeValue((uint8_t *)vPerformAction, (sizeof(vPerformAction) / sizeof(vPerformAction[0])), true);
}
