/*
	 ESP32 Bluetooth Low Energy presence detection, for use with MQTT.

	 Project and documentation are available on GitHub at https://jptrsn.github.io/ESP32-mqtt-room/

   Some giants upon whose shoulders the project stands -- major thanks to:

   pcbreflux for the original version of this code, as well as the eddystone handlers https://github.com/pcbreflux

   Andreis Speiss for his work on YouTube and his invaluable github at https://github.com/sensorsiot.

	 Sidddy for the implementation of Mi Flora plant sensor support. https://github.com/sidddy/flora

   Based on Neil Kolban example for IDF: https://github.com/nkolban/esp32-snippets/blob/master/cpp_utils/tests/BLE%20Tests/SampleScan.cpp
   Ported to Arduino ESP32 by Evandro Copercini
*/
#include <WiFi.h>
extern "C" {
	#include "freertos/FreeRTOS.h"
	#include "freertos/timers.h"
}
#include "soc/timer_group_struct.h"
#include "soc/timer_group_reg.h"

#include <AsyncTCP.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include "BLEBeacon.h"
#include "BLEEddystoneTLM.h"
#include "BLEEddystoneURL.h"
#include "Common_settings.h"
#include "Settings.h"

#include <Adafruit_BME280.h>
Adafruit_BME280 bme; // I2C

static const int scanTime = singleScanTime;
static const int waitTime = scanInterval;
static const uint16_t beaconUUID = 0xFEAA;
#ifdef TxDefault
static const int defaultTxPower = TxDefault;
#else
static const int defaultTxPower = -72;
#endif
#define ENDIAN_CHANGE_U16(x) ((((x)&0xFF00)>>8) + (((x)&0xFF)<<8))
#ifdef BME280_enable
static unsigned BME280_status;
#endif
#ifdef Deep_sleep
#define LED_GPIO 22   
#else
#define LED_GPIO LED_BUILTIN  
#endif

#define uS_TO_M_FACTOR 3600000000ULL  /* Conversion factor for micro seconds to hours */
#define TIME_TO_SLEEP  6        /* Time ESP32 will go to sleep (in hours) */
static int voltage = 0;
static int loopCount = 0;
static int powerOn = 0;


WiFiClient espClient; 
AsyncMqttClient mqttClient;
TimerHandle_t mqttReconnectTimer;
TimerHandle_t wifiReconnectTimer;
bool updateInProgress = false;
String localIp;
byte retryAttempts = 0;
unsigned long last = 0;
unsigned long lastBME280 = 0;
unsigned long lastSleep = 0;
BLEScan* pBLEScan;
TaskHandle_t BLEScan;
float distance;

String getProximityUUIDString(BLEBeacon beacon) {
  std::string serviceData = beacon.getProximityUUID().toString().c_str();
  int serviceDataLength = serviceData.length();
  String returnedString = "";
  int i = serviceDataLength;
  while (i > 0)
  {
    if (serviceData[i-1] == '-') {
      i--;
    }
    char a = serviceData[i-1];
    char b = serviceData[i-2];
    returnedString += b;
    returnedString += a;

    i -= 2;
  }

  return returnedString;
}

float calculateDistance(int rssi, int txPower) {

	float distFl;

  if (rssi == 0) {
      return -1.0;
  }

  if (!txPower) {
      // somewhat reasonable default value
      txPower = defaultTxPower;
  }

	if (txPower > 0) {
		txPower = txPower * -1;
	}

  const float ratio = rssi * 1.0 / txPower;
  if (ratio < 1.0) {
      distFl = pow(ratio, 10);
  } else {
      distFl = (0.89976) * pow(ratio, 7.7095) + 0.111;
  }

	return round(distFl * 100) / 100;

}

