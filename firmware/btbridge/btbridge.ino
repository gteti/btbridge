// btbridge: tastiera+trackpad BLE (ProtoArc XK01 TP) -> USB HID: tastiera boot + mouse boot + consumer.
// Client HID BLE in report protocol (il trackpad non esiste in boot protocol): i report arrivano
// senza ID e si distinguono dalla lunghezza (8 tastiera, 6-7 mouse, 2 consumer).
// Il mouse (int16) diventa un report boot da 4 byte (int8), compatibile con BIOS e Linux vecchi.
#include <bluefruit.h>
#include <Adafruit_TinyUSB.h>

// Descrittori boot (docs/design-handover.md, Appendice A).
static const uint8_t kbd_desc[] = {
  0x05,0x01,0x09,0x06,0xA1,0x01,0x05,0x07,0x19,0xE0,0x29,0xE7,0x15,0x00,0x25,0x01,
  0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x03,0x95,0x05,0x75,0x01,
  0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x03,0x95,0x06,
  0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,0x29,0x65,0x81,0x00,0xC0
};
static const uint8_t mouse_desc[] = {
  0x05,0x01,0x09,0x02,0xA1,0x01,0x09,0x01,0xA1,0x00,0x05,0x09,0x19,0x01,0x29,0x03,
  0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x05,0x81,0x03,
  0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x03,
  0x81,0x06,0xC0,0xC0
};

// Terza interfaccia: tasti multimediali (Fn+F1..F12: volume, luminosita', ecc.), 16 bit, non boot.
static const uint8_t cons_desc[] = {
  0x05,0x0C,0x09,0x01,0xA1,0x01,0x15,0x00,0x26,0xFF,0x03,0x19,0x00,0x2A,0xFF,0x03,
  0x75,0x10,0x95,0x01,0x81,0x00,0xC0
};
Adafruit_USBD_HID usbKbd, usbMouse, usbCons;
// La tastiera non ha boot mouse: si usa il report protocol (Report Map letta con la probe):
// ID1 tastiera 8 byte, ID3 consumer 2 byte, ID2 mouse 7 byte = tasti(1) X(int16) Y(int16) rotella pan.
// I report arrivano senza ID: si distinguono dalla lunghezza.
BLEClientService hidSvc(UUID16_SVC_HUMAN_INTERFACE_DEVICE);
enum { NREP = 6 };
BLEClientCharacteristic rep[NREP] = {
  BLEClientCharacteristic(UUID16_CHR_REPORT), BLEClientCharacteristic(UUID16_CHR_REPORT),
  BLEClientCharacteristic(UUID16_CHR_REPORT), BLEClientCharacteristic(UUID16_CHR_REPORT),
  BLEClientCharacteristic(UUID16_CHR_REPORT), BLEClientCharacteristic(UUID16_CHR_REPORT)};

// Coda dai callback BLE al loop() (i callback girano nel task BLE).
struct Kbd { uint8_t b[8]; };
struct Mse { uint8_t b[4]; };
static Kbd kq[16]; static volatile uint8_t kh = 0, kt = 0;
static Mse mq[64]; static volatile uint8_t mh = 0, mt = 0;

static uint16_t cq[16]; static volatile uint8_t ch = 0, ct = 0;
static volatile uint32_t rxK = 0, rxM = 0;  // debug: report BLE ricevuti
static inline int8_t clamp8(int v) { return v > 127 ? 127 : v < -127 ? -127 : v; }

static void pushMouse(uint8_t btn, int x, int y, int wheel) {
  do {  // delta int16 -> piu' report int8; il resto si accumula sul successivo
    uint8_t n = (mh + 1) & 63; if (n == mt) return;
    int8_t sx = clamp8(x), sy = clamp8(y);
    mq[mh].b[0] = btn; mq[mh].b[1] = (uint8_t)sx; mq[mh].b[2] = (uint8_t)sy; mq[mh].b[3] = (uint8_t)clamp8(wheel);
    mh = n; x -= sx; y -= sy; wheel = 0;
  } while (x || y);
}

static void onReport(BLEClientCharacteristic*, uint8_t* d, uint16_t len) {
  if (len == 8) {
    rxK++;
    uint8_t n = (kh + 1) & 15; if (n == kt) return;
    memcpy(kq[kh].b, d, 8); kh = n;
  } else if (len >= 6 && len <= 7) {  // mouse; len 6 = senza pan
    rxM++;
    pushMouse(d[0] & 7, (int16_t)(d[1] | d[2] << 8), (int16_t)(d[3] | d[4] << 8), (int8_t)d[5]);
  } else if (len == 2) {  // consumer (media)
    uint8_t n = (ch + 1) & 15; if (n == ct) return;
    cq[ch] = d[0] | d[1] << 8; ch = n;
  }
}

