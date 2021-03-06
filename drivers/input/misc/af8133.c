/******************** (C) COPYRIGHT Voltafield 2014 ********************
*
* File Name          : af8133.c
* Authors            : Production, CAE Team
*                    : Gary Huang
* Date               : 2015/Jul/06
* Description        : AF7133E/AF8133i Magneto sensor Driver
*
************************************************************************
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* THE PRESENT SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES
* OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, FOR THE SOLE
* PURPOSE TO SUPPORT YOUR APPLICATION DEVELOPMENT.
* AS A RESULT, STMICROELECTRONICS SHALL NOT BE HELD LIABLE FOR ANY DIRECT,
* INDIRECT OR CONSEQUENTIAL DAMAGES WITH RESPECT TO ANY CLAIMS ARISING FROM THE
* CONTENT OF SUCH SOFTWARE AND/OR THE USE MADE BY CUSTOMERS OF THE CODING
* INFORMATION CONTAINED HEREIN IN CONNECTION WITH THEIR PRODUCTS.
*
*******************************************************************************/
/*******************************************************************************
Version History.

20150326  1st version
20150528  Qualcomm Andorid 5.1 new architecture
*******************************************************************************/

#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/delay.h>

//#include <linux/af8133.h>
#include "af8133.h"
#include <linux/sensors.h>

#define AF8133_DEBUG 0
#define YHJ_DBG 0
#if YHJ_DBG
#define Y_DBG(s,args...)	{printk("////yhj : func [%s], line [%d], ",__func__,__LINE__); printk(s,## args);}
#else
#define Y_DBG(s,args...) {}
#endif
//#define AF8133_DRIVER_VERSION     "1.0.5"
#define AF8133_DRIVER_VERSION     "2.0.0" // Byron 20150811

#define DELAY_TIME_DEFAULT         50   //ms, 20Hz
#define DELAY_TIME_MIN             40   //ms, 200Hz
#define DELAY_TIME_MAX            100   //ms, 5Hz

#define AF8133_SET_RESET_NUM      5
#define AF8133_OFFSET_INDEX       2   //(int)(AF8133_SET_RESET_NUM/2)


//static struct i2c_board_info __initdata i2c_af8133={ I2C_BOARD_INFO(AF8133_I2C_DEV_NAME, 0x1c)};  
static struct i2c_client *this_client;
struct af8133_data {
        struct i2c_client *client;
        struct af8133_platform_data *pdata;
        struct mutex lock;
        struct input_dev *input;
        struct delayed_work work;
#if 1 // Byron 20150811 , to implement dedicated workqueue
	 struct workqueue_struct	*work_queue;
#endif
        struct sensors_classdev cdev;

        atomic_t enabled;
#if 1 // Byron 20150811 , use hrtimer to improve the timer resolution.
	int	use_hrtimer;
	struct hrtimer	poll_timer;
#endif
};

static const struct af8133_platform_data default_af8133_pdata = {
        .poll_interval = DELAY_TIME_DEFAULT,
        .axis_map_x = 0,
        .axis_map_y = 1,
        .axis_map_z = 2,
        .negate_x = 0, // negated data , 20150811 , Byron
        .negate_y = 0, // negated data
        .negate_z = 0, // negated data
        .reg33 =0,
};

static struct sensors_classdev sensors_cdev = {
        .name = "af8133-mag",
        .vendor = "VTC, Inc",
        .version=2, /*.version = 1,  */ // 20150811 Byron
        .handle = SENSORS_MAGNETIC_FIELD_HANDLE,
        .type = SENSOR_TYPE_MAGNETIC_FIELD,
        .max_range = "2200",
        .resolution = "0.036",
        .sensor_power = "0.05",
        .min_delay = DELAY_TIME_MIN*1000, //us
        .max_delay = DELAY_TIME_MAX*1000, //us
//        .min_delay = DELAY_TIME_MIN*1000000, //ns
//        .max_delay = DELAY_TIME_MAX*1000000, //ns
        .fifo_reserved_event_count = 0,
        .fifo_max_event_count = 0,
        .enabled = 0,
        .delay_msec = DELAY_TIME_DEFAULT,
        .sensors_enable = NULL,
        .sensors_poll_delay = NULL,
};

