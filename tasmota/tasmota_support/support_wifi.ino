/*
  support_wifi.ino - wifi support for Tasmota

  Copyright (C) 2021  Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*********************************************************************************************\
 * Wifi
\*********************************************************************************************/

// Enable one of three below options for wifi re-connection debugging
//#define WIFI_FORCE_RF_CAL_ERASE            // Erase rf calibration sector on restart only
//#define WIFI_RF_MODE_RF_CAL                // Set RF_MODE to RF_CAL for restart and deepsleep during user_rf_pre_init
//#define WIFI_RF_PRE_INIT                   // Set RF_MODE to RF_CAL for restart, deepsleep and power on during user_rf_pre_init

#ifndef WIFI_RSSI_THRESHOLD
#define WIFI_RSSI_THRESHOLD     10         // Difference in dB between current network and scanned network
#endif
#ifndef WIFI_RESCAN_MINUTES
#define WIFI_RESCAN_MINUTES     44         // Number of minutes between wifi network rescan
#endif
#ifndef WIFI_RETRY_SECONDS
#define WIFI_RETRY_SECONDS      20         // Number of seconds connection to wifi network will retry
#endif

const uint8_t WIFI_CONFIG_SEC = 180;       // seconds before restart
const uint8_t WIFI_CHECK_SEC = 20;         // seconds
const uint8_t WIFI_RETRY_OFFSET_SEC = WIFI_RETRY_SECONDS;  // seconds

#include <ESP8266WiFi.h>                   // Wifi, MQTT, Ota, WifiManager

int WifiGetRssiAsQuality(int rssi) {
  int quality = 0;

  if (rssi <= -100) {
    quality = 0;
  } else if (rssi >= -50) {
    quality = 100;
  } else {
    quality = 2 * (rssi + 100);
  }
  return quality;
}

//                                           0    1   2       3        4
const char kWifiEncryptionTypes[] PROGMEM = "OPEN|WEP|WPA/PSK|WPA2/PSK|WPA/WPA2/PSK"
#ifdef ESP32
//                                            5               6        7             8
                                            "|WPA2-Enterprise|WPA3/PSK|WPA2/WPA3/PSK|WAPI/PSK"
#endif  // ESP32
;

String WifiEncryptionType(uint32_t i) {
#ifdef ESP8266
  // Reference. WiFi.encryptionType =
  // 2 : ENC_TYPE_TKIP - WPA / PSK
  // 4 : ENC_TYPE_CCMP - WPA2 / PSK
  // 5 : ENC_TYPE_WEP  - WEP
  // 7 : ENC_TYPE_NONE - open network
  // 8 : ENC_TYPE_AUTO - WPA / WPA2 / PSK
  uint8_t typea[] = { 0,2,0,3,1,0,0,4 };
  int type = typea[WiFi.encryptionType(i) -1 &7];
#else
  int type = WiFi.encryptionType(i);
#endif
  if ((type < 0) || (type > 8)) { type = 0; }
  char stemp1[20];
  GetTextIndexed(stemp1, sizeof(stemp1), type, kWifiEncryptionTypes);
  return stemp1;
}

bool WifiConfigCounter(void)
{
  if (Wifi.config_counter) {
    Wifi.config_counter = WIFI_CONFIG_SEC;
  }
  return (Wifi.config_counter);
}

void WifiConfig(uint8_t type)
{
  if (!Wifi.config_type) {
    if ((WIFI_RETRY == type) || (WIFI_WAIT == type)) { return; }
#ifdef USE_EMULATION
    UdpDisconnect();
#endif  // USE_EMULATION
    WiFi.disconnect();                       // Solve possible Wifi hangs
    Wifi.config_type = type;

#ifndef USE_WEBSERVER
    if (WIFI_MANAGER == Wifi.config_type) {
      Wifi.config_type = WIFI_SERIAL;
    }
#endif  // USE_WEBSERVER

    Wifi.config_counter = WIFI_CONFIG_SEC;   // Allow up to WIFI_CONFIG_SECS seconds for phone to provide ssid/pswd
    Wifi.counter = Wifi.config_counter +5;
    TasmotaGlobal.blinks = 255;
    if (WIFI_RESTART == Wifi.config_type) {
      TasmotaGlobal.restart_flag = 2;
    }
    else if (WIFI_SERIAL == Wifi.config_type) {
      AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_WIFI D_WCFG_6_SERIAL " " D_ACTIVE_FOR_3_MINUTES));
    }
#ifdef USE_WEBSERVER
    else if (WIFI_MANAGER == Wifi.config_type || WIFI_MANAGER_RESET_ONLY == Wifi.config_type) {
      WifiManagerBegin(WIFI_MANAGER_RESET_ONLY == Wifi.config_type);
    }
#endif  // USE_WEBSERVER
  }
}

#ifdef CONFIG_IDF_TARGET_ESP32C3
  // https://github.com/espressif/arduino-esp32/issues/6264#issuecomment-1040147331
  // There's an include for this but it doesn't define the function if it doesn't think it needs it, so manually declare the function
extern "C" void phy_bbpll_en_usb(bool en);
#endif  // CONFIG_IDF_TARGET_ESP32C3

void WifiSetMode(WiFiMode_t wifi_mode) {
#ifdef CONFIG_IDF_TARGET_ESP32C3
  // https://github.com/espressif/arduino-esp32/issues/6264#issuecomment-1094376906
  // This brings the USB serial-jtag back to life. Suggest doing this immediately after wifi startup.
  phy_bbpll_en_usb(true);
#endif  // CONFIG_IDF_TARGET_ESP32C3
  if (WiFi.getMode() == wifi_mode) { return; }

  if (wifi_mode != WIFI_OFF) {
    WiFi.hostname(TasmotaGlobal.hostname);  // ESP32 needs this here (before WiFi.mode) for core 2.0.0

    // See: https://github.com/esp8266/Arduino/issues/6172#issuecomment-500457407
    WiFi.forceSleepWake(); // Make sure WiFi is really active.
    delay(100);
  }

  uint32_t retry = 2;
  while (!WiFi.mode(wifi_mode) && retry--) {
    AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_WIFI "Retry set Mode..."));
    delay(100);
  }

  if (wifi_mode == WIFI_OFF) {
    delay(1000);
    WiFi.forceSleepBegin();
    delay(1);
  } else {
    delay(30); // Must allow for some time to init.
  }
}

