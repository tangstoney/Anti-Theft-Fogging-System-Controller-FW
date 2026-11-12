/**
 * @file cw2217.c
 * @brief CW2217EAAD 电量计寄存器访问。
 */
#include "i2c1.h"
#include "cw2217.h"

#define CW_REG_VERSION 0x00U
#define CW_REG_VCELL_H 0x02U
#define CW_REG_VCELL_L 0x03U
#define CW_REG_SOC_H   0x04U
#define CW_REG_SOC_L   0x05U
#define CW_REG_TEMP    0x06U
#define CW_REG_CONFIG  0x08U

/* CONFIG 写值：SLEEP[7:6]=00、RESTART[5:4]=11；随后 RESTART[5:4]=00 进入 active。 */
#define CW_CONFIG_WAKE    0x30U
#define CW_CONFIG_ACTIVE  0x00U

static int cw_read(uint8_t reg, uint8_t *value)
{
    int ret = I2c1_WriteRead(CW2217_ADDR7, &reg, 1U, value, 1U);

    return (ret == I2C1_OK) ? CW2217_OK : CW2217_ERR_BUS;
}

static int cw_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2];
    int ret;

    buf[0] = reg;
    buf[1] = value;
    ret = I2c1_Write(CW2217_ADDR7, buf, 2U);

    return (ret == I2C1_OK) ? CW2217_OK : CW2217_ERR_BUS;
}

int Cw2217_Probe(uint8_t *version)
{
    uint8_t reg = 0U;
    int ret;

    if (version == 0)
    {
        return CW2217_ERR_PARAM;
    }

    ret = cw_read(CW_REG_VERSION, &reg);
    if (ret != CW2217_OK)
    {
        return ret;
    }

    *version = reg;
    return (reg == CW2217_VERSION_EXPECTED) ? CW2217_OK : CW2217_ERR_ID;
}

int Cw2217_Wake(void)
{
    int ret = cw_write(CW_REG_CONFIG, CW_CONFIG_WAKE);

    if (ret != CW2217_OK)
    {
        return ret;
    }
    return cw_write(CW_REG_CONFIG, CW_CONFIG_ACTIVE);
}

int Cw2217_ReadSample(Cw2217Sample *out)
{
    uint8_t high = 0U;
    uint8_t low = 0U;
    int ret;

    if (out == 0)
    {
        return CW2217_ERR_PARAM;
    }

    ret = cw_read(CW_REG_VERSION, &out->version);
    if (ret != CW2217_OK)
    {
        return ret;
    }

    ret = cw_read(CW_REG_SOC_H, &high);
    if (ret != CW2217_OK)
    {
        return ret;
    }
    ret = cw_read(CW_REG_SOC_L, &low);
    if (ret != CW2217_OK)
    {
        return ret;
    }
    out->soc_raw = (uint16_t)(((uint16_t)high << 8) | low);

    ret = cw_read(CW_REG_VCELL_H, &high);
    if (ret != CW2217_OK)
    {
        return ret;
    }
    ret = cw_read(CW_REG_VCELL_L, &low);
    if (ret != CW2217_OK)
    {
        return ret;
    }
    out->vcell_raw = (uint16_t)(((uint16_t)(high & 0x3FU) << 8) | low);

    ret = cw_read(CW_REG_TEMP, &out->temp_raw);
    if (ret != CW2217_OK)
    {
        return ret;
    }

    return CW2217_OK;
}
