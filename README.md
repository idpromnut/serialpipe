# serialpipe
Wifi-enabled logging of a UART/serial port via the ESP8266 (NodeMCU 1.0)

The default configuration will listen on the UART connected to the USB at 115200 baud. To put the system into "configuration mode", connect a serial console to the NodeMCU, send the 'c' character and reset the NodeMCU. You will be then able to config the Wifi parameters, telnet listen port and Device Under Test and logging console baud rates.