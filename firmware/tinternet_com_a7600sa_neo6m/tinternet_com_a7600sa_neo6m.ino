/*
 * Rastreador FLT Suzano - LilyGO T-Internet-COM (ESP32-WROVER-E + Ethernet
 * LAN8720 + modem SIM7600SA/A7600SA via adaptador Mini PCIE)
 * + GPS externo NEO-6M numa UART livre da placa.
 *
 * - Le o NMEA do NEO-6M continuamente (o modulo emite sentencas a 1Hz por
 *   padrao de fabrica), atualizando lat/lon/velocidade/satelites a cada 1s.
 * - Envia a posicao para o ThingSpeak (mesmo canal/campos que o dashboard
 *   index.html deste repo ja consome: field1=lat, field2=lon) usando o
 *   A7600SA como modem celular.
 *
 * IMPORTANTE sobre o intervalo de envio:
 *   O plano gratuito do ThingSpeak aceita no maximo 1 update a cada 15s por
 *   canal. Por isso a leitura/parse do GPS roda a 1Hz (uteis para log local,
 *   calculo de velocidade/rumo, filtragem de fix), mas o envio HTTP para o
 *   ThingSpeak respeita THINGSPEAK_MIN_INTERVAL_MS. Para historico real a
 *   1Hz, grave os pontos no cartao TF (ja presente nesta placa) e faca
 *   upload em lote depois, ou troque o backend por um que aceite 1Hz.
 *
 * Bibliotecas (Arduino Library Manager):
 *   - TinyGSM       (vshymanskyy)
 *   - TinyGPSPlus   (mikalhart)
 *
 * Pinagem T-Internet-COM (conferida no pinout oficial do fabricante e no
 * exemplo Xinyuan-LilyGO/T-Internet-COM -> example/Arduino/ATdebug):
 *   Modem (A7600SA / SIM7600SA, via adaptador Mini PCIE):
 *       PWRKEY = GPIO32   TX (ESP32->modem) = GPIO33   RX (modem->ESP32) = GPIO35
 *   GPS NEO-6M (UART2, pinos livres nesta placa):
 *       ESP32 GPIO16 <- TX do NEO-6M
 *       ESP32 GPIO17 -> RX do NEO-6M (opcional, NEO-6M raramente precisa)
 *       NEO-6M: VCC->3V3, GND->GND
 *   (GPIO16/17 nao sao usados por Ethernet (LAN8720/RMII), TF card, modem ou
 *   LED RGB nesta placa - conferido no pinout oficial "T-Internet-COM PINMAP")
 */

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>
#include <TinyGPS++.h>

// ---------- Modem A7600SA / SIM7600SA (adaptador Mini PCIE) ----------
#define MODEM_PWRKEY  32
#define MODEM_TX      33
#define MODEM_RX      35

// ---------- GPS NEO-6M ----------
#define GPS_RX_PIN    16   // <- TX do NEO-6M
#define GPS_TX_PIN    17   // -> RX do NEO-6M (opcional)
#define GPS_BAUD      9600

// ---------- Rede celular ----------
const char apn[]  = "SEU_APN_AQUI";   // ex.: "zap.vivo.com.br", "claro.com.br"
const char user[] = "";
const char pass[] = "";

// ---------- ThingSpeak (mesmo canal do index.html) ----------
const char* server        = "api.thingspeak.com";
const String writeKeyGps  = "SUA_WRITE_API_KEY_DO_CANAL_GPS";
const unsigned long THINGSPEAK_MIN_INTERVAL_MS = 15000; // limite do plano free

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

  SerialGPS.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  modemPowerOn();
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  Serial.println("Inicializando modem A7600SA...");
  modem.restart();
  Serial.print("Conectando na rede/APN... ");
  if (modem.gprsConnect(apn, user, pass)) {
    Serial.println("OK");
  } else {
    Serial.println("falhou (vai tentar de novo no loop)");
  }
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

  unsigned long t0 = millis();
  while (gsmClient.connected() && millis() - t0 < 5000) {
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
