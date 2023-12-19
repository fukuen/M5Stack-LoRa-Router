#include <M5Stack.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

#include <rom/rtc.h>
#include <driver/adc.h>

#include "lw_packets.h"

#define WIFI_SSID1 "<ssid>"
#define WIFI_PASS1 "<password>"

char buffer[512];

WebSocketsClient webSocket;

uint8_t payload[1024];
int payload_len = 0;

static char recv_buf[512];
static bool is_exist = false;

String str_buffer = "";
int recv_index = 0;

Lorawan_fcnt_t fcnt;
Lorawan_devCfg_t devCfg;

lwPackets_api_t api;
lwPackets_state_t state;

const char *devAddr = "00000000";
const char *nwkSKey = "11111111111111111111111111111111";
const char *appSKey = "11111111111111111111111111111111";
uint32_t DevAddr = 0;
uint8_t NwkSKey[16] = {0};
uint8_t AppSKey[16] = {0};

#define ROUTER "2c-f7-f1-13-12-34-56-78"

#define ENDPOINT "wss://xxxxxxxxxxxxxx.lns.lorawan.ap-northeast-1.amazonaws.com:443"
#define DEFAULT_ROUTER "/router-info"

const char *rootCA =
    "-----BEGIN CERTIFICATE-----\n"
    "...\n"
    "-----END CERTIFICATE-----\n";

const char *certificate =
    "-----BEGIN CERTIFICATE-----\n"
    "...\n"
    "-----END CERTIFICATE-----\n";

const char *privateKey =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "...\n"
    "-----END RSA PRIVATE KEY-----\n";

int wsconn = 0;

String url, host, path;
int port;

time_t nowSec;
time_t rxSec;
struct tm timeinfo;
char ar[80];

String step = "";

String router;
String muxs;
String gateway_url;
String error;

String muxs_1;
String muxs_2;

int snr = 0;
int rssi = 0;

void connectAWSIoT();
void connectWs();

static void printLog(const char *format, ...)
{
  char buffer[100];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(buffer, sizeof(buffer), format, args);
  if (len > 100 - 1)
  {
    strcpy(&buffer[100 - 5], "...\n");
  }
  va_end(args);
  return;
}

void printCurrentTime()
{
  nowSec = time(nullptr);

  gmtime_r(&nowSec, &timeinfo);
  Serial.print("Current time: ");
  Serial.print(asctime(&timeinfo));
}

void setClock()
{
  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org", "time.nist.gov");

  Serial.print("Waiting for NTP time sync: ");
  nowSec = time(nullptr);
  while (nowSec < 8 * 3600 * 2)
  {
    delay(500);
    Serial.print(".");
    yield();
    nowSec = time(nullptr);
  }
  Serial.println();

  printCurrentTime();
}

const char *hex2bin(const char *hex, char *bin, size_t binsize)
{
  if (hex && bin)
  {
    for (; binsize && isxdigit(hex[0]) && isxdigit(hex[1]); hex += 2, bin += 1, binsize -= 1)
    {
      int r = sscanf(hex, "%2hhx", bin);
      if (r != 1)
      {
        break;
      }
    }
  }
  return hex;
}

void hexdump(const void *mem, uint32_t len, uint8_t cols = 16)
{
  const uint8_t *src = (const uint8_t *)mem;
  Serial.printf("\n[HEXDUMP] Address: 0x%08X len: 0x%X (%d)", (ptrdiff_t)src, len, len);
  for (uint32_t i = 0; i < len; i++)
  {
    if (i % cols == 0)
    {
      Serial.printf("\n[0x%08X] 0x%08X: ", (ptrdiff_t)src, i);
    }
    Serial.printf("%02X ", *src);
    src++;
  }
  Serial.printf("\n");
}

