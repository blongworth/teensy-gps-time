#include <Arduino.h>
#include <TimeLib.h>
#include <TinyGPSPlus.h>
#include <SD.h>
#include <MTP_Teensy.h>
/*
   TinyGPSPlus (TinyGPSPlus) object.
   Compare to RTC on interval, set RTC if !=
   Write time and position to SD on interval
   Use struct to store data
   TODO: add sync to another teensy with "T" unixtime serial send
*/

#define SerialGPS Serial1
#define SerialLander Serial2
#define TIME_HEADER "T"
#define SET_TIMEOUT 10 * 1000
#define PPS_PIN 2
#define DATAFILE_INTERVAL 30  // Create new file every hour
#define BUFFER_SIZE 10
#define SAMPLE_INTERVAL_US 100000  // 100ms in microseconds

IntervalTimer adcTimer;
volatile uint16_t adcBuffer[BUFFER_SIZE];
volatile uint8_t bufferIndex = 0;
volatile bool bufferFull = false;
const unsigned int FLUSH_INTERVAL = 10;  // Flush every 10 writes
File dataFile;  // Global file handle
// Offset hours from gps time (UTC)
const int offset = 1;   // Central European Time
time_t prevDisplay = 0; // when the digital clock was displayed
elapsedMillis setclock;
TinyGPSPlus gps;
const int chipSelect = BUILTIN_SDCARD;
char iso_ts[25];
char iso_gps[25];
bool primed = false;
bool high_wait = false;
volatile bool pps_state = false;
elapsedMillis gpsms;
elapsedMillis tms;
bool logging = false;

void ppsISR() {
  bool state = digitalRead(PPS_PIN);
  if (state && !pps_state) { // Rising edge
    gpsms = 0;
  }
  pps_state = state;
}

void adcISR() {
  adcBuffer[bufferIndex] = analogRead(A2);
  bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
  if (bufferIndex == 0) {
    bufferFull = true;
  }
}

float getADCAverage() {
  float sum = 0;
  uint8_t count = bufferFull ? BUFFER_SIZE : bufferIndex;
  if (count == 0) return 0;
  
  for (uint8_t i = 0; i < count; i++) {
    sum += adcBuffer[i];
  }
  return sum / count;
}

time_t getTeensy3Time()
{
  return Teensy3Clock.get();
}

void displayInfo()
{
  if (gps.location.isUpdated())
  {
    Serial.print(F("Location: ")); 
    Serial.print(gps.location.lat(), 6);
    Serial.print(F(","));
    Serial.print(gps.location.lng(), 6);
    Serial.println();
  }

  if (gps.date.isUpdated() && gps.time.isUpdated())
  {
    Serial.print(F("  Date/Time: "));
    Serial.print(gps.date.month());
    Serial.print(F("/"));
    Serial.print(gps.date.day());
    Serial.print(F("/"));
    Serial.print(gps.date.year());
    Serial.print(F(" "));
    if (gps.time.hour() < 10) Serial.print(F("0"));
    Serial.print(gps.time.hour());
    Serial.print(F(":"));
    if (gps.time.minute() < 10) Serial.print(F("0"));
    Serial.print(gps.time.minute());
    Serial.print(F(":"));
    if (gps.time.second() < 10) Serial.print(F("0"));
    Serial.print(gps.time.second());
    Serial.print(F("."));
    if (gps.time.centisecond() < 10) Serial.print(F("0"));
    Serial.print(gps.time.centisecond());
    Serial.print(F(" Fix age: "));
    Serial.print(gps.time.age());
    Serial.println();
    Serial.println(gps.time.value());
  }
}

void getISO8601Timestamp(char* buffer, size_t bufferSize)
{
  snprintf(buffer, bufferSize, "%04d-%02d-%02dT%02d:%02d:%02d.%02dZ", 
           year(), month(), day(), hour(), minute(), second(), (int)tms);
}

void getISO8601TimestampGPS(char* buffer, size_t bufferSize)
{
  snprintf(buffer, bufferSize, "%04d-%02d-%02dT%02d:%02d:%02d.%02dZ", 
           gps.date.year(), gps.date.month(), gps.date.day(),
           gps.time.hour(), gps.time.minute(), gps.time.second(), (int)gpsms);
}

void sendTime()
{
  char timestamp[25];
  getISO8601Timestamp(timestamp, sizeof(timestamp));
  long curtime = now();
  Serial2.write(TIME_HEADER);
  Serial2.write(curtime);
  Serial2.write("\n");
}

void setTimeGPS()
{
  static time_t next_time;

  // pps output stops when no fix
  if (!primed &&
      //gps.time.isUpdated() &&
      gps.time.age() < 500 &&
      (setclock >= SET_TIMEOUT || year() < 2024))
  {
    Serial.println("Priming GPS time");

    tmElements_t tm;
    tm.Year = gps.date.year() - 1970;
    tm.Month = gps.date.month();
    tm.Day = gps.date.day();
    tm.Hour = gps.time.hour();
    tm.Minute = gps.time.minute();
    tm.Second = gps.time.second();
    next_time = makeTime(tm);

    primed = true;
    if (pps_state) high_wait = true;
  }

  if (primed)
  { 
    // Set system time from GPS data string
    // if high, wait for low, set flag
    // if high and flag set, set time

    if (!pps_state)
    {
      high_wait = false;
    }
    if (pps_state && !high_wait)
    {
      Serial.println("Synching time");
      setTime(next_time);
      Teensy3Clock.set(next_time);
      sendTime();
      setclock = 0;
      primed = false;
    }
  }
}

