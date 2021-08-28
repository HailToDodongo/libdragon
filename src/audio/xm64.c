#include <libdragon.h>
#include "libxm/xm.h"
#include "libxm/xm_internal.h"
#include <stdbool.h>

static void wave_read(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen, bool seeking) {
	xm_sample_t *samp = (xm_sample_t*)ctx;

	extern void raw_waveform_read(samplebuffer_t *sbuf, int base_rom_addr, int wpos, int wlen, int bps);
	raw_waveform_read(sbuf, samp->data8_offset, wpos, wlen, samp->bits >> 4);
}

static int tick(void *arg) {
	xm64player_t *xmp = (xm64player_t*)arg;
	xm_context_t *ctx = xmp->ctx;
	int first_ch = xmp->first_ch;

	for (int i=0;i<ctx->module.num_channels;i++) {
		xm_channel_context_t *ch = &ctx->channels[i];
		ch->sample_position = mixer_ch_get_pos(first_ch+i);
	}

	// If we're requested to stop playback, do it.
	if (!xmp->playing) {
		for (int i=0;i<ctx->module.num_channels;i++)
			mixer_ch_stop(xmp->first_ch+i);
		// Do not reschedule again
		return 0;
	}

	if (xmp->seek.patidx >= 0) {
		// Seek was requested. Do it.
		xm_seek(ctx, xmp->seek.patidx, xmp->seek.row, xmp->seek.tick);
		xmp->seek.patidx = -1;
	}

	assert(ctx->remaining_samples_in_tick <= 0);
	xm_tick(ctx);

	float gvol = ctx->global_volume * ctx->amplification;

	for (int i=0;i<ctx->module.num_channels;i++) {
		xm_channel_context_t *ch = &ctx->channels[i];
		if (ch->sample) {
			waveform_t *w = ch->sample->wave;

			// Check if this sample is muted. This is an user-level muting
			// control exposed via the xm.h API that we respect in case the
			// user wants to mute some channels (usually for debugging).
			bool muted = ch->muted || ch->instrument->muted;

			// Play the waveform. Notice that the waveform might already
			// being played in this channel, in which case the play
			// command only resets its position to 0, and keep the sample
			// buffer full, which is what we want.
			// The mixer doesn't currently allow for mixer_ch_play() to keep
			// the current position, but even if it did, xm_tick() might
			// have changed it since last tick, because there is a XM effect
			// to force the position in the sample. So it's better to
			// set it every time with mixer_ch_set_pos.
			mixer_ch_play(first_ch+i, w);
			mixer_ch_set_pos(first_ch+i, ch->sample_position);

			// Configure also frequency and volume that might have changed
			// since last tick.
			// debugf("vol %f %f\n", ch->actual_volume, ch->actual_panning);
			mixer_ch_set_freq(first_ch+i, ch->frequency);
			mixer_ch_set_vol(first_ch+i,
				muted ? 0 : gvol * ch->actual_volume[0],
				muted ? 0 : gvol * ch->actual_volume[1]);
		} else {
			// No sample in this channel: the channel is mute. Just stop it.
			mixer_ch_stop(first_ch+i);
		}
	}

	// Schedule next tick according to the number of samples in this tick.
	int delay = ceilf(ctx->remaining_samples_in_tick);
	ctx->remaining_samples_in_tick -= delay;
	return delay;
}

