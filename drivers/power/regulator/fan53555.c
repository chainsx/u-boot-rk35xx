// SPDX-License-Identifier:     GPL-2.0+
/*
 * (C) 2018 Theobroma Systems Design und Consulting GmbH
 */

#include <common.h>
#include <bitfield.h>
#include <errno.h>
#include <dm.h>
#include <fdtdec.h>
#include <log.h>
#include <asm/gpio.h>
#include <linux/bitops.h>
#include <linux/printk.h>
#include <power/fan53555.h>
#include <power/pmic.h>
#include <power/regulator.h>

/**
 * struct ic_types - definition of fan53555-family devices
 *
 * @die_id: Identifies the DIE_ID (lower nibble of the ID1 register)
 * @die_rev: Identifies the DIE_REV (lower nibble of the ID2 register)
 * @vsel_min: starting voltage (step 0) in uV
 * @vsel_step: increment of the voltage in uV
 *
 * The voltage ramp (i.e. minimum voltage and step) is selected from the
 * combination of 2 nibbles: DIE_ID and DIE_REV.
 *
 * See http://www.onsemi.com/pub/Collateral/FAN53555-D.pdf for details.
 */
static const struct {
	unsigned int vendor;
	u8 die_id;
	u8 die_rev;
	bool check_rev;
	u32 vsel_min;
	u32 vsel_step;
} ic_types[] = {
	/* Option 00 */
	{ FAN53555_VENDOR_FAIRCHILD, 0x0, 0x3, true,  600000, 10000 },
	/* Option 13 */
	{ FAN53555_VENDOR_FAIRCHILD, 0x0, 0xf, true,  800000, 10000 },
	/* Option 23 */
	{ FAN53555_VENDOR_FAIRCHILD, 0x0, 0xc, true,  600000, 12500 },
	/* Option 01 */
	{ FAN53555_VENDOR_FAIRCHILD, 0x1, 0x3, true,  600000, 10000 },
	/* Option 03 */
	{ FAN53555_VENDOR_FAIRCHILD, 0x3, 0x3, true,  600000, 10000 },
	/* Option 04 */
	{ FAN53555_VENDOR_FAIRCHILD, 0x4, 0xf, true,  603000, 12826 },
	/* Option 05 */
	{ FAN53555_VENDOR_FAIRCHILD, 0x5, 0x3, true,  600000, 10000 },
	/* Option 08 */
	{ FAN53555_VENDOR_FAIRCHILD, 0x8, 0x1, true,  600000, 10000 },
	/* Option 08 */
	{ FAN53555_VENDOR_FAIRCHILD, 0x8, 0xf, true,  600000, 10000 },
	/* Option 09 */
	{ FAN53555_VENDOR_FAIRCHILD, 0xc, 0xf, true,  603000, 12826 },
	/* SYL82X */
	{ FAN53555_VENDOR_SILERGY,   0x8, 0x0, false, 712500, 12500 },
	/* SYL83X */
	{ FAN53555_VENDOR_SILERGY,   0x9, 0x0, false, 712500, 12500 },
};

/* I2C-accessible byte-sized registers */
enum {
	/* Voltage setting */
	FAN53555_VSEL0 = 0x00,
	FAN53555_VSEL1,
	/* Control register */
	FAN53555_CONTROL,
	/* IC Type */
	FAN53555_ID1,
	/* IC mask version */
	FAN53555_ID2,
	/* Monitor register */
	FAN53555_MONITOR,
};

struct fan53555_plat {
	/* Voltage setting register */
	unsigned int vol_reg;
	unsigned int sleep_reg;

};

struct fan53555_priv {
	/* IC Vendor */
	unsigned int vendor;
	/* IC Type and Rev */
	unsigned int die_id;
	unsigned int die_rev;
	/* Voltage range and step(linear) */
	unsigned int vsel_min;
	unsigned int vsel_step;
	/* Voltage slew rate limiting */
	unsigned int slew_rate;
	/* Sleep voltage cache */
	unsigned int sleep_vol_cache;

	int rk860_type;
};