bool sendMessage(lorawan_packet_t *packet, String encrypted)
{
  bool ret = false;

  nowSec = time(nullptr);

  DynamicJsonDocument json(512);

  json["msgtype"] = "updf";
  json["MHdr"] = packet->MHDR.type << 5 | packet->MHDR.version;
  json["DevAddr"] = packet->BODY.MACPayload.FHDR.DevAddr;
  json["FCtrl"] = packet->BODY.MACPayload.FHDR.FCtrl.uplink.ADR << 7 | packet->BODY.MACPayload.FHDR.FCtrl.uplink.ADRACKReq << 6 |
                  packet->BODY.MACPayload.FHDR.FCtrl.uplink.ACK << 5 | packet->BODY.MACPayload.FHDR.FCtrl.uplink.ClassB << 4 |
                  packet->BODY.MACPayload.FHDR.FCtrl.uplink.FOptsLen;
  json["FCnt"] = packet->BODY.MACPayload.FHDR.FCnt16;
  json["FOpts"] = "";
  json["FPort"] = packet->BODY.MACPayload.FPort;
  String pl = "";
  json["FRMPayload"] = encrypted;
  json["MIC"] = packet->MIC;
  json["RefTime"] = nowSec;
  json["DR"] = 3;
  json["Freq"] = 923400000;
  json["upinfo"]["rctx"] = 0;
  json["upinfo"]["xtime"] = nowSec * 38 * 1000000;
  json["upinfo"]["gpstime"] = 0;
  json["upinfo"]["fts"] = -1;
  json["upinfo"]["rssi"] = -50;
  json["upinfo"]["snr"] = 11 / 4;
  json["upinfo"]["rxtime"] = rxSec;
  serializeJson(json, buffer, sizeof(buffer));
  Serial.println(buffer);
  webSocket.sendTXT(buffer);

  return ret;
}

void processMessage(uint8_t *payload, size_t length)
{
  String msg;

  DynamicJsonDocument json_response(2048);
  deserializeJson(json_response, (char *)payload);

  serializeJson(json_response, Serial);
  Serial.println("");

  if (json_response.containsKey("router"))
  {
    router = json_response["router"].as<String>();
    muxs = json_response["muxs"].as<String>();
    gateway_url = json_response["uri"].as<String>();
    error = json_response["error"].as<String>();
  }
  else if (json_response.containsKey("msgtype"))
  {
    if (muxs_1 == "")
    {
      muxs_1 = String((char *)payload);
    }
    else if (muxs_2 == "")
    {
      muxs_2 = String((char *)payload);
    }
  }
  else
  {
    Serial.println("UNKNOWN PAYLOAD!");
  }
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
  switch (type)
  {
  case WStype_DISCONNECTED:
    Serial.printf("[WSc] Disconnected!\n");
    wsconn = 0;
    break;
  case WStype_CONNECTED:
  {
    Serial.printf("[WSc] Connected to url: %s\n", payload);
    wsconn = 1;
    if (step == "wsRouterInfo")
    {
      webSocket.sendTXT("{\"router\":\"" ROUTER "\"}");
    }
    else if (step == "wsGateway")
    {
      webSocket.sendTXT("{\"msgtype\":\"version\",\"station\":\"2.0.6(rpi/debug)\",\"firmware\":null,\"package\":null,\"model\":\"rpi\",\"protocol\":2,\"features\":\"rmtsh\"}");
    }
  }
  break;
  case WStype_TEXT:
    Serial.printf("[WSc] get text: %s\n", payload);
    processMessage(payload, length);
    break;
  case WStype_BIN:
    Serial.printf("[WSc] get binary length: %u\n", length);
    hexdump(payload, length);
    break;
  case WStype_ERROR:
    Serial.printf("[WSc] Error\n");
    break;
  case WStype_FRAGMENT_TEXT_START:
    Serial.printf("[WSc] Frag text start\n");
    break;
  case WStype_FRAGMENT_BIN_START:
    Serial.printf("[WSc] Frag bin start\n");
    break;
  case WStype_FRAGMENT:
    Serial.printf("[WSc] Frag\n");
    break;
  case WStype_FRAGMENT_FIN:
    Serial.printf("[WSc] Frag fin\n");
    break;
  case WStype_PING:
    Serial.printf("[WSc] Ping\n");
    break;
  case WStype_PONG:
    Serial.printf("[WSc] Pong\n");
    break;
  default:
    break;
  }
}

