#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/time.h>

#include <SDL.h>
#include <SDL_thread.h>

/* The output audio sample rate. */
static int audio_sample_rate = 44100;

const int CHANNELS = 2;
const int BPC = 2; // Bytes per channel.
const int BPS = 4; // Bytes per sample.

const int FRAMES = 3;
const int FRAME_PADDING = 2; // Pixels on each side.

static SDL_Surface *rgb_surface = NULL;
static SDL_Surface *rgba_surface = NULL;

// http://dranger.com/ffmpeg/

/*******************************************************************************
 * SDL_RWops <-> AVIOContext
 * */

static int rwops_read(void *opaque, uint8_t *buf, int buf_size) {
    SDL_RWops *rw = (SDL_RWops *) opaque;

    int rv = rw->read(rw, buf, 1, buf_size);
    return rv;

}

static int rwops_write(void *opaque, uint8_t *buf, int buf_size) {
    printf("Writing to an SDL_rwops is a really bad idea.\n");
    return -1;
}

static int64_t rwops_seek(void *opaque, int64_t offset, int whence) {
    SDL_RWops *rw = (SDL_RWops *) opaque;

    if (whence == AVSEEK_SIZE) {
    	return rw->size(rw);
    }

    // Ignore flags like AVSEEK_FORCE.
    whence &= (SEEK_SET | SEEK_CUR | SEEK_END);

    int64_t rv = rw->seek(rw, (int) offset, whence);
    return rv;
}

#define RWOPS_BUFFER 65536


static AVIOContext *rwops_open(SDL_RWops *rw) {

    unsigned char *buffer = av_malloc(RWOPS_BUFFER);
    AVIOContext *rv = avio_alloc_context(
        buffer,
        RWOPS_BUFFER,
        0,
        rw,
        rwops_read,
        rwops_write,
        rwops_seek);

    return rv;
}

static void rwops_close(SDL_RWops *rw) {
	rw->close(rw);
}

static double get_time(void) {
	return av_gettime() * 1e-6;
}


typedef struct PacketQueue {
	AVPacketList *first;
	AVPacketList *last;
} PacketQueue;

typedef struct FrameQueue {
	AVFrame *first;
	AVFrame *last;
} FrameQueue;


typedef struct SurfaceQueueEntry {
	struct SurfaceQueueEntry *next;

	SDL_Surface *surf;

	/* The pts, converted to seconds. */
	double pts;
} SurfaceQueueEntry;

typedef struct MediaState {


	/* The condition and lock. */
	SDL_cond* cond;
	SDL_mutex* lock;


	SDL_RWops *rwops;
	char *filename;

	/*
	 * This becomes true when the decode thread starts, when
	 * it is the decode thread's job to deallocate this object.
	 */
	int started;

	/* This becomes true once the decode thread has finished initializing
	 * and the readers and writers can do their thing.
	 */
	int ready; // Lock.

	/* This is set to true when data has been read, in order to ask the
	 * decode thread to produce more data.
	 */
	int needs_decode; // Lock.

	/*
	 * This is set to true when data has been read, in order to ask the
	 * decode thread to shut down and deallocate all resources.
	 */
	int quit; // Lock

	/* The number of seconds to skip at the start. */
	double skip;

	/* These become true when the audio and video finish. */
	int audio_finished;
	int video_finished;

	/* Indexes of video and audio streams. */
	int video_stream;
	int audio_stream;

	/* The main context. */
	AVFormatContext *ctx;

	/* Contexts for decoding audio and video streams. */
	AVCodecContext *video_context;
	AVCodecContext *audio_context;

	/* Queues of packets going to the audio and video
	 * streams.
	 */
	PacketQueue video_packet_queue;
	PacketQueue audio_packet_queue;

	/* The queue of converted audio frames. */
	FrameQueue audio_queue; // Lock

	/* The size of the audio queue, and the target size in seconds. */
	int audio_queue_samples;
	int audio_queue_target_seconds;

	/* A frame used for decoding. */
	AVFrame *audio_decode_frame;

	/* The audio frame being read from, and the index into the audio frame. */
	AVFrame *audio_out_frame; // Lock
	int audio_out_index; // Lock

	SwrContext *swr;

	/* The duration of the audio stream, in samples.
	 * 0 means to play until we run out of data.
	 */
	unsigned int audio_duration;

	/* The number of samples that have been read so far. */
	unsigned int audio_read_samples; // Lock

	/* A frame that video is decoded into. */
	AVFrame *video_decode_frame;

	/* The video packet we're decoding, and the partial packet. */
	AVPacket video_pkt;
	AVPacket video_pkt_tmp;


} MediaState;

