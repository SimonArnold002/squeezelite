/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2025, ralph_irving@hotmail.com
 *  
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Additions (c) Paul Hermann, 2015-2025 under the same license terms
 *   -Control of Raspberry pi GPIO for amplifier power
 *   -Launch script on power status change from LMS
 */

#include "squeezelite.h"
#include <jni.h>
#include <signal.h>
#include <dlfcn.h>
#include <unwind.h>
#include <ucontext.h>
#include <sys/prctl.h>
#include <android/api-level.h>
#include <sys/system_properties.h>
#include <ctype.h>

extern struct outputstate output;
extern struct buffer *outputbuf;
extern bool user_rates;
extern bool pcm_track_on;
extern bool sas_on;
extern bool es_on;
#if DSD
extern dsd_format _es_dsd_format(bool native);
#endif

static JavaVM *jvm = NULL;
static jclass clazz = 0;
static jobject obj = 0;

extern int PaOpenSLES_ENABLED;
extern int PaAAudio_ENABLED;
extern int LibAAudio_init();

static void sighandler(int signum) {
	slimproto_stop();

	// remove ourselves in case above does not work, second SIGINT will cause non gracefull shutdown
	signal(signum, SIG_DFL);
}

/**
 Catch SEGV, log, and exit (normally). This is to prevent Android showing errors when stopping.
 */
// TEST BUILD (dsdtest-30): log the crashing thread, the fault address and a backtrace first, to find
// the crash at the R5's player shutdown (ledger R5 SHUTDOWN SEGV)
struct crash_trace { uintptr_t pc[24]; int n; };

static _Unwind_Reason_Code crash_frame(struct _Unwind_Context *ctx, void *arg) {
	struct crash_trace *t = arg;
	uintptr_t pc = _Unwind_GetIP(ctx);
	if (pc && t->n < 24) t->pc[t->n++] = pc;
	return t->n < 24 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

static void crash_where(const char *what, uintptr_t pc) {
	Dl_info info;
	if (pc && dladdr((void *)pc, &info) && info.dli_fname) {
		LOG_ERROR("  %s %s+%#lx %s", what, info.dli_fname, (unsigned long)(pc - (uintptr_t)info.dli_fbase),
				  info.dli_sname ? info.dli_sname : "");
	} else {
		LOG_ERROR("  %s %#lx", what, (unsigned long)pc);
	}
}

static void segv_handler(int sig, siginfo_t *si, void *uc_void) {
	ucontext_t *uc = uc_void;
	struct crash_trace t = { .n = 0 };
	char name[17] = "";
	int i;
	prctl(PR_GET_NAME, name);
	LOG_ERROR("SEGV/ABRT! signal %d code %d addr %p, thread %d '%s'", sig, si ? si->si_code : 0,
			  si ? si->si_addr : NULL, gettid(), name);
#if defined(__aarch64__)
	crash_where("pc", uc->uc_mcontext.pc);
	crash_where("lr", uc->uc_mcontext.regs[30]);
#elif defined(__arm__)
	crash_where("pc", uc->uc_mcontext.arm_pc);
	crash_where("lr", uc->uc_mcontext.arm_lr);
#endif
	_Unwind_Backtrace(crash_frame, &t);
	for (i = 0; i < t.n; i++) {
		char what[8];
		snprintf(what, sizeof(what), "#%02d", i);
		crash_where(what, t.pc[i]);
	}
	exit(0);
}

static void init_jvm(JNIEnv * env, jobject jobj) {
	jvm = NULL;
	if (JNI_OK!=(*env)->GetJavaVM(env, &jvm)) {
		LOG_ERROR("Failed to get java VM");
		return;
	}
	obj = (*env)->NewGlobalRef(env, jobj);
	clazz = (*env)->NewGlobalRef(env, (*env)->GetObjectClass(env, obj));
}

#if DSD
static log_level loglevel = lERROR; // for the functions below; start() shadows it with its parameter

static void set_dsd_loglevel(log_level level) {
	loglevel = level;
}

/*
 Native DSD on FiiO players. FiiO's Android adds AudioFormat.ENCODING_DSD_32BIT; an AudioTrack
 opened with it is routed to the DAC's DSD path. The track rate is the DSD bit rate / 32, and
 the data is the DSD_U16_LE byte stream (16-bit L/R interleave, earliest bit in the MSB),
 packed by output_pa.c. Only the writer threads in output_pa.c, one at a time, call the track
 functions below (the DSD writer, and the PCM writer on Android 8.1).
 */
static int dsd_encoding = 0;

// FiiO firmware gives apps PCM up to 384k (705.6k and 768k play only in its USB DAC mode: an
// app's stream is set to the rate but runs at ~61k, silent) and native DSD up to DSD256. So
// the player offers rates up to 384k: LMS downsamples higher-rate files, and DSD converted to
// PCM above that (DSD512) is resampled here, only when needed.
static const unsigned fiio_rates[] = { 384000, 352800, 192000, 176400, 96000, 88200, 48000, 44100,
									   32000, 24000, 22050, 16000, 12000, 11025, 8000 };

// The output route, from Library's audio device callback (its "OutputRoute" thread, never the
// main thread: output_route_pa waits for the output lock): whether it takes native DSD, and the
// rates of the USB DAC it goes to (route_dac false: the player's own rates). Kept here across
// player restarts and handed to output_pa.c only while the output module is up (route_live, from
// output init to close, and only with native DSD): route_mutex orders that against start()
// bringing the output up and down.
extern bool output_route_pa(bool dsd_ok, const unsigned dac_rates[]);
static pthread_mutex_t route_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool route_ok = true;
static bool route_dac = false;
static unsigned route_rates[MAX_SUPPORTED_SAMPLERATES];
static bool route_live = false;

static void dsd_route_live(bool live) {
	pthread_mutex_lock(&route_mutex);
	route_live = live && dsd_encoding;
	if (route_live) output_route_pa(route_ok, route_dac ? route_rates : NULL);
	pthread_mutex_unlock(&route_mutex);
}

// The writer's AudioTrack: native DSD, or PCM on Android 8.1 (pcm_track_open), written as floats.
// output_pa.c runs one writer at a time, so the two share these
static JNIEnv *dsd_env = NULL;
static jobject dsd_track = NULL;
static jbyteArray dsd_array = NULL;
static jint dsd_array_len = 0;
static jfloatArray pcm_array = NULL;
static jint pcm_array_len = 0;
static jmethodID dsd_write_id, pcm_write_id, dsd_head_id;
static const char *track_kind = "DSD"; // for the log

static bool jni_failed(JNIEnv *env, const char *what) {
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
		LOG_ERROR("%s AudioTrack: %s failed", track_kind, what);
		return true;
	}
	return false;
}

// FiiO's ENCODING_DSD_32BIT, or 0 when this Android does not have it
static int find_dsd_encoding(JNIEnv *env) {
	jclass fmt = (*env)->FindClass(env, "android/media/AudioFormat");
	if (!fmt) {
		(*env)->ExceptionClear(env);
		return 0;
	}
	jfieldID id = (*env)->GetStaticFieldID(env, fmt, "ENCODING_DSD_32BIT", "I");
	int encoding = 0;
	if (id) {
		encoding = (*env)->GetStaticIntField(env, fmt, id);
	} else {
		(*env)->ExceptionClear(env);
	}
	(*env)->DeleteLocalRef(env, fmt);
	return encoding;
}

// builder.name(arg), or NULL (exception cleared) when the method is missing or throws:
// the Builder setters throw IllegalArgumentException for a value the firmware rejects
static jobject call_builder(JNIEnv *env, jobject builder, const char *name, const char *ret, jint arg) {
	jclass cls = (*env)->GetObjectClass(env, builder);
	char sig[96];
	snprintf(sig, sizeof(sig), "(I)%s", ret);
	jmethodID id = (*env)->GetMethodID(env, cls, name, sig);
	jobject next = NULL;
	(*env)->DeleteLocalRef(env, cls);
	if (jni_failed(env, name) || !id) return NULL;
	next = (*env)->CallObjectMethod(env, builder, id, arg);
	if (jni_failed(env, name)) return NULL;
	return next;
}