static int VTC_i2c_Rx(char *rxData, int length)
{
        uint8_t retry;
        struct i2c_msg msgs[] = 
        {
                {
                        .addr = this_client->addr,
                        .flags = 0,
                        .len = 1,
                        .buf = rxData,
                },
                {
                        .addr = this_client->addr,
                        .flags = I2C_M_RD,
                        .len = length,
                        .buf = rxData,
                },
        };

        for (retry = 0; retry < 3; retry++) 
        {
                if (i2c_transfer(this_client->adapter, msgs, 2) > 0)
                        break;
                else
                        mdelay(10);
        }

        if (retry >= 3) 
        {
                printk(KERN_ERR "%s: retry over 3\n", __func__);
                return -EIO;
        } 
        else
                return 0;
}

static int VTC_i2c_Tx(char *txData, int length)
{
        int retry;
        struct i2c_msg msg[] = 
        {
                {
                        .addr = this_client->addr,
                        .flags = 0,
                        .len = length,
                        .buf = txData,
                },
        };

        for (retry = 0; retry <= 3; retry++) 
        {
                if (i2c_transfer(this_client->adapter, msg, 1) > 0)
                        break;
                else
                        mdelay(10);
        }

        if (retry > 3) 
        {
                printk(KERN_ERR "%s: retry over 3\n", __func__);
                return -EIO;
        }
        else
                return 0;
}


static int af8133_measurement_setting(u8 reg14, u8 reg33)
{
  int err;
  u8  data[2];

  //full measurement, set/reset setting 
  data[0]= 0x14;
  data[1]= reg14;
  err = VTC_i2c_Tx(data, 2);
  if(err < 0) return -1;

  //field range +/-1200uT
  data[0]= 0x33;
  data[1]= reg33;
  err = VTC_i2c_Tx(data, 2);
  if(err < 0) return -1;

  //field range +/-1200uT
  data[0]= 0x0B;
  data[1]= 0x3C; 
  err = VTC_i2c_Tx(data, 2);
  if(err < 0) return -1;

  //disable moving average
  data[0]= 0x13;
  data[1]= 0x00; 
  err = VTC_i2c_Tx(data, 2);
  if(err < 0) return -1;

  return 0;
}

static int af8133_mag_get_data(struct af8133_data *mag)
{
  int err;
  u8  data[6];
  int i;
  int xyz[3];
  ktime_t timestamp;

  data[0] = 0x03;
  err = VTC_i2c_Rx(data, 6);
  if(err < 0) goto err_i2c_fail;

  xyz[0] = data[0] | (data[1] << 8); 
  xyz[1] = data[2] | (data[3] << 8);
  xyz[2] = data[4] | (data[5] << 8);

  timestamp = ktime_get_boottime();

  for(i=0;i<3;i++) xyz[i] = (xyz[i] > 32767) ? (xyz[i] - 65536) : xyz[i];

  for(i=0;i<3;i++) xyz[i] -= mag->pdata->offset[i];  
        
  err = af8133_measurement_setting(0x38, mag->pdata->reg33);
  if(err < 0) goto err_i2c_fail;      

  //next read
  data[0] = 0x0A;
  data[1] = 0x01; 
  err = VTC_i2c_Tx(data, 2);
  if(err < 0) goto err_i2c_fail; 

  input_report_abs(mag->input, ABS_X, xyz[0]);
  input_report_abs(mag->input, ABS_Y, xyz[1]);
  input_report_abs(mag->input, ABS_Z, xyz[2]);
	input_event(mag->input, EV_SYN, SYN_TIME_SEC, ktime_to_timespec(timestamp).tv_sec);
	input_event(mag->input, EV_SYN, SYN_TIME_NSEC, ktime_to_timespec(timestamp).tv_nsec);
  input_sync(mag->input);

  return 0;

err_i2c_fail:
  printk(KERN_ERR "%s:failed\n", __func__);
  return err;
}