void wsRouterInfo()
{
  step = "wsRouterInfo";
  url = ENDPOINT;
  String url_tmp;
  if (url.startsWith("wss"))
  {
    url_tmp = url.substring(6, url.length());
  }
  else
  {
    url_tmp = url.substring(5, url.length());
  }

  int16_t first_slash_pos = url_tmp.indexOf("/");
  if (first_slash_pos == -1)
  {
    host = url_tmp;
    path = DEFAULT_ROUTER;
  }
  else
  {
    host = url_tmp.substring(0, first_slash_pos);
    path = url_tmp.substring(first_slash_pos, url_tmp.length());
  }

  int16_t colon_pos = host.indexOf(":");
  if (colon_pos == -1)
  {
    port = 443;
  }
  else
  {
    port = host.substring(colon_pos + 1, host.length()).toInt();
    host = host.substring(0, colon_pos);
  }

  Serial.printf("connecting to router %s%s\n", host.c_str(), path.c_str());
  if (url.startsWith("wss"))
  {
    webSocket.setSSLClientCertKey(certificate, privateKey);
    webSocket.beginSslWithCA(host.c_str(), port, path.c_str(), rootCA);
  }
  else
  {
    if (String("0123456789").indexOf(host.charAt(0)) > -1)
    {
      IPAddress ip;
      ip.fromString(host);
      webSocket.begin(ip, port, path.c_str());
    }
    else
    {
      webSocket.begin(host.c_str(), port, path.c_str());
    }
  }
  int i = 0;
  while (!wsconn)
  {
    webSocket.loop();
    delay(100);
    i++;
    if (i > 50)
    {
      break;
    }
  }
  while (wsconn)
  {
    webSocket.loop();
    delay(100);
    i++;
    if (i > 50)
    {
      break;
    }
  }
  webSocket.disconnect();
}

void wsGateway()
{
  step = "wsGateway";
  url = gateway_url;
  Serial.println(url);
  if (url == "")
  {
    delay(1000);
    return;
  }

  if (gateway_url.startsWith("wss"))
  {
    url = url.substring(6, url.length());
  }
  else
  {
    url = url.substring(5, url.length());
  }

  int16_t first_slash_pos = url.indexOf("/");
  if (first_slash_pos == -1)
  {
    path = "/";
    host = url;
  }
  else
  {
    host = url.substring(0, first_slash_pos);
    path = url.substring(first_slash_pos, url.length());
  }
  Serial.println(path);

  int16_t colon_pos = host.indexOf(":");
  if (colon_pos == -1)
  {
    port = 443;
  }
  else
  {
    port = host.substring(colon_pos + 1, host.length()).toInt();
    host = host.substring(0, colon_pos);
  }
  Serial.println(host);

  Serial.printf("connecting to muxs %s%s\n", host.c_str(), path.c_str());
  if (gateway_url.startsWith("wss"))
  {
    webSocket.setSSLClientCertKey(certificate, privateKey);
    webSocket.beginSslWithCA(host.c_str(), port, path.c_str(), rootCA, "");
  }
  else
  {
    if (String("0123456789").indexOf(host.charAt(0)) > -1)
    {
      IPAddress ip;
      ip.fromString(host);
      webSocket.begin(ip, port, path.c_str());
    }
    else
    {
      webSocket.begin(host.c_str(), port, path.c_str());
    }
  }
  int i = 0;
  while (!wsconn)
  {
    webSocket.loop();
    delay(100);
    i++;
    if (i > 50)
    {
      break;
    }
  }
}

static int at_send_check_response(char *p_ack, int timeout_ms, char *p_cmd, ...)
{
  int ch = 0;
  int index = 0;
  int startMillis = 0;
  va_list args;
  memset(recv_buf, 0, sizeof(recv_buf));
  va_start(args, p_cmd);
  Serial1.printf(p_cmd, args);
  Serial.printf(p_cmd, args);
  va_end(args);
  delay(200);
  startMillis = millis();

  if (p_ack == NULL)
  {
    return 0;
  }

  do
  {
    while (Serial1.available() > 0)
    {
      ch = Serial1.read();
      recv_buf[index++] = ch;
      Serial.print((char)ch);
      delay(2);
    }

    if (strstr(recv_buf, p_ack) != NULL)
    {
      return 1;
    }

  } while (millis() - startMillis < timeout_ms);
  return 0;
}

