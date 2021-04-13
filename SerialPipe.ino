/*
 * SerialPipe Logging Tool
 * Copyright (c) 2020 Chris Matthews.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 * 
 * 
 * INSTRUCTIONS
 * To install ESP8266 in Arduino IDE
 * Start Arduino and open the Preferences window.
 * Enter https://arduino.esp8266.com/stable/package_esp8266com_index.json 
 * into the File>Preferences>Additional Boards Manager URLs field of the Arduino IDE. 
 * You can add multiple URLs, separating them with commas.
 * Open Boards Manager from Tools -> Board menu and install esp8266 platform 
 * (and don't forget to select your ESP8266 board from Tools -> Board menu after installation).
 * Once installed, select proper board, with Tools -> Boards -> ESP8266
 * 
 * USAGE
 * During runtime, the serial.swap() is used to switch the serial port to use
 * GPIO15/D8(TX) and GPIO13/D7(RX) instead of the normal GPIO1(TX) and GPIO3(RX).
 * This ensure that once bootup is done the USB-UART converter is disconnected and free
 * UART for use for incoming/outgoing serialpipe connection
 * 
 */
#include <ESP8266WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include <algorithm> // std::min
#include <SoftwareSerial.h>


// Program Version
#define FW_VERSION_MAJOR  1
#define FW_VERSION_MINOR  2
#define FW_VERSION_PATCH  0


/********************************* DEFINES *********************************/
// Key that will start logger in "configuration" mode
#define CONFIGURATION_KEY ('c')

// Device under test baudrate
#define DEFAULT_BAUD_DUT    115200

// Debugging console for the logger itself (no DUT logs are logged here)
#define DEFAULT_BAUD_LOGGER 115200

#define RXBUFFERSIZE 1024
#define STACK_PROTECTOR  512 // bytes

#define DEFAULT_SSID "default"
#define DEFAULT_PSK  "undefined"

// Default port to listen for incoming TCP connections
#define DEFAULT_LISTEN_PORT 23

// Default Wifi TX power (from 0.0 to 20.5)
#define DEFAULT_TX_POWER (12.0)

// How many clients should be able to telnet to this ESP8266
#define MAX_SRV_CLIENTS 2

// Configuration mode baud rate (this is non-configurable via the configuration console)
#define CONFIG_CONSOLE_BAUD_SERIAL      115200
// Milliseconds to wait for user input to switch to configuration mode before starting serial logger mode
#define CONFIG_CONSOLE_ENTRY_DELAY_MS   1000

// LED blink rates
#define STATUS_LED_ON_MS  100
#define STATUS_LED_OFF_MS 5000
#define USER_INPUT_LED_ON_MS  150
#define USER_INPUT_LED_OFF_MS 150


/***********************************  TYPES  ********************************/
typedef struct wifi_settings_t {
  char ssid[32];
  char psk[64];
  float tx_power;
  int listen_port;
} wifi_settings_t;

typedef struct uart_settings_t {
  uint32_t log_baud_rate;
  uint32_t dut_baud_rate;
} uart_settings_t;

typedef struct serialpipe_config_t {
  wifi_settings_t wifi;
  uart_settings_t uart;
} serialpipe_config_t;

typedef struct serialpipe_config_file_t {
  serialpipe_config_t config;
  uint8_t crc;
} serialpipe_config_file_t;


/************************************ VARIABLES ******************************/
SoftwareSerial* logger = nullptr;

int status_led_state = HIGH;
uint32_t status_led_change_ms = 0;

WiFiServer server(DEFAULT_LISTEN_PORT);
WiFiClient serverClients[MAX_SRV_CLIENTS];

bool configuration_mode = false;
bool configuration_good = false;
serialpipe_config_t serialpipe_config;