static int af8133_i2c_init(struct af8133_data *mag)
{
  int err;
  int i, j, k;
  int mag_pos[3][5], mag_neg[3][5];
  u8  data[6];

  //*********************************
  //get calibration parameter
  //*********************************
  data[0]= 0x14;
  data[1]= 0x3C; 
  err = VTC_i2c_Tx(data, 2);
  if(err < 0) return -1;    

  data[0] = 0x1F;
  err = VTC_i2c_Rx(data, 1);
  if(err < 0) return -1;    

  if(data[0] & 0x04) mag->pdata->reg33 = 0x14;
  else if(data[0] & 0x02) mag->pdata->reg33 = 0x17;
  else if(data[0] & 0x01) mag->pdata->reg33 = 0x15;
  else mag->pdata->reg33 = 0x16; 

  //*********************************
  //check product code
  //*********************************
  data[0] = 0x00;
  err = VTC_i2c_Rx(data, 1);
  if(err < 0) return -1;

  if(data[0] != 0x50) return -2;

  //*********************************
  //get offset by Set/Reset
  //*********************************
  err = af8133_measurement_setting(0x34, mag->pdata->reg33);
  if(err < 0) return err; 

  //full measurement, reset only 
  for(i=0;i<AF8133_SET_RESET_NUM;i++)
  {                   
    data[0]= 0x0A;
    data[1]= 0x01; 
    err = VTC_i2c_Tx(data, 2);
    if(err < 0) return -1;

    mdelay(2);

    data[0]= 0x03; 
    err = VTC_i2c_Rx(data, 6);
    if(err < 0) return -1;

    mag_neg[0][i] = data[0] | (data[1] << 8); 
    mag_neg[1][i] = data[2] | (data[3] << 8);
    mag_neg[2][i] = data[4] | (data[5] << 8);

    for(j=0;j<3;j++) mag_neg[j][i] = (mag_neg[j][i] > 32767) ? (mag_neg[j][i] - 65536) : mag_neg[j][i];
  }

  //full measurement, set only
  err = af8133_measurement_setting(0x38, mag->pdata->reg33);
  if(err < 0) return err; 

  for(i=0;i<AF8133_SET_RESET_NUM;i++)
  {                   
    data[0]= 0x0A;
    data[1]= 0x01; 
    err = VTC_i2c_Tx(data, 2);
    if(err < 0) return -1;

    mdelay(2);

    data[0]= 0x03; 
    err = VTC_i2c_Rx(data, 6);
    if(err < 0) return -1;

    mag_pos[0][i] = data[0] | (data[1] << 8); 
    mag_pos[1][i] = data[2] | (data[3] << 8);
    mag_pos[2][i] = data[4] | (data[5] << 8);

    for(j=0;j<3;j++) mag_pos[j][i] = (mag_pos[j][i] > 32767) ? (mag_pos[j][i] - 65536) : mag_pos[j][i];
  }

  //sort data
  for(i=0;i<3;i++){    
    for(j=0;j<4;j++){
      for(k=0;k<4;k++){
        if(mag_neg[i][k] < mag_neg[i][k+1]){
          int tmp = mag_neg[i][k];
          mag_neg[i][k] = mag_neg[i][k+1];
          mag_neg[i][k+1] = tmp;
         }
        if(mag_pos[i][k] < mag_pos[i][k+1]){
          int tmp = mag_pos[i][k];
          mag_pos[i][k] = mag_pos[i][k+1];
          mag_pos[i][k+1] = tmp;
         } 
       }
     }
   }

  for(i=0;i<3;i++)
    mag->pdata->offset[i] = (mag_pos[i][AF8133_OFFSET_INDEX]+mag_neg[i][AF8133_OFFSET_INDEX])/2; 

  return 0;
}