static jobject build(JNIEnv *env, jobject builder, const char *sig) {
	jclass cls = (*env)->GetObjectClass(env, builder);
	jmethodID id = (*env)->GetMethodID(env, cls, "build", sig);
	jobject built = NULL;
	(*env)->DeleteLocalRef(env, cls);
	if (jni_failed(env, "build") || !id) return NULL;
	built = (*env)->CallObjectMethod(env, builder, id);
	if (jni_failed(env, "build")) return NULL;
	return built;
}

// a set... call on a builder; false when it failed
static bool set(JNIEnv *env, jobject builder, const char *name, const char *ret, jint arg) {
	jobject next = call_builder(env, builder, name, ret, arg);
	if (!next) return false;
	(*env)->DeleteLocalRef(env, next);
	return true;
}

// the same for a setter taking an object
static bool set_object(JNIEnv *env, jobject builder, const char *name, const char *sig, jobject arg) {
	jclass cls = (*env)->GetObjectClass(env, builder);
	jmethodID id = (*env)->GetMethodID(env, cls, name, sig);
	jobject next = NULL;
	(*env)->DeleteLocalRef(env, cls);
	if (jni_failed(env, name) || !id) return false;
	next = (*env)->CallObjectMethod(env, builder, id, arg);
	if (jni_failed(env, name) || !next) return false;
	(*env)->DeleteLocalRef(env, next);
	return true;
}

// AudioTrack.Builder in PERFORMANCE_MODE_POWER_SAVING (API 26), which asks for a deep buffer
// output; NULL (exception cleared) on a failure
static jobject build_power_saving(JNIEnv *env, jobject attrs, jobject format, unsigned buffer_bytes) {
	jclass cls = (*env)->FindClass(env, "android/media/AudioTrack$Builder");
	jobject b = (*env)->NewObject(env, cls, (*env)->GetMethodID(env, cls, "<init>", "()V"));
	(*env)->DeleteLocalRef(env, cls);
	if (jni_failed(env, "AudioTrack.Builder")) return NULL;
	if (!set_object(env, b, "setAudioAttributes", "(Landroid/media/AudioAttributes;)Landroid/media/AudioTrack$Builder;", attrs) ||
		!set_object(env, b, "setAudioFormat", "(Landroid/media/AudioFormat;)Landroid/media/AudioTrack$Builder;", format) ||
		!set(env, b, "setBufferSizeInBytes", "Landroid/media/AudioTrack$Builder;", buffer_bytes) ||
		!set(env, b, "setTransferMode", "Landroid/media/AudioTrack$Builder;", 1) || // MODE_STREAM
		!set(env, b, "setPerformanceMode", "Landroid/media/AudioTrack$Builder;", 2)) { // PERFORMANCE_MODE_POWER_SAVING
		return NULL;
	}
	return build(env, b, "()Landroid/media/AudioTrack;");
}

// Opens a playing AudioTrack (USAGE_MEDIA, CONTENT_TYPE_MUSIC, stereo) into dsd_track, on this
// thread, which stays attached until dsd_track_close. Native DSD uses the constructor;
// power_saving uses build_power_saving. Local refs left behind on a failure are freed by
// DetachCurrentThread
static bool track_open(int encoding, unsigned rate, unsigned buffer_bytes, bool power_saving) {
	JNIEnv *env;
	jclass cls;
	jobject b, attrs, format, track = NULL;

	if ((*jvm)->AttachCurrentThread(jvm, &env, NULL) != JNI_OK) {
		LOG_ERROR("%s AudioTrack: failed to attach thread", track_kind);
		return false;
	}
	dsd_env = env;

	cls = (*env)->FindClass(env, "android/media/AudioAttributes$Builder");
	b = (*env)->NewObject(env, cls, (*env)->GetMethodID(env, cls, "<init>", "()V"));
	(*env)->DeleteLocalRef(env, cls);
	if (jni_failed(env, "AudioAttributes.Builder")) goto fail;
	if (!set(env, b, "setUsage", "Landroid/media/AudioAttributes$Builder;", 1) || // USAGE_MEDIA
		!set(env, b, "setContentType", "Landroid/media/AudioAttributes$Builder;", 2) || // CONTENT_TYPE_MUSIC
		!(attrs = build(env, b, "()Landroid/media/AudioAttributes;"))) goto fail;
	(*env)->DeleteLocalRef(env, b);

	cls = (*env)->FindClass(env, "android/media/AudioFormat$Builder");
	b = (*env)->NewObject(env, cls, (*env)->GetMethodID(env, cls, "<init>", "()V"));
	(*env)->DeleteLocalRef(env, cls);
	if (jni_failed(env, "AudioFormat.Builder")) goto fail;
	if (!set(env, b, "setEncoding", "Landroid/media/AudioFormat$Builder;", encoding) ||
		!set(env, b, "setSampleRate", "Landroid/media/AudioFormat$Builder;", rate) ||
		!set(env, b, "setChannelMask", "Landroid/media/AudioFormat$Builder;", 0xC) || // CHANNEL_OUT_STEREO
		!(format = build(env, b, "()Landroid/media/AudioFormat;"))) goto fail;
	(*env)->DeleteLocalRef(env, b);

	cls = (*env)->FindClass(env, "android/media/AudioTrack");
	if (power_saving) {
		if (!(track = build_power_saving(env, attrs, format, buffer_bytes))) goto fail;
	} else {
		track = (*env)->NewObject(env, cls, (*env)->GetMethodID(env, cls, "<init>",
								  "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;III)V"),
								  attrs, format, (jint)buffer_bytes, 1 /* MODE_STREAM */, 0 /* AUDIO_SESSION_ID_GENERATE */);
		if (jni_failed(env, "AudioTrack")) goto fail;
	}
	dsd_write_id = (*env)->GetMethodID(env, cls, "write", "([BII)I");
	pcm_write_id = (*env)->GetMethodID(env, cls, "write", "([FIII)I");
	dsd_head_id = (*env)->GetMethodID(env, cls, "getPlaybackHeadPosition", "()I");
	if ((*env)->CallIntMethod(env, track, (*env)->GetMethodID(env, cls, "getState", "()I")) != 1) { // STATE_INITIALIZED
		jni_failed(env, "getState");
		LOG_ERROR("%s AudioTrack: not initialised at %u", track_kind, rate);
		goto fail;
	}
	(*env)->CallVoidMethod(env, track, (*env)->GetMethodID(env, cls, "play", "()V"));
	if (jni_failed(env, "play")) goto fail;
	(*env)->DeleteLocalRef(env, cls);

	dsd_track = (*env)->NewGlobalRef(env, track);
	(*env)->DeleteLocalRef(env, track);
	(*env)->DeleteLocalRef(env, attrs);
	(*env)->DeleteLocalRef(env, format);
	return true;

fail:
	if (track) {
		// release now: an unreleased track holds the output until the GC finalizes it
		(*env)->CallVoidMethod(env, track, (*env)->GetMethodID(env, cls, "release", "()V"));
		jni_failed(env, "release");
	}
	(*jvm)->DetachCurrentThread(jvm);
	dsd_env = NULL;
	return false;
}

bool dsd_track_open(unsigned rate, unsigned buffer_bytes, unsigned chunk_bytes) {
	JNIEnv *env;

	if (!jvm || !dsd_encoding) return false;
	track_kind = "DSD";
	if (!track_open(dsd_encoding, rate, buffer_bytes, false)) return false;
	env = dsd_env;
	dsd_array = (*env)->NewGlobalRef(env, (*env)->NewByteArray(env, chunk_bytes));
	dsd_array_len = chunk_bytes;
	LOG_INFO("DSD AudioTrack open: rate %u, encoding %d, buffer %u bytes", rate, dsd_encoding, buffer_bytes);
	return true;
}

// PCM on Android 8.1 (output_pa.c pcm_writer): stereo ENCODING_PCM_FLOAT at rate with a buffer of
// buffer_frames, written in chunks of up to chunk_frames. In power saving mode, which on the HiBy R5
// gives the deep buffer direct output that OpenSL ES and AAudio test streams played on without a
// fault; the AAudio run the R5's driver deadlocked on asked for no mode
bool pcm_track_open(unsigned rate, unsigned buffer_frames, unsigned chunk_frames) {
	JNIEnv *env;
	jclass cls;
	jint frames;

	if (!jvm) return false;
	track_kind = "PCM";
	if (!track_open(4 /* ENCODING_PCM_FLOAT */, rate, buffer_frames * 8, true)) return false;
	env = dsd_env;
	pcm_array = (*env)->NewGlobalRef(env, (*env)->NewFloatArray(env, chunk_frames * 2));
	pcm_array_len = chunk_frames * 2;
	cls = (*env)->GetObjectClass(env, dsd_track);
	frames = (*env)->CallIntMethod(env, dsd_track, (*env)->GetMethodID(env, cls, "getBufferSizeInFrames", "()I"));
	if (jni_failed(env, "getBufferSizeInFrames")) frames = 0;
	(*env)->DeleteLocalRef(env, cls);
	LOG_INFO("PCM AudioTrack open: rate %u, float, buffer %d frames (%u asked)", rate, frames, buffer_frames);
	return true;
}

