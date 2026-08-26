/* Coded By DisLoPik for the AuroraOS Project. */
/*
 * Minimal I2C driver (ARM9).
 *
 * Learned from GodMode9 (https://github.com/d0k3/GodMode9). The driver GodMode9
 * uses is the fastboot3DS one by derrek & profi200
 * (arm9/source/system/i2c.{c,h}). Only the bus-init + single-register write
 * path is ported here -- that is all AuroraOS needs to ask the MCU to power the
 * console off. See src/i2c.c for the snippets that were taken.
 */
#ifndef AURORA_I2C_H
#define AURORA_I2C_H

#include <stdint.h>
#include <stdbool.h>

/* I2C device IDs. Values taken from GodMode9 arm9/source/system/i2c.h. */
typedef enum {
    I2C_DEV_POWER   = 0, /* unconfirmed */
    I2C_DEV_CAMERA  = 1, /* unconfirmed */
    I2C_DEV_CAMERA2 = 2, /* unconfirmed */
    I2C_DEV_MCU     = 3  /* management microcontroller: power / LCDs / LEDs */
} I2cDevice;

/* Initialise the I2C buses. Safe to call more than once. */
void I2C_init(void);

/* Write a single byte to a device register. Returns true on success. */
bool I2C_writeReg(I2cDevice devId, uint8_t regAddr, uint8_t data);

#endif /* AURORA_I2C_H */
