//================================================================
// ライブラリ
//================================================================
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFiServer.h>
#include <LiquidCrystal.h>
#include <time.h>
#include <stdlib.h>
#include <ArduinoJson.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <WiFiNTP.h>
#include <ArduinoOTA.h> // ★★★ OTA（無線書き込み）用のライブラリを追加 ★★★
#include "config.h"

//================================================================
// デバッグ用マクロ
//================================================================
#if DEBUG
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(x, ...) Serial.printf((x), ##__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(x, ...)
#endif

//================================================================
// ピン定義
//================================================================
namespace Pins {
    const int LCD_RS = 2, LCD_E = 3, LCD_D4 = 4, LCD_D5 = 5, LCD_D6 = 6, LCD_D7 = 7;
    const int LED_PIN = 16;
    const int BUTTON_PIN = 14;
    const int TILT_PIN = 17;    // 傾斜スイッチ AT-407 (GP17)
    const int BUZZER_PIN = 15;  // ブザー (GP15)
    const int TRIG_PIN = 26;
    const int ECHO_PIN = 27;
}

//================================================================
// グローバル状態管理
//================================================================
namespace State {
    // 動作モードの定義
    enum Mode {
        MAIN_DISPLAY, MENU, HISTORY, SWITCHBOT_DEVICE_SELECT,
        DEVICE_CONTROL, WOL_SELECT, DISTANCE_MEASURE, GET_DEVICE_LIST,
        BATH_MODE, BATH_MODE_SUBMENU, BATH_MODE_MANUAL
    };

    // メニューの状態
    struct MenuState {
        Mode currentMode = MAIN_DISPLAY;
        int menuSelection = 0;
        int deviceSelection = 0;
        int commandSelection = 0;
    };

    // マルチコア対応: コア間で共有する変数は volatile に変更
    // システム全体の状態
    struct SystemState {
        volatile bool wifiConnected = false; // Wi-Fi接続状態 (コア1が書き込み、コア0が読み取り)
        volatile bool ntpInitialized = false;  // NTP同期状態 (コア1が書き込み、コア0が読み取り)
        volatile bool otaInitialized = false;  // ★★★ OTA初期化フラグを追加 ★★★
        bool needsRedraw = true;
        bool forceMainScreenRedraw = true;
        bool ledAlwaysOn = false;
        bool isBlinking = false;
        bool displayFlipped = false;  // 傾斜スイッチによる表示反転フラグ
        bool bathAlerted = false;     // お風呂通知済みフラグ
        float bathTargetCm = 20.0f;   // 検知目標距離 (cm)
        int bathManualCm = 0;         // 手動設定中の距離 (cm)
        int blinkCount = 0;
        int maxBlinks = 0;
        unsigned long lastBlinkTime = 0;
        unsigned long blinkDuration = 0;
    };

    // センサー関連のデータ
    struct SensorData {
        volatile float temperature = -999.0;         // (コア1が書き込み、コア0が読み取り)
        volatile float humidity = -999.0;             // (コア1が書き込み、コア0が読み取り)
        volatile float outdoorTemperature = -999.0;  // 親機の室外温度
        volatile float outdoorHumidity = -999.0;     // 親機の室外湿度
        float distance = -1.0;
        unsigned long distanceStableTime = 0;
        unsigned long lastMeasureTime = 0;
    };
    
    // タイマー関連
    struct TimerState {
        unsigned long lastActivityTime = 0;
        unsigned long outdoorDisplayUntil = 0; // 室外データ表示終了時刻 (0=非表示)
    };

    // グローバルオブジェクトと変数のインスタンス化
    MenuState menu;
    SystemState system;
    SensorData sensors;
    TimerState timers;

    LiquidCrystal lcd(Pins::LCD_RS, Pins::LCD_E, Pins::LCD_D4, Pins::LCD_D5, Pins::LCD_D6, Pins::LCD_D7);
    WiFiServer server(80);
    WiFiUDP WOL_UDP;

    JsonDocument lightningHistoryDoc;
    int historyPage = 0;
    int ac_temperature = 25;
    bool distanceTriggeredByParent = false;
    unsigned long lastParentRequestTime = 0;
    bool parentConnectedOnce = false;
}

// コア間通信フラグ
volatile bool requestBathNotify = false;           // Core0→Core1: 親機にお風呂通知を送る
volatile bool requestBathClear = false;            // Core0→Core1: 親機にお風呂クリアを送る
volatile bool requestNotifyDistanceStopped = false; // Core0→Core1: 子機が距離測定を終了したことを親機に通知する
volatile bool requestDistancePush = false;         // Core0→Core1: 距離値を親機にプッシュ送信する
volatile int  distancePushValue10 = -10;           // Core0→Core1: プッシュ値 (cm x 10, -10=エラー)
unsigned long distanceStopCooldownUntil = 0;       // 終了後の再起動防止クールダウン

//================================================================
// メニュー & コマンドデータ定義
//================================================================
const char* DEVICE_MENU_TEXT[] = {"1.Light", "2.TV", "3.Air Con", "4.Fan", "5.Speaker", "6.Others"};
const char* DEVICE_IDS[] = {DEVICE_ID_LIGHT, DEVICE_ID_TV, DEVICE_ID_AC, DEVICE_ID_FAN, DEVICE_ID_SPEAKER, DEVICE_ID_OTHERS};
const int NUM_DEVICE_MENU_ITEMS = sizeof(DEVICE_MENU_TEXT) / sizeof(char*);

const char* LIGHT_CMD_TEXT[] = {"1.Turn ON", "2.Turn OFF", "3.Bright UP", "4.Bright DOWN"};
const int NUM_LIGHT_CMDS = sizeof(LIGHT_CMD_TEXT) / sizeof(char*);

const char* TV_CMD_TEXT[] = {"1.Turn ON", "2.Turn OFF", "3.CH UP", "4.CH DOWN", "5.Vol UP", "6.Vol DOWN"};
const int NUM_TV_CMDS = sizeof(TV_CMD_TEXT) / sizeof(char*);

const char* FAN_CMD_TEXT[] = {"1.Turn ON", "2.Turn OFF", "3.Speed UP"};
const int NUM_FAN_CMDS = sizeof(FAN_CMD_TEXT) / sizeof(char*);

const char* ON_OFF_CMD_TEXT[] = {"1.Turn ON", "2.Turn OFF"};
const int NUM_ON_OFF_CMDS = sizeof(ON_OFF_CMD_TEXT) / sizeof(char*);

const char* WOL_MENU_TEXT[] = {"1.Desktop PC", "2.Server PC"};
const char* WOL_MAC_ADDRESSES[] = {MAC_DESKTOP, MAC_SERVER};
const int NUM_WOL_ITEMS = sizeof(WOL_MENU_TEXT) / sizeof(char*);


//================================================================
// プロトタイプ宣言
//================================================================
namespace Display { void showError(const char* message); }
namespace Network { void connectWiFi(); }
namespace Sensors { float measureDistance(); }
void changeMode(State::Mode newMode);
void setup1();
void loop1();

//================================================================
// メニュー定義
//================================================================
namespace Menu {
    struct MenuItem {
        const char* text;
        void (*action)(); // 実行するアクション
    };

    // 各メニューのアクション関数
    void enterHistory() { if (State::system.wifiConnected) { changeMode(State::HISTORY); } else { Display::showError("WiFi Required"); } }
    void enterAppliances() { if (State::system.wifiConnected) { changeMode(State::SWITCHBOT_DEVICE_SELECT); } else { Display::showError("WiFi Required"); } }
    void enterWol() { if (State::system.wifiConnected) { changeMode(State::WOL_SELECT); } else { Display::showError("WiFi Required"); } }
    void enterDistanceMeasure() { changeMode(State::DISTANCE_MEASURE); }
    void enterBathMode() {
        State::system.bathAlerted = false;
        State::menu.menuSelection = 0;
        changeMode(State::BATH_MODE_SUBMENU);
    }
    void toggleLedAlwaysOn() { 
        State::system.ledAlwaysOn = !State::system.ledAlwaysOn;
        State::lcd.clear();
        State::lcd.print("LED Always ON");
        State::lcd.setCursor(0, 1);
        State::lcd.print(State::system.ledAlwaysOn ? "Enabled" : "Disabled");
        delay(1500);
        changeMode(State::MAIN_DISPLAY);
    }
    void resyncTime() { 
        if (State::system.wifiConnected) {
            State::lcd.clear();
            State::lcd.print("Resyncing Time..");
            State::system.ntpInitialized = false; // コア1がこのフラグを見て再同期する
            delay(1500);
            changeMode(State::MAIN_DISPLAY);
        } else {
            Display::showError("WiFi Required");
        }
    }
    void getDeviceList() { if (State::system.wifiConnected) { changeMode(State::GET_DEVICE_LIST); } else { Display::showError("WiFi Required"); } }

    // メインメニューの項目
    const MenuItem mainMenu[] = {
        {"1.History",      enterHistory},
        {"2.Appliances",   enterAppliances},
        {"3.Wake on LAN",  enterWol},
        {"4.Measure Dist", enterDistanceMeasure},
        {"5.Bath Mode",    enterBathMode},
        {"6.LED Always ON",toggleLedAlwaysOn},
        {"7.Resync Time",  resyncTime},
        {"8.Get DeviceIDs",getDeviceList}
    };
    const int NUM_MAIN_MENU_ITEMS = sizeof(mainMenu) / sizeof(mainMenu[0]);
}

//================================================================
// ユーティリティ & モード変更
//================================================================
void changeMode(State::Mode newMode) {
    State::menu.currentMode = newMode;
    State::system.needsRedraw = true;
    State::timers.lastActivityTime = millis();
    if (newMode == State::MAIN_DISPLAY) State::system.forceMainScreenRedraw = true;
    if (newMode == State::HISTORY) State::historyPage = 0;
    if (newMode != State::DISTANCE_MEASURE) State::distanceTriggeredByParent = false;
    DEBUG_PRINTF("[Core 0] Mode changed to: %d\n", newMode);
}

void startBlinking(int times, int duration) {
    if (State::system.isBlinking) return;
    State::system.isBlinking = true;
    State::system.maxBlinks = times * 2;
    State::system.blinkCount = 0;
    State::system.blinkDuration = duration;
    State::system.lastBlinkTime = millis();
    digitalWrite(Pins::LED_PIN, HIGH);
    State::system.blinkCount++;
}

void handleBlink() {
    if (!State::system.isBlinking) return;
    if (millis() - State::system.lastBlinkTime >= State::system.blinkDuration) {
        State::system.lastBlinkTime = millis();
        if (State::system.blinkCount >= State::system.maxBlinks) {
            State::system.isBlinking = false;
        } else {
            digitalWrite(Pins::LED_PIN, !digitalRead(Pins::LED_PIN));
            State::system.blinkCount++;
        }
    }
}

//================================================================
// ディスプレイ管理 (コア0)
//================================================================
namespace Display {
    // 文字列を逆順にするヘルパー
    void reverseStr(char* str, int len) {
        for (int i = 0; i < len / 2; i++) {
            char tmp = str[i];
            str[i] = str[len - 1 - i];
            str[len - 1 - i] = tmp;
        }
    }

    void printLcdLine(int line, const char* text) {
        char buf[17];
        snprintf(buf, sizeof(buf), "%-16s", text);
        if (State::system.displayFlipped) {
            reverseStr(buf, 16);   // 文字を左右反転
            line = 1 - line;       // 行を上下入れ替え (0↔1)
        }
        State::lcd.setCursor(0, line);
        State::lcd.print(buf);
    }

    void showError(const char* message) {
        State::lcd.clear();
        printLcdLine(0, message);
        delay(2000);
        changeMode(State::MAIN_DISPLAY);
    }

    void updateMainDisplay() {
        static unsigned long lastTimeUpdate = 0;
        // 画面の更新頻度を上げて点滅を滑らかにする (250ms = 1秒に4回)
        if (State::system.forceMainScreenRedraw || millis() - lastTimeUpdate > 250) {
            char timeBuf[17];
            // WiFi接続時のみ時刻を表示
            if (State::system.wifiConnected) {
                if(State::system.ntpInitialized && time(nullptr) > 100000) {
                    time_t now_t = time(nullptr);
                    struct tm* now_tm = localtime(&now_t);
                    // millis() を使って0.5秒間隔の点滅を実現
                    if ((millis() / 500) % 2 == 0) { // 500ミリ秒ごとに表示/非表示を切り替え
                        strftime(timeBuf, sizeof(timeBuf), "%m/%d(%a) %H:%M", now_tm);
                    } else {
                        strftime(timeBuf, sizeof(timeBuf), "%m/%d(%a) %H %M", now_tm);
                    }
                } else {
                    // NTPアニメーション: ...→ ..→  .→   →.  →.. →...
                    static const char* syncDots[] = {"...", " ..", "  .", "   ", ".  ", ".. "};
                    int frame = (millis() / 400) % 6;
                    snprintf(timeBuf, sizeof(timeBuf), "Syncing time%s", syncDots[frame]);
                }
            } else {
                // オフライン時はメッセージを表示
                strcpy(timeBuf, "WiFi Offline");
            }
            printLcdLine(0, timeBuf);
            
            char sensorBuf[17];
            if (State::timers.outdoorDisplayUntil > 0 && millis() < State::timers.outdoorDisplayUntil) {
                // 室外データを3秒表示
                if (State::sensors.outdoorTemperature > -50.0)
                    sprintf(sensorBuf, "O:%.1fC H:%.1f%%", State::sensors.outdoorTemperature, State::sensors.outdoorHumidity);
                else
                    strcpy(sensorBuf, "No Outdoor Data");
            } else {
                State::timers.outdoorDisplayUntil = 0;
                if(State::sensors.temperature > -50.0) sprintf(sensorBuf, "T:%.1fC H:%.1f%%", State::sensors.temperature, State::sensors.humidity);
                else strcpy(sensorBuf, "No Parent Data");
            }
            printLcdLine(1, sensorBuf);

            State::system.forceMainScreenRedraw = false;
            lastTimeUpdate = millis();
        }
    }

    void drawPagedMenu(const Menu::MenuItem items[], int numItems, int &selection) {
        int page = selection / 2;
        for (int i=0; i<2; i++) {
            int itemIndex = page * 2 + i;
            char line[17];
            if (itemIndex < numItems) {
                snprintf(line, sizeof(line), "%c %-14.14s", (selection == itemIndex ? '>' : ' '), items[itemIndex].text);
            } else {
                strcpy(line, "");
            }
            printLcdLine(i, line);
        }
    }
     void drawPagedMenu(const char* items[], int numItems, int &selection) {
        int page = selection / 2;
        for (int i=0; i<2; i++) {
            int itemIndex = page * 2 + i;
            char line[17];
            if (itemIndex < numItems) {
                snprintf(line, sizeof(line), "%c %-14.14s", (selection == itemIndex ? '>' : ' '), items[itemIndex]);
            } else {
                strcpy(line, "");
            }
            printLcdLine(i, line);
        }
    }

    void drawBathSubMenu() {
        char l0[17], l1[17];
        snprintf(l0, sizeof(l0), "%c 1.Default20cm", (State::menu.menuSelection == 0 ? '>' : ' '));
        snprintf(l1, sizeof(l1), "%c 2.Manual Set", (State::menu.menuSelection == 1 ? '>' : ' '));
        printLcdLine(0, l0);
        printLcdLine(1, l1);
    }

    void drawBathManualScreen() {
        char l0[17];
        snprintf(l0, sizeof(l0), "Set: %d cm", State::system.bathManualCm);
        printLcdLine(0, l0);
        printLcdLine(1, "[+5] LPr:Start");
    }
}

//================================================================
// センサー & ネットワーク関数
//================================================================
namespace Network {
    // ★★★ OTA機能の初期化（コア1で実行） ★★★
    void initOTA() {
        if (State::system.otaInitialized || !State::system.wifiConnected) {
            return;
        }
        // Arduino IDEで表示されるホスト名（識別名）を設定
        ArduinoOTA.setHostname("pico-w-controller");

        // 書き込み開始時の処理
        ArduinoOTA.onStart([]() {
            String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
            DEBUG_PRINTLN("Start updating " + type);
        });
        // 書き込み終了時の処理
        ArduinoOTA.onEnd([]() {
            DEBUG_PRINTLN("\nEnd");
        });
        // 書き込み中の進捗表示
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            DEBUG_PRINTF("Progress: %u%%\r", (progress / (total / 100)));
        });
        // エラー発生時の処理
        ArduinoOTA.onError([](ota_error_t error) {
            DEBUG_PRINTF("Error[%u]: ", error);
            if (error == OTA_AUTH_ERROR) DEBUG_PRINTLN("Auth Failed");
            else if (error == OTA_BEGIN_ERROR) DEBUG_PRINTLN("Begin Failed");
            else if (error == OTA_CONNECT_ERROR) DEBUG_PRINTLN("Connect Failed");
            else if (error == OTA_RECEIVE_ERROR) DEBUG_PRINTLN("Receive Failed");
            else if (error == OTA_END_ERROR) DEBUG_PRINTLN("End Failed");
        });

        ArduinoOTA.begin(); // OTAサービスを開始
        State::system.otaInitialized = true;
        DEBUG_PRINTLN("[Core 1] OTA Ready. Hostname: pico-w-controller");
    }

    bool macStringToBytes(const char* macStr, byte* bytes) {
        if (strlen(macStr) != 17) return false;
        for (int i = 0; i < 6; i++) {
            char hex[3] = {macStr[i * 3], macStr[i * 3 + 1], '\0'};
            bytes[i] = (byte)strtol(hex, NULL, 16);
            if (i < 5 && macStr[i * 3 + 2] != ':') return false;
        }
        return true;
    }

    void sendWolPacket(const char* macAddressString) {
        byte macAddressBytes[6];
        if (!macStringToBytes(macAddressString, macAddressBytes)) {
            Display::showError("Invalid MAC Addr");
            changeMode(State::MAIN_DISPLAY);
            return;
        }
        byte magicPacket[102];
        memset(magicPacket, 0xFF, 6);
        for (int i = 0; i < 16; i++) memcpy(&magicPacket[6 + i * 6], macAddressBytes, 6);
        
        State::lcd.clear();
        Display::printLcdLine(0, "Sending WOL...");

        IPAddress broadcastIp = WiFi.localIP();
        broadcastIp[3] = 255;
        
        if (State::WOL_UDP.beginPacket(broadcastIp, 9)) {
            State::WOL_UDP.write(magicPacket, sizeof(magicPacket));
            State::WOL_UDP.endPacket();
            Display::printLcdLine(0, "WOL Packet Sent");
            Display::printLcdLine(1, macAddressString);
        } else {
            Display::printLcdLine(0, "WOL Send Fail");
        }
        delay(2000);
        changeMode(State::MAIN_DISPLAY);
    }
    
    void sendSwitchBotCommand(const char* deviceId, const char* command, const char* parameter) {
        State::lcd.clear();
        Display::printLcdLine(0, "Sending...");
        WiFiClientSecure client;
        client.setInsecure();
        if (client.connect("api.switch-bot.com", 443)) {
            String jsonBody = "{\"command\": \"" + String(command) + "\", \"parameter\": \"" + String(parameter) + "\", \"commandType\": \"command\"}";
            client.println("POST /v1.1/devices/" + String(deviceId) + "/commands HTTP/1.1");
            client.println("Host: api.switch-bot.com");
            client.println("Content-Type: application/json; charset=utf8");
            client.printf("Authorization: %s\r\n", SWITCHBOT_TOKEN);
            client.printf("Content-Length: %d\r\n\r\n", jsonBody.length());
            client.print(jsonBody);
            unsigned long timeout = millis();
            while (client.connected() && !client.available()){ if(millis() - timeout > 2000) break; }
            client.stop();
            Display::printLcdLine(0, "Command Sent!");
        } else {
            Display::printLcdLine(0, "API Conn. Fail");
        }
        delay(1500);
        changeMode(State::MAIN_DISPLAY);
    }
    
    void fetchAndDrawHistory() {
        Display::printLcdLine(0, "Fetching Hist...");
        WiFiClient client;
        if (!client.connect(PARENT_IP, PARENT_PORT)) {
            Display::showError("Fetch Failed");
            return;
        }
        client.println("GET /history HTTP/1.1");
        client.print("Host: "); client.println(PARENT_IP);
        client.println("Connection: close\r\n");

        unsigned long timeout = millis();
        while (client.available() == 0) if (millis() - timeout > 5000) { client.stop(); Display::showError("Timeout"); return; }
        while (client.available()) { String line = client.readStringUntil('\n'); if (line == "\r") break; }
        
        if (deserializeJson(State::lightningHistoryDoc, client) != DeserializationError::Ok) {
            Display::showError("JSON Parse Err");
        } else {
            JsonArray history = State::lightningHistoryDoc.as<JsonArray>();
            State::lcd.clear();
            if (history.isNull() || history.size() == 0) {
                Display::printLcdLine(0, "No History Data");
                return;
            }
            int startIndex = State::historyPage * 2;
            for (int i = 0; i < 2; i++) {
                if (startIndex + i < history.size()) {
                    JsonObject item = history[startIndex + i];
                    char buf[17];
                    snprintf(buf, sizeof(buf), "%d:%s %dkm", startIndex + i + 1, item["timestamp"].as<const char*>(), item["distance"].as<int>());
                    Display::printLcdLine(i, buf);
                }
            }
        }
        client.stop();
    }

    // ★★★ マルチコア対応: この関数はコア1で実行される ★★★
    void handleParentCommunication() {
        if (!State::system.wifiConnected) return; // オフラインなら何もしない
        WiFiClient client;
        if (!client.connect(PARENT_IP, PARENT_PORT)) {
            DEBUG_PRINTLN("[Core 1] Parent connection failed");
            State::sensors.temperature = -999.0; State::sensors.humidity = -999.0; return;
        }
        if(!State::parentConnectedOnce) { startBlinking(3, 166); State::parentConnectedOnce = true; }
        client.println("GET /data HTTP/1.1");
        client.print("Host: "); client.println(PARENT_IP);
        client.println("Connection: close\r\n");
        unsigned long timeout = millis();
        while (client.available() == 0) if (millis() - timeout > 3000) { client.stop(); return; }
        if (client.find("{")) {
            String jsonPayload = "{" + client.readString();
            JsonDocument doc;
            if (deserializeJson(doc, jsonPayload) == DeserializationError::Ok) {
                // volatile変数に書き込む
                State::sensors.temperature = doc["temperature"];
                State::sensors.humidity = doc["humidity"];
                if (doc["outdoor_temperature"].is<float>())
                    State::sensors.outdoorTemperature = doc["outdoor_temperature"].as<float>();
                if (doc["outdoor_humidity"].is<float>())
                    State::sensors.outdoorHumidity = doc["outdoor_humidity"].as<float>();
            }
        }
        client.stop();
    }
    
    // ★★★ マルチコア対応: この関数はコア1で実行される ★★★
    void handleServerClient() {
        if (!State::system.wifiConnected) return;
        WiFiClient client = State::server.accept();
        if (client) {
            DEBUG_PRINTLN("[Core 1] Client connected");
            String req = client.readStringUntil('\n');
            if (req.startsWith("GET /distance_stop")) {
                // 親機が距離測定を終了 → 子機もメイン画面へ
                State::distanceTriggeredByParent = false;
                distanceStopCooldownUntil = millis() + 3000; // 3秒クールダウン
                if (State::menu.currentMode == State::DISTANCE_MEASURE) {
                    changeMode(State::MAIN_DISPLAY);
                }
                client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n");
                client.print("OK");
            } else if (req.startsWith("GET /distance_start")) {
                // 親機からの測定開始要求
                if (millis() >= distanceStopCooldownUntil) {
                    State::distanceTriggeredByParent = true;
                    changeMode(State::DISTANCE_MEASURE);
                }
                client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n");
                client.print("OK");
            } else if (req.startsWith("GET /distance")) {
                // 現在値を返すのみ（モード変更なし）
                float dist = State::sensors.distance;
                JsonDocument doc; doc["distance"] = (dist >= 0) ? dist : -1.0;
                client.println("HTTP/1.1 200 OK");
                client.println("Content-Type: application/json");
                client.println("Connection: close\r\n");
                serializeJson(doc, client);
            } else if (req.startsWith("GET /bath_clear")) {
                // 親機からのお風呂クリア通知
                State::system.bathAlerted = false;
                State::system.isBlinking = false;
                digitalWrite(Pins::LED_PIN, LOW);
                if (State::menu.currentMode == State::BATH_MODE) {
                    changeMode(State::MAIN_DISPLAY);
                }
                client.println("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n");
                client.print("OK");
            } else {
                client.println("HTTP/1.1 404 Not Found\r\n");
            }
            delay(1);
            client.stop();
        }
    }

    // ★★★ マルチコア対応: この関数はコア1のsetup1から呼ばれる ★★★
    void connectWiFi() {
        DEBUG_PRINTLN("[Core 1] Connecting to WiFi...");
        WiFi.disconnect();
        WiFi.mode(WIFI_STA);

        if (USE_STATIC_IP) {
            IPAddress staticIP(STATIC_IP_BYTES), gateway(GATEWAY_BYTES), subnet(SUBNET_BYTES), primaryDNS(PRIMARY_DNS_BYTES);
            WiFi.config(staticIP, primaryDNS, gateway, subnet);
        }
        
        unsigned long connectStart = millis();
        int credIndex = 0;
        // 30秒のタイムアウト
        while(millis() - connectStart < 30000) {
            const char* current_ssid = wifiCredentials[credIndex].ssid;
            DEBUG_PRINTF("[Core 1] Trying SSID: %s\n", current_ssid);
            WiFi.begin(current_ssid, wifiCredentials[credIndex].password);
            
            unsigned long attemptStart = millis();
            while (millis() - attemptStart < 10000) {
                if (WiFi.status() == WL_CONNECTED) {
                    State::system.wifiConnected = true; // volatileフラグを立てる
                    DEBUG_PRINTLN("\n[Core 1] WiFi connected!");
                    DEBUG_PRINT("[Core 1] IP address: "); DEBUG_PRINTLN(WiFi.localIP());
                    return; // 接続成功
                }
                delay(500);
            }
            credIndex = (credIndex + 1) % numWifiCredentials;
        }

        // タイムアウトした場合
        State::system.wifiConnected = false;
        DEBUG_PRINTLN("\n[Core 1] Failed to connect to WiFi.");
    }
}

