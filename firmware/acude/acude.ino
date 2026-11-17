// =====================================================================
// Monitoramento de Temperatura do Açude - Trabalho Final de IoT
// Centro Universitário FAG - 2026
//
// O ESP32 lê a temperatura da água, decide localmente quando ligar o
// aerador (regra com histerese), mostra o estado no display e no LED
// e envia os dados por Wi-Fi + MQTT (com TLS) para o painel web.
// =====================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>   // conexão criptografada (TLS)
#include <PubSubClient.h>       // cliente MQTT
#include <ArduinoJson.h>        // monta e lê mensagens JSON
#include <OneWire.h>            // barramento 1-Wire do sensor
#include <DallasTemperature.h>  // leitura do DS18B20
#include <Wire.h>               // I2C do display
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "secrets.h"            // Wi-Fi e token (NÃO vai para o GitHub)

// ---------- Pinos ----------
#define PINO_SENSOR 4    // DATA do DS18B20 (com resistor de 4,7 kΩ para 3V3)
#define PINO_RELE   26   // IN1 do relé que liga a ventoinha (aerador)
#define PINO_BUZZER 27   // I/O do buzzer
#define LED_R 25         // LED RGB catodo comum
#define LED_G 33
#define LED_B 32

// Nível que aciona cada peça (descoberto nos testes do hardware)
#define RELE_LIGA      HIGH   // relé com o jumper em H
#define RELE_DESLIGA   LOW
#define BUZZER_LIGA    LOW    // este módulo de buzzer apita em LOW
#define BUZZER_DESLIGA HIGH

// ---------- MQTT ----------
const char* MQTT_BROKER  = "broker.hivemq.com";
const int   MQTT_PORTA   = 8883;                 // MQTT sobre TLS
const char* TOPICO_DADOS = "acude/t02n/dados";   // ESP32 publica aqui
const char* TOPICO_CMD   = "acude/t02n/cmd";     // ESP32 recebe comandos aqui

// ---------- Regra local ----------
float limiteQuente  = 30.0;    // a partir daqui liga o aerador
float limiteRetorno = 28.0;    // só desliga quando cair até aqui (histerese)
const float LIMITE_FRIO = 20.0; // abaixo disso: alerta de água fria

// ---------- Objetos ----------
OneWire oneWire(PINO_SENSOR);
DallasTemperature sensor(&oneWire);
Adafruit_SSD1306 tela(128, 64, &Wire, -1);
WiFiClientSecure wifiClient;
PubSubClient mqtt(wifiClient);

// ---------- Estado do sistema ----------
bool aeradorLigado = false;
bool modoManual = false;        // false = automático (regra local decide)
bool comandoAerador = false;    // usado só no modo manual
String estado = "INICIANDO";
float tempAtual = 0;

// Controle de tempo sem delay(): cada tarefa roda no seu intervalo
unsigned long ultimaLeitura = 0;
unsigned long ultimoEnvio = 0;
unsigned long ultimaTentativa = 0;

// Acende o LED RGB na cor pedida (catodo comum: HIGH acende)
void corLED(bool r, bool g, bool b) {
  digitalWrite(LED_R, r);
  digitalWrite(LED_G, g);
  digitalWrite(LED_B, b);
}

// Mostra estado, temperatura, modo e conexões no display OLED (R5)
void mostrarNaTela() {
  tela.clearDisplay();
  tela.setTextSize(1);
  tela.setCursor(0, 0);
  tela.println(estado);                       // faixa amarela do display

  tela.setTextSize(3);
  tela.setCursor(0, 20);
  if (tempAtual == DEVICE_DISCONNECTED_C) tela.println("ERRO");
  else { tela.print(tempAtual, 1); tela.println("C"); }

  tela.setTextSize(1);
  tela.setCursor(0, 48);
  tela.print(modoManual ? "MANUAL " : "AUTO ");
  tela.println(aeradorLigado ? "Aer:ON" : "Aer:OFF");
  tela.setCursor(0, 56);
  tela.print("WiFi:");
  tela.print(WiFi.status() == WL_CONNECTED ? "OK " : "-- ");
  tela.print("MQTT:");
  tela.println(mqtt.connected() ? "OK" : "--");
  tela.display();
}

// Chamada quando chega um comando no tópico TOPICO_CMD.
// Valida tudo antes de aplicar: tamanho, JSON, token e valores.
void aoReceber(char* topico, byte* payload, unsigned int tamanho) {
  if (tamanho > 200) {                         // evita mensagem gigante
    Serial.println("CMD rejeitado: muito grande");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload, tamanho)) {
    Serial.println("CMD rejeitado: JSON invalido");
    return;
  }

  // Autenticação: só aceita comando com o token definido no secrets.h
  if (!doc["token"].is<const char*>() || String(doc["token"].as<const char*>()) != CMD_TOKEN) {
    Serial.println("CMD rejeitado: token invalido");
    return;
  }

  // {"modo":"manual"} ou {"modo":"auto"}
  if (doc["modo"].is<const char*>()) {
    String m = doc["modo"].as<String>();
    if (m == "manual") modoManual = true;
    else if (m == "auto") modoManual = false;
    else { Serial.println("CMD rejeitado: modo invalido"); return; }
  }

  // {"aerador":true/false} - só tem efeito no modo manual
  if (doc["aerador"].is<bool>()) {
    comandoAerador = doc["aerador"];
  }

  // {"limite":31} - aceita só uma faixa segura para peixes tropicais
  if (doc["limite"].is<float>()) {
    float l = doc["limite"];
    if (l < 22 || l > 35) {
      Serial.println("CMD rejeitado: limite fora de 22-35");
      return;
    }
    limiteQuente = l;
    limiteRetorno = l - 2;
  }
  Serial.println("CMD aceito");
}

