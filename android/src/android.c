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

extern struct outputstate output;
extern struct buffer *outputbuf;
extern bool user_rates;
extern bool pcm_track_on;
extern bool sas_on;

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

JNIEXPORT void JNICALL Java_org_lyrion_squeezelite_Library_start(JNIEnv *env, jobject jobj, jstring lms_param, jstring mac_param, jstring name_param, jint idle, jint fixed_vol, jint loglevel, jint mobile_network, jint buffer_size, jint force_opensles) {
	const char *server = (*env)->GetStringUTFChars(env, lms_param, NULL);
	const char *mac_str = (*env)->GetStringUTFChars(env, mac_param, NULL);
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
	// FiiO players take native DSD through an AudioTrack, and the HiBy R5 through HiBy's own route.
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

	output_init_pa(log_output, output_device, output_buf_size, output_params, rates, rate_delay, (unsigned int)idle, 0!=fixed_vol);

#if DSD
	dsd_init(dsd_outfmt, dsd_delay);
	if (dsd_encoding) {
		mutex_lock(outputbuf->mutex);
		output.dsd_max_rate = 705600; // DSD256: DSD_U16_LE frames run at the DSD rate / 16
		mutex_unlock(outputbuf->mutex);
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
#endif
	output_close_pa();

#if USE_SSL && !LINKALL && !NO_SSLSYM
	free_ssl_symbols();
#endif

	(*env)->ReleaseStringUTFChars(env, lms_param, server);
	(*env)->ReleaseStringUTFChars(env, mac_param, mac_str);
	(*env)->ReleaseStringUTFChars(env, name_param, name);
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