namespace Sensors {
    float measureDistance() {
        digitalWrite(Pins::TRIG_PIN, LOW); delayMicroseconds(2);
        digitalWrite(Pins::TRIG_PIN, HIGH); delayMicroseconds(10);
        digitalWrite(Pins::TRIG_PIN, LOW);
        long duration = pulseIn(Pins::ECHO_PIN, HIGH, 25000UL);
        return (duration == 0) ? -1.0 : duration * 0.017;
    }

    void handleDistanceMeasureScreen() {
        if (millis() - State::sensors.lastMeasureTime > 250) {
            State::sensors.lastMeasureTime = millis();
            float currentDist = measureDistance();
            char buf[17];
            if(State::system.needsRedraw) {
                State::lcd.clear();
                Display::printLcdLine(0, State::distanceTriggeredByParent ? "Remote Distance" : "Distance");
                State::system.needsRedraw = false;
                State::sensors.distanceStableTime = millis();
            }

            if (currentDist >= 0) {
                sprintf(buf, "Dist: %.1f cm", currentDist);
                // ★先に差分チェックしてからdistanceを更新（順序が重要）
                if (abs(currentDist - State::sensors.distance) > 0.5) {
                    State::sensors.distanceStableTime = millis();
                }
                State::sensors.distance = currentDist;
            } else {
                strcpy(buf, "Measure Error");
                State::sensors.distanceStableTime = millis();
            }
            Display::printLcdLine(1, buf);
            // 親機からのリモート中は距離値を親機にプッシュ
            if (State::distanceTriggeredByParent) {
                distancePushValue10 = (currentDist >= 0) ? (int)(currentDist * 10) : -10;
                requestDistancePush = true;
            }
        }

        // リモート・手動共通: 5秒間距離が変化しなければメイン画面に戻る
        // （リモートの場合は親機から /distance_stop が来た場合も別途即時終了する）
        if (State::sensors.distanceStableTime > 0
            && millis() - State::sensors.distanceStableTime > 5000) {
            State::distanceTriggeredByParent = false;
            distanceStopCooldownUntil = millis() + 3000; // 3秒クールダウン
            // 親機がまだULTRASONIC_MONITORにいる場合、終了を通知する
            requestNotifyDistanceStopped = true;
            changeMode(State::MAIN_DISPLAY);
        }
    }

