/*
  Smart Attendance System using ESP32 + RC522 RFID + I2C 16x2 LCD

  Wiring assumed:
    RC522: SDA/SS=GPIO5, SCK=18, MOSI=23, MISO=19, RST=22, 3.3V, GND
    I2C LCD: SDA=GPIO21, SCL=GPIO17, GND, VCC (use only a safe voltage/signal setup)
    Buzzer module: S=GPIO25, middle/VCC=3V3 only if its rating allows, -=GND
    Green LED: GPIO26 -> 220 ohm resistor -> LED long leg; short leg -> GND
    Red LED:   GPIO27 -> 220 ohm resistor -> LED long leg; short leg -> GND

  Libraries to install in Arduino IDE:
    - MFRC522
    - LiquidCrystal I2C (for LiquidCrystal_I2C.h)

  Serial Monitor: 115200 baud, line ending = Newline

  Serial commands:
    HELP
    LIST
    ADD AA:BB:CC:DD Student Name   (replace with the real UID printed by your reader)

  Student registrations are stored in ESP32 Preferences and survive reboot.
  Attendance marks are kept for the current power session and printed to Serial.
  Without an RTC or Wi-Fi time, uptime_ms is elapsed time since boot, not date/time.
*/

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// ---------- Pin configuration ----------
const int RFID_SS_PIN = 5;
const int RFID_RST_PIN = 22;
const int RFID_SCK_PIN = 18;
const int RFID_MISO_PIN = 19;
const int RFID_MOSI_PIN = 23;

const int LCD_SDA_PIN = 21;
const int LCD_SCL_PIN = 17;
const uint8_t LCD_I2C_ADDRESS = 0x27;  // If the LCD stays blank, it may use 0x3F instead.

const int BUZZER_PIN = 25;
const int GREEN_LED_PIN = 26;
const int RED_LED_PIN = 27;

// Most 16x2 I2C LCDs are 16 columns x 2 rows.
LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, 16, 2);
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Preferences preferences;

// ---------- Student storage ----------
const int MAX_STUDENTS = 30;
struct Student {
  String uid;
  String name;
};
Student students[MAX_STUDENTS];
int studentCount = 0;

// Attendance is once per registered card per power session.
String attendedUIDs[MAX_STUDENTS];
int attendedCount = 0;

bool messageIsShowing = false;
unsigned long messageUntil = 0;
const unsigned long MESSAGE_DURATION_MS = 2200;

// Turn any UID spelling into a consistent format, e.g. AA:BB:CC:DD.
String normalizeUID(String input) {
  input.trim();
  String hexOnly = "";
  for (unsigned int i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    if (c >= 'a' && c <= 'f') c = c - 'a' + 'A';
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) {
      hexOnly += c;
    }
  }

  String formatted = "";
  for (unsigned int i = 0; i < hexOnly.length(); i++) {
    if (i > 0 && i % 2 == 0) formatted += ':';
    formatted += hexOnly.charAt(i);
  }
  return formatted;
}

bool isUsableUID(const String &uid) {
  if (uid.length() < 11) return false;  // At least 6 bytes in colon-separated format.
  int hexCount = 0;
  for (unsigned int i = 0; i < uid.length(); i++) {
    if (uid.charAt(i) != ':') hexCount++;
  }
  return (hexCount >= 8 && hexCount % 2 == 0);
}

String getScannedUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (i > 0) uid += ':';
    if (rfid.uid.uidByte[i] < 0x10) uid += '0';
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void showMessage(String line1, String line2, unsigned long durationMs = MESSAGE_DURATION_MS) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
  messageIsShowing = true;
  messageUntil = millis() + durationMs;
}

void showHomeScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Smart Attendance");
  lcd.setCursor(0, 1);
  lcd.print("Scan your card");
  messageIsShowing = false;
}

void setIndicators(bool greenOn, bool redOn) {
  digitalWrite(GREEN_LED_PIN, greenOn ? HIGH : LOW);
  digitalWrite(RED_LED_PIN, redOn ? HIGH : LOW);
}

// Assumes the buzzer module is active-HIGH and supports 3.3V supply.
void beepOnce(unsigned int durationMs) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepSuccess() {
  beepOnce(120);
}

void beepError() {
  beepOnce(90);
  delay(100);
  beepOnce(90);
}

int findStudentByUID(const String &uid) {
  for (int i = 0; i < studentCount; i++) {
    if (students[i].uid == uid) return i;
  }
  return -1;
}

bool alreadyAttended(const String &uid) {
  for (int i = 0; i < attendedCount; i++) {
    if (attendedUIDs[i] == uid) return true;
  }
  return false;
}

void loadStudents() {
  studentCount = preferences.getUInt("count", 0);
  if (studentCount > MAX_STUDENTS) studentCount = MAX_STUDENTS;

  for (int i = 0; i < studentCount; i++) {
    String uidKey = "u" + String(i);
    String nameKey = "n" + String(i);
    students[i].uid = normalizeUID(preferences.getString(uidKey.c_str(), ""));
    students[i].name = preferences.getString(nameKey.c_str(), "Student");
  }
}

