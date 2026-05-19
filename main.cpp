
#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <Ethernet.h>
#include <HTTPClient.h> 
#include <ArduinoJson.h>
#include "user_define.h"
#include "LGFX_Config.h"
#include "power_monitor.h"

static LGFX_Config lcd;
static PowerMonitor pwr;

// =============================================================================
// ⚠️ 環境に合わせて正確に書き換えてください
// =============================================================================
const char* wifi_ssid     = "Insulora";     // あなたのスマホのテザリングSSID
const char* wifi_password = "foresp32"; // テザリングのパスワード
const char* supabase_host = "pppwabefjkebcilpukgo.supabase.co"; // URLの「https://」を除いた文字列

// https://pppwabefjkebcilpukgo.supabase.co
// https://pppwabefjkebcilpukgo.suffpabase.co/rest/v1/
const char* supabase_key  = "sb_publishable_orkeACes282SWr-i9lnaTQ_rh57ciVq";       // 確保した最新のKey
//                           sb_publishable_-SnsZH-cXmpfGiZ_iGZrfQ_KxsmkSkY
// =============================================================================

//uint8_t mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
uint8_t mac[6] = {0}; 
IPAddress local_IP(192, 168, 1, 10);
IPAddress dns_server(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

bool remote_led_req = false;

void updateDisplay(float volts, float noise, bool pressed, const char* wifi_stat, const char* sd_stat, const char* eth_stat) {
    lcd.setTextSize(2);
    lcd.setCursor(20, 95); lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.printf("VOUT: %.3f V  ", volts);
    lcd.setCursor(20, 120); lcd.setTextColor(noise > 50.0f ? TFT_RED : TFT_GREEN, TFT_BLACK); lcd.printf("NOISE: %.1f mV  ", noise);
    lcd.setCursor(20, 145); lcd.setTextColor(TFT_CYAN, TFT_BLACK); lcd.printf("WiFi: %s      ", wifi_stat);
    lcd.setCursor(20, 170); lcd.setTextColor(TFT_YELLOW, TFT_BLACK); lcd.printf("SD  : %s      ", sd_stat);
    lcd.setCursor(20, 195); lcd.setTextColor(strcmp(eth_stat, "LINK UP") == 0 ? TFT_MAGENTA : TFT_DARKGRAY, TFT_BLACK); lcd.printf("W5500: %s      ", eth_stat);
}

void syncCloudData(float volts, float noise, bool pressed) {
    // 💡 追記: Wi-Fi状態のチェックログ
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[CLOUD] Cannot sync: Wi-Fi Disconnected!");
        return;
    }

    Serial.println("[CLOUD] Attempting to connect to Supabase REST API...");
    HTTPClient http;
    
    // 💡 解決①：URLの末尾に「&on_conflict=id」を追加して、重複時は上書きするよう明示します
    String serverPath = "https://" + String(supabase_host) + "/rest/v1/device_status?id=eq.esp32_01&select=led_cmd&on_conflict=id";
    http.begin(serverPath);


    // ヘッダーの設定
    // 50行目付近（syncCloudData内）：「return=representation」と「resolution=merge-duplicates」をカンマで1行に合体
    http.addHeader("apikey", supabase_key);
    http.addHeader("Authorization", "Bearer " + String(supabase_key));
    http.addHeader("Content-Type", "application/json");

    // 💡 これでSupabaseが204ではなく「200 OK（最新データ付き）」を返してくれます
    http.addHeader("Prefer", "return=representation, resolution=merge-duplicates"); 

    // 💡 解決②：データを新しくねじ込むために、現在のデバイスID「esp32_01」をJSONに含めます
    JsonDocument sendDoc;
    sendDoc["id"] = "esp32_01"; 
    sendDoc["volts"] = volts;
    sendDoc["noise"] = noise;
    sendDoc["pressed"] = pressed;

    String jsonStr;
    serializeJson(sendDoc, jsonStr);

    // 💡 解決③：PATCHではなく「POST」メソッドで送信を実行します
    int httpResponseCode = http.POST(jsonStr); 
    Serial.printf("[CLOUD] POST Sent. Response Code: %d\n", httpResponseCode);
    
    if (httpResponseCode >= 200 && httpResponseCode < 300) {
        String response = http.getString();
        JsonDocument recvDoc;
        DeserializationError error = deserializeJson(recvDoc, response);
        
        if (!error && recvDoc.is<JsonArray>()) {
            JsonArray arr = recvDoc.as<JsonArray>();
            if (arr.size() > 0) {
                remote_led_req = arr[0]["led_cmd"].as<bool>(); // 💡 修正: 配列の0番目の要素にアクセス
                digitalWrite(DEBUG_LED_PIN, remote_led_req ? HIGH : LOW);
                Serial.printf("[CLOUD] LED Command Parsed successfully -> %s\n", remote_led_req ? "ON" : "OFF");
            } else {
                Serial.println("[CLOUD] Warning: Response array is empty. Check if 'esp32_01' exists in DB.");
            }
        } else {
            Serial.printf("[CLOUD] JSON Parse Error: %s\n", error.c_str());
        }
    } else {
        String errorResponse = http.getString();
        Serial.printf("[CLOUD] HTTP Failed. Raw Body: %s\n", errorResponse.c_str());
    }
    http.end(); 
}

