#define GPIO_3DS_H
#ifdef GPIO_3DS_H
#define GPIO_DS 10147100h //needed because of 3DS's DS/DSI mode
int GPIO_write(const char* c); //writes to GPIO
#endif