bool sendTelemetry(int deviceCount = -1, int reportCount = -1, int voltage = -1, int loopCount = -1, int powerOn = -1) {
	StaticJsonDocument<256> tele;
	tele["room"] = room;
	tele["ip"] = localIp;
	tele["hostname"] = WiFi.getHostname();
	tele["scan_dur"] = scanTime;
	tele["wait_dur"] = waitTime;
	tele["max_dist"] = maxDistance;

	if (deviceCount > -1) {
		Serial.printf("devices_discovered: %d\n\r",deviceCount);
    tele["disc_ct"] = deviceCount;
	}

	if (reportCount > -1) {
		Serial.printf("devices_reported: %d\n\r",reportCount);
    tele["rept_ct"] = reportCount;
	}

	if (voltage > -1) {
		Serial.printf("voltage: %d\n\r",voltage);
    tele["voltage"] = voltage;
	}

	if (loopCount > -1) {
		Serial.printf("loop_count: %d\n\r",loopCount);
    tele["loop_ct"] = loopCount;
	}

	if (powerOn > -1) {
		Serial.printf("power_on: %d\n\r",powerOn);
    tele["power_on"] = powerOn;
	}

	char teleMessageBuffer[258];
	serializeJson(tele, teleMessageBuffer);

	if (mqttClient.publish(telemetryTopic, 0, 1, teleMessageBuffer) == true) {
		Serial.println("Telemetry sent");
		return true;
	} else {
		Serial.println("Error sending telemetry");
		return false;
	}
}

bool sendBME280() {
	if (debug) mqttClient.publish(debugTopic, 0, 0, "sendBME280 start");
	StaticJsonDocument<256> BME280;
	BME280["temperature"] = bme.readTemperature();
	BME280["humidity"] = bme.readHumidity();
	BME280["pressure"] = bme.readPressure() / 100.0F;

	char BME280MessageBuffer[258];
	serializeJson(BME280, BME280MessageBuffer);

	if (mqttClient.publish(temperatureTopic, 0, 0, BME280MessageBuffer) == true) {
		Serial.println("BME280 info sent");
		if (debug) mqttClient.publish(debugTopic, 0, 0, "sendBME280 end");
		return true;
	} else {
		Serial.println("Error sending BME280 info");
		mqttClient.publish(errorTopic, 0, 0, "Error sending BME280 info");
		if (debug) mqttClient.publish(debugTopic, 0, 0, "sendBME280 end");
		return false;
	}
}

void connectToWifi() {
  Serial.println("Connecting to WiFi...");
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
	WiFi.setHostname(hostname);
	WiFi.begin(ssid, password);
}

bool handleWifiDisconnect() {
	Serial.println("WiFi has been disconnected.");
	if (WiFi.isConnected()) {
		Serial.println("WiFi appears to be connected. Not retrying.");
		return true;
	}
	if (retryAttempts > 10) {
#ifndef Deep_sleep
		Serial.println("Too many retries. Restarting");
		ESP.restart();
#endif
	} else {
		retryAttempts++;
	}
	if (mqttClient.connected()) {
		mqttClient.disconnect();
	}
	if (xTimerIsTimerActive(mqttReconnectTimer) != pdFALSE) {
		xTimerStop(mqttReconnectTimer, 0); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
	}

	if (xTimerReset(wifiReconnectTimer, 0) == pdFAIL) {
		Serial.println("failed to restart");
		xTimerStart(wifiReconnectTimer, 0);
		return false;
	} else {
		Serial.println("restarted");
		return true;
	}

}

void connectToMqtt() {
  Serial.print("Connecting to MQTT with ClientId ");
  Serial.println(hostname);
	if (WiFi.isConnected() && !updateInProgress) {
		mqttClient.setServer(mqttHost, mqttPort);
		mqttClient.setWill(availabilityTopic, 0, 1, "DISCONNECTED");
		mqttClient.setKeepAlive(60);
		mqttClient.setCredentials(mqttUser, mqttPassword);
		mqttClient.setClientId(hostname);
	  	mqttClient.connect();
	} else {
		Serial.println("Cannot reconnect MQTT - WiFi error");
		handleWifiDisconnect();
	}
}

