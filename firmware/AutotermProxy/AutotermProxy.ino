/*
 * Proxy UART Autoterm - Arduino Mega 2560
 *
 * Serial  : USB vers PC / Node-RED (115200)
 * Serial1 : commande  (RX1 = 19, TX1 = 18)
 * Serial2 : chauffage (RX2 = 17, TX2 = 16)
 *
 * Relais transparent octet par octet entre commande et chauffage.
 * Les trames relayées ne sont jamais filtrées.
 *
 * Affichage USB, une trame par ligne :
 *   C: AA 03 00 00 0F 58 7C   [CRC OK]   trame reçue de la commande
 *   H: AA 04 ...              [CRC OK]   trame reçue du chauffage
 *   C: AA 03 ...              [CRC KO]   trame complète, CRC invalide
 *   C: AA 03 00               [BROKEN]   trame abandonnée après timeout
 *   PROXY READY                          au démarrage
 *
 * Format de trame :
 *   AA | device | payload_len | 00 | cmd_id | payload... | CRC16 (BE)
 *   CRC16 Modbus (poly 0xA001, init 0xFFFF), poids fort d'abord (big-endian),
 *   calculé sur tous les octets sauf les 2 derniers.
 *   Attention : ordre inverse du Modbus RTU standard.
 *
 * Injection depuis USB (toujours vers le chauffage), ligne terminée par LF :
 *   AA 03 00 00 03 5D 7C      arrêt, CRC inclus (big-endian)
 *
 * Réponses USB :
 *   INJ: ...        trame injectée
 *   ERR HEX         caractère non hexadécimal / nombre impair de chiffres
 *   ERR LENGTH      longueur incohérente avec payload_len
 *   ERR CRC         CRC invalide
 *   ERR BUSY        bus occupé ou buffer TX insuffisant : réessayer
 *   ERR OVERFLOW    ligne de commande trop longue
 */

#include <Arduino.h>

static const uint32_t AUTOTERM_BAUD = 9600;
static const uint32_t USB_BAUD      = 115200;

static const uint8_t  MIN_FRAME = 7;
static const uint16_t MAX_FRAME = 7 + 255;

static const uint32_t FRAME_TIMEOUT_MS = 20;
static const uint32_t BUS_QUIET_MS     = 5;   // silence requis avant injection

struct Assembler {
  uint8_t  buf[MAX_FRAME];
  uint16_t len;
  uint16_t expected;
  char     tag;
  uint32_t lastByteMs;
};

static Assembler asmC = {{0}, 0, 0, 'C', 0};
static Assembler asmH = {{0}, 0, 0, 'H', 0};

static const uint16_t CMD_BUF_SIZE = 3 * MAX_FRAME + 8;
static char     cmdBuf[CMD_BUF_SIZE];
static uint16_t cmdLen      = 0;
static bool     cmdOverflow = false;

// ---------- utilitaires ----------

static void printHex(uint8_t b)
{
  if (b < 16) Serial.print('0');
  Serial.print(b, HEX);
}

static void printBytes(const uint8_t *buf, uint16_t n)
{
  for (uint16_t i = 0; i < n; i++) {
    printHex(buf[i]);
    if (i + 1 < n) Serial.print(' ');
  }
}

static uint16_t crc16(const uint8_t *buf, uint16_t len)
{
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xA001;
      else         crc >>= 1;
    }
  }
  return crc;
}

// CRC attendu en big-endian : octet fort à n-2, octet faible à n-1
static bool crcOk(const uint8_t *buf, uint16_t n)
{
  if (n < MIN_FRAME) return false;
  uint16_t calc = crc16(buf, n - 2);
  uint16_t got  = ((uint16_t)buf[n - 2] << 8) | (uint16_t)buf[n - 1];
  return calc == got;
}



// ---------- assemblage des trames RX (affichage uniquement) ----------

static void resetAsm(Assembler &a, bool report)
{
  if (report && a.len > 0) {
    Serial.print(a.tag);
    Serial.print(": ");
    printBytes(a.buf, a.len);
    Serial.println("   [BROKEN]");
  }
  a.len = 0;
  a.expected = 0;
}

static void emitFrame(Assembler &a)
{
  Serial.print(a.tag);
  Serial.print(": ");
  printBytes(a.buf, a.len);
  Serial.println(crcOk(a.buf, a.len) ? "   [CRC OK]" : "   [CRC KO]");
  a.len = 0;
  a.expected = 0;
}

static void feed(Assembler &a, uint8_t b)
{
  a.lastByteMs = millis();

  if (a.len == 0) {
    if (b == 0xAA) a.buf[a.len++] = b;
    return;
  }

  a.buf[a.len++] = b;

  if (a.len == 3) a.expected = MIN_FRAME + a.buf[2];

  if (a.expected && a.len == a.expected) emitFrame(a);
}

static void checkTimeout(Assembler &a)
{
  if (a.len > 0 && (uint32_t)(millis() - a.lastByteMs) > FRAME_TIMEOUT_MS) {
    resetAsm(a, true);
  }
}

static void relay(HardwareSerial &src, HardwareSerial &dst, Assembler &a)
{
  while (src.available() > 0) {
    uint8_t b = (uint8_t)src.read();
    dst.write(b);
    feed(a, b);
  }
}

// ---------- injection depuis USB (toujours vers le chauffage) ----------

static int hexVal(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Bus considéré libre : aucune trame en cours et silence depuis BUS_QUIET_MS
static bool busIdle()
{
  if (asmC.len != 0 || asmH.len != 0) return false;
  uint32_t now = millis();
  return (uint32_t)(now - asmC.lastByteMs) >= BUS_QUIET_MS &&
         (uint32_t)(now - asmH.lastByteMs) >= BUS_QUIET_MS;
}

static void inject(char *line)
{
  uint8_t buf[MAX_FRAME];
  uint16_t n = 0;
  char *p = line;

  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    int hi = hexVal(*p++);
    int lo = (*p) ? hexVal(*p++) : -1;
    if (hi < 0 || lo < 0 || n >= sizeof(buf)) {
      Serial.println("ERR HEX");
      return;
    }
    buf[n++] = (uint8_t)((hi << 4) | lo);
  }

  if (n == 0) return;

  if (n < MIN_FRAME || n != (uint16_t)(MIN_FRAME + buf[2])) {
    Serial.println("ERR LENGTH");
    return;
  }
  if (!crcOk(buf, n)) {
    Serial.println("ERR CRC");
    return;
  }

  if (!busIdle() || Serial2.availableForWrite() < (int)n) {
    Serial.println("ERR BUSY");
    return;
  }

  Serial2.write(buf, n);

  Serial.print("INJ: ");
  printBytes(buf, n);
  Serial.println();
}

static void readUsb()
{
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (cmdOverflow) {
        Serial.println("ERR OVERFLOW");
      } else if (cmdLen > 0) {
        cmdBuf[cmdLen] = '\0';
        inject(cmdBuf);
      }
      cmdLen = 0;
      cmdOverflow = false;
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    } else {
      cmdOverflow = true;
    }
  }
}

// ---------- setup / loop ----------

void setup()
{
  Serial.begin(USB_BAUD);
  Serial1.begin(AUTOTERM_BAUD);
  Serial2.begin(AUTOTERM_BAUD);
  Serial.println("PROXY READY");
}

void loop()
{
  relay(Serial1, Serial2, asmC);
  relay(Serial2, Serial1, asmH);
  checkTimeout(asmC);
  checkTimeout(asmH);
  readUsb();
}