static AVFrame *dequeue_frame(FrameQueue *fq);
static void free_packet_queue(PacketQueue *pq);

static void deallocate(MediaState *ms) {

	/* Destroy video stuff. */
	av_frame_free(&ms->video_decode_frame);

	av_free_packet(&ms->video_pkt);

	/* Destroy audio stuff. */
	swr_free(&ms->swr);

	av_frame_free(&ms->audio_decode_frame);
	av_frame_free(&ms->audio_out_frame);

	while (1) {
		AVFrame *f = dequeue_frame(&ms->audio_queue);

		if (!f) {
			break;
		}

		av_frame_free(&f);
	}

	/* Destroy/Close core stuff. */
	free_packet_queue(&ms->audio_packet_queue);
	free_packet_queue(&ms->video_packet_queue);

	avcodec_free_context(&ms->video_context);
	avcodec_free_context(&ms->audio_context);

	avformat_close_input(&ms->ctx);

	/* Destroy alloc stuff. */
	SDL_DestroyCond(ms->cond);
	SDL_DestroyMutex(ms->lock);

	rwops_close(ms->rwops);

	av_free(ms->filename);
	av_free(ms);
}

static void enqueue_frame(FrameQueue *fq, AVFrame *frame) {
	frame->opaque = NULL;

	if (fq->first) {
		fq->last->opaque = frame;
		fq->last = frame;
	} else {
		fq->first = fq->last = frame;
	}
}

static AVFrame *dequeue_frame(FrameQueue *fq) {
	if (!fq->first) {
		return NULL;
	}

	AVFrame *rv = fq->first;
	fq->first = (AVFrame *) rv->opaque;

	if (!fq->first) {
		fq->last = NULL;
	}

	return rv;
}


static void enqueue_packet(PacketQueue *pq, AVPacket *pkt) {
	AVPacketList *pl = av_malloc(sizeof(AVPacketList));

	pl->pkt = *pkt;
	pl->next = NULL;

	if (!pq->first) {
		pq->first = pq->last = pl;
	} else {
		pq->last->next = pl;
		pq->last = pl;
	}
}

static int dequeue_packet(PacketQueue *pq, AVPacket *pkt) {
	if (! pq->first ) {
		return 0;
	}

	AVPacketList *pl = pq->first;

	*pkt = pl->pkt;

	pq->first = pl->next;

	if (!pq->first) {
		pq->last = NULL;
	}

	av_free(pl);

	return 1;
}

static void free_packet_queue(PacketQueue *pq) {
	AVPacket scratch;

	while (dequeue_packet(pq, &scratch)) {
		av_free_packet(&scratch);
	}
}


/**
 * Reads a packet from one of the queues, filling the other queue if
 * necessary.
 */
static int read_packet(MediaState *ms, PacketQueue *pq, AVPacket *pkt) {
	AVPacket scratch;

	while (1) {
		if (dequeue_packet(pq, pkt)) {
			return 1;
		}

		if (av_read_frame(ms->ctx, &scratch)) {
			pkt->data = NULL;
			pkt->size = 0;
			return 0;
		}

		av_dup_packet(&scratch);

		if (scratch.stream_index == ms->video_stream) {
			enqueue_packet(&ms->video_packet_queue, &scratch);
		} else if (scratch.stream_index == ms->audio_stream) {
			enqueue_packet(&ms->audio_packet_queue, &scratch);
		} else {
			av_free_packet(&scratch);
		}
	}


}