void printDigits(int digits) {
  // utility function for digital clock display: prints preceding colon and leading 0
  Serial.print(":");
  if(digits < 10) Serial.print('0');
  Serial.print(digits);
}

void digitalClockDisplay(){
  // digital clock display of the time
  Serial.print(hour());
  printDigits(minute());
  printDigits(second());
  Serial.print(" ");
  Serial.print(day());
  Serial.print(" ");
  Serial.print(month());
  Serial.print(" ");
  Serial.print(year()); 
  Serial.println(); 
}

void logData() {
  static unsigned int writeCount = 0;

  char timestamp[25];
  getISO8601Timestamp(timestamp, sizeof(timestamp));
  char dataString[120];

  float voltage = (getADCAverage() * 3.3) / 4096.0;  // Convert to voltage

  if (gps.location.age() < 1000)
  {
    double longitude = gps.location.lng();
    double latitude = gps.location.lat();
    snprintf(dataString, sizeof(dataString), "%s,%.6f,%.6f,%.3f",
             timestamp, longitude, latitude, voltage);
  }
  else
  {
    const char *longitude = "NA";
    const char *latitude = "NA";
    snprintf(dataString, sizeof(dataString), "%s,%s,%s", timestamp, longitude, latitude);
    snprintf(dataString, sizeof(dataString), "%s,%s,%s,%.3f",
             timestamp, longitude, latitude, voltage);
  }

  // Check if file is open, if not try to open it
  if (!dataFile) {
    dataFile = SD.open("datalog.txt", FILE_WRITE);
    if (!dataFile) {
      Serial.println("error opening datalog.txt");
      return;
    }
  }

  // Write to file
  dataFile.println(dataString);
  Serial.println(dataString);
  // Increment counter and flush if needed
  writeCount++;
  if (writeCount >= FLUSH_INTERVAL) {
    dataFile.flush();
    writeCount = 0;
  }
}

void createNewDatafile() {
  static time_t lastFileTime = 0;
  time_t currentTime = now();

  if (currentTime - lastFileTime >= DATAFILE_INTERVAL || lastFileTime == 0) {
    if (dataFile) {
      dataFile.close();
    }
    
    char filename[32];
    snprintf(filename, sizeof(filename), "data_%04d%02d%02d_%02d%02d.txt", 
        year(), month(), day(), hour(), minute());
    
    dataFile = SD.open(filename, FILE_WRITE);
    if (!dataFile) {
      Serial.println("Error creating new data file");
      return;
    }
    
    lastFileTime = currentTime;
    Serial.print("Created new file: ");
    Serial.println(filename);
  }
}

void menu() {
  Serial.println();
  Serial.println("Menu Options:");
  Serial.println("\ts - Start Logging data");
  Serial.println("\tx - Stop Logging data");
  Serial.println("\tr - reset MTP");
  Serial.println("\th - Menu");
  Serial.println();
}

void setup()
{
  
  MTP.begin();
  pinMode(PPS_PIN, INPUT);
  analogReadResolution(12);

  adcTimer.begin(adcISR, SAMPLE_INTERVAL_US);

  attachInterrupt(digitalPinToInterrupt(PPS_PIN), ppsISR, CHANGE);
  Serial.begin(9600);
  while (!Serial);  // Wait for Arduino Serial Monitor to open
  SerialGPS.begin(9600);
  SerialLander.begin(9600);
  Serial.println("Waiting for GPS time ... ");
  setSyncProvider(getTeensy3Time);

  delay(100);
  if (timeStatus()!= timeSet) {
    Serial.println("Unable to sync with the RTC");
  } else {
    Serial.println("RTC has set the system time");
  }

  Serial.print("Initializing SD card...");

  // see if the card is present and can be initialized:
  if (!SD.begin(chipSelect)) {
    Serial.println("Card failed, or not present");
    while (1) {
      // No SD card, so don't do anything more - stay stuck here
    }
  }
  Serial.println("card initialized.");
  MTP.addFilesystem(SD, "SD Card");
  menu();
}

void loop()
{
  // Service MTP
  MTP.loop();
  
  if (Serial.available()) {
    uint8_t command = Serial.read();
    int ch = Serial.read();
    while (ch == ' ')
      ch = Serial.read();

    switch (command) {
    case 's': {
      Serial.println("\nLogging Data!!!");
      logging = true; 
    } break;
    case 'x':
      Serial.println("Stopping data acquisition...");
      if (dataFile) {
        logging = false;
        dataFile.close();
      }
      Serial.println("Data acquisition stopped");
      break;
    case 'r':
      Serial.println("Send Device Reset Event");
      MTP.send_DeviceResetEvent();
      break;
    default:
      menu();
      break;
    }
    while (Serial.read() != -1)
      ; // remove rest of characters.
  }

  while (SerialGPS.available())
  {
    // process gps messages
    gps.encode(SerialGPS.read());
  }

  setTimeGPS();

  if (timeStatus() != timeNotSet)
  {
    if (now() != prevDisplay)
    { // update the display only if the time has changed
      tms = 0;
      prevDisplay = now();
      getISO8601Timestamp(iso_ts, sizeof(iso_ts));
      getISO8601TimestampGPS(iso_gps, sizeof(iso_gps));
      Serial.print("Teensy clock: ");
      Serial.println(iso_ts);
      Serial.print("GPS clock:    ");
      Serial.println(iso_gps);
      if (logging)
      {
        createNewDatafile();
        logData();
      }
      else
      {
        Serial.println("Not logging");
      }
      Serial.println();
    }
  }
}