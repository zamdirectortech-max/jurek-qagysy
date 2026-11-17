# Bundled driver

Copied from the user's installed SparkFun MAX3010x Pulse and Proximity Sensor Library 1.1.2 on 2026-09-15.
Upstream: https://github.com/sparkfun/SparkFun_MAX3010x_Sensor_Library

Included files: MAX30105.h, MAX30105.cpp, heartRate.h, and the supplied LICENSE.md.
The unused heartRate.cpp and other algorithms are not included; the sketch uses its existing PulseDetector.

Changes:

- Renamed the library's I2C_BUFFER_LENGTH macro to MAX3010X_I2C_BUFFER_LENGTH, including its references. Values are unchanged (32 on ESP32); ESP32 Wire retains its own 128-byte default. This removes macro redefinition diagnostics without altering either buffer size.
- Removed the unused, uninitialized local originalContents in enableSlot(). It had no reads, writes, constructor, or side effects.

No register operations, sensor configuration, or FIFO algorithm were changed.
The globally installed library remains unchanged.