bool handleMqttDisconnect() {
	Serial.println("MQTT has been disconnected.");
	if (updateInProgress) {
		Serial.println("Not retrying MQTT connection - OTA update in progress");
		return true;
	}
	if (retryAttempts > 10) {
#ifndef Deep_sleep
		Serial.println("Too many retries. Restarting");
		ESP.restart();
#endif
	} else {
		retryAttempts++;
	}
	if (WiFi.isConnected() && !updateInProgress) {
		Serial.println("Starting MQTT reconnect timer");
    if (xTimerReset(mqttReconnectTimer, 0) == pdFAIL) {
			Serial.println("failed to restart");
			xTimerStart(mqttReconnectTimer, 0);
		} else {
			Serial.println("restarted");
		}
    } else {
		Serial.print("Disconnected from WiFi; starting WiFi reconnect timiler\t");
		handleWifiDisconnect();
	}
    return true;
}

void WiFiEvent(WiFiEvent_t event) {
    Serial.printf("[WiFi-event] event: %x\n\r", event);
		switch(event) {
	    case SYSTEM_EVENT_STA_GOT_IP:
					digitalWrite(LED_GPIO, !LED_ON);
	        Serial.print("IP address: \t");
	        Serial.println(WiFi.localIP());
					localIp = WiFi.localIP().toString().c_str();
					Serial.print("Hostname: \t");
					Serial.println(WiFi.getHostname());
	        connectToMqtt();
					if (xTimerIsTimerActive(wifiReconnectTimer) != pdFALSE) {
						Serial.println("Stopping wifi reconnect timer");
						xTimerStop(wifiReconnectTimer, 0);
					}
					retryAttempts = 0;
	        break;
	    case SYSTEM_EVENT_STA_DISCONNECTED:
					digitalWrite(LED_GPIO, LED_ON);
	        Serial.println("WiFi lost connection, resetting timer\t");
					handleWifiDisconnect();
					break;
			case SYSTEM_EVENT_WIFI_READY:
					Serial.println("Wifi Ready");
					handleWifiDisconnect();
					break;
			case SYSTEM_EVENT_STA_START:
					Serial.println("STA Start");
					tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, hostname);
					if (xTimerIsTimerActive(wifiReconnectTimer) != pdFALSE) {
						TickType_t xRemainingTime = xTimerGetExpiryTime( wifiReconnectTimer ) - xTaskGetTickCount();
						Serial.print("WiFi Time remaining: ");
						Serial.println(xRemainingTime);
					} else {
						Serial.println("WiFi Timer is inactive; resetting\t");
						handleWifiDisconnect();
					}
					break;
			case SYSTEM_EVENT_STA_STOP:
					Serial.println("STA Stop");
					handleWifiDisconnect();
					break;
			default:
					Serial.println("Event not considered");
					handleWifiDisconnect();
					break;
    }
}

void onMqttConnect(bool sessionPresent) {
  Serial.println("Connected to MQTT.");
	retryAttempts = 0;

	if (mqttClient.publish(availabilityTopic, 0, 1, "CONNECTED") == true) {
		//Serial.print("Success sending message to topic:\t");
		//Serial.println(availabilityTopic);
	} else {
		Serial.println("Error sending message");
	}

	//sendTelemetry();

}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  Serial.print("Disconnected from MQTT. Reason: ");
  Serial.println(static_cast<uint8_t>(reason));
  handleMqttDisconnect();
}

