#ifndef GPIO_3DS_H
#define GPIO_3DS_H

#define GPIO_DS 0x10147100u /* GPIO register used in the 3DS's DS/DSI mode */

int GPIO_write(const char *c); /* writes a byte to GPIO */

#endif /* GPIO_3DS_H */
