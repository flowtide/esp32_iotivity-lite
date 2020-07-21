# Porting esp32-iotivity into ESP32 Version 4.1 build system

This directory contains source files porting old [esp32-iotivity](https://github.com/espressif/esp32-iotivity.git)
into year 2020 released ESP32 Version 4.1 build system.

### build & test
- Change working direction in `ESP-IDF Command Prompt`
```
cd examples
idf.py menuconfig
```
- setup WIFI and other configuration
```
idf.py menuconfig
```
- Build project
```
idf.py build
```
- Run & monitor
```
idf.py -p COM29 flash monitor
```
