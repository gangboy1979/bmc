--- linux/drivers/net/arm/ast_ether.c	1970-01-01 08:00:00.000000000 +0800
+++ linux.new/drivers/net/arm/ast_ether.c	2011-08-17 17:46:42.039042000 +0800
@@ -0,0 +1,1119 @@
+/*
+ * drivers/net/ast_ether.c - AST2050/AST2100/AST2150/AST2200 ethernet MAC driver
+ */
+
+#include <linux/module.h>
+#include <linux/version.h>
+#include <linux/kernel.h>
+#include <linux/sched.h>
+#include <linux/types.h>
+#include <linux/fcntl.h>
+#include <linux/interrupt.h>
+#include <linux/ptrace.h>
+#include <linux/ioport.h>
+#include <linux/in.h>
+#include <linux/slab.h>
+#include <linux/string.h>
+#include <linux/init.h>
+#include <linux/proc_fs.h>
+#include <asm/bitops.h>
+#include <asm/io.h>
+#include <linux/dma-mapping.h>
+#include <linux/errno.h>
+#include <linux/delay.h>
+#include <linux/netdevice.h>
+#include <linux/etherdevice.h>
+#include <linux/skbuff.h>
+#include <linux/crc32.h>
+#include <linux/ethtool.h>
+#include <linux/mii.h>
+#include <linux/rtnetlink.h>
+
+#include "ast_ether.h"
+
+const unsigned long ast_ether_phys_base[AST_ETHER_MAX_NUM] = {AST_ETHER_REG_BASE1, AST_ETHER_REG_BASE2};
+const unsigned long ast_ether_irq[AST_ETHER_MAX_NUM] = {AST_ETHER_IRQ1, AST_ETHER_IRQ2};
+
+static struct net_device *ast_ether_netdev[AST_ETHER_MAX_NUM];
+
+extern unsigned long enetaddr[][6];
+static void ast_ether_get_hardware_addr(struct net_device* dev)
+{
+	memcpy(dev->dev_addr, enetaddr[dev->dev_id], ETH_ALEN);
+}
+
+static void ast_ether_phy_rw_waiting(unsigned int ioaddr)
+{
+	int count;
+
+	count = 0;
+	while (ioread32(ioaddr + AST_ETHER_PHYCR) & (PHYCR_READ | PHYCR_WRITE)) {
+		mdelay(10);
+		count ++;
+		if (count > 100)
+			break;
+	}
+}
+
+/* Reads a register from the MII Management serial interface */
+static u16 ast_ether_phy_read_register(struct net_device *dev, u8 phyaddr, u8 phyreg)
+{
+	unsigned int ioaddr = dev->base_addr;
+	u32 reg_val;
+	
+	reg_val = ioread32(ioaddr + AST_ETHER_PHYCR);
+
+	reg_val &= ~(PHYCR_WRITE | PHYCR_READ | PHYCR_REGAD_MASK | PHYCR_PHYAD_MASK); /* clear PHYAD, REGAD, MIIRD, MIIWR */
+	reg_val |= (phyaddr << PHYCR_PHYAD_SHIFT) & PHYCR_PHYAD_MASK;
+	reg_val |= (phyreg << PHYCR_REGAD_SHIFT) & PHYCR_REGAD_MASK;
+	reg_val |= PHYCR_READ;
+	
+	iowrite32(reg_val, ioaddr + AST_ETHER_PHYCR);
+	
+	ast_ether_phy_rw_waiting(ioaddr);
+
+	return ((ioread32(ioaddr + AST_ETHER_PHYDATA) >> 16));
+}
+
+/* Writes a register to the MII Management serial interface */
+static void ast_ether_phy_write_register(struct net_device *dev, u8 phyaddr, u8 phyreg, u16 phydata)
+{
+	unsigned int ioaddr = dev->base_addr;
+	u32 reg_val;
+
+	reg_val = ioread32(ioaddr + AST_ETHER_PHYCR);
+
+	reg_val &= ~(PHYCR_WRITE | PHYCR_READ | PHYCR_REGAD_MASK | PHYCR_PHYAD_MASK); /* clear PHYAD, REGAD, MIIRD, MIIWR */
+	reg_val |= (phyaddr << PHYCR_PHYAD_SHIFT) & PHYCR_PHYAD_MASK;
+	reg_val |= (phyreg << PHYCR_REGAD_SHIFT) & PHYCR_REGAD_MASK;
+	reg_val |= PHYCR_WRITE;
+
+	iowrite32(phydata, ioaddr + AST_ETHER_PHYDATA);
+	iowrite32(reg_val, ioaddr + AST_ETHER_PHYCR);
+
+	ast_ether_phy_rw_waiting(ioaddr);
+}
+
+/* read OUI and Model of PHY */
+static void ast_ether_read_phy_id(struct net_device* dev)
+{
+	struct ast_ether_priv *priv = netdev_priv(dev);
+	u32 reg_val;
+
+	/* OUI[23:22] are not represented in PHY ID registers */
+	reg_val = ast_ether_phy_read_register(dev, 0, PHY_COM_REG_ADDR_ID_HI);
+	priv->phy_oui = (reg_val & 0xFFFF) << 6; /* OUI[21:6] in ID_HI[15:0] */
+
+	reg_val = ast_ether_phy_read_register(dev, 0, PHY_COM_REG_ADDR_ID_LO);
+	priv->phy_oui |= (reg_val & 0xFFFF) >> 10; /* OUI[5:0] in ID_LO[15:10] */
+
+	priv->phy_model = (reg_val & 0x03F0) >> 4; /* MODEL[5:0] in ID_LO[9:4] */
+
+	switch (priv->phy_oui) {
+	case MARVELL_88E1111_OUI:
+	case BROADCOM_BCM5221_OUI:
+	case BROADCOM_BCM5241_OUI:
+	case MICREL_KSZ8041_OUI:
+	case REALTEK_RTL82X1X_OUI:
+		break;
+	default:
+		priv->phy_oui = 0; /* not supported PHY or NC-SI */
+		break;
+	}
+}
+
+/**
+ * ast_mii_check_gmii_support (origin from mii_check_gmii_support) - check if the MII supports Gb interfaces 
+ * @dev: the net interface
+ */  
+int ast_mii_check_gmii_support(struct net_device *dev)                                                                                                     
+{        
+    uint32_t reg = 0;
+    reg = ast_ether_phy_read_register(dev, 0, MII_BMSR);
+
+    if (reg & BMSR_ESTATEN) {
+        reg = ast_ether_phy_read_register(dev, 0, MII_ESTATUS);
+        if (reg & (ESTATUS_1000_TFULL | ESTATUS_1000_THALF))
+            return 1; 
+    }
+
+    return 0;
+}  
+
+/* Configures the PHY to restart Auto-Negotiation. */
+void ast_ether_phy_restart_auto_neg(struct net_device* dev)
+{
+	u16 reg_val;
+
+	/* restart auto-negotiation */
+	reg_val = ast_ether_phy_read_register(dev, 0, PHY_COM_REG_ADDR_CONTROL);
+	reg_val |= PHY_COM_REG_CONTROL_RE_AUTO;
+	ast_ether_phy_write_register(dev, 0, PHY_COM_REG_ADDR_CONTROL, reg_val);
+
+	/* Waiting for complete Auto-Negotiation */
+	while ((ast_ether_phy_read_register(dev, 0, PHY_COM_REG_ADDR_STATUS) & PHY_COM_REG_STATUS_AUTO_OK) == 0)
+		mdelay(2);
+}
+
+static void ast_ether_set_link_status(struct net_device* dev)
+{
+	struct ast_ether_priv *priv = netdev_priv(dev);
+	unsigned int ioaddr = dev->base_addr;
+	unsigned int reg_val, duplex, speed;
+
+	if (priv->phy_oui != 0) {
+		reg_val = ast_ether_phy_read_register(dev, 0, PHY_COM_REG_ADDR_STATUS);
+		if (reg_val & PHY_COM_REG_STATUS_LINK) {
+            if (rtnl_trylock())
+            {
+				if(priv->link_state != 1)
+				{
+					netdev_link_up(dev);
+					priv->link_state = 1;
+				}
+                rtnl_unlock();
+            }
+            else
+            {
+				if(priv->link_state != 1)
+				{
+					netdev_link_up(dev);
+					priv->link_state = 1;
+				}
+            }
+			/* read speed and duplex from PHY */
+			switch (priv->phy_oui) {
+			case MARVELL_88E1111_OUI:
+				reg_val = ast_ether_phy_read_register(dev, 0, MARVELL_88E1111_REG_ADDR_PHY_STATUS);
+				duplex = (reg_val & MARVELL_88E1111_REG_PHY_STATUS_DUPLEX_MASK) >> 13;
+				speed = (reg_val & MARVELL_88E1111_REG_PHY_STATUS_SPEED_MASK) >> 14;
+				break;
+			case BROADCOM_BCM5221_OUI:
+			case BROADCOM_BCM5241_OUI:
+				reg_val = ast_ether_phy_read_register(dev, 0, BROADCOM_BCM5221_REG_ADDR_AUX_CTRL_STAT);
+				duplex = (reg_val & BROADCOM_BCM5221_REG_AUX_CTRL_STAT_DEPLEX) ? PHY_DUPLEX_FULL : PHY_DUPLEX_HALF;
+				speed = (reg_val & BROADCOM_BCM5221_REG_AUX_CTRL_STAT_SPEED) ? PHY_SPEED_100M : PHY_SPEED_10M;
+				break;
+			case REALTEK_RTL82X1X_OUI:
+				switch (priv->phy_model) {
+				case 0x01:
+					reg_val = ast_ether_phy_read_register(dev, 0, PHY_COM_REG_ADDR_CONTROL);
+					duplex = (reg_val & PHY_COM_REG_CONTROL_DUPLEX) ? PHY_DUPLEX_FULL : PHY_DUPLEX_HALF;
+					speed = (reg_val & PHY_COM_REG_CONTROL_SPEED) ? PHY_SPEED_100M : PHY_SPEED_10M;
+					break;
+				case 0x11:
+					reg_val = ast_ether_phy_read_register(dev, 0, REALTEK_RTL8211B_REG_ADDR_PHY_STAT);
+					duplex = (reg_val & REALTEK_RTL8211B_REG_ADDR_PHY_STS_DEPLEX) ? PHY_DUPLEX_FULL : PHY_DUPLEX_HALF;
+					speed = (reg_val & REALTEK_RTL8211B_REG_ADDR_PHY_STS_SPEED_MASK) >> REALTEK_RTL8211B_REG_ADDR_PHY_STS_SPEED_SHIFT;
+					break;
+				default:
+					speed = PHY_SPEED_100M;
+					duplex = 1;
+				}
+				break;
+			case MICREL_KSZ8041_OUI:
+				reg_val = ast_ether_phy_read_register(dev, 0, MICREL_KSZ8041_REG_STATUS);
+				switch ((reg_val & MICREL_KSZ8041_REG_STATUS_MASK) >> MICREL_KSZ8041_REG_STATUS_SHIFT) {
+				case MICREL_KSZ8041_REG_STATUS_HALF_10:
+					duplex = PHY_DUPLEX_HALF;
+					speed = PHY_SPEED_10M;
+					break;
+				case MICREL_KSZ8041_REG_STATUS_HALF_100:
+					duplex = PHY_DUPLEX_HALF;
+					speed = PHY_SPEED_100M;
+					break;
+				case MICREL_KSZ8041_REG_STATUS_FULL_10:
+					duplex = PHY_DUPLEX_FULL;
+					speed = PHY_SPEED_10M;
+					break;
+				case MICREL_KSZ8041_REG_STATUS_FULL_100:
+					duplex = PHY_DUPLEX_FULL;
+					speed = PHY_SPEED_100M;
+					break;
+				default:
+					duplex = PHY_DUPLEX_FULL;
+					speed = PHY_SPEED_100M;
+					break;
+				}
+				break;
+			default:
+				speed = PHY_SPEED_100M;
+				duplex = 1;
+				break;
+			}
+			netif_carrier_on(dev);
+		} else { /* link is down */
+            if (rtnl_trylock())
+            {
+				if(priv->link_state != 0)
+				{
+					netdev_link_down(dev);
+					priv->link_state = 0;
+				}
+                rtnl_unlock();
+            }
+            else
+            {
+				if(priv->link_state != 0)
+				{
+					netdev_link_down(dev);
+					priv->link_state = 0;
+				}
+            }
+			/* default setting: 100M, duplex */
+			speed = PHY_SPEED_100M;
+			duplex = 1;
+			netif_carrier_off(dev);
+		}
+	} else { /* not support PHY of NC-SI */
+		/* default setting: 100M, duplex */
+		speed = PHY_SPEED_100M;
+		duplex = 1;
+		netif_carrier_on(dev);
+	}
+
+	reg_val = ioread32(ioaddr + AST_ETHER_MACCR);
+
+	/* set speed */
+	if (speed == PHY_SPEED_1000M) { /* 1000 Mbps */
+		priv->maccr |= (MACCR_GMAC_MODE | MACCR_SPEED_100);
+		reg_val |= (MACCR_GMAC_MODE | MACCR_SPEED_100);
+	} else { /* 10/100 Mbps */
+		reg_val &= ~(MACCR_GMAC_MODE | MACCR_SPEED_100);
+		priv->maccr &= ~(MACCR_GMAC_MODE | MACCR_SPEED_100);
+
+		if (speed == PHY_SPEED_100M) {
+			reg_val |= MACCR_SPEED_100;
+			priv->maccr |= MACCR_SPEED_100;
+		}
+	}
+
+	/* set duplex */
+	if (duplex == PHY_DUPLEX_FULL) {
+		priv->maccr |= MACCR_FULLDUP;
+		reg_val |= MACCR_FULLDUP;
+	} else {
+		priv->maccr &= ~MACCR_FULLDUP;
+		reg_val &= ~MACCR_FULLDUP;
+	}
+
+	iowrite32(reg_val, ioaddr + AST_ETHER_MACCR);
+}
+
+static void ast_ether_phy_status_checker(unsigned long data)
+{
+	struct net_device *dev;
+	struct ast_ether_priv *priv;
+
+	dev = (struct net_device *) data;
+	priv = netdev_priv(dev);
+
+	ast_ether_set_link_status(dev);
+
+	priv->timer.expires = jiffies + (AST_PHY_STATUS_CHECK_INTERVAL * HZ);
+	add_timer(&priv->timer);
+}
+
+/* allocates memory for Tx/Rx descriptors, we use ring structure(AST2100 does not support chain structure) */
+static void ast_ether_alloc_descriptor(struct net_device *dev)
+{
+	struct ast_ether_priv *priv = netdev_priv(dev);
+
+	/* allocate Rx descriptors */
+	priv->rx_desc = dma_alloc_coherent(NULL, sizeof(struct ast_eth_desc) * RX_DES_NUM, &priv->rx_desc_dma, GFP_KERNEL);
+	if (priv->rx_desc == NULL) {
+		printk(KERN_ERR "%s: allocate Rx descriptor failed.\n", dev->name);
+	}
+	memset(priv->rx_desc, 0, sizeof(struct ast_eth_desc) * RX_DES_NUM);
+
+	/* allocate Tx descriptors */
+	priv->tx_desc = dma_alloc_coherent(NULL, sizeof(struct ast_eth_desc) * TX_DES_NUM, &priv->tx_desc_dma, GFP_KERNEL);
+	if (priv->tx_desc == NULL) {
+		printk(KERN_ERR "%s: allocate Tx descriptor failed.\n", dev->name);
+	}
+	memset(priv->tx_desc, 0, sizeof(struct ast_eth_desc) * TX_DES_NUM);
+}
+
+static void ast_ether_init_descriptor(struct net_device *dev)
+{
+	struct ast_ether_priv *priv = netdev_priv(dev);
+	struct sk_buff *skb;
+	int i;
+
+	/* initializes Rx descriptors */
+	priv->cur_rx = 0;
+	priv->dirty_rx = 0;
+	for (i = 0; i < RX_DES_NUM; i ++) {
+		priv->rx_desc[i].status &= ~RX_DESC_RXPKT_RDY; /* haredware owns descriptor */
+		priv->rx_skbuff[i] = NULL;
+	}
+
+	/* allocate Rx buffers */
+	for (i = 0; i < RX_DES_NUM; i ++) {
+		skb = dev_alloc_skb(RX_BUF_SIZE + 16);
+		priv->rx_skbuff[i] = skb;
+		if (skb == NULL) {
+			printk(KERN_ERR "%s: allocate Rx buffer failed.\n", dev->name);
+			break;
+		}
+		skb->dev = dev; /* Mark as being used by this device. */
+		skb_reserve(skb, NET_IP_ALIGN); /* 16 byte align the IP header. */
+
+		priv->rx_skb_dma[i] = dma_map_single(NULL, skb->data, RX_BUF_SIZE, DMA_FROM_DEVICE);
+		priv->rx_desc[i].buffer = priv->rx_skb_dma[i];
+		priv->rx_desc[i].status = 0;
+	}
+	priv->rx_desc[RX_DES_NUM - 1].status |= RX_DESC_EDORR; /* last descriptor */
+
+	/* initializes Tx descriptors */
+	priv->cur_tx = 0;
+	priv->dirty_tx = 0;
+	for (i = 0; i < TX_DES_NUM; i ++) {
+		priv->tx_desc[i].status &= ~TX_DESC_TXDMA_OWN; /* software owns descriptor */
+		priv->tx_skbuff[i] = NULL;
+		priv->tx_skb_dma[i] = 0;
+	}
+	priv->tx_desc[TX_DES_NUM - 1].status |= TX_DESC_EDOTR; /* last descriptor */
+}
+
+static void ast_ether_up(struct net_device *dev)
+{
+	struct ast_ether_priv *priv = netdev_priv(dev);
+	unsigned int ioaddr = dev->base_addr;
+	unsigned int reg_value;
+	unsigned int rfifo_rsize;
+	unsigned int tfifo_rsize;
+	u32 hardware_addr;
+	int i;
+
+	/* software reset */
+	iowrite32(MACCR_SW_RST, ioaddr + AST_ETHER_MACCR);
+
+	/* wait SW_RST bit to be auto cleared */
+	while ((ioread32(ioaddr + AST_ETHER_MACCR) & MACCR_SW_RST) != 0)
+		mdelay(10);
+
+	iowrite32(0, ioaddr + AST_ETHER_IER); /* Disable all interrupts */
+
+	/* reset Rx descriptors */
+	priv->cur_rx = 0;
+	priv->dirty_rx = 0;
+	for (i = 0; i < RX_DES_NUM; i ++) {
+		priv->rx_desc[i].status &= ~RX_DESC_RXPKT_RDY; /* haredware owns descriptor */
+	}
+
+	/* reset Tx descriptors */
+	priv->cur_tx = 0;
+	priv->dirty_tx = 0;
+	for (i = 0; i < TX_DES_NUM; i ++) {
+		priv->tx_desc[i].status &= ~TX_DESC_TXDMA_OWN; /* software owns descriptor */
+		priv->tx_skbuff[i] = NULL;
+		priv->tx_skb_dma[i] = 0;
+	}
+
+	/* set the MAC address */
+	hardware_addr = ((u32)dev->dev_addr[0]) << 8 | (u32)dev->dev_addr[1];
+	iowrite32(hardware_addr, ioaddr + AST_ETHER_MAC_MADR);
+
+	hardware_addr = 0;
+	for (i = 0; i < 4; i ++)
+		hardware_addr |= ((u32)(dev->dev_addr[i + 2])) << (8 * (4 - 1 - i));
+	iowrite32(hardware_addr, ioaddr + AST_ETHER_MAC_LADR);
+
+	/* hardware works with bus address */
+	iowrite32(priv->rx_desc_dma, ioaddr + AST_ETHER_RXR_BADR);
+	iowrite32(priv->tx_desc_dma, ioaddr + AST_ETHER_TXR_BADR);
+
+	/* set interrupt timer */
+	reg_value = (((1 << ITCR_TXINT_THR_SHIFT) & ITCR_TXINT_THR_MASK) |
+				((0 << ITCR_TXINT_CNT_SHIFT) & ITCR_TXINT_CNT_MASK) |
+				((1 << ITCR_RXINT_THR_SHIFT) & ITCR_RXINT_THR_MASK) |
+				((0 << ITCR_RXINT_CNT_SHIFT) & ITCR_RXINT_CNT_MASK));
+	iowrite32(reg_value, ioaddr + AST_ETHER_ITC);
+
+	/* set automatic polling timer */
+	reg_value = (((0 << APTC_TXPOLL_CNT_SHIFT) & APTC_TXPOLL_CNT_MASK) | 
+				((1 << APTC_RXPOLL_CNT_SHIFT) & APTC_RXPOLL_CNT_MASK));
+	iowrite32(reg_value, ioaddr + AST_ETHER_APTC);
+	
+	/* set Tx/Rx descriptor size, max size of DMA burst, Rx FIOF hish/low threshold */
+	iowrite32(0x00022F72, ioaddr + AST_ETHER_DBLAC);
+
+	/* read hardware FIFO size */
+	reg_value = ioread32(ioaddr + AST_ETHER_FEAR);
+	rfifo_rsize = (reg_value & FEAR_RFIFO_RSIZE_MASK) >> FEAR_RFIFO_RSIZE_SHIFT;
+	tfifo_rsize = (reg_value & FEAR_TFIFO_RSIZE_MASK) >> FEAR_TFIFO_RSIZE_SHIFT;
+
+	/* set Tx/Rx FIFO size and Tx priority threshold */
+	reg_value = ioread32(ioaddr + AST_ETHER_TPAFCR);
+	reg_value &= ~(FPAFCR_TFIFO_SIZE_MASK | FPAFCR_RFIFO_SIZE_MASK);
+	reg_value |= (tfifo_rsize << FPAFCR_TFIFO_SIZE_SHIFT);
+	reg_value |= (rfifo_rsize << FPAFCR_RFIFO_SIZE_SHIFT);
+	iowrite32(reg_value, ioaddr + AST_ETHER_TPAFCR);
+
+	/* set rx buffer size */
+	iowrite32(RX_BUF_SIZE & RBSR_MASK, ioaddr + AST_ETHER_RBSR);
+
+	/* enable interrupts */
+	reg_value = ISR_AHB_ERR | ISR_TPKT_LOST | ISR_TPKT2E | ISR_RPKT_LOST | ISR_RXBUF_UNAVA | ISR_RPKT2B;
+	if (priv->phy_oui != 0) {
+		init_timer(&priv->timer);
+		priv->timer.data = (unsigned long) dev;
+		priv->timer.function = ast_ether_phy_status_checker;
+		priv->timer.expires = jiffies + (AST_PHY_STATUS_CHECK_INTERVAL * HZ);
+		add_timer(&priv->timer);
+	}
+	iowrite32(reg_value, ioaddr + AST_ETHER_IER);
+
+	/* enable trans/recv, ... */
+	iowrite32(priv->maccr, ioaddr + AST_ETHER_MACCR);
+}
+
+static void ast_ether_down(unsigned int ioaddr)
+{
+	iowrite32(0, ioaddr + AST_ETHER_IER); /* Disables all interrupts */
+	iowrite32(0, ioaddr + AST_ETHER_MACCR); /* Disables trans/recv, ... */
+}
+
+static int ast_ether_tx(struct sk_buff *skb, struct net_device *dev)
+{
+	struct ast_ether_priv *priv = netdev_priv(dev);
+	volatile struct ast_eth_desc *cur_desc;
+	int entry;
+	int length;
+	unsigned long flags;
+
+	spin_lock_irqsave(&priv->lock, flags);
+
+	/* Note: the chip doesn't have auto-pad! */
+	length = skb->len;
+	if (length < ETH_ZLEN) {
+		if (skb_padto(skb, ETH_ZLEN) < 0) {
+			priv->stats.tx_dropped ++;
+			spin_unlock_irqrestore(&priv->lock, flags);
+			return 0;
+		}
+		length = ETH_ZLEN;
+	}
+
+	entry = priv->cur_tx & (TX_DES_NUM - 1);
+	cur_desc = &priv->tx_desc[entry];
+
+	if (cur_desc->status & TX_DESC_TXDMA_OWN) {
+		netif_stop_queue(dev);
+		priv->stats.tx_dropped ++;
+		spin_unlock_irqrestore(&priv->lock, flags);
+		return 1;
+	}
+
+	priv->tx_skbuff[entry] = skb;
+	priv->tx_skb_dma[entry] = dma_map_single(NULL, skb->data, length, DMA_TO_DEVICE);
+
+	cur_desc->buffer = priv->tx_skb_dma[entry];
+	cur_desc->status &= ~(TX_DESC_TXBUF_SIZE | TX_DESC_CRC_ERR);
+	cur_desc->status |= (length | TX_DESC_LTS | TX_DESC_FTS | TX_DESC_TXDMA_OWN);
+
+	dev->trans_start = jiffies;
+	priv->cur_tx ++;
+
+	wmb();
+	iowrite32(0xffffffff, dev->base_addr + AST_ETHER_TXPD); /* Trigger an Tx Poll demand */
+
+	if (priv->cur_tx == (priv->dirty_tx + TX_DES_NUM)) { /* Tx ring is full */
+		netif_stop_queue(dev);
+	}
+
+	spin_unlock_irqrestore(&priv->lock, flags);
+
+	return 0;
+}
+
+static void ast_ether_free_tx_buf(struct net_device *dev)
+{
+	struct ast_ether_priv *priv = netdev_priv(dev);
+	int entry;
+	unsigned long flags;
+
+	spin_lock_irqsave(&priv->lock, flags);
+
+	/* Free used tx sk_buffs */
+	while (priv->dirty_tx != priv->cur_tx) {
+		entry = priv->dirty_tx & (TX_DES_NUM - 1);
+		if (priv->tx_desc[entry].status & TX_DESC_TXDMA_OWN)
+			break;
+		priv->stats.tx_bytes += priv->tx_skbuff[entry]->len;
+		dma_unmap_single(NULL, priv->tx_skb_dma[entry], priv->tx_skbuff[entry]->len, DMA_TO_DEVICE);
+		dev_kfree_skb_irq(priv->tx_skbuff[entry]);
+		priv->tx_skbuff[entry] = NULL;
+		priv->tx_skb_dma[entry] = 0;
+		priv->dirty_tx ++;
+	}
+
+	if (netif_queue_stopped(dev) && (priv->cur_tx != (priv->dirty_tx + TX_DES_NUM)))
+		netif_wake_queue(dev);
+
+	spin_unlock_irqrestore(&priv->lock, flags);
+}
+
+static void ast_ether_realloc_rx_buf(struct net_device *dev)
+{
+	struct ast_ether_priv *priv = netdev_priv(dev);
+	struct sk_buff *skb;
+	int entry;
+
+	/* Reallocate the Rx ring buffers. */
+	while (priv->dirty_rx != priv->cur_rx) {
+		entry = priv->dirty_rx & (RX_DES_NUM - 1);
+		if (priv->rx_skbuff[entry] == NULL) {
+			skb = dev_alloc_skb(RX_BUF_SIZE + 16);
+			priv->rx_skbuff[entry] = skb;
+			if (skb == NULL) {
+				printk(KERN_ERR "%s: reallocate Rx buffer failed.\n", dev->name);
+				break;
+			}
+
+			skb->dev = dev; /* Mark as being used by this device. */
+			skb_reserve(skb, NET_IP_ALIGN); /* 16 byte align the IP header. */
+
+			priv->rx_skb_dma[entry] = dma_map_single(NULL, skb->data, RX_BUF_SIZE, DMA_FROM_DEVICE);
+			priv->rx_desc[entry].buffer = priv->rx_skb_dma[entry];
+			wmb();
+			priv->rx_desc[entry].status &= RX_DESC_EDORR; /* clear all field except EDORR, return ownership to hardware */
+		}
+		priv->dirty_rx ++;
+	}
+}
+
+static inline void ast_ether_rx_csum(struct sk_buff *skb, struct ast_eth_desc *desc)
+{
+	u32 status = desc->vlan;
+	u32 protocol_type = status & RX_DESC_PROTL_TYPE_MASK;
+
+	if (((protocol_type == RX_DESC_PROTL_TYPE_IP) && !(status & RX_DESC_IPCS_FAIL)) ||
+		((protocol_type == RX_DESC_PROTL_TYPE_UDP) && !(status & RX_DESC_UDPCS_FAIL)) ||
+		((protocol_type == RX_DESC_PROTL_TYPE_TCP) && !(status & RX_DESC_TCPCS_FAIL)))
+		skb->ip_summed = CHECKSUM_UNNECESSARY;
+	else
+		skb->ip_summed = CHECKSUM_NONE;
+}
+
+static void ast_ether_rx(struct net_device *dev)
+{
+	struct ast_ether_priv *priv = netdev_priv(dev);
+	struct sk_buff *skb;
+	u32 status;
+	int pkt_len;
+	int has_error;
+	int has_first;
+	int entry;
+
+	has_error = 0;
+	has_first = 0;
+
+	while ((priv->dirty_rx + RX_DES_NUM) != priv->cur_rx) {
+		entry = priv->cur_rx & (RX_DES_NUM - 1);
+		status = priv->rx_desc[entry].status;
+		if (!(status & RX_DESC_RXPKT_RDY)) /* owned by hardware */
+            break;
+
+		if (status & RX_DESC_FRS) { /* first frame of packet, check error */
+			has_first = 1;
+
+			if (status & (RX_DESC_CRC_ERR | RX_DESC_FTL | RX_DESC_RUNT | RX_DESC_RX_ODD_NB | RX_DESC_FIFO_FULL)) { /* There was an error. */
+				priv->stats.rx_errors ++;
+				has_error = 1;
+			}
+
+			if (status & RX_DESC_MULTICAST)
+				priv->stats.multicast ++;
+		}
+
+		skb = priv->rx_skbuff[entry];
+		dma_unmap_single(NULL, priv->rx_skb_dma[entry], RX_BUF_SIZE, DMA_FROM_DEVICE);
+		priv->rx_skbuff[entry] = NULL;
+		priv->rx_skb_dma[entry] = 0;
+
+		if (has_error) {
+			if (status & RX_DESC_CRC_ERR)
+				priv->stats.rx_crc_errors ++;
+			if (status & RX_DESC_FIFO_FULL)
+				priv->stats.rx_fifo_errors ++;
+		} else {
+			pkt_len = (status & RX_DESC_VDBC) - 4; /* Omit the four octet CRC from the length. */
+			if (pkt_len > 1518) {
+				printk(KERN_WARNING "%s: Bogus packet size of %d (%#x).\n", dev->name, pkt_len, pkt_len);
+				pkt_len = 1518;
+				priv->stats.rx_length_errors ++;
+			}
+
+			ast_ether_rx_csum(skb, &priv->rx_desc[entry]);
+			skb_put(skb, pkt_len); /* reflect the DMA operation result */
+			skb->protocol = eth_type_trans(skb, dev);
+
+			netif_rx(skb);
+	
+			dev->last_rx = jiffies;
+			priv->stats.rx_packets ++;
+			priv->stats.rx_bytes += pkt_len;
+		}
+
+		priv->cur_rx ++;
+	}
+}
+
+static irqreturn_t ast_ether_irq_handler(int irq, void *dev_id)
+{
+	struct net_device *dev;
+	struct ast_ether_priv *priv;
+	unsigned int ioaddr;
+	unsigned int intr_mask;
+	unsigned int status;
+
+	dev = (struct net_device *)dev_id;
+	priv = netdev_priv(dev);
+	ioaddr = dev->base_addr;
+
+	intr_mask = ioread32(ioaddr + AST_ETHER_IER);
+	status = ioread32(ioaddr + AST_ETHER_ISR) & intr_mask;
+	iowrite32(status, ioaddr + AST_ETHER_ISR); /* write to clear */
+
+	if (status & ISR_PHYSTS_CHG) { /* PHY status change */
+		printk("ISR_PHYSTS_CHG\n");
+	}
+
+	if (status & ISR_RPKT2B) { /* Rx completed */
+		ast_ether_rx(dev);
+		ast_ether_realloc_rx_buf(dev);
+	}
+
+	if (status & ISR_RXBUF_UNAVA)
+		printk("ISR_RXBUF_UNAVA\n");
+
+	if (status & ISR_RPKT_LOST)
+		printk("ISR_RPKT_LOST\n");
+
+	if (status & ISR_TPKT2E) { /* Tx completed */
+		priv->stats.tx_packets ++;
+		ast_ether_free_tx_buf(dev);
+	}
+
+	if (status & ISR_TPKT_LOST)
+		printk("ISR_TPKT_LOST\n");
+
+	if (status & ISR_AHB_ERR)
+		printk("AHB error\n");
+
+	return IRQ_HANDLED;
+}
+
+/* The kernel calls this function when someone wants to use the net_device, typically "ifconfig eth%d up". */
+static int ast_ether_open(struct net_device *dev)
+{
+	int err;
+	int dev_id;
+	struct ast_ether_priv *priv;
+
+	dev_id = dev->dev_id;
+
+	err = request_irq(dev->irq, ast_ether_irq_handler, IRQF_DISABLED, dev->name, dev);
+	if (err != 0) {
+		return err;
+    }
+
+	ast_ether_read_phy_id(dev);
+	ast_ether_set_link_status(dev);
+
+	ast_ether_init_descriptor(dev);
+	ast_ether_up(dev);
+
+	netif_start_queue(dev);
+
+
+    priv = netdev_priv(dev);
+    priv->supports_gmii = ast_mii_check_gmii_support(dev);
+	return 0;
+}
+
+static int ast_ether_close(struct net_device *dev)
+{
+	int dev_id;
+	struct ast_ether_priv *priv;
+
+	dev_id = dev->dev_id;
+	priv = netdev_priv(dev);
+
+	netif_stop_queue(dev);
+
+	ast_ether_down(dev->base_addr);
+
+	if (priv->timer.function != NULL)
+	    del_timer_sync(&priv->timer);
+
+	free_irq(dev->irq, dev);
+
+	return 0;
+}
+
+/* Called by the kernel to send a packet out into the void of the net. */
+static void ast_ether_timeout(struct net_device *dev)
+{
+	/* If we get here, some higher level has decided we are broken.
+	There should really be a "kick me" function call instead. */
+	printk(KERN_WARNING "%s: transmit timed out\n", dev->name);
+
+	/* "kick" the adaptor */
+	ast_ether_up(dev);
+
+	netif_wake_queue(dev);
+	dev->trans_start = jiffies;
+}
+
+static struct net_device_stats* ast_ether_get_stats(struct net_device *dev)
+{
+	struct ast_ether_priv *priv = netdev_priv(dev);
+
+	return &priv->stats;
+}
+
+static void ast_ether_set_multicast_list(struct net_device *dev)
+{
+	unsigned int ioaddr = dev->base_addr;
+	struct ast_ether_priv *priv = netdev_priv(dev);
+	struct dev_mc_list *cur_addr;
+	u32 bit_nr;
+	u32 hash_table[2];
+
+	priv->maccr &= ~(MACCR_RX_ALLADR | MACCR_RX_MULTIPKT_EN | MACCR_RX_MULTIPKT_EN);
+
+	if (dev->flags & IFF_PROMISC) {
+		priv->maccr |= MACCR_RX_ALLADR;
+	} else if (dev->flags & IFF_ALLMULTI) {
+		priv->maccr |= MACCR_RX_MULTIPKT_EN;
+	} else {
+		hash_table[1] = hash_table[0] = 0;
+		if (dev->mc_list && dev->flags & IFF_MULTICAST) {
+			priv->maccr |= MACCR_RX_HT_EN;
+
+			for (cur_addr = dev->mc_list; cur_addr != NULL; cur_addr = cur_addr->next) {
+				/* use invert of bits[7:2] of ethernet crc(little endian) result as hash table index */
+				bit_nr = (~ether_crc_le(ETH_ALEN, cur_addr->dmi_addr) & 0x000000fc) >> 2;
+				hash_table[bit_nr >> 5] |= 1 << (bit_nr & 0x1f);
+			}
+		}
+
+		/* set the hash table to filter out unwanted multicast packets before they take up memory. */
+		iowrite32(hash_table[0], ioaddr + AST_ETHER_MAHT0);
+		iowrite32(hash_table[1], ioaddr + AST_ETHER_MAHT1);
+	}
+
+	iowrite32(priv->maccr, ioaddr + AST_ETHER_MACCR);
+}
+
+static int ast_ether_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
+{
+    struct ast_ether_priv *priv = netdev_priv(dev);
+	uint32_t advert, bmcr, lpa, nego;
+	uint32_t advert2 = 0, bmcr2 = 0, lpa2 = 0;
+	
+	cmd->phy_address = 0;
+	cmd->supported = (SUPPORTED_10baseT_Half | SUPPORTED_10baseT_Full | SUPPORTED_100baseT_Half | SUPPORTED_100baseT_Full |
+	     				SUPPORTED_Autoneg | SUPPORTED_TP | SUPPORTED_MII);
+	if (priv->supports_gmii)
+	    cmd->supported |= SUPPORTED_1000baseT_Full;   /* Only support 1G full-duplex in ast2300 */
+	                  
+	cmd->transceiver = XCVR_INTERNAL; /* only supports internal transceiver */
+	cmd->advertising = ADVERTISED_TP | ADVERTISED_MII;
+	advert = ast_ether_phy_read_register(dev, 0, MII_ADVERTISE);
+    if (priv->supports_gmii)
+        advert2 = ast_ether_phy_read_register(dev, 0, MII_CTRL1000);
+
+	if (advert & ADVERTISE_10HALF)
+		cmd->advertising |= ADVERTISED_10baseT_Half;
+	if (advert & ADVERTISE_10FULL)
+		cmd->advertising |= ADVERTISED_10baseT_Full;
+	if (advert & ADVERTISE_100HALF)
+		cmd->advertising |= ADVERTISED_100baseT_Half;
+	if (advert & ADVERTISE_100FULL)
+		cmd->advertising |= ADVERTISED_100baseT_Full;
+	if (advert2 & ADVERTISE_1000FULL)
+		cmd->advertising |= ADVERTISED_1000baseT_Full;
+			
+	bmcr = ast_ether_phy_read_register(dev, 0, MII_BMCR);
+	lpa = ast_ether_phy_read_register(dev, 0, MII_LPA);
+    if (priv->supports_gmii) {
+        bmcr2 = ast_ether_phy_read_register(dev, 0, MII_CTRL1000);
+        lpa2 = ast_ether_phy_read_register(dev, 0, MII_STAT1000);
+    }
+	if (bmcr & BMCR_ANENABLE) {
+		cmd->autoneg = AUTONEG_ENABLE;
+		cmd->advertising |= ADVERTISED_Autoneg;
+		nego = mii_nway_result(advert & lpa);
+
+        if ((bmcr2 & (ADVERTISE_1000HALF | ADVERTISE_1000FULL)) &
+		    (lpa2 >> 2))
+			cmd->speed = SPEED_1000;
+            
+		else if (nego == LPA_100FULL || nego == LPA_100HALF)
+			cmd->speed = SPEED_100;
+		else
+			cmd->speed = SPEED_10;
+
+        if ((lpa2 & LPA_1000FULL) || nego == LPA_100FULL ||
+            nego == LPA_10FULL) 
+			cmd->duplex = DUPLEX_FULL;
+		else
+			cmd->duplex = DUPLEX_HALF;
+	} else {
+		cmd->autoneg = AUTONEG_DISABLE;
+
+        cmd->speed = ((bmcr & BMCR_SPEED1000 &&
+            (bmcr & BMCR_SPEED100) == 0) ? SPEED_1000 :
+                (bmcr & BMCR_SPEED100) ? SPEED_100 : SPEED_10);
+		cmd->duplex = (bmcr & BMCR_FULLDPLX) ? DUPLEX_FULL : DUPLEX_HALF;
+	}
+
+	return 0;
+}
+
+static int ast_ether_set_settings(struct net_device *dev, struct ethtool_cmd *cmd)
+{
+    struct ast_ether_priv *priv = netdev_priv(dev);
+	uint32_t bmcr, advert, tmp;
+	uint32_t advert2 = 0, tmp2 = 0;
+
+	ast_ether_phy_write_register(dev, 0, MII_BMCR, BMCR_RESET);
+
+	if (cmd->speed != SPEED_10 && 
+        cmd->speed != SPEED_100 &&
+        cmd->speed != SPEED_1000)
+		return -EINVAL;
+	if (cmd->duplex != DUPLEX_HALF && cmd->duplex != DUPLEX_FULL)
+		return -EINVAL;
+	if (cmd->autoneg != AUTONEG_DISABLE && cmd->autoneg != AUTONEG_ENABLE)
+		return -EINVAL;
+
+	if (cmd->autoneg == AUTONEG_ENABLE) {
+		if ((cmd->advertising & (ADVERTISED_10baseT_Half | 
+                                 ADVERTISED_10baseT_Full | 
+                                 ADVERTISED_100baseT_Half |
+                                 ADVERTISED_100baseT_Full | 
+                                 ADVERTISED_1000baseT_Half | 
+                                 ADVERTISED_1000baseT_Full)) == 0)
+			return -EINVAL;
+
+		/* advertise only what has been requested */
+		advert = ast_ether_phy_read_register(dev, 0, MII_ADVERTISE);
+		tmp = advert & ~(ADVERTISE_ALL | ADVERTISE_100BASE4);
+		
+        if (priv->supports_gmii) {
+			advert2 = ast_ether_phy_read_register(dev, 0, MII_CTRL1000);
+			tmp2 = advert2 & ~(ADVERTISE_1000HALF | ADVERTISE_1000FULL);
+		}
+		
+		if (cmd->advertising & ADVERTISED_10baseT_Half)
+			tmp |= ADVERTISE_10HALF;
+		if (cmd->advertising & ADVERTISED_10baseT_Full)
+			tmp |= ADVERTISE_10FULL;
+		if (cmd->advertising & ADVERTISED_100baseT_Half)
+			tmp |= ADVERTISE_100HALF;
+		if (cmd->advertising & ADVERTISED_100baseT_Full)
+			tmp |= ADVERTISE_100FULL;
+		if (priv->supports_gmii) {
+			if (cmd->advertising & ADVERTISED_1000baseT_Half)
+                tmp2 |= ADVERTISE_1000HALF;
+			if (cmd->advertising & ADVERTISED_1000baseT_Full)
+				tmp2 |= ADVERTISE_1000FULL;
+		}
+		
+		if (advert != tmp) {
+			ast_ether_phy_write_register(dev, 0, MII_ADVERTISE, tmp);
+		}
+		if ((priv->supports_gmii) && (advert2 != tmp2))
+			ast_ether_phy_write_register(dev, 0, MII_CTRL1000, tmp2);
+			
+		/* turn on auto negotiation, and force a re-negotiate */
+		bmcr = ast_ether_phy_read_register(dev, 0, MII_BMCR);
+		bmcr |= (BMCR_ANENABLE | BMCR_ANRESTART);
+		ast_ether_phy_write_register(dev, 0, MII_BMCR, bmcr);
+	} else {
+        if ((cmd->speed == SPEED_1000) && (cmd->duplex == DUPLEX_HALF)) /* not supported */
+                return -EINVAL;
+		/* turn off auto negotiation, set speed and duplexity */
+		bmcr = ast_ether_phy_read_register(dev, 0, MII_BMCR);
+		tmp = bmcr & ~(BMCR_ANENABLE | BMCR_SPEED100 |
+			       BMCR_SPEED1000 | BMCR_FULLDPLX);
+
+        if (cmd->speed == SPEED_1000) {
+            tmp |= BMCR_SPEED1000;
+            tmp |= (BMCR_ANENABLE | BMCR_ANRESTART);  /* Don't know why connection lost if auto negotiation is off */
+        } else if (cmd->speed == SPEED_100)
+			tmp |= BMCR_SPEED100;
+		if (cmd->duplex == DUPLEX_FULL)
+			tmp |= BMCR_FULLDPLX;
+		if (bmcr != tmp)
+			ast_ether_phy_write_register(dev, 0, MII_BMCR, tmp);
+	}
+
+	return 0;
+}
+
+static const struct ethtool_ops ast_ether_ethtool_ops = {
+	.get_settings = ast_ether_get_settings,
+	.set_settings = ast_ether_set_settings,
+	.get_link = ethtool_op_get_link,
+};
+
+/* This function is invoked by register_netdev() */
+int ast_ether_setup(struct net_device *dev)
+{
+	struct ast_ether_priv *priv;
+
+	ether_setup(dev); /* Fill in the fields of the device structure with ethernet values. */
+
+	dev->open = ast_ether_open;
+	dev->stop = ast_ether_close;
+	dev->hard_start_xmit = ast_ether_tx;
+	dev->get_stats = ast_ether_get_stats;
+	dev->tx_timeout = ast_ether_timeout;
+	dev->set_multicast_list = ast_ether_set_multicast_list;
+
+	/*
+	 * initialize private data structure 'priv'
+	 * it is zeroed and aligned in alloc_etherdev
+	 */
+	priv = netdev_priv(dev);
+	spin_lock_init(&priv->lock);
+
+	priv->maccr = MACCR_CRC_APD | MACCR_RXMAC_EN | MACCR_TXMAC_EN | MACCR_RXDMA_EN | MACCR_TXDMA_EN | MACCR_RX_BROADPKT_EN | MACCR_RX_RUNT;
+
+	ast_ether_alloc_descriptor(dev);
+    priv->link_state = -1;
+
+	return 0;
+}
+
+static int ast_ether_init_one(int id)
+{
+	struct net_device *dev;
+	int err;
+
+	ast_ether_netdev[id] = NULL;
+
+	/* alloc_etherdev ensures aligned and zeroed private structures */
+	dev = alloc_etherdev(sizeof(struct ast_ether_priv));
+	if (!dev)
+		return -ENOMEM;
+
+	if (!request_mem_region(ast_ether_phys_base[id], AST_ETHER_IO_REG_SIZE, "ast_ether")) {
+		err = -EBUSY;
+		goto out_alloc_etherdev;
+	}
+
+	dev->base_addr = (unsigned long)ioremap(ast_ether_phys_base[id], AST_ETHER_IO_REG_SIZE);
+	if (!dev->base_addr) {
+		err = -ENOMEM;
+		goto out_release_mem_region;
+	}
+
+	dev->irq = ast_ether_irq[id];
+	IRQ_SET_HIGH_LEVEL(dev->irq);
+	IRQ_SET_LEVEL_TRIGGER(dev->irq);
+	dev->init = ast_ether_setup;
+	dev->dev_id = id;
+	ast_ether_get_hardware_addr(dev);
+	SET_ETHTOOL_OPS(dev, &ast_ether_ethtool_ops);
+
+	err = register_netdev(dev);
+	if (err != 0) {
+		goto out_iomap;
+	}
+
+	ast_ether_netdev[id] = dev;
+	return 0;
+
+out_iomap:
+	iounmap((void *) dev->base_addr);
+out_release_mem_region:
+	release_mem_region(ast_ether_phys_base[id], AST_ETHER_IO_REG_SIZE);
+out_alloc_etherdev:
+	free_netdev(dev);
+
+	return err;
+}
+
+#define AST_SCU_CLK_STOP_MAC2            0x00200000 /* bit 21 */
+#define AST_SCU_CLK_STOP_MAC1            0x00100000 /* bit 20 */
+
+#define AST_SCU_RESET_MAC2               0x00001000 /* bit 12 */
+#define AST_SCU_RESET_MAC1               0x00000800 /* bit 11 */
+
+#define AST_SCU_UNLOCK_MAGIC             0x1688A8A8
+
+void __init ast_ether_scu_init(void)
+{
+	uint32_t reg;
+
+	iowrite32(AST_SCU_UNLOCK_MAGIC, SCU_KEY_CONTROL_REG); /* unlock SCU */
+
+	/* enable clcok */
+	reg = ioread32(SCU_CLK_STOP_REG);
+	reg &= ~(AST_SCU_CLK_STOP_MAC1 | AST_SCU_CLK_STOP_MAC2);
+	iowrite32(reg, SCU_CLK_STOP_REG);
+
+	mdelay(10);
+
+	/* stop the reset */
+	reg = ioread32(SCU_SYS_RESET_REG);
+	reg &= ~(AST_SCU_RESET_MAC1 | AST_SCU_RESET_MAC2);
+	iowrite32(reg, SCU_SYS_RESET_REG);
+
+	mdelay(10);
+
+	iowrite32(0, SCU_KEY_CONTROL_REG); /* lock SCU */
+}
+
+int __init ast_ether_init(void)
+{
+	int result, id;
+
+	ast_ether_scu_init();
+
+	result = -ENODEV;
+
+	for (id = 0; id < AST_ETHER_USE_NUM; id ++) {
+		if (ast_ether_init_one(id) == 0)
+			result = 0;
+	}
+	return result;
+}
+
+void ast_ether_exit(void)
+{
+	int id;
+	struct ast_ether_priv *priv;
+
+	for (id = 0; id < AST_ETHER_USE_NUM; id ++) {
+		if (ast_ether_netdev[id] == NULL)
+			continue;
+
+		priv = netdev_priv(ast_ether_netdev[id]);
+		if (priv->rx_desc)
+			dma_free_coherent(NULL, sizeof(struct ast_eth_desc) * RX_DES_NUM, priv->rx_desc, priv->rx_desc_dma);
+		if (priv->tx_desc)
+			dma_free_coherent(NULL, sizeof(struct ast_eth_desc) * TX_DES_NUM, priv->tx_desc, priv->tx_desc_dma);
+
+		iounmap((void *) ast_ether_netdev[id]->base_addr);
+		release_mem_region(ast_ether_phys_base[id], AST_ETHER_IO_REG_SIZE);
+
+		unregister_netdev(ast_ether_netdev[id]);
+		free_netdev(ast_ether_netdev[id]);
+		ast_ether_netdev[id] = NULL;
+	}
+}
+
+module_init(ast_ether_init);
+module_exit(ast_ether_exit);
+
+MODULE_AUTHOR("American Megatrends Inc.");
+MODULE_DESCRIPTION("AST2050/AST2100/AST2150/AST2200 Ethernet MAC driver");
+MODULE_LICENSE("GPL");