static ssize_t attr_get_polling_rate(struct device *dev,
                                     struct device_attribute *attr,
                                     char *buf)
{
        struct af8133_data *mag = dev_get_drvdata(dev);
       unsigned long interval_ms;
       mutex_lock(&mag->lock);
        interval_ms = mag->pdata->poll_interval;
        mutex_unlock(&mag->lock);
	return sprintf(buf, "%ld\n", interval_ms);
}

static ssize_t attr_set_polling_rate(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t size)
{
        struct af8133_data *mag = dev_get_drvdata(dev);
        unsigned long interval_ms;

        if (strict_strtoul(buf, 10, &interval_ms))
                return -EINVAL;
        if (!interval_ms)
                return -EINVAL;

	Y_DBG(" %s -before, interval_ms=%lu.\n",__func__,interval_ms);

	if(interval_ms > DELAY_TIME_MAX)
        	interval_ms = DELAY_TIME_MAX;
        else if(interval_ms < DELAY_TIME_MIN)
        	interval_ms = DELAY_TIME_MIN;

	Y_DBG("%s -after, interval_ms=%lu.\n",__func__,interval_ms);
	
        mutex_lock(&mag->lock);
        mag->pdata->poll_interval = interval_ms;
        mutex_unlock(&mag->lock);

        if(atomic_read(&mag->enabled))
         schedule_delayed_work(&mag->work, msecs_to_jiffies(mag->pdata->poll_interval));

        return size;
}

static ssize_t attr_get_enable(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
        struct af8133_data *mag = dev_get_drvdata(dev);
        int val = atomic_read(&mag->enabled);
        return sprintf(buf, "%d\n", val);
}

static ssize_t attr_set_enable(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf, size_t size)
{
       struct af8133_data *mag = dev_get_drvdata(dev);
       unsigned int pre_en = atomic_read(&mag->enabled);
       unsigned int new_en = 0;

        //if (strict_strtoul(buf, 10, &new_en))
        //  return -EINVAL;
        if (sysfs_streq(buf, "1"))
                new_en = 1;
        else if (sysfs_streq(buf, "0"))
                new_en = 0;

        if (new_en != pre_en)
        {
         if(new_en)
          {
           schedule_delayed_work(&mag->work, msecs_to_jiffies(mag->pdata->poll_interval));
           atomic_set(&mag->enabled, 1);
          }
         else
          {
           cancel_delayed_work_sync(&mag->work);
           atomic_set(&mag->enabled, 0);
          }
        }

        return size;
}


static struct device_attribute attributes[] = {
        __ATTR(pollrate_ms, 0777, attr_get_polling_rate, attr_set_polling_rate),
        __ATTR(enable_device, 0777, attr_get_enable, attr_set_enable),
};

static int create_sysfs_interfaces(struct device *dev)
{
        int i;
        for (i = 0; i < ARRAY_SIZE(attributes); i++)
                if (device_create_file(dev, attributes + i))
                        goto error;
        return 0;

error:
        for ( ; i >= 0; i--)
                device_remove_file(dev, attributes + i);
        dev_err(dev, "%s:Unable to create interface\n", __func__);
        return -1;
}

static int remove_sysfs_interfaces(struct device *dev)
{
        int i;
        for (i = 0; i < ARRAY_SIZE(attributes); i++)
                device_remove_file(dev, attributes + i);
        return 0;
}

static void af8133_work_func(struct work_struct *work)
{
        struct af8133_data *mag = container_of((struct delayed_work *)work, struct af8133_data, work);

        int err = af8133_mag_get_data(mag);
        if (err < 0)
          dev_err(&mag->client->dev, "get magnetometer data failed\n");
#if 0 // 20150811 Byron
       schedule_delayed_work(&mag->work, msecs_to_jiffies(mag->pdata->poll_interval));
#else
	if (!mag->use_hrtimer)
		queue_delayed_work(mag->work_queue, &mag->work,msecs_to_jiffies(mag->pdata->poll_interval));	
#endif
}

