// SPDX-License-Identifier: GPL-2.0
//

#include <linux/debugfs.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/of_irq.h>
#include <linux/clk-provider.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

#ifdef CONFIG_MACH_PIONEER3
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#endif

/*
 * 0x1f222100
 * 0x1f222300
 * 0x00: write buffer
 * 0x10: read buffer
 * 0x20: size
 * 	11 - 8 | 3 - 0
 * 	 rdsz  | wrsz
 * 0x24: ctrl
 *          | 15 - 8 |  7   |  6   |  2  |  1  | 0
 *          | clkdiv | cpol | cpha | int | rst | en
 * 0x28: dc
 * 0x2c: dc
 * 0x30: frame config
 * 0x38: frame config
 * 0x40: lsb first
 * 0x68: trigger
 * 	   0
 * 	trigger
 * 0x6c: done - transfer done
 *     0
 *   done
 * 0x70: done clear - write to clear done
 *     0
 *   done clear
 * 0x7c: chip select
 * clearing bit enables chip select
 *
 * 0xe0 - ef -- Full duplex read buffer?
 *
 * The vendor driver has defines for these locations
 * calling them "FULL_DEPLUX_RD<n>". There are apparently
 * 16 of them and they each hold 2 bytes of the data that was
 * read in so you can have a total of 32 bytes. But you can only
 * write 8 bytes at a time and the pointer for this buffer seems
 * to reset to 0 when a write happens. TL;DR; not 100% sure how
 * it works.
 */

#define DRIVER_NAME "spi_msc313"

/* DMA registers might only exist on SSD20XD and later */
#define REG_DMALEN_L	0xc0
#define REG_DMALEN_H	0xc4
#define REG_DMAEN	0xc8
#define REG_DMARW	0xcc

static const struct reg_field dmalenl_field	= REG_FIELD(REG_DMALEN_L, 0, 15);
static const struct reg_field dmalenh_field	= REG_FIELD(REG_DMALEN_H, 0, 7);
static const struct reg_field dmaen_field	= REG_FIELD(REG_DMAEN, 0, 0);
static const struct reg_field dmarw_field	= REG_FIELD(REG_DMARW, 0, 0);

/* SPI registers that are present in all versions */
#define REG_WRITEBUF	0x100
#define REG_READBUF	0x110
#define REG_SIZE	0x120
#define REG_CTRL	0x124
#define REG_TRIGGER	0x168
#define REG_DONE	0x16c
#define REG_DONECLR	0x170
#define REG_CS		0x17c

/* newer chips (ssd210+) only? */
#define REG_CLOCKNOTGATED	0x1d8

#define REG_FDREADBUF	0x1e0

#define DIV_SHIFT	8
#define DIV_WIDTH	3

#define FIFOSZ		8

/*
 * There seems to be a minimum size for DMA transfers.
 * If a <32 bytes transfer is triggered the following transfer
 * will be short and the SPI controller will never signal
 * complete.
 *
 * This doesn't seem to be just a case of the transfer
 * has to be longer than N bytes as some mixes of sizes <32
 * bytes don't trigger the issue.
 *
 * Anyhow, making sure everything is above 32 bytes
 * long seems to work.
 */
#define DMA_MIN		32

static const struct reg_field size_write_field	= REG_FIELD(REG_SIZE, 0, 3);
static const struct reg_field size_read_field	= REG_FIELD(REG_SIZE, 8, 11);
static const struct reg_field ctrl_en_field		= REG_FIELD(REG_CTRL, 0, 0);
static const struct reg_field ctrl_rst_field	= REG_FIELD(REG_CTRL, 1, 1);
static const struct reg_field ctrl_int_field	= REG_FIELD(REG_CTRL, 2, 2);
static const struct reg_field ctrl_cpha_field	= REG_FIELD(REG_CTRL, 6, 6);
static const struct reg_field ctrl_cpol_field	= REG_FIELD(REG_CTRL, 7, 7);
static const struct reg_field trigger_field		= REG_FIELD(REG_TRIGGER, 0, 0);
static const struct reg_field done_done_field	= REG_FIELD(REG_DONE, 0, 0);
static const struct reg_field doneclr_field		= REG_FIELD(REG_DONECLR, 0, 0);
static const struct reg_field cs_field			= REG_FIELD(REG_CS, 0, 0);

