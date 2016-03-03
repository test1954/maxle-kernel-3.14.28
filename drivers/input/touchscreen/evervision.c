
/*
  22.09.2015  - taken from 
  https://raw.githubusercontent.com/jmore-reachtech/reach-imx-linux/reach-imx-3.10.17_1.0.2/drivers/input/touchscreen/evervision.c

  https://github.com/jmore-reachtech/reach-imx-linux/blob/reach-imx_3.10.17_1.0.2_ads7846/arch/arm/boot/dts/imx6dl-g2h-13.dts
*/
#define DEBUG
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/debugfs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/semaphore.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/fs.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>

#define DEV_NAME    		"evervision-i2c"

#define NUM_FINGERS_SUPPORTED               10
#define MSG_SIZE                            62

#define X_MIN                               0x00
#define Y_MIN                               0x00
#define X_MAX                               0xFFF
#define Y_MAX                               0xFFF
#define EVERVISION_MAX_REPORTED_PRESSURE    10
#define EVERVISION_MAX_TOUCH_SIZE           100
#define EVERVISION_DRIVER_VERSION           1

#define EVERVISION_ReadResponseMsg_Noaddress       1
#define EVERVISION_ReadResponseMsg                 2
#define EVERVISION_ReadSendCMDgetCRCMsg            3
#define EVERVISION_HandleTouchMsg                  4

#define OFFSET_CID		0
#define OFFSET_STATUS	0
#define OFFSET_X_L		1
#define OFFSET_Y_L		2
#define OFFSET_X_H		3
#define OFFSET_Y_H		3
#define OFFSET_PRESSURE	4
#define OFFSET_AREA		5

#define MULTITOUCH_INT_GPIO		MXS_PIN_ENCODE(0x2, 19) /* bank2 starts at 64 and this is pin 19*/

struct point_data {
        short Status;
        short X;
        short Y;
        short Pressure;
        short Area;
};

struct evervision_data
{
	struct i2c_client    *client;
	struct input_dev     *input;
	struct semaphore     sema;
	struct workqueue_struct *wq;
	struct work_struct work;
	int irq;
	short irq_type;
	struct point_data PointBuf[NUM_FINGERS_SUPPORTED];
	#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
	#endif
	int dev_major;
	int dev_minor;
	struct cdev *dev_cdevp;
	struct class *dev_classp;
	int update_mode;
	int x_range;
	int y_range;
	int orient;
	int backup_rc;
	int en_water_proof;
	int fw_ver[16];
	int delta;
};

//static int LastUpdateID = 0;
static struct evervision_data *evervision_gpts = NULL;

int      evervision_release(struct inode *, struct file *);
int      evervision_open(struct inode *, struct file *);
ssize_t  evervision_write(struct file *file, const char *buf, size_t count, loff_t *ppos);
ssize_t  evervision_read(struct file *file, char *buf, size_t count, loff_t *ppos);
long	 evervision_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static struct cdev evervision_cdev;
static struct class *evervision_class;
static int evervision_major = 0;

int evervision_open(struct inode *inode, struct file *filp)
{
	return 0;
}
EXPORT_SYMBOL(evervision_open);

int  evervision_release(struct inode *inode, struct file *filp)
{
	return 0;
}
EXPORT_SYMBOL(evervision_release);

ssize_t evervision_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
	int ret;
	char *tmp;

	if (count > 8192)
		count = 8192;

	tmp = (char *)kmalloc(count,GFP_KERNEL);
	if (tmp==NULL)
		return -ENOMEM;
	if (copy_from_user(tmp,buf,count)) {
		kfree(tmp);
		return -EFAULT;
	}
	printk("writing %zu bytes.\n", count);

	ret = i2c_master_send(evervision_gpts->client, tmp, count);
	kfree(tmp);
	return ret;
}
EXPORT_SYMBOL(evervision_write);

ssize_t evervision_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
	char *tmp;
	int ret;

	if (count > 8192)
		count = 8192;

	tmp = (char *)kmalloc(count,GFP_KERNEL);
	if (tmp==NULL)
		return -ENOMEM;

	printk("reading %zu bytes.\n", count);

	ret = i2c_master_recv(evervision_gpts->client, tmp, count);
	if (ret >= 0)
		ret = copy_to_user(buf,tmp,count)?-EFAULT:ret;
	kfree(tmp);
	return ret;
}
EXPORT_SYMBOL(evervision_read);