bool reportDevice(BLEAdvertisedDevice advertisedDevice) {

	// Serial.printf("\n\n");

	StaticJsonDocument<500> doc;

	String mac_address = advertisedDevice.getAddress().toString().c_str();
	mac_address.replace(":","");
	mac_address.toLowerCase();
	// Serial.print("mac:\t");
	// Serial.println(mac_address);
	int rssi = advertisedDevice.getRSSI();

	doc["id"] = mac_address;
	doc["uuid"] = mac_address;
	doc["rssi"] = rssi;

	if (advertisedDevice.haveName()){
		String nameBLE = String(advertisedDevice.getName().c_str());
		// Serial.print("Name: ");
		// Serial.println(nameBLE);
		doc["name"] = nameBLE;

	} else {
		// doc["name"] = "unknown";
		// Serial.println("Device name unknown");
	}

	// Serial.printf("\n\r");
	// Serial.printf("Advertised Device: %s \n\r", advertisedDevice.toString().c_str());
	std::string strServiceData = advertisedDevice.getServiceData();
	 uint8_t cServiceData[100];
	 strServiceData.copy((char *)cServiceData, strServiceData.length(), 0);

	 if (advertisedDevice.getServiceDataUUID().equals(BLEUUID(beaconUUID))==true) {  // found Eddystone UUID
		// Serial.printf("is Eddystone: %d %s length %d\n", advertisedDevice.getServiceDataUUID().bitSize(), advertisedDevice.getServiceDataUUID().toString().c_str(),strServiceData.length());
		if (cServiceData[0]==0x10) {
			 BLEEddystoneURL oBeacon = BLEEddystoneURL();
			 oBeacon.setData(strServiceData);
			 // Serial.printf("Eddystone Frame Type (Eddystone-URL) ");
			 // Serial.printf(oBeacon.getDecodedURL().c_str());
			 doc["url"] = oBeacon.getDecodedURL().c_str();

		} else if (cServiceData[0]==0x20) {
			 BLEEddystoneTLM oBeacon = BLEEddystoneTLM();
			 oBeacon.setData(strServiceData);
			 // Serial.printf("Eddystone Frame Type (Unencrypted Eddystone-TLM) \n");
			 // Serial.printf(oBeacon.toString().c_str());
		} else {
			// Serial.println("service data");
			for (int i=0;i<strServiceData.length();i++) {
				// Serial.printf("[%X]",cServiceData[i]);
			}
		}
		// Serial.printf("\n");
	} else {
		if (advertisedDevice.haveManufacturerData()==true) {
			std::string strManufacturerData = advertisedDevice.getManufacturerData();


			uint8_t cManufacturerData[100];
			strManufacturerData.copy((char *)cManufacturerData, strManufacturerData.length(), 0);

			if (strManufacturerData.length()==25 && cManufacturerData[0] == 0x4C  && cManufacturerData[1] == 0x00 ) {
				BLEBeacon oBeacon = BLEBeacon();
				oBeacon.setData(strManufacturerData);

				String proximityUUID = getProximityUUIDString(oBeacon);

				distance = calculateDistance(rssi, oBeacon.getSignalPower());

				// Serial.print("RSSI: ");
				// Serial.print(rssi);
				// Serial.print("\ttxPower: ");
				// Serial.print(oBeacon.getSignalPower());
				// Serial.print("\tDistance: ");
				// Serial.println(distance);

				int major = ENDIAN_CHANGE_U16(oBeacon.getMajor());
				int minor = ENDIAN_CHANGE_U16(oBeacon.getMinor());

				doc["major"] = major;
				doc["minor"] = minor;

				doc["uuid"] = proximityUUID;
				doc["id"] = proximityUUID + "-" + String(major) + "-" + String(minor);
				doc["txPower"] = oBeacon.getSignalPower();
				doc["distance"] = distance;

			} else {

				if (advertisedDevice.haveTXPower()) {
					distance = calculateDistance(rssi, advertisedDevice.getTXPower());
					doc["txPower"] = advertisedDevice.getTXPower();
				} else {
					distance = calculateDistance(rssi, defaultTxPower);
				}

				doc["distance"] = distance;

				// Serial.printf("strManufacturerData: %d \n\r",strManufacturerData.length());
				// TODO: parse manufacturer data

			}
		 } else {

			if (advertisedDevice.haveTXPower()) {
				distance = calculateDistance(rssi, advertisedDevice.getTXPower());
				doc["txPower"] = advertisedDevice.getTXPower();
				doc["distance"] = distance;
			} else {
				distance = calculateDistance(rssi, defaultTxPower);
				doc["distance"] = distance;
			}

			// Serial.printf("no Beacon Advertised ServiceDataUUID: %d %s \n\r", advertisedDevice.getServiceDataUUID().bitSize(), advertisedDevice.getServiceDataUUID().toString().c_str());
		 }
		}

		char JSONmessageBuffer[512];
		serializeJson(doc, JSONmessageBuffer);

		String publishTopic = String(channel) + "/" + room;

		if (mqttClient.connected()) {
			if (maxDistance == 0 || doc["distance"] < maxDistance) {
				if (mqttClient.publish((char *)publishTopic.c_str(), 0, 0, JSONmessageBuffer) == true) {

			    // Serial.print("Success sending message to topic: "); Serial.println(publishTopic);
					return true;

			  } else {
			    Serial.print("Error sending message: ");
					Serial.println(publishTopic);
			    Serial.print("Message: ");
					Serial.println(JSONmessageBuffer);
					return false;
			  }
			} else {
				Serial.printf("%s exceeded distance threshold %.2f\n\r", mac_address.c_str(), distance);
				return false;
			}

		} else {

			Serial.println("MQTT disconnected.");
			if (xTimerIsTimerActive(mqttReconnectTimer) != pdFALSE) {
				TickType_t xRemainingTime = xTimerGetExpiryTime( mqttReconnectTimer ) - xTaskGetTickCount();
				Serial.print("Time remaining: ");
				Serial.println(xRemainingTime);
			} else {
				handleMqttDisconnect();
			}
		}
		return false;
}

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {

	void onResult(BLEAdvertisedDevice advertisedDevice) {

		digitalWrite(LED_GPIO, LED_ON);
		vTaskDelay(10 / portTICK_PERIOD_MS);
		digitalWrite(LED_GPIO, !LED_ON);

	}

};