enum {
	IS_RK860_0_ONLY = 1,
	IS_RK860_1_ONLY,
	IS_RK860_0_1,
};

static int fan53555_regulator_of_to_plat(struct udevice *dev)
{
	struct fan53555_plat *plat = dev_get_plat(dev);
	struct dm_regulator_uclass_plat *uc_pdata =
		dev_get_uclass_plat(dev);
	u32 sleep_vsel;

	/* This is a buck regulator */
	uc_pdata->type = REGULATOR_TYPE_BUCK;

	sleep_vsel = dev_read_u32_default(dev, "fcs,suspend-voltage-selector",
					  FAN53555_VSEL1);

	/*
	 * Depending on the device-tree settings, the 'normal mode'
	 * voltage is either controlled by VSEL0 or VSEL1.
	 */
	switch (sleep_vsel) {
	case FAN53555_VSEL0:
		plat->sleep_reg = FAN53555_VSEL0;
		plat->vol_reg = FAN53555_VSEL1;
		break;
	case FAN53555_VSEL1:
		plat->sleep_reg = FAN53555_VSEL1;
		plat->vol_reg = FAN53555_VSEL0;
		break;
	default:
		pr_err("%s: invalid vsel id %d\n", dev->name, sleep_vsel);
		return -EINVAL;
	}

	return 0;
}

static int fan53555_regulator_get_value(struct udevice *dev)
{
	struct fan53555_plat *pdata = dev_get_plat(dev);
	struct fan53555_priv *priv = dev_get_priv(dev);
	int reg;
	int voltage;

	/* We only support a single voltage selector (i.e. 'normal' mode). */
	reg = pmic_reg_read(dev->parent, pdata->vol_reg);
	if (reg < 0)
		return reg;
	voltage = priv->vsel_min + (reg & 0x3f) * priv->vsel_step;

	debug("%s: %d uV\n", __func__, voltage);
	return voltage;
}

static int fan53555_regulator_set_value(struct udevice *dev, int uV)
{
	struct fan53555_plat *pdata = dev_get_plat(dev);
	struct fan53555_priv *priv = dev_get_priv(dev);
	u8 vol;

	vol = (uV - priv->vsel_min) / priv->vsel_step;
	debug("%s: uV=%d; writing volume %d: %02x\n",
	      __func__, uV, pdata->vol_reg, vol);

	return pmic_clrsetbits(dev->parent, pdata->vol_reg, GENMASK(6, 0), vol);
}

static int fan53555_voltages_setup(struct udevice *dev)
{
	struct fan53555_priv *priv = dev_get_priv(dev);
	int i;

	/* Init voltage range and step */
	for (i = 0; i < ARRAY_SIZE(ic_types); ++i) {
		if (ic_types[i].vendor != priv->vendor)
			continue;

		if (ic_types[i].die_id != priv->die_id)
			continue;

		if (ic_types[i].check_rev &&
		    ic_types[i].die_rev != priv->die_rev)
			continue;

		priv->vsel_min = ic_types[i].vsel_min;
		priv->vsel_step = ic_types[i].vsel_step;

		return 0;
	}

	pr_err("%s: %s: die id %d rev %d not supported!\n",
	       dev->name, __func__, priv->die_id, priv->die_rev);
	return -EINVAL;
}

enum {
	DIE_ID_SHIFT = 0,
	DIE_ID_WIDTH = 4,
	DIE_REV_SHIFT = 0,
	DIE_REV_WIDTH = 4,
};