struct msc313_spi {
	struct device *dev;
	struct spi_master *master;
	struct clk_hw *divider;

	struct regmap *regmap;

	struct regmap_field *en;
	struct regmap_field *inten;
	struct regmap_field *rst;

	struct regmap_field *cpol;
	struct regmap_field *cpha;
	struct regmap_field *wrsz;
	struct regmap_field *rdsz;
	struct regmap_field *done;
	struct regmap_field *doneclr;

	struct regmap_field *trigger;
	struct regmap_field *cs;

	struct completion tfr_done;

	spinlock_t lock;

	/* dma */
	struct regmap_field *dmalenl, *dmalenh;
	struct regmap_field *dmaen;
	struct regmap_field *dmarw;

	struct dma_chan *dmachan;
	struct completion dma_done;
	bool dma_success;
};

static const struct regmap_config msc313_spi_regmap_config = {
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 4,
	.fast_io = true,
};

static int msc313_spi_setup(struct spi_device *spi)
{
	struct msc313_spi *mspi = spi_master_get_devdata(spi->master);

	regmap_field_write(mspi->cpha, spi->mode & SPI_CPHA ? 1 : 0);
	regmap_field_write(mspi->cpol, spi->mode & SPI_CPOL ? 1 : 0);

	return 0;
}

static void msc313_spi_loadtxbuff(struct msc313_spi *mspi, const u8 *txbuff, unsigned int len)
{
	int i, j, b, reg;
	unsigned int value;

	for (i = 0; (i < FIFOSZ/2) && (i * 2 < len); i++) {
		value = 0;
		b = i * 2;
		for (j = 0; j < 2 && (b + j < len); j++) {
			value |= (unsigned int)(*txbuff++) << (8 * j);
		}
		reg = REG_WRITEBUF + (i * 0x4);
		regmap_write(mspi->regmap, reg, value);
		//printk("wrote %d -> %x\n", i, value);
	}
}

static void msc313_spi_saverxbuff(struct msc313_spi *mspi, unsigned int bufoff,
		u8 *rxbuff, unsigned int len)
{
	int i, j, reg;
	unsigned int value;

	for (i = 0; (i < FIFOSZ/2) && (i * 2 < len); i++) {
		reg = bufoff + (i * 0x4);
		regmap_read(mspi->regmap, reg, &value);
		//printk("read %d -> %x\n", i, value);
		for (j = i * 2; (j < ((i * 2) + 2)) && (j < len); j++) {
			*rxbuff++ = value & 0xff;
			value = value >> 8;
		}
	}
}

static void msc313_spi_set_cs(struct spi_device *spi, bool enable)
{
	struct msc313_spi *mspi = spi_master_get_devdata(spi->master);

	regmap_field_write(mspi->cs, enable ? 1 : 0);
}

static void msc313_spi_dma_callback(void *dma_async_param,
				    const struct dmaengine_result *result)
{
	struct msc313_spi *mspi = dma_async_param;

	mspi->dma_success = result->result == DMA_TRANS_NOERROR;

	complete_all(&mspi->dma_done);
}

static void msc313_spi_reset(struct msc313_spi *mspi)
{
	regmap_field_write(mspi->rst, 0);
	udelay(10);
	regmap_field_write(mspi->rst, 1);
	udelay(10);
}

static inline int msc313_spi_trigger(struct msc313_spi *mspi, bool dma)
{
	regmap_field_force_write(mspi->trigger, 1);

	if (dma) {
		/* DMA should finish first */
		wait_for_completion(&mspi->dma_done);

		if (!mspi->dma_success) {
			dev_err(mspi->dev, "dma failure\n");
			return -EIO;
		}
	}

	if (!wait_for_completion_timeout(&mspi->tfr_done, HZ * 10))  {
		dev_err(mspi->dev, "timeout waiting for transfer to complete, dma: %d\n", dma);
		return -EIO;
	}

	return 0;
}

