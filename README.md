# ESP32CAM-OTA

## Functionnalities
- Serving on ```/``` camera JPEG stream with HTTP basic authentification.
- Serving on ```/metrics``` ESP32-CAM metrics for Prometheus with HTTP basic authentification such as :
    - Free heap
    - Temperature
    - Up time
    - Wifi signal
- HTTPS OTA client waiting for a new firmaware version available on this code repository. Application version is defined in ```/CMakeLists.txt``` as ```set(PROJECT_VER "X.X.X")```.
- WiFi client