/*
 * Copyright (C) 2015 Balázs Triszka <balika011@protonmail.ch>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pwm.h>
#include <linux/of_gpio.h>
#include <linux/err.h>

#include "../staging/android/timed_output.h"

struct isa1000_vib {
	int gpio_isa1000_en;
	int gpio_haptic_en;
	int timeout;
	int pwm_channel;
	unsigned int pwm_frequency;
	int pwm_duty_percent;
	struct pwm_device *pwm;
	struct work_struct work;
	struct mutex lock;
	struct hrtimer vib_timer;
	struct timed_output_dev timed_dev;
	int state;
};

static struct isa1000_vib vib_dev = {
	.pwm_frequency = 25000,
	.pwm_duty_percent = 100,
};

static int isa1000_set_state(struct isa1000_vib *vib, int on)
{
	if (on) {
		int rc;
		unsigned int pwm_period_ns = NSEC_PER_SEC / vib->pwm_frequency;

		rc = pwm_config(vib->pwm, (pwm_period_ns * vib->pwm_duty_percent) / 100, pwm_period_ns);
		if (rc < 0) {
			pr_err("Unable to config pwm\n");
			return rc;
		}

		rc = pwm_enable(vib->pwm);
		if (rc < 0) {
			pr_err("Unable to enable pwm\n");
			return rc;
		}

		gpio_set_value_cansleep(vib->gpio_isa1000_en, 1);
	} else {
		gpio_set_value_cansleep(vib->gpio_isa1000_en, 0);
		pwm_disable(vib->pwm);
	}

	return 0;
}

static void isa1000_enable(struct timed_output_dev *dev, int value)
{
	struct isa1000_vib *vib = container_of(dev, struct isa1000_vib, timed_dev);

	mutex_lock(&vib->lock);
	hrtimer_cancel(&vib->vib_timer);

	if (value == 0)
		vib->state = 0;
	else {
		vib->state = 1;
		value = value > vib->timeout ? vib->timeout : value;
		hrtimer_start(&vib->vib_timer, ktime_set(value / 1000, (value % 1000) * 1000000), HRTIMER_MODE_REL);
	}

	mutex_unlock(&vib->lock);
	schedule_work(&vib->work);
}

static void isa1000_update(struct work_struct *work)
{
	struct isa1000_vib *vib = container_of(work, struct isa1000_vib, work);
	isa1000_set_state(vib, vib->state);
}

static int isa1000_get_time(struct timed_output_dev *dev)
{
	struct isa1000_vib *vib = container_of(dev, struct isa1000_vib, timed_dev);

	if (hrtimer_active(&vib->vib_timer)) {
		ktime_t r = hrtimer_get_remaining(&vib->vib_timer);
		return (int) ktime_to_us(r);
	}

	return 0;
}

static enum hrtimer_restart isa1000_timer_func(struct hrtimer *timer)
{
	struct isa1000_vib *vib = container_of(timer, struct isa1000_vib, vib_timer);

	vib->state = 0;
	schedule_work(&vib->work);

	return HRTIMER_NORESTART;
}

static ssize_t isa1000_amp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct timed_output_dev *timed_dev = dev_get_drvdata(dev);
	struct isa1000_vib *vib = container_of(timed_dev, struct isa1000_vib, timed_dev);

	return sprintf(buf, "%d\n", vib->pwm_duty_percent);
}

static ssize_t isa1000_amp_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct timed_output_dev *timed_dev = dev_get_drvdata(dev);
	struct isa1000_vib *vib = container_of(timed_dev, struct isa1000_vib, timed_dev);

	int tmp;
	sscanf(buf, "%d", &tmp);
	if (tmp > 100)
		tmp = 100;
	else if (tmp < 80)
		tmp = 80;

	vib->pwm_duty_percent = tmp;

	return size;
}

static ssize_t isa1000_pwm_show(struct device *dev,struct device_attribute *attr, char *buf)
{
	struct timed_output_dev *timed_dev = dev_get_drvdata(dev);
	struct isa1000_vib *vib = container_of(timed_dev, struct isa1000_vib, timed_dev);

	return sprintf(buf, "%u\n", vib->pwm_frequency);
}

static ssize_t isa1000_pwm_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct timed_output_dev *timed_dev = dev_get_drvdata(dev);
	struct isa1000_vib *vib = container_of(timed_dev, struct isa1000_vib, timed_dev);

	int tmp;
	sscanf(buf, "%u", &tmp);
	if (tmp > 50000)
		tmp = 50000;
	else if (tmp < 10000)
		tmp = 10000;

	vib->pwm_frequency = tmp;

	return size;
}
static struct device_attribute isa1000_device_attrs[] = {
	__ATTR(amp, S_IRUGO | S_IWUSR, isa1000_amp_show, isa1000_amp_store),
	__ATTR(pwm, S_IRUGO | S_IWUSR, isa1000_pwm_show, isa1000_pwm_store),
};

static int isa1000_parse_dt(struct platform_device *pdev, struct isa1000_vib *vib)
{
	int ret;

	ret = of_get_named_gpio_flags(pdev->dev.of_node, "gpio-isa1000-en", 0, NULL);
	if (ret < 0) {
		dev_err(&pdev->dev, "please check enable gpio");
		return ret;
	}
	vib->gpio_isa1000_en = ret;

	ret = of_get_named_gpio_flags(pdev->dev.of_node, "gpio-haptic-en", 0, NULL);
	if (ret < 0) {
		dev_err(&pdev->dev, "please check enable gpio");
		return ret;
	}
	vib->gpio_haptic_en = ret;

	ret = of_property_read_u32(pdev->dev.of_node, "timeout-ms", &vib->timeout);
	if (ret < 0)
		dev_err(&pdev->dev, "please check timeout");

	ret = of_property_read_u32(pdev->dev.of_node, "pwm-channel", &vib->pwm_channel);
	if (ret < 0)
		dev_err(&pdev->dev, "please check pwm output channel");
	
	/* print values*/
	dev_info(&pdev->dev, "gpio-isa1000-en: %i, gpio-haptic-en: %i, timeout-ms: %i, pwm-channel: %i", 
				vib->gpio_isa1000_en, vib->gpio_haptic_en, vib->timeout, vib->pwm_channel);

	return 0;
}