// blocking write of the whole buffer; returns false on a write error
bool dsd_track_write(const u8_t *buf, unsigned len) {
	JNIEnv *env = dsd_env;
	if (!env || !dsd_track || len > (unsigned)dsd_array_len) return false;
	(*env)->SetByteArrayRegion(env, dsd_array, 0, len, (const jbyte *)buf);
	unsigned done = 0;
	while (done < len) {
		jint n = (*env)->CallIntMethod(env, dsd_track, dsd_write_id, dsd_array, (jint)done, (jint)(len - done));
		if (jni_failed(env, "write")) return false;
		if (n <= 0) { // 0 = the track was paused or stopped under us: never spin on it
			LOG_ERROR("DSD AudioTrack: write error %d", n);
			return false;
		}
		done += n;
	}
	return true;
}

// the same for frames stereo float frames (pcm_track_open)
bool pcm_track_write(const float *buf, unsigned frames) {
	JNIEnv *env = dsd_env;
	unsigned len = frames * 2, done = 0;
	if (!env || !dsd_track || !pcm_array || len > (unsigned)pcm_array_len) return false;
	(*env)->SetFloatArrayRegion(env, pcm_array, 0, len, buf);
	while (done < len) {
		jint n = (*env)->CallIntMethod(env, dsd_track, pcm_write_id, pcm_array, (jint)done, (jint)(len - done),
									   0 /* WRITE_BLOCKING */);
		if (jni_failed(env, "write")) return false;
		if (n <= 0) { // 0 = the track was paused or stopped under us: never spin on it
			LOG_ERROR("PCM AudioTrack: write error %d", n);
			return false;
		}
		done += n;
	}
	return true;
}

// frames (at the track rate) the AudioTrack has played
unsigned dsd_track_position(void) {
	JNIEnv *env = dsd_env;
	if (!env || !dsd_track) return 0;
	jint pos = (*env)->CallIntMethod(env, dsd_track, dsd_head_id);
	if (jni_failed(env, "getPlaybackHeadPosition")) return 0;
	return (unsigned)pos;
}

void dsd_track_close(void) {
	JNIEnv *env = dsd_env;
	if (!env) return;
	if (dsd_track) {
		jclass cls = (*env)->GetObjectClass(env, dsd_track);
		(*env)->CallVoidMethod(env, dsd_track, (*env)->GetMethodID(env, cls, "stop", "()V"));
		jni_failed(env, "stop");
		(*env)->CallVoidMethod(env, dsd_track, (*env)->GetMethodID(env, cls, "release", "()V"));
		jni_failed(env, "release");
		(*env)->DeleteLocalRef(env, cls);
		(*env)->DeleteGlobalRef(env, dsd_track);
		dsd_track = NULL;
	}
	if (dsd_array) {
		(*env)->DeleteGlobalRef(env, dsd_array);
		dsd_array = NULL;
	}
	if (pcm_array) {
		(*env)->DeleteGlobalRef(env, pcm_array);
		pcm_array = NULL;
	}
	(*jvm)->DetachCurrentThread(jvm);
	dsd_env = NULL;
	LOG_INFO("%s AudioTrack closed", track_kind);
}

/*
 HiBy's own route to its DAC (the R5, Android 8.1). Its firmware has libsmartaudioservice.so, listed in
 /system/etc/public.libraries.txt so that any app may load it, and HiBy Music plays through it. It builds
 Android's native AudioTrack, which has no 192k limit, on the R5's offload output (PCM 32k-768k). There is
 no header: the calls below are read from its arm64 code (ledger §A3 HIBY'S OWN ROUTE). create_track
 returns a table of functions, each taking the table first. Only the PCM writer in output_pa.c uses it.
 */
struct sas_track {
	size_t (*write)(struct sas_track *t, const void *buf, size_t bytes); // blocking; returns bytes, 0 = no AudioTrack
	void (*stop)(struct sas_track *t);
	void (*stop2)(struct sas_track *t);
	void (*start)(struct sas_track *t);
	void (*flush)(struct sas_track *t);
	int (*playing)(struct sas_track *t);
	int (*latency)(struct sas_track *t);  // AudioTrack::latency(), ms
	int (*rate)(struct sas_track *t);
	int (*channels)(struct sas_track *t);
	int (*unknown)(struct sas_track *t);
	void *ctx;
};
static struct sas_track *(*sas_create)(int rate, int channels, int format, int unused); // format 3: 32-bit PCM, 5: DSD
static void (*sas_release)(struct sas_track *t);
static struct sas_track *sas_track = NULL;

// The rates the player offers through it: its offload output takes 32k-768k, and 384k is the R5's
// own limit. Below 32k PCM goes to OpenSL ES
static const unsigned hiby_rates[] = { 384000, 352800, 192000, 176400, 96000, 88200, 48000, 44100,
									   32000, 24000, 22050, 16000, 12000, 11025, 8000 };

static bool sas_init(void) {
	void *lib = dlopen("libsmartaudioservice.so", RTLD_NOW);
	if (!lib) return false;
	sas_create = dlsym(lib, "create_track");
	sas_release = dlsym(lib, "release_track");
	if (!sas_create || !sas_release) {
		dlclose(lib);
		return false;
	}
	return true;
}

// A playing track at rate frames a second: 32-bit PCM, or with dsd native DSD, whose frames hold 32
// bits a channel (format 5: create_track takes the DSD rate and runs the track at a 32nd of it).
// Returns its latency in ms, or -1 when it failed
int sas_track_open(unsigned rate, bool dsd) {
	unsigned asked = dsd ? rate * 32 : rate;
	struct sas_track *t = sas_create((int)asked, 2, dsd ? 5 : 3, 0);
	int latency;
	if (!t) {
		LOG_ERROR("HiBy track: create_track failed at %u%s", asked, dsd ? " (DSD)" : "");
		return -1;
	}
	// the table holds what was asked: else it is not the one read from the R5's library
	if (t->rate(t) != (int)asked || t->channels(t) != 2) {
		LOG_ERROR("HiBy track: unknown create_track table at %u", asked);
		sas_release(t);
		return -1;
	}
	t->start(t);
	latency = t->latency(t);
	sas_track = t;
	if (dsd) {
		LOG_INFO("HiBy track open: native DSD%u at %u, latency %d ms", asked / 44100, rate, latency);
	} else {
		LOG_INFO("HiBy track open: rate %u, 32-bit, latency %d ms", rate, latency);
	}
	return latency;
}

// blocking write of the whole buffer; false when the track has no AudioTrack
bool sas_track_write(const u8_t *buf, unsigned bytes) {
	return sas_track && sas_track->write(sas_track, buf, bytes) == bytes;
}

void sas_track_close(void) {
	if (sas_track) {
		sas_release(sas_track);
		sas_track = NULL;
		LOG_INFO("HiBy track closed");
	}
}

/*
 Eversolo's own route to its built-in DAC (the DMP-A8, Android 11). Android's Java AudioFormat there has
 no DSD encoding, but the firmware has libaudiosink.so, listed in /system/etc/public.libraries.txt so that
 any app may load it, and Eversolo's own squeezelite (/system/bin/zeabcdclient) plays through it. It builds
 Android's native AudioTrack on the DAC's DIRECT output. There is no header: the calls below are read from
 its arm64 code (ledger §A3 EVERSOLO'S OWN ROUTE). It holds one sink for the whole process.
 audio_sink_open(rate, format, channels) takes Android's PCM formats, DoP as 10/11/12 (32-bit, 8_24 and
 packed 24-bit PCM on the DIRECT output) and AUDIO_FORMAT_DSD. DSD goes as the A8's own DSD output
 setting says, as Eversolo's squeezelite sends it (ledger §A3 EVERSOLO DSD FOLLOWS ITS SETTING): on Native,
 when the output takes it, native DSD (squeezelite's DSD_U32_BE frames, output_pa.c es_dsd_pack); on DoP,
 DoP in 32-bit words, squeezelite's DOP frames as they are; on PCM no DSD at all (LMS converts it). Only
 the PCM writer in output_pa.c uses the sink.
 */