void saveStudent(int index) {
  String uidKey = "u" + String(index);
  String nameKey = "n" + String(index);
  preferences.putString(uidKey.c_str(), students[index].uid);
  preferences.putString(nameKey.c_str(), students[index].name);
  preferences.putUInt("count", studentCount);
}

void printHelp() {
  Serial.println();
  Serial.println("--- Smart Attendance commands ---");
  Serial.println("HELP");
  Serial.println("LIST");
  Serial.println("ADD AA:BB:CC:DD Student Name");
  Serial.println("Use the exact UID printed when you scan your card.");
  Serial.println("Serial Monitor: 115200 baud, line ending Newline.");
  Serial.println();
}

void listStudents() {
  Serial.println("--- Registered cards ---");
  if (studentCount == 0) {
    Serial.println("No students registered yet. Scan a card, then use ADD <UID> <Name>.");
    return;
  }
  for (int i = 0; i < studentCount; i++) {
    Serial.print(i + 1);
    Serial.print(". ");
    Serial.print(students[i].name);
    Serial.print(" | UID: ");
    Serial.println(students[i].uid);
  }
}

void addOrUpdateStudent(String uidInput, String name) {
  String uid = normalizeUID(uidInput);
  name.trim();

  if (!isUsableUID(uid) || name.length() == 0) {
    Serial.println("Format: ADD AA:BB:CC:DD Student Name");
    Serial.println("Use the UID printed by the RFID reader.");
    return;
  }

  int existingIndex = findStudentByUID(uid);
  if (existingIndex >= 0) {
    students[existingIndex].name = name;
    saveStudent(existingIndex);
    Serial.print("Updated registered student: ");
    Serial.print(name);
    Serial.print(" | UID: ");
    Serial.println(uid);
    return;
  }

  if (studentCount >= MAX_STUDENTS) {
    Serial.println("Registration full: maximum of 30 students reached.");
    return;
  }

  students[studentCount].uid = uid;
  students[studentCount].name = name;
  saveStudent(studentCount);
  studentCount++;
  preferences.putUInt("count", studentCount);

  Serial.print("Registered: ");
  Serial.print(name);
  Serial.print(" | UID: ");
  Serial.println(uid);
}

void processSerialCommand() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  int firstSpace = line.indexOf(' ');
  String command = (firstSpace < 0) ? line : line.substring(0, firstSpace);
  command.toUpperCase();

  if (command == "HELP") {
    printHelp();
  } else if (command == "LIST") {
    listStudents();
  } else if (command == "ADD") {
    if (firstSpace < 0) {
      Serial.println("Format: ADD AA:BB:CC:DD Student Name");
      return;
    }
    String rest = line.substring(firstSpace + 1);
    rest.trim();
    int separator = rest.indexOf(' ');
    if (separator < 0) {
      Serial.println("Please include both the card UID and student name.");
      Serial.println("Example: ADD AA:BB:CC:DD Student One");
      return;
    }
    String uidText = rest.substring(0, separator);
    String studentName = rest.substring(separator + 1);
    studentName.trim();
    addOrUpdateStudent(uidText, studentName);
  } else {
    Serial.println("Unknown command. Type HELP.");
  }
}

void handleCard(String uid) {
  Serial.print("Card UID: ");
  Serial.println(uid);

  int index = findStudentByUID(uid);
  if (index < 0) {
    setIndicators(false, true);
    showMessage("Card not found", "Not registered");
    beepError();
    Serial.println("This card is not registered.");
    Serial.print("To register it, type: ADD ");
    Serial.print(uid);
    Serial.println(" Student Name");
    return;
  }

  const String studentName = students[index].name;
  if (alreadyAttended(uid)) {
    setIndicators(true, false);
    showMessage("Already marked", studentName);
    beepOnce(70);
    Serial.print("DUPLICATE, name=");
    Serial.print(studentName);
    Serial.print(", UID=");
    Serial.println(uid);
    return;
  }

  if (attendedCount < MAX_STUDENTS) {
    attendedUIDs[attendedCount++] = uid;
  }
  setIndicators(true, false);
  showMessage("Attendance OK", studentName);
  beepSuccess();

  Serial.print("ATTENDANCE, name=");
  Serial.print(studentName);
  Serial.print(", UID=");
  Serial.print(uid);
  Serial.print(", uptime_ms=");
  Serial.println(millis());
  Serial.println("Note: uptime_ms is elapsed time since boot, not calendar date/time.");
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(100);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  setIndicators(false, false);

  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Smart Attendance");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  // Explicit SPI pins for the ESP32.
  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();
  delay(50);

  preferences.begin("attendance", false);
  loadStudents();

  Serial.println();
  Serial.println("Smart Attendance System ready.");
  Serial.print("Registered students: ");
  Serial.println(studentCount);
  printHelp();

  showHomeScreen();
}

void loop() {
  processSerialCommand();

  if (messageIsShowing) {
    if ((long)(millis() - messageUntil) >= 0) {
      setIndicators(false, false);
      showHomeScreen();
    } else {
      return;
    }
  }

  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String uid = getScannedUID();
  handleCard(uid);

  // Finish communication with this card.
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