void display_payload_0x06()
{
  Serial.println();
  Serial.printf("id: %02x\n", payload[0]);
  Serial.printf("eventStatus: %06x\n", payload[1] << 16 | payload[2] << 8 | payload[3]);
  Serial.printf("motionSegmentNumber: %d\n", payload[4]);
  Serial.printf("utcTime: %d\n", long(payload[5] << 24 | payload[6] << 16 | payload[7] << 8 | payload[8]));
  Serial.printf("longitude: %.6lf\n", (double)long(payload[9] << 24 | payload[10] << 16 | payload[11] << 8 | payload[12]) / 1000000);
  Serial.printf("latitude: %.6lf\n", (double)long(payload[13] << 24 | payload[14] << 16 | payload[15] << 8 | payload[16]) / 1000000);
  Serial.printf("temperature: %.1f\n", (float)(payload[17] << 8 | payload[18]) / 10);
  Serial.printf("light: %d\n", payload[19] << 8 | payload[20]);
  Serial.printf("batteryLevel: %d\n", payload[21]);

  M5.Lcd.fillRect(0, 48, 320, 240 - 48, TFT_BLACK);
  M5.Lcd.setCursor(0, 48);
  M5.Lcd.printf("utcTime: %d\n", long(payload[5] << 24 | payload[6] << 16 | payload[7] << 8 | payload[8]));
  M5.Lcd.printf("longitude: %.6lf\n", (double)long(payload[9] << 24 | payload[10] << 16 | payload[11] << 8 | payload[12]) / 1000000);
  M5.Lcd.printf("latitude: %.6lf\n", (double)long(payload[13] << 24 | payload[14] << 16 | payload[15] << 8 | payload[16]) / 1000000);
  M5.Lcd.printf("temperature: %.1f\n", (float)(payload[17] << 8 | payload[18]) / 10);
  M5.Lcd.printf("light: %d\n", payload[19] << 8 | payload[20]);
  M5.Lcd.printf("batteryLevel: %d\n", payload[21]);
}

void display_payload_0x11()
{
  Serial.println();
  Serial.printf("id: %02x\n", payload[0]);
  Serial.printf("unknown: %d\n", payload[1]);
  Serial.printf("eventStatus: %06x\n", payload[2] << 16 | payload[3] << 8 | payload[4]);
  Serial.printf("utcTime: %d\n", long(payload[5] << 24 | payload[6] << 16 | payload[7] << 8 | payload[8]));
  Serial.printf("temperature: %.1f\n", (float)(payload[9] << 8 | payload[10]) / 10);
  Serial.printf("light: %d\n", payload[11] << 8 | payload[12]);
  Serial.printf("batteryLevel: %d\n", payload[13]);

  M5.Lcd.fillRect(0, 48, 320, 240 - 48, TFT_BLACK);
  M5.Lcd.setCursor(0, 48);
  M5.Lcd.printf("utcTime: %d\n", long(payload[5] << 24 | payload[6] << 16 | payload[7] << 8 | payload[8]));
  M5.Lcd.printf("longitude: \n");
  M5.Lcd.printf("latitude: \n");
  M5.Lcd.printf("temperature: %.1f\n", (float)(payload[9] << 8 | payload[10]) / 10);
  M5.Lcd.printf("light: %d\n", payload[11] << 8 | payload[12]);
  M5.Lcd.printf("batteryLevel: %d\n", payload[13]);
}

void init_lw_packets()
{
  api.LogError = printLog;
  api.LogInfo = printLog;

  fcnt.AFCntDwn = 0;
  fcnt.FCntUp = 0;
  fcnt.NFCntDwn = 0;

  devCfg.LorawanVersion = LORAWAN_VERSION_1_0;
  devCfg.DevAddr = DevAddr;
  char *p = (char *)hex2bin((const char *)appSKey, (char *)AppSKey, sizeof(AppSKey));
  for (int i = 0; i < 16; i++)
  {
    devCfg.AppSKey[i] = AppSKey[i];
    devCfg.FNwkSIntKey[i] = AppSKey[i];
    devCfg.NwkSEncKey[i] = AppSKey[i];
    devCfg.SNwkSIntKey[i] = AppSKey[i];
  }
  state.pDevCfg = &devCfg;
  state.pFCntCtrl = &fcnt;

  LoRaWAN_PacketsUtil_Init(api, state);
}