#define ES_FORMAT_DOP_32 10      // DoP in 32-bit words: PCM_32_BIT with the DIRECT flags
#define ES_FORMAT_DSD 0x1A000000 // AUDIO_FORMAT_DSD: native DSD, 32 bits a channel a frame
// the A8's DSD output setting (persist.zidoo.dsd.output.mode: zidoo_ui's "PCM", "DOP", "Native")
#define ES_DSD_PCM 0
#define ES_DSD_DOP 1
#define ES_DSD_NATIVE 2
static int (*es_init_sink)(void);                            // audio_sink_init: gets a session id
static int (*es_open)(int rate, int format, int channels);   // 0 = open
static int (*es_write_loop)(const void *buf, int bytes);     // blocking; bytes written, < 0 = error
static int (*es_latency)(void);                              // AudioTrack::latency(), ms
static int (*es_close)(void);
static int (*es_release)(void);                              // releases the session id
static int (*es_dsd_mode_get)(void);                         // audio_sink_get_usb_dsd_mode: the DSD setting
static int (*es_supports)(int format);                       // audio_sink_is_support_pcm_format: 1 = taken
static bool es_open_now = false;
static int es_dsd_mode_at_start = ES_DSD_DOP;

static bool es_init(void) {
	void *lib = dlopen("libaudiosink.so", RTLD_NOW);
	if (!lib) return false;
	es_init_sink = dlsym(lib, "audio_sink_init");
	es_open = dlsym(lib, "audio_sink_open");
	es_write_loop = dlsym(lib, "audio_sink_write_loop");
	es_latency = dlsym(lib, "audio_sink_get_latency");
	es_close = dlsym(lib, "audio_sink_close");
	es_release = dlsym(lib, "audio_sink_release");
	if (!es_init_sink || !es_open || !es_write_loop || !es_latency || !es_close || !es_release) {
		dlclose(lib);
		return false;
	}
	// without these DSD goes as DoP, the library's own default
	es_dsd_mode_get = dlsym(lib, "audio_sink_get_usb_dsd_mode");
	es_supports = dlsym(lib, "audio_sink_is_support_pcm_format");
	return true;
}

// The A8's DSD output setting, as Eversolo's squeezelite reads it: the library's getDsdOutputMode, which
// reads persist.zidoo.dsd.output.mode and gives DoP when it is unset or unknown
static int es_dsd_mode(void) {
	int mode = es_dsd_mode_get ? es_dsd_mode_get() : ES_DSD_DOP;
	return mode == ES_DSD_PCM || mode == ES_DSD_NATIVE ? mode : ES_DSD_DOP;
}

// Whether the A8's output takes native DSD now (the library asks its policy service): Eversolo's
// squeezelite checks this before it sends native DSD, and sends DoP otherwise
static bool es_native_ok(void) {
	return es_supports && es_supports(ES_FORMAT_DSD);
}

static const char *es_dsd_mode_name(int mode) {
	return mode == ES_DSD_PCM ? "PCM" : mode == ES_DSD_NATIVE ? "Native" : "DoP";
}

// The A8's DAC, as the start-up probe found it (eversolo-7): its direct output takes 44.1k-768k (vendor
// usb_audio_policy_configuration.xml) and Android mixes the lower rates to it. The probe is not run
// there: each rate it tries opens a track on the DAC, which the A8 takes as our player taking its DAC,
// at every player start, with nothing playing (ledger §A3 EVERSOLO HOLDS NO DAC)
static const unsigned es_rates[] = { 768000, 705600, 384000, 352800, 192000, 176400, 96000, 88200, 48000, 44100,
									 32000, 24000, 22050, 16000, 12000, 11025, 8000 };
// A stopped output lets go of the A8's DAC after this, so a skip or a seek keeps it; a pause lets go at
// once (slimproto.c). Elsewhere it is LMS's idle time, 30 s (Library.STREAM_IDLE_TIMEOUT)
#define ES_IDLE_MS 1000

// A playing sink, as Eversolo's squeezelite opens it (init, then open): native DSD at rate frames a
// second (the DSD rate / 32), or DoP (the DSD rate / 16). Returns its latency in ms, or -1 when it failed
int es_track_open(unsigned rate, bool native) {
	int ret, latency;
	es_init_sink();
	ret = es_open((int)rate, native ? ES_FORMAT_DSD : ES_FORMAT_DOP_32, 2);
	if (ret != 0) {
		LOG_ERROR("Eversolo sink: audio_sink_open(%u, %s, 2) failed: %d", rate, native ? "native DSD" : "DoP 32-bit", ret);
		es_release();
		return -1;
	}
	es_open_now = true;
	latency = es_latency();
	LOG_INFO("Eversolo sink open: %s at %u (DSD%u), latency %d ms", native ? "native DSD" : "DoP", rate,
			 rate * (native ? 32 : 16) / 44100, latency);
	return latency < 0 ? 0 : latency;
}

// blocking write of the whole buffer; false when the sink took less
bool es_track_write(const u8_t *buf, unsigned bytes) {
	int ret = es_write_loop(buf, (int)bytes);
	if (ret != (int)bytes) {
		LOG_ERROR("Eversolo sink: audio_sink_write_loop wrote %d of %u bytes", ret, bytes);
		return false;
	}
	return true;
}

void es_track_close(void) {
	if (es_open_now) {
		es_close();
		es_release();
		es_open_now = false;
		LOG_INFO("Eversolo sink closed");
	}
}

/*
 The A8's now playing screen shows the media session of the package in zidoo.music.package.now, which its
 audio libraries fill from the playing process's /proc/self/cmdline (ledger §A3 EVERSOLO VOLUME IS THE
 DAC'S), and when another source takes the DAC the A8 force-stops the package of that name (§A3 EVERSOLO
 KILLS, NEVER PAUSES). The player runs in its own process, "<package>:squeezelitePlayer", which matches no
 session. So the player process takes the name the app gives it (Library.startPlayer): the Squeezelite
 Eversolo helper app's package, whose session is a copy of ours (EversoloHelperLink), or without the
 helper this app's own. As Android's Process.setArgV0 does: the name written over the argument block, the
 rest zeroed. Only the process name changes; the app and its processes are as they were.
 */
static void es_name_process(const char *name) {
	char cmdline[256] = "", stat[1024], *p;
	unsigned long arg_start = 0, arg_end = 0;
	FILE *f;
	size_t n;
	int field;

	if (!(f = fopen("/proc/self/cmdline", "r"))) return;
	n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
	fclose(f);
	cmdline[n] = '\0';
	if (!strcmp(cmdline, name)) return; // already so named

	// arg_start and arg_end are fields 48 and 49; the name in field 2 may hold spaces, so count from its ')'
	if (!(f = fopen("/proc/self/stat", "r"))) return;
	n = fread(stat, 1, sizeof(stat) - 1, f);
	fclose(f);
	stat[n] = '\0';
	if (!(p = strrchr(stat, ')'))) return;
	for (field = 2; p && field < 48; field++) { // each step: p at the space before field + 1
		p = strchr(p + 1, ' ');
	}
	if (!p || sscanf(p + 1, "%lu %lu", &arg_start, &arg_end) != 2 || arg_end <= arg_start ||
		strlen(name) >= arg_end - arg_start || strncmp((char *)arg_start, cmdline, strlen(cmdline))) {
		LOG_ERROR("Eversolo: process not renamed (argument block not found)");
		return;
	}
	memset((char *)arg_start, 0, arg_end - arg_start);
	strcpy((char *)arg_start, name);
	LOG_INFO("Eversolo: player process named %s for the now playing screen", name);
}

/*
 TEST BUILD (Eversolo): the A8 has no adb, so the player copies its own process's log (an app may read
 its own) to Download/squeezelite-eversolo.txt, which the probe app serves. That includes libaudiosink's
 own lines (tag AudioSink) and Android's audio client lines (AudioTrack, AAudio). Android 11 refused a
 plain fopen there: the Java side makes the file through MediaStore and hands its descriptor over
 (Library.setLogFd, before start). Restarted, truncated, at each player start.
 */
static int es_log_fd = -1;
static void es_log_start(void);

