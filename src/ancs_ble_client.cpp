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
	if (notificationAlreadyInQueue == false)
	{
		notificationQueue->addNotification(notifyUUID, pending, isIncomingCall(pending));
	}
	else
	{
		Notification *notification = notificationQueue->getNotification(notifyUUID);
		notification->isComplete = false;
		notification->titleReceived = false;
		notification->messageReceived = false;
		notification->title.clear();
		notification->message.clear();
	}

	const uint8_t vIdentifier[] = {0x0, uuid[0], uuid[1], uuid[2], uuid[3], ANCS::NotificationAttributeIDAppIdentifier};
	pControlPointCharacteristic->writeValue((uint8_t *)vIdentifier, 6, true);
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

	switch (pData[5])
	{
	case ANCS::NotificationAttributeIDAppIdentifier:
		notification->type = message;
		break;
	case 0x1:
		notification->title = message;
		notification->titleReceived = true;
		break;
	case 0x3:
		notification->message = message;
		notification->messageReceived = true;
		break;
	}
	if (notification->titleReceived && notification->messageReceived && (!notification->title.empty() || !notification->message.empty()))
	{
		if (notificationCB && notification->isComplete == false)
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
