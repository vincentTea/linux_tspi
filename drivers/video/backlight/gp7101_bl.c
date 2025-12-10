#include "linux/stddef.h"
#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/input/mt.h>
#include <linux/random.h>
#include <linux/backlight.h>

#if 1
#define MY_DEBUG(fmt,arg...)  printk("gp7101_bl:%s %d "fmt"",__FUNCTION__,__LINE__,##arg);
#else
#define MY_DEBUG(fmt,arg...)
#endif

#define BACKLIGHT_NAME "gp7101-backlight"

/* I2C 背光控制器寄存器定义 */
#define BACKLIGHT_REG_CTRL_8  0x03  
#define BACKLIGHT_REG_CTRL_16 0x02

/* 背光控制器设备数据结构 */
struct gp7101_backlight_data {
    struct i2c_client *client;
    
};

s32 i2c_read(struct i2c_client *client,u8 *addr,u8 addr_len, u8 *buf, s32 len)
{
    struct i2c_msg msgs[2];
    s32 ret=-1;
    msgs[0].flags = !I2C_M_RD;
    msgs[0].addr  = client->addr;
    msgs[0].len   = addr_len;
    msgs[0].buf   = &addr[0];
    msgs[1].flags = I2C_M_RD;
    msgs[1].addr  = client->addr;
    msgs[1].len   = len;
    msgs[1].buf   = &buf[0];

    ret = i2c_transfer(client->adapter, msgs, 2);
    if(ret == 2)return 0;

    if(addr_len == 2){
        MY_DEBUG("I2C Read: 0x%04X, %d bytes failed, errcode: %d! Process reset.", (((u16)(addr[0] << 8)) | addr[1]), len, ret);
    }else {
        MY_DEBUG("I2C Read: 0x%02X, %d bytes failed, errcode: %d! Process reset.", addr[0], len, ret);
    }
    
    return -1;
}

s32 i2c_write(struct i2c_client *client, u8 *addr, u8 addr_len, u8 *buf,s32 len)
{
    struct i2c_msg msg;
    s32 ret = -1;
    u8 *temp_buf;

    msg.flags = !I2C_M_RD;
    msg.addr  = client->addr;
    msg.len   = len+addr_len;

    temp_buf= kzalloc(msg.len, GFP_KERNEL);
    if (!temp_buf){
        goto error;
    }
    
    // 装填地址
    memcpy(temp_buf, addr, addr_len);
    // 装填数据
    memcpy(temp_buf + addr_len, buf, len);
    msg.buf = temp_buf;

    ret = i2c_transfer(client->adapter, &msg, 1);
    if (ret == 1) {
        kfree(temp_buf);
        return 0;
    }

error:
    if(addr_len == 2){
        MY_DEBUG("I2C Read: 0x%04X, %d bytes failed, errcode: %d! Process reset.", (((u16)(addr[0] << 8)) | addr[1]), len, ret);
    }else {
        MY_DEBUG("I2C Read: 0x%02X, %d bytes failed, errcode: %d! Process reset.", addr[0], len, ret);
    }
    if (temp_buf)
        kfree(temp_buf);
    return -1;
}

/* 设置背光亮度 */
static int gp7101_backlight_set(struct backlight_device *bl)
{
    struct gp7101_backlight_data *data = bl_get_data(bl);
    struct i2c_client *client = data->client;
    u8 addr[1] = {BACKLIGHT_REG_CTRL_8};
    u8 buf[1] = {bl->props.brightness}; 

    MY_DEBUG("pwm:%d", bl->props.brightness);

    i2c_write(client, addr, sizeof(addr), buf, sizeof(buf));

    return 0;
}

/* 背光设备操作函数 */
static struct backlight_ops gp7101_backlight_ops = {
    .update_status = gp7101_backlight_set,
};

static int gp7101_bl_probe(struct i2c_client *client,
            const struct i2c_device_id *id)
{
    struct backlight_device *bl;
    struct gp7101_backlight_data *data;
    struct backlight_properties props;
    struct device_node *np = client->dev.of_node;

    MY_DEBUG("locat");

    data = devm_kzalloc(&client->dev, sizeof(struct gp7101_backlight_data), GFP_KERNEL);
    if (data == NULL){
        dev_err(&client->dev, "Alloc GFP_KERNEL memory failed.");
        return -ENOMEM;
    } 

    memset(&props, 0, sizeof(props));
    props.type = BACKLIGHT_RAW;
    props.max_brightness = 255; 

    of_property_read_u32(np, "max-brightness-levels",
			    &props.max_brightness);

	of_property_read_u32(np, "default-brightness-level", 
                &props.brightness);

    if(props.max_brightness>255 || props.max_brightness<0){
        props.max_brightness = 255;
    }
    if(props.brightness>props.max_brightness || props.brightness<0){
        props.brightness = props.max_brightness;
    }
    /* 初始化背光设备 */
    bl = devm_backlight_device_register(&client->dev, "backlight", &client->dev, data, &gp7101_backlight_ops,&props);
    if (IS_ERR(bl)) {
        dev_err(&client->dev, "failed to register backlight device\n");
        return PTR_ERR(bl);
    }
    data->client = client;
    i2c_set_clientdata(client, data);

    MY_DEBUG("max_brightness:%d brightness:%d",props.max_brightness, props.brightness);
    backlight_update_status(bl);

    return 0;
}

static void gp7101_bl_remove(struct i2c_client *client)
{
    MY_DEBUG("locat");
    //return 0;
}

static const struct of_device_id gp7101_bl_of_match[] = {
    { .compatible = BACKLIGHT_NAME, },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gp7101_bl_of_match);


static struct i2c_driver gp7101_bl_driver = {
    .probe      = gp7101_bl_probe,
    .remove     = gp7101_bl_remove,
    .driver = {
        .name     = BACKLIGHT_NAME,
	 .of_match_table = of_match_ptr(gp7101_bl_of_match),
    },
};

static int __init my_init(void)
{
    MY_DEBUG("locat");
    return i2c_add_driver(&gp7101_bl_driver);
}

static void __exit my_exit(void)
{
    MY_DEBUG("locat");
	i2c_del_driver(&gp7101_bl_driver);
}

module_init(my_init);
module_exit(my_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Touch driver");
MODULE_AUTHOR("support@hibuffx.com");
