// NKolban's original library has a setServiceSolicitation method. As of writing, this is not in the
// Espressif core libs. If you are using NKolban's branch of the library, or if this moves into
// the core libraries eventually, uncomment this.
// #define BLE_LIB_HAS_SERVICE_SOLICITATION

#include "esp32notifications.h"
#include "ancs_ble_client.h"
#include "ble_security.h"

#include "BLEAddress.h"
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLEClient.h"
#include "BLEUtils.h"
#include "BLE2902.h"
#ifdef ENABLE_IOS_SETTINGS_PAIRING_HELPER
#include "BLEHIDDevice.h"
#endif

#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>

extern const BLEUUID ancsServiceUUID;

#ifndef BLE_LIB_HAS_SERVICE_SOLICITATION
// Use a static function, instead of doing a whole private implementation just for a this one small patch.
static void setServiceSolicitation(class BLEAdvertisementData &advertisementData, BLEUUID uuid);
#endif

#ifdef BLE_CLEAR_BONDS_ON_BOOT
static void clearBondedDevices();
#endif

class MyServerCallbacks : public BLEServerCallbacks
{

public:
	BLENotifications *instance;

	MyServerCallbacks(BLENotifications *parent)
			: instance(parent)
	{
	}

	void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param)
	{
		instance->isAdvertising = false;
		instance->client = new ANCSBLEClient(); // @todo memory leaks?
		instance->client->setNotificationArrivedCallback(instance->cbNotification);
		instance->client->setNotificationRemovedCallback(instance->cbRemoved);
		::xTaskCreatePinnedToCore(&ANCSBLEClient::startClientTask, "ClientTask", 10000, new BLEAddress(param->connect.remote_bda), 5, &instance->client->clientTaskHandle, 0);

		delay(1000);

		if (instance->cbStateChanged)
		{
			instance->cbStateChanged(BLENotifications::StateConnected);
		}
	};

	void onDisconnect(BLEServer *pServer)
	{
		if (instance->client != nullptr && instance->client->clientTaskHandle != nullptr)
		{
			::vTaskDelete(instance->client->clientTaskHandle);
			instance->client->clientTaskHandle = nullptr;
		}
		instance->isAdvertising = false;
		if (instance->cbStateChanged)
		{
			instance->cbStateChanged(BLENotifications::StateDisconnected);
		}
		delete instance->client;
		instance->client = nullptr;
	}
};

BLENotifications::BLENotifications()
		: cbStateChanged(nullptr), cbNotification(nullptr), cbRemoved(nullptr), server(nullptr), client(nullptr),
#ifdef ENABLE_IOS_SETTINGS_PAIRING_HELPER
			pairingHidDevice(nullptr),
#endif
			localName("ESP32 ANCS"), isAdvertising(false)
{
}

const char *BLENotifications::getNotificationCategoryDescription(NotificationCategory category) const
{
	switch (category)
	{
	case CategoryIDOther:
		return "other";
	case CategoryIDIncomingCall:
		return "incoming call";
	case CategoryIDMissedCall:
		return "missed call";
	case CategoryIDVoicemail:
		return "voicemail";
	case CategoryIDSocial:
		return "social";
	case CategoryIDSchedule:
		return "schedule";
	case CategoryIDEmail:
		return "email";
	case CategoryIDNews:
		return "news";
	case CategoryIDHealthAndFitness:
		return "health and fitness";
	case CategoryIDBusinessAndFinance:
		return "business and finance";
	case CategoryIDLocation:
		return "location";
	case CategoryIDEntertainment:
		return "entertainment";
	default:
		return "unknown";
	}
}

bool BLENotifications::begin(const char *name)
{
	localName = name;
	BLEDevice::init(localName);
	BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_NO_MITM);
#ifdef BLE_CLEAR_BONDS_ON_BOOT
	clearBondedDevices();
#endif
	server = BLEDevice::createServer();
	server->setCallbacks(new MyServerCallbacks(this));
	BLEDevice::setSecurityCallbacks(new NotificationSecurityCallbacks()); // @todo memory leak?
