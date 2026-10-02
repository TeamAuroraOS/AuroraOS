#ifndef AURORA_I2C_H
#define AURORA_I2C_H

#include <stdint.h>
#include <stdbool.h>

/* Values from GodMode9 arm9/source/system/i2c.h. */
typedef enum {
    I2C_DEV_POWER   = 0, /* unconfirmed */
    I2C_DEV_CAMERA  = 1, /* unconfirmed */
    I2C_DEV_CAMERA2 = 2, /* unconfirmed */
    I2C_DEV_MCU     = 3  /* management microcontroller: power / LCDs / LEDs */
} I2cDevice;

/* Safe to call more than once. */
void I2C_init(void);

bool I2C_writeReg(I2cDevice devId, uint8_t regAddr, uint8_t data);

bool I2C_readRegBuf(I2cDevice devId, uint8_t regAddr, uint8_t *out, uint32_t size);

#endif