static int msc313_spi_transfer_one_dma(struct spi_controller *ctlr,
		struct spi_device *spi, struct spi_transfer *transfer)
{
	struct msc313_spi *mspi = spi_controller_get_devdata(ctlr);
	struct dma_async_tx_descriptor *dmadesc;
	const u8 *txbuf = transfer->tx_buf;
	unsigned int len = transfer->len;
	u8 *rxbuf = transfer->rx_buf;
	dma_addr_t dmaaddr = 0;
	bool read = rxbuf ? true : false;
	enum dma_data_direction dmadir = read ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	enum dma_transfer_direction dmatrans = read ? DMA_DEV_TO_MEM : DMA_MEM_TO_DEV;
	int ret;

	/* Setup the spi controller side of the DMA */
	regmap_field_write(mspi->dmaen, 1);
	regmap_field_write(mspi->dmarw, read ? 1 : 0);
	regmap_field_write(mspi->dmalenl, len);
	regmap_field_write(mspi->dmalenh, len >> 16);
	regmap_field_write(mspi->rdsz, 0);
	regmap_field_write(mspi->wrsz, 0);

	dmaaddr = dma_map_single(mspi->dev, read ? rxbuf : txbuf, len, dmadir);
	ret = dma_mapping_error(mspi->dev, dmaaddr);
	if (ret)
		return ret;

	dmadesc = dmaengine_prep_slave_single(mspi->dmachan, dmaaddr, len, dmatrans, 0);
	if (!dmadesc) {
		ret = -ENOMEM;
		goto out;
	}

	dmadesc->callback_result = msc313_spi_dma_callback;
	dmadesc->callback_param = mspi;
	reinit_completion(&mspi->dma_done);
	reinit_completion(&mspi->tfr_done);

	dmaengine_submit(dmadesc);
	dma_async_issue_pending(mspi->dmachan);

	ret = msc313_spi_trigger(mspi, true);

out:
	regmap_field_write(mspi->dmaen, 0);
	dma_unmap_single(mspi->dev, dmaaddr, len, dmadir);

	return ret;
}

static int msc313_spi_transfer_one_pio(struct spi_controller *ctlr, struct spi_device *spi,
									   struct spi_transfer *transfer)
{
	struct msc313_spi *mspi = spi_controller_get_devdata(ctlr);
	const u8 *txbuf = transfer->tx_buf;
	unsigned int len = transfer->len;
	u8 *rxbuf = transfer->rx_buf;
	int txed = 0;
	int ret;

	while (len - txed > 0) {
		int blksz = min(len - txed, (unsigned int) FIFOSZ);
		unsigned int rdbuf = REG_READBUF;

		reinit_completion(&mspi->tfr_done);

		if (txbuf) {
			msc313_spi_loadtxbuff(mspi, txbuf, blksz);
			txbuf += blksz;
			regmap_field_write(mspi->wrsz, blksz);
			regmap_field_write(mspi->rdsz, 0);
			/*
			 * If there was a txbuf then we did a write and the
			 * read back data will be in the other read buffer.
			 */
			rdbuf = REG_FDREADBUF;
		} else if (rxbuf) {
			regmap_field_write(mspi->wrsz, 0);
			regmap_field_write(mspi->rdsz, blksz);
		} else
			return -EINVAL;

		ret = msc313_spi_trigger(mspi, false);
		if (ret)
			return ret;

		/* save any incoming data into the buffer */
		if (rxbuf) {
			msc313_spi_saverxbuff(mspi, rdbuf, rxbuf, blksz);
			rxbuf += blksz;
		}

		txed += blksz;
	}

	return 0;
}

static bool msc313_spi_transfer_dmaok(struct msc313_spi *mspi, struct spi_transfer *transfer)
{
	if (!mspi->dmachan)
		return false;

	/* DMA only works for half duplex */
	if (transfer->tx_buf && transfer->rx_buf)
		return false;

	if (transfer->len < DMA_MIN)
		return false;

	/*
	 * max transfer size is enforced by
	 * the spi layer, don't need to check here.
	 */

	return true;
}

static int msc313_spi_transfer_one(struct spi_controller *ctlr,
		struct spi_device *spi, struct spi_transfer *transfer)
{
	struct msc313_spi *mspi = spi_controller_get_devdata(ctlr);
	int ret;

	clk_set_rate(mspi->divider->clk, transfer->speed_hz);

	if (msc313_spi_transfer_dmaok(mspi, transfer))
		ret = msc313_spi_transfer_one_dma(ctlr, spi, transfer);
	else
		ret = msc313_spi_transfer_one_pio(ctlr, spi, transfer);