long evervision_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return 0;
}
EXPORT_SYMBOL(evervision_ioctl);

static inline int evervision_parser_packet(u8 *buff, struct evervision_data *evervision)
{
	//int i, FID, 
    int POINTS;
	//short ContactID = -1;
    int X, Y;
    int EVENT;

    POINTS = buff[2] & 0x0F;
    Y = ((buff[3] & 0x0F) << 8) | (buff[4]);
    X = ((buff[5] & 0x0F) << 8) | (buff[6]);
    EVENT = (buff[3] & 0xC0) >> 6;

    //printk("touches = %d (0x%02X) \n", POINTS, buff[2]);
    //printk("event = %d (0x%02X) \n", EVENT, buff[3]);
    //printk(" x=%d, y=%d \n\n", X,Y);

    /* tslib only support a single touch */
    switch (EVENT) {
        case 0:
        case 2:
            input_report_abs(evervision->input, ABS_X, X);
            input_report_abs(evervision->input, ABS_Y, Y);
            //input_report_abs(evervision->input, ABS_PRESSURE, 255);
            input_report_key(evervision->input, BTN_TOUCH, 1);
            input_sync(evervision->input);
            break;
        case 1:
            input_report_abs(evervision->input, ABS_X, X );   // maxle
            input_report_abs(evervision->input, ABS_Y, Y );   // maxle
            //input_report_abs(evervision->input, ABS_PRESSURE, 0);
            input_report_key(evervision->input, BTN_TOUCH, 0);
            input_sync(evervision->input);
            break;
        default:
            printk("Invalid EVENT: %d \n", EVENT);
            goto ERR_PACKET;
    }

	return 0;

ERR_PACKET:
	printk("ERR PACKET\n");
	return -1;
}

static inline int evervision_read_block_onetime(struct i2c_client *client, u16 length, u8 *value)
{
	struct evervision_data *evervision;
	int rc = 0;

	evervision = i2c_get_clientdata(client);

	rc = i2c_master_recv(evervision->client, value, length);

	if (rc == length) {
		return length;
	} else {
		mdelay(10);
		printk("Evervision: i2c read failed\n");
		return -1;
	}
}

static void evervision_worker(struct work_struct *work)
{
	struct evervision_data *evervision = container_of(work, struct evervision_data, work);
	u8 buffer[MSG_SIZE] = {0}; 
	//int i;

	if(evervision_read_block_onetime(evervision->client, MSG_SIZE, &buffer[0]) < 0) {
		printk("Evervision: evervision_read_block failed, try again\n"); 
	} 

	#if 0 
	for (i=0; i<MSG_SIZE; i++) {
		if(i==0 || i==7 || i==14 || i==21 || i==28 || i==35 || i==42 || i==49 || i==56)
			printk("\nfinger[%d] : ", (i/6)+1);
		printk("0x%02x ", buffer[i]);
	}
	#endif

	evervision_parser_packet(buffer,evervision);

	enable_irq(evervision->client->irq);
}

static irqreturn_t evervision_irq_handler(int irq, void *dev_id)
{
	struct evervision_data *ts = dev_id;

	disable_irq_nosync(ts->client->irq);
    schedule_work(&ts->work);

	return IRQ_HANDLED;
}

static int evervision_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct evervision_data *pdata;
    int error = 0;
    int ret = 0; 
    uint16_t max_x = 0, max_y = 0;

    pr_debug("%s: irq = %d \n", __func__, client->irq);

    pdata = kzalloc(sizeof(struct evervision_data), GFP_KERNEL);
    if (pdata == NULL) {
		printk("Evervision: insufficient memory\n");
		error = -ENOMEM;
		goto err_evervision_alloc;
	}

    INIT_WORK(&pdata->work, evervision_worker);
    pdata->client = client;
    i2c_set_clientdata(client, pdata);  

   	ret = request_irq(client->irq, evervision_irq_handler
        , IRQF_TRIGGER_FALLING | IRQF_DISABLED, client->name, pdata);
	if (ret == 0)
        pr_debug("%s: using IRQ \n", __func__);
	else
		dev_err(&client->dev, "request_irq failed\n");

	evervision_gpts = kzalloc(sizeof(*pdata), GFP_KERNEL);

	pdata->input = input_allocate_device();
	if (pdata->input == NULL){
		ret = -ENOMEM;
		goto err_input_dev_alloc_failed;
	}

	pdata->input->name = "evervision-ts";
	//set_bit(EV_KEY, pdata->input->evbit);
	//set_bit(EV_ABS, pdata->input->evbit);
	//set_bit(BTN_TOUCH, pdata->input->keybit);
	pdata->input->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS); //maxle
	pdata->input->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH); //maxle


	max_x = 2048; // maxle  pdata->x_range;
	max_y = 2048; // maxle  pdata->y_range;
 
	__set_bit(ABS_X, pdata->input->absbit);
	__set_bit(ABS_Y, pdata->input->absbit);

	input_set_abs_params(pdata->input, ABS_X, 0, max_x, 0, 0);
	input_set_abs_params(pdata->input, ABS_Y, 0, max_y, 0, 0);
	//input_set_abs_params(pdata->input, ABS_PRESSURE, 0, 10, 0, 0);  maxle

	if(input_register_device(pdata->input)){
		printk("Can not register input device.");
		goto err_input_register_device_failed;
	}
  
 
	return 0;