JNIEXPORT void JNICALL Java_org_lyrion_squeezelite_Library_setLogFd(JNIEnv *env, jobject jobj, jint fd) {
	es_log_fd = fd;
	es_log_start(); // now, so the app's lines from before the player starts are in the file too
}

// Who the A8 thinks has the audio, at an output open: its now playing source and the switch flag it
// polls (ledger §A3 EVERSOLO VOLUME IS THE DAC'S, EVERSOLO'S OWN ROUTE)
void es_log_audio_owner(const char *when) {
	char now[PROP_VALUE_MAX] = "", last[PROP_VALUE_MAX] = "", changed[PROP_VALUE_MAX] = "";
	__system_property_get("zidoo.music.package.now", now);
	__system_property_get("zidoo.music.package.last", last);
	__system_property_get("zidoo.music.value.changed", changed);
	LOG_INFO("Eversolo audio owner %s: now '%s', last '%s', changed %s", when, now, last, changed);
}

// On Android 11 (API 30) liblog lets a process install its own logger, which then receives every
// log line written in the process: ours, libaudiosink's, and Android's audio client's (AudioTrack,
// AAudio). It writes each to the file and passes it on to logd as before. The A8 gave an empty file
// with a logcat copy: an app's shell evidently may not run logcat there. Older Android: logcat copy.
struct es_log_message {      // liblog's struct __android_log_message (API 30)
	size_t struct_size;
	int32_t buffer_id;
	int32_t priority;
	const char *tag;
	const char *file;
	uint32_t line;
	const char *message;
};
typedef void (*es_logger_fn)(const struct es_log_message *message);
static es_logger_fn es_logd_logger = NULL;
static FILE *es_log_out = NULL;
static pthread_mutex_t es_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void es_log_line(int priority, const char *tag, const char *message) {
	struct timespec ts;
	struct tm tm;
	char when[24];
	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	strftime(when, sizeof(when), "%m-%d %H:%M:%S", &tm);
	pthread_mutex_lock(&es_log_lock);
	fprintf(es_log_out, "%s.%03ld %5d %5d %c %s: %s\n", when, ts.tv_nsec / 1000000, (int)getpid(), (int)gettid(),
			"??VDIWEF"[priority & 7], tag ? tag : "", message ? message : "");
	fflush(es_log_out);
	pthread_mutex_unlock(&es_log_lock);
}

static void es_logger(const struct es_log_message *m) {
	if (es_logd_logger) es_logd_logger(m);
	if (es_log_out && m) es_log_line(m->priority, m->tag, m->message);
}

/*
 Losing the DAC (ledger §A3 EVERSOLO TAKEOVER, EVERSOLO HOLDS NO DAC). When one of the A8's own sources
 starts, the A8 gives it the DAC and kills our track ("dead IAudioTrack, Offloaded or Direct", then "Error
 -32 obtaining an audio buffer, giving up"). Our output never noticed: it went on writing into nothing, LMS
 showed it playing, and later tracks reused the dead stream, which the A8 never registered again, so it
 never switched back to us. The A8 closes any other app's player the moment its own source registers;
 ours is not closed (with the helper, the helper is), so it lets go of the DAC itself, as fast. A watcher
 reads, every ES_WATCH_MS (the rate the A8 checks at), which app the A8 says has the audio; when that
 changes from us to another, the output goes off as at an idle timeout (the stream closes and a monitor
 waits) and LMS is asked to pause. The A8 switches its source (its play type) when the user picks one,
 which can be long before its player registers: its own player, on a Qobuz track, first loads it, and it
 sat at play type 5 with our output still registered while it never got going (2026-10-23 18:16). So while
 our output is registered the watcher also reads the play type, from the A8's play-info API: once the A8
 has shown us as its source (an outside app, ES_TYPE_OUTSIDE_APP), any other type is one of its own
 sources, and the output goes off the same way. Pressing play then opens a fresh stream, which the A8
 registers, switching to us as it does for any other app. A pause from LMS turns the output off at once
 (slimproto.c), a stop after ES_IDLE_MS.
 */
#define ES_WATCH_MS 20          // EverSoloOnlineInfoManager checks for a source change this often
#define ES_TYPE_MS 300          // the play type, while our output is registered
#define ES_TYPE_OUTSIDE_APP 8   // the A8's play type for an outside app's audio (ours); 5 is its own player
#define ES_MODE_MS 1000         // the A8's DSD output setting (a property: no request)
static char es_me[PROP_VALUE_MAX] = "";   // the name the A8 registers us by: our process name
static bool es_registered = false;        // the A8 has registered our output since it opened (lock O)
static bool es_lost_by_writer = false;    // the DoP writer saw the DAC go (lock O), see es_output_lost
static bool es_lost_was_playing = false;
static volatile bool es_watching = false;
static pthread_t es_watch_thread;

// The play type the A8 shows, from the getState its remote app polls (a status line, no headers, the
// JSON, then it closes), or -1 when it does not answer. Only the watcher calls it
static int es_play_type(void) {
	static const char req[] = "GET /ZidooMusicControl/v2/getState HTTP/1.0\r\nHost: 127.0.0.1:9529\r\n\r\n";
	static char buf[16384]; // the reply is about 3 KB
	struct sockaddr_in addr;
	struct timeval tv = { 0, 500000 };
	size_t got = 0;
	ssize_t n;
	char *p = NULL;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(9529);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (!connect(fd, (struct sockaddr *)&addr, sizeof(addr)) && send(fd, req, sizeof(req) - 1, 0) == (ssize_t)(sizeof(req) - 1)) {
		while (got < sizeof(buf) - 1 && (n = recv(fd, buf + got, sizeof(buf) - 1 - got, 0)) > 0) {
			got += n;
			buf[got] = '\0';
			// the first is the top level's (everSoloPlayInfo repeats it); done once its number has ended
			if ((p = strstr(buf, "\"playType\":")) && strpbrk(p + 11, ",}")) break;
			p = NULL;
		}
	}
	close(fd);
	return p && isdigit((unsigned char)p[11]) ? atoi(p + 11) : -1;
}

static void es_read_me(void) {
	FILE *f = fopen("/proc/self/cmdline", "r");
	size_t n = 0;
	if (f) {
		n = fread(es_me, 1, sizeof(es_me) - 1, f);
		fclose(f);
	}
	es_me[n] = '\0'; // the first argument: the name, up to its NUL
}

// called with the output lock held: whether the A8 says another app has the audio now
bool es_dac_taken(void) {
	char now[PROP_VALUE_MAX] = "";
	__system_property_get("zidoo.music.package.now", now);
	return es_me[0] && now[0] && strcmp(now, es_me) != 0;
}

// called with the output lock held, by the DoP writer when the sink failed because the DAC went: the
// output goes off now, so its reopen does not take the DAC straight back; the watcher pauses LMS
void es_output_lost(void) {
	es_lost_was_playing = output.state != OUTPUT_STOPPED && output.state != OUTPUT_OFF;
	output.state = OUTPUT_OFF;
	es_registered = false;
	es_lost_by_writer = true;
}

// one of Library's Eversolo hooks (no arguments), from the watcher's thread
static void es_tell_app(const char *name) {
	JNIEnv *env;
	bool detached;
	jmethodID method;
	if (!jvm || !obj || !clazz) return;
	detached = JNI_EDETACHED == (*jvm)->GetEnv(jvm, &env, JNI_VERSION_1_6);
	if (detached && JNI_OK != (*jvm)->AttachCurrentThread(jvm, &env, NULL)) {
		LOG_ERROR("Failed to get attach current thread");
		return;
	}
	method = (*env)->GetMethodID(env, clazz, name, "()V");
	if (method) {
		(*env)->CallVoidMethod(env, obj, method);
	} else {
		(*env)->ExceptionClear(env);
		LOG_ERROR("%s not found", name);
	}
	if (detached) {
		(*jvm)->DetachCurrentThread(jvm);
	}
}

static void es_pause_lms(void) {
	es_tell_app("eversoloOutputLost");
}