    // ブザーを短く3回鳴らす (合計約3秒, ブロッキング)
    void triggerBuzzer3() {
        for (int i = 0; i < 3; i++) {
            digitalWrite(Pins::BUZZER_PIN, HIGH);
            delay(300);
            digitalWrite(Pins::BUZZER_PIN, LOW);
            delay(700);
        }
    }

    void handleBathModeScreen() {
        if (State::system.bathAlerted) {
            // --- 通知後フェーズ: メッセージ + 時刻を固定表示 ---
            if (State::system.needsRedraw) {
                State::lcd.clear();
                Display::printLcdLine(0, "Bath is Ready!");
                char timeBuf[17] = "Time: --:--";
                if (State::system.ntpInitialized && time(nullptr) > 100000) {
                    time_t t = time(nullptr);
                    struct tm* tm = localtime(&t);
                    char tmp[9];
                    strftime(tmp, sizeof(tmp), "%H:%M", tm);
                    snprintf(timeBuf, sizeof(timeBuf), "Time: %s", tmp);
                }
                Display::printLcdLine(1, timeBuf);
                State::system.needsRedraw = false;
            }
            // LED は loop() の末尾で 1 秒点滅する
            return;
        }

        // --- 監視フェーズ: 現在距離 / 目標距離を表示 ---
        if (millis() - State::sensors.lastMeasureTime > 200) {
            State::sensors.lastMeasureTime = millis();
            float currentDist = measureDistance();

            char line0[17], line1[17];
            if (currentDist >= 0) {
                State::sensors.distance = currentDist;
                snprintf(line0, sizeof(line0), "Now:  %.1f cm", currentDist);
            } else {
                strcpy(line0, "Now:  Error");
            }
            snprintf(line1, sizeof(line1), "Goal: %.1f cm", State::system.bathTargetCm);

            if (State::system.needsRedraw) { State::lcd.clear(); State::system.needsRedraw = false; }
            Display::printLcdLine(0, line0);
            Display::printLcdLine(1, line1);

            // 目標距離以下になったら通知
            if (currentDist >= 0 && currentDist <= State::system.bathTargetCm) {
                State::system.bathAlerted = true;
                State::system.needsRedraw = true;

                // ブザー3回 (約3秒)
                triggerBuzzer3();

                // 親機に通知リクエストをセット (Core1 が送信)
                requestBathNotify = true;
            }
        }
        // 長押しで MAIN_DISPLAY に戻る処理は Input::handleButton で行う
    }
}