void WiFiSetSleepMode(void)
{
/* Excerpt from the esp8266 non os sdk api reference (v2.2.1):
 * Sets sleep type for power saving. Set WIFI_NONE_SLEEP to disable power saving.
 * - Default mode: WIFI_MODEM_SLEEP.
 * - In order to lower the power comsumption, ESP8266 changes the TCP timer
 *   tick from 250ms to 3s in WIFI_LIGHT_SLEEP mode, which leads to increased timeout for
 *   TCP timer. Therefore, the WIFI_MODEM_SLEEP or deep-sleep mode should be used
 *   where there is a requirement for the accurancy of the TCP timer.
 *
 * Sleep is disabled in core 2.4.1 and 2.4.2 as there are bugs in their SDKs
 * See https://github.com/arendst/Tasmota/issues/2559
 */

// Sleep explanation: https://github.com/esp8266/Arduino/blob/3f0c601cfe81439ce17e9bd5d28994a7ed144482/libraries/ESP8266WiFi/src/ESP8266WiFiGeneric.cpp#L255
/*
  if (TasmotaGlobal.sleep && Settings->flag3.sleep_normal) {  // SetOption60 - Enable normal sleep instead of dynamic sleep
    WiFi.setSleepMode(WIFI_LIGHT_SLEEP);        // Allow light sleep during idle times
  } else {
    WiFi.setSleepMode(WIFI_MODEM_SLEEP);        // Disable sleep (Esp8288/Arduino core and sdk default)
  }
*/
  bool wifi_no_sleep = Settings->flag5.wifi_no_sleep;
#ifdef CONFIG_IDF_TARGET_ESP32C3
  wifi_no_sleep = true;                         // Temporary patch for IDF4.4, wifi sleeping may cause wifi drops
#endif
  if (0 == TasmotaGlobal.sleep || wifi_no_sleep) {
    if (!TasmotaGlobal.wifi_stay_asleep) {
      WiFi.setSleepMode(WIFI_NONE_SLEEP);       // Disable sleep
    }
  } else {
    if (Settings->flag3.sleep_normal) {         // SetOption60 - Enable normal sleep instead of dynamic sleep
      WiFi.setSleepMode(WIFI_LIGHT_SLEEP);      // Allow light sleep during idle times
    } else {
      WiFi.setSleepMode(WIFI_MODEM_SLEEP);      // Sleep (Esp8288/Arduino core and sdk default)
    }
  }
  WifiSetOutputPower();
}

void WifiBegin(uint8_t flag, uint8_t channel) {
#ifdef USE_EMULATION
  UdpDisconnect();
#endif  // USE_EMULATION

  WiFi.persistent(false);   // Solve possible wifi init errors (re-add at 6.2.1.16 #4044, #4083)
#if defined(USE_IPV6) && defined(ESP32)
  WiFi.IPv6(true);
#endif

#ifdef USE_WIFI_RANGE_EXTENDER
  if (WiFi.getMode() != WIFI_AP_STA || !RgxApUp()) {  // Preserve range extender connections (#17103)
    WiFi.disconnect(true);  // Delete SDK wifi config
    delay(200);
    WifiSetMode(WIFI_STA);  // Disable AP mode
  }
#else
  WiFi.disconnect(true);    // Delete SDK wifi config
  delay(200);
  WifiSetMode(WIFI_STA);    // Disable AP mode
#endif

  WiFiSetSleepMode();
//  if (WiFi.getPhyMode() != WIFI_PHY_MODE_11N) { WiFi.setPhyMode(WIFI_PHY_MODE_11N); }  // B/G/N
//  if (WiFi.getPhyMode() != WIFI_PHY_MODE_11G) { WiFi.setPhyMode(WIFI_PHY_MODE_11G); }  // B/G
#ifdef ESP32
  if (Wifi.phy_mode) {
    WiFi.setPhyMode(WiFiPhyMode_t(Wifi.phy_mode));  // 1-B/2-BG/3-BGN
  }
#endif
  if (!WiFi.getAutoConnect()) { WiFi.setAutoConnect(true); }
//  WiFi.setAutoReconnect(true);
  switch (flag) {
  case 0:  // AP1
  case 1:  // AP2
    Settings->sta_active = flag;
    break;
  case 2:  // Toggle
    Settings->sta_active ^= 1;
  }        // 3: Current AP
  if (!strlen(SettingsText(SET_STASSID1 + Settings->sta_active))) {
    Settings->sta_active ^= 1;  // Skip empty SSID
  }
  if (Settings->ipv4_address[0]) {
    WiFi.config(Settings->ipv4_address[0], Settings->ipv4_address[1], Settings->ipv4_address[2], Settings->ipv4_address[3], Settings->ipv4_address[4]);  // Set static IP
  }
  WiFi.hostname(TasmotaGlobal.hostname);  // ESP8266 needs this here (after WiFi.mode)

  char stemp[40] = { 0 };
  if (channel) {
    WiFi.begin(SettingsText(SET_STASSID1 + Settings->sta_active), SettingsText(SET_STAPWD1 + Settings->sta_active), channel, Wifi.bssid);
    // Add connected BSSID and channel for multi-AP installations
    char hex_char[18];
    snprintf_P(stemp, sizeof(stemp), PSTR(" Channel %d BSSId %s"), channel, ToHex_P((unsigned char*)Wifi.bssid, 6, hex_char, sizeof(hex_char), ':'));
  } else {
    WiFi.begin(SettingsText(SET_STASSID1 + Settings->sta_active), SettingsText(SET_STAPWD1 + Settings->sta_active));
  }
  AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_WIFI D_CONNECTING_TO_AP "%d %s%s " D_IN_MODE " 11%c " D_AS " %s..."),
    Settings->sta_active +1, SettingsText(SET_STASSID1 + Settings->sta_active), stemp, pgm_read_byte(&kWifiPhyMode[WiFi.getPhyMode() & 0x3]), TasmotaGlobal.hostname);

  if (Settings->flag5.wait_for_wifi_result) {  // SetOption142 - (Wifi) Wait 1 second for wifi connection solving some FRITZ!Box modem issues (1)
    WiFi.waitForConnectResult(1000);  // https://github.com/arendst/Tasmota/issues/14985
  }
}