// Reconecta Wi-Fi e MQTT a cada 5 s, sem travar o resto do sistema
void manterConexoes() {
  if (millis() - ultimaTentativa < 5000) return;
  ultimaTentativa = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi caiu, tentando reconectar...");
    WiFi.reconnect();
    return;
  }
  if (!mqtt.connected()) {
    String id = "acude-" + String((uint32_t)ESP.getEfuseMac(), HEX);  // ID único da placa
    if (mqtt.connect(id.c_str())) {
      mqtt.subscribe(TOPICO_CMD);
      Serial.println("MQTT conectado (TLS)");
    } else {
      Serial.println("MQTT falhou, tenta de novo em 5 s");
    }
  }
}

// Monta o payload JSON e publica no broker (R4)
void enviarDados() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  doc["id"] = "acude1";
  doc["temp"] = round(tempAtual * 10) / 10.0;   // uma casa decimal
  doc["estado"] = estado;
  doc["aerador"] = aeradorLigado;
  doc["modo"] = modoManual ? "manual" : "auto";
  doc["limite"] = limiteQuente;
  doc["ts"] = millis() / 1000;                  // segundos desde que ligou
  char buffer[200];
  serializeJson(doc, buffer);
  mqtt.publish(TOPICO_DADOS, buffer);
  Serial.print("Enviado: ");
  Serial.println(buffer);
}

// Regra local (R1 e R2): lê o sensor e aciona relé, buzzer e LED.
// Não depende de internet.
void lerEDecidir() {
  sensor.requestTemperatures();
  tempAtual = sensor.getTempCByIndex(0);

  // Sensor desconectado: estado seguro, desliga tudo
  if (tempAtual == DEVICE_DISCONNECTED_C) {
    estado = "FALHA NO SENSOR";
    aeradorLigado = false;
    digitalWrite(PINO_RELE, RELE_DESLIGA);
    digitalWrite(PINO_BUZZER, BUZZER_DESLIGA);
    corLED(1, 0, 1);                           // roxo
    return;
  }

  if (modoManual) {
    aeradorLigado = comandoAerador;            // quem manda é o painel
  } else {
    // Histerese: liga em 30 °C, só desliga em 28 °C
    if (tempAtual >= limiteQuente)       aeradorLigado = true;
    else if (tempAtual <= limiteRetorno) aeradorLigado = false;
  }

  bool alarme = false;
  if (tempAtual >= limiteQuente)      { estado = "ALERTA: AGUA QUENTE"; corLED(1,0,0); alarme = true; }  // vermelho
  else if (tempAtual <= LIMITE_FRIO)  { estado = "ALERTA: AGUA FRIA";   corLED(0,0,1); alarme = true; }  // azul
  else if (aeradorLigado)             { estado = "RESFRIANDO";          corLED(1,1,0); }                 // amarelo
  else                                { estado = "NORMAL";              corLED(0,1,0); }                 // verde

  digitalWrite(PINO_RELE, aeradorLigado ? RELE_LIGA : RELE_DESLIGA);
  digitalWrite(PINO_BUZZER, alarme ? BUZZER_LIGA : BUZZER_DESLIGA);
}

void setup() {
  Serial.begin(115200);
  pinMode(PINO_RELE, OUTPUT);
  pinMode(PINO_BUZZER, OUTPUT);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  digitalWrite(PINO_RELE, RELE_DESLIGA);       // começa tudo desligado (estado seguro)
  digitalWrite(PINO_BUZZER, BUZZER_DESLIGA);

  sensor.begin();
  if (!tela.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("ERRO: display");
  tela.setTextColor(SSD1306_WHITE);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);           // não espera conectar: o sistema já funciona sem rede
  wifiClient.setInsecure();                    // criptografa, mas não valida o certificado (limitação documentada)
  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  mqtt.setCallback(aoReceber);
}

void loop() {
  manterConexoes();
  mqtt.loop();                                 // processa comandos recebidos

  if (millis() - ultimaLeitura >= 1000) {      // lê e decide a cada 1 s
    ultimaLeitura = millis();
    lerEDecidir();
    mostrarNaTela();
  }
  if (millis() - ultimoEnvio >= 10000) {       // envia a cada 10 s
    ultimoEnvio = millis();
    enviarDados();
  }
}