static AVCodecContext *find_context(AVFormatContext *ctx, int index) {

	if (index == -1) {
		return NULL;
	}

	AVCodec *codec;
	AVCodecContext *codec_ctx = NULL;
	AVCodecContext *codec_ctx_orig = ctx->streams[index]->codec;

	codec = avcodec_find_decoder(codec_ctx_orig->codec_id);

	if (codec == NULL) {
		return NULL;
	}

	codec_ctx = avcodec_alloc_context3(codec);

	if (avcodec_copy_context(codec_ctx, codec_ctx_orig)) {
		goto fail;
	}

	if (avcodec_open2(codec_ctx, codec, NULL)) {
		goto fail;
	}

	return codec_ctx;

fail:
	avcodec_free_context(&codec_ctx);
	return NULL;
}


/**
 * Decodes audio. Returns 0 if no audio was decoded, or 1 if some audio was
 * decoded.
 */
static void decode_audio(MediaState *ms) {
	AVPacket pkt;
	AVPacket pkt_temp;
	AVFrame *converted_frame;

	if (!ms->audio_context) {
		ms->audio_finished = 1;
		return;
	}

	if (ms->audio_decode_frame == NULL) {
		ms->audio_decode_frame = av_frame_alloc();
	}

	double timebase = av_q2d(ms->ctx->streams[ms->audio_stream]->time_base);

	while (ms->audio_queue_samples < ms->audio_queue_target_seconds * audio_sample_rate ) {

		read_packet(ms, &ms->audio_packet_queue, &pkt);

		pkt_temp = pkt;

		do {
			int got_frame;
			int read_size = avcodec_decode_audio4(ms->audio_context, ms->audio_decode_frame, &got_frame, &pkt_temp);

			if (read_size < 0) {
				ms->audio_finished = 1;
				return;
			}

			pkt_temp.data += read_size;
			pkt_temp.size -= read_size;

			if (!got_frame) {
				if (pkt.data == NULL) {
					ms->audio_finished = 1;
					av_free_packet(&pkt);
					return;
				}

				break;
			}

			if (!ms->audio_decode_frame->channel_layout) {
				ms->audio_decode_frame->channel_layout = av_get_default_channel_layout(ms->audio_decode_frame->channels);
			}

			converted_frame = av_frame_alloc();
			converted_frame->sample_rate = audio_sample_rate;
			converted_frame->channel_layout = AV_CH_LAYOUT_STEREO;
			converted_frame->format = AV_SAMPLE_FMT_S16;

			if(swr_convert_frame(ms->swr, converted_frame, ms->audio_decode_frame)) {
				av_frame_free(&converted_frame);
				continue;
			}


			double start = av_frame_get_best_effort_timestamp(ms->audio_decode_frame) * timebase;
			double end = start + 1.0 * converted_frame->nb_samples / audio_sample_rate;

			SDL_LockMutex(ms->lock);

			if (start >= ms->skip) {

				// Normal case, queue the frame.
				ms->audio_queue_samples += converted_frame->nb_samples;
				enqueue_frame(&ms->audio_queue, converted_frame);

			} else if (end < ms->skip) {
				// Totally before, drop the frame.
				av_frame_free(&converted_frame);

			} else {
				// The frame straddles skip, so we queue the (necessarily single)
				// frame and set the index into the frame.
				ms->audio_out_frame = converted_frame;
				ms->audio_out_index = BPS * (int) ((ms->skip - start) * audio_sample_rate);

			}

			SDL_UnlockMutex(ms->lock);

		} while (pkt_temp.size);

		av_free_packet(&pkt);
	}

	return;

}