void WifiBeginAfterScan(void)
{
  // Not active
  if (0 == Wifi.scan_state) { return; }
  // Init scan when not connected
  if (1 == Wifi.scan_state) {
    memset((void*) &Wifi.bssid, 0, sizeof(Wifi.bssid));
    Wifi.best_network_db = -127;
    Wifi.scan_state = 3;
  }
  // Init scan when connected
  if (2 == Wifi.scan_state) {
    uint8_t* bssid = WiFi.BSSID();                  // Get current bssid
    memcpy((void*) &Wifi.bssid, (void*) bssid, sizeof(Wifi.bssid));
    Wifi.best_network_db = WiFi.RSSI();             // Get current rssi and add threshold
    if (Wifi.best_network_db < -WIFI_RSSI_THRESHOLD) {
      Wifi.best_network_db += WIFI_RSSI_THRESHOLD;
    }
    Wifi.scan_state = 3;
  }
  // Init scan
  if (3 == Wifi.scan_state) {
    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
      WiFi.scanNetworks(true);                      // Start wifi scan async
      Wifi.scan_state++;
      AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_WIFI "Network (re)scan started..."));
      return;
    }
  }
  int8_t wifi_scan_result = WiFi.scanComplete();
  // Check scan done
  if (4 == Wifi.scan_state) {
    if (wifi_scan_result != WIFI_SCAN_RUNNING) {
      Wifi.scan_state++;
    }
  }
  // Scan done
  if (5 == Wifi.scan_state) {
    int32_t channel = 0;                            // No scan result
    int8_t ap = 3;                                  // AP default if not found
    uint8_t last_bssid[6];                          // Save last bssid
    memcpy((void*) &last_bssid, (void*) &Wifi.bssid, sizeof(last_bssid));

    if (wifi_scan_result > 0) {
      // Networks found
      for (uint32_t i = 0; i < wifi_scan_result; ++i) {

        String ssid_scan;
        int32_t rssi_scan;
        uint8_t sec_scan;
        uint8_t* bssid_scan;
        int32_t chan_scan;
        bool hidden_scan;

        WiFi.getNetworkInfo(i, ssid_scan, sec_scan, rssi_scan, bssid_scan, chan_scan, hidden_scan);

        bool known = false;
        uint32_t j;
        for (j = 0; j < MAX_SSIDS; j++) {
          if (ssid_scan == SettingsText(SET_STASSID1 + j)) {  // SSID match
            known = true;
            if (rssi_scan > Wifi.best_network_db) {      // Best network
              if (sec_scan == ENC_TYPE_NONE || SettingsText(SET_STAPWD1 + j)) {  // Check for passphrase if not open wlan
                Wifi.best_network_db = (int8_t)rssi_scan;
                channel = chan_scan;
                ap = j;                             // AP1 or AP2
                memcpy((void*) &Wifi.bssid, (void*) bssid_scan, sizeof(Wifi.bssid));
              }
            }
            break;
          }
        }
        char hex_char[18];
        AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_WIFI "Network %d, AP%c, SSId %s, Channel %d, BSSId %s, RSSI %d, Encryption %d"),
          i,
          (known) ? (j) ? '2' : '1' : '-',
          ssid_scan.c_str(),
          chan_scan,
          ToHex_P((unsigned char*)bssid_scan, 6, hex_char, sizeof(hex_char), ':'),
          rssi_scan,
          (sec_scan == ENC_TYPE_NONE) ? 0 : 1);
        delay(0);
      }
      WiFi.scanDelete();                            // Clean up Ram
      delay(0);
    }
    Wifi.scan_state = 0;
    // If bssid changed then (re)connect wifi
    for (uint32_t i = 0; i < sizeof(Wifi.bssid); i++) {
      if (last_bssid[i] != Wifi.bssid[i]) {
        WifiBegin(ap, channel);                     // 0 (AP1), 1 (AP2) or 3 (default AP)
        break;
      }
    }
  }

  // Init scan for wifiscan command
  if (6 == Wifi.scan_state) {
    if (wifi_scan_result != WIFI_SCAN_RUNNING) {
      WiFi.scanNetworks(true);                      // Start wifi scan async
      Wifi.scan_state++;
      AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_WIFI "Network scan started..."));
      return;
    }
  }
  // Check scan done
  if (7 == Wifi.scan_state) {
    if (wifi_scan_result != WIFI_SCAN_RUNNING) {
      AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_WIFI "Network scan finished..."));
      Wifi.scan_state++;
      return;
    }
  }
  // Scan done. Show SSId's scan result by MQTT and in console
  if (7 < Wifi.scan_state) {
    Wifi.scan_state++;

    ResponseClear();

    uint32_t initial_item = (Wifi.scan_state - 9)*10;

    if ( wifi_scan_result > initial_item ) {
      // Sort networks by RSSI
      uint32_t indexes[wifi_scan_result];
      for (uint32_t i = 0; i < wifi_scan_result; i++) {
        indexes[i] = i;
      }
      for (uint32_t i = 0; i < wifi_scan_result; i++) {
        for (uint32_t j = i + 1; j < wifi_scan_result; j++) {
          if (WiFi.RSSI(indexes[j]) > WiFi.RSSI(indexes[i])) {
            std::swap(indexes[i], indexes[j]);
          }
        }
      }
      delay(0);

      // Publish the list
      uint32_t end_item = ( wifi_scan_result > initial_item + 10 ) ? initial_item + 10 : wifi_scan_result;
      for (uint32_t i = initial_item; i < end_item; i++) {
        Response_P(PSTR("{\"" D_CMND_WIFISCAN "\":{\"" D_STATUS5_NETWORK "%d\":{\"" D_SSID "\":\"%s\",\"" D_BSSID "\":\"%s\",\"" D_CHANNEL
                        "\":\"%d\",\"" D_JSON_SIGNAL "\":\"%d\",\"" D_RSSI "\":\"%d\",\"" D_JSON_ENCRYPTION "\":\"%s\"}}}"),
                        i+1,
                        WiFi.SSID(indexes[i]).c_str(),
                        WiFi.BSSIDstr(indexes[i]).c_str(),
                        WiFi.channel(indexes[i]),
                        WiFi.RSSI(indexes[i]),
                        WifiGetRssiAsQuality(WiFi.RSSI(indexes[i])),
                        WifiEncryptionType(indexes[i]).c_str());
        MqttPublishPrefixTopicRulesProcess_P(RESULT_OR_STAT, PSTR(D_CMND_WIFISCAN));
      }
    } else if (9 == Wifi.scan_state) {
      Response_P(PSTR("{\"" D_CMND_WIFISCAN "\":\"" D_NO_NETWORKS_FOUND "\"}"));
      MqttPublishPrefixTopicRulesProcess_P(RESULT_OR_STAT, PSTR(D_CMND_WIFISCAN));
    }
    delay(0);
  }
  // Wait 1 minute before cleaning the results so the user can ask for the them using wifiscan command (HTTP use-case)
  if (69 == Wifi.scan_state) {
    //AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_WIFI "Network scan results deleted..."));
    Wifi.scan_state = 0;
    WiFi.scanDelete();                            // Clean up Ram
  }

}