//================================================================
// 入力処理 (コア0)
//================================================================
namespace Input {
    bool buttonState = HIGH, lastButtonState = HIGH;
    unsigned long lastDebounceTime = 0, buttonPressStartTime = 0;
    bool longPressHandled = false;

    void handleButton() {
        bool reading = digitalRead(Pins::BUTTON_PIN);
        if (reading != lastButtonState) lastDebounceTime = millis();
        
        if ((millis() - lastDebounceTime) > 50) {
            if (reading != buttonState) {
                buttonState = reading;
                State::timers.lastActivityTime = millis();
                if (buttonState == LOW) { // Press
                    buttonPressStartTime = millis();
                    longPressHandled = false;
                } else { // Release
                    if (!longPressHandled) { // Short Press
                        // BATH_MODE で通知済みなら短押しで解除
                        if (State::menu.currentMode == State::BATH_MODE && State::system.bathAlerted) {
                            State::system.bathAlerted = false;
                            State::system.isBlinking = false;
                            digitalWrite(Pins::LED_PIN, LOW);
                            requestBathClear = true; // 親機にクリア通知
                            changeMode(State::MAIN_DISPLAY);
                        } else {
                        State::system.needsRedraw = true;
                        switch (State::menu.currentMode) {
                            case State::MAIN_DISPLAY:
                                // メイン画面での短押し: 室外温湿度を3秒表示
                                State::timers.outdoorDisplayUntil = millis() + 3000;
                                State::system.needsRedraw = true;
                                break;
                            case State::MENU: State::menu.menuSelection = (State::menu.menuSelection + 1) % Menu::NUM_MAIN_MENU_ITEMS; break;
                            case State::BATH_MODE_SUBMENU: State::menu.menuSelection = (State::menu.menuSelection + 1) % 2; break;
                            case State::BATH_MODE_MANUAL: State::system.bathManualCm += 5; break;
                            case State::SWITCHBOT_DEVICE_SELECT: State::menu.deviceSelection = (State::menu.deviceSelection + 1) % NUM_DEVICE_MENU_ITEMS; break;
                            case State::WOL_SELECT: State::menu.menuSelection = (State::menu.menuSelection + 1) % NUM_WOL_ITEMS; break;
                            case State::DEVICE_CONTROL: {
                                int numCmds = 0;
                                switch(State::menu.deviceSelection){
                                    case 0: numCmds = NUM_LIGHT_CMDS; break;
                                    case 1: numCmds = NUM_TV_CMDS; break;
                                    case 2: numCmds = 5; break;
                                    case 3: numCmds = NUM_FAN_CMDS; break;
                                    case 4: case 5: numCmds = NUM_ON_OFF_CMDS; break;
                                }
                                if (numCmds > 0) State::menu.commandSelection = (State::menu.commandSelection + 1) % numCmds;
                                break;
                            }
                            default: break;
                        }
                        }
                    }
                }
            }
        }
        lastButtonState = reading;

        if (buttonState == LOW && !longPressHandled && (millis() - buttonPressStartTime > 1000)) {
            longPressHandled = true;
            State::timers.lastActivityTime = millis();
            State::system.needsRedraw = true;
            
            switch (State::menu.currentMode) {
                case State::MAIN_DISPLAY: changeMode(State::MENU); break;
                case State::MENU: Menu::mainMenu[State::menu.menuSelection].action(); break;
                case State::BATH_MODE_SUBMENU:
                    if (State::menu.menuSelection == 0) {
                        // 1.デフォルト: 20cm で検知開始
                        State::system.bathTargetCm = 20.0f;
                        State::system.bathAlerted = false;
                        changeMode(State::BATH_MODE);
                    } else {
                        // 2.手動設定: 距離入力画面へ
                        State::system.bathManualCm = 0;
                        changeMode(State::BATH_MODE_MANUAL);
                    }
                    break;
                case State::BATH_MODE_MANUAL:
                    // 長押しで選択距離を確定してモニタリング開始
                    State::system.bathTargetCm = (float)State::system.bathManualCm;
                    State::system.bathAlerted = false;
                    State::menu.menuSelection = 0;
                    changeMode(State::BATH_MODE);
                    break;
                case State::DISTANCE_MEASURE:
                    // 長押しで距離測定を終了し、親機にも通知
                    State::distanceTriggeredByParent = false;
                    distanceStopCooldownUntil = millis() + 3000; // 3秒クールダウン
                    requestNotifyDistanceStopped = true;
                    changeMode(State::MAIN_DISPLAY);
                    break;
                case State::BATH_MODE:
                    // 長押しでお風呂モードを完全終了
                    State::system.bathAlerted = false;
                    State::system.isBlinking = false;
                    digitalWrite(Pins::LED_PIN, LOW);
                    if (requestBathNotify == false) requestBathClear = true; // 通知済みならクリアも送信
                    changeMode(State::MAIN_DISPLAY);
                    break;
                case State::SWITCHBOT_DEVICE_SELECT: State::menu.commandSelection = 0; changeMode(State::DEVICE_CONTROL); break;
                case State::WOL_SELECT: Network::sendWolPacket(WOL_MAC_ADDRESSES[State::menu.menuSelection]); break;
                case State::DEVICE_CONTROL: {
                    const char* deviceId = DEVICE_IDS[State::menu.deviceSelection];
                    switch (State::menu.deviceSelection) {
                        case 0: // Light
                            if (State::menu.commandSelection == 0) Network::sendSwitchBotCommand(deviceId, "turnOn", "default");
                            else if (State::menu.commandSelection == 1) Network::sendSwitchBotCommand(deviceId, "turnOff", "default");
                            else if (State::menu.commandSelection == 2) Network::sendSwitchBotCommand(deviceId, "brightnessUp", "default");
                            else if (State::menu.commandSelection == 3) Network::sendSwitchBotCommand(deviceId, "brightnessDown", "default");
                            break;
                        case 1: // TV
                            if (State::menu.commandSelection == 0) Network::sendSwitchBotCommand(deviceId, "turnOn", "default");
                            else if (State::menu.commandSelection == 1) Network::sendSwitchBotCommand(deviceId, "turnOff", "default");
                            else if (State::menu.commandSelection == 2) Network::sendSwitchBotCommand(deviceId, "channelUp", "default");
                            else if (State::menu.commandSelection == 3) Network::sendSwitchBotCommand(deviceId, "channelDown", "default");
                            else if (State::menu.commandSelection == 4) Network::sendSwitchBotCommand(deviceId, "volumeAdd", "default");
                            else if (State::menu.commandSelection == 5) Network::sendSwitchBotCommand(deviceId, "volumeSub", "default");
                            break;
                        case 2: // AC
                            {
                                char param[30];
                                if (State::menu.commandSelection == 0) Network::sendSwitchBotCommand(deviceId, "turnOn", "default");
                                else if (State::menu.commandSelection == 1) Network::sendSwitchBotCommand(deviceId, "turnOff", "default");
                                else if (State::menu.commandSelection == 2) { State::ac_temperature++; sprintf(param, "%d,2,1,auto", State::ac_temperature); Network::sendSwitchBotCommand(deviceId, "setAll", param); }
                                else if (State::menu.commandSelection == 3) { State::ac_temperature--; sprintf(param, "%d,2,1,auto", State::ac_temperature); Network::sendSwitchBotCommand(deviceId, "setAll", param); }
                                else if (State::menu.commandSelection == 4) Network::sendSwitchBotCommand(deviceId, "setAll", "25,1,1,auto");
                            }
                            break;
                        case 3: // Fan
                            if (State::menu.commandSelection == 0) Network::sendSwitchBotCommand(deviceId, "turnOn", "default");
                            else if (State::menu.commandSelection == 1) Network::sendSwitchBotCommand(deviceId, "turnOff", "default");
                            else if (State::menu.commandSelection == 2) Network::sendSwitchBotCommand(deviceId, "fanSpeed", "1");
                            break;
                        case 4: case 5: // Speaker, Others
                            if (State::menu.commandSelection == 0) Network::sendSwitchBotCommand(deviceId, "turnOn", "default");
                            else if (State::menu.commandSelection == 1) Network::sendSwitchBotCommand(deviceId, "turnOff", "default");
                            break;
                    }
                    break;
                  }
                default: changeMode(State::MAIN_DISPLAY); break;
            }
        }
    }
}

