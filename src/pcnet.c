/* pcnet.c: PCNet-PCI II/III Ethernet controller driver */
/*
 * Authors:
 *		Dmitry Podgorny <pasis.ua@gmail.com>
 *		Denis Kirjanov <kirjanov@gmail.com>
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define DRV_NAME	"pcnet_dummy"
#define DRV_VERSION	"dev"
#define DRV_DESCRIPTION	"PCNet-PCI II/III Ethernet controller driver"

MODULE_AUTHOR("Dmitry Podgorny <pasis.ua@gmail.com>");
MODULE_AUTHOR("Denis Kirjanov <kirjanov@gmail.com>");
MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL");

static DEFINE_PCI_DEVICE_TABLE(pcnet_dummy_pci_tbl) = {
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_LANCE) },
	{ }
};

enum {
	PCNET_IOSIZE_LEN = 0x20,
};

enum {
	PCNET_IO_PROM = 0x0,
	PCNET_IO_RDP = 0x10,
	PCNET_IO_RAP = 0x12,
	PCNET_IO_RESET = 0x14,
	PCNET_IO_BDP = 0x16,
};

enum {
	PCNET_IO32_PROM = 0x0,
	PCNET_IO32_RDP = 0x10,
	PCNET_IO32_RAP = 0x14,
	PCNET_IO32_RESET = 0x18,
	PCNET_IO32_BDP = 0x1c,
};

enum {
	CSR0 = 0,
	CSR1 = 1,
	CSR2 = 2,
	CSR4 = 4,
	CSR15 = 15,
	/* missed frame count */
	CSR112 = 112,
};

enum {
	BCR18 = 18,
};

enum {
	CSR0_INIT = 0x0001,
	CSR0_START= 0x0002,
	CSR0_STOP = 0x0004,
	/* transmit demand */
	CSR0_TDMD = 0x0008,
	/* transmitter on */
	CSR0_TXON = 0x0010,
	/* receiver on */
	CSR0_RXON = 0x0020,
	/* interrupt enable */
	CSR0_IENA = 0x0040,
	CSR0_INTR = 0x0080,
	/* initialisation done */
	CSR0_IDON = 0x0100,
	CSR0_TINT = 0x0200,
	CSR0_RINT = 0x0400,
	CSR0_MERR = 0x0800,
	CSR0_MISS = 0x1000,
	CSR0_CERR = 0x2000,
	CSR0_BABL = 0x4000,
	CSR0_ERR  = 0x8000,
};

enum {
	BCR18_DWIO = 0x0080,
};

/* PCnet-PCI II controller initialization includes the reading
 * of the initialization block in memory to obtain the operat-
 * ing parameters. 
 * When SSIZE32 (BCR20, bit 8) is set to ONE, all
 * initialization block entries are logically 32-bits wide.
 *
 * The PCnet-PCI II controller obtains the start address of
 * the initialization block from the contents of CSR1 (least
 * significant 16 bits of address) and CSR2 (most signifi-
 * cant 16 bits of address). The host must write CSR1 and
 * CSR2 before setting the INIT bit. The initialization block
 * contains the user defined conditions for PCnet-PCI II
 * controller operation, together with the base addresses
 * and length information of the transmit and receive
 * descriptor rings.
 */

/* Am79C970A reference, p. 69 */
struct pcnet_dummy_init_block {
	__le16 mode;
	/* num of entries in TX/RX rings */
	__le16 txlen_rxlen;
	/* MAC address */
	u8 mac_addr[6];
	__le16 reserved;
	/* The Logical Address Filter (LADRF) is a programmable
	 * 64-bit mask that is used to accept incoming packets
	 * based on Logical (Multicast) Addresses.
	 */
	__le32 laddr_filter_low;
	__le32 laddr_filter_hi;
	/* programmed with the physical address of the receive
	 * and transmit descriptor rings
	 */
	__le32 rx_ring;
	__le32 tx_ring;
} __attribute__ ((__packed__));

/* 32bit TX/RX descriptors */
/* PCnet Software Design Considerations, p.5 */
struct pcnet_dummy_xmit_descr {
	__le32 addr;
	/* The BCNT fields of the transmit and receive descriptors
	 * are 12-bit negative numbers representing the twos com-
	 * plement of the buffer size in bytes
	 */
	__le16 size;
	__le16 status;
	__le32 flags;
	__le32 reserved;
};

struct pcnet_dummy_private {
	/* TODO:
	 * DMA buffer management operations
	 */
	spinlock_t lock;
	struct pci_dev *pci_dev;
	void __iomem *base;
};

static unsigned short pcnet_dummy_csr_read16(void __iomem *ioaddr,
		unsigned short csr)
{
	iowrite16(csr, ioaddr + PCNET_IO_RAP);
	return ioread16(ioaddr + PCNET_IO_RDP);
}

static unsigned long pcnet_dummy_csr_read32(void __iomem *ioaddr,
		unsigned long csr)
{
	iowrite32(csr, ioaddr + PCNET_IO32_RAP);
	/* 16 most significant bits are undefined */
	return ioread32(ioaddr + PCNET_IO32_RDP) & 0xffff;
}

