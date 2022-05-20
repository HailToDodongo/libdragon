#include <libdragon.h>
#include "../../src/video/profile.h"

#define NUM_DISPLAY   4

void audio_poll(void) {	
	if (audio_can_write()) {    	
		PROFILE_START(PS_AUDIO, 0);
		short *buf = audio_write_begin();
		mixer_poll(buf, audio_get_buffer_length());
		audio_write_end();
		PROFILE_STOP(PS_AUDIO, 0);
	}
}

void video_poll(void) {


}

int main(void) {
	controller_init();
	debug_init_isviewer();
	debug_init_usblog();

	display_init(RESOLUTION_320x240, DEPTH_32_BPP, NUM_DISPLAY, GAMMA_NONE, ANTIALIAS_OFF);
	dfs_init(DFS_DEFAULT_LOCATION);
	rdp_init();

	audio_init(44100, 4);
	mixer_init(8);

	mpeg2_t mp2;
	mpeg2_open(&mp2, "rom:/cgi.m1v");

	wav64_t music;
	wav64_open(&music, "cgi.wav64");

	float fps = mpeg2_get_framerate(&mp2);
	throttle_init(fps, 0, 8);

	mixer_ch_play(0, &music.wave);

	debugf("start\n");
	int nframes = 0;
	display_context_t disp = 0;
	rspq_syncpoint_t syncf = 0;

	while (1) {
		mixer_throttle(44100.0f / fps);

		if (!mpeg2_next_frame(&mp2))
			break;

		if (syncf)
			rspq_wait_syncpoint(syncf);

		RSP_WAIT_LOOP(500) {
			disp = display_lock();
			if (disp) break;
		}

		rdp_attach_display(disp);
		rdp_set_default_clipping();

		mpeg2_draw_frame(&mp2, disp);

		#if 0
		rdp_detach_display();
		display_show(disp);
		#else
		rdp_detach_display_async(display_show);
		#endif

		syncf = rspq_syncpoint();

		audio_poll();

		nframes++;
		// uint32_t t1 = TICKS_READ();
		// if (TICKS_DISTANCE(t0, t1) > TICKS_PER_SECOND && nframes) {
		// 	float fps = (float)nframes / (float)TICKS_DISTANCE(t0,t1) * TICKS_PER_SECOND;
		// 	debugf("FPS: %.2f\n", fps);
		// 	t0 = t1;
		// 	nframes = 0;
		// }

		int ret = throttle_wait();
		if (ret < 0) {
			debugf("videoplayer: frame %d too slow (%d Kcycles)\n", nframes, -ret);
		}

		audio_poll();

		// PROFILE_START(PS_SYNC, 0);
		// rspq_sync();
		// PROFILE_STOP(PS_SYNC, 0);

	}
}