// The A8's DSD output setting changed while the player runs. Between Native and DoP the next DSD track
// follows it (dsd.c reads output.dsdfmt at each track start). To or from PCM it waits for the player to
// restart: whether the player offers LMS DSD is told when it connects
static void es_follow_dsd_mode(int was, int mode) {
	if (es_dsd_mode_at_start == ES_DSD_PCM || mode == ES_DSD_PCM) {
		LOG_INFO("Eversolo: its DSD output is now set to %s (was %s): that takes effect when the player restarts",
				 es_dsd_mode_name(mode), es_dsd_mode_name(was));
	} else {
		bool native = mode == ES_DSD_NATIVE && es_native_ok();
		dsd_format fmt;
		mutex_lock(outputbuf->mutex);
		fmt = _es_dsd_format(native);
		mutex_unlock(outputbuf->mutex);
		LOG_INFO("Eversolo: its DSD output is now set to %s: the next DSD track plays as %s", es_dsd_mode_name(mode),
				 fmt == DSD_U32_BE ? "native DSD" : fmt == DOP ? "DoP" : "PCM (the sink failed earlier)");
	}
}

static void *es_watch(void *arg) {
	char now[PROP_VALUE_MAX];
	u32_t active_since = 0, type_read_at = 0, mode_read_at = gettime_ms();
	bool told_unregistered = false, told_no_type = false;
	bool shown_ours = false; // the A8 has shown us as its source since our output opened
	int mode = es_dsd_mode_at_start;
	es_read_me(); // after any rename (es_name_process)
	LOG_INFO("Eversolo: watching which app has the DAC, ours is '%s'", es_me);
	while (es_watching) {
		bool lost = false, was_playing = false, registered, newly_shown = false;
		int type = -1, switched_to = -1;
		if (gettime_ms() - mode_read_at >= ES_MODE_MS) {
			int m = es_dsd_mode();
			mode_read_at = gettime_ms();
			if (m != mode) {
				es_follow_dsd_mode(mode, m);
				mode = m;
			}
		}
		mutex_lock(outputbuf->mutex);
		registered = output.state != OUTPUT_OFF && es_registered;
		mutex_unlock(outputbuf->mutex);
		// over HTTP, so outside the lock
		if (registered && gettime_ms() - type_read_at >= ES_TYPE_MS) {
			type_read_at = gettime_ms();
			type = es_play_type();
			if (type < 0 && !told_no_type) {
				told_no_type = true;
				LOG_INFO("Eversolo: its play type is not readable (getState on 127.0.0.1:9529)");
			}
		}
		now[0] = '\0';
		__system_property_get("zidoo.music.package.now", now);
		mutex_lock(outputbuf->mutex);
		if (output.state == OUTPUT_OFF) {
			es_registered = false;
			active_since = 0;
			told_unregistered = false;
			told_no_type = false;
			shown_ours = false;
		} else {
			if (!active_since) active_since = gettime_ms();
			if (!strcmp(now, es_me)) {
				es_registered = true;
				if (type == ES_TYPE_OUTSIDE_APP && !shown_ours) {
					shown_ours = newly_shown = true;
				} else if (shown_ours && type >= 0 && type != ES_TYPE_OUTSIDE_APP) {
					switched_to = type;
				}
			}
			if (switched_to >= 0 || (es_registered && strcmp(now, es_me))) {
				was_playing = output.state != OUTPUT_STOPPED;
				output.state = OUTPUT_OFF;
				output.pa_reopen = true;
				es_registered = false;
				lost = true;
			} else if (!es_registered && !told_unregistered && gettime_ms() - active_since > 3000) {
				told_unregistered = true;
				LOG_INFO("Eversolo: our output is not registered by the A8 after 3 s ('%s' has the DAC)", now);
			}
		}
		if (es_lost_by_writer) {
			was_playing = es_lost_was_playing;
			es_lost_by_writer = false;
			lost = true;
		}
		mutex_unlock(outputbuf->mutex);
		if (newly_shown) {
			LOG_INFO("Eversolo: the A8 shows us as its source (play type %d)", type);
			es_tell_app("eversoloShowsUs"); // its volume becomes LMS's (EversoloVolume)
		}
		if (lost) {
			if (switched_to >= 0) {
				LOG_INFO("Eversolo: the A8 switched to one of its own sources (play type %d): output off%s",
						 switched_to, was_playing ? ", LMS asked to pause" : "");
			} else {
				LOG_INFO("Eversolo: '%s' took the DAC: output off%s", now, was_playing ? ", LMS asked to pause" : "");
			}
			wake_controller();
			if (was_playing) es_pause_lms();
		}
		usleep(ES_WATCH_MS * 1000);
	}
	return NULL;
}

static void es_watch_start(void) {
	es_watching = true;
	if (pthread_create(&es_watch_thread, NULL, es_watch, NULL)) {
		es_watching = false;
	}
}

static void es_watch_stop(void) {
	if (es_watching) {
		es_watching = false;
		pthread_join(es_watch_thread, NULL);
	}
}

static void *es_log_copy(void *arg) {
	char cmd[80], line[1024];
	FILE *in;
	snprintf(cmd, sizeof(cmd), "logcat -v threadtime --pid=%d 2>&1", (int)getpid());
	in = popen(cmd, "r");
	if (!in) {
		es_log_line(ANDROID_LOG_ERROR, "squeezelite", "cannot run logcat");
		return NULL;
	}
	while (fgets(line, sizeof(line), in)) {
		pthread_mutex_lock(&es_log_lock);
		fputs(line, es_log_out);
		fflush(es_log_out);
		pthread_mutex_unlock(&es_log_lock);
	}
	es_log_line(ANDROID_LOG_ERROR, "squeezelite", "logcat ended");
	pclose(in);
	return NULL;
}

static void es_log_start(void) {
	static bool started = false; // once per process: the logger stays for its life
	void (*set_logger)(es_logger_fn) = NULL;
	void *liblog;
	char header[96];
	pthread_t t;
	if (started) return;
	started = true;
	es_log_out = es_log_fd >= 0 ? fdopen(es_log_fd, "w") : NULL;
	if (!es_log_out) {
		LOG_ERROR("Eversolo log: no log file (descriptor %d): %s", es_log_fd, strerror(errno));
		return;
	}
	snprintf(header, sizeof(header), "log started, pid %d, Android API %d", (int)getpid(), android_get_device_api_level());
	es_log_line(ANDROID_LOG_INFO, "squeezelite", header);
	if ((liblog = dlopen("liblog.so", RTLD_NOW))) {
		set_logger = (void (*)(es_logger_fn))dlsym(liblog, "__android_log_set_logger");
		es_logd_logger = (es_logger_fn)dlsym(liblog, "__android_log_logd_logger");
	}
	if (set_logger && es_logd_logger) {
		set_logger(es_logger);
		es_log_line(ANDROID_LOG_INFO, "squeezelite", "logging through liblog's logger");
	} else if (pthread_create(&t, NULL, es_log_copy, NULL) == 0) {
		es_log_line(ANDROID_LOG_INFO, "squeezelite", "logging through a logcat copy");
		pthread_detach(t);
	}
}
#endif

void send_output_state_to_app(u8_t spdif, u8_t dac) {
    if (!jvm || !obj || !clazz) {
		return;
	}
	JNIEnv *env;
	bool detached = JNI_EDETACHED == (*jvm)->GetEnv(jvm, &env, JNI_VERSION_1_6);
	if (detached) {
		if (JNI_OK!=(*jvm)->AttachCurrentThread(jvm, &env, NULL)) {
			LOG_ERROR("Failed to get attach current thread");
			return;
		}
	}
	jmethodID method = (*env)->GetMethodID(env, clazz, "outputChanged", "(II)V");
	if (method) {
		(*env)->CallVoidMethod(env, obj, method, spdif, dac);
	}
	if (detached) {
		(*jvm)->DetachCurrentThread(jvm);
	}
}

void send_volume_to_app(u32_t left, u32_t right) {
    if (!jvm || !obj || !clazz) {
		return;
	}
	JNIEnv *env;
	bool detached = JNI_EDETACHED == (*jvm)->GetEnv(jvm, &env, JNI_VERSION_1_6);
	if (detached) {
		if (JNI_OK!=(*jvm)->AttachCurrentThread(jvm, &env, NULL)) {
			LOG_ERROR("Failed to get attach current thread");
			return;
		}
	}
	jmethodID method = (*env)->GetMethodID(env, clazz, "volumeChanged", "(II)V");
	if (method) {
		(*env)->CallVoidMethod(env, obj, method, left, right);
	}
	if (detached) {
		(*jvm)->DetachCurrentThread(jvm);
	}
}

