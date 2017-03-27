/*
 * Copyright (C) 2016 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup    
 * @ingroup     
 * @brief       
 * @{
 * @file		umdk-opt3001.c
 * @brief       umdk-opt3001 module implementation
 * @author      Oleg Artamonov <info@unwds.com>
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "periph/gpio.h"
#include "periph/i2c.h"

#include "board.h"

#include "opt3001.h"

#include "unwds-common.h"
#include "umdk-opt3001.h"
#include "unwds-gpio.h"

#include "thread.h"
#include "rtctimers.h"

static opt3001_t dev;

static uwnds_cb_t *callback;

static kernel_pid_t timer_pid;

static msg_t timer_msg = {};
static rtctimer_t timer;

static bool is_polled = false;

static struct {
	uint8_t is_valid;
	uint8_t publish_period_min;
	uint8_t i2c_dev;
} opt3001_config;

static bool init_sensor(void) {
	// dev.i2c = UMDK_OPT3001_I2C;

	printf("[opt3001] Initializing ultrasound distance meter as opt3001\n");

	return opt3001_init(&dev) == 0;
}

static void prepare_result(module_data_t *buf) {
	opt3001_measure_t measure = {};
	opt3001_measure(&dev, &measure);
    
    uint16_t luminocity;
    /* OPT3001 reports luminocity as 4-bit value, 83865 lux maximum */
    /* let's limit it to 2-bit and 65535 lux for practical purposes */
    /* no need to precisely measure direct sunlight */
    if (measure.luminocity > UINT16_MAX)
    {
       luminocity = UINT16_MAX; 
    } else {
       luminocity = measure.luminocity;
    }

	printf("[opt3001] Luminocity %u lux\n", luminocity);

	buf->length = 1 + sizeof(luminocity); /* Additional byte for module ID */

	buf->data[0] = UNWDS_OPT3001_MODULE_ID;

	/* Copy measurements into response */
	memcpy(buf->data + 1, (uint8_t *) &luminocity, sizeof(luminocity));
}

static void *timer_thread(void *arg) {
    msg_t msg;
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);
    
    puts("[umdk-opt3001] Periodic publisher thread started");

    while (1) {
        msg_receive(&msg);

        rtctimers_remove(&timer);

        module_data_t data = {};
        data.as_ack = is_polled;
        is_polled = false;

        prepare_result(&data);

        /* Notify the application */
        callback(&data);

        /* Restart after delay */
        rtctimers_set_msg(&timer, /* 60 * */ opt3001_config.publish_period_min, &timer_msg, timer_pid);
    }

    return NULL;
}

static void reset_config(void) {
	opt3001_config.is_valid = 0;
	opt3001_config.publish_period_min = UMDK_OPT3001_PUBLISH_PERIOD_MIN;
	opt3001_config.i2c_dev = UMDK_OPT3001_I2C;
}

static void init_config(void) {
	reset_config();

	if (!unwds_read_nvram_config(UNWDS_OPT3001_MODULE_ID, (uint8_t *) &opt3001_config, sizeof(opt3001_config)))
		return;

	if ((opt3001_config.is_valid == 0xFF) || (opt3001_config.is_valid == 0)) {
		reset_config();
		return;
	}

	if (opt3001_config.i2c_dev >= I2C_NUMOF) {
		reset_config();
		return;
	}
}

static inline void save_config(void) {
	opt3001_config.is_valid = 1;
	unwds_write_nvram_config(UNWDS_OPT3001_MODULE_ID, (uint8_t *) &opt3001_config, sizeof(opt3001_config));
}

static void set_period (int period) {
    rtctimers_remove(&timer);

    opt3001_config.publish_period_min = period;
	save_config();

	/* Don't restart timer if new period is zero */
	if (opt3001_config.publish_period_min) {
        rtctimers_set_msg(&timer, 60 * opt3001_config.publish_period_min, &timer_msg, timer_pid);
		printf("[opt3001] Period set to %d minute (s)\n", opt3001_config.publish_period_min);
    } else {
        puts("[opt3001] Timer stopped");
    }
}

int umdk_opt3001_shell_cmd(int argc, char **argv) {
    if (argc == 1) {
        puts ("opt3001 get - get results now");
        puts ("opt3001 send - get and send results now");
        puts ("opt3001 period <N> - set period to N minutes");
        puts ("opt3001 reset - reset settings to default");
        return 0;
    }
    
    char *cmd = argv[1];
	
    if (strcmp(cmd, "get") == 0) {
        module_data_t data = {};
        prepare_result(&data);
    }
    
    if (strcmp(cmd, "send") == 0) {
        is_polled = true;
		/* Send signal to publisher thread */
		msg_send(&timer_msg, timer_pid);
    }
    
    if (strcmp(cmd, "period") == 0) {
        char *val = argv[2];
        set_period(atoi(val));
    }
    
    if (strcmp(cmd, "reset") == 0) {
        reset_config();
        save_config();
    }
    
    return 1;
}

void umdk_opt3001_init(uint32_t *non_gpio_pin_map, uwnds_cb_t *event_callback) {
	(void) non_gpio_pin_map;

	callback = event_callback;

	init_config();
	printf("[opt3001] Publish period: %d min\n", opt3001_config.publish_period_min);

	if (!init_sensor()) {
		puts("[umdk-opt3001] Unable to init sensor!");
        return;
	}

	/* Create handler thread */
	char *stack = (char *) allocate_stack();
	if (!stack) {
		puts("umdk-opt3001: unable to allocate memory. Are too many modules enabled?");
		return;
	}
    
    unwds_add_shell_command("opt3001", "type 'opt3001' for commands list", umdk_opt3001_shell_cmd);
    
	timer_pid = thread_create(stack, UNWDS_STACK_SIZE_BYTES, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, timer_thread, NULL, "opt3001 thread");

    /* Start publishing timer */
	rtctimers_set_msg(&timer, 60 * opt3001_config.publish_period_min, &timer_msg, timer_pid);
}

static void reply_fail(module_data_t *reply) {
	reply->length = 2;
	reply->data[0] = UNWDS_OPT3001_MODULE_ID;
	reply->data[1] = 255;
}

static void reply_ok(module_data_t *reply) {
	reply->length = 2;
	reply->data[0] = UNWDS_OPT3001_MODULE_ID;
	reply->data[1] = 0;
}

bool umdk_opt3001_cmd(module_data_t *cmd, module_data_t *reply) {
	if (cmd->length < 1) {
		reply_fail(reply);
		return true;
	}

	umdk_opt3001_cmd_t c = cmd->data[0];
	switch (c) {
	case UMDK_OPT3001_CMD_SET_PERIOD: {
		if (cmd->length != 2) {
			reply_fail(reply);
			break;
		}

		uint8_t period = cmd->data[1];
		set_period(period);

		reply_ok(reply);
		break;
	}

	case UMDK_OPT3001_CMD_POLL:
		is_polled = true;

		/* Send signal to publisher thread */
		msg_send(&timer_msg, timer_pid);

		return false; /* Don't reply */

	case UMDK_OPT3001_CMD_SET_I2C: {
		// i2c_t i2c = (i2c_t) cmd->data[1];
		// dev.i2c = i2c;

		// opt3001_config.i2c_dev = i2c;

		init_sensor();

		reply_ok(reply);
		break;
	}

	default:
		reply_fail(reply);
		break;
	}

	return true;
}

#ifdef __cplusplus
}
#endif