uint16_t WifiLinkCount(void)
{
  return Wifi.link_count;
}

String WifiDowntime(void)
{
  return GetDuration(Wifi.downtime);
}

void WifiSetState(uint8_t state)
{
  if (state == TasmotaGlobal.global_state.wifi_down) {
    if (state) {
      TasmotaGlobal.rules_flag.wifi_connected = 1;
      Wifi.link_count++;
      Wifi.downtime += UpTime() - Wifi.last_event;
    } else {
      TasmotaGlobal.rules_flag.wifi_disconnected = 1;
      Wifi.last_event = UpTime();
    }
  }
  TasmotaGlobal.global_state.wifi_down = state ^1;
  if (!TasmotaGlobal.global_state.wifi_down) {
    TasmotaGlobal.global_state.network_down = 0;
  }
}

#ifdef USE_IPV6
//
// Scan through all interfaces to find a global or local IPv6 address
// Arg:
//    is_local: is the address Link-Local (true) or Global (false)
//    if_type: possible values are "st" for Wifi STA, "en" for Ethernet, "lo" for localhost (not useful)
static String WifiFindIPv6(bool is_local, const char * if_type = "st") {
  for (netif* intf = netif_list; intf != nullptr; intf = intf->next) {
    if (intf->name[0] == if_type[0] && intf->name[1] == if_type[1]) {
      for (uint32_t i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
        ip_addr_t *ipv6 = &intf->ip6_addr[i];
        if (IP_IS_V6_VAL(*ipv6) && !ip_addr_isloopback(ipv6) && !ip_addr_isany(ipv6) && ((bool)ip_addr_islinklocal(ipv6) == is_local)) {
          return IPAddress(ipv6).toString();
        }
      }
    }
  }
  return String();
}

// Returns only IPv6 global address (no loopback and no link-local)
String WifiGetIPv6(void)
{
  return WifiFindIPv6(false, "st");
}

String WifiGetIPv6LinkLocal(void)
{
  return WifiFindIPv6(true, "st");
}

// add an IPv6 link-local address to all netif
void CreateLinkLocalIPv6(void)
{
#ifdef ESP32
  for (auto intf = esp_netif_next(NULL); intf != NULL; intf = esp_netif_next(intf)) {
    esp_netif_create_ip6_linklocal(intf);
  }
#endif // ESP32
}

// 
void WifiDumpAddressesIPv6(void)
{
  for (netif* intf = netif_list; intf != nullptr; intf = intf->next) {
    if (!ip_addr_isany_val(intf->ip_addr)) AddLog(LOG_LEVEL_DEBUG, "WIF: '%c%c' IPv4 %s", intf->name[0], intf->name[1], IPAddress(intf->ip_addr).toString().c_str());
    for (uint32_t i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
      if (!ip_addr_isany_val(intf->ip6_addr[i]))
        AddLog(LOG_LEVEL_DEBUG, "WIF: '%c%c' IPv6 %s %s", intf->name[0], intf->name[1],
                                IPAddress(intf->ip6_addr[i]).toString().c_str(),
                                ip_addr_islinklocal(&intf->ip6_addr[i]) ? "local" : "");
    }
  }
}
#endif  // USE_IPV6

// Check to see if we have any routable IP address
bool WifiHasIP(void) {
#ifdef USE_IPV6
  return !WiFi.localIP().isAny();
#else
  return (uint32_t)WiFi.localIP() != 0;
#endif // USE_IPV6
}

