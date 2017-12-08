/****************************************************************
 ****************************************************************
 **                                                            **
 **    (C)Copyright 2006-2009, American Megatrends Inc.        **
 **                                                            **
 **            All Rights Reserved.                            **
 **                                                            **
 **        5555 Oakbrook Pkwy Suite 200, Norcross              **
 **                                                            **
 **        Georgia - 30093, USA. Phone-(770)-246-8600.         **
 **                                                            **
 ****************************************************************
 ****************************************************************/

/****************************************************************
 *
 * platform_ast2300evb.c
 * platform-specific initialization module for AST2300EVB
 *
 ****************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <asm/io.h>
#include <asm/uaccess.h>

#define AST_SCU_MULTI_PIN_I2C9			0x00400000 /* bit 22 */
#define AST_SCU_MULTI_PIN_I2C8			0x00200000 /* bit 21 */
#define AST_SCU_MULTI_PIN_I2C7			0x00100000 /* bit 20 */
#define AST_SCU_MULTI_PIN_I2C6			0x00080000 /* bit 19 */
#define AST_SCU_MULTI_PIN_I2C5			0x00040000 /* bit 18 */
#define AST_SCU_MULTI_PIN_I2C4			0x00020000 /* bit 17 */
#define AST_SCU_MULTI_PIN_I2C3			0x00010000 /* bit 16 */

#define AST_SCU_MULTI_PIN_MAC2_MD		0x00000004 /* bit 2 */

#define AST_SCU_MULTI_PIN_MAC1_MDIO		0x80000000 /* bit 31 */
#define AST_SCU_MULTI_PIN_MAC1_MDC		0x40000000 /* bit 30 */

#define AST_SCU_MULTI_PIN_NAND_FLWP		0x00000080 /* bit 7 */
#define AST_SCU_MULTI_PIN_NAND_FLBUSY	0x00000040 /* bit 6 */

#define AST_SCU_MULTI_PIN_SDHC2			0x00000002 /* bit 1 */
#define AST_SCU_MULTI_PIN_SDHC1			0x00000001 /* bit 0 */

int __init init_module(void)
{
	uint32_t reg;

	/* multi-function pin configuration */
	iowrite32(0x1688A8A8, SCU_KEY_CONTROL_REG); /* unlock SCU */

	reg = ioread32(AST_SCU_VA_BASE + 0x84);
	reg |= AST_SCU_MULTI_PIN_NAND_FLWP | AST_SCU_MULTI_PIN_NAND_FLBUSY;
	iowrite32(reg, AST_SCU_VA_BASE + 0x84);

	reg = ioread32(AST_SCU_VA_BASE + 0x88);
	reg |= AST_SCU_MULTI_PIN_MAC1_MDIO | AST_SCU_MULTI_PIN_MAC1_MDC;
	iowrite32(reg, AST_SCU_VA_BASE + 0x88);

	reg = ioread32(AST_SCU_VA_BASE + 0x90);
	reg |= AST_SCU_MULTI_PIN_SDHC1 | AST_SCU_MULTI_PIN_MAC2_MD | AST_SCU_MULTI_PIN_I2C3 | AST_SCU_MULTI_PIN_I2C4
			| AST_SCU_MULTI_PIN_I2C9 |  AST_SCU_MULTI_PIN_I2C8 | AST_SCU_MULTI_PIN_I2C7 | AST_SCU_MULTI_PIN_I2C6 | AST_SCU_MULTI_PIN_I2C5;
	iowrite32(reg, AST_SCU_VA_BASE + 0x90);

	iowrite32(0, SCU_KEY_CONTROL_REG); /* lock SCU */

	/* GPIO value and direction initialization */
	iowrite32(0, GPIO_DATA_REG);
	iowrite32(0, GPIO_DIRECTION_REG);
	iowrite32(0, GPIO_DATA_REG + 0x20);
	iowrite32(0, GPIO_DIRECTION_REG + 0x20);

	return 0;
}

void __exit cleanup_module(void)
{
	/* do nothing */
}

MODULE_AUTHOR("American Megatrends Inc.");
MODULE_DESCRIPTION("platform-specific initialization module for AST2300EVB");
MODULE_LICENSE("GPL");
