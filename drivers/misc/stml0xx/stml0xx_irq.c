/*
 * Copyright (C) 2010-2014 Motorola Mobility LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/input-polldev.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/switch.h>
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <linux/stml0xx.h>

irqreturn_t stml0xx_isr(int irq, void *dev)
{
	static struct timespec ts;
	static struct stml0xx_work_struct *stm_ws;
	struct stml0xx_data *ps_stml0xx = dev;
	getrawmonotonic(&ts);

	if (stml0xx_irq_disable)
		return IRQ_HANDLED;

	stm_ws = kmalloc(
		sizeof(struct stml0xx_work_struct),
		GFP_ATOMIC);
	if (!stm_ws) {
		dev_err(dev, "stml0xx_isr: unable to allocate work struct");
		return IRQ_HANDLED;
	}

	INIT_WORK((struct work_struct *)stm_ws, stml0xx_irq_work_func);
	stm_ws->ts_ns = ts_to_ns(ts);

	queue_work(ps_stml0xx->irq_work_queue, (struct work_struct *)stm_ws);
	return IRQ_HANDLED;
}

void stml0xx_irq_work_func(struct work_struct *work)
{
	int err;
	u32 irq_status;
	struct stml0xx_work_struct *stm_ws = (struct stml0xx_work_struct *)work;
	struct stml0xx_data *ps_stml0xx = stml0xx_misc_data;
	unsigned char buf[SPI_MSG_SIZE];

	dev_dbg(&stml0xx_misc_data->spi->dev, "stml0xx_irq_work_func");
	mutex_lock(&ps_stml0xx->lock);

	stml0xx_wake(ps_stml0xx);

	if (ps_stml0xx->mode == BOOTMODE)
		goto EXIT;

	if (ps_stml0xx->is_suspended)
		goto EXIT;

	/* read interrupt mask register */
	err = stml0xx_spi_send_read_reg(INTERRUPT_STATUS, buf, 3);
	if (err < 0) {
		dev_err(&stml0xx_misc_data->spi->dev,
			"Reading from stml0xx failed");
		goto EXIT;
	}
	irq_status = (buf[IRQ_NOWAKE_HI] << 16) |
	    (buf[IRQ_NOWAKE_MED] << 8) | buf[IRQ_NOWAKE_LO];

	if (irq_status & M_ACCEL) {
		/* read accelerometer values from STML0XX */
		err = stml0xx_spi_send_read_reg(ACCEL_X, buf, 6);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading Accel from stml0xx failed");
			goto EXIT;
		}

		stml0xx_as_data_buffer_write(
			ps_stml0xx,
			DT_ACCEL,
			buf,
			6,
			0,
			stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending acc(x,y,z)values:x=%d,y=%d,z=%d",
			STM16_TO_HOST(ACCEL_RD_X, buf),
			STM16_TO_HOST(ACCEL_RD_Y, buf),
			STM16_TO_HOST(ACCEL_RD_Z, buf));
	}
	if (irq_status & M_ACCEL2) {
		/* read 2nd accelerometer values from STML0XX */
		err = stml0xx_spi_send_read_reg(ACCEL2_X, buf, 6);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading 2nd Accel from stml0xx failed");
			goto EXIT;
		}

		stml0xx_as_data_buffer_write(ps_stml0xx,
			DT_ACCEL2,
			buf,
			6,
			0,
			stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending acc2(x,y,z)values:x=%d,y=%d,z=%d",
			STM16_TO_HOST(ACCEL_RD_X, buf),
			STM16_TO_HOST(ACCEL_RD_Y, buf),
			STM16_TO_HOST(ACCEL_RD_Z, buf));
	}
	if (irq_status & M_LIN_ACCEL) {
		dev_err(&stml0xx_misc_data->spi->dev,
			"Invalid M_LIN_ACCEL bit set. irq_status = 0x%06x",
			irq_status);

		/* read linear accelerometer values from STML0XX */
		err = stml0xx_spi_send_read_reg(LIN_ACCEL_X, buf, 6);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading Linear Accel from stml0xx failed");
			goto EXIT;
		}

		stml0xx_as_data_buffer_write(ps_stml0xx, DT_LIN_ACCEL,
					     buf, 6, 0, stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending lin_acc(x,y,z)values:x=%d,y=%d,z=%d",
			STM16_TO_HOST(ACCEL_RD_X, buf),
			STM16_TO_HOST(ACCEL_RD_Y, buf),
			STM16_TO_HOST(ACCEL_RD_Z, buf));
	}
	if (irq_status & M_ECOMPASS) {
		unsigned char status;
		/*Read orientation values */
		err = stml0xx_spi_send_read_reg(MAG_HX, buf, 13);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading Ecompass failed");
			goto EXIT;
		}
		status = buf[COMPASS_STATUS];
		stml0xx_as_data_buffer_write(ps_stml0xx, DT_MAG,
					     buf, 6, status, stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending mag(x,y,z)values:x=%d,y=%d,z=%d",
			STM16_TO_HOST(MAG_X, buf), STM16_TO_HOST(MAG_Y, buf),
			STM16_TO_HOST(MAG_Z, buf));

		stml0xx_as_data_buffer_write(ps_stml0xx, DT_ORIENT,
					     buf + 6, 6, status, stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending orient(x,y,z)values:x=%d,y=%d,z=%d",
			STM16_TO_HOST(ORIENT_X, buf),
			STM16_TO_HOST(ORIENT_Y, buf),
			STM16_TO_HOST(ORIENT_Z, buf));
	}
	if (irq_status & M_GYRO) {
		err = stml0xx_spi_send_read_reg(GYRO_X, buf, 6);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading Gyroscope failed");
			goto EXIT;
		}
		stml0xx_as_data_buffer_write(
			ps_stml0xx,
			DT_GYRO,
			buf,
			6,
			0,
			stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending gyro(x,y,z)values:x=%d,y=%d,z=%d",
			STM16_TO_HOST(GYRO_RD_X, buf),
			STM16_TO_HOST(GYRO_RD_Y, buf),
			STM16_TO_HOST(GYRO_RD_Z, buf));
	}
	/*MODIFIED UNCALIBRATED GYRO */
	if (irq_status & M_UNCALIB_GYRO) {
		err = stml0xx_spi_send_read_reg(UNCALIB_GYRO_X, buf, 12);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading Gyroscope failed");
			goto EXIT;
		}
		stml0xx_as_data_buffer_write(ps_stml0xx, DT_UNCALIB_GYRO,
					     buf, 12, 0, stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending Gyro uncalib(x,y,z)values:%d,%d,%d;%d,%d,%d",
			STM16_TO_HOST(GYRO_RD_X, buf),
			STM16_TO_HOST(GYRO_RD_Y, buf),
			STM16_TO_HOST(GYRO_RD_Z, buf),
			STM16_TO_HOST(GYRO_UNCALIB_X, buf),
			STM16_TO_HOST(GYRO_UNCALIB_Y, buf),
			STM16_TO_HOST(GYRO_UNCALIB_Z, buf));
	}
	if (irq_status & M_UNCALIB_MAG) {
		err = stml0xx_spi_send_read_reg(UNCALIB_MAG_X, buf, 12);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading Gyroscope failed");
			goto EXIT;
		}

		stml0xx_as_data_buffer_write(ps_stml0xx, DT_UNCALIB_MAG,
					     buf, 12, 0, stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending Gyro uncalib(x,y,z)values:%d,%d,%d;%d,%d,%d",
			STM16_TO_HOST(MAG_X, buf), STM16_TO_HOST(MAG_Y, buf),
			STM16_TO_HOST(MAG_Z, buf),
			STM16_TO_HOST(MAG_UNCALIB_X, buf),
			STM16_TO_HOST(MAG_UNCALIB_Y, buf),
			STM16_TO_HOST(MAG_UNCALIB_Z, buf));
	}
	if (irq_status & M_ALS) {
		err = stml0xx_spi_send_read_reg(ALS_LUX, buf, 2);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading ALS from stml0xx failed");
			goto EXIT;
		}
		stml0xx_as_data_buffer_write(
			ps_stml0xx,
			DT_ALS,
			buf,
			2,
			0,
			stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending ALS %d", STM16_TO_HOST(ALS_VALUE, buf));
	}
	if (irq_status & M_TEMPERATURE) {
		/* Read temperature value */
		err = stml0xx_spi_send_read_reg(TEMPERATURE_DATA, buf, 2);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading Temperature failed");
			goto EXIT;
		}
		stml0xx_as_data_buffer_write(
			ps_stml0xx,
			DT_TEMP,
			buf,
			2,
			0,
			stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending temp(x)value:%d",
			STM16_TO_HOST(TEMP_VALUE, buf));
	}
	if (irq_status & M_PRESSURE) {
		dev_err(&stml0xx_misc_data->spi->dev,
			"Invalid M_PRESSURE bit set. irq_status = 0x%06x",
			irq_status);
		/* Read pressure value */
		err = stml0xx_spi_send_read_reg(CURRENT_PRESSURE, buf, 4);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading Pressure failed");
			goto EXIT;
		}
		stml0xx_as_data_buffer_write(ps_stml0xx, DT_PRESSURE,
					     buf, 4, 0, stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending pressure %d",
			STM32_TO_HOST(PRESSURE_VALUE, buf));
	}
	if (irq_status & M_GRAVITY) {
		dev_err(&stml0xx_misc_data->spi->dev,
			"Invalid M_GRAVITY bit set. irq_status = 0x%06x",
			irq_status);

		/* read gravity values from STML0XX */
		err = stml0xx_spi_send_read_reg(GRAVITY_X, buf, 6);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading Gravity from stml0xx failed");
			goto EXIT;
		}

		stml0xx_as_data_buffer_write(
			ps_stml0xx,
			DT_GRAVITY,
			buf,
			6,
			0,
			stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending gravity(x,y,z)values:x=%d,y=%d,z=%d",
			STM16_TO_HOST(GRAV_X, buf), STM16_TO_HOST(GRAV_Y, buf),
			STM16_TO_HOST(GRAV_Z, buf));
	}
	if (irq_status & M_DISP_ROTATE) {
		/* Read Display Rotate value */
		err = stml0xx_spi_send_read_reg(DISP_ROTATE_DATA, buf, 1);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading disp_rotate failed");
			goto EXIT;
		}
		stml0xx_as_data_buffer_write(ps_stml0xx, DT_DISP_ROTATE,
					     buf, 1, 0, stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending disp_rotate(x)value: %d", buf[DISP_VALUE]);
	}
	if (irq_status & M_DISP_BRIGHTNESS) {
		err = stml0xx_spi_send_read_reg(DISPLAY_BRIGHTNESS, buf, 1);
		if (err < 0) {
			dev_err(&stml0xx_misc_data->spi->dev,
				"Reading Display Brightness failed");
			goto EXIT;
		}
		stml0xx_as_data_buffer_write(ps_stml0xx, DT_DISP_BRIGHT,
					     buf, 1, 0, stm_ws->ts_ns);

		dev_dbg(&stml0xx_misc_data->spi->dev,
			"Sending Display Brightness %d", buf[DISP_VALUE]);
	}
EXIT:
	kfree((void *)stm_ws);
	stml0xx_sleep(ps_stml0xx);
	/* For now HAE needs events even if the activity is still */
	mutex_unlock(&ps_stml0xx->lock);
}