void WifiCheckIp(void) {
#ifdef USE_IPV6
  if (WL_CONNECTED == WiFi.status()) {
    if (!Wifi.ipv6_local_link_called) {
      WiFi.enableIpV6();
      Wifi.ipv6_local_link_called = true;
      // AddLog(LOG_LEVEL_DEBUG, PSTR("WIF: calling enableIpV6"));
    }
    
  }
#endif // USE_IPV6

  if ((WL_CONNECTED == WiFi.status()) && WifiHasIP()) {
    WifiSetState(1);
    Wifi.counter = WIFI_CHECK_SEC;
    Wifi.retry = Wifi.retry_init;
    Wifi.max_retry = 0;
    if (Wifi.status != WL_CONNECTED) {
      AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_WIFI D_CONNECTED));
//      AddLog(LOG_LEVEL_INFO, PSTR("Wifi: Set IP addresses"));
      Settings->ipv4_address[1] = (uint32_t)WiFi.gatewayIP();
      Settings->ipv4_address[2] = (uint32_t)WiFi.subnetMask();
      Settings->ipv4_address[3] = (uint32_t)WiFi.dnsIP();
      Settings->ipv4_address[4] = (uint32_t)WiFi.dnsIP(1);

      // Save current AP parameters for quick reconnect
      Settings->wifi_channel = WiFi.channel();
      uint8_t *bssid = WiFi.BSSID();
      memcpy((void*) &Settings->wifi_bssid, (void*) bssid, sizeof(Settings->wifi_bssid));
    }
    Wifi.status = WL_CONNECTED;
  } else {
    WifiSetState(0);
    uint8_t wifi_config_tool = Settings->sta_config;
    Wifi.status = (Wifi.retry &1) ? WiFi.status() : 0;  // Skip every second to reset result WiFi.status()
    switch (Wifi.status) {
      case WL_CONNECTED:
        AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_WIFI D_CONNECT_FAILED_NO_IP_ADDRESS));
        Wifi.status = 0;
        Wifi.retry = Wifi.retry_init;
        break;
      case WL_NO_SSID_AVAIL:
        AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_WIFI D_CONNECT_FAILED_AP_NOT_REACHED));
        Settings->wifi_channel = 0;  // Disable stored AP
        if (WIFI_WAIT == Settings->sta_config) {
          Wifi.retry = Wifi.retry_init;
        } else {
          if (Wifi.retry > (Wifi.retry_init / 2)) {
            Wifi.retry = Wifi.retry_init / 2;
          }
          else if (Wifi.retry) {
            Wifi.retry = 0;
          }
        }
        break;
      case WL_CONNECT_FAILED:
        AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_WIFI D_CONNECT_FAILED_WRONG_PASSWORD));
        Settings->wifi_channel = 0;  // Disable stored AP
        if (Wifi.retry > (Wifi.retry_init / 2)) {
          Wifi.retry = Wifi.retry_init / 2;
        }
        else if (Wifi.retry) {
          Wifi.retry = 0;
        }
        break;
      default:  // WL_IDLE_STATUS and WL_DISCONNECTED
        if (!Wifi.retry || ((Wifi.retry_init / 2) == Wifi.retry)) {
          AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_WIFI D_CONNECT_FAILED_AP_TIMEOUT));
          Settings->wifi_channel = 0;  // Disable stored AP
          Wifi.max_retry++;
          if (100 == Wifi.max_retry) {  // Restart after 100 * (WIFI_RETRY_OFFSET_SEC + MAC) / 2 seconds
            TasmotaGlobal.restart_flag = 2;
          }
        } else {
          if (!strlen(SettingsText(SET_STASSID1)) && !strlen(SettingsText(SET_STASSID2))) {
            Settings->wifi_channel = 0;  // Disable stored AP
            wifi_config_tool = WIFI_MANAGER;  // Skip empty SSIDs and start Wifi config tool
            Wifi.retry = 0;
          } else {
            AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_WIFI D_ATTEMPTING_CONNECTION));
          }
        }
    }
    if (Wifi.retry) {
      if (Settings->flag3.use_wifi_scan) {  // SetOption56 - Scan wifi network at restart for configured AP's
        if (Wifi.retry_init == Wifi.retry) {
          Wifi.scan_state = 1;    // Select scanned SSID
        }
      } else {
        if (Wifi.retry_init == Wifi.retry) {
          WifiBegin(3, Settings->wifi_channel);  // Select default SSID
        }
        if ((Settings->sta_config != WIFI_WAIT) && ((Wifi.retry_init / 2) == Wifi.retry)) {
          WifiBegin(2, 0);        // Select alternate SSID
        }
      }
      Wifi.retry--;
    } else {
      WifiConfig(wifi_config_tool);
      Wifi.retry = Wifi.retry_init;
    }
    Wifi.counter = 1;             // Re-check in 1 second
  }
}

void WifiCheck(uint8_t param)
{
  Wifi.counter--;
  switch (param) {
  case WIFI_SERIAL:
  case WIFI_MANAGER:
    WifiConfig(param);
    break;
  default:
    if (Wifi.config_counter) {
      Wifi.config_counter--;
      Wifi.counter = Wifi.config_counter +5;
      if (Wifi.config_counter) {
        if (!Wifi.config_counter) {
          if (strlen(WiFi.SSID().c_str())) {
            SettingsUpdateText(SET_STASSID1, WiFi.SSID().c_str());
          }
          if (strlen(WiFi.psk().c_str())) {
            SettingsUpdateText(SET_STAPWD1, WiFi.psk().c_str());
          }
          Settings->sta_active = 0;
          AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_WIFI D_WCFG_2_WIFIMANAGER D_CMND_SSID "1 %s"), SettingsText(SET_STASSID1));
        }
      }
      if (!Wifi.config_counter) {
//        SettingsSdkErase();  //  Disabled v6.1.0b due to possible bad wifi connects
        TasmotaGlobal.restart_flag = 2;
      }
    } else {
      if (Wifi.counter <= 0) {
        AddLog(LOG_LEVEL_DEBUG_MORE, PSTR(D_LOG_WIFI D_CHECKING_CONNECTION));
        Wifi.counter = WIFI_CHECK_SEC;
        WifiCheckIp();
      }
      if ((WL_CONNECTED == WiFi.status()) && WifiHasIP() && !Wifi.config_type) {
        WifiSetState(1);
        if (Settings->flag3.use_wifi_rescan) {  // SetOption57 - Scan wifi network every 44 minutes for configured AP's
          if (!(TasmotaGlobal.uptime % (60 * WIFI_RESCAN_MINUTES))) {
            if (!Wifi.scan_state) { Wifi.scan_state = 2; } // If wifi scan routine is free, use it. Otherwise, wait for next RESCAN TIME
          }
        }
      } else {
        WifiSetState(0);
        Mdns.begun = 0;
      }
    }
    if (Wifi.scan_state) { WifiBeginAfterScan(); }
  }
}

int WifiState(void)
{
  int state = -1;

  if (!TasmotaGlobal.global_state.wifi_down) { state = WIFI_RESTART; }
  if (Wifi.config_type) { state = Wifi.config_type; }
  return state;
}

String WifiGetOutputPower(void)
{
  char stemp1[TOPSZ];
  dtostrfd((float)(Settings->wifi_output_power) / 10, 1, stemp1);
  return String(stemp1);
}