/************************************ CODE ***********************************/
void setup() {
    Serial.begin(CONFIG_CONSOLE_BAUD_SERIAL);
    Serial.setRxBufferSize(RXBUFFERSIZE);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN,HIGH);

    Serial.printf("\n\nSerialPipe\nVersion %d.%d.%d\n\n", FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH);
    Serial.printf("MAC Address ");
    Serial.println(WiFi.macAddress());
    load_config();
    Serial.printf("\n\nPress '%c' to enter configuration console...\n", CONFIGURATION_KEY);
    char ch;
    while((ch = Serial.read()) != CONFIGURATION_KEY) {
      ESP.wdtFeed();
      if (millis() >= CONFIG_CONSOLE_ENTRY_DELAY_MS) break;
    }

    if (ch == CONFIGURATION_KEY) {
      setupConfigurationConsole();
    }
    else {
      setupSerialPipe();
    }
}

void loop() {
  if (configuration_mode == true) {
    configurationConsole();
  }
  else {
    serialPipe();
  }
}

/*
 * Setup code for serial logging mode
 */
void setupSerialPipe() {
  Serial.begin(serialpipe_config.uart.dut_baud_rate);
  Serial.setRxBufferSize(RXBUFFERSIZE);
  digitalWrite(LED_BUILTIN,LOW);

  Serial.swap();
  // Hardware serial is now on RX:GPIO13 TX:GPIO15
  // use SoftwareSerial on regular RX(3)/TX(1) for logging
  logger = new SoftwareSerial(3, 1);
  logger->begin(serialpipe_config.uart.log_baud_rate);
  logger->enableIntTx(false);
  logger->printf("ESP8266 Version: ");
  logger->println(ESP.getFullVersion());
  logger->printf("Serial baud: %d (8n1: %d KB/s)\n", serialpipe_config.uart.dut_baud_rate, serialpipe_config.uart.log_baud_rate * 8 / 10 / 1024);
  logger->printf("Serial receive buffer size: %d bytes\n", RXBUFFERSIZE);

  WiFi.setOutputPower(DEFAULT_TX_POWER);  
  logger->printf("ESP8266 Version: ");
  WiFi.mode(WIFI_STA);
  WiFi.begin(serialpipe_config.wifi.ssid, serialpipe_config.wifi.psk);
  logger->printf("\nTX Power set to %.1f", serialpipe_config.wifi.tx_power);
  logger->print("\nConnecting to ");
  logger->println(serialpipe_config.wifi.ssid);
  while (WiFi.status() != WL_CONNECTED) {
    logger->print('.');
    delay(500);
  }
  logger->println();
  logger->print("connected, address=");
  logger->println(WiFi.localIP());

  //start server
  server.begin(serialpipe_config.wifi.listen_port);
  server.setNoDelay(true);

  logger->print("Ready! Use 'telnet ");
  logger->print(WiFi.localIP());
  logger->printf(" %d' to connect\n", serialpipe_config.wifi.listen_port);
}

/*
 * Setup code for configuration mode
 */
void setupConfigurationConsole() {
    Serial.begin(CONFIG_CONSOLE_BAUD_SERIAL);
    Serial.printf("\n\nCONFIGURATION MODE\n\n");
    configuration_mode = true;
}

/**
 * Serial logger mode logic
 * 
 * Note that this function will be called by loop() multiple times, hence no 
 * infinite while() loop should be present in this function.
 */
