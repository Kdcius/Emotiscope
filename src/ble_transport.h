/*
-----------------------------------------------------------------------------
--- EMOTISCOPE ENGINE -------------------------------------------------------

	ble_transport.h
		- BLE GATT transport for the command protocol (Nordic UART Service)

	Carries the exact same pipe-delimited command strings as the WebSocket
	transport, newline-framed so they survive BLE's packet fragmentation:

		phone -> ESP : writes to the RX characteristic, '\n' marks end of command
		ESP -> phone : notifications on the TX characteristic, '\n' appended

	A single BLE client is supported (advertising stops while connected and
	resumes on disconnect). Commands are tagged with BLE_CLIENT_SLOT so that
	parse_command() replies route back over BLE.

-----------------------------------------------------------------------------
*/

#if TRANSPORT_BLE

// NOTE: the BLE library bundled with arduino-esp32 3.0.0-rc1 references a
// typo'd IDF type name that IDF v5.1.4 corrected; a -D token alias in
// platformio.ini (esp_ble_gap_ext_adv_reprot_t) patches it for every
// translation unit, including the BLE library's own sources.
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define BLE_DEVICE_NAME "Emotiscope"

// Nordic UART Service UUIDs - the de-facto standard for serial-over-BLE,
// directly usable from Web Bluetooth
#define BLE_NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e" // client writes commands here
#define BLE_NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // client subscribes for replies

BLECharacteristic* ble_tx_characteristic = NULL;

volatile bool ble_client_connected  = false;
volatile bool ble_client_subscribed = false;
volatile bool ble_welcome_pending   = false;

// Usable notification payload = ATT MTU - 3. Starts at the BLE minimum;
// updated when the client negotiates a larger MTU (Android/Windows usually
// land at 517, making every message single-packet).
volatile uint16_t ble_max_payload = 20;

// Reassembly buffer for inbound command fragments
char ble_rx_accumulator[MAX_COMMAND_LENGTH];
uint16_t ble_rx_length = 0;

// Send one command string to the connected BLE client, '\n'-terminated,
// chunked to the negotiated MTU. Safe to call when nothing is connected.
void ble_send(const char* message) {
	if (ble_client_subscribed == false || ble_tx_characteristic == NULL) { return; }

	static char framed[MAX_COMMAND_LENGTH + 2];
	uint16_t length = strlen(message);
	if (length > MAX_COMMAND_LENGTH) { return; }
	memcpy(framed, message, length);
	framed[length] = '\n';
	length += 1;

	uint16_t offset = 0;
	while (offset < length) {
		uint16_t chunk = min((uint16_t)(length - offset), (uint16_t)ble_max_payload);
		ble_tx_characteristic->setValue((uint8_t*)(framed + offset), chunk);
		ble_tx_characteristic->notify();
		offset += chunk;

		// Only pace when a message actually spans packets (i.e. MTU stayed small)
		if (offset < length) { delay(3); }
	}
}

// Feed raw bytes from the RX characteristic into the command queue,
// splitting on newlines
void ble_feed_rx(const uint8_t* data, uint16_t length) {
	for (uint16_t i = 0; i < length; i++) {
		char byte = (char)data[i];
		if (byte == '\n' || byte == '\r') {
			if (ble_rx_length > 0) {
				queue_command(ble_rx_accumulator, ble_rx_length, BLE_CLIENT_SLOT);
				ble_rx_length = 0;
			}
		}
		else if (ble_rx_length < MAX_COMMAND_LENGTH - 1) {
			ble_rx_accumulator[ble_rx_length++] = byte;
		}
		else {
			ble_rx_length = 0; // Oversized command with no terminator: drop it
		}
	}
}

class BLETransportServerCallbacks : public BLEServerCallbacks {
	void onConnect(BLEServer* server) override {
		ble_client_connected = true;
		printf("[ble] client connected\n");
	}

	void onDisconnect(BLEServer* server) override {
		ble_client_connected  = false;
		ble_client_subscribed = false;
		ble_welcome_pending   = false;
		ble_rx_length = 0;
		ble_max_payload = 20;
		printf("[ble] client disconnected, resuming advertising\n");
		BLEDevice::startAdvertising(); // Bluedroid does not resume this on its own
	}

	void onMtuChanged(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
		ble_max_payload = param->mtu.mtu - 3;
		printf("[ble] MTU negotiated: %u (payload %u)\n", param->mtu.mtu, ble_max_payload);
	}
};

class BLETransportRxCallbacks : public BLECharacteristicCallbacks {
	void onWrite(BLECharacteristic* characteristic) override {
		ble_feed_rx(characteristic->getData(), characteristic->getLength());
	}
};

// Fires when the client writes the TX characteristic's CCCD (0x2902), i.e.
// subscribes to notifications. That is the moment the app is ready to listen,
// so it stands in for the WebSocket onOpen -> "welcome" handshake. The welcome
// itself is deferred to the main loop (run_ble_transport) rather than sent
// from the Bluetooth task.
class BLETransportCccdCallbacks : public BLEDescriptorCallbacks {
	void onWrite(BLEDescriptor* descriptor) override {
		uint8_t* value = descriptor->getValue();
		if (value[0] & 1) {
			ble_client_subscribed = true;
			ble_welcome_pending   = true;
			printf("[ble] client subscribed to notifications\n");
		}
		else {
			ble_client_subscribed = false;
		}
	}
};

void init_ble() {
	printf("init_ble\n");

	BLEDevice::init(BLE_DEVICE_NAME);
	BLEDevice::setMTU(517);

	BLEServer* server = BLEDevice::createServer();
	server->setCallbacks(new BLETransportServerCallbacks());

	BLEService* service = server->createService(BLE_NUS_SERVICE_UUID);

	ble_tx_characteristic = service->createCharacteristic(
		BLE_NUS_TX_UUID,
		BLECharacteristic::PROPERTY_NOTIFY
	);
	BLE2902* cccd = new BLE2902();
	cccd->setCallbacks(new BLETransportCccdCallbacks());
	ble_tx_characteristic->addDescriptor(cccd);

	BLECharacteristic* rx_characteristic = service->createCharacteristic(
		BLE_NUS_RX_UUID,
		BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
	);
	rx_characteristic->setCallbacks(new BLETransportRxCallbacks());

	service->start();

	BLEAdvertising* advertising = BLEDevice::getAdvertising();
	advertising->addServiceUUID(BLE_NUS_SERVICE_UUID);
	advertising->setScanResponse(true);
	BLEDevice::startAdvertising();

	printf("[ble] advertising as \"%s\"\n", BLE_DEVICE_NAME);
}

// Called every loop() iteration from run_web() - completes the handshake
// from the main task instead of the Bluetooth stack's task
void run_ble_transport() {
	if (ble_welcome_pending == true) {
		ble_welcome_pending = false;
		ble_send("welcome");
	}
}

#else

// BLE disabled: keep call sites compiling without #if churn
void ble_send(const char* message) {}
void init_ble() {}
void run_ble_transport() {}

#endif
