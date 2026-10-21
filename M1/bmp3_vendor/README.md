# Bosch BMP3 SensorAPI

Unmodified `bmp3.c`, `bmp3.h`, and `bmp3_defs.h` from Bosch Sensortec
[BMP3_SensorAPI](https://github.com/boschsensortec/BMP3_SensorAPI/tree/db4cf8e4140c593b8c3d85f8c6c07335c7ffa9dc),
commit `db4cf8e4140c593b8c3d85f8c6c07335c7ffa9dc` (v2.0.6).
The BSD-3-Clause license and copyright are retained in each source header.

M1 uses the official 64-bit integer compensation, factory coefficients and
little-endian register decoding for BMP388 (ID 0x50) and BMP390 (ID 0x60).
The adapter is in `../barometer.c`; no pressure offset is applied.