void serialPipe() {

  if (status_led_change_ms <= millis()) {
    if (status_led_state == HIGH) {
      status_led_state = LOW;
      status_led_change_ms = STATUS_LED_ON_MS + millis();
    }
    else {
      status_led_state = HIGH;
      status_led_change_ms = STATUS_LED_OFF_MS + millis();
    }
    digitalWrite(LED_BUILTIN, status_led_state);
  }

  // Check if there is activity on the debug console (i.e. the user would like to switch to config mode for example
  if (logger->read() == CONFIGURATION_KEY) {
    logger->printf("Switching to configuration mode...\n");
    Serial.swap();
    Serial.begin(CONFIG_CONSOLE_BAUD_SERIAL);
    configuration_mode = true;
    return;
  }

  //check if there are any new clients
  if (server.hasClient()) {
    //find free/disconnected spot
    int i;
    for (i = 0; i < MAX_SRV_CLIENTS; i++)
      if (!serverClients[i]) { // equivalent to !serverClients[i].connected()
        serverClients[i] = server.available();
        logger->print("New client: index ");
        logger->print(i);
        break;
      }

    //no free/disconnected spot so reject
    if (i == MAX_SRV_CLIENTS) {
      server.available().println("busy");
      // hints: server.available() is a WiFiClient with short-term scope
      // when out of scope, a WiFiClient will
      // - flush() - all data will be sent
      // - stop() - automatically too
      logger->printf("server is busy with %d active connections\n", MAX_SRV_CLIENTS);
    }
  }

  //check TCP clients for data
  // Incredibly, this code is faster than the bufferred one below - #4620 is needed
  // loopback/3000000baud average 348KB/s
  for (int i = 0; i < MAX_SRV_CLIENTS; i++)
    while (serverClients[i].available() && Serial.availableForWrite() > 0) {
      // working char by char is not very efficient
      Serial.write(serverClients[i].read());
    }

  // determine maximum output size "fair TCP use"
  // client.availableForWrite() returns 0 when !client.connected()
  size_t maxToTcp = 0;
  for (int i = 0; i < MAX_SRV_CLIENTS; i++)
    if (serverClients[i]) {
      size_t afw = serverClients[i].availableForWrite();
      if (afw) {
        if (!maxToTcp) {
          maxToTcp = afw;
        } else {
          maxToTcp = std::min(maxToTcp, afw);
        }
      } else {
        // warn but ignore congested clients
        logger->println("one client is congested");
      }
    }

  //check UART for data
  size_t len = std::min((size_t)Serial.available(), maxToTcp);
  len = std::min(len, (size_t)STACK_PROTECTOR);
  if (len) {
    uint8_t sbuf[len];
    size_t serial_got = Serial.readBytes(sbuf, len);
    // push UART data to all connected telnet clients
    for (int i = 0; i < MAX_SRV_CLIENTS; i++)
      // if client.availableForWrite() was 0 (congested)
      // and increased since then,
      // ensure write space is sufficient:
      if (serverClients[i].availableForWrite() >= serial_got) {
        size_t tcp_sent = serverClients[i].write(sbuf, serial_got);
        if (tcp_sent != len) {
          logger->printf("len mismatch: available:%zd serial-read:%zd tcp-write:%zd\n", len, serial_got, tcp_sent);
        }
      }
  }
}

/**
 * Configuration mode logic
 * 
 * Prompts user to enter configuration details, saves to flash and resets logger device.
 */