static int af8133_mag_input_init(struct af8133_data *mag)
{
       int err;
       struct input_dev *dev;
  
        dev = input_allocate_device();

        if (!dev) {
          return -ENOMEM;
        }

        dev->id.bustype = BUS_I2C;
        dev->name = AF8133_INPUT_DEV_NAME;

        set_bit(EV_ABS, dev->evbit);
        input_set_abs_params(dev, ABS_X, -32768, 32767, 0, 0);
        input_set_abs_params(dev, ABS_Y, -32768, 32767, 0, 0);
        input_set_abs_params(dev, ABS_Z, -32768, 32767, 0, 0);

       input_set_drvdata(dev, mag);

        err = input_register_device(dev);
       if (err < 0) {
         input_free_device(dev);
         return err;
        }

       mag->input = dev;

        return 0;
}

static int af8133_set_enable(struct sensors_classdev *sensors_cdev,
                unsigned int enable)
{
        int rc = 0;
	 ktime_t ktime;
        struct af8133_data *mag = container_of(sensors_cdev,
                        struct af8133_data, cdev);

       int pre_en = atomic_read(&mag->enabled);
       int new_en = enable;
	printk("!!!%s enable=%d !\n",__func__,enable);
        mutex_lock(&mag->lock);
        if (new_en != pre_en)
        {
         if(new_en)
          {
 	#if 0 // Byron 20150811
           schedule_delayed_work(&mag->work, msecs_to_jiffies(mag->pdata->poll_interval));
	#else
		if (mag->use_hrtimer){
			ktime = ktime_set(0,
					mag->pdata->poll_interval * NSEC_PER_MSEC);
			hrtimer_start(&mag->poll_timer,ktime,HRTIMER_MODE_REL);
		}else
		       //schedule_delayed_work(&mag->work, msecs_to_jiffies(mag->pdata->poll_interval));
			queue_delayed_work(mag->work_queue, &mag->work,msecs_to_jiffies(mag->pdata->poll_interval));
	#endif
		   
           atomic_set(&mag->enabled, 1);
          }
         else
          {
	#if 0 // Byron 20150811
           cancel_delayed_work_sync(&mag->work);
	#else
		if (mag->use_hrtimer) {
			hrtimer_cancel(&mag->poll_timer);
			cancel_work_sync(&mag->work.work);
			} else {
			cancel_delayed_work_sync(&mag->work);
		}
	#endif
           atomic_set(&mag->enabled, 0);
          }
        }
        mutex_unlock(&mag->lock);

        return rc;
}

//static int af8133_set_poll_delay(struct sensors_classdev *sensors_cdev,
//                unsigned int interval_ns)
static int af8133_set_poll_delay(struct sensors_classdev *sensors_cdev,
                unsigned int interval_ms)
{
        struct af8133_data *mag = container_of(sensors_cdev,
                        struct af8133_data, cdev);
        //unsigned long interval_ms = interval_ns/1000000;

	Y_DBG("%s -before, interval_ms=%u.\n",__func__,interval_ms);

        if (!interval_ms)
                return -EINVAL;

        if(interval_ms > DELAY_TIME_MAX)
       	  interval_ms = DELAY_TIME_MAX;
        else if(interval_ms < DELAY_TIME_MIN)
       	  interval_ms = DELAY_TIME_MIN;

	printk("%s -after, interval_ms=%u.\n",__func__,interval_ms);

        mutex_lock(&mag->lock);
        mag->pdata->poll_interval = interval_ms;
        mutex_unlock(&mag->lock);

        if(atomic_read(&mag->enabled))
         schedule_delayed_work(&mag->work, msecs_to_jiffies(mag->pdata->poll_interval));


        return 0;
}