	/*
	 * A failed DMA transfer etc causes the controller to lock up
	 * so if there was an error reset the controller.
	 */
	if (ret)
		msc313_spi_reset(mspi);

	//mdelay(20);

	spi_finalize_current_transfer(ctlr);

	return ret;
}

static irqreturn_t msc313_spi_irq(int irq, void *data)
{
	struct msc313_spi *mspi = data;
	unsigned int done;

	regmap_field_read(mspi->done, &done);
	if (!done)
		return IRQ_NONE;

	regmap_field_force_write(mspi->doneclr, 1);
	complete_all(&mspi->tfr_done);

	return IRQ_HANDLED;
}

static const struct clk_div_table div_table[] = {
	{0, 2},
	{1, 4},
	{2, 8},
	{3, 16},
	{4, 32},
	{5, 64},
	{6, 128},
	{7, 256},
	{ 0 },
};

struct msc313_spi_data {
	int (*dma_probe)(struct msc313_spi *mspi, struct device *dev);
	size_t (*max_transfer_size)(struct spi_device *spi);
};

static const struct msc313_spi_data msc313_data = {
};

static size_t msc313_spi_dmalimit_maxtransfer(struct spi_device *spi)
{
	return 0xffffff;
}

static int msc313_spi_ssd20xd_dma_probe(struct msc313_spi *mspi, struct device *dev)
{
	struct dma_chan *dmachan;

	dmachan = dma_request_chan(dev, "movedma");
		if (IS_ERR(dmachan))
			return -EPROBE_DEFER;

	mspi->dmachan = dmachan;

	return 0;
}

static const struct msc313_spi_data ssd20xd_data = {
	.dma_probe = msc313_spi_ssd20xd_dma_probe,
	.max_transfer_size = msc313_spi_dmalimit_maxtransfer,
};

#ifdef CONFIG_MACH_PIONEER3
static int msc313_spi_ssd210_dma_probe(struct msc313_spi *mspi, struct device *dev)
{
	struct regmap *bdmasel = syscon_regmap_lookup_by_phandle(dev->of_node, "mstar,bdmaselwtf");
	struct dma_chan *dmachan;

	if (IS_ERR(bdmasel))
		return PTR_ERR(bdmasel);

	// THIS IS A HACK!!
	regmap_write(bdmasel, 0x0, 0x11);

	// Vendor kernel does this?
	//regmap_write(mspi->regmap, REG_CLOCKNOTGATED, 0x100);

	dmachan = dma_request_chan(dev, "bdma");
	if (IS_ERR(dmachan))
		return -EPROBE_DEFER;

	mspi->dmachan = dmachan;

	return 0;
}

static const struct msc313_spi_data ssd210_data = {
		.dma_probe = msc313_spi_ssd210_dma_probe,
		.max_transfer_size = msc313_spi_dmalimit_maxtransfer,
};
#endif

