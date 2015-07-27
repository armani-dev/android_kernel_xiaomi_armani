/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
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

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/input.h>
#include <linux/input/gen_vkeys.h>

#define MAX_BUF_SIZE	256
#define VKEY_VER_CODE	"0x01"

#define MAX_VKEY_ATTR 8

#define HEIGHT_SCALE_NUM 8
#define HEIGHT_SCALE_DENOM 10

#define VKEY_Y_OFFSET_DEFAULT 0

/* numerator and denomenator for border equations */
#define BORDER_ADJUST_NUM 3
#define BORDER_ADJUST_DENOM 4

static struct kobject * vkey_obj;

static struct mutex vkey_mutex;
static int vkey_count = 0;
static char * vkey_name[MAX_VKEY_ATTR];
static char * vkey_value[MAX_VKEY_ATTR];

static ssize_t vkey_show(struct kobject  *obj,
		struct kobj_attribute *attr, char *buf)
{
	int i;
	char * vkey_buf = NULL;
	if (attr && attr->attr.name) {
		for (i=0; i < vkey_count; i++) {
			if (strcmp(vkey_name[i], attr->attr.name) == 0) {
				vkey_buf = vkey_value[i];
				break;
			}
		}
		if (vkey_buf) {
			strlcpy(buf, vkey_buf, MAX_BUF_SIZE);
			return strnlen(buf, MAX_BUF_SIZE);
		}
	}
	buf[0] = 0;
	return 0;
}

static struct kobj_attribute vkey_obj_attr[MAX_VKEY_ATTR] = {
	{ },
};

static struct attribute *vkey_attr[MAX_VKEY_ATTR] = {
	&vkey_obj_attr[0].attr,
	NULL,
};

static struct attribute_group vkey_grp = {
	.attrs = vkey_attr,
};

static int __devinit vkey_parse_dt(struct device *dev,
			struct vkeys_platform_data *pdata, const char ** vkeys_str)
{
	struct device_node *np = dev->of_node;
	struct property *prop;
	int rc, val;

	rc = of_property_read_string(np, "label", &pdata->name);
	if (rc) {
		dev_err(dev, "Failed to read label\n");
		return -EINVAL;
	}
	
	*vkeys_str = NULL;
	rc = of_property_read_string(np, "vkeys", vkeys_str);
	if (!rc && *vkeys_str) {
		//dev_info(dev, "vkeys = \"%s\" \n", *vkeys_str);
		return 0;
	}
	*vkeys_str = NULL;

	rc = of_property_read_u32(np, "qcom,disp-maxx", &pdata->disp_maxx);
	if (rc) {
		dev_err(dev, "Failed to read display max x\n");
		return -EINVAL;
	}

	rc = of_property_read_u32(np, "qcom,disp-maxy", &pdata->disp_maxy);
	if (rc) {
		dev_err(dev, "Failed to read display max y\n");
		return -EINVAL;
	}

	rc = of_property_read_u32(np, "qcom,panel-maxx", &pdata->panel_maxx);
	if (rc) {
		dev_err(dev, "Failed to read panel max x\n");
		return -EINVAL;
	}

	rc = of_property_read_u32(np, "qcom,panel-maxy", &pdata->panel_maxy);
	if (rc) {
		dev_err(dev, "Failed to read panel max y\n");
		return -EINVAL;
	}

	prop = of_find_property(np, "qcom,key-codes", NULL);
	if (prop) {
		pdata->num_keys = prop->length / sizeof(u32);
		pdata->keycodes = devm_kzalloc(dev,
			sizeof(u32) * pdata->num_keys, GFP_KERNEL);
		if (!pdata->keycodes)
			return -ENOMEM;
		rc = of_property_read_u32_array(np, "qcom,key-codes",
				pdata->keycodes, pdata->num_keys);
		if (rc) {
			dev_err(dev, "Failed to read key codes\n");
			return -EINVAL;
		}
	}

	pdata->y_offset = VKEY_Y_OFFSET_DEFAULT;
	rc = of_property_read_u32(np, "qcom,y-offset", &val);
	if (!rc)
		pdata->y_offset = val;
	else if (rc != -EINVAL) {
		dev_err(dev, "Failed to read y position offset\n");
		return rc;
	}
	return 0;
}