#if 1 // Byron 20150811
static enum hrtimer_restart VTC_timer_func(struct hrtimer *timer)
{
	struct af8133_data *mag;

	mag = container_of(timer, struct af8133_data, poll_timer);

	queue_work(mag->work_queue, &mag->work.work);
	hrtimer_forward_now(&mag->poll_timer,ns_to_ktime(mag->pdata->poll_interval));

	return HRTIMER_RESTART;
}
#endif

static int af8133_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
        struct af8133_data *mag;
        int err;

        pr_info("%s: driver version is %s\n", __func__, AF8133_DRIVER_VERSION);

        if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) 
        {
                dev_err(&client->dev, "i2c_check_functionality error\n");
                err = -ENODEV;
                goto err_check_func;
        }

        mag = kzalloc(sizeof(struct af8133_data), GFP_KERNEL);
        if (!mag) {
                dev_err(&client->dev, "failed to allocate memory for module data\n");
                err = -ENOMEM;
                goto err_alloc_mag_data;
        }

        mutex_init(&mag->lock);
        mutex_lock(&mag->lock);
        mag->client = client;

        mag->pdata = kmalloc(sizeof(*mag->pdata), GFP_KERNEL);
        if (!mag->pdata) 
                goto err_alloc_mag_pdata;

        if (client->dev.platform_data == NULL) {
                memcpy(mag->pdata, &default_af8133_pdata, sizeof(*mag->pdata));
        } else {
                memcpy(mag->pdata, client->dev.platform_data, sizeof(*mag->pdata));
        }

        i2c_set_clientdata(client, mag);
        this_client = mag->client;

        err = af8133_i2c_init(mag);
        if (err < 0)
          goto err_alloc_mag_pdata;               

        err = af8133_mag_input_init(mag);
        if (err < 0)
          goto err_input_init;

        err = create_sysfs_interfaces(&client->dev);
        if (err < 0) {
                dev_err(&client->dev, "%s create sysfs interfacce failed\n", AF8133_I2C_DEV_NAME);
                goto err_clean_input;
        }

       /* workqueue init */
#if 0 // 20150811 , Byron
       INIT_DELAYED_WORK(&mag->work, af8133_work_func);
#else
	if (mag->use_hrtimer) {
		hrtimer_init(&mag->poll_timer, CLOCK_MONOTONIC,HRTIMER_MODE_REL);
		mag->poll_timer.function = VTC_timer_func;
		mag->work_queue = alloc_workqueue("VTC_poll_work",WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_HIGHPRI, 1);
		INIT_WORK(&mag->work.work, af8133_work_func);
		} else {
		mag->work_queue = alloc_workqueue("VTC_poll_work",WQ_NON_REENTRANT, 0);
		INIT_DELAYED_WORK(&mag->work, af8133_work_func);
		}	
#endif
        mag->cdev = sensors_cdev;
        mag->cdev.sensors_enable = af8133_set_enable;
        mag->cdev.sensors_poll_delay = af8133_set_poll_delay;
        err = sensors_classdev_register(&mag->input->dev, &mag->cdev);
        if (err) {
                dev_err(&client->dev, "sensors class register failed.\n");
                goto err_clean_input;
        }

        mutex_unlock(&mag->lock);
        dev_info(&client->dev, "af8133 probed\n");
        
        atomic_set(&mag->enabled, 0);

        return 0;

err_clean_input:
        input_unregister_device(mag->input);
err_input_init:
        kfree(mag->pdata);
err_alloc_mag_pdata:
        kfree(mag);
err_alloc_mag_data:
err_check_func:
        pr_err("%s: AF8133 Driver Initialization failed\n", __func__);
        i2c_set_clientdata(client, NULL);
        return err;
}


