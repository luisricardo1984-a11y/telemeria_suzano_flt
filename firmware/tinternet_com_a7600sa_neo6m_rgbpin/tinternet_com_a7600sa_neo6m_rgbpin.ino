/*
 * Rastreador FLT Suzano - LilyGO T-Internet-COM (ESP32-WROVER-E + Ethernet
 * LAN8720 + modem SIM7600SA/A7600SA via adaptador Mini PCIE) + GPS externo
 * NEO-6M numa UART totalmente dedicada, independente do modem.
 *
 * Por que GPS externo em vez do GNSS interno do A7600SA:
 *   O A7600SA so expoe uma UART pro ESP32. Ler o GNSS interno por AT command
 *   nessa mesma UART usada para dados/HTTP faz os dois disputarem o canal,
 *   represando amostras de posicao durante os uploads. Usando um NEO-6M
 *   numa UART separada, GPS e modem rodam em paralelo sem nenhuma disputa -
 *   1Hz de verdade, sem lacunas.
 *
 * Pino usado para o GPS: apenas IO12 (RX-only), que nesta placa so estava
 * ligado ao LED RGB (WS2812) onboard - o unico "extra" sacrificavel sem
 * mexer em SD/TF ou Ethernet, que continuam livres e funcionando.
 *
 * ATENCAO - GPIO12 e pino de strapping do ESP32 (MTDI, define a tensao da
 * flash no boot). Se ficar em nivel alto no instante do reset, o ESP32 pode
 * falhar para ligar. Linha serial ociosa fica em nivel alto por padrao, e
 * o NEO-6M pode estar ligado/energizado antes do ESP32 terminar o boot.
 * MITIGACAO OBRIGATORIA: solde um resistor de pull-down de 10k ohm entre
 * IO12 e GND. Isso mantem o pino em nivel baixo durante o boot sem
 * atrapalhar a comunicacao serial depois (a saida do NEO-6M facilmente
 * sobrepoe um pull-down de 10k).
 *
 * Bibliotecas (Arduino Library Manager):
 *   - TinyGSM       (vshymanskyy)
 *   - TinyGPSPlus   (mikalhart)
 *
 * Pinagem T-Internet-COM (conferida no pinout oficial do fabricante e no
 * exemplo Xinyuan-LilyGO/T-Internet-COM -> example/Arduino/ATdebug):
 *   Modem (A7600SA / SIM7600SA, via adaptador Mini PCIE):
 *       PWRKEY = GPIO32   TX (ESP32->modem) = GPIO33   RX (modem->ESP32) = GPIO35
 *   GPS NEO-6M (UART2, RX-only, era o pino do LED RGB onboard):
 *       ESP32 GPIO12 <- TX do NEO-6M  (com pull-down de 10k pra GND)
 *       NEO-6M: VCC->3V3, GND->GND
 *       (RX do NEO-6M fica sem ligacao - nao precisamos mandar comando pra ele)
 */

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>
#include <TinyGPS++.h>

// ---------- Modem A7600SA / SIM7600SA (adaptador Mini PCIE) ----------
#define MODEM_PWRKEY  32
#define MODEM_TX      33
#define MODEM_RX      35

// ---------- GPS NEO-6M (era o pino do LED RGB onboard) ----------
#define GPS_RX_PIN    12   // <- TX do NEO-6M (lembrar do pull-down de 10k)
#define GPS_BAUD      9600

// ---------- Rede celular ----------
const char apn[]  = "SEU_APN_AQUI";   // ex.: "zap.vivo.com.br", "claro.com.br"
const char user[] = "";
const char pass[] = "";

// ---------- ThingSpeak (mesmo canal do index.html) ----------
const char* server        = "api.thingspeak.com";
const String writeKeyGps  = "SUA_WRITE_API_KEY_DO_CANAL_GPS";
const unsigned long THINGSPEAK_MIN_INTERVAL_MS = 15000; // limite do plano free
const unsigned long UPLOAD_WAIT_MS             = 800;   // drena resposta sem travar

HardwareSerial SerialAT(1);
HardwareSerial SerialGPS(2);
TinyGsm modem(SerialAT);
TinyGsmClient gsmClient(modem);
TinyGPSPlus gps;

unsigned long lastGpsTick = 0;
unsigned long lastUpload  = 0;
double lastLat = 0.0, lastLon = 0.0;
bool hasFix = false;

void modemPowerOn() {
  // Sequencia oficial do fabricante (example/Arduino/ATdebug):
  // pulso HIGH de 300ms no PWRKEY liga o modulo.
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(300);
  digitalWrite(MODEM_PWRKEY, LOW);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // RX-only: nao precisamos mandar nada pro NEO-6M, so ouvir (-1 = sem pino de TX)
  SerialGPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, -1);

  modemPowerOn();
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  Serial.println("Inicializando modem A7600SA...");
  modem.restart();
  Serial.print("Conectando na rede/APN... ");
  Serial.println(modem.gprsConnect(apn, user, pass) ? "OK" : "falhou (tenta de novo no loop)");
}

void readGpsAt1Hz() {
  // Processa todo byte disponivel do NEO-6M assim que chega (nao bloqueante)
  while (SerialGPS.available() > 0) {
    if (gps.encode(SerialGPS.read())) {
      if (gps.location.isValid() && gps.location.isUpdated()) {
        lastLat = gps.location.lat();
        lastLon = gps.location.lng();
        hasFix  = true;
      }
    }
  }

  if (millis() - lastGpsTick >= 1000) {
    lastGpsTick = millis();
    if (hasFix) {
      Serial.printf("[GPS 1Hz] lat=%.6f lon=%.6f sats=%d hdop=%.1f vel=%.1fkm/h\n",
                     lastLat, lastLon, gps.satellites.value(),
                     gps.hdop.hdop(), gps.speed.kmph());
    } else {
      Serial.println("[GPS 1Hz] aguardando fix...");
    }
  }
}

void uploadToThingSpeak() {
  if (!hasFix) return;

  if (!modem.isGprsConnected()) {
    modem.gprsConnect(apn, user, pass);
    return;
  }

  if (!gsmClient.connect(server, 80)) {
    Serial.println("Falha ao conectar no ThingSpeak");
    return;
  }

  String url = "/update?api_key=" + writeKeyGps +
               "&field1=" + String(lastLat, 6) +
               "&field2=" + String(lastLon, 6);

  gsmClient.print(String("GET ") + url + " HTTP/1.1\r\n" +
                  "Host: " + server + "\r\n" +
                  "Connection: close\r\n\r\n");

  // GPS agora esta numa UART separada (SerialGPS), entao nao ha mais
  // disputa de canal aqui - mesmo assim mantemos a espera curta, so
  // para nao segurar o loop() sem necessidade.
  unsigned long t0 = millis();
  while (gsmClient.connected() && millis() - t0 < UPLOAD_WAIT_MS) {
    while (gsmClient.available()) gsmClient.read();
  }
  gsmClient.stop();

  Serial.printf("Enviado ao ThingSpeak: lat=%.6f lon=%.6f\n", lastLat, lastLon);
}

void loop() {
  readGpsAt1Hz();

  if (millis() - lastUpload >= THINGSPEAK_MIN_INTERVAL_MS) {
    lastUpload = millis();
    uploadToThingSpeak();
  }
}