static void fan53555_rk860_calibration(struct udevice *dev, struct fan53555_priv *di)
{
	int ret;
	uint8_t buffer0[3], buffer1[3];
	uint8_t value, version0 = 0x04, version1 = 0x04;
	uint8_t flag = 0;

	di->rk860_type = 0;
	ret = dm_i2c_addr_read(dev, 0x40, 0x0E, &value, 1);
	if (ret < 0) {
		printf("%s>>>>>>hardware do not have rk860-0\n", __func__);
	} else {
		if (value == 0x00 || value == 0x04) {
			di->rk860_type = IS_RK860_0_ONLY;
			version0 = value & 0x04;
			printf("%s>>>>>>hardware have rk860-0, reg[0x0e] = 0x%x\n", __func__, value);
		} else {
			printf("%s>>>>>>hardware 0x40 i2c-dev is not rk860-0, maybe syr827/syr837, reg[0x0e] = 0x%x\n", __func__, value);
			flag = 1;
		}
	}

	ret = dm_i2c_addr_read(dev, 0x41, 0x0E, &value, 1);
	if (ret < 0) {
		printf("%s>>>>>>hardware do not have rk860-1\n", __func__);
	} else {
		if (value == 0x44 || value == 0x40) {
			if (di->rk860_type)
				di->rk860_type = IS_RK860_0_1;
			else
				di->rk860_type = IS_RK860_1_ONLY;

			version1 = value & 0x04;
			printf("%s>>>>>>hardware have rk860-1, reg[0x0e] = 0x%x\n", __func__, value);
		} else {
			printf("%s>>>>>>hardware 0x41 i2c-dev is not rk860-1, maybe syr828/syr838, reg[0x0e] = 0x%x\n", __func__, value);
			flag = 2;
		}
	}

	if (flag == 1 && di->rk860_type == IS_RK860_1_ONLY && version1 == 0) {
		printf("%s>>>>>>warnning..., can not support this hardware mode\n", __func__);
		return;
	}

	printf("%s>>>>>rk860_type = %d\n", __func__, di->rk860_type);

	switch (di->rk860_type) {
		case IS_RK860_0_ONLY:
			if (version0 == 0) {
				dm_i2c_addr_read(dev, 0x40, 0x0B, &buffer0[0], 1);
				dm_i2c_addr_read(dev, 0x40, 0x0C, &buffer0[1], 1);
				dm_i2c_addr_read(dev, 0x40, 0x0D, &buffer0[2], 1);
				value=0x5a;
				dm_i2c_addr_write(dev, 0x40, 0x0A, &value, 1);
				value=0x04;
				dm_i2c_addr_write(dev, 0x40, 0x0E, &value, 1);
				dm_i2c_addr_write(dev, 0x40, 0x0B, &buffer0[0], 1);
				dm_i2c_addr_write(dev, 0x40, 0x0C, &buffer0[1], 1);
				dm_i2c_addr_write(dev, 0x40, 0x0D, &buffer0[2], 1);

				printf("%s>>>>>> 0x0A = 0x%x,0x0B = 0x%x,0x0C = 0x%x,0x0D = 0x%x,0x0E = 0x%x\n",__func__, \
						pmic_reg_read(dev->parent, 0x0A), \
						pmic_reg_read(dev->parent, 0x0B), \
						pmic_reg_read(dev->parent, 0x0C), \
						pmic_reg_read(dev->parent, 0x0D), \
						pmic_reg_read(dev->parent, 0x0E)  \
						);
				printf("%s>>>>>>rk860-0 calibration okay.\n", __func__);
			}
			break;

		case IS_RK860_1_ONLY:
			if (version1 == 0) {
				dm_i2c_addr_read(dev, 0x41, 0x0B, &buffer1[0], 1);
				dm_i2c_addr_read(dev, 0x41, 0x0C, &buffer1[1], 1);
				dm_i2c_addr_read(dev, 0x41, 0x0D, &buffer1[2], 1);
				value=0x5a;
				dm_i2c_addr_write(dev, 0x41, 0x0A, &value, 1);
				value=0x44;
				dm_i2c_addr_write(dev, 0x40, 0x0E, &value, 1);
				dm_i2c_addr_write(dev, 0x41, 0x0B, &buffer1[0], 1);
				dm_i2c_addr_write(dev, 0x41, 0x0C, &buffer1[1], 1);
				dm_i2c_addr_write(dev, 0x41, 0x0D, &buffer1[2], 1);

				printf("%s>>>>>> 0x0A = 0x%x,0x0B = 0x%x,0x0C = 0x%x,0x0D = 0x%x,0x0E = 0x%x\n",__func__, \
						pmic_reg_read(dev->parent, 0x0A), \
						pmic_reg_read(dev->parent, 0x0B), \
						pmic_reg_read(dev->parent, 0x0C), \
						pmic_reg_read(dev->parent, 0x0D), \
						pmic_reg_read(dev->parent, 0x0E)  \
						);
				printf("%s>>>>>>rk860-1 calibration okay.\n", __func__);
			}
			break;

		case IS_RK860_0_1:
			if (version0 == 0 || version1 == 0) {
				dm_i2c_addr_read(dev, 0x40, 0x0B, &buffer0[0], 1);
				dm_i2c_addr_read(dev, 0x40, 0x0C, &buffer0[1], 1);
				dm_i2c_addr_read(dev, 0x40, 0x0D, &buffer0[2], 1);
				dm_i2c_addr_read(dev, 0x41, 0x0B, &buffer1[0], 1);
				dm_i2c_addr_read(dev, 0x41, 0x0C, &buffer1[1], 1);
				dm_i2c_addr_read(dev, 0x41, 0x0D, &buffer1[2], 1);
				value = 0x5a;
				dm_i2c_addr_write(dev, 0x40, 0x0A, &value, 1);
				value = 0x84;
				dm_i2c_addr_write(dev, 0x40, 0x0E, &value, 1);
				value = 0x5a;
				dm_i2c_addr_write(dev, 0x41, 0x0A, &value, 1);
				value = 0x44;
				dm_i2c_addr_write(dev, 0x40, 0x0E, &value, 1);
				dm_i2c_addr_write(dev, 0x41, 0x0B, &buffer1[0], 1);
				dm_i2c_addr_write(dev, 0x41, 0x0C, &buffer1[1], 1);
				dm_i2c_addr_write(dev, 0x41, 0x0D, &buffer1[2], 1);
				value = 0x04;
				dm_i2c_addr_write(dev, 0x42, 0x0E, &value, 1);
				dm_i2c_addr_write(dev, 0x40, 0x0B, &buffer0[0], 1);
				dm_i2c_addr_write(dev, 0x40, 0x0C, &buffer0[1], 1);
				dm_i2c_addr_write(dev, 0x40, 0x0D, &buffer0[2], 1);

				printf("%s>>>>>>rk860-0, rk860-1 calibration okay.\n", __func__);
			}
			break;

		default:
			printf("%s>>>>>>do nothing\n", __func__);
			break;
	}

	return;
}

