// btbridge PROBE: scans for a BLE HID keyboard, bonds, and dumps its Report Map
// and every raw input notification over USB serial (115200). Throwaway tool.
#include <bluefruit.h>

volatile uint32_t advCount = 0;
BLEClientService hid(UUID16_SVC_HUMAN_INTERFACE_DEVICE);
BLEClientCharacteristic reportMap(UUID16_CHR_REPORT_MAP);
BLEClientCharacteristic protoMode(UUID16_CHR_PROTOCOL_MODE);
BLEClientCharacteristic bootKbd(UUID16_CHR_BOOT_KEYBOARD_INPUT_REPORT);
BLEClientCharacteristic bootMouse(UUID16_CHR_BOOT_MOUSE_INPUT_REPORT);
enum { NREP = 6 };
BLEClientCharacteristic rep[NREP] = {
  BLEClientCharacteristic(UUID16_CHR_REPORT), BLEClientCharacteristic(UUID16_CHR_REPORT),
  BLEClientCharacteristic(UUID16_CHR_REPORT), BLEClientCharacteristic(UUID16_CHR_REPORT),
  BLEClientCharacteristic(UUID16_CHR_REPORT), BLEClientCharacteristic(UUID16_CHR_REPORT)};

static void hexdump(const uint8_t* d, uint16_t n) {
  for (uint16_t i = 0; i < n; i++) Serial.printf("%02X ", d[i]);
  Serial.println();
}

void notifyCb(BLEClientCharacteristic* c, uint8_t* data, uint16_t len) {
  Serial.printf("[NOTIFY h=%u] len=%u : ", c->valueHandle(), len);
  hexdump(data, len);
}

// Scan callback runs on a small stack: keep it tiny, no printing. Details go to loop().
struct AdvRec { uint8_t addr[6]; uint8_t type; uint8_t hid; };
volatile AdvRec advQ[8]; volatile uint8_t advHead = 0, advTail = 0;

void scanCb(ble_gap_evt_adv_report_t* r) {
  advCount++;
  bool hidAd = Bluefruit.Scanner.checkReportForUuid(r, UUID16_SVC_HUMAN_INTERFACE_DEVICE);
  uint8_t nx = (advHead + 1) & 7;
  if (nx != advTail) {
    memcpy((void*)advQ[advHead].addr, r->peer_addr.addr, 6);
    advQ[advHead].type = r->peer_addr.addr_type;
    advQ[advHead].hid = hidAd;
    advHead = nx;
  }
  if (hidAd) Bluefruit.Central.connect(r); else Bluefruit.Scanner.resume();
}

void connectCb(uint16_t h) {
  BLEConnection* conn = Bluefruit.Connection(h);
  Serial.println("Connected, requesting pairing/encryption...");
  conn->requestPairing();
}

void securedCb(uint16_t h) {
  Serial.println("Secured. Discovering HID service...");
  if (!hid.discover(h)) { Serial.println("No HID service"); Bluefruit.disconnect(h); return; }
  Bluefruit.Discovery.discoverCharacteristic(h, reportMap, protoMode, bootKbd, bootMouse);
  for (int i = 0; i < NREP; i++) rep[i].discover();  // all remaining Report chars

  static uint8_t map[768];
  uint16_t n = reportMap.discovered() ? reportMap.read(map, sizeof(map)) : 0;
  Serial.printf("REPORT MAP (%u bytes):\n", n);
  hexdump(map, n);

  Serial.printf("protocolMode:%d bootKbd:%d bootMouse:%d\n",
                protoMode.discovered(), bootKbd.discovered(), bootMouse.discovered());
  for (int i = 0; i < NREP; i++) {
    if (!rep[i].discovered()) continue;
    Serial.printf("Report char #%d handle=%u props=0x%02X\n", i, rep[i].valueHandle(), rep[i].properties());
    if (rep[i].properties() & 0x10) {  // notify
      rep[i].setNotifyCallback(notifyCb);
      rep[i].enableNotify();
    }
  }
  Serial.println("Listening: type, move trackpad, scroll, press media keys...");
}

void disconnectCb(uint16_t h, uint8_t reason) {
  Serial.printf("Disconnected, reason 0x%02X\n", reason);
}

void setup() {
  Serial.begin(115200);
  // USB must be mounted before the SoftDevice starts, otherwise enumeration dies.
  uint32_t t0 = millis();
  while ((!TinyUSBDevice.mounted() || !Serial) && millis() - t0 < 10000) delay(10);
  Serial.println("btbridge probe");
  Serial.println("S1 before Bluefruit.begin");
  Bluefruit.begin(1, 1);
  Serial.println("S2 bluefruit up");
  Bluefruit.setConnLedInterval(250);
  Bluefruit.Security.setIOCaps(false, false, false);  // Just Works
  Bluefruit.Security.setSecuredCallback(securedCb);
  Serial.println("S3 security set");
  hid.begin();
  reportMap.begin(); protoMode.begin(); bootKbd.begin(); bootMouse.begin();
  for (int i = 0; i < NREP; i++) rep[i].begin();
  Bluefruit.Central.setConnectCallback(connectCb);
  Bluefruit.Central.setDisconnectCallback(disconnectCb);
  Bluefruit.Scanner.setRxCallback(scanCb);
  Bluefruit.Scanner.restartOnDisconnect(true);

  Bluefruit.Scanner.setInterval(160, 80);
  Bluefruit.Scanner.useActiveScan(false);
  Serial.println("S4 before scan start");
  Bluefruit.Scanner.start(0);
  Serial.println("Scanning: put the keyboard in pairing mode.");
}
void loop() {
  static uint32_t last = 0;
  while (advTail != advHead) {
    AdvRec a; memcpy(&a, (void*)&advQ[advTail], sizeof(a)); advTail = (advTail + 1) & 7;
    static uint8_t seen[24][6]; static int ns = 0; bool known = false;
    for (int i = 0; i < ns; i++) if (!memcmp(seen[i], a.addr, 6)) known = true;
    if (known || ns >= 24) continue;
    memcpy(seen[ns++], a.addr, 6);
    Serial.printf("ADV type=%u hid=%u addr=", a.type, a.hid);
    Serial.printBufferReverse(a.addr, 6, ':');
    Serial.println();
  }
  if (millis() - last > 2000) {
    last = millis();
    Serial.printf("[alive %lus scanning=%d adv=%lu]\n", millis() / 1000, Bluefruit.Scanner.isRunning(), (unsigned long)advCount);
  }
  delay(20);
}