#ifdef ENABLE_IOS_SETTINGS_PAIRING_HELPER
	setupIOSSettingsPairingHelper();
#endif

	startAdvertising();
	return true;
}

#ifdef ENABLE_IOS_SETTINGS_PAIRING_HELPER
void BLENotifications::setupIOSSettingsPairingHelper()
{
	if (pairingHidDevice != nullptr)
	{
		return;
	}

	static uint8_t keyboardReportMap[] = {
			0x05, 0x01, // Usage Page (Generic Desktop)
			0x09, 0x06, // Usage (Keyboard)
			0xA1, 0x01, // Collection (Application)
			0x85, 0x01, // Report ID (1)
			0x05, 0x07, // Usage Page (Keyboard)
			0x19, 0xE0, // Usage Minimum (Keyboard Left Control)
			0x29, 0xE7, // Usage Maximum (Keyboard Right GUI)
			0x15, 0x00, // Logical Minimum (0)
			0x25, 0x01, // Logical Maximum (1)
			0x75, 0x01, // Report Size (1)
			0x95, 0x08, // Report Count (8)
			0x81, 0x02, // Input (Data, Variable, Absolute)
			0x95, 0x01, // Report Count (1)
			0x75, 0x08, // Report Size (8)
			0x81, 0x01, // Input (Constant)
			0x95, 0x05, // Report Count (5)
			0x75, 0x01, // Report Size (1)
			0x05, 0x08, // Usage Page (LEDs)
			0x19, 0x01, // Usage Minimum (Num Lock)
			0x29, 0x05, // Usage Maximum (Kana)
			0x91, 0x02, // Output (Data, Variable, Absolute)
			0x95, 0x01, // Report Count (1)
			0x75, 0x03, // Report Size (3)
			0x91, 0x01, // Output (Constant)
			0x95, 0x06, // Report Count (6)
			0x75, 0x08, // Report Size (8)
			0x15, 0x00, // Logical Minimum (0)
			0x25, 0x65, // Logical Maximum (101)
			0x05, 0x07, // Usage Page (Keyboard)
			0x19, 0x00, // Usage Minimum (Reserved)
			0x29, 0x65, // Usage Maximum (Keyboard Application)
			0x81, 0x00, // Input (Data, Array)
			0xC0        // End Collection
	};

	pairingHidDevice = new BLEHIDDevice(server);
	pairingHidDevice->manufacturer();
	pairingHidDevice->manufacturer("iSimpleLab");
	pairingHidDevice->pnp(0x02, 0xE502, 0xA111, 0x0210);
	pairingHidDevice->hidInfo(0x00, 0x02);
	BLECharacteristic *inputReport = pairingHidDevice->inputReport(1);
	BLECharacteristic *outputReport = pairingHidDevice->outputReport(1);
	BLECharacteristic *bootInput = pairingHidDevice->bootInput();
	BLECharacteristic *bootOutput = pairingHidDevice->bootOutput();
	static uint8_t emptyKeyboardInput[8] = {0};
	static uint8_t emptyKeyboardOutput[1] = {0};
	inputReport->setValue(emptyKeyboardInput, sizeof(emptyKeyboardInput));
	outputReport->setValue(emptyKeyboardOutput, sizeof(emptyKeyboardOutput));
	bootInput->setValue(emptyKeyboardInput, sizeof(emptyKeyboardInput));
	bootOutput->setValue(emptyKeyboardOutput, sizeof(emptyKeyboardOutput));
	pairingHidDevice->reportMap(keyboardReportMap, sizeof(keyboardReportMap));
	pairingHidDevice->startServices();
	pairingHidDevice->setBatteryLevel(100);
}
#endif

bool BLENotifications::stop()
{
	BLEDevice::deinit(false);
	return true;
}

void BLENotifications::setConnectionStateChangedCallback(ble_notifications_state_changed_t callback)
{
	cbStateChanged = callback;
}

void BLENotifications::setNotificationCallback(ble_notification_arrived_t callback)
{
	cbNotification = callback;
}

void BLENotifications::setRemovedCallback(ble_notification_removed_t callback)
{
	cbRemoved = callback;
}