err_input_register_device_failed:
	input_free_device(pdata->input);
err_evervision_alloc:
	return error;

err_input_dev_alloc_failed:
    kfree(pdata);

    return ret;
}

static int evervision_ts_remove(struct i2c_client *client)
{
	struct evervision_data *evervision;

	pr_debug("%s: \n", __func__);

	evervision = i2c_get_clientdata(client);

	if (evervision != NULL) {
		if (evervision->irq)
			free_irq(gpio_to_irq(client->irq), evervision);

		input_unregister_device(evervision->input);
	}
	kfree(evervision);
	i2c_set_clientdata(client, NULL);

	return 0;
}

static const struct i2c_device_id evervision_ts_id[] =
{
	{ "evervision_ts", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, evervision_ts_id);

static struct of_device_id evervision_ts_dt_ids[] = {
	{ .compatible = "kws,evervision_ts" },
	{ /* sentinel */ }
};

static struct i2c_driver evervision_driver = {
	.driver = {
		.name	= "evervision_ts",
		.owner	= THIS_MODULE,
//		.of_match_table	= of_match_ptr(evervision_ts_dt_ids),
		.of_match_table	= evervision_ts_dt_ids,
	},
	.id_table	= evervision_ts_id,
	.probe		= evervision_ts_probe,
	.remove		= evervision_ts_remove,
};

static struct file_operations nc_fops = {
	.owner =        THIS_MODULE,
	.write		= evervision_write,
	.read		= evervision_read,
	.open		= evervision_open,
	.unlocked_ioctl = evervision_ioctl,
	.release	= evervision_release,
};

static int evervision_init(void)
{
   	int result;
	int err = 0;
	dev_t devno = MKDEV(evervision_major, 0);

    pr_debug("%s: \n", __func__);
	result  = alloc_chrdev_region(&devno, 0, 1, DEV_NAME);
	if(result < 0){
		printk("fail to allocate chrdev (%d) \n", result);
		return 0;
	}

	evervision_major = MAJOR(devno);
	cdev_init(&evervision_cdev, &nc_fops);
	evervision_cdev.owner = THIS_MODULE;
	evervision_cdev.ops = &nc_fops;
	err =  cdev_add(&evervision_cdev, devno, 1);
	if(err){
		printk("fail to add cdev (%d) \n", err);
		return 0;
	}
	
	evervision_class = class_create(THIS_MODULE, DEV_NAME);
	if (IS_ERR(evervision_class)) {
		result = PTR_ERR(evervision_class);
		unregister_chrdev(evervision_major, DEV_NAME);
		printk("fail to create class (%d) \n", result);
		return result;
	}
	device_create(evervision_class, NULL, MKDEV(evervision_major, 0), NULL, DEV_NAME);
 
	result = i2c_add_driver(&evervision_driver);
	return result;
}

static void __exit evervision_exit(void)
{
    dev_t dev_id = MKDEV(evervision_major, 0);

    printk("%s: \n", __func__);

	i2c_del_driver(&evervision_driver);

    cdev_del(&evervision_cdev);

	device_destroy(evervision_class, dev_id); //delete device node under /dev
	class_destroy(evervision_class); //delete class created by us
	unregister_chrdev_region(dev_id, 1);

}

module_init(evervision_init);
module_exit(evervision_exit);

MODULE_DESCRIPTION("Driver for Evervision Touchscreen Controller");
MODULE_LICENSE("GPL");