void send_playback_state_to_app(void) {
	if (!jvm || !obj || !clazz) {
		return;
	}
	// Callers do not hold the output lock
	mutex_lock(outputbuf->mutex);
	bool playing = output.state != OUTPUT_OFF && output.state != OUTPUT_STOPPED;
	mutex_unlock(outputbuf->mutex);

	JNIEnv *env;
	bool detached = JNI_EDETACHED == (*jvm)->GetEnv(jvm, &env, JNI_VERSION_1_6);
	if (detached) {
		if (JNI_OK!=(*jvm)->AttachCurrentThread(jvm, &env, NULL)) {
			LOG_ERROR("Failed to get attach current thread");
			return;
		}
	}
	jmethodID method = (*env)->GetMethodID(env, clazz, "playbackStateChanged", "(Z)V");
	if (method) {
		(*env)->CallVoidMethod(env, obj, method, (jboolean)(playing ? JNI_TRUE : JNI_FALSE));
	} else {
		// A failed GetMethodID leaves an exception pending, and the next JNI call from this
		// thread would then abort the VM. Happens if the native library is newer than the java.
		(*env)->ExceptionClear(env);
		LOG_ERROR("playbackStateChanged not found");
	}
	if (detached) {
		(*jvm)->DetachCurrentThread(jvm);
	}
}

// What the output now sends the DAC: native DSD at the DSD bit rate, or PCM at the stream rate
void send_output_format_to_app(bool dsd, unsigned rate) {
	if (!jvm || !obj || !clazz) {
		return;
	}
	JNIEnv *env;
	bool detached = JNI_EDETACHED == (*jvm)->GetEnv(jvm, &env, JNI_VERSION_1_6);
	if (detached) {
		if (JNI_OK!=(*jvm)->AttachCurrentThread(jvm, &env, NULL)) {
			LOG_ERROR("Failed to get attach current thread");
			return;
		}
	}
	jmethodID method = (*env)->GetMethodID(env, clazz, "outputFormatChanged", "(ZI)V");
	if (method) {
		(*env)->CallVoidMethod(env, obj, method, (jboolean)(dsd ? JNI_TRUE : JNI_FALSE), (jint)rate);
	} else {
		(*env)->ExceptionClear(env);
		LOG_ERROR("outputFormatChanged not found");
	}
	if (detached) {
		(*jvm)->DetachCurrentThread(jvm);
	}
}

void send_connection_state_to_app(const char *address) {
	if (!jvm || !obj || !clazz) {
		return;
	}
	JNIEnv *env;
	bool detached = JNI_EDETACHED == (*jvm)->GetEnv(jvm, &env, JNI_VERSION_1_6);
	if (detached) {
		if (JNI_OK!=(*jvm)->AttachCurrentThread(jvm, &env, NULL)) {
			LOG_ERROR("Failed to get attach current thread");
			return;
		}
	}
	jmethodID method = (*env)->GetMethodID(env, clazz, "connectionStateChanged", "(Ljava/lang/String;)V");
	if (method) {
		jstring jStr = (*env)->NewStringUTF(env, address);
		(*env)->CallVoidMethod(env, obj, method, jStr);
		(*env)->DeleteLocalRef(env, jStr);
	}
	if (detached) {
		(*jvm)->DetachCurrentThread(jvm);
	}
}

JNIEXPORT void JNICALL Java_org_lyrion_squeezelite_Library_start(JNIEnv *env, jobject jobj, jstring lms_param, jstring mac_param, jstring name_param, jint idle, jint fixed_vol, jint loglevel, jint mobile_network, jint buffer_size, jint force_opensles, jstring eversolo_name_param) {
	const char *server = (*env)->GetStringUTFChars(env, lms_param, NULL);
	const char *mac_str = (*env)->GetStringUTFChars(env, mac_param, NULL);
	const char *eversolo_name = (*env)->GetStringUTFChars(env, eversolo_name_param, NULL);
	char *output_device = "default";
	char *include_codecs = NULL;
	char *exclude_codecs = "";
	const char *name = (*env)->GetStringUTFChars(env, name_param, NULL);
	char *namefile = NULL;
	const char *modelname = "SqueezeLiteAndroid";
	u8_t mac[6];
	unsigned stream_buf_size = buffer_size>0 ? buffer_size*1024*1024 : STREAMBUF_SIZE;
	unsigned output_buf_size = 0; // set later
	unsigned rates[MAX_SUPPORTED_SAMPLERATES] = { 0 };
	unsigned rate_delay = 0;
	char *resample = NULL;
	unsigned max_rate = 0;
	char *output_params = NULL;
#if DSD
	unsigned dsd_delay = 0;
	dsd_format dsd_outfmt = PCM;
#endif
	log_level log_output = loglevel;
	log_level log_stream = loglevel;
	log_level log_decode = loglevel;
	log_level log_slimproto = loglevel;

	if (!force_opensles && LibAAudio_init()) {
		PaOpenSLES_ENABLED = 0;
		PaAAudio_ENABLED = 1;
		LOG_DEBUG("Using AAudio");
	} else {
		PaOpenSLES_ENABLED = 1;
		PaAAudio_ENABLED = 0;
		LOG_DEBUG("Using OpenSL ES");
	}

	if (mac_str) {
		int byte = 0;
		char *tmp;
		char *t = strtok(mac_str, ":");
		while (t && byte < 6) {
			mac[byte++] = (u8_t)strtoul(t, &tmp, 16);
			t = strtok(NULL, ":");
		}
	}

	init_jvm(env, jobj);
#if DSD
	// FiiO players take native DSD through an AudioTrack, the HiBy R5 through HiBy's own route, and
	// Eversolo players (the DMP-A8) native DSD or DoP through Eversolo's own sink, as the A8 is set.
	// Elsewhere the player does not offer DSD (no dsf/dff in what it tells LMS), so LMS converts it to
	// PCM before sending it (the DSDPlayer plugin; without it LMS cannot play DSD here), at no more than
	// the MaxSampleRate the player reports. Converted here it would run at the DSD rate / 8 with no
	// resampler to lower it (that is the FiiO's only): DSD128 at 705.6k, which OpenSL ES refused on a
	// HiBy R5 (Android 8.1)
	set_dsd_loglevel(loglevel);
	dsd_encoding = find_dsd_encoding(env);
	// Android 8.1 (the HiBy R5): PCM from 48k up plays through a Java AudioTrack (output_pa.c
	// pcm_writer), unless the user forces OpenSL ES. Where HiBy's own route is there (the R5), PCM from
	// 32k to 384k and native DSD go through that instead, and the player offers rates up to 384k
	pcm_track_on = !force_opensles && android_get_device_api_level() == __ANDROID_API_O_MR1__;
	sas_on = pcm_track_on && !dsd_encoding && sas_init();
	// Eversolo players (the DMP-A8): DSD through Eversolo's own sink. PCM stays as it was
	es_on = !dsd_encoding && !sas_on && es_init();
	if (dsd_encoding) {
		// fiio_rates, resampling only a rate the route does not take (to one it does)
		static char fiio_resample[] = "hE"; // process_init splits it in place
		dsd_outfmt = DSD_U16_LE;
		LOG_INFO("native DSD available (encoding %d)", dsd_encoding);
		memcpy(rates, fiio_rates, sizeof(fiio_rates));
		user_rates = true;
		resample = fiio_resample;
		max_rate = fiio_rates[0]; // what LMS is told, whatever DAC the route goes to when it connects
	} else if (sas_on) {
		// DSD as 32-bit words at the DSD rate / 32, the rate HiBy's DSD track runs at, up to DSD256
		// (352.8k, under the 384k the player offers); higher DSD is converted here to PCM
		dsd_outfmt = DSD_U32_LE;
		memcpy(rates, hiby_rates, sizeof(hiby_rates));
		user_rates = true;
		max_rate = hiby_rates[0];
		LOG_INFO("HiBy audio service: PCM up to %u and native DSD through it", hiby_rates[0]);
	} else if (es_on) {
		// DSD as the A8's DSD output setting says, as Eversolo's squeezelite follows it. Native DSD
		// frames run at the DSD rate / 32: up to DSD512 (705.6k, the top of the DAC's AUDIO_FORMAT_DSD
		// rates); DoP frames at the DSD rate / 16: up to DSD256 (705.6k, under the DAC's 768k). Higher
		// DSD is converted here to PCM (output.dsd_max_rate, below). On PCM the player does not offer
		// DSD, so LMS converts it. es_watch follows a later change of the setting
		int mode = es_dsd_mode();
		bool native = mode == ES_DSD_NATIVE && es_native_ok();
		es_dsd_mode_at_start = mode;
		dsd_outfmt = native ? DSD_U32_BE : DOP;
		es_log_start();
		if (eversolo_name && eversolo_name[0]) {
			// the user's choice (app setting): the Eversolo's own now playing screen
			es_name_process(eversolo_name);
		}
		if (mode == ES_DSD_PCM) {
			exclude_codecs = "dsd";
			LOG_INFO("Eversolo audio sink: the A8's DSD output is set to PCM: DSD is not offered to LMS, which converts it to PCM");
		} else if (native) {
			LOG_INFO("Eversolo audio sink: native DSD through it, up to DSD512 (the A8's DSD output is set to Native)");
		} else {
			LOG_INFO("Eversolo audio sink: DSD as DoP through it, up to DSD256 (the A8's DSD output is set to %s%s)",
					 es_dsd_mode_name(mode), mode == ES_DSD_NATIVE ? ", but its output does not take native DSD" : "");
		}
	} else {
		exclude_codecs = "dsd";
		LOG_INFO("no native DSD: DSD is not offered to LMS, which converts it to PCM");
	}
#endif
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = segv_handler;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGABRT, &sa, NULL);
	}