//================================================================
// Setup & Loop (コア0)
//================================================================
void setup() {
    Serial.begin(115200);
    // コア0のハードウェア初期化
    pinMode(Pins::LED_PIN, OUTPUT);
    pinMode(Pins::BUTTON_PIN, INPUT_PULLUP);
    pinMode(Pins::TILT_PIN, INPUT_PULLUP);   // 傾斜スイッチ
    pinMode(Pins::BUZZER_PIN, OUTPUT);       // ブザー
    digitalWrite(Pins::BUZZER_PIN, LOW);
    pinMode(Pins::TRIG_PIN, OUTPUT);
    pinMode(Pins::ECHO_PIN, INPUT);
    State::lcd.begin(16, 2);
    
    digitalWrite(Pins::LED_PIN, HIGH);
    State::lcd.clear();
    Display::printLcdLine(0, "Initializing...");
    delay(1000);
    digitalWrite(Pins::LED_PIN, LOW);
    
    DEBUG_PRINTLN("[Core 0] Starting Core 1 for network tasks.");
    rp2040.restartCore1(); // コア1を起動

    // コア1がWi-Fiに接続するのを待つ（10秒タイムアウト→オフライン時もメイン画面へ）
    State::lcd.clear();
    Display::printLcdLine(0, "Connecting WiFi");
    unsigned long wifi_wait_start = millis();
    while(!State::system.wifiConnected && millis() - wifi_wait_start < 10000) {
        static const char* connDots[] = {"...", " ..", "  .", "   ", ".  ", ".. "};
        int cf = ((millis() - wifi_wait_start) / 400) % 6;
        char connBuf[17];
        snprintf(connBuf, sizeof(connBuf), "WiFi%s", connDots[cf]);
        State::lcd.setCursor(0, 1);
        State::lcd.print(connBuf);
        delay(100);
    }

    State::lcd.clear();
    if(State::system.wifiConnected){
        Display::printLcdLine(0, "WiFi Connected!");
        Display::printLcdLine(1, WiFi.localIP().toString().c_str());
        delay(2000);
    } else {
        Display::printLcdLine(0, "WiFi Failed");
        Display::printLcdLine(1, "Offline Mode");
        delay(2000);
    }
    
    changeMode(State::MAIN_DISPLAY);
    DEBUG_PRINTLN("[Core 0] Setup complete.");
}