static int af8133_remove(struct i2c_client *client)
{
        struct af8133_data *mag = i2c_get_clientdata(client);

        if( mag == NULL )
                return 0;

       pr_info("%s: af8133 remove\n", __func__);

#if 1 // 20150811 Byron
	if (mag->use_hrtimer) {
		hrtimer_cancel(&mag->poll_timer);
		cancel_work_sync(&mag->work.work);
	} else {
		cancel_delayed_work_sync(&mag->work);
	}
	destroy_workqueue(mag->work_queue);
#endif
        sensors_classdev_unregister(&mag->cdev);
        remove_sysfs_interfaces(&client->dev);
       input_unregister_device(mag->input);

       kfree(mag->pdata);
        kfree(mag);

        return 0;
}

static int af8133_suspend(struct device *dev)
{
       struct af8133_data *mag = dev_get_drvdata(dev);

        if( mag == NULL )
                return 0;
	#if 0  // Byron 20150811
       if (atomic_read(&mag->enabled))
         cancel_delayed_work_sync(&mag->work);
	#else
	if (atomic_read(&mag->enabled)){
		if (mag->use_hrtimer)
			hrtimer_cancel(&mag->poll_timer);
		else
			cancel_delayed_work_sync(&mag->work);
	}
	#endif

       pr_info("%s: af8133 suspend\n", __func__);
        return 0;
}

static int af8133_resume(struct device *dev)
{
        struct af8133_data *mag = dev_get_drvdata(dev);
	 ktime_t ktime;
        if( mag == NULL )
                return 0;
#if 0 // 20150815 Byron
       if (atomic_read(&mag->enabled))
         schedule_delayed_work(&mag->work, msecs_to_jiffies(mag->pdata->poll_interval));
#else
	if(atomic_read(&mag->enabled)){
		if (mag->use_hrtimer){
			ktime = ktime_set(0,
					mag->pdata->poll_interval * NSEC_PER_MSEC);
			hrtimer_start(&mag->poll_timer,ktime,HRTIMER_MODE_REL);
			}else
			queue_delayed_work(mag->work_queue, &mag->work,msecs_to_jiffies(mag->pdata->poll_interval));
		}
#endif

        pr_info("%s: af8133 resume\n", __func__);
        return 0;
}

static const struct i2c_device_id af8133_i2c_id[] = {
        {AF8133_I2C_DEV_NAME, 0},
        {},
};

//MODULE_DEVICE_TABLE(i2c, af8133_mag_id);

static struct dev_pm_ops af8133_pm = {
        .suspend = af8133_suspend,
        .resume = af8133_resume,
};

static struct of_device_id af8133_match_table[] = {
	{ .compatible = "vtc,af8133", },
	{},
};

static struct i2c_driver af8133_driver = {
        .driver = {
                        .owner = THIS_MODULE,
                        .name = AF8133_I2C_DEV_NAME,
			                  .of_match_table = af8133_match_table,
                        .pm = &af8133_pm,
            },
        .probe = af8133_probe,
        .remove = af8133_remove,
        .id_table = af8133_i2c_id,
};


static int __init af8133_init(void)
{
        pr_info("%s: af8133 3-axis megnetometer driver: init\n", __func__);
                                    
        //struct i2c_adapter *adapter;                                     
        //struct i2c_client *client;                                       

        //adapter = i2c_get_adapter(2);                                                        
        //client = i2c_new_device(adapter, &i2c_af8133);                         

        return i2c_add_driver(&af8133_driver);
}

static void __exit af8133_exit(void)
{
        pr_info("%s: af8133 3-axis megnetometer driver: exit\n", __func__);
        i2c_del_driver(&af8133_driver);
        return;
}

module_init(af8133_init);
module_exit(af8133_exit);

//MODULE_DESCRIPTION("af8133 3-axis magnetometer driver");
//MODULE_AUTHOR("Voltafield");
//MODULE_LICENSE("GPL");
//MODULE_VERSION(AF8133_DRIVER_VERSION);