#if USE_SSL && !LINKALL && !NO_SSLSYM
	ssl_loaded = load_ssl_symbols();
#endif

	// set the output buffer size if not specified on the command line, take account of resampling
	// and of rates above 192k
	if (!output_buf_size) {
		output_buf_size = OUTPUTBUF_SIZE;
		if (resample || rates[0] > 192000) {
			unsigned scale = 8;
			if (rates[0]) {
				scale = rates[0] / 44100;
				if (scale > 8) scale = 8;
				if (scale < 1) scale = 1;
			}
			output_buf_size *= scale;
		}
	}

	stream_init(log_stream, stream_buf_size);

#if DSD
	if (es_on) {
		// after the buffer size, which stays as it was when the probe gave these rates
		memcpy(rates, es_rates, sizeof(es_rates));
		user_rates = true;
		idle = ES_IDLE_MS;
	}
#endif
	output_init_pa(log_output, output_device, output_buf_size, output_params, rates, rate_delay, (unsigned int)idle, 0!=fixed_vol);

#if DSD
	dsd_init(dsd_outfmt, dsd_delay);
	if (dsd_encoding) {
		mutex_lock(outputbuf->mutex);
		output.dsd_max_rate = 705600; // DSD256: DSD_U16_LE frames run at the DSD rate / 16
		mutex_unlock(outputbuf->mutex);
	} else if (es_on) {
		mutex_lock(outputbuf->mutex);
		output.dsd_max_rate = 705600; // DoP to DSD256 (the DSD rate / 16), native DSD to DSD512 (/ 32)
		mutex_unlock(outputbuf->mutex);
		es_watch_start(); // the output exists now: see es_watch
	}
	dsd_route_live(true);
#endif

	decode_init(log_decode, include_codecs, exclude_codecs);

#if RESAMPLE
	if (resample) {
		process_init(resample);
	}
#endif

	slimproto(log_slimproto, !server || strlen(server)==0 ? NULL : server, mac, name, namefile, modelname, max_rate, mobile_network);

	decode_close();
	stream_close();

#if DSD
	dsd_route_live(false);
	es_watch_stop(); // before the output it reads goes
#endif
	output_close_pa();

#if USE_SSL && !LINKALL && !NO_SSLSYM
	free_ssl_symbols();
#endif

	(*env)->ReleaseStringUTFChars(env, lms_param, server);
	(*env)->ReleaseStringUTFChars(env, mac_param, mac_str);
	(*env)->ReleaseStringUTFChars(env, name_param, name);
	(*env)->ReleaseStringUTFChars(env, eversolo_name_param, eversolo_name);
}

JNIEXPORT void JNICALL Java_org_lyrion_squeezelite_Library_stop(JNIEnv * env, jobject jobj) {
	slimproto_stop();
}

// The id of the codec decoding the track playing, so of the format LMS sends it in ('f' flac,
// 'm' mp3, 'd' dsd, 'p' pcm, 'a' aac, 'o' ogg: the codecs this build has); 0 when not known (before
// the first track, and from a flush to the next track start). Read without the lock, as
// streamRate: one value each, set at each track start
JNIEXPORT jchar JNICALL Java_org_lyrion_squeezelite_Library_streamCodec(JNIEnv * env, jobject jobj) {
#if DSD
	return (jchar)(unsigned char)output.current_codec;
#else
	return 0;
#endif
}

// The rate of the stream LMS sends the track playing in, before the library resamples it (native
// DSD: its frame rate); 0 when not known. Read without the lock: at a track start this and
// streamCodec can come from different tracks for one read, and the status query that follows the
// track start (STMs) reads both again
JNIEXPORT jint JNICALL Java_org_lyrion_squeezelite_Library_streamRate(JNIEnv * env, jobject jobj) {
#if DSD
	return (jint)output.current_raw_rate;
#else
	return 0;
#endif
}

// For the Eversolo volume sync (EversoloVolume): whether the A8 has our audio as its own, its package.now
// being our name, which it stays after a pause until another app plays; false until the watcher has
// read our name
JNIEXPORT jboolean JNICALL Java_org_lyrion_squeezelite_Library_eversoloOurs(JNIEnv * env, jobject jobj) {
#if DSD
	char now[PROP_VALUE_MAX] = "";
	__system_property_get("zidoo.music.package.now", now);
	return es_me[0] && !strcmp(now, es_me) ? JNI_TRUE : JNI_FALSE;
#else
	return JNI_FALSE;
#endif
}

// And its mute flag, which its volume app sets at each mute and unmute (AudioMangerUtils.setDacIsMuteVolume)
JNIEXPORT jboolean JNICALL Java_org_lyrion_squeezelite_Library_eversoloMuted(JNIEnv * env, jobject jobj) {
#if DSD
	char mute[PROP_VALUE_MAX] = "";
	__system_property_get("persist.zidoo.dac.isMute", mute);
	return !strcmp(mute, "1") ? JNI_TRUE : JNI_FALSE;
#else
	return JNI_FALSE;
#endif
}

// dsd_ok is false while a USB audio device is attached: the FiiO routes to it, and it cannot play
// the DSD track. dac_rates are that device's rates, NULL or empty when not known (the player's own
// are kept); those above the player's own maximum are dropped. Returns whether Library should have
// LMS stream the track playing again: a track in the buffer, decoded for the old route, would not
// play right on the new one (output_route_pa). False when the route is not applied: the output
// module is down, or there is no native DSD (not a FiiO: the route changes nothing).
JNIEXPORT jboolean JNICALL Java_org_lyrion_squeezelite_Library_outputRoute(JNIEnv * env, jobject jobj, jboolean dsd_ok, jintArray dac_rates) {
	jboolean restart = JNI_FALSE;
#if DSD
	unsigned rates[MAX_SUPPORTED_SAMPLERATES] = { 0 };
	unsigned n = 0;
	jsize len = dac_rates ? (*env)->GetArrayLength(env, dac_rates) : 0;
	if (len > 0) {
		jint *r = (*env)->GetIntArrayElements(env, dac_rates, NULL);
		jsize i;
		unsigned j;
		// insert each, highest first, no repeats
		for (i = 0; r && i < len && n < MAX_SUPPORTED_SAMPLERATES - 1; i++) {
			unsigned rate = r[i] > 0 ? (unsigned)r[i] : 0;
			if (!rate || rate > fiio_rates[0]) continue;
			for (j = 0; j < n && rates[j] > rate; j++);
			if (j < n && rates[j] == rate) continue;
			memmove(rates + j + 1, rates + j, (n - j) * sizeof(rates[0]));
			rates[j] = rate;
			n++;
		}
		if (r) (*env)->ReleaseIntArrayElements(env, dac_rates, r, JNI_ABORT);
	}
	pthread_mutex_lock(&route_mutex);
	route_ok = dsd_ok;
	route_dac = n > 0;
	memcpy(route_rates, rates, sizeof(route_rates));
	if (route_live && output_route_pa(route_ok, route_dac ? route_rates : NULL)) restart = JNI_TRUE;
	pthread_mutex_unlock(&route_mutex);
#endif
	return restart;
}