int process_frame()
{
  String str_tmp;
  int str_len = 0;
  int payload_len = 0;
  char *p_start = NULL;

  String str = String(recv_buf);

  if (str.startsWith("+TEST: RX \""))
  {
    str_tmp = str.substring(11, str.length());
    str_tmp = str_tmp.substring(0, str_tmp.indexOf("\""));
    str_len = str_tmp.length();
    hex2bin(str_tmp.c_str(), buffer, sizeof(buffer));

    lorawan_packet_t *packet = LoRaWAN_UnmarshalPacketFor((const uint8_t *)buffer, str_len / 2, 0);
    if (packet == NULL)
    {
      payload[0] = 0;
      LoRaWAN_DeletePacket(packet);
    }
    else
    {
      lorawan_logLoraPacket(packet, true);
      payload_len = packet->BODY.MACPayload.payloadLength;
      memcpy(payload, packet->pPayload, packet->BODY.MACPayload.payloadLength);

      String encrypted = str_tmp.substring(str_len - payload_len * 2 - 8, str_len - 8);
      Serial.printf("encripted %s", encrypted.c_str());
      sendMessage(packet, encrypted);

      LoRaWAN_DeletePacket(packet);

      if (payload[0] == 0x06)
      {
        display_payload_0x06();
      }
      else if (payload[0] == 0x11)
      {
        display_payload_0x11();
      }
    }
  }

  p_start = strstr(recv_buf, "RSSI:");
  if (p_start && (1 == sscanf(p_start, "RSSI:%d,", &rssi)))
  {
    M5.Lcd.setCursor(0, 16);
    M5.Lcd.print("                ");
    M5.Lcd.setCursor(16, 16);
    M5.Lcd.print("rssi:");
    M5.Lcd.print(rssi);
  }
  p_start = strstr(recv_buf, "SNR:");
  if (p_start && (1 == sscanf(p_start, "SNR:%d", &snr)))
  {
    M5.Lcd.setCursor(0, 32);
    M5.Lcd.print("                ");
    M5.Lcd.setCursor(16, 32);
    M5.Lcd.print("snr :");
    M5.Lcd.print(snr);
  }

  return 1;
}

void setup()
{
  M5.begin();

  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, 22, 21);

  SPIFFS.begin();
  M5.Lcd.clear(BLACK);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.printf("Connecting to WiFi");

  // for BtnA bug
  adc_power_acquire(); // ADC Power ON

  esp_chip_info_t out_info;
  esp_chip_info(&out_info);
  Serial.printf("chip revision %d\n", out_info.revision);

  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID1, WIFI_PASS1);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    M5.Lcd.print(".");
  }

  setClock();

  if (at_send_check_response("+AT: OK", 100, "AT\r\n"))
  {
    is_exist = true;
    at_send_check_response("+MODE: TEST", 1000, "AT+MODE=TEST\r\n");
    at_send_check_response("+TEST: RFCFG", 1000, "AT+TEST=RFCFG,923.400,SF9,125,8,8,14,ON,OFF,ON\r\n");
    delay(200);

    at_send_check_response("+TEST: RXLRPKT", 1000, "AT+TEST=RXLRPKT\r\n");
  }
  else
  {
    is_exist = false;
    Serial.print("No E5 module found.\r\n");
    M5.Lcd.setCursor(0, 1);
    M5.Lcd.print("unfound E5 !");
  }

  init_lw_packets();

  webSocket.onEvent(webSocketEvent);
  webSocket.enableHeartbeat(60000, 60000, 5);
  webSocket.setExtraHeaders("");
  wsRouterInfo();
  wsGateway();
  webSocket.sendPing();
}

void loop()
{
  webSocket.loop();

  nowSec = time(nullptr);

  while (Serial1.available())
  {
    char ch = Serial1.read();
    recv_buf[recv_index++] = ch;
    if (ch == '\n')
    {
      rxSec = time(nullptr);
      recv_buf[recv_index++] = 0;
      Serial.print("***" + String(recv_buf));
      process_frame();
      recv_index = 0;
    }
  }

  M5.update();
}