void loop() { // コア0のメインループ
    Input::handleButton();  
    handleBlink();  

    // ---- 傾斜スイッチによる表示反転 (デバウンス付き) ----
    {
        static bool lastTiltRaw = HIGH;
        static unsigned long tiltDebounceTime = 0;
        bool tiltRaw = digitalRead(Pins::TILT_PIN);
        if (tiltRaw != lastTiltRaw) {
            tiltDebounceTime = millis();
            lastTiltRaw = tiltRaw;
        }
        if (millis() - tiltDebounceTime > 80) {
            bool flipped = (tiltRaw == HIGH); // HIGH = 球が離れている = 逆さ
            if (flipped != State::system.displayFlipped) {
                State::system.displayFlipped = flipped;
                State::lcd.clear();
                State::system.needsRedraw = true;
                State::system.forceMainScreenRedraw = true;
            }
        }
    }
    // -------------------------------------------------------
    
    // BATH_MODE_MANUAL 独自タイムアウト: 無操作5秒でサブメニューに戻る
    if (State::menu.currentMode == State::BATH_MODE_MANUAL
        && millis() - State::timers.lastActivityTime > 5000) {
        State::menu.menuSelection = 0;
        changeMode(State::BATH_MODE_SUBMENU);
    }

    // DISTANCE_MEASURE / BATH_MODE / BATH_MODE_MANUAL は全体タイムアウトから除外
    if (State::menu.currentMode != State::MAIN_DISPLAY
        && State::menu.currentMode != State::DISTANCE_MEASURE
        && State::menu.currentMode != State::BATH_MODE
        && State::menu.currentMode != State::BATH_MODE_MANUAL
        && (millis() - State::timers.lastActivityTime > 5000)) {
        changeMode(State::MAIN_DISPLAY);
    }
    
    if (State::menu.currentMode == State::MAIN_DISPLAY) {
        Display::updateMainDisplay();
    } else if (State::menu.currentMode == State::DISTANCE_MEASURE) {
        Sensors::handleDistanceMeasureScreen();
    } else if (State::menu.currentMode == State::BATH_MODE) {
        Sensors::handleBathModeScreen();
    } else if (State::system.needsRedraw) {
        State::lcd.clear();
        switch (State::menu.currentMode) {
            case State::MENU: Display::drawPagedMenu(Menu::mainMenu, Menu::NUM_MAIN_MENU_ITEMS, State::menu.menuSelection); break;
            case State::BATH_MODE_SUBMENU: Display::drawBathSubMenu(); break;
            case State::BATH_MODE_MANUAL: Display::drawBathManualScreen(); break;
            case State::HISTORY: Network::fetchAndDrawHistory(); break;
            case State::SWITCHBOT_DEVICE_SELECT: Display::drawPagedMenu(DEVICE_MENU_TEXT, NUM_DEVICE_MENU_ITEMS, State::menu.deviceSelection); break;
            case State::WOL_SELECT: Display::drawPagedMenu(WOL_MENU_TEXT, NUM_WOL_ITEMS, State::menu.menuSelection); break;
            case State::DEVICE_CONTROL: {
                switch(State::menu.deviceSelection) {
                    case 0: Display::drawPagedMenu(LIGHT_CMD_TEXT, NUM_LIGHT_CMDS, State::menu.commandSelection); break;
                    case 1: Display::drawPagedMenu(TV_CMD_TEXT, NUM_TV_CMDS, State::menu.commandSelection); break;
                    case 2: {
                        char items[5][17];
                        strcpy(items[0], "1.Turn ON"); strcpy(items[1], "2.Turn OFF");
                        sprintf(items[2], "3.Temp UP (%d)", State::ac_temperature);
                        sprintf(items[3], "4.Temp DOWN(%d)", State::ac_temperature);
                        strcpy(items[4], "5.Mode Cool");
                        const char* ptrs[5]; for(int i=0; i<5; i++) ptrs[i] = items[i];
                        Display::drawPagedMenu(ptrs, 5, State::menu.commandSelection);
                        break;
                    }
                    case 3: Display::drawPagedMenu(FAN_CMD_TEXT, NUM_FAN_CMDS, State::menu.commandSelection); break;
                    case 4: case 5: Display::drawPagedMenu(ON_OFF_CMD_TEXT, NUM_ON_OFF_CMDS, State::menu.commandSelection); break;
                }
                break;
            }
            case State::GET_DEVICE_LIST:
                Display::printLcdLine(0, "Getting IDs...");
                Display::printLcdLine(1, "See Serial Mon.");
                {
                    WiFiClientSecure client; client.setInsecure();
                    if (client.connect("api.switch-bot.com", 443)) {
                        client.println("GET /v1.1/devices HTTP/1.1");
                        client.println("Host: api.switch-bot.com");
                        client.printf("Authorization: %s\r\n", SWITCHBOT_TOKEN);
                        client.println("Connection: close\r\n");
                        while(client.connected()) { if(client.available()){ Serial.write(client.read()); } }
                    }
                }
                delay(4000);
                changeMode(State::MAIN_DISPLAY);
                break;
        }
        State::system.needsRedraw = false;
    }

    // お風呂通知中は LED を 1 秒間隔で点滅し続ける
    if (State::system.bathAlerted) {
        static unsigned long lastBathBlink = 0;
        static bool bathBlinkState = false;
        if (millis() - lastBathBlink >= 1000) {
            lastBathBlink = millis();
            bathBlinkState = !bathBlinkState;
            digitalWrite(Pins::LED_PIN, bathBlinkState ? HIGH : LOW);
        }
    } else if (State::menu.currentMode == State::DISTANCE_MEASURE) {
        // 距離測定中は LED 常時点灯
        digitalWrite(Pins::LED_PIN, HIGH);
    } else if (State::system.ledAlwaysOn && !State::system.isBlinking) {
        digitalWrite(Pins::LED_PIN, HIGH);
    } else if (!State::system.ledAlwaysOn && !State::system.isBlinking) {
        digitalWrite(Pins::LED_PIN, LOW);
    }
}