static int msc313_spi_probe(struct platform_device *pdev)
{
	const struct msc313_spi_data *match_data;
	struct device *dev = &pdev->dev;
	struct spi_master *master;
	int ret, irq, numparents;
	struct msc313_spi *spi;
	const char *parents[1];
	void __iomem *base;
	char *sclk_name;
	struct clk *sclk;

	match_data = of_device_get_match_data(dev);
	if (!match_data)
		return -EINVAL;

	numparents = of_clk_parent_fill(pdev->dev.of_node, parents, ARRAY_SIZE(parents));
	if (numparents != 1)
		return -EINVAL;

	master = spi_alloc_master(&pdev->dev, sizeof(struct msc313_spi));
	if (!master)
		return -ENOMEM;

	platform_set_drvdata(pdev, master);

	spi = spi_master_get_devdata(master);
	spin_lock_init(&spi->lock);
	spi->dev = &pdev->dev;
	spi->master = master;
	init_completion(&spi->tfr_done);
	init_completion(&spi->dma_done);

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	spi->regmap = devm_regmap_init_mmio(spi->dev, base,
			&msc313_spi_regmap_config);
	if (IS_ERR(spi->regmap))
		return PTR_ERR(spi->regmap);

	spi->wrsz = devm_regmap_field_alloc(spi->dev, spi->regmap, size_write_field);
	spi->rdsz = devm_regmap_field_alloc(spi->dev, spi->regmap, size_read_field);

	spi->en = devm_regmap_field_alloc(spi->dev, spi->regmap, ctrl_en_field);
	spi->rst = devm_regmap_field_alloc(spi->dev, spi->regmap, ctrl_rst_field);
	spi->inten = devm_regmap_field_alloc(spi->dev, spi->regmap, ctrl_int_field);

	spi->cpha = devm_regmap_field_alloc(spi->dev, spi->regmap, ctrl_cpha_field);
	spi->cpol = devm_regmap_field_alloc(spi->dev, spi->regmap, ctrl_cpol_field);

	spi->trigger = devm_regmap_field_alloc(spi->dev, spi->regmap, trigger_field);
	spi->done = devm_regmap_field_alloc(spi->dev, spi->regmap, done_done_field);
	spi->doneclr = devm_regmap_field_alloc(spi->dev, spi->regmap, doneclr_field);
	spi->cs = devm_regmap_field_alloc(spi->dev, spi->regmap, cs_field);

	spi->dmalenl = devm_regmap_field_alloc(spi->dev, spi->regmap, dmalenl_field);
	spi->dmalenh = devm_regmap_field_alloc(spi->dev, spi->regmap, dmalenh_field);
	spi->dmaen = devm_regmap_field_alloc(spi->dev, spi->regmap, dmaen_field);
	spi->dmarw = devm_regmap_field_alloc(spi->dev, spi->regmap, dmarw_field);

	sclk_name = devm_kasprintf(dev, GFP_KERNEL, "%s_sclk", dev_name(dev));
	spi->divider = devm_clk_hw_register_divider_table(dev, sclk_name, parents[0], 0,
			base + REG_CTRL, DIV_SHIFT, DIV_WIDTH,
			0, div_table, &spi->lock);
	if (IS_ERR(spi->divider))
		return PTR_ERR(spi->divider);

	sclk = spi->divider->clk;

	ret = clk_prepare_enable(sclk);
	if (ret)
		dev_err(&pdev->dev, "clk enable failed: %d\n", ret);

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!irq)
		return -EINVAL;

	ret = devm_request_irq(&pdev->dev, irq, msc313_spi_irq, IRQF_SHARED,
			dev_name(&pdev->dev), spi);

	master->dev.of_node = pdev->dev.of_node;
	master->bus_num = pdev->id;
	master->num_chipselect = 1;
	master->mode_bits = SPI_CPHA | SPI_CPOL;
	master->max_speed_hz = clk_round_rate(sclk, ~0);
	master->min_speed_hz = clk_round_rate(sclk, 0);
	master->setup = msc313_spi_setup;
	master->set_cs = msc313_spi_set_cs;
	master->transfer_one = msc313_spi_transfer_one;
	master->bits_per_word_mask = SPI_BPW_MASK(8);

	/*
	 * When DMA is enabled there is a tx limit,
	 * DMA enabled compatibles should provide a function
	 * to find the limit.
	 */
	master->max_transfer_size = match_data->max_transfer_size;

	regmap_field_write(spi->rst, 1);
	regmap_field_write(spi->inten, 1);
	regmap_field_write(spi->en, 1);

	if (match_data->dma_probe) {
		int ret = match_data->dma_probe(spi, dev);
		if (ret)
			return ret;
	}

	return devm_spi_register_master(&pdev->dev, master);
}

static const struct of_device_id msc313_spi_of_match[] = {
	{
		.compatible = "mstar,msc313-spi",
		.data = &msc313_data,
	},
	{
		.compatible = "sstar,ssd20xd-spi",
		.data = &ssd20xd_data,
	},
#ifdef CONFIG_MACH_PIONEER3
	{
		.compatible = "sstar,ssd210-spi",
		.data = &ssd210_data,
	},
#endif
	{},
};
MODULE_DEVICE_TABLE(of, msc313_spi_of_match);

static struct platform_driver msc313_spi_driver = {
	.probe = msc313_spi_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = msc313_spi_of_match,
	},
};
module_platform_driver(msc313_spi_driver);

MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_DESCRIPTION("MStar MSC313 SPI driver");
MODULE_AUTHOR("Daniel Palmer <daniel@thingy.jp>");
MODULE_LICENSE("GPL v2");