static int isa1000_probe(struct platform_device *pdev)
{
	struct isa1000_vib *vib;
	int i, ret;

	platform_set_drvdata(pdev, &vib_dev);
	vib = (struct isa1000_vib *) platform_get_drvdata(pdev);

	/* parse dt */
	ret = isa1000_parse_dt(pdev, vib);
	if (ret < 0) {
		dev_err(&pdev->dev, "error occured while parsing dt\n");
	}

	ret = gpio_is_valid(vib->gpio_isa1000_en);
	if (ret) {
		ret = gpio_request(vib->gpio_isa1000_en, "gpio_isa1000_en");
		if (ret) {
			dev_err(&pdev->dev, "gpio %d request failed",vib->gpio_isa1000_en);
			return ret;
		}
	} else {
		dev_err(&pdev->dev, "invalid gpio %d\n", vib->gpio_isa1000_en);
		return ret;
	}

	ret = gpio_is_valid(vib->gpio_haptic_en);
	if (ret) {
		ret = gpio_request(vib->gpio_haptic_en, "gpio_haptic_en");
		if (ret) {
			dev_err(&pdev->dev, "gpio %d request failed\n", vib->gpio_haptic_en);
			return ret;
		}
	} else {
		dev_err(&pdev->dev, "invalid gpio %d\n", vib->gpio_haptic_en);
		return ret;
	}

	gpio_direction_output(vib->gpio_isa1000_en, 0);
	gpio_direction_output(vib->gpio_haptic_en, 1);

	vib->pwm = pwm_request(vib->pwm_channel, "isa1000");
	if (IS_ERR(vib->pwm)) {
		dev_err(&pdev->dev,"pwm request failed");
		return PTR_ERR(vib->pwm);
	}

	mutex_init(&vib->lock);

	INIT_WORK(&vib->work, isa1000_update);

	hrtimer_init(&vib->vib_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vib->vib_timer.function = isa1000_timer_func;

	vib->timed_dev.name = "vibrator";
	vib->timed_dev.get_time = isa1000_get_time;
	vib->timed_dev.enable = isa1000_enable;
	ret = timed_output_dev_register(&vib->timed_dev);
	if (ret < 0)
		return ret;

	for (i = 0; i < ARRAY_SIZE(isa1000_device_attrs); i++) {
		ret = device_create_file(vib->timed_dev.dev, &isa1000_device_attrs[i]);
		if (ret < 0) {
			pr_err("%s: failed to create sysfs\n", __func__);
			goto err_sysfs;
		}
	}

	return 0;

err_sysfs:
	for (; i >= 0; i--)
		device_remove_file(vib->timed_dev.dev, &isa1000_device_attrs[i]);

	timed_output_dev_unregister(&vib->timed_dev);
	return ret;
}

static int isa1000_remove(struct platform_device *pdev)
{
	struct isa1000_vib *vib = platform_get_drvdata(pdev);

	timed_output_dev_unregister(&vib->timed_dev);

	hrtimer_cancel(&vib->vib_timer);

	cancel_work_sync(&vib->work);

	mutex_destroy(&vib->lock);

	gpio_free(vib->gpio_haptic_en);
	gpio_free(vib->gpio_isa1000_en);

	return 0;
}

static struct of_device_id vibrator_match_table[] = {
	{ .compatible = "imagis,isa1000", },
	{ },
};

static struct platform_driver isa1000_driver = {
	.probe		= isa1000_probe,
	.remove		= isa1000_remove,
	.driver		= {
		.name	= "isa1000",
		.of_match_table = vibrator_match_table,
	},
};

static int __init isa1000_init(void)
{
	return platform_driver_register(&isa1000_driver);
}
module_init(isa1000_init);

static void __exit isa1000_exit(void)
{
	return platform_driver_unregister(&isa1000_driver);
}
module_exit(isa1000_exit);

MODULE_AUTHOR("Balázs Triszka <balika011@protonmail.ch>");
MODULE_DESCRIPTION("ISA1000 Haptic Motor driver");
MODULE_LICENSE("GPL v2");