void scanForDevices(void * parameter) {
	while(1) {
	    voltage += analogRead(VIN_GPIO);
	    loopCount += 1;
		if (!updateInProgress && WiFi.isConnected() && (millis() - last > (waitTime * 1000) || last == 0)) {
			powerOn = analogRead(POWER_GPIO);
	        voltage = voltage / loopCount;
			Serial.print("Scanning...\t");
			BLEScanResults foundDevices = pBLEScan->start(scanTime);
			int devicesCount = foundDevices.getCount();
	    Serial.printf("Scan done! Devices found: %d\n\r",devicesCount);

			int devicesReported = 0;
			if (mqttClient.connected()) {
			  for (uint32_t i = 0; i < devicesCount; i++) {
					bool included = reportDevice(foundDevices.getDevice(i));
					if (included) {
						devicesReported++;
					}
				}
#ifdef Deep_sleep
				sendTelemetry(devicesCount, devicesReported, voltage, loopCount, powerOn);
#else
				sendTelemetry(devicesCount, devicesReported);
#endif
				pBLEScan->clearResults();
#ifdef BME280_enable
				if ((millis() - lastBME280 > 30000) && BME280_status) {
					sendBME280();
					lastBME280 = millis();
				}
				else {
					if (debug) mqttClient.publish(debugTopic, 0, 0, "BME280 info not sent");
				}
#endif                                                          
			} else {
				Serial.println("Cannot report; mqtt disconnected");
				if (xTimerIsTimerActive(mqttReconnectTimer) != pdFALSE) {
					TickType_t xRemainingTime = xTimerGetExpiryTime( mqttReconnectTimer ) - xTaskGetTickCount();
					Serial.print("Time remaining: ");
					Serial.println(xRemainingTime);
				} else {
					handleMqttDisconnect();
				}
			}
			loopCount = 0;
			voltage = 0;
			last = millis();
	  }
#ifdef Deep_sleep
		if (powerOn < 300 && !updateInProgress) {
			if (((millis() - lastSleep > 60000) && WiFi.isConnected() && mqttClient.connected()) || (millis() - lastSleep > 120000)) {
				mqttClient.publish(availabilityTopic, 0, 1, "SLEEPING");
				Serial.println("Going to sleep in 5 seconds");
				delay(5000);
				esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_M_FACTOR);
				Serial.println("Going to sleep for " + String(TIME_TO_SLEEP) + " Hours");
				Serial.flush(); 
				esp_deep_sleep_start();
			}
		}
		else {
			lastSleep = millis();
		}
#endif                                                          
	}
}

