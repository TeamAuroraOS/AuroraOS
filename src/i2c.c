/*
 * Minimal I2C driver (ARM9).
 *
 * Snippets learned from / adapted from GodMode9
 * (https://github.com/d0k3/GodMode9, arm9/source/system/i2c.c), which uses the
 * fastboot3DS I2C driver by derrek & profi200. Everything below marked
 * "from GodMode9" is a direct port of that code; the read path was dropped
 * because AuroraOS only needs to write to the MCU to power off.
 */
#include "i2c.h"

/* --- Bus register bases (from GodMode9 i2c.c) --- */
#define I2C1_REGS_BASE (0x10161000) /* bus 0 */
#define I2C2_REGS_BASE (0x10144000) /* bus 1 -- the MCU lives here */
#define I2C3_REGS_BASE (0x10148000) /* bus 2 */

/* --- CNT control bits (from GodMode9 i2c.h) --- */
#define I2C_STOP       (1u)
#define I2C_START      (1u << 1)
#define I2C_ERROR      (1u << 2)
#define I2C_DIRE_WRITE (0u)
#define I2C_IRQ_ENABLE (1u << 6)
#define I2C_ENABLE     (1u << 7)
#define I2C_GET_ACK(reg) ((bool)((reg) >> 4 & 1u))

/* Register block layout, one per bus (from GodMode9 i2c.c). */
typedef struct {
    volatile uint8_t  REG_I2C_DATA;
    volatile uint8_t  REG_I2C_CNT;
    volatile uint16_t REG_I2C_CNTEX;
    volatile uint16_t REG_I2C_SCL;
} I2cRegs;

/* devId -> (busId, 8-bit device address). Rows from GodMode9 i2c.c. */
static const struct {
    uint8_t busId;
    uint8_t devAddr;
} i2cDevTable[] = {
    {0, 0x4A}, /* I2C_DEV_POWER   */
    {0, 0x7A}, /* I2C_DEV_CAMERA  */
    {0, 0x78}, /* I2C_DEV_CAMERA2 */
    {1, 0x4A}, /* I2C_DEV_MCU     */
};

/* from GodMode9 i2c.c */
static I2cRegs *i2cGetBusRegsBase(uint8_t busId) {
    switch (busId) {
        case 0:  return (I2cRegs *)I2C1_REGS_BASE;
        case 1:  return (I2cRegs *)I2C2_REGS_BASE;
        case 2:  return (I2cRegs *)I2C3_REGS_BASE;
        default: return (I2cRegs *)0;
    }
}

/* from GodMode9 i2c.c */
static void i2cWaitBusy(I2cRegs *const regs) {
    while (regs->REG_I2C_CNT & I2C_ENABLE)
        ;
}

/* from GodMode9 i2c.c (I2C_init) */
void I2C_init(void) {
    for (uint8_t bus = 0; bus < 3; bus++) {
        I2cRegs *regs = i2cGetBusRegsBase(bus);
        i2cWaitBusy(regs);
        regs->REG_I2C_CNTEX = 2;    /* magic clock-config values from GodMode9 */
        regs->REG_I2C_SCL   = 1280;
    }
}

/* from GodMode9 i2c.c (i2cStartTransfer) -- write-only variant.
   Selects the device, then the register, retrying up to 8 times on NAK. */
static bool i2cStartTransfer(I2cDevice devId, uint8_t regAddr,
                             I2cRegs *const regs) {
    const uint8_t devAddr = i2cDevTable[devId].devAddr;

    for (uint32_t i = 0; i < 8; i++) {
        i2cWaitBusy(regs);

        /* Select device and start. */
        regs->REG_I2C_DATA = devAddr;
        regs->REG_I2C_CNT  = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_START;
        i2cWaitBusy(regs);
        if (!I2C_GET_ACK(regs->REG_I2C_CNT)) { /* ack 0 => failed */
            regs->REG_I2C_CNT = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_ERROR | I2C_STOP;
            continue;
        }

        /* Select register (write direction). */
        regs->REG_I2C_DATA = regAddr;
        regs->REG_I2C_CNT  = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_DIRE_WRITE;
        i2cWaitBusy(regs);
        if (!I2C_GET_ACK(regs->REG_I2C_CNT)) { /* ack 0 => failed */
            regs->REG_I2C_CNT = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_ERROR | I2C_STOP;
            continue;
        }

        return true;
    }

    return false;
}

/* from GodMode9 i2c.c (I2C_writeRegBuf, reduced to a single byte) */
bool I2C_writeReg(I2cDevice devId, uint8_t regAddr, uint8_t data) {
    I2cRegs *const regs = i2cGetBusRegsBase(i2cDevTable[devId].busId);

    if (!i2cStartTransfer(devId, regAddr, regs))
        return false;

    regs->REG_I2C_DATA = data;
    regs->REG_I2C_CNT  = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_DIRE_WRITE | I2C_STOP;
    i2cWaitBusy(regs);

    return I2C_GET_ACK(regs->REG_I2C_CNT);
}