static int fan53555_probe(struct udevice *dev)
{
	struct fan53555_priv *priv = dev_get_priv(dev);
	int ID1, ID2;

	debug("%s\n", __func__);

	/* read chip ID1 and ID2 (two registers, starting at ID1) */
	ID1 = pmic_reg_read(dev->parent, FAN53555_ID1);
	if (ID1 < 0)
		return ID1;

	ID2 = pmic_reg_read(dev->parent, FAN53555_ID2);
	if (ID2 < 0)
		return ID2;

	/* extract vendor, die_id and die_rev */
	priv->vendor = dev->driver_data;
	priv->die_id = ID1 & GENMASK(3, 0);
	priv->die_rev = ID2 & GENMASK(3, 0);

	if (fan53555_voltages_setup(dev) < 0)
		return -ENODATA;

	printf("%s: FAN53555 option %d rev %d detected\n",
	      __func__, priv->die_id, priv->die_rev);
	
        if (priv->vendor == FAN53555_VENDOR_SILERGY) {
		fan53555_rk860_calibration(dev, priv);
        }

	return 0;
}

static const struct dm_regulator_ops fan53555_regulator_ops = {
	.get_value	= fan53555_regulator_get_value,
	.set_value	= fan53555_regulator_set_value,
};

U_BOOT_DRIVER(fan53555_regulator) = {
	.name = "fan53555_regulator",
	.id = UCLASS_REGULATOR,
	.ops = &fan53555_regulator_ops,
	.of_to_plat = fan53555_regulator_of_to_plat,
	.plat_auto	= sizeof(struct fan53555_plat),
	.priv_auto	= sizeof(struct fan53555_priv),
	.probe = fan53555_probe,
};
