#include <Wire.h>
#include <Adafruit_PN532.h>
#include <DFRobotDFPlayerMini.h>
#include <HardwareSerial.h>

// ==================== PIN DEFINITIONS ====================
#define SDA_PIN 9
#define SCL_PIN 8
#define DFPLAYER_TX_PIN 0
#define DFPLAYER_RX_PIN 1
#define VOLUME_UP_PIN 5
#define VOLUME_DOWN_PIN 6
#define LED_PIN 4

// ==================== CONSTANTS ====================
const int DEFAULT_VOLUME = 20;
const int MAX_VOLUME = 30;
const int MIN_VOLUME = 0;
const unsigned long NFC_CHECK_INTERVAL = 200;
const unsigned long TAG_GRACE_PERIOD = 2000;
const unsigned long BUTTON_DEBOUNCE_DELAY = 200;

// ==================== HARDWARE INSTANCES ====================
Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);
HardwareSerial dfPlayerSerial(1);
DFRobotDFPlayerMini dfPlayer;

// ==================== STATE VARIABLES ====================
struct SystemState {
  uint8_t lastUID[7] = {0};
  uint8_t lastUIDLength = 0;
  unsigned long lastTagDetectionTime = 0;
  unsigned long lastNFCCheckTime = 0;
  bool isTagPresent = false;
  bool isSongPlaying = false;
  int currentTrack = 0;
  int currentVolume = DEFAULT_VOLUME;
} state;

struct ButtonState {
  bool lastUpState = HIGH;
  bool lastDownState = HIGH;
  unsigned long lastUpPress = 0;
  unsigned long lastDownPress = 0;
} buttons;

// Operation mode
enum Mode { PLAY_MODE, WRITE_MODE, READ_MODE };
Mode currentMode = PLAY_MODE;

// ==================== UTILITY FUNCTIONS ====================
String uidToString(uint8_t* uid, uint8_t length) {
  String result = "";
  for (uint8_t i = 0; i < length; i++) {
    if (i > 0) result += ":";
    if (uid[i] < 0x10) result += "0";
    result += String(uid[i], HEX);
  }
  result.toUpperCase();
  return result;
}

bool uidsMatch(uint8_t* uid1, uint8_t* uid2, uint8_t length) {
  for (uint8_t i = 0; i < length; i++) {
    if (uid1[i] != uid2[i]) return false;
  }
  return true;
}

// ==================== HARDWARE INITIALIZATION ====================
void initializeButtons() {
  pinMode(VOLUME_UP_PIN, INPUT_PULLUP);
  pinMode(VOLUME_DOWN_PIN, INPUT_PULLUP);
  Serial.println("✅ Volume buttons initialized");
}

void initializeLED() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}

void initializeI2C() {
  Wire.begin(SDA_PIN, SCL_PIN);
}

void initializeNFC() {
  Serial.println("Initializing PN532 NFC Reader...");
  nfc.begin();

  uint32_t version = nfc.getFirmwareVersion();
  if (!version) {
    Serial.println("❌ ERROR: PN532 not found!");
    while (1);
  }

  Serial.print("✅ Found PN5");
  Serial.println((version >> 24) & 0xFF, HEX);
  nfc.SAMConfig();
}

void initializeDFPlayer() {
  Serial.println("Initializing DFPlayer Mini...");
  dfPlayerSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX_PIN, DFPLAYER_TX_PIN);
  if (!dfPlayer.begin(dfPlayerSerial)) {
    Serial.println("❌ ERROR: DFPlayer not responding!");
    while (1);
  }

  dfPlayer.volume(DEFAULT_VOLUME);
  dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
  dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
  Serial.println("✅ DFPlayer Mini online");
}

// ==================== PLAYBACK CONTROL ====================
void setLED(bool on) { digitalWrite(LED_PIN, on ? HIGH : LOW); }

void playSong(int trackNumber) {
  Serial.println("🎵 PLAYING: Track " + String(trackNumber));
  dfPlayer.play(trackNumber);
  setLED(true);
  state.currentTrack = trackNumber;
  state.isSongPlaying = true;
}

void stopSong() {
  if (!state.isSongPlaying) return;
  Serial.println("⏹️  STOPPING: Track " + String(state.currentTrack));
  dfPlayer.stop();
  setLED(false);
  state.isSongPlaying = false;
  state.currentTrack = 0;
}

// ==================== VOLUME CONTROL ====================
void adjustVolume(int delta) {
  int newVolume = state.currentVolume + delta;
  if (newVolume < MIN_VOLUME) newVolume = MIN_VOLUME;
  if (newVolume > MAX_VOLUME) newVolume = MAX_VOLUME;
  state.currentVolume = newVolume;
  dfPlayer.volume(state.currentVolume);
  Serial.println("🔊 Volume: " + String(state.currentVolume));
}