void setup() {
    Serial.begin(115200);
    delay(500); // 起動直後のシリアル安定用
    
    pinMode(DEBUG_LED_PIN, OUTPUT);
    digitalWrite(DEBUG_LED_PIN, LOW);

    lcd.init(); lcd.setRotation(1); lcd.fillScreen(TFT_BLACK);
    lcd.drawRect(5, 5, lcd.width() - 10, lcd.height() - 10, TFT_GREEN);
    lcd.fillRect(15, 85, lcd.width() - 30, 130, TFT_BLACK);
    lcd.drawRect(15, 85, lcd.width() - 30, 130, TFT_DARKGRAY);
    
    lcd.setTextColor(TFT_WHITE); lcd.setTextSize(2);
    lcd.setCursor(10, 20); lcd.printf("CLOUD LIVE DEMO");
    lcd.setCursor(10, 45); lcd.printf("Supabase DB Link");

    pwr.begin();

    // 1. Wi-Fi接続
    WiFi.begin(wifi_ssid, wifi_password);
    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\nWiFi Connected!");

    // 2. SDカード初期化
    if (SD.begin(SD_CS_N_PIN, SPI, 16000000)) {
        File file = SD.open("/ultimate_test.csv", FILE_WRITE);
        if (file) { file.println("Time,Voltage,Noise_mV"); file.close(); }
    }

    // 3. W5500有線LAN初期化
    pinMode(W5500_RST_N_PIN, OUTPUT);
    digitalWrite(W5500_RST_N_PIN, LOW); delay(20);
    digitalWrite(W5500_RST_N_PIN, HIGH); delay(50);
    Ethernet.init(W5500_CS_N_PIN);
    SPI.begin(W5500_SCK_PIN, W5500_MISO_PIN, W5500_MOSI_PIN, W5500_CS_N_PIN);
    esp_read_mac(mac, ESP_MAC_WIFI_STA); 
    Serial.printf("[SYSTEM] Internal MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    Ethernet.begin(mac, local_IP, dns_server, gateway, subnet);
}

void loop() {
    static uint32_t last_send_time = 0;
    uint32_t current_time = millis();
    
    const char* wifi_status = (WiFi.status() == WL_CONNECTED) ? "CONNECTED" : "DISCONNECT";
    const char* sd_status = "IDLE";
    const char* eth_status = Ethernet.hardwareStatus() != EthernetNoHardware ? "LINK UP" : "LINK DOWN";

    // 1. センサーデータのサンプリング
    PowerMonitor::VoltageQuality data = pwr.measureQuality(SW2_N_PIN, "CLOUD_LIVE");
    float real_volt = data.avg_volt * 2.0f;
    float real_noise = data.noise_mv * 2.0f;

    // 2. 1秒おきのクラウド同期（loop内の delay の影響を排除するため不等号で管理）
    if (current_time - last_send_time >= 1000) {
        syncCloudData(real_volt, real_noise, data.is_sw_pressed);
        last_send_time = current_time;
    }

    // 3. SDカードログ保存
    File file = SD.open("/ultimate_test.csv", FILE_APPEND);
    if (file) {
        file.printf("%lu,%.3f,%.1f\n", current_time, real_volt, real_noise);
        file.flush(); file.close();
        sd_status = "WRITING...";
    }

    // 4. 画面更新
    updateDisplay(real_volt, real_noise, data.is_sw_pressed, wifi_status, sd_status, eth_status);

    // 5. LEDインジケーター（不要なディレイによるループ遅延を最小化）
    if (!remote_led_req) {
        digitalWrite(DEBUG_LED_PIN, HIGH); delay(10);
        digitalWrite(DEBUG_LED_PIN, LOW);  delay(10);
    } else {
        digitalWrite(DEBUG_LED_PIN, HIGH);
        delay(20);
    }
}
