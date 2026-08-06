/*
 * Rastreador FLT Suzano - LilyGO T-Internet-COM (ESP32-WROVER-E + Ethernet
 * LAN8720 + modem SIM7600SA/A7600SA via adaptador Mini PCIE)
 *
 * Usa o GNSS INTEGRADO do A7600SA (nao um GPS externo) para nao ocupar
 * nenhum GPIO extra - assim SD/TF, Ethernet e o LED RGB continuam livres
 * e utilizaveis ao mesmo tempo. A familia SIM7600/A76xx traz um receptor
 * GNSS embutido no proprio modulo, com um conector de antena GNSS dedicado
 * (u.FL "GNSS", separado do "MAIN"/"AUX" da antena LTE) na placa mini PCIe.
 * A posicao e lida via comando AT (+CGPS / getGPS do TinyGSM) pela MESMA
 * UART ja usada para a rede celular - por isso nao precisa de fiacao nova.
 *
 * REQUISITO DE HARDWARE: ligue uma antena GNSS ativa (3.3-5V) no conector
 * u.FL "GNSS" do cartao A7600SA (fica ao lado dos conectores MAIN/AUX da
 * antena LTE, na propria placa mini PCIe - nao no adaptador T-Internet-COM).
 * Sem essa antena o AT+CGPS ate liga, mas nunca obtem fix.
 *
 * - O modem faz a leitura interna do GNSS continuamente; o firmware faz
 *   polling via AT a 1Hz (getGPS), que e o ritmo natural de atualizacao
 *   do receptor.
 * - Envia a posicao para o ThingSpeak (mesmo canal/campos que o dashboard
 *   index.html deste repo ja consome: field1=lat, field2=lon).
 *
 * IMPORTANTE sobre o intervalo de envio:
 *   O plano gratuito do ThingSpeak aceita no maximo 1 update a cada 15s por
 *   canal. Por isso o polling do GNSS roda a 1Hz (util para log local,
 *   calculo de velocidade, etc.), mas o envio HTTP respeita
 *   THINGSPEAK_MIN_INTERVAL_MS.
 *
 * TRADE-OFF conhecido (GNSS e dados no mesmo UART):
 *   Como o A7600SA so expoe uma UART pro ESP32, os comandos AT do GNSS e o
 *   trafego de dados do upload disputam o mesmo canal serial, sequencialmente
 *   (nunca ao mesmo tempo - o loop() e single-threaded). Isso nao trava o
 *   firmware, mas durante a janela de upload (a cada 15s, ~UPLOAD_WAIT_MS)
 *   o polling do GNSS fica represado, perdendo 1 ou 2 amostras naquele
 *   ciclo. Na pratica isso ainda deixa >90% das leituras saindo a 1Hz, o
 *   que costuma ser suficiente para rastreamento veicular/ferroviario. Se
 *   precisar de 1Hz sem NENHUMA lacuna, a alternativa e um GPS externo em
 *   UART propria - mas nesta placa isso exige abrir mao do slot de SD (os
 *   unicos pinos soldaveis livres, IO2/IO15, sao os do header do TF) ou
 *   solda de precisao direto nos pads GPIO16/17 do modulo ESP32-WROVER-E.
 *
 * Biblioteca (Arduino Library Manager):
 *   - TinyGSM (vshymanskyy) - inclui suporte a GNSS do SIM7600 (enableGPS/getGPS)
 *
 * Pinagem do modem A7600SA/SIM7600SA no T-Internet-COM (conferida no
 * pinout oficial e no exemplo Xinyuan-LilyGO/T-Internet-COM ->
 * example/Arduino/ATdebug):
 *   PWRKEY = GPIO32   TX (ESP32->modem) = GPIO33   RX (modem->ESP32) = GPIO35
 */

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>

// ---------- Modem A7600SA / SIM7600SA (adaptador Mini PCIE) ----------
#define MODEM_PWRKEY  32
#define MODEM_TX      33
#define MODEM_RX      35

// ---------- Rede celular ----------
const char apn[]  = "SEU_APN_AQUI";   // ex.: "zap.vivo.com.br", "claro.com.br"
const char user[] = "";
const char pass[] = "";

// ---------- ThingSpeak (mesmo canal do index.html) ----------
const char* server        = "api.thingspeak.com";
const String writeKeyGps  = "SUA_WRITE_API_KEY_DO_CANAL_GPS";
const unsigned long THINGSPEAK_MIN_INTERVAL_MS = 15000; // limite do plano free
const unsigned long GNSS_POLL_INTERVAL_MS      = 1000;  // leitura local a 1Hz
const unsigned long UPLOAD_WAIT_MS             = 800;   // drena resposta sem travar a UART

HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
TinyGsmClient gsmClient(modem);

unsigned long lastGnssPoll = 0;
unsigned long lastUpload   = 0;
float lastLat = 0, lastLon = 0, lastSpeed = 0, lastAlt = 0, lastAccuracy = 0;
int   lastVsat = 0, lastUsat = 0;
bool  hasFix = false;

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

  modemPowerOn();
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  Serial.println("Inicializando modem A7600SA...");
  modem.restart();

  Serial.print("Ligando GNSS interno... ");
  Serial.println(modem.enableGPS() ? "OK" : "falhou");

  Serial.print("Conectando na rede/APN... ");
  Serial.println(modem.gprsConnect(apn, user, pass) ? "OK" : "falhou (tenta de novo no loop)");
}

void pollGnssAt1Hz() {
  if (millis() - lastGnssPoll < GNSS_POLL_INTERVAL_MS) return;
  lastGnssPoll = millis();

  hasFix = modem.getGPS(&lastLat, &lastLon, &lastSpeed, &lastAlt,
                         &lastVsat, &lastUsat, &lastAccuracy);

  if (hasFix) {
    Serial.printf("[GNSS 1Hz] lat=%.6f lon=%.6f vel=%.1fkm/h sats=%d/%d acc=%.1fm\n",
                   lastLat, lastLon, lastSpeed, lastUsat, lastVsat, lastAccuracy);
  } else {
    Serial.println("[GNSS 1Hz] aguardando fix (confira a antena GNSS)...");
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

  // Nao esperamos a resposta completa do ThingSpeak: o envio ja foi feito
  // assim que os bytes saem da UART, e ficar bloqueado aqui e o que mais
  // rouba tempo da UART do canal de AT commands, atrasando o polling do
  // GNSS que roda no mesmo link serial. UPLOAD_WAIT_MS curto so drena o
  // pouco que ja chegou, sem travar o loop esperando o servidor responder.
  unsigned long t0 = millis();
  while (gsmClient.connected() && millis() - t0 < UPLOAD_WAIT_MS) {
    while (gsmClient.available()) gsmClient.read();
  }
  gsmClient.stop();

  Serial.printf("Enviado ao ThingSpeak: lat=%.6f lon=%.6f\n", lastLat, lastLon);
}

void loop() {
  pollGnssAt1Hz();

  if (millis() - lastUpload >= THINGSPEAK_MIN_INTERVAL_MS) {
    lastUpload = millis();
    uploadToThingSpeak();
  }
}
