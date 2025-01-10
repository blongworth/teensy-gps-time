# Teensy logger demonstrator

* Log timestamp, GPS pos, and Analog voltage at 1 Hz
* Menu for starting and stopping logging
* Rotate log files, leaving file open between rotations
* Flush SD file data to make writes "safe" once every 10s and when stopping logging.
* sample ADC at 10-100Hz via timer interrupt. Store samples in array and average for 1Hz reading
* Synchronize teensy clock to GPS time to better than 100 ms using GPS pulse per second output and an interrupt
* Share files with PC via MTP