static SurfaceQueueEntry *decode_video_frame(MediaState *ms) {

	while (1) {

		if (! ms->video_pkt_tmp.size) {
			av_free_packet(&ms->video_pkt);
			read_packet(ms, &ms->video_packet_queue, &ms->video_pkt);
			ms->video_pkt_tmp = ms->video_pkt;
		}

		int got_frame = 0;
		int read_size = avcodec_decode_video2(ms->video_context, ms->video_decode_frame, &got_frame, &ms->video_pkt_tmp);

		if (read_size < 0) {
			printf("Bad exit.\n");
			ms->video_finished = 1;
			return NULL;
		}

		ms->video_pkt_tmp.data += read_size;
		ms->video_pkt_tmp.size -= read_size;

		if (got_frame) {
			break;
		}

		if (!got_frame && !ms->video_pkt.size) {
			printf("Good exit.\n");
			ms->video_finished = 1;
			return NULL;
		}

	}

//	double timebase = 1.0 * ms->video_context->time_base.num / ms->video_context->time_base.den;

	double pts = 1.0 * av_frame_get_best_effort_timestamp(ms->video_decode_frame) \
			* av_q2d(ms->ctx->streams[ms->video_stream]->time_base);

//	printf("%f %f\n", pts, timebase);

	printf("%f\n", pts);

	return NULL;
}


static void decode_video(MediaState *ms) {
	if (!ms->video_context) {
		ms->video_finished = 1;
		return;
	}

	if (!ms->video_decode_frame) {
		ms->video_decode_frame = av_frame_alloc();
	}

	while (!ms->video_finished) {
		decode_video_frame(ms);
	}

}


