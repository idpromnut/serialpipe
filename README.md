# serialpipe
Wifi-enabled logging of a UART/serial port via the ESP8266 (NodeMCU 1.0)

The default configuration will listen on the UART connected to the USB at 115200 baud. To put the system into "configuration mode", plug in the NodeMCU into a USB on your computer and connect a serial console to the NodeMCU. Reset the NodeMCU and send the 'c' character. You will be then able to configure the Wifi parameters, telnet listen port and Device Under Test and logging console baud rates.

Debug messages will be sent to the USB console (pins 1->TX and 3->RX). The logger will listen for data on the hardware UART (pins 15->TX and 13->RX). The default baud rate for both the debug/configuration and Device Under Test console is 115200. To provide power to the logger, you either need to provide 3.3V to the NodeMCU's 3V3 pin or 7-12V on the VIN pin.