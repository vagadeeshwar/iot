// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/*
 * This is an Arduino-based Azure IoT Hub sample for ESPRESSIF ESP8266 board.
 * It uses our Azure Embedded SDK for C to help interact with Azure IoT.
 * For reference, please visit https://github.com/azure/azure-sdk-for-c.
 *
 * To connect and work with Azure IoT Hub you need an MQTT client, connecting, subscribing
 * and publishing to specific topics to use the messaging features of the hub.
 * Our azure-sdk-for-c is an MQTT client support library, helping to compose and parse the
 * MQTT topic names and messages exchanged with the Azure IoT Hub.
 *
 * This sample performs the following tasks:
 * - Synchronize the device clock with a NTP server;
 * - Initialize our "az_iot_hub_client" (struct for data, part of our azure-sdk-for-c);
 * - Initialize the MQTT client (here we use Nick Oleary's PubSubClient, which also handle the tcp
 * connection and TLS);
 * - Connect the MQTT client (using server-certificate validation, SAS-tokens for client
 * authentication);
 * - Periodically send telemetry data to the Azure IoT Hub.
 *
 * To properly connect to your Azure IoT Hub, please fill the information in the `iot_configs.h`
 * file.
 */

//Command to observe telemetry sent to azure iothub via cli
//  az iot hub monitor-events --login 'HostName=I-o-T.azure-devices.net;SharedAccessKeyName=iothubowner;SharedAccessKey=7d/psPhy1fpSKt+pZaB2a9ij13J2rJFKiAIoTNS0KWk=' --device-id 3 

// C99 libraries
#include <cstdlib>
#include <stdbool.h>
#include <string.h>
#include <time.h>

// Libraries for MQTT client, WiFi connection and SAS-token generation.
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <bearssl/bearssl.h>
#include <bearssl/bearssl_hmac.h>
#include <libb64/cdecode.h>

// Azure IoT SDK for C includes
#include <az_core.h>
#include <az_iot.h>
#include <azure_ca.h>

// Sensor Headers
#include <Adafruit_Sensor.h>
#include<EEPROM.h>

// Additional sample headers
#include "iot_configs.h"

// When developing for your own Arduino-based platform,
// please follow the format '(ard;<platform>)'.
#define AZURE_SDK_CLIENT_USER_AGENT "c%2F" AZ_SDK_VERSION_STRING "(ard;esp8266)"

// Utility macros and defines
#define sizeofarray(a) (sizeof(a) / sizeof(a[0]))
#define ONE_HOUR_IN_SECS 3600
#define NTP_SERVERS "pool.ntp.org", "time.nist.gov"
#define MQTT_PACKET_SIZE 1024

// Translate iot_configs.h defines into variables used by the sample
static const char* ssid = IOT_CONFIG_WIFI_SSID;
static const char* password = IOT_CONFIG_WIFI_PASSWORD;
static const char* host = IOT_CONFIG_IOTHUB_FQDN;
static const char* device_id = IOT_CONFIG_DEVICE_ID;
static const char* device_key = IOT_CONFIG_DEVICE_KEY;
static const int port = 8883;

// Memory allocated for the sample's variables and structures.
static WiFiClientSecure wifi_client;
static X509List cert((const char*)ca_pem);
static PubSubClient mqtt_client(wifi_client);
static az_iot_hub_client client;
static char sas_token[200];
static uint8_t signature[512];
static unsigned char encrypted_signature[32];
static char base64_decoded_device_key[32];
static unsigned long next_telemetry_send_time_ms = 0;
static char telemetry_topic[128];
static uint8_t telemetry_payload[100];
static uint32_t telemetry_send_count = 0;

#define sensor1 D0 //Entry                       //Assumptions: One Exit One Entry Strictly Followed
#define sensor2 D1 //Exit                        //If the setup is taken off power, data is saved and saved with the help of EEPROM 
// #define led 7 //Blinks whenever someone enters or leaves
#define ledr D2 //Noone can enter
#define ledg D3 //Someone can enter
#define ledb D4 
#define buzzer D5 // Beeps when someone enters when current=allowed   Calls a separate function which triggers continuous beeps until allowed=current

