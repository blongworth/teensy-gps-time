#include <Arduino.h>
#include <TimeLib.h>
#include <TinyGPSPlus.h>
#include <SD.h>

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

// Offset hours from gps time (UTC)
const int offset = 1;   // Central European Time
time_t prevDisplay = 0; // when the digital clock was displayed
elapsedMillis setclock;
TinyGPSPlus gps;
const int chipSelect = BUILTIN_SDCARD;

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

void setTimeGPS()
{
  Serial.println("Setting time to GPS");
  int Year = gps.date.year();
  byte Month = gps.date.month();
  byte Day = gps.date.day();
  byte Hour = gps.time.hour();
  byte Minute = gps.time.minute();
  byte Second = gps.time.second();

  // Set system time from GPS data string
  setTime(Hour, Minute, Second, Day, Month, Year);

  // Set the RTC in teensy from system time
  Teensy3Clock.set(now());
}

void printDigits(int digits) {
  // utility function for digital clock display: prints preceding colon and leading 0
  Serial.print(":");
  if(digits < 10)
    Serial.print('0');
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

void getISO8601Timestamp(char* buffer, size_t bufferSize)
{
  snprintf(buffer, bufferSize, "%04d-%02d-%02dT%02d:%02d:%02dZ", 
           year(), month(), day(), hour(), minute(), second());
}

void logData() {
  char timestamp[25];
  getISO8601Timestamp(timestamp, sizeof(timestamp));
  char dataString[100];
  if (gps.location.isUpdated())
  {
    double longitude = gps.location.lng(); 
    double latitude = gps.location.lat();
    snprintf(dataString, sizeof(dataString), "%s,%.6f,%.6f", timestamp, longitude, latitude);
  }
  else
  {
    const char* longitude = "NA"; 
    const char* latitude = "NA";
    snprintf(dataString, sizeof(dataString), "%s,%s,%s", timestamp, longitude, latitude);
  }

  File dataFile = SD.open("datalog.txt", FILE_WRITE);

  // if the file is available, write to it:
  if (dataFile) {
    dataFile.println(dataString);
    dataFile.close();
    // print to the serial port too:
    Serial.println(dataString);
  } else {
    // if the file isn't open, pop up an error:
    Serial.println("error opening datalog.txt");
  }
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

void setup()
{
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
}

void loop()
{
  while (SerialGPS.available()) {
    if (gps.encode(SerialGPS.read())) { // process gps messages
      // when TinyGPS reports new data...
      // displayInfo();
      if (gps.time.isUpdated() && 
          gps.time.age() < 500 && 
          (setclock >= SET_TIMEOUT || year() < 2024)) 
      {
        setTimeGPS();
        sendTime();
        setclock = 0;
      }
    }
  }
  if (timeStatus() != timeNotSet)
  {
    if (now() != prevDisplay)
    { // update the display only if the time has changed
      prevDisplay = now();
      Serial.print("Teensy clock: ");
      digitalClockDisplay();
      logData();
    }
  }
}