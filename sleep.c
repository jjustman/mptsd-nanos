/*
 * mptsd sleep calibration
 * Copyright (C) 2010-2011 Unix Solutions Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 */
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>
#include <math.h>

#include "libfuncs/log.h"

#include "config.h"

void * calibrate_sleep(void *_config) {
	struct timeval tv1, tv2;
	unsigned long diff = 0, loops = 0;
	CONFIG *conf = _config;

	if (!conf->quiet) {
		LOGf("\tCalibrating sleep timeout...\n");
		LOGf("\tRequest timeout   : %ld us\n", conf->output_tmout);
	}

	struct timespec nsleep;
        nsleep.tv_sec =	0;
        nsleep.tv_nsec = (100);

	gettimeofday(&tv1, NULL);
	do {
	  nanosleep(&nsleep,NULL);
	} while (loops++ != 3000);
	gettimeofday(&tv2, NULL);

	printf("tv.2 nsec: %lu,\ntv1.nsec: %lu\n", tv2.tv_usec, tv1.tv_usec);;
	
	diff += timeval_diff_usec(&tv1, &tv2) - 1;
	printf("diff is: %lu", diff);
	conf->usleep_overhead = diff / (loops*10);
	conf->output_tmout -= conf->usleep_overhead;

	if (!conf->quiet) {
		LOGf("\tnanosleep(1000) overhead: %ld us\n", conf->usleep_overhead);
		LOGf("\tOutput pkt tmout  : %ld us\n", conf->output_tmout);
	}

	if (conf->output_tmout < 0) {
		LOGf("usleep overhead is too high! Make sure the kernel is compiled with CONFIG_HIGH_RES_TIMERS.\n");
		conf->output_tmout = 0;
	}

	pthread_exit(0);
}
