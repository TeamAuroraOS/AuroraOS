#include "i2c.h"

#define I2C1_REGS_BASE (0x10161000) /* bus 0 */
#define I2C2_REGS_BASE (0x10144000) /* bus 1, MCU */
#define I2C3_REGS_BASE (0x10148000) /* bus 2 */

#define I2C_STOP       (1u)
#define I2C_START      (1u << 1)
#define I2C_ERROR      (1u << 2)
#define I2C_ACK        (1u << 4)
#define I2C_DIRE_READ  (1u << 5)
#define I2C_DIRE_WRITE (0u)
#define I2C_IRQ_ENABLE (1u << 6)
#define I2C_ENABLE     (1u << 7)
#define I2C_BUSY_TIMEOUT 1000000u
#define I2C_GET_ACK(reg) ((bool)((reg) >> 4 & 1u))

typedef struct {
    volatile uint8_t  REG_I2C_DATA;
    volatile uint8_t  REG_I2C_CNT;
    volatile uint16_t REG_I2C_CNTEX;
    volatile uint16_t REG_I2C_SCL;
} I2cRegs;

static const struct {
    uint8_t busId;
    uint8_t devAddr;
} i2cDevTable[] = {
    {0, 0x4A}, /* I2C_DEV_POWER   */
    {0, 0x7A}, /* I2C_DEV_CAMERA  */
    {0, 0x78}, /* I2C_DEV_CAMERA2 */
    {1, 0x4A}, /* I2C_DEV_MCU     */
};

static I2cRegs *i2cGetBusRegsBase(uint8_t busId) {
    switch (busId) {
        case 0:  return (I2cRegs *)I2C1_REGS_BASE;
        case 1:  return (I2cRegs *)I2C2_REGS_BASE;
        case 2:  return (I2cRegs *)I2C3_REGS_BASE;
        default: return (I2cRegs *)0;
    }
}

static bool i2cWaitBusy(I2cRegs *const regs) {
    for (uint32_t timeout = I2C_BUSY_TIMEOUT; timeout; timeout--)
        if (!(regs->REG_I2C_CNT & I2C_ENABLE))
            return true;
    return false;
}

void I2C_init(void) {
    for (uint8_t bus = 0; bus < 3; bus++) {
        I2cRegs *regs = i2cGetBusRegsBase(bus);
        /* A stuck peripheral must not hang the whole OS during startup. */
        if (!i2cWaitBusy(regs))
            continue;
        regs->REG_I2C_CNTEX = 2;
        regs->REG_I2C_SCL   = 1280;
    }
}

static bool i2cStartTransfer(I2cDevice devId, uint8_t regAddr, bool read,
                             I2cRegs *const regs) {
    const uint8_t devAddr = i2cDevTable[devId].devAddr;

    for (uint32_t i = 0; i < 8; i++) {
        if (!i2cWaitBusy(regs))
            return false;

        /* Select device (write) and start. */
        regs->REG_I2C_DATA = devAddr;
        regs->REG_I2C_CNT  = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_START;
        if (!i2cWaitBusy(regs))
            return false;
        if (!I2C_GET_ACK(regs->REG_I2C_CNT)) {
            regs->REG_I2C_CNT = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_ERROR | I2C_STOP;
            continue;
        }

        regs->REG_I2C_DATA = regAddr;
        regs->REG_I2C_CNT  = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_DIRE_WRITE;
        if (!i2cWaitBusy(regs))
            return false;
        if (!I2C_GET_ACK(regs->REG_I2C_CNT)) {
            regs->REG_I2C_CNT = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_ERROR | I2C_STOP;
            continue;
        }

        if (read) {
            /* Repeated start, reselect the device in read mode. */
            regs->REG_I2C_DATA = devAddr | 1u;
            regs->REG_I2C_CNT  = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_START;
            if (!i2cWaitBusy(regs))
                return false;
            if (!I2C_GET_ACK(regs->REG_I2C_CNT)) {
                regs->REG_I2C_CNT =
                    I2C_ENABLE | I2C_IRQ_ENABLE | I2C_ERROR | I2C_STOP;
                continue;
            }
        }

        return true;
    }

    return false;
}

bool I2C_writeReg(I2cDevice devId, uint8_t regAddr, uint8_t data) {
    if ((unsigned)devId >= sizeof(i2cDevTable) / sizeof(i2cDevTable[0]))
        return false;
    I2cRegs *const regs = i2cGetBusRegsBase(i2cDevTable[devId].busId);

    if (!i2cStartTransfer(devId, regAddr, false, regs))
        return false;

    regs->REG_I2C_DATA = data;
    regs->REG_I2C_CNT  = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_DIRE_WRITE | I2C_STOP;
    if (!i2cWaitBusy(regs))
        return false;

    return I2C_GET_ACK(regs->REG_I2C_CNT);
}

bool I2C_readRegBuf(I2cDevice devId, uint8_t regAddr, uint8_t *out,
                    uint32_t size) {
    if ((unsigned)devId >= sizeof(i2cDevTable) / sizeof(i2cDevTable[0]) ||
        (size && !out))
        return false;
    if (size == 0)
        return true;

    I2cRegs *const regs = i2cGetBusRegsBase(i2cDevTable[devId].busId);

    if (!i2cStartTransfer(devId, regAddr, true, regs))
        return false;

    /* All but the last byte: read and ACK to request the next one. */
    while (--size) {
        regs->REG_I2C_CNT = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_DIRE_READ | I2C_ACK;
        if (!i2cWaitBusy(regs))
            return false;
        *out++ = regs->REG_I2C_DATA;
    }
    /* Last byte: NAK + stop. */
    regs->REG_I2C_CNT = I2C_ENABLE | I2C_IRQ_ENABLE | I2C_DIRE_READ | I2C_STOP;
    if (!i2cWaitBusy(regs))
        return false;
    *out = regs->REG_I2C_DATA;

    return true;
}