static void pcnet_dummy_csr_write32(void __iomem *ioaddr,
		unsigned long data, unsigned long csr)
{
	iowrite32(csr, ioaddr + PCNET_IO32_RAP);
	iowrite32(data, ioaddr + PCNET_IO32_RDP);
}

static unsigned long pcnet_dummy_bcr_read32(void __iomem *ioaddr,
		unsigned long bcr)
{
	iowrite32(bcr, ioaddr + PCNET_IO32_RAP);
	/* 16 most significant bits are undefined */
	return ioread32(ioaddr + PCNET_IO32_BDP) & 0xffff;
}

static void pcnet_dummy_reset(void __iomem *ioaddr)
{
	ioread16(ioaddr + PCNET_IO_RESET);
	if (pcnet_dummy_csr_read16(ioaddr, CSR0) != CSR0_STOP)
		ioread32(ioaddr + PCNET_IO32_RESET);
}

static int pcnet_dummy_open(struct net_device *ndev)
{
	return 0;
}

static int pcnet_dummy_stop(struct net_device *ndev)
{
	return 0;
}

static int pcnet_dummy_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	return 0;
}

#ifdef HAVE_NET_DEVICE_OPS
/* net_device_ops structure is new for 2.6.31 */
static const struct net_device_ops pcnet_dummy_net_device_ops = {
	.ndo_open = pcnet_dummy_open,
	.ndo_stop = pcnet_dummy_stop,
	.ndo_start_xmit = pcnet_dummy_start_xmit
};
#endif /* HAVE_NET_DEVICE_OPS */

static int __devinit pcnet_dummy_init_netdev(struct pci_dev *pdev,
		unsigned long ioaddr)
{
	struct net_device *ndev = pci_get_drvdata(pdev);
	struct pcnet_dummy_private *pp;
	int irq;

	irq = pdev->irq;
	pp = netdev_priv(ndev);
	ndev->base_addr = ioaddr;
	ndev->irq = irq;
	pp->pci_dev = pdev;
	pp->base = (void *)ioaddr;
	spin_lock_init(&pp->lock);

	/* init DMA rings */
	/* init net_dev_ops */
	/* FIXME:
	 * isolate explicit HAVE_NET_DEVICE_OPS
	 * with a macro
	 */
#ifdef HAVE_NET_DEVICE_OPS
	ndev->netdev_ops = &pcnet_dummy_net_device_ops;
#else
	ndev->open = pcnet_dummy_open;
	ndev->stop = pcnet_dummy_stop;
	ndev->hard_start_xmit = pcnet_dummy_start_xmit;
#endif /* HAVE_NET_DEVICE_OPS */

	/* registers net_device and returns err */
	/* TODO: reset the chip at the end */

	return 0;
}

static int __devinit pcnet_dummy_init_one(struct pci_dev *pdev,
		const struct pci_device_id *ent)
{
	struct pcnet_dummy_private *pp;
	struct net_device *ndev;
	void __iomem *ioaddr;
#ifdef USE_IO_OPS
	int bar = 0;
#else
	int bar = 1;
#endif

	if (pci_enable_device(pdev))
		return -ENODEV;
	/* enables bus-mastering for device pdev */
	pci_set_master(pdev);

	ndev = alloc_etherdev(sizeof(*pp));
	if (!ndev)
		goto out;
	/* register ourself under /sys/class/net/ */
	SET_NETDEV_DEV(ndev, &pdev->dev);

	if (pci_request_regions(pdev, DRV_NAME))
		goto out_netdev;

	ioaddr = pci_iomap(pdev, bar, PCNET_IOSIZE_LEN);
	if (!ioaddr)
		goto out_res;
	pci_set_drvdata(pdev, ndev);

	if (pcnet_dummy_init_netdev(pdev, (unsigned long)ioaddr))
		goto out_res_unmap;

	return 0;

out_res_unmap:
	pci_iounmap(pdev, ioaddr);
out_res:
	pci_release_regions(pdev);
out_netdev:
	free_netdev(ndev);
out:
	pci_disable_device(pdev);
	return -ENODEV;
}

static void __devexit pcnet_dummy_remove_one(struct pci_dev *pdev)
{
	struct net_device *ndev = pci_get_drvdata(pdev);
	struct pcnet_dummy_private *pp;

	pp = netdev_priv(ndev);
	pci_iounmap(pdev, pp->base);
	free_netdev(ndev);
	pci_disable_device(pdev);
	pci_release_regions(pdev);
	pci_set_drvdata(pdev, NULL);
}

static struct pci_driver pcnet_dummy_driver = {
	.name		= DRV_NAME,
	.id_table	= pcnet_dummy_pci_tbl,
	.probe		= pcnet_dummy_init_one,
	.remove		= __devexit_p(pcnet_dummy_remove_one),
#if 0
	/* FIXME: add PM hooks */
#endif
};

static int __init pcnet_init(void)
{
	printk(KERN_INFO DRV_NAME ": " DRV_DESCRIPTION "\n");

	return pci_register_driver(&pcnet_dummy_driver);
}

static void __exit pcnet_exit(void)
{
	pci_unregister_driver(&pcnet_dummy_driver);
}

module_init(pcnet_init);
module_exit(pcnet_exit);
