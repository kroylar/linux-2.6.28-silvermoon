/*
 * linux/drivers/input/gpio_ir.c
 *
 * Support for the Aspenite /Zyloniteii /Teton development Platforms
 *
 * Copyright (C) 2008 Marvell International Ltd.
 *
 * 2008-010-12: Ofer Zaarur <ofer.zaarur@marvell.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/irq.h>

#include <mach/gpio_ir.h>
#include <mach/ir_key_def.h>
#include <mach/timer_services.h>

#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/gpio.h>

MODULE_LICENSE("GPL");

/* Only and only one XXX_READOUT_IRCBUFFER can be defined */
#define MARVELL_READOUT_IRCBUFFER

/*
 * Nec SC33 code:
 * ___                  _-_-_-_-_-_-_-_~~
 * Pre 0 1 1 1 0 1 1 0  1 0 0 0 1 0 0 1
 */


/*
 * for Nex protocol analysis
 */
#define ENDED 		100000 	/*uSec*/
#define END_MIN		35000 	/*uSec*/
#define END_MAX		42000 	/*uSec*/
#define PREAM_MIN	12000	/*uSec*/
#define PREAM_MAX	21000	/*uSec*/
#define ONE_MIN		2100	/*uSec*/
#define ONE_MAX		4100	/*uSec*/
#define ZERO_MIN	800	/*uSec*/
#define ZERO_MAX	2100	/*uSec*/
#define word_size 	32     	/*word size according to the protocol */
#define calibration_n 	2167    /* timer resolution eq=1500 OS timer HZ */


#define IR_SHIFT(x)     (x % (sizeof(int) * 8)) /* Shift6*/
#define GPIO_BIT(x)     (1 << ((x) & 0x1f))

/*
 * Circular buffer
 */
#define CIRC_BUFF_MASK 0x3ff
#define CIRC_BUFF_LENGTH 0x400
static unsigned int cbuffer[CIRC_BUFF_LENGTH];
static unsigned int cb_start=0, cb_end=0;


/*
 * Table of IR-signal-code and key
 */
ir_key_table_t marvell_nec_ir_key_table[] = {

	{0x00, MV_IR_KEY_DIGIT_1},
	{0x01, MV_IR_KEY_DIGIT_2},
	{0x02, MV_IR_KEY_DIGIT_3},
	{0x03, MV_IR_KEY_DIGIT_4},
	{0x04, MV_IR_KEY_DIGIT_5},
	{0x05, MV_IR_KEY_DIGIT_6},
	{0x06, MV_IR_KEY_DIGIT_7},
	{0x07, MV_IR_KEY_DIGIT_8},
	{0x08, MV_IR_KEY_DIGIT_9},
	{0x09, MV_IR_KEY_DIGIT_10},
	{0x0A, MV_IR_KEY_DIGIT_11},
	{0x0B, MV_IR_KEY_DIGIT_12},
	{0x0C, MV_IR_KEY_DIGIT_13},
	{0x0D, MV_IR_KEY_DIGIT_14},
	{0x0E, MV_IR_KEY_DIGIT_15},
	{0x0F, MV_IR_KEY_DIGIT_16},
	{0x10, MV_IR_KEY_DIGIT_17},
	{0x11, MV_IR_KEY_DIGIT_18},
	{0x12, MV_IR_KEY_DIGIT_19},
	{0x13, MV_IR_KEY_DIGIT_20},
	{0x14, MV_IR_KEY_DIGIT_21},
	{0x15, MV_IR_KEY_DIGIT_22},
	{0x16, MV_IR_KEY_DIGIT_23},
	{0x17, MV_IR_KEY_DIGIT_24},
	{0x18, MV_IR_KEY_DIGIT_25},
	{0x19, MV_IR_KEY_DIGIT_26},
	{0x1A, MV_IR_KEY_DIGIT_27},
	{0x1B, MV_IR_KEY_DIGIT_28},
	{0x1C, MV_IR_KEY_DIGIT_29},
	{0x1D, MV_IR_KEY_DIGIT_30},
	{0x1E, MV_IR_KEY_DIGIT_31},
	{0x1F, MV_IR_KEY_DIGIT_32},
	{0x20, MV_IR_KEY_DIGIT_33},
	{0, MV_IR_KEY_NULL},

};

ir_key_table_t *ir_key_table = marvell_nec_ir_key_table;

unsigned int ir_key = MV_IR_KEY_NULL;
unsigned int ir_key_recvflag = 0;

/*
 * structure to store the time when interrupt comes
 */
typedef struct system_time {
	unsigned int jiff;
	unsigned int base;
} system_time_t;
system_time_t len_time = {0, 0};

/*
 * analyse the level length based on NEC protocol.
 * NEC ir transmitter timing:
 * Preamble: 9ms/4.5ms/0.56ms, Bit1: 1.69ms/0.56ms, Bit0: 0.56ms/0.56ms.
 * Hold: 9ms/2.25ms/0.56ms
 */
static unsigned int word = 0, dont_send_more=0 , count_word = 0 , preamble = 0;
static unsigned int end = 0, i=0;