// LED RGB del XIAO (attivi bassi). Colore = bit0 rosso, bit1 verde, bit2 blu.
enum { RED = 1, GREEN = 2, YELLOW = 3, BLUE = 4, WHITE = 7, CYAN = 6 };
static void rgb(uint8_t c) {
  digitalWrite(LED_RED, (c & 1) ? LOW : HIGH);
  digitalWrite(LED_GREEN, (c & 2) ? LOW : HIGH);
  digitalWrite(LED_BLUE, (c & 4) ? LOW : HIGH);
}
static void flash(uint8_t c, int on_ms = 500, int off_ms = 1500) { rgb(c); delay(on_ms); rgb(0); delay(off_ms); }
static void blink(int n) { for (int i = 0; i < n; i++) flash(GREEN, 200, 300); }

// Stato mostrato dal LED (n lampi ogni 3 s): 1 scansione senza adv, 2 (non usato),
// 3 adv HID visto/connessione, 4 connesso, 5 sicuro e notifiche attive.
static volatile uint8_t st = 1, why = 0;  // why: ultima causa di fallimento (debug LED)
static void scanCb(ble_gap_evt_adv_report_t* r) { st = 3; Bluefruit.Central.connect(r); }

static volatile uint32_t t4 = 0;
static void connectCb(uint16_t h) {
  st = 4; t4 = millis();
  Bluefruit.Connection(h)->requestPairing();
}

static void securedCb(uint16_t h) {
  BLEConnection* c = Bluefruit.Connection(h);
  if (!c->secured()) { c->removeBondKey(); why = 7; c->disconnect(); return; }
  if (!hidSvc.discover(h)) { why = 1; c->disconnect(); return; }
  BLEClientCharacteristic* chr[NREP];
  for (int i = 0; i < NREP; i++) chr[i] = &rep[i];
  Bluefruit.Discovery.discoverCharacteristic(h, chr, NREP);
  uint8_t n = 0;
  for (int i = 0; i < NREP; i++) {
    if (!rep[i].discovered() || !(rep[i].properties() & 0x10)) continue;  // solo con notify
    rep[i].setNotifyCallback(onReport);
    if (rep[i].enableNotify()) n++;
  }
  if (!n) { why = 1; c->disconnect(); return; }
  st = 5; why = 0;
  blink(n);  // n = report con notifiche attive (attesi 3: tastiera, consumer, mouse)
}

static void releaseAll() {  // al disconnect: niente tasti/pulsanti bloccati
  uint8_t z8[8] = {0}, z4[4] = {0}; uint16_t z2 = 0;
  if (usbCons.ready()) usbCons.sendReport(0, &z2, 2);
  if (usbKbd.ready()) usbKbd.sendReport(0, z8, 8);
  if (usbMouse.ready()) usbMouse.sendReport(0, z4, 4);
}
static void disconnectCb(uint16_t, uint8_t reason) { if (!why) why = reason == 0x13 ? 2 : reason == 0x16 ? 3 : reason == 0x08 ? 4 : reason == 0x3E ? 5 : 6; st = 1; kt = kh; mt = mh; ct = ch; releaseAll(); }