void configurationConsole() {
  String new_ssid;
  String new_psk;
  int new_listen_port;
  String temp_num;
  int new_dut_baud;
  int new_log_baud;
  float new_tx_power;
  bool user_confirm;
  digitalWrite(LED_BUILTIN,LOW);

  delay(2000);

  // dump any remaining characters in the serial buffer
  while(Serial.available() > 0) {
     Serial.read();
     ESP.wdtFeed();
  }

  Serial.printf("Mounting LittleFS\n");
  if (!LittleFS.begin()) {
    Serial.printf("Failed to mount LittleFS\n");
    Serial.printf("Formatting LittleFS filesystem\n");
    LittleFS.format();
    Serial.printf("Retrying Mount LittleFS\n");
    if (!LittleFS.begin()) {
      Serial.printf("LittleFS mount failed\n");
      while(1); // die here
    }
  }

  Serial.setTimeout(6000000);

  Serial.printf("\nEnter SSID: ");
  new_ssid.remove(0,new_ssid.length());
  console_read(new_ssid);
  
  Serial.printf("\nEnter pre-shared key: ");
  new_psk.remove(0,new_psk.length());
  console_read(new_psk);
  
  Serial.printf("\nTX Power: ");
  temp_num.remove(0,temp_num.length());
  console_read(temp_num);
  new_tx_power = atof(temp_num.c_str());
  
  Serial.printf("\nListen port: ");
  temp_num.remove(0,temp_num.length());
  console_read(temp_num);
  new_listen_port = atoi(temp_num.c_str());
  
  Serial.printf("\nDUT Baud Rate: ");
  temp_num.remove(0,temp_num.length());
  console_read(temp_num);
  new_dut_baud = atoi(temp_num.c_str());
  
  Serial.printf("\nLog Baud Rate: ");
  temp_num.remove(0,temp_num.length());
  console_read(temp_num);
  new_log_baud = atoi(temp_num.c_str());

  Serial.printf("\n\n\nNew configuration:\n\n");
  Serial.printf("SSID:              [%s]\n", new_ssid.c_str());
  Serial.printf("Pre-Shared Key:    [%s]\n", new_psk.c_str());
  Serial.printf("TX Power:          [%.1f]\n", new_tx_power);
  Serial.printf("Listen port:       [%d]\n", new_listen_port);
  Serial.printf("DUT baud rate:     [%d]\n", new_dut_baud);
  Serial.printf("Log baud rate:     [%d]\n", new_log_baud);

  Serial.printf("\nStore configuration? [y/N]");
  user_confirm = false;
  while(!user_confirm) {
    switch(Serial.read()) {
      case 'y':
      case 'Y':
        memset(&serialpipe_config, 0, sizeof(serialpipe_config_t));
        strcpy(serialpipe_config.wifi.ssid, new_ssid.c_str());
        strcpy(serialpipe_config.wifi.psk, new_psk.c_str());
        serialpipe_config.wifi.tx_power = new_tx_power;
        serialpipe_config.wifi.listen_port = new_listen_port;
        serialpipe_config.uart.dut_baud_rate = new_dut_baud;
        serialpipe_config.uart.log_baud_rate = new_log_baud;
        print_configuration();
        save_config();
        Serial.printf("\n\nConfiguration stored.\n");
        user_confirm = true;
        break;
      case 'n':
      case 'N':
        Serial.printf("\n\nDiscarding changes.\n");
        user_confirm = true;
        break;
      default:
        break;
    }
    ESP.wdtFeed();
  }
  Serial.printf("Resetting...\n\n");
  delay(1000);
  digitalWrite(LED_BUILTIN,HIGH);
  ESP.restart();
}

void load_config() {
  serialpipe_config_file_t sp_file;
  Serial.printf("load_config: Mounting LittleFS\n");
  if (!LittleFS.begin()) {
    Serial.printf("load_config: Failed to mount LittleFS\n");
    Serial.printf("load_config: Formatting LittleFS filesystem\n");
    LittleFS.format();
    Serial.printf("load_config: Retrying Mount LittleFS\n");
    if (!LittleFS.begin()) {
      Serial.printf("load_config: LittleFS format/mount failed\n");
      Serial.flush();
      while(1); // die here
    }
  }

  File config_file = LittleFS.open("/serialpipe.conf", "r");
  size_t bytes_read = 0;
  if (config_file.size() == sizeof(serialpipe_config_file_t)) {
    bytes_read = config_file.readBytes((char *)&sp_file, (int)sizeof(serialpipe_config_file_t));

    if ( sp_file.crc != CRC8((uint8_t *)&(sp_file.config), sizeof(serialpipe_config_t))) {
      // clear bytes read which will trigger the default configuration to be copied into the current config below.
      bytes_read = 0;
    }
    
    memcpy(&serialpipe_config, &(sp_file.config), sizeof(serialpipe_config_t));
    Serial.printf("load_config: Read %d bytes from config file\n", bytes_read);
  }
  config_file.close();
  LittleFS.end();

  // set a "default" config
  if (bytes_read != sizeof(serialpipe_config_file_t)) {
    set_default_config();
    save_config();
    Serial.print("load_config: No config found, using Defaults\n");
  }
  Serial.printf("Loaded Config:\n\n");
  print_configuration();
  Serial.flush();
}

