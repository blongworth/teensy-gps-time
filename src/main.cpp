/*
Datalogger demo

RTC and teensy clock synced to GPS
~ms precision using GPS PPS signal
log position and analog reading
sample and average analog at 100Hz
Keep SD file open, flush every 10s
serial control of start/stop
SD available via MTP
*/

#include <Arduino.h>
#include <TimeLib.h>
#include <TinyGPSPlus.h>
#include <SD.h>
#include <MTP_Teensy.h>
#include <Flasher.h>
#include <EEPROM.h>

#define SerialGPS Serial1
#define SerialLander Serial2
#define TIME_HEADER "T"
#define GPS_SET_TIMEOUT 10 * 1000 // GPS sync frequency (ms)
#define PPS_PIN 33
#define ANALOG_CHAN A0
#define DATAFILE_INTERVAL 3600 * 4  // Time before new file creation (s)
// sample analog at 100Hz
#define BUFFER_SIZE 100
#define SAMPLE_INTERVAL_US 10000  // 10ms in microseconds

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

// create a Flasher object
int onTime = 1000;
int offTime = 0;
Flasher flasher(LED_BUILTIN, onTime, offTime);


void ppsISR() {
  gpsms = 0;
  pps_state = 1;
}

void adcISR() {
  adcBuffer[bufferIndex] = analogRead(ANALOG_CHAN);
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
  if (pps_state)
  {
    pps_state = 0;
    // pps output stops when no fix
    if (year() < 2024 ||
       (setclock >= GPS_SET_TIMEOUT &&
        gps.time.age() < 1000))
    {
      tmElements_t tm;
      tm.Year = gps.date.year() - 1970;
      tm.Month = gps.date.month();
      tm.Day = gps.date.day();
      tm.Hour = gps.time.hour();
      tm.Minute = gps.time.minute();
      tm.Second = gps.time.second();
      next_time = makeTime(tm);

      setTime(next_time);
      Teensy3Clock.set(next_time);
      sendTime();
      setclock = 0;
      Serial.println("Synching time");
    }
  }
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

  attachInterrupt(digitalPinToInterrupt(PPS_PIN), ppsISR, RISING);
  Serial.begin(9600);
  SerialGPS.begin(9600);
  SerialLander.begin(9600);
  Serial.println("Waiting for GPS time ... ");
  setSyncProvider(getTeensy3Time);
  flasher.begin();

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
  if (EEPROM.read(0))
  {
    Serial.println("Logging Data!!!");
    logging = true;
    flasher.update(100, 900);
  }
}

void loop()
{
  // Service MTP
  MTP.loop();
  
  // Service flasher
  flasher.run();
  
  if (Serial.available()) {
    uint8_t command = Serial.read();
    int ch = Serial.read();
    while (ch == ' ')
      ch = Serial.read();

    switch (command) {
    case 's': {
      Serial.println("\nLogging Data!!!");
      logging = true; 
      EEPROM.write(0, 1); 
      flasher.update(100, 900);
    } break;
    case 'x':
      Serial.println("Stopping data acquisition...");
      if (dataFile) {
        logging = false;
        EEPROM.write(0, 0);
        dataFile.close();
        flasher.update(onTime, offTime);
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
      if (gps.time.age() >= 1000 ) gpsms = 0;
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