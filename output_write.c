/*
 * mptsd output writing
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
#include <inttypes.h>
#include <assert.h>

#include "libfuncs/io.h"
#include "libfuncs/log.h"
#include "libfuncs/list.h"

#include "libtsfuncs/tsfuncs.h"

#include "sleep.h"
#include "data.h"
#include "config.h"
#include "network.h"

void increase_process_priority() {
	return;
#ifdef __linux__
	struct sched_param param;
	param.sched_priority = 99;
	if (sched_setscheduler(0, SCHED_FIFO, &param)==-1) {
		log_perror("sched_setscheduler() failed!", errno);
	} else {
		LOGf("PRIO : sched_setschedule() succeded.\n");
	}
#endif
}

void ts_frame_process(CONFIG *conf, OUTPUT *o, uint8_t *data) {
	int i;
	uint16_t pid;
	uint8_t *ts_packet;
	for (i=0; i<FRAME_PACKET_SIZE; i+=TS_PACKET_SIZE) {
		ts_packet = data + i;
		pid = ts_packet_get_pid(ts_packet);

		if (pid == 0x1fff) // NULL packet
			o->padding_period += TS_PACKET_SIZE;

		if (ts_packet_has_pcr(ts_packet)) {
			uint64_t pcr = ts_packet_get_pcr(ts_packet);	// Current PCR
			uint64_t new_pcr = pcr;
			uint64_t bytes = o->traffic + i;

			if (o->last_pcr[pid]) {
				uint64_t old_pcr     = o->last_pcr[pid];
				uint64_t old_org_pcr = o->last_org_pcr[pid];
				uint64_t old_bytes   = o->last_traffic[pid];
				if (old_org_pcr < pcr) { // Detect PCR wraparound
					new_pcr = old_pcr + (double)((bytes - old_bytes) * 8 * 27000000) / o->output_bitrate;
					// Rewrite pcrs || Move pcrs & rewrite prcs
					if (conf->pcr_mode == 2 || conf->pcr_mode == 3) {
						ts_packet_set_pcr(ts_packet, new_pcr);
					}
					if (conf->debug) {
						uint64_t ts_rate = (double)(((bytes - old_bytes) * 8) * 27000000) / (pcr - old_org_pcr);
						uint64_t ts_rate_new = (double)(((bytes - old_bytes) * 8) * 27000000) / (new_pcr - old_pcr);
						LOGf("PCR[%03x]: old:%14" PRIu64 " new:%14" PRIu64 " pcr_diff:%8" PRId64 " ts_rate:%9" PRIu64 " ts_rate_new:%9" PRIu64 " diff:%9" PRId64 " | passed:%" PRIu64 "\n",
							pid,
							pcr,
							new_pcr,
							pcr - new_pcr,
							ts_rate,
							ts_rate_new,
							ts_rate - ts_rate_new,
							bytes - old_bytes
						);
					}
 				}
			} else {
//				if (config->debug) {
//					LOGf("PCR[%03x]: %10llu init\n", pid, pcr);
//				}
			}
			o->last_pcr[pid] = new_pcr;
			o->last_org_pcr[pid] = pcr;
			o->last_traffic[pid] = bytes;
		}
	}
}

ssize_t ts_frame_write(OUTPUT *o, uint8_t *data) {
	ssize_t written;
	written = fdwrite(o->out_sock, (char *)data, FRAME_PACKET_SIZE);
	if (written >= 0) {
		o->traffic        += written;
		o->traffic_period += written;
	}

	if (o->ofd) {
	  ssize_t fp_written;
	  fp_written =  write(o->ofd, data, FRAME_PACKET_SIZE);
	  assert(fp_written > 0);
	  //written += fp_written;
	}
	return written;
}

void * output_handle_write(void *_config) {
	CONFIG *conf = _config;
	OUTPUT *o = conf->output;
	int buf_in_use = 0;
	struct timeval stats_ts, now;
	struct timeval start_write_ts, end_write_ts, used_ts;
	unsigned long long stats_interval;

	signal(SIGPIPE, SIG_IGN);

	increase_process_priority();

	gettimeofday(&stats_ts, NULL);
	while (!o->dienow) {
		gettimeofday(&now, NULL);
		OBUF *curbuf = &o->obuf[buf_in_use];

		while (curbuf->status != obuf_full) { // Wait untill the buffer is ready ot it is already emptying
			if (o->dienow)
				goto OUT;
			//LOGf("MIX: Waiting for obuf %d\n", buf_in_use);
			struct timespec msleep;
			msleep.tv_sec = 0;
			msleep.tv_nsec = 100;
			nanosleep(&msleep, NULL);
		}
		curbuf->status = obuf_emptying; // Mark buffer as being filled

		// Show stats
		stats_interval = timeval_diff_msec(&stats_ts, &now);
		if (stats_interval > conf->timeouts.stats) {
			stats_ts = now;
			double out_kbps = (double)(o->traffic_period * 8) / 1000;
			double out_mbps = (double)out_kbps / 1000;
			double opadding = ((double)o->padding_period / o->traffic_period) * 100;

			if (!conf->quiet) {
				LOGf("STAT  : Pad:%6.2f%% Traf:%5.2f Mbps | %8.2f | %7" PRIu64 "\n",
					opadding,
					out_mbps,
					out_kbps,
					o->traffic_period
				);
			}
			o->traffic_period = 0;
			o->padding_period = 0;
		}

		gettimeofday(&start_write_ts, NULL);
		int packets_written = 0, real_sleep_time = conf->output_tmout - conf->usleep_overhead;
		long time_taken, time_diff, real_time, overhead = 0, overhead_total = 0;
		ssize_t written = 0;
		while (curbuf->written < curbuf->size) {
			if (o->dienow)
				goto OUT;
			long sleep_interval = conf->output_tmout;
			uint8_t *ts_frame = curbuf->buf + curbuf->written;
			ts_frame_process(conf, o, ts_frame);	// Fix PCR and count NULL packets
			written += ts_frame_write(o, ts_frame);	// Write packet to network/file
			curbuf->written += FRAME_PACKET_SIZE;
			if (packets_written) {
				time_taken = timeval_diff_usec(&start_write_ts, &used_ts);
				real_time  = packets_written * (conf->output_tmout + conf->usleep_overhead);
				time_diff = real_time - time_taken;
				overhead = (time_taken / packets_written) - sleep_interval;
				overhead_total += overhead;
/*
				LOGf("[%5d] time_taken:%5ld real_time:%5ld time_diff:%ld | overhead:%5ld overhead_total:%5ld\n",
					packets_written,
					time_taken,
					real_time,
					time_diff,
					overhead,
					overhead_total
				);
*/
				if (time_diff > real_sleep_time) {
					sleep_interval = time_diff - conf->usleep_overhead;
					if (sleep_interval < 0)
						sleep_interval = 1;
					// LOGf("Add sleep. time_diff: %ld sleep_interval: %ld\n", time_diff, sleep_interval);
				} else {
					//LOGf("Skip sleep %ld\n", time_diff);
					sleep_interval = 0;
				}

			}
			if (sleep_interval > 0) {
			  struct timespec sival;
			  sival.tv_sec = 0;
			  sival.tv_nsec = sleep_interval * 1000;
			  
			  nanosleep(&sival, NULL);
			  
			}
			gettimeofday(&used_ts, NULL);
			packets_written++;
		}
		gettimeofday(&end_write_ts, NULL);
		unsigned long long write_time = timeval_diff_usec(&start_write_ts, &end_write_ts);
		if (write_time < o->obuf_ms * 1000) {
			//LOGf("Writen for -%llu us less\n", o->obuf_ms*1000 - write_time);
		  struct timespec obsleep;
		  
                          obsleep.tv_sec = 0;
                          obsleep.tv_nsec = o->obuf_ms*1000000 - write_time;
		          nanosleep(&obsleep, NULL);
			  
		} else {
			//LOGf("Writen for +%llu us more\n", write_time - o->obuf_ms*1000);
		}

		obuf_reset(curbuf); // Buffer us all used up
		buf_in_use = buf_in_use ? 0 : 1; // Switch buffer
		if (written < 0) {
		  LOGf("OUTPUT: Error writing into output socket: %ld.\n", written);
	
			shutdown_fd(&o->out_sock);
			connect_output(o);
		}
	}
OUT:
	LOG("OUTPUT: WRITE thread stopped.\n");
	o->dienow++;
	return 0;
}