//================================================================
// Setup & Loop (コア1)
//================================================================
void setup1() {
    DEBUG_PRINTLN("[Core 1] Core 1 started.");
    Network::connectWiFi();
    
    if (State::system.wifiConnected) {
        Network::initOTA(); // ★★★ Wi-Fi接続後にOTAを初期化 ★★★

        // NTP同期
        NTP.begin(NTP_SERVER);
        setenv("TZ", "JST-9", 1);
        tzset();
        time_t now = time(nullptr);
        int sync_attempts = 0;
        while (now < 100000 && sync_attempts++ < 100) { delay(100); now = time(nullptr); }
        if (now > 100000) {
             State::system.ntpInitialized = true;
             DEBUG_PRINTLN("[Core 1] NTP sync successful.");
        } else {
             DEBUG_PRINTLN("[Core 1] NTP sync failed.");
        }
        // Webサーバー開始
        State::server.begin();
        DEBUG_PRINTLN("[Core 1] Web server started.");
    }
    DEBUG_PRINTLN("[Core 1] Setup complete.");
}

void loop1() { // コア1のメインループ
    if (State::system.wifiConnected) {
        ArduinoOTA.handle(); // ★★★ OTAの待機処理を常時実行 ★★★

        // NTPの再同期チェック
        if (!State::system.ntpInitialized) {
             time_t now = time(nullptr);
             if (now < 100000) {
                 NTP.begin(NTP_SERVER);
                 delay(1000);
             } else {
                 State::system.ntpInitialized = true;
             }
        }
        
        // 親機との通信 (10秒ごと)
        static unsigned long lastParentCommTime = 0;
        if (millis() - lastParentCommTime > 10000) {
            Network::handleParentCommunication();
            lastParentCommTime = millis();
        }

        // お風呂通知を親機に送信
        if (requestBathNotify) {
            requestBathNotify = false;
            WiFiClient notifyClient;
            if (notifyClient.connect(PARENT_IP, PARENT_PORT)) {
                notifyClient.println("GET /bath_alert HTTP/1.1");
                notifyClient.print("Host: ");
                notifyClient.println(PARENT_IP);
                notifyClient.println("Connection: close\r\n");
                delay(100);
                notifyClient.stop();
            }
        }

        // お風呂クリアを親機に送信
        if (requestBathClear) {
            requestBathClear = false;
            WiFiClient clearClient;
            if (clearClient.connect(PARENT_IP, PARENT_PORT)) {
                clearClient.println("GET /bath_clear HTTP/1.1");
                clearClient.print("Host: ");
                clearClient.println(PARENT_IP);
                clearClient.println("Connection: close\r\n");
                delay(100);
                clearClient.stop();
            }
        }

        // 距離測定終了を親機に通知
        if (requestNotifyDistanceStopped) {
            requestNotifyDistanceStopped = false;
            WiFiClient stopNotifyClient;
            if (stopNotifyClient.connect(PARENT_IP, PARENT_PORT)) {
                stopNotifyClient.println("GET /distance_stop HTTP/1.1");
                stopNotifyClient.print("Host: ");
                stopNotifyClient.println(PARENT_IP);
                stopNotifyClient.println("Connection: close\r\n");
                delay(100);
                stopNotifyClient.stop();
            }
        }

        // 距離値を親機にプッシュ (最短250msの送信間隔を強制)
        if (requestDistancePush) {
            static unsigned long lastPushTime = 0;
            if (millis() - lastPushTime >= 250) {
                int val10 = distancePushValue10;
                requestDistancePush = false;
                lastPushTime = millis();
                WiFiClient pushClient;
                pushClient.setTimeout(200);
                if (pushClient.connect(PARENT_IP, PARENT_PORT)) {
                    char path[48];
                    snprintf(path, sizeof(path), "GET /distance_update?v=%d HTTP/1.1", val10);
                    pushClient.println(path);
                    pushClient.print("Host: ");
                    pushClient.println(PARENT_IP);
                    pushClient.println("Connection: close\r\n");
                    delay(30);
                    pushClient.stop();
                }
            }
        }

        // 親機からのリクエスト待機
        Network::handleServerClient();  
    }
    delay(10); // 他のタスクにCPU時間を譲る
}