/*
 *Analyse the address and key
 *
 */
static void analyseword (struct cir_device *cir, int word)
{
	if ((word << CODE_SIZE) == (CUSTOM_CODE << CODE_SIZE) && !dont_send_more) {
		if ((word & 0xff000000)==(~(word << 8) & 0xff000000)) {
			ir_key = MV_IR_KEY_NULL;
			i = 0;
	/* check for the key */
			while (ir_key_table[i].ir_key != MV_IR_KEY_NULL) {
				if ((ir_key_table[i].ir_encode) == ((word & 0xff0000)
					>> CODE_SIZE)) {
					ir_key = ir_key_table[i].ir_key;
	/*				printk(KERN_INFO "The key %d was found\n",ir_key); */
					input_report_key(cir->input_dev, ir_key , 1);
					input_report_key(cir->input_dev, ir_key , 0);
					cb_start = cb_end;
					dont_send_more = 1;
					break;
				}

				i++;
			}
			if (ir_key == MV_IR_KEY_NULL) {
				printk(">>> Unable to find matching key for %x\n", word);
			}
			else {
				printk(">>> Found matching key! %d\n", ir_key);
			}
			/* key not found, just noises */
		}
	}
	else {
		uart_putc('N');
	}
}
static void clear_ircbuffer (struct cir_device *cir)
{
	unsigned int event_len;

	while ((cb_start != cb_end) && (((cb_start+1) & CIRC_BUFF_MASK) != cb_end)) {
		event_len = cbuffer[cb_start]*1000;
		/*printk(KERN_INFO "%u\n",event_len); */
		cb_start++;
		cb_start &= CIRC_BUFF_MASK;
		if ((event_len > ZERO_MIN) && (event_len < ZERO_MAX)) {
			uart_putc('0');
			if (preamble == 1) {
				count_word++;
			}
			if (count_word == word_size) {
				analyseword (cir, word);
				count_word = 0;
				preamble = 0;
				word = 0;
			}
			continue;
		}
		if ((event_len >= ONE_MIN) && (event_len < ONE_MAX)) {
			uart_putc('1');
			if (preamble == 1) {
				word |= 1 << count_word;
				count_word++;
			}
			if (count_word == word_size) {
				analyseword (cir, word);
				count_word = 0;
				preamble = 0;
				word = 0;
			}
			continue;
		}
		if ((event_len > PREAM_MIN) && (event_len < PREAM_MAX)) {
			uart_putc('P');
			preamble = 1;
			count_word = 0;
			end = 0;
			continue;
		}
		if ((event_len > END_MIN) && (event_len < END_MAX)){
			uart_putc('S');
			continue;
		}
		if (event_len > ENDED){
			uart_putc('E');
			dont_send_more = 0;
			continue;
		}
	}
}

/*
 * This function should be called in an independent thread.
 * It reads out bit-wised code.
 */
static void process_times(unsigned long nodata)
{
	int event_count = 0;
	while (cb_start != cb_end) {
		event_count++;
		printk("Event %ld ticks long: %d\n", (cbuffer[cb_start] & ~0x80000000L), cbuffer[cb_start] >> 31);
		cb_start = (cb_start+1) & CIRC_BUFF_MASK;
	}
	printk("Read out %d events\n", event_count);
	return;
}

DEFINE_TIMER(process_time_timer, process_times, 0, 0);


static irqreturn_t cir_interrupt(int irq, void *dev_id)
{
	struct cir_device *cir = (struct cir_device *)dev_id;
	static unsigned long this, prev;

	prev = this;
	this = timer_services_counter_read(COUNTER_0);
	cbuffer[cb_end] = ((this - prev) & 0x7fffffffL)
		 	| ((__raw_readl(cir->mmio_base + 0x00) & 0x40) << 25);

	cb_end = (cb_end+1) & CIRC_BUFF_MASK;
	if (cb_end == cb_start)
		cb_start = (cb_start+1) & CIRC_BUFF_MASK;

	mod_timer(&process_time_timer, jiffies + msecs_to_jiffies(20));

	return IRQ_HANDLED;
}

/**
 * cir_enable - enable the cir -unmask
 *
 * Turn on the cir.
 */
void cir_enable(struct cir_dev *dev)
{
	struct cir_device *cir = dev->cir;
	__raw_writel(GPIO_BIT(IR_SHIFT(cir->pin)), cir->mmio_base + GAPMASK0);
	__raw_writel(GPIO_BIT(IR_SHIFT(cir->pin)), cir->mmio_base + GSFER0);
	__raw_writel(GPIO_BIT(IR_SHIFT(cir->pin)), cir->mmio_base + GSRER0);
}

EXPORT_SYMBOL(cir_enable);

/**
 * cir_disable - shut down the cir - mask
 *
 * Turn off the cir port.
 */
void cir_disable(struct cir_dev *dev)
{
	struct cir_device *cir = dev->cir;

	__raw_writel(GPIO_BIT(IR_SHIFT(cir->pin)), cir->mmio_base + GCPMASK0);
}

EXPORT_SYMBOL(cir_disable);