void print_configuration() {
  Serial.printf("Wifi:\n");
  Serial.printf("  SSID:        %s\n", serialpipe_config.wifi.ssid);
  Serial.printf("  PSK:         %s\n", serialpipe_config.wifi.psk);
  Serial.printf("  TX Power:    %.1f\n", serialpipe_config.wifi.tx_power);
  Serial.printf("  Listen port: %d\n", serialpipe_config.wifi.listen_port);
  Serial.printf("DUT Baud:      %d\n", serialpipe_config.uart.dut_baud_rate);
  Serial.printf("Log Baud:      %d\n", serialpipe_config.uart.log_baud_rate);
}

void save_config() {
  serialpipe_config_file_t sp_file;

  memcpy(&(sp_file.config), &serialpipe_config, sizeof(serialpipe_config_t));
  sp_file.crc = CRC8((uint8_t *)&serialpipe_config, sizeof(serialpipe_config_t));
  if (!LittleFS.begin()) {
    Serial.printf("save_config: Failed to mount LittleFS\n");
  }
  else {
    File config_file = LittleFS.open("/serialpipe.conf", "w");
    size_t bytes_written = 0;
    bytes_written = config_file.write((char *)&sp_file, (int)sizeof(serialpipe_config_file_t));
    Serial.printf("save_config: Wrote %d bytes to config file (crc=%02X)\n", bytes_written, sp_file.crc);
    config_file.close();
    LittleFS.end();
  }
}

void set_default_config() {
    memset(&serialpipe_config, 0, sizeof(serialpipe_config_t));
    memcpy(serialpipe_config.wifi.ssid, DEFAULT_SSID, sizeof(DEFAULT_SSID));
    memcpy(serialpipe_config.wifi.psk, DEFAULT_PSK, sizeof(DEFAULT_PSK));
    serialpipe_config.wifi.tx_power = DEFAULT_TX_POWER;
    serialpipe_config.wifi.listen_port = DEFAULT_LISTEN_PORT;
    serialpipe_config.uart.log_baud_rate = DEFAULT_BAUD_LOGGER;
    serialpipe_config.uart.dut_baud_rate = DEFAULT_BAUD_DUT;
}

void console_read(String& input) {
  char ch = Serial.read();
  status_led_change_ms = 0;
  status_led_state = HIGH;
  digitalWrite(LED_BUILTIN,HIGH);

  while(ch != '\n') {

    if (status_led_change_ms <= millis()) {
      if (status_led_state == HIGH) {
        status_led_state = LOW;
        status_led_change_ms = USER_INPUT_LED_ON_MS + millis();
      }
      else {
        status_led_state = HIGH;
        status_led_change_ms = USER_INPUT_LED_OFF_MS + millis();
      }
      digitalWrite(LED_BUILTIN, status_led_state);
    }

    if (ch == 0x08 && input.length() > 0) {
      input.remove(input.length()-1,1);
      Serial.write(ch);
    }
    else if (isPrintable(ch) && ch != '\n') {
      input += ch;
      Serial.write(ch);
    }
    ch = Serial.read();
    ESP.wdtFeed();
  }
}


// CRC-8 - based on the CRC8 formulas by Dallas/Maxim
// code released under the terms of the GNU GPL 3.0 license
// Copyright Leonardo Miliani, 2013
// https://www.leonardomiliani.com/en/2013/un-semplice-crc8-per-arduino/
byte CRC8(const byte *data, byte len) {
  byte crc = 0x00;
  while (len--) {
    byte extract = *data++;
    for (byte tempI = 8; tempI; tempI--) {
      byte sum = (crc ^ extract) & 0x01;
      crc >>= 1;
      if (sum) {
        crc ^= 0x8C;
      }
      extract >>= 1;
    }
  }
  return crc;
}