void WifiSetOutputPower(void)
{
  WiFi.setOutputPower((float)(Settings->wifi_output_power) / 10);
}

/*
  See Esp.h, core_esp8266_phy.cpp and test_overrides.ino
  RF_DEFAULT = 0,  // RF_CAL or not after deep-sleep wake up, depends on init data byte 108.
  RF_CAL = 1,      // RF_CAL after deep-sleep wake up, there will be large current.
  RF_NO_CAL = 2,   // no RF_CAL after deep-sleep wake up, there will only be small current.
  RF_DISABLED = 4  // disable RF after deep-sleep wake up, just like modem sleep, there will be the smallest current.
*/
#ifdef WIFI_RF_MODE_RF_CAL
#ifndef USE_DEEPSLEEP
RF_MODE(RF_CAL);
#endif  // USE_DEEPSLEEP
#endif  // WIFI_RF_MODE_RF_CAL

#ifdef WIFI_RF_PRE_INIT
bool rf_pre_init_flag = false;
RF_PRE_INIT()
{
#ifndef USE_DEEPSLEEP
  system_deep_sleep_set_option(1);   // The option is 1 by default.
  system_phy_set_rfoption(RF_CAL);
#endif  // USE_DEEPSLEEP
  system_phy_set_powerup_option(3);  // 3: RF initialization will do the whole RF calibration which will take about 200ms; this increases the current consumption.
  rf_pre_init_flag = true;
}
#endif  // WIFI_RF_PRE_INIT

void WifiEnable(void) {
  Wifi.counter = 1;
}

//#ifdef ESP8266
//#include <sntp.h>                       // sntp_servermode_dhcp()
//#endif  // ESP8266

#ifdef ESP32
void WifiEvents(arduino_event_t *event);
#endif

void WifiConnect(void)
{
  if (!Settings->flag4.network_wifi) { return; }

#if defined(ESP32) && !defined(FIRMWARE_MINIMAL)
  static bool wifi_event_registered = false;
  if (!wifi_event_registered) {
    WiFi.onEvent(WifiEvents);   // register event listener only once
    wifi_event_registered = true;
  }
#endif // ESP32
  WifiSetState(0);
  WifiSetOutputPower();

//#ifdef ESP8266
  // https://github.com/arendst/Tasmota/issues/16061#issuecomment-1216970170
//  sntp_servermode_dhcp(0);
//#endif  // ESP8266

  WiFi.persistent(false);     // Solve possible wifi init errors
  Wifi.status = 0;
  Wifi.retry_init = WIFI_RETRY_OFFSET_SEC + (ESP_getChipId() & 0xF);  // Add extra delay to stop overrun by simultanous re-connects
  Wifi.retry = Wifi.retry_init;
  Wifi.max_retry = 0;
  Wifi.counter = 1;

  memcpy((void*) &Wifi.bssid, (void*) Settings->wifi_bssid, sizeof(Wifi.bssid));

#ifdef WIFI_RF_PRE_INIT
  if (rf_pre_init_flag) {
    AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_WIFI "Pre-init done"));
  }
#endif  // WIFI_RF_PRE_INIT
}

void WifiShutdown(bool option) {
  // option = false - Legacy disconnect also used by DeepSleep
  // option = true  - Disconnect with SDK wifi calibrate sector erase when WIFI_FORCE_RF_CAL_ERASE enabled
  delay(100);                 // Allow time for message xfer - disabled v6.1.0b

#ifdef USE_EMULATION
  UdpDisconnect();
  delay(100);                 // Flush anything in the network buffers.
#endif  // USE_EMULATION

  if (Settings->flag.mqtt_enabled) {  // SetOption3 - Enable MQTT
    MqttDisconnect();
    delay(100);               // Flush anything in the network buffers.
  }

#ifdef WIFI_FORCE_RF_CAL_ERASE
  if (option) {
    WiFi.disconnect(false);   // Disconnect wifi
    SettingsErase(4);         // Delete SDK wifi config and calibrate data
  } else
#endif  // WIFI_FORCE_RF_CAL_ERASE
  {
    // Enable from 6.0.0a until 6.1.0a - disabled due to possible cause of bad wifi connect on core 2.3.0
    // Re-enabled from 6.3.0.7 with ESP.restart replaced by ESP.reset
    // Courtesy of EspEasy
    // WiFi.persistent(true);    // use SDK storage of SSID/WPA parameters
    ETS_UART_INTR_DISABLE();
    wifi_station_disconnect();  // this will store empty ssid/wpa into sdk storage
    ETS_UART_INTR_ENABLE();
    // WiFi.persistent(false);   // Do not use SDK storage of SSID/WPA parameters
  }
  delay(100);                 // Flush anything in the network buffers.
}

void WifiDisable(void) {
  if (!TasmotaGlobal.global_state.wifi_down) {
    WifiShutdown();
    WifiSetMode(WIFI_OFF);
  }
  TasmotaGlobal.global_state.wifi_down = 1;
}

void EspRestart(void)
{
  ResetPwm();
  WifiShutdown(true);
  CrashDumpClear();           // Clear the stack dump in RTC

  if (TasmotaGlobal.restart_halt) {
    while (1) {
      OsWatchLoop();          // Feed OsWatch timer to prevent restart
      SetLedLink(1);          // Wifi led on
      delay(200);             // Satisfy SDK
      SetLedLink(0);          // Wifi led off
      delay(800);             // Satisfy SDK
    }
  } else {
    ESP_Restart();
  }
}

#ifdef ESP8266
//
// Gratuitous ARP, backported from https://github.com/esp8266/Arduino/pull/6889
//
extern "C" {
#if LWIP_VERSION_MAJOR == 1
#include "netif/wlan_lwip_if.h" // eagle_lwip_getif()
#include "netif/etharp.h" // gratuitous arp
#else
#include "lwip/etharp.h" // gratuitous arp
#endif
}