int allowed, current; //allowed can be changed using serial monitor
String inputString = "";         // A string to hold incoming data
boolean stringComplete = false;  // Whether the string is complete

// Auxiliary functions
static void connectToWiFi()
{
  Serial.begin(115200);
  Serial.println();
  Serial.print("Connecting to WIFI SSID ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.print("WiFi connected, IP address: ");
  Serial.println(WiFi.localIP());
}

static void initializeTime()
{
  Serial.print("Setting time using SNTP");

  configTime(-5 * 3600, 0, NTP_SERVERS);
  time_t now = time(NULL);
  while (now < 1510592825)
  {
    delay(500);
    Serial.print(".");
    now = time(NULL);
  }
  Serial.println("done!");
}

static char* getCurrentLocalTimeString()
{
  time_t now = time(NULL);
  return ctime(&now);
}

static void printCurrentTime()
{
  Serial.print("Current time: ");
  Serial.print(getCurrentLocalTimeString());
}

static void initializeClients()
{
  az_iot_hub_client_options options = az_iot_hub_client_options_default();
  options.user_agent = AZ_SPAN_FROM_STR(AZURE_SDK_CLIENT_USER_AGENT);

  wifi_client.setTrustAnchors(&cert);
  if (az_result_failed(az_iot_hub_client_init(
          &client,
          az_span_create((uint8_t*)host, strlen(host)),
          az_span_create((uint8_t*)device_id, strlen(device_id)),
          &options)))
  {
    Serial.println("Failed initializing Azure IoT Hub client");
    return;
  }

  mqtt_client.setServer(host, port);
  mqtt_client.setCallback(receivedCallback);
}

/*
 * @brief           Gets the number of seconds since UNIX epoch until now.
 * @return uint32_t Number of seconds.
 */
static uint32_t getSecondsSinceEpoch() { return (uint32_t)time(NULL); }

static int generateSasToken(char* sas_token, size_t size)
{
  az_span signature_span = az_span_create((uint8_t*)signature, sizeofarray(signature));
  az_span out_signature_span;
  az_span encrypted_signature_span
      = az_span_create((uint8_t*)encrypted_signature, sizeofarray(encrypted_signature));

  uint32_t expiration = getSecondsSinceEpoch() + ONE_HOUR_IN_SECS;

  // Get signature
  if (az_result_failed(az_iot_hub_client_sas_get_signature(
          &client, expiration, signature_span, &out_signature_span)))
  {
    Serial.println("Failed getting SAS signature");
    return 1;
  }

  // Base64-decode device key
  int base64_decoded_device_key_length
      = base64_decode_chars(device_key, strlen(device_key), base64_decoded_device_key);

  if (base64_decoded_device_key_length == 0)
  {
    Serial.println("Failed base64 decoding device key");
    return 1;
  }

  // SHA-256 encrypt
  br_hmac_key_context kc;
  br_hmac_key_init(
      &kc, &br_sha256_vtable, base64_decoded_device_key, base64_decoded_device_key_length);

  br_hmac_context hmac_ctx;
  br_hmac_init(&hmac_ctx, &kc, 32);
  br_hmac_update(&hmac_ctx, az_span_ptr(out_signature_span), az_span_size(out_signature_span));
  br_hmac_out(&hmac_ctx, encrypted_signature);

  // Base64 encode encrypted signature
  String b64enc_hmacsha256_signature = base64::encode(encrypted_signature, br_hmac_size(&hmac_ctx));

  az_span b64enc_hmacsha256_signature_span = az_span_create(
      (uint8_t*)b64enc_hmacsha256_signature.c_str(), b64enc_hmacsha256_signature.length());

  // URl-encode base64 encoded encrypted signature
  if (az_result_failed(az_iot_hub_client_sas_get_password(
          &client,
          expiration,
          b64enc_hmacsha256_signature_span,
          AZ_SPAN_EMPTY,
          sas_token,
          size,
          NULL)))
  {
    Serial.println("Failed getting SAS token");
    return 1;
  }

  return 0;
}

static int connectToAzureIoTHub()
{
  size_t client_id_length;
  char mqtt_client_id[128];
  if (az_result_failed(az_iot_hub_client_get_client_id(
          &client, mqtt_client_id, sizeof(mqtt_client_id) - 1, &client_id_length)))
  {
    Serial.println("Failed getting client id");
    return 1;
  }

  mqtt_client_id[client_id_length] = '\0';

  char mqtt_username[128];
  // Get the MQTT user name used to connect to IoT Hub
  if (az_result_failed(az_iot_hub_client_get_user_name(
          &client, mqtt_username, sizeofarray(mqtt_username), NULL)))
  {
    printf("Failed to get MQTT clientId, return code\n");
    return 1;
  }

  Serial.print("Client ID: ");
  Serial.println(mqtt_client_id);

  Serial.print("Username: ");
  Serial.println(mqtt_username);

  mqtt_client.setBufferSize(MQTT_PACKET_SIZE);

  while (!mqtt_client.connected())
  {
    time_t now = time(NULL);

    Serial.print("MQTT connecting ... ");

    if (mqtt_client.connect(mqtt_client_id, mqtt_username, sas_token))
    {
      Serial.println("connected.");
    }
    else
    {
      Serial.print("failed, status code =");
      Serial.print(mqtt_client.state());
      Serial.println(". Trying again in 5 seconds.");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }

  mqtt_client.subscribe(AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC);

  return 0;
}

static void establishConnection()
{
  connectToWiFi();
  initializeTime();
  printCurrentTime();
  initializeClients();

  // The SAS token is valid for 1 hour by default in this sample.
  // After one hour the sample must be restarted, or the client won't be able
  // to connect/stay connected to the Azure IoT Hub.
  if (generateSasToken(sas_token, sizeofarray(sas_token)) != 0)
  {
    Serial.println("Failed generating MQTT password");
  }
  else
  {
    connectToAzureIoTHub();
  }

  // digitalWrite(LED_PIN, LOW);
}

static char* getTelemetryPayload(int current, int allowed)
{
  az_span temp_span = az_span_create(telemetry_payload, sizeof(telemetry_payload));
  
  temp_span = az_span_copy(temp_span, AZ_SPAN_FROM_STR("{ \"msgCount\": "));
  (void)az_span_u32toa(temp_span, telemetry_send_count++, &temp_span);

  temp_span = az_span_copy(temp_span, AZ_SPAN_FROM_STR(", \"current\": "));
  char curr_str[8];
  sprintf(curr_str, "%d", current); // Convert the integer 'current' to a string and store it in curr_str.
  temp_span = az_span_copy(temp_span, az_span_create_from_str(curr_str));


  temp_span = az_span_copy(temp_span, AZ_SPAN_FROM_STR(", \"allowed\": "));
  char allow_str[8];
  sprintf(allow_str, "%d", allowed);
  temp_span = az_span_copy(temp_span, az_span_create_from_str(allow_str));

  temp_span = az_span_copy(temp_span, AZ_SPAN_FROM_STR(" }"));
  temp_span = az_span_copy_u8(temp_span, '\0');

  return (char*)telemetry_payload;
}

static void sendTelemetry(int current, int allowed)
{
  // digitalWrite(LED_PIN, HIGH);
  Serial.print(millis());
  Serial.print(" ESP8266 Sending telemetry . . . ");
  if (az_result_failed(az_iot_hub_client_telemetry_get_publish_topic(
          &client, NULL, telemetry_topic, sizeof(telemetry_topic), NULL)))
  {
    Serial.println("Failed az_iot_hub_client_telemetry_get_publish_topic");
    return;
  }

  mqtt_client.publish(telemetry_topic, getTelemetryPayload(current, allowed), false);
  Serial.println("OK");
  // delay(100);
  // digitalWrite(LED_PIN, LOW);
}

void receivedCallback(char* topic, byte* payload, unsigned int length)
{
  Serial.print("Received [");
  Serial.print(topic);
  Serial.print("]: ");
  
  String message = "";
  for (int i = 0; i < length; i++)
  {
    message += (char)payload[i];
    Serial.print((char)payload[i]);
  }
  Serial.println("");

  // Process the message
  if (message != "")
    allowed=atoi(message.c_str());
}

void setup()
{
  pinMode(sensor1,INPUT);
  pinMode(sensor2,INPUT);
  // pinMode(led, OUTPUT);
  pinMode(ledr,OUTPUT);
  pinMode(ledg,OUTPUT);
  pinMode(ledb,OUTPUT);
  pinMode(buzzer, OUTPUT);

  establishConnection();

  EEPROM.begin(2*sizeof(int));

  // EEPROM.put(0, 0);
  // EEPROM.put(sizeof(current), 4);
  // EEPROM.commit();

  load_current();
  load_allowed();
  if (!mqtt_client.connected())
    {
      establishConnection();
    }
    sendTelemetry(current,allowed);

  Serial.print("Use the Serial Monitor to change maximum allowed customers!\n");
  Serial.print("Number of persons inside: ");
  Serial.println(current);
  Serial.print("\n");
  Serial.print("Number of persons allowed: ");
  Serial.println(allowed);
  Serial.print("\n");
  
  led_rgb();
  buzzer_(); //Checks for buzzer when power is resumed

  inputString.reserve(200); // Reserve 200 bytes for the inputString
}

void loop()
{
  entry_();
  exit_();
  if (stringComplete) {
    // Convert string to integer
    int receivedValue = inputString.toInt();
    
    // Check if the value is not zero and update
    if(receivedValue != 0) {
      allowed = receivedValue;
      Updateload();
      buzzer_();
      led_rgb();

      Serial.print("Current allowed value is: ");
      Serial.println(allowed);
      Serial.print("\n");
      Serial.print("Use the Serial Monitor to change maximum allowed customers!\n");

      if (!mqtt_client.connected())
      {
        establishConnection();
      }
    sendTelemetry(current,allowed);
    }
    // Clear the string for new input
    inputString = "";
    stringComplete = false;
  }
  // MQTT loop must be called to process Device-to-Cloud and Cloud-to-Device.
  mqtt_client.loop();
  delay(200);
}

void entry_()
{ if(digitalRead(sensor1)==LOW){
   if(current<allowed)
    {
      ++current;
    Updatecurrent();
    Serial.print("Someone has Entered\n");
    Serial.print("Number of persons inside: ");
    Serial.println(current);
    Serial.print("\n");
    led_rgb();
    // led_();
    }
   else
   {
    ++current;
    Updatecurrent();
    Serial.print("Someone has Entered without permission!\n");
    Serial.print("Number of persons inside: ");
    Serial.println(current);
    Serial.print("\n");
    buzzer_();
    // led_();
   }
    // Check if connected, reconnect if needed.
    if (!mqtt_client.connected())
    {
      establishConnection();
    }
    sendTelemetry(current,allowed);
   }
}

void exit_()
{
  if(digitalRead(sensor2)==LOW)
    {
      if(current==0) return;
      --current;
    Updatecurrent();
    Serial.print("Someone has Left\n");
    Serial.print("Number of persons inside: ");
    Serial.println(current);
    Serial.print("\n");
    led_rgb();
    buzzer_();
    // led_();
     // Check if connected, reconnect if needed.
    if (!mqtt_client.connected())
    {
      establishConnection();
    }
    sendTelemetry(current,allowed);
    }
}

void led_rgb()
{
  if(current<allowed)
    {analogWrite(ledg, 0);
    analogWrite(ledr, 255);
    analogWrite(ledb, 255);}
  else 
    {analogWrite(ledr, 0);
    analogWrite(ledg, 255);
    analogWrite(ledb, 255);}
}

// void led_()
// {
//   digitalWrite(led, HIGH);
//   delay(100);
//   digitalWrite(led, LOW);
// }

void buzzer_()
{
    if(current>allowed)
    digitalWrite(buzzer,HIGH);
    else
    digitalWrite(buzzer,LOW);
}

void Updatecurrent()
{
    EEPROM.put(0, current);
    EEPROM.commit();
    }

void Updateload()
{
    EEPROM.put(sizeof(current), allowed);
    EEPROM.commit();
    }

void load_current()
{
    EEPROM.get(0, current);}

void load_allowed()
{
    EEPROM.get(sizeof(current), allowed);
}   

void serialEvent() {
  while (Serial.available()) {
    // Get the new byte
    char inChar = (char)Serial.read();

    // If the incoming character is a newline, set the flag
    if (inChar == '\n') {
      stringComplete = true;
    } else {
      // Otherwise, add it to the input string
      inputString += inChar;
    }
  }
}