void configureOTA() {
	ArduinoOTA
    .onStart([]() {
			Serial.println("OTA Start");
			pBLEScan->stop();
			updateInProgress = true;
			mqttClient.disconnect(true);
			xTimerStop(mqttReconnectTimer, 0); // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
    })
    .onEnd([]() {
			updateInProgress = false;
			digitalWrite(LED_GPIO, !LED_ON);
      Serial.println("\n\rEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
			byte percent = (progress / (total / 100));
      Serial.printf("Progress: %u", percent);
      Serial.println("");
			digitalWrite(LED_GPIO, percent % 2);
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
			ESP.restart();
    });
	ArduinoOTA.setHostname(hostname);
	ArduinoOTA.setPort(8266);
  ArduinoOTA.begin();
}

void setup() {

  Serial.begin(115200);

	pinMode(LED_GPIO, OUTPUT);
	digitalWrite(LED_GPIO, LED_ON);
#ifdef Deep_sleep
	pinMode (VIN_GPIO, INPUT);
	pinMode (POWER_GPIO, INPUT);

	esp_sleep_enable_ext0_wakeup(POWER_GPIO,1); //1 = High, 0 = Low
#endif                                                          

  #ifdef BME280_enable
    // default settings
    BME280_status = bme.begin();  
    // You can also pass in a Wire library object like &Wire2
    // status = bme.begin(0x76, &Wire2)
    if (!BME280_status) {
        Serial.println("Could not find a valid BME280 sensor, check wiring, address, sensor ID!");
        Serial.print("SensorID was: 0x"); Serial.println(bme.sensorID(),16);
        Serial.print("        ID of 0xFF probably means a bad address, a BMP 180 or BMP 085\n");
        Serial.print("   ID of 0x56-0x58 represents a BMP 280,\n");
        Serial.print("        ID of 0x60 represents a BME 280.\n");
        Serial.print("        ID of 0x61 represents a BME 680.\n");
        //while (1) delay(10);
    }

    bme.setSampling(Adafruit_BME280::MODE_NORMAL,
                    Adafruit_BME280::SAMPLING_X16,  // temperature
                    Adafruit_BME280::SAMPLING_X16, // pressure
                    Adafruit_BME280::SAMPLING_X16,  // humidity
                    Adafruit_BME280::FILTER_X16,
                    //Adafruit_BME280::FILTER_OFF,
                    Adafruit_BME280::STANDBY_MS_0_5 );
#endif                      

  mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToMqtt));
  wifiReconnectTimer = xTimerCreate("wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, reinterpret_cast<TimerCallbackFunction_t>(connectToWifi));

  WiFi.onEvent(WiFiEvent);

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);

  connectToWifi();

	configureOTA();

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan(); //create new scan
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(activeScan);
	pBLEScan->setInterval(bleScanInterval);
	pBLEScan->setWindow(bleScanWindow);

	xTaskCreatePinnedToCore(
		scanForDevices,
		"BLE Scan",
		4096,
		pBLEScan,
		1,
		&BLEScan,
		1);

}

void loop() {
	TIMERG0.wdt_wprotect=TIMG_WDT_WKEY_VALUE;
	TIMERG0.wdt_feed=1;
	TIMERG0.wdt_wprotect=0;
	ArduinoOTA.handle();
}