void BLENotifications::actionPositive(uint32_t uuid)
{
	client->performAction(uuid, uint8_t(ANCS::NotificationActionPositive));
}

void BLENotifications::actionNegative(uint32_t uuid)
{
	client->performAction(uuid, uint8_t(ANCS::NotificationActionNegative));
}

void BLENotifications::startAdvertising()
{
	// Start soliciting the Apple ANCS service and make the device visible to searches on iOS (from Apple ANCS documentation)
	BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();

#ifdef BLE_USE_STATIC_RANDOM_ADDRESS
	static bool randomAddressConfigured = false;
	if (randomAddressConfigured == false)
	{
		esp_bd_addr_t staticRandomAddress = {0xD3, 0x15, 0x06, 0xE6, 0x69, 0xAD};
		pAdvertising->setDeviceAddress(staticRandomAddress, BLE_ADDR_TYPE_RANDOM);
		randomAddressConfigured = true;
	}
#endif

	if (isAdvertising == true)
	{
		// There is no way to query the BLEAdvertising object to see if it is advertising, so we keep a variable.
		pAdvertising->stop();
	}

	BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();
	oAdvertisementData.setFlags(0x06);

#ifdef BLE_LIB_HAS_SERVICE_SOLICITATION
	oAdvertisementData.setServiceSolicitation(ANCSBLEClient::getAncsServiceUUID());
#else
	setServiceSolicitation(oAdvertisementData, ANCSBLEClient::getAncsServiceUUID());
#endif
#ifdef ENABLE_IOS_SETTINGS_PAIRING_HELPER
	oAdvertisementData.setCompleteServices(BLEUUID((uint16_t)0x1812));
	oAdvertisementData.setAppearance(HID_KEYBOARD);
#else
	oAdvertisementData.setShortName("ANCS");
#endif

	pAdvertising->setAdvertisementData(oAdvertisementData);
	BLEAdvertisementData scanResponseData = BLEAdvertisementData();
	scanResponseData.setName(localName);
	pAdvertising->setScanResponseData(scanResponseData);
	pAdvertising->setScanResponse(true);

	// Set security
	BLESecurity *pSecurity = new BLESecurity();
	pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
	pSecurity->setCapability(ESP_IO_CAP_NONE);
	pSecurity->setKeySize(16);
	pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
	pSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

	// Start advertising
	pAdvertising->start();
	isAdvertising = true;
}

#ifndef BLE_LIB_HAS_SERVICE_SOLICITATION
void setServiceSolicitation(BLEAdvertisementData &advertisementData, BLEUUID uuid)
{
	char cdata[2];
	switch (uuid.bitSize())
	{
	case 16:
	{
		// [Len] [0x14] [UUID16] data
		cdata[0] = 3;
		cdata[1] = ESP_BLE_AD_TYPE_SOL_SRV_UUID; // 0x14
		advertisementData.addData(std::string(cdata, 2) + std::string((char *)&uuid.getNative()->uuid.uuid16, 2));
		break;
	}

	case 128:
	{
		// [Len] [0x15] [UUID128] data
		cdata[0] = 17;
		cdata[1] = ESP_BLE_AD_TYPE_128SOL_SRV_UUID; // 0x15
		advertisementData.addData(std::string(cdata, 2) + std::string((char *)uuid.getNative()->uuid.uuid128, 16));
		break;
	}

	default:
		return;
	}
}
#endif

#ifdef BLE_CLEAR_BONDS_ON_BOOT
void clearBondedDevices()
{
	int bondCount = esp_ble_get_bond_device_num();
	if (bondCount <= 0)
	{
		return;
	}

	esp_ble_bond_dev_t *bondedDevices = new esp_ble_bond_dev_t[bondCount];
	int deviceCount = bondCount;
	esp_err_t listResult = esp_ble_get_bond_device_list(&deviceCount, bondedDevices);
	if (listResult != ESP_OK)
	{
		delete[] bondedDevices;
		return;
	}

	for (int i = 0; i < deviceCount; ++i)
	{
		esp_ble_remove_bond_device(bondedDevices[i].bd_addr);
	}

	delete[] bondedDevices;
}
#endif