static int decode_thread(void *arg) {
	MediaState *ms = (MediaState *) arg;

	int err;

	AVIOContext *io_context = rwops_open(ms->rwops);

	AVFormatContext *ctx = avformat_alloc_context();
	ms->ctx = ctx;

	ctx->pb = io_context;

	err = avformat_open_input(&ctx, ms->filename, NULL, NULL);
	if (err) {
		goto finish;
	}

	err = avformat_find_stream_info(ctx, NULL);
	if (err) {
		goto finish;
	}

	ms->video_stream = -1;
	ms->audio_stream = -1;

	for (int i = 0; i < ctx->nb_streams; i++) {
		if (ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			if (ms->video_stream == -1) {
				ms->video_stream = i;
			}
		}

		if (ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
			if (ms->audio_stream == -1) {
				ms->audio_stream = i;
			}
		}
	}

	ms->video_context = find_context(ctx, ms->video_stream);
	ms->audio_context = find_context(ctx, ms->audio_stream);

	ms->swr = swr_alloc();

	// Compute the number of samples we need to play back.

	if (av_fmt_ctx_get_duration_estimation_method(ctx) != AVFMT_DURATION_FROM_BITRATE) {

		long long duration = ((long long) ctx->duration) * audio_sample_rate;
		ms->audio_duration = (unsigned int) (duration /  AV_TIME_BASE);

		// Check that the duration is reasonable (between 0s and 3600s). If not,
		// reject it.
		if (ms->audio_duration < 0 || ms->audio_duration > 3600 * audio_sample_rate) {
			ms->audio_duration = 0;
		}
	}

	if (ms->skip != 0.0) {
		av_seek_frame(ctx, -1, (int64_t) (ms->skip * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
	}

	while (!ms->quit) {

		if (! ms->audio_finished) {
			decode_audio(ms);
		}

		if (! ms->video_finished) {
			decode_video(ms);
		}

		SDL_LockMutex(ms->lock);

		if (!ms->ready) {
			ms->ready = 1;
			SDL_CondBroadcast(ms->cond);
		}

		if (!(ms->needs_decode || ms->quit)) {
			SDL_CondWait(ms->cond, ms->lock);
		}

		ms->needs_decode = 0;

		SDL_UnlockMutex(ms->lock);
	}


finish:
	/* Data used by the decoder should be freed here, while data shared with
	 * the readers should be freed in media_close.
	 */

	SDL_LockMutex(ms->lock);

	/* Ensures that every stream becomes ready. */
	if (!ms->ready) {
		ms->ready = 1;
		SDL_CondBroadcast(ms->cond);
	}

	while (!ms->quit) {
		SDL_CondWait(ms->cond, ms->lock);
	}

	SDL_UnlockMutex(ms->lock);

	deallocate(ms);

	return 0;
}


int media_read_audio(struct MediaState *ms, Uint8 *stream, int len) {
	SDL_LockMutex(ms->lock);

	while (!ms->ready) {
		SDL_CondWait(ms->cond, ms->lock);
	}

	int rv = 0;

	ms->audio_duration = 0;

	if (ms->audio_duration) {
		unsigned int remaining = (ms->audio_duration - ms->audio_read_samples) * BPS;
		if (len > remaining) {
			len = remaining;
		}

		if (!remaining) {
			ms->audio_finished = 1;
		}

	}

	while (len) {

		if (!ms->audio_out_frame) {
			ms->audio_out_frame = dequeue_frame(&ms->audio_queue);
			ms->audio_out_index = 0;
		}

		if (!ms->audio_out_frame) {
			break;
		}

		AVFrame *f = ms->audio_out_frame;

		int avail = f->nb_samples * BPS - ms->audio_out_index;
		int count;

		if (len > avail) {
			count = avail;
		} else {
			count = len;
		}

		memcpy(stream, &f->data[0][ms->audio_out_index], count);

		ms->audio_out_index += count;

		ms->audio_read_samples += count / BPS;
		ms->audio_queue_samples -= count / BPS;

		rv += count;
		len -= count;
		stream += count;

		if (ms->audio_out_index >= f->nb_samples * BPS) {
			av_frame_free(&ms->audio_out_frame);
			ms->audio_out_index = 0;
		}
	}

	/* Only signal if we've consumed something. */
	if (rv) {
		ms->needs_decode = 1;
		SDL_CondBroadcast(ms->cond);
	}

	SDL_UnlockMutex(ms->lock);

	return rv;
}

void media_start(MediaState *ms) {
	char buf[1024];

	snprintf(buf, 1024, "decode: %s", ms->filename);
	SDL_Thread *t = SDL_CreateThread(decode_thread, buf, (void *) ms);

	if (t) {
		ms->started = 1;
		SDL_DetachThread(t);
	}
}


MediaState *media_open(SDL_RWops *rwops, const char *filename) {
	MediaState *ms = av_calloc(1, sizeof(MediaState));

	ms->filename = av_strdup(filename);
	ms->rwops = rwops;

	ms->cond = SDL_CreateCond();
	ms->lock = SDL_CreateMutex();

	ms->audio_queue_target_seconds = 2;

	return ms;
}

/**
 * Sets the start and end of the stream. This must be called before
 * media_start.
 *
 * start
 *    The time in the stream at which the media starts playing.
 * end
 *    If not 0, the time at which the stream is forced to end if it has not
 *    already. If 0, the stream plays until its natural end.
 */
void media_start_end(MediaState *ms, double start, double end) {
	ms->skip = start;

	if (end != 0) {
		ms->audio_duration = (int) ((end - start) * audio_sample_rate);
	}
}


void media_close(MediaState *ms) {

	if (!ms->started) {
		deallocate(ms);
		return;
	}

	/* Tell the decoder to terminate. It will deallocate everything for us. */
	SDL_LockMutex(ms->lock);
	ms->quit = 1;
	SDL_CondBroadcast(ms->cond);
	SDL_UnlockMutex(ms->lock);

}

void media_sample_surfaces(SDL_Surface *rgb, SDL_Surface *rgba) {
	rgb_surface = rgb;
	rgba_surface = rgba;
}


void media_init(int rate, int status) {

	audio_sample_rate = rate;

    av_register_all();

    if (status) {
        av_log_set_level(AV_LOG_INFO);
    } else {
        av_log_set_level(AV_LOG_ERROR);
    }

}