void stationKeepAliveNow(void) {
  AddLog(LOG_LEVEL_DEBUG_MORE, PSTR(D_LOG_WIFI "Sending Gratuitous ARP"));
  for (netif* interface = netif_list; interface != nullptr; interface = interface->next)
    if (
          (interface->flags & NETIF_FLAG_LINK_UP)
      && (interface->flags & NETIF_FLAG_UP)
#if LWIP_VERSION_MAJOR == 1
      && interface == eagle_lwip_getif(STATION_IF) /* lwip1 does not set if->num properly */
      && (!ip_addr_isany(&interface->ip_addr))
#else
      && interface->num == STATION_IF
      && (!ip4_addr_isany_val(*netif_ip4_addr(interface)))
#endif
  )
  {
    etharp_gratuitous(interface);
    break;
  }
}

void wifiKeepAlive(void) {
  static uint32_t wifi_timer = millis();                     // Wifi keepalive timer

  uint32_t wifiTimerSec = Settings->param[P_ARP_GRATUITOUS];  // 8-bits number of seconds, or minutes if > 100

  if ((WL_CONNECTED != Wifi.status) || (0 == wifiTimerSec)) { return; }   // quick exit if wifi not connected or feature disabled

  if (TimeReached(wifi_timer)) {
    stationKeepAliveNow();
    if (wifiTimerSec > 100) {
      wifiTimerSec = (wifiTimerSec - 100) * 60;              // convert >100 as minutes, ex: 105 = 5 minutes, 110 = 10 minutes
    }
    SetNextTimeInterval(wifi_timer, wifiTimerSec * 1000);
  }
}
#endif  // ESP8266

bool WifiHostByName(const char* aHostname, IPAddress& aResult) {
#ifdef ESP8266
  if (WiFi.hostByName(aHostname, aResult, Settings->dns_timeout)) {
    // Host name resolved
    if (0xFFFFFFFF != (uint32_t)aResult) {
      return true;
    }
  }
#else
  // DnsClient can't do one-shot mDNS queries so use WiFi.hostByName() for *.local
  aResult = (uint32_t) 0x00000000L;    // indirectly force to be IPv4, since the client touches the binary format later
  size_t hostname_len = strlen(aHostname);
  if (strstr_P(aHostname, PSTR(".local")) == &aHostname[hostname_len] - 6) {
    if (WiFi.hostByName(aHostname, aResult)) {
      // Host name resolved
      if (0xFFFFFFFF != (uint32_t)aResult) {
        AddLog(LOG_LEVEL_DEBUG, "WIF: Resolving '%s' (%s)", aHostname, aResult.toString().c_str());
        return true;
      }
    }
  } else {
    // Use this instead of WiFi.hostByName or connect(host_name,.. to block less if DNS server is not found
    uint32_t dns_address = (!TasmotaGlobal.global_state.eth_down) ? Settings->eth_ipv4_address[3] : Settings->ipv4_address[3];
    DnsClient.begin((IPAddress)dns_address);
    if (1 == DnsClient.getHostByName(aHostname, aResult)) {
      AddLog(LOG_LEVEL_DEBUG, "WIF: Resolving '%s' (%s)", aHostname, aResult.toString().c_str());
      return true;
    }
  }
#endif
  AddLog(LOG_LEVEL_DEBUG, PSTR("DNS: Unable to resolve '%s'"), aHostname);
  return false;
}

bool WifiDnsPresent(const char* aHostname) {
  IPAddress aResult;
  return WifiHostByName(aHostname, aResult);
}

void WifiPollNtp() {
  static uint8_t ntp_sync_minute = 0;
  static uint32_t ntp_run_time = 0;

  if (TasmotaGlobal.global_state.network_down || Rtc.user_time_entry) { return; }

  uint8_t uptime_minute = (TasmotaGlobal.uptime / 60) % 60;  // 0 .. 59
  if ((ntp_sync_minute > 59) && (uptime_minute > 2)) {
    ntp_sync_minute = 1;                 // If sync prepare for a new cycle
  }
  // First try ASAP to sync. If fails try once every 60 seconds based on chip id
  uint8_t offset = (TasmotaGlobal.uptime < 30) ? RtcTime.second + ntp_run_time : (((ESP_getChipId() & 0xF) * 3) + 3) ;

  if ( (((offset == RtcTime.second) && ( (RtcTime.year < 2016) ||                  // Never synced
                                         (ntp_sync_minute == uptime_minute))) ||   // Re-sync every hour
       TasmotaGlobal.ntp_force_sync ) ) {                                          // Forced sync

    TasmotaGlobal.ntp_force_sync = false;

    AddLog(LOG_LEVEL_DEBUG_MORE, PSTR("NTP: Sync time..."));
    ntp_run_time = millis();
    uint64_t ntp_nanos = WifiGetNtp();
    uint32_t ntp_time = ntp_nanos / 1000000000;
    ntp_run_time = (millis() - ntp_run_time) / 1000;
//    AddLog(LOG_LEVEL_DEBUG, PSTR("NTP: Runtime %d"), ntp_run_time);
    if (ntp_run_time < 5) { ntp_run_time = 0; }  // DNS timeout is around 10s

    if (ntp_time > START_VALID_TIME) {
      Rtc.utc_time = ntp_time;
      Rtc.nanos = ntp_nanos % 1000000000;
      ntp_sync_minute = 60;             // Sync so block further requests
      RtcSync("NTP");
    } else {
      ntp_sync_minute++;                // Try again in next minute
    }
  }
}

