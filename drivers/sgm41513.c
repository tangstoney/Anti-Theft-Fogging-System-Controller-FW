/**
 * @file sgm41513.c
 * @brief SGM41513 充电器状态读取：REG08/REG09/REG0B。
 */
#include "i2c1.h"
#include "sgm41513.h"

#define SGM_REG08 0x08U
#define SGM_REG09 0x09U
#define SGM_REG0B 0x0BU

static int sgm_read_reg(uint8_t reg, uint8_t *value)
{
    int ret = I2c1_WriteRead(SGM41513_ADDR7, &reg, 1U, value, 1U);

    return (ret == I2C1_OK) ? SGM_OK : SGM_ERR_BUS;
}

int Sgm41513_ReadId(uint8_t *pn, uint8_t *dev_rev)
{
    uint8_t reg;
    int ret;

    if (pn == 0)
    {
        return SGM_ERR_PARAM;
    }

    ret = sgm_read_reg(SGM_REG0B, &reg);
    if (ret != SGM_OK)
    {
        return ret;
    }

    /* REG0B：D[6:3] PN[3:0]，D[1:0] DEV_REV[1:0]。 */
    *pn = (uint8_t)((reg >> 3) & 0x0FU);
    if (dev_rev != 0)
    {
        *dev_rev = (uint8_t)(reg & 0x03U);
    }
    return SGM_OK;
}

int Sgm41513_ReadStatus(Sgm41513Status *out)
{
    uint8_t history;
    uint8_t current;
    int ret;

    if (out == 0)
    {
        return SGM_ERR_PARAM;
    }

    ret = sgm_read_reg(SGM_REG08, &out->reg08);
    if (ret != SGM_OK)
    {
        return ret;
    }

    /*
     * REG09 带历史锁存：第一次读返回“自上次读取以来的历史故障”，
     * 第二次读才是当前仍然有效的故障（手册 I2C 故障读取说明）。
     * 两次都读，避免把已经消失的故障一直当成故障闪烁。
     */
    ret = sgm_read_reg(SGM_REG09, &history);
    if (ret != SGM_OK)
    {
        return ret;
    }
    ret = sgm_read_reg(SGM_REG09, &current);
    if (ret != SGM_OK)
    {
        return ret;
    }

    out->reg09 = current;
    out->vbus_stat = SGM_REG08_VBUS_STAT(out->reg08);
    out->chrg_stat = SGM_REG08_CHRG_STAT(out->reg08);
    out->pg_stat = SGM_REG08_PG_STAT(out->reg08);
    out->therm_stat = SGM_REG08_THERM_STAT(out->reg08);
    out->vsys_stat = SGM_REG08_VSYS_STAT(out->reg08);
    out->chrg_fault = SGM_REG09_CHRG_FAULT(current);
    out->bat_fault = SGM_REG09_BAT_FAULT(current);
    out->ntc_fault = SGM_REG09_NTC_FAULT(current);

    return SGM_OK;
}