void checkVolumeButtons() {
  unsigned long now = millis();
  bool upPressed = (digitalRead(VOLUME_UP_PIN) == LOW);
  bool downPressed = (digitalRead(VOLUME_DOWN_PIN) == LOW);

  if (upPressed && !buttons.lastUpState &&
      (now - buttons.lastUpPress > BUTTON_DEBOUNCE_DELAY)) {
    adjustVolume(1);
    buttons.lastUpPress = now;
  }
  buttons.lastUpState = upPressed;

  if (downPressed && !buttons.lastDownState &&
      (now - buttons.lastDownPress > BUTTON_DEBOUNCE_DELAY)) {
    adjustVolume(-1);
    buttons.lastDownPress = now;
  }
  buttons.lastDownState = downPressed;
}

// ==================== NFC TAG READING / WRITING ====================
int readSongNumberFromTag() {
  uint8_t data[4];
  bool success = nfc.ntag2xx_ReadPage(4, data);
  if (!success) {
    Serial.println("❌ Failed to read tag data");
    return -1;
  }

  if (data[0] == 'S' && data[1] == 'O' && data[2] == 'N') {
    int songNum = data[3];
    if (songNum >= 1 && songNum <= 99) return songNum;
  }
  Serial.println("❌ Tag not programmed correctly");
  return -1;
}

void writeSongNumber(uint8_t songNum) {
  Serial.print("\nPlace NFC tag to write song #");
  Serial.println(songNum);
  uint8_t uid[7]; uint8_t uidLength;
  bool success = false;
  for (int i = 0; i < 50; i++) {
    success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 200);
    if (success) break;
  }
  if (!success) {
    Serial.println("Timeout - no tag detected");
    return;
  }

  uint8_t data[4] = {'S', 'O', 'N', songNum};
  success = nfc.ntag2xx_WritePage(4, data);
  delay(100);
  if (success) {
    Serial.println("✓ Tag written successfully!");
  } else {
    Serial.println("✗ Write failed");
  }
}

void readSongTag() {
  uint8_t uid[7]; uint8_t uidLength;
  Serial.println("Place NFC tag to read...");
  bool success = false;
  for (int i = 0; i < 50; i++) {
    success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 200);
    if (success) break;
  }
  if (!success) {
    Serial.println("Timeout - no tag detected");
    return;
  }

  int songNum = readSongNumberFromTag();
  if (songNum != -1) Serial.println("✓ Song number: " + String(songNum));
}

// ==================== TAG HANDLING FOR PLAY MODE ====================
void handleNewTag(const String& uid, uint8_t* rawUID, uint8_t length) {
  Serial.println("\n=== NFC TAG DETECTED ===");
  Serial.println("  UID: " + uid);
  int songNumber = readSongNumberFromTag();
  if (songNumber == -1) {
    stopSong();
    return;
  }
  if (state.isSongPlaying && state.currentTrack != songNumber) stopSong();
  if (!state.isSongPlaying) playSong(songNumber);
}

void checkNFCTag() {
  uint8_t uid[7];
  uint8_t uidLength;
  bool tagDetected = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100);
  if (tagDetected) {
    state.lastTagDetectionTime = millis();
    bool isNewTag = !state.isTagPresent || !uidsMatch(uid, state.lastUID, uidLength);
    if (isNewTag) {
      String uidString = uidToString(uid, uidLength);
      handleNewTag(uidString, uid, uidLength);
      memcpy(state.lastUID, uid, uidLength);
      state.lastUIDLength = uidLength;
    }
    state.isTagPresent = true;
  } else {
    if (state.isTagPresent) {
      state.isTagPresent = false;
    }
  }
}

void handleGracePeriod() {
  if (state.isTagPresent || !state.isSongPlaying) return;
  unsigned long timeSinceRemoval = millis() - state.lastTagDetectionTime;
  if (timeSinceRemoval > TAG_GRACE_PERIOD) stopSong();
}

// ==================== COMMAND HANDLER ====================
void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd.startsWith("write ")) {
    int songNum = cmd.substring(6).toInt();
    if (songNum >= 1 && songNum <= 99) {
      currentMode = WRITE_MODE;
      writeSongNumber(songNum);
      currentMode = PLAY_MODE;
    } else {
      Serial.println("Error: number must be 1–99");
    }
  } else if (cmd == "read") {
    currentMode = READ_MODE;
    readSongTag();
    currentMode = PLAY_MODE;
  } else if (cmd == "playmode") {
    currentMode = PLAY_MODE;
    Serial.println("Switched to PLAY MODE");
  } else if (cmd.length() > 0) {
    Serial.println("Commands:");
    Serial.println("  write <num> - program tag");
    Serial.println("  read        - read tag");
    Serial.println("  playmode    - normal playback");
  }
}

// ==================== MAIN PROGRAM ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n🎵 ESP32 NFC Music Player + Tag Writer v1.0\n");
  initializeButtons();
  initializeLED();
  initializeI2C();
  initializeDFPlayer();
  initializeNFC();
  Serial.println("Type 'read' or 'write <number>' to access tag mode.\n");
}

void loop() {
  handleSerialCommands();

  if (currentMode == PLAY_MODE) {
    unsigned long now = millis();
    checkVolumeButtons();
    if (now - state.lastNFCCheckTime >= NFC_CHECK_INTERVAL) {
      checkNFCTag();
      state.lastNFCCheckTime = now;
    }
    handleGracePeriod();
  }

  delay(10);
}