uint64_t WifiGetNtp(void) {
  static uint8_t ntp_server_id = 0;

//  AddLog(LOG_LEVEL_DEBUG, PSTR("NTP: Start NTP Sync %d ..."), ntp_server_id);

  IPAddress time_server_ip;

  char fallback_ntp_server[16];
  snprintf_P(fallback_ntp_server, sizeof(fallback_ntp_server), PSTR("%d.pool.ntp.org"), random(0,3));

  char* ntp_server;
  for (uint32_t i = 0; i <= MAX_NTP_SERVERS; i++) {
    if (ntp_server_id > MAX_NTP_SERVERS) { ntp_server_id = 0; }
    ntp_server = (ntp_server_id < MAX_NTP_SERVERS) ? SettingsText(SET_NTPSERVER1 + ntp_server_id) : fallback_ntp_server;
    if (strlen(ntp_server)) {
      break;
    }
    ntp_server_id++;
  }
  if (!WifiHostByName(ntp_server, time_server_ip)) {
    ntp_server_id++;
//    AddLog(LOG_LEVEL_DEBUG, PSTR("NTP: Unable to resolve '%s'"), ntp_server);
    return 0;
  }

//  AddLog(LOG_LEVEL_DEBUG, PSTR("NTP: NtpServer '%s' IP %_I"), ntp_server, (uint32_t)time_server_ip);

  WiFiUDP udp;

  uint32_t attempts = 3;
  while (attempts > 0) {
    uint32_t port = random(1025, 65535);   // Create a random port for the UDP connection.
    if (udp.begin(port) != 0) {
      break;
    }
    attempts--;
  }
  if (0 == attempts) { return 0; }

  while (udp.parsePacket() > 0) {          // Discard any previously received packets
    yield();
  }

  const uint32_t NTP_PACKET_SIZE = 48;     // NTP time is in the first 48 bytes of message
  uint8_t packet_buffer[NTP_PACKET_SIZE];  // Buffer to hold incoming & outgoing packets
  memset(packet_buffer, 0, NTP_PACKET_SIZE);
  packet_buffer[0]  = 0b11100011;          // LI, Version, Mode
  packet_buffer[1]  = 0;                   // Stratum, or type of clock
  packet_buffer[2]  = 6;                   // Polling Interval
  packet_buffer[3]  = 0xEC;                // Peer Clock Precision
  packet_buffer[12] = 49;
  packet_buffer[13] = 0x4E;
  packet_buffer[14] = 49;
  packet_buffer[15] = 52;

  if (udp.beginPacket(time_server_ip, 123) == 0) {  // NTP requests are to port 123
    ntp_server_id++;                                // Next server next time
    udp.stop();
    return 0;
  }
  udp.write(packet_buffer, NTP_PACKET_SIZE);
  udp.endPacket();

  uint32_t begin_wait = millis();
  while (!TimeReached(begin_wait + 1000)) {         // Wait up to one second
    uint32_t size        = udp.parsePacket();
    uint32_t remote_port = udp.remotePort();

    if ((size >= NTP_PACKET_SIZE) && (remote_port == 123)) {
      udp.read(packet_buffer, NTP_PACKET_SIZE);     // Read packet into the buffer
      udp.stop();

      if ((packet_buffer[0] & 0b11000000) == 0b11000000) {
        // Leap-Indicator: unknown (clock unsynchronized)
        // See: https://github.com/letscontrolit/ESPEasy/issues/2886#issuecomment-586656384
        AddLog(LOG_LEVEL_DEBUG, PSTR("NTP: IP %_I unsynced"), (uint32_t)time_server_ip);
        ntp_server_id++;                            // Next server next time
        return 0;
      }

      // convert four bytes starting at location 40 to a long integer
      // TX time is used here.
      uint32_t secs_since_1900 = (uint32_t)packet_buffer[40] << 24;
      secs_since_1900 |= (uint32_t)packet_buffer[41] << 16;
      secs_since_1900 |= (uint32_t)packet_buffer[42] << 8;
      secs_since_1900 |= (uint32_t)packet_buffer[43];
      if (0 == secs_since_1900) {                   // No time stamp received
        ntp_server_id++;                            // Next server next time
        return 0;
      }
      uint32_t tmp_fraction = (uint32_t)packet_buffer[44] << 24;
      tmp_fraction |= (uint32_t)packet_buffer[45] << 16;
      tmp_fraction |= (uint32_t)packet_buffer[46] << 8;
      tmp_fraction |= (uint32_t)packet_buffer[47];
      uint32_t fraction = (((uint64_t)tmp_fraction) * 1000000000) >> 32;
      return (((uint64_t)secs_since_1900) - 2208988800UL) * 1000000000 + fraction;
    }
    delay(10);
  }
  // Timeout.
  AddLog(LOG_LEVEL_DEBUG, PSTR("NTP: No reply from %_I"), (uint32_t)time_server_ip);
  udp.stop();
  ntp_server_id++;                                  // Next server next time
  return 0;
}

// --------------------------------------------------------------------------------
// Respond to some Arduino/esp-idf events for better IPv6 support
// --------------------------------------------------------------------------------
#ifdef ESP32
// typedef void (*WiFiEventSysCb)(arduino_event_t *event);
void WifiEvents(arduino_event_t *event) {
  switch (event->event_id) {

#ifdef USE_IPV6
    case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
    case ARDUINO_EVENT_ETH_GOT_IP6:
    {
      ip_addr_t ip_addr6;
      ip_addr_copy_from_ip6(ip_addr6, event->event_info.got_ip6.ip6_info.ip);
      IPAddress addr(ip_addr6);
      AddLog(LOG_LEVEL_DEBUG, PSTR("%s: IPv6 %s %s"),
             event->event_id == ARDUINO_EVENT_ETH_GOT_IP6 ? "ETH" : "WIF",
             addr.isLocal() ? PSTR("Local") : PSTR("Global"), addr.toString().c_str());
    }
    break;
#endif // USE_IPV6
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    case ARDUINO_EVENT_ETH_GOT_IP:
    {
      ip_addr_t ip_addr4;
      ip_addr_copy_from_ip4(ip_addr4, event->event_info.got_ip.ip_info.ip);
      AddLog(LOG_LEVEL_DEBUG, PSTR("%s: IPv4 %_I, mask %_I, gateway %_I"),
             event->event_id == ARDUINO_EVENT_ETH_GOT_IP ? "ETH" : "WIF",
              event->event_info.got_ip.ip_info.ip.addr,
              event->event_info.got_ip.ip_info.netmask.addr,
              event->event_info.got_ip.ip_info.gw.addr);

    }
    break;

    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      // AddLog(LOG_LEVEL_DEBUG, PSTR("WIF: Received ARDUINO_EVENT_WIFI_STA_CONNECTED"));
      Wifi.ipv6_local_link_called = false;    // not sure if this is needed, make sure link-local is restored at each reconnect
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
      Wifi.ipv6_local_link_called = false;
      break;

    default:
      break;
  }
}
#endif // ESP32