#ifdef CONFIG_PM
/*
 * Basic power management.
 */

static int cir_suspend(struct platform_device *pdev, pm_message_t state)
{
	/* require timer services can be disabled */
	return 0;
}

static int cir_resume(struct platform_device *pdev)
{
	/* require timer services resume to be enabled */
	return 0;
}
#else
#define cir_suspend	NULL
#define cir_resume	NULL
#endif

static int __devinit cir_probe(struct platform_device *pdev)
{
	struct cir_device *cir;
	struct resource *res;
	int ret = 0;

   	cir = kzalloc(sizeof(struct cir_device), GFP_KERNEL);
   	if (cir == NULL) {
      		dev_err(&pdev->dev, "failed to allocate memory\n");
      		return -ENOMEM;
   	}
	
	/* Allocate an input device data structure */
	cir->input_dev = input_allocate_device();
	if (!cir->input_dev) {
		kfree(cir);
		return -ENOMEM;
	}

	/* Allocate GPIO memory range */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		dev_err(&pdev->dev,"no memory resource defined\n");
		input_free_device(cir->input_dev);
		kfree(cir);
		ret = -ENODEV;
	}
	cir->mmio_base = ioremap_nocache(res->start, SZ_256);
	dev_dbg(&pdev->dev, "cir->address 0x%p", cir->mmio_base);

	/* Figure out which GPIO IRQ to use */
	cir->irq = platform_get_irq(pdev, 0);
	dev_dbg(&pdev->dev, "cir->irq : %d \n", cir->irq);
	if (cir->irq < 0) {
		dev_err(&pdev->dev, "no IRQ resource defined\n");
		ret = -ENODEV;
		goto err_free_io;
	}

	/* Actually set up the IRQ */
	ret = request_irq(cir->irq, cir_interrupt,
			  IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
			  "IR port", cir);
	dev_dbg(&pdev->dev, "ret from request irq. %d\n", ret);
   	if (ret) {
		dev_err(&pdev->dev, "Unable to request IRQ: %d\n", ret);
		goto err_free_io;
	}

	cir->pin = IRQ_TO_GPIO(cir->irq);

	platform_set_drvdata(pdev, cir);
	dev_dbg(&pdev->dev, " Initialize CIR driver completed \n");

	/* setup input device */
	cir->input_dev->name = "aspenite_cir";
	cir->input_dev->phys = "aspenite_cir/input2";
	cir->input_dev->dev.parent = &pdev->dev;
	cir->input_dev->evbit[0] = BIT(EV_KEY);
	cir->pdev = pdev->dev.platform_data;

	/* Announce that the CIR will generate  key map */
	ir_key = MV_IR_KEY_NULL;
	for (i=0; i< 33;) {
		ir_key = ir_key_table[i].ir_key;
		set_bit(ir_key, cir->input_dev->keybit);
		i++;
	}
	dev_dbg(&pdev->dev, " key map was set \n");

	/* Register with the input subsystem */
	if (input_register_device(cir->input_dev)) 
		dev_dbg(&pdev->dev, "can't register CIR_input Driver.\n");
	dev_dbg(&pdev->dev, "CIR_input Driver Initialized.\n");

	return 0;
	
err_free_io:
	if (cir && cir->irq > 0)
		free_irq(cir->irq, cir);
	if (cir && cir->input_dev)
		input_free_device(cir->input_dev);
	if (cir)
		kfree(cir);
	return ret;
}

static int __devexit cir_remove(struct platform_device *pdev)
{
	struct cir_device *cir;
	
	cir = platform_get_drvdata(pdev);
	if (cir == NULL)
		return -ENODEV;

	if (cir->irq > 0)
		free_irq(cir->irq, cir);

	if (cir->input_dev)
		input_unregister_device(cir->input_dev);

	kfree(cir);
	return 0;
}

static int __devinit aspenite_cir_probe(struct platform_device *pdev)
{
	return cir_probe(pdev);
}

static struct platform_driver aspenite_cir_driver = {
	.driver		= {
		.name	= "aspenite-cir",
	},
	.probe		= aspenite_cir_probe,
	.remove		= __devexit_p(cir_remove),
	.suspend	= cir_suspend,
	.resume		= cir_resume,
};

/**
 * cir_init - setup the cir port
 *
 * initialise  cir.
 *
 */

int __init cir_init(struct cir_dev *dev)
{
	if (gpio_request(dev->cir->pin, "IR_PIN")) {
		printk(KERN_ERR "Request GPIO failed,"
				"gpio: %d \n", dev->cir->pin);
		return -EIO;
	}

	/* Direction is input */
	gpio_direction_input(dev->cir->pin);
	return 0;
}


static int __init aspenite_cir_init(void)
{
	int ret = 0;
	ret = platform_driver_register(&aspenite_cir_driver);
	if (ret) {
		printk(KERN_ERR "failed to register aspenite_cir_driver");
		return ret;
	}
	return ret;
}

static void __exit aspenite_cir_exit(void)
{
	platform_driver_unregister(&aspenite_cir_driver);
}

late_initcall(aspenite_cir_init);
module_exit(aspenite_cir_exit);