void setup() {
  pinMode(LED_RED, OUTPUT); pinMode(LED_GREEN, OUTPUT); pinMode(LED_BLUE, OUTPUT); rgb(0);
  // USB per primo e SENZA ritardi: le interfacce HID vanno aggiunte prima che l'host
  // enumeri (altrimenti Windows vede solo la seriale). Bluefruit.begin() prima del mount
  // uccide l'enumerazione.
  Serial.begin(115200);
#if !defined(MOUSE_ONLY) && !defined(NO_HID)
  usbKbd.setBootProtocol(HID_ITF_PROTOCOL_KEYBOARD);
  usbKbd.setPollInterval(2);
  usbKbd.setReportDescriptor(kbd_desc, sizeof(kbd_desc));
  usbKbd.begin();
#endif
#if !defined(KBD_ONLY) && !defined(NO_HID)
  usbMouse.setBootProtocol(HID_ITF_PROTOCOL_MOUSE);
  usbMouse.setPollInterval(2);
  usbMouse.setReportDescriptor(mouse_desc, sizeof(mouse_desc));
  usbMouse.begin();
#endif
  usbCons.setPollInterval(4);
  usbCons.setReportDescriptor(cons_desc, sizeof(cons_desc));
  usbCons.begin();
  flash(RED);  // stadio A: interfacce USB registrate
  uint32_t t0 = millis();
  while (!TinyUSBDevice.mounted() && millis() - t0 < 15000) delay(10);
  flash(GREEN);  // stadio B: USB attesa finita

#ifdef NO_BLE
  flash(TinyUSBDevice.mounted() ? WHITE : RED);  // solo test USB
  return;
#endif
  rgb(5);  // magenta fisso = dentro Bluefruit.begin(); se resta acceso, e' bloccato
  bool ok = Bluefruit.begin(1, 1);
  rgb(0); delay(300);
  flash(ok ? BLUE : YELLOW);  // stadio C: BLU = begin ok, GIALLO = begin fallito
  Bluefruit.autoConnLed(false);  // il LED lo gestiamo noi (heartbeat)
  Bluefruit.setName("btbridge");
  hidSvc.begin();
  for (int i = 0; i < NREP; i++) rep[i].begin(&hidSvc);
  Bluefruit.Security.setIOCaps(false, false, false);  // Just Works
  Bluefruit.Security.setSecuredCallback(securedCb);
  // tastiera ri-accoppiata (modo pairing): chiave vecchia -> cancella il bond e riprova pulito
  Bluefruit.Security.setPairCompleteCallback([](uint16_t h, uint8_t status) {
    if (status != BLE_GAP_SEC_STATUS_SUCCESS) { BLEConnection* c = Bluefruit.Connection(h); c->removeBondKey(); why = 7; c->disconnect(); }
  });
  Bluefruit.Central.setConnectCallback(connectCb);
  Bluefruit.Central.setDisconnectCallback(disconnectCb);
  Bluefruit.Scanner.setRxCallback(scanCb);
  Bluefruit.Scanner.restartOnDisconnect(true);
  Bluefruit.Scanner.setInterval(160, 80);
  Bluefruit.Scanner.filterService(hidSvc);  // solo dispositivi con servizio HID
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.start(0);
  flash(WHITE);  // stadio D: avvio completato, poi heartbeat
}

// Heartbeat: lampo di 150 ms ogni 2 s, colore = stato:
// blu scansione, giallo HID trovato, ciano connesso, verde sicuro e attivo.
static void heartbeat() {
  static const uint8_t col[6] = {0, BLUE, BLUE, YELLOW, CYAN, GREEN};
  // secondo lampo LUNGO (500 ms, il primo e' breve) = ultimo errore: rosso discover HID fallita, magenta tastiera
  // ha chiuso (0x13), bianco chiuso da noi (0x16), verde timeout (0x08), giallo connessione
  // fallita (0x3E), blu altro.
  static const uint8_t wc[8] = {0, RED, 5, WHITE, GREEN, YELLOW, BLUE, CYAN};
  uint32_t t = millis() % 2000;
  rgb(t < 150 ? col[st] : (t >= 600 && t < 1100 && why) ? wc[why] : 0);
}

void loop() {
  // debug: 300 ms di luce fissa quando arriva un report BLE: blu = tastiera, magenta = mouse,
  // rosso = report in coda ma USB non pronta
  static uint32_t pk = 0, pm = 0, until = 0, col = 0;
  if (rxK != pk) { pk = rxK; until = millis() + 300; col = BLUE; }
  if (rxM != pm) { pm = rxM; until = millis() + 300; col = 5; }
  if ((kt != kh && !usbKbd.ready()) || (mt != mh && !usbMouse.ready())) { until = millis() + 300; col = RED; }
  if ((int32_t)(until - millis()) > 0) rgb(col); else heartbeat();
  // connesso ma non sicuro da 6 s (bond obsoleto, nessun evento): cancella bond e riparti
  if (st == 4 && millis() - t4 > 6000) {
    st = 1; why = 7; t4 = millis();
    uint8_t n = Bluefruit.Central.connected();
    for (uint16_t h = 0; h < 4 && n; h++) { BLEConnection* c = Bluefruit.Connection(h); if (c && c->connected()) { c->removeBondKey(); c->disconnect(); } }
  }
  while (ct != ch) {
    if (!usbCons.ready()) break;
    usbCons.sendReport(0, &cq[ct], 2); ct = (ct + 1) & 15;
  }
  while (kt != kh) {
    if (!usbKbd.ready()) break;
    usbKbd.sendReport(0, kq[kt].b, 8); kt = (kt + 1) & 15;
  }
  while (mt != mh) {
    if (!usbMouse.ready()) break;
    usbMouse.sendReport(0, mq[mt].b, 4); mt = (mt + 1) & 63;
  }
  delay(1);
}