void xm64player_open(xm64player_t *player, const char *fn) {
	memset(player, 0, sizeof(*player));

	// No pending seek at the moment, we start from beginning anyway.
	player->seek.patidx = -1;

	char romfn[strlen(fn)+5+1];
	snprintf(romfn, sizeof(romfn), "rom:/%s", fn);
	player->fh = fopen(romfn, "rb");
	assertf(player->fh >= 0, "Cannot open file: %s", fn);

	// Load the XM context
	int sample_rate = audio_get_frequency();
	assertf(sample_rate >= 0, "audio_init() and mixer_init() must be called before xm64player_open()");
	if (xm_context_load(&player->ctx, player->fh, sample_rate) != 0) {
		// Check if the file looks like a standard XM, so to provide
		// a clear message in that case.
		char signature[16] = {0};
		fseek(player->fh, 0, SEEK_SET);
		fread(signature, 1, 15, player->fh);
		if (strcmp(signature, "Extended Module") == 0) {
			assertf(0, "cannot load XM file %s -- please convert to XM64 with audio64", fn);
		}
		assertf(0, "error loading XM64 file -- file corrupted?");
	}

	uint32_t base_rom_addr = dfs_rom_addr(fn);

	// Count samples
	int ninst = xm_get_number_of_instruments(player->ctx);
	int nwaves = 0;
	for (int i=0;i<ninst;i++)
		nwaves += xm_get_number_of_samples(player->ctx, i+1);

	// Allocate waveforms (one per XM64's "samples" aka waveforms)
	player->waves = malloc(sizeof(waveform_t) * nwaves);
	int nw = 0;
	for (int i=0;i<ninst;i++) {
		xm_instrument_t *inst = &player->ctx->module.instruments[i];
		for (int j=0;j<inst->num_samples;j++) {
			xm_sample_t *samp = &inst->samples[j];

			// Convert offset of samples from relative to absolute
			samp->data8_offset += base_rom_addr;

			// Initialize the waveform_t structures with information
			// coming from the XM "sample".
			samp->wave = &player->waves[nw++];
			memset(samp->wave, 0, sizeof(waveform_t));
			samp->wave->name = strdup(fn); // FIXME: maybe better use a proper name here
			samp->wave->bits = samp->bits;
			samp->wave->channels = 1;
			samp->wave->frequency = sample_rate; // fake, will be changed at every key-on
			samp->wave->len = samp->length;
			samp->wave->loop_len = samp->loop_type == XM_NO_LOOP ? 0 : samp->loop_length;
			if (samp->wave->loop_len&1)
				samp->wave->loop_len -= 1;
			samp->wave->read = wave_read;
			samp->wave->ctx = samp;
		}
	}
}

int xm64player_num_channels(xm64player_t *player) {
	return xm_get_number_of_channels(player->ctx);
}

void xm64player_play(xm64player_t *player, int first_ch) {
	assert(first_ch + xm_get_number_of_channels(player->ctx) <= MIXER_MAX_CHANNELS);

	if (!player->playing) {
		// XM64 header contains the optimal size for sample buffers on each
		// channel, to minimize memory consumption. 
		for (int i=0; i<player->ctx->module.num_channels; i++)
			mixer_ch_set_limits(first_ch+i, 0, 0, player->ctx->ctx_size_stream_sample_buf[i]);

		mixer_add_event(0, tick, player);
		player->first_ch = first_ch;
		player->playing = true;
	}
}

void xm64player_stop(xm64player_t *player) {
	// Let the mixer callback stop playing
	player->playing = false;
}

void xm64player_tell(xm64player_t *player, int *patidx, int *row, float *secs) {
	// Disable interrupts to try to avoid race conditions with the player
	// running in a different thread. Notice that this is not sufficient
	// (you would need some kind of mutex), but let's say good enough,
	// especially since the audio thread is going to be higher priority.
	disable_interrupts();
	uint8_t patidx_, row_; uint64_t samples;
	xm_get_position(player->ctx, &patidx_, NULL, &row_, &samples);
	bool has_pending_seek = player->seek.patidx >= 0;
	if (patidx) *patidx = has_pending_seek ? player->seek.patidx : patidx_;
	if (row) *row = has_pending_seek ? player->seek.row : row_;
	if (secs) *secs = (float)samples / (float)player->ctx->rate;
	enable_interrupts();
}

void xm64player_seek(xm64player_t *player, int patidx, int row, int tick) {
	// Schedule seeking at next tick. Don't seek here to avoid
	// race conditions when the mixer is running in another thread.
	disable_interrupts();
	player->seek.patidx = patidx;
	player->seek.row = row;
	player->seek.tick = tick;
	enable_interrupts();
}

void xm64player_set_vol(xm64player_t *player, float volume) {
	// Store the volume in the libxm context as amplification.
	// 0.25f is the default suggested value, so we scale by it.
	player->ctx->amplification = volume * 0.25f;
}

void xm64player_close(xm64player_t *player) {
	// FIXME: we need to stop playing without racing with the audio thread.
	// This is not correct and may crash.
	disable_interrupts();
	if (player->playing) {
		mixer_remove_event(tick, player);
		player->playing = false;
	}
	for (int i=0;i<player->ctx->module.num_channels;i++) {
		mixer_ch_stop(player->first_ch+i);
		mixer_ch_set_limits(player->first_ch, 0, 0, 0);
	}
	enable_interrupts();

	if (player->fh != NULL) {
		fclose(player->fh);
		player->fh = NULL;
	}

	if (player->waves) {
		for (int i=0;i<player->nwaves;i++)
			free((void*)player->waves[i].name);
		free(player->waves);
		player->waves = NULL;
	}

	if (player->ctx) {
		xm_free_context(player->ctx);
		player->ctx = NULL;
	}
}