static int __devinit _vkeys_probe(struct platform_device *pdev)
{
	struct vkeys_platform_data *pdata;
	int width, height, center_x, center_y;
	int x1 = 0, x2 = 0, i, c = 0, ret, border;
	char *name;
	char * vkey_buf;
	const char * vkeys_str = NULL;

	vkey_buf = devm_kzalloc(&pdev->dev, MAX_BUF_SIZE, GFP_KERNEL);
	if (!vkey_buf) {
		dev_err(&pdev->dev, "Failed to allocate memory\n");
		return -ENOMEM;
	}

	if (pdev->dev.of_node) {
		pdata = devm_kzalloc(&pdev->dev,
			sizeof(struct vkeys_platform_data), GFP_KERNEL);
		if (!pdata) {
			dev_err(&pdev->dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}

		ret = vkey_parse_dt(&pdev->dev, pdata, &vkeys_str);
		if (ret) {
			dev_err(&pdev->dev, "Parsing DT failed(%d)", ret);
			return ret;
		}
	} else
		pdata = pdev->dev.platform_data;

	if (!vkeys_str) {
		if (!pdata || !pdata->name || !pdata->keycodes || !pdata->num_keys ||
			!pdata->disp_maxx || !pdata->disp_maxy || !pdata->panel_maxy) {
			dev_err(&pdev->dev, "pdata is invalid\n");
			return -EINVAL;
		}

		border = (pdata->panel_maxx - pdata->disp_maxx) * 2;
		width = ((pdata->disp_maxx - (border * (pdata->num_keys - 1)))
				/ pdata->num_keys);
		height = (pdata->panel_maxy - pdata->disp_maxy);
		center_y = pdata->disp_maxy + (height / 2) + pdata->y_offset;
		height = height * HEIGHT_SCALE_NUM / HEIGHT_SCALE_DENOM;

		x2 -= border * BORDER_ADJUST_NUM / BORDER_ADJUST_DENOM;

		for (i = 0; i < pdata->num_keys; i++) {
			x1 = x2 + border;
			x2 = x2 + border + width;
			center_x = x1 + (x2 - x1) / 2;
			c += snprintf(vkey_buf + c, MAX_BUF_SIZE - c,
					"%s:%d:%d:%d:%d:%d\n",
					VKEY_VER_CODE, pdata->keycodes[i],
					center_x, center_y, width, height);
		}

		vkey_buf[c] = '\0';
	} else {
		/*	
		file: sys/board_properties/virtualkeys.synaptics_rmi4_i2c
		file: sys/board_properties/virtualkeys.ft5x06_ts
		0x01:139:101:1343:120:96:0x01:102:360:1343:150:96:0x01:158:618:1343:120:96
		|    key x   y    w   h  |    key x   y    w   h  |    key x   y    w   h
		*/	
		strcpy(vkey_buf, vkeys_str);
	}	
	
	dev_info(&pdev->dev, "%s: %s: vkey_buf = '%s'\n", __func__, pdata->name, vkey_buf);

	name = devm_kzalloc(&pdev->dev, sizeof(*name) * MAX_BUF_SIZE,
					GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	snprintf(name, MAX_BUF_SIZE,
				"virtualkeys.%s", pdata->name);
	
	vkey_obj_attr[vkey_count].attr.name = name;
	vkey_obj_attr[vkey_count].attr.mode = S_IRUGO;
	vkey_obj_attr[vkey_count].show = vkey_show;
	vkey_attr[vkey_count] = &vkey_obj_attr[vkey_count].attr;
	vkey_attr[vkey_count+1] = NULL;

	if (!vkey_obj) {
		vkey_obj = kobject_create_and_add("board_properties", NULL);
		if (!vkey_obj) {
			dev_err(&pdev->dev, "unable to create kobject\n");
			return -ENOMEM;
		}
		ret = sysfs_create_group(vkey_obj, &vkey_grp);
		if (ret) {
			dev_err(&pdev->dev, "failed to create attributes\n");
			goto destroy_kobj;
		}
		dev_info(&pdev->dev, "%s: create vkey_grp \"%s\" \n", __func__, name);
	} else {
		ret = sysfs_update_group(vkey_obj, &vkey_grp);
		if (ret) {
			dev_err(&pdev->dev, "failed to update attributes\n");
			vkey_attr[vkey_count] = NULL;
			return ret;
		}
		dev_info(&pdev->dev, "%s: update vkey_grp \"%s\" \n", __func__, name);
	}
	vkey_name[vkey_count] = name;
	vkey_value[vkey_count] = vkey_buf;
	vkey_count++;
	return 0;

destroy_kobj:
	kobject_put(vkey_obj);

	return ret;
}

static int __devinit vkeys_probe(struct platform_device *pdev)
{
	int rc;
	mutex_lock(&vkey_mutex);
	rc = _vkeys_probe(pdev);
	mutex_unlock(&vkey_mutex);
	return rc;
}

static int __devexit vkeys_remove(struct platform_device *pdev)
{
	mutex_lock(&vkey_mutex);
	if (vkey_obj) {
		sysfs_remove_group(vkey_obj, &vkey_grp);
		kobject_put(vkey_obj);
		vkey_obj = NULL;
	}
	mutex_unlock(&vkey_mutex);
	return 0;
}

static struct of_device_id vkey_match_table[] = {
	{ .compatible = "qcom,gen-vkeys",},
	{ },
};

static struct platform_driver vkeys_driver = {
	.probe = vkeys_probe,
	.remove = __devexit_p(vkeys_remove),
	.driver = {
		.owner = THIS_MODULE,
		.name = "gen_vkeys",
		.of_match_table = vkey_match_table,
	},
};

static int __init vkeys_driver_init(void)
{
	mutex_init(&vkey_mutex);
	return platform_driver_register(&vkeys_driver);
}
module_init(vkeys_driver_init);

static void __exit vkeys_driver_exit(void)
{
	mutex_destroy(&vkey_mutex);
	platform_driver_unregister(&vkeys_driver);
}
module_exit(vkeys_driver_exit); 

MODULE_LICENSE("GPL v2");
