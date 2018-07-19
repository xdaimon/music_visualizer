#pragma once

#include <iostream>
#include <chrono>
namespace chrono = std::chrono;
#include <cmath> // std::abs
#include <limits> // numeric_limits<T>::infinity()
#include <mutex>
#include <complex>
using std::complex;

#include "AudioStreams/AudioStream.h"
#include "ShaderConfig.h" // AudioOptions

#include "ffts.h"

static const int VISUALIZER_BUFSIZE = 1024;
struct AudioData {
    float* audio_l;
    float* audio_r;
    float* freq_l;
    float* freq_r;
    std::mutex mtx;
};

// TODO wrap fft stuff/complexity into a class
// FFT computation library needs aligned memory
// TODO std::aligned_alloc is in c++17 apparently
#ifdef WINDOWS
#define Aligned_Alloc(align, sz) _aligned_malloc(sz, align)
#define Aligned_Free(ptr) _aligned_free(ptr)
#else
#define Aligned_Alloc(align, sz) aligned_alloc(align, sz)
#define Aligned_Free(ptr) free(ptr)
#endif

// sample rate of the audio stream
static const int SR = 48000;
// sample rate of audio given to FFT
static const int SRF = SR / 2;
// length of the system audio capture buffer (in frames)
static const int ABL = 512;
// number of system audio capture buffers to keep
static const int ABN = 16;
// total audio buffer length. length of all ABN buffers combined (in frames)
static const int TBL = ABL * ABN;
static const int FFTLEN = TBL / 2;
// length of visualizer 1D texture buffers. Number of frames of audio this module outputs.
static const int VL = VISUALIZER_BUFSIZE;

// I think the FFT looks best when it has 8192/48000, or 4096/24000, time granularity. However, I
// like the wave to be 96000hz just because then it fits on the screen nice. To have both an FFT on
// 24000hz data and to display 96000hz waveform data, I ask pulseaudio to output 48000hz and then
// resample accordingly. I've compared the 4096 sample FFT over 24000hz data to the 8192 sample FFT
// over 48000hz data and they are surprisingly similar. I couldn't tell a difference really. I've
// also compared resampling with ffmpeg to my current naive impl. Hard to notice a difference.

// Cross correlation sync options
// store the 9 most recent buffers sent to the visualizer (stores contents of visualizer buffer).
static const int HISTORY_NUM_FRAMES = 7;

// search an interval of 500 samples centered around the current read index (r) for the maximum cross correlation.
static const int HISTORY_SEARCH_RANGE = 350;

// only compute the cross correlation for every third offset in the range 
// (so HISTORY_SEARCH_RANGE / HISTORY_SEARCH_GRANULARITY) number of cross correlations are computed for each frame.
static const int HISTORY_SEARCH_GRANULARITY = 3;

// take into consideration the whole visualizer buffer
static const int HISTORY_BUFF_SZ = VL;

// The performance cost for the cross correlations is
// HISTORY_SEARCH_RANGE * HISTORY_NUM_FRAMES * HISTORY_BUFF_SZ / HISTORY_SEARCH_GRANULARITY

// AudioProcess does not create AudioStream because ProceduralAudioStream must be created by AudioProcess's owner
template <typename ClockT, typename AudioStreamT>
class AudioProcess {
public:
    AudioProcess(AudioStreamT&, AudioOptions);
    ~AudioProcess();

    void step();
    void start() {
        while (!exit_audio_system_flag) {
            if (audio_system_paused) {
                step();
            }
            else {
                std::this_thread::sleep_for(chrono::milliseconds(500));
            }
        }
    }
    void exit_audio_system() {
        exit_audio_system_flag = true;
    }
    void pause_audio_system() {
        audio_system_paused = false;
    }
    void start_audio_system() {
        audio_system_paused = true;
    }
    AudioData& get_audio_data() {
        return audio_sink;
    }
    void set_audio_options(AudioOptions& ao) {
        xcorr_sync = ao.xcorr_sync;
        fft_sync = ao.fft_sync;
        wave_smoother = ao.wave_smooth;
        // TODO should i have this option?
        // ao.fft_smooth;
    }

private:
    bool audio_system_paused = true;
    bool exit_audio_system_flag = false;

    // Mixes the old buffer of audio with the new buffer of audio.
    float wave_smoother;

    // The audio pointer in the circular buffer should be moved such that it travels only distances
    // that are whole multiples, not fractional multiples, of the audio wave's wavelength.
    // But how do we find the wavelength of the audio wave? If we try and look for the zero's
    // of the audio wave and note the distance between them, then we will not get an accurate
    // or consistent measure of the wavelength because the noise in the waveform will likely throw
    // our measure off.
    //
    // The wavelength of a waveform is related to the frequency by this equation
    // wavelength = velocity/frequency
    //
    // velocity = sample rate
    // wavelength = number of samples of one period of the wave
    // frequency = dominant frequency of the waveform
    bool fft_sync;

    // Increases similarity between successive frames of audio output by the AudioProcess
    bool xcorr_sync;

    typename ClockT::time_point now_time;
    const std::chrono::nanoseconds _60fpsDura = dura(1.f / 60.f);
    typename ClockT::time_point next_time = now_time + _60fpsDura;
    int frame_id = 0;

    float* prev_buff_l[HISTORY_NUM_FRAMES];
    float* prev_buff_r[HISTORY_NUM_FRAMES];

    float* audio_buff_l;
    float* audio_buff_r;

    ffts_plan_t* fft_plan;
    complex<float>* fft_out_l;
    float* fft_in_l;
    complex<float>* fft_out_r;
    float* fft_in_r;
    float* fft_window;

    int writer;
    int reader_l;
    int reader_r;
    float freq_l;
    float freq_r;

    float channel_max_l;
    float channel_max_r;

    struct AudioData audio_sink;
    AudioStreamT& audio_stream;

    // Returns the bin holding the max frequency of the fft. We only consider the first 100 bins.
    static int max_bin(const complex<float>* f);
    static float max_frequency(const complex<float>* f);

    // returns a fraction of freq such that freq / c <= thres for c = some power of two
    static float get_harmonic_less_than(float freq, float thres);

    // Move p around the circular buffer by the amound and direction delta
    // tbl: total buffer length of the circular buffer
    static int move_index(int p, int delta, int tbl);
    static int dist_forward(int from, int to, int tbl);
    static int dist_backward(int from, int to, int tbl);

    static inline float mix(float x, float y, float m);

    // Adds the wavelength computed by sample_rate / frequency to r.
    // If w is within VL units in front of r, then adjust r so that this is no longer true.
    // w: index of the writer in the circular buffer
    // r: index of the reader in the circular buffer
    // freq: frequency to compute the wavelength with
    static int advance_index(int w, int r, float freq, int tbl);

    // Compute the manhattan distance between two vectors.
    // a_circular: a circular buffer containing the first vector
    // b_buff: a buffer containing the second vector
    // a_sz: circumference of a_circular
    // b_sz: length of b_buff
    static float
    reverse_dot_prod(const float* a_circular, const float* b_buff, int a_offset, int a_sz, int b_sz, float a_scale);

    // w: writer
    // r: reader
    // dist: half the amount to search by
    // buff: buffer of audio to compare prev_buffer with
    //
    // buffer sizes: Sure, I could make it all variable, but does that mean the function can work
    // with arbitrary inputs? No, the inputs depend on each other too much. Writing the
    // tests/asserts in anticipation that the inputs would vary is too much work, when I know that
    // the inputs actually will not vary.
    static int cross_correlation_sync(int w,
                                      int r,
                                      int dist,
                                      float* prev_buff[HISTORY_NUM_FRAMES],
                                      int frame_id,
                                      const float* buff,
                                      float channel_max);

    // converts number of seconds x to a time duration for the ClockT type
    static typename ClockT::duration dura(float x);
};

template <typename ClockT, typename AudioStreamT>
AudioProcess<ClockT, AudioStreamT>::AudioProcess(AudioStreamT& _audio_stream, AudioOptions audio_options)
    : audio_stream(_audio_stream), audio_sink() {
    if (audio_stream.get_sample_rate() != 48000) {
        std::cout << "The AudioProcess is meant to consume 48000hz audio but the given AudioStream "
             << "produces " << audio_stream.get_sample_rate() << "hz audio." << std::endl;
        exit(1);
    }
    if (audio_stream.get_max_buff_size() < ABL) {
        std::cout << "AudioProcess needs at least " << ABL << " frames per call to get_next_pcm"
             << " but the given AudioStream only provides " << audio_stream.get_max_buff_size()
             << "." << std::endl;
        exit(1);
    }

    xcorr_sync = audio_options.xcorr_sync;
    fft_sync = audio_options.fft_sync;
    wave_smoother = audio_options.wave_smooth;
    // ao.fft_smooth;

    // new float[x]() zero initializes
    audio_buff_l = new float[TBL]();
    audio_buff_r = new float[TBL]();

    for (int i = 0; i < HISTORY_NUM_FRAMES; ++i) {
        prev_buff_l[i] = new float[HISTORY_BUFF_SZ]();
        prev_buff_r[i] = new float[HISTORY_BUFF_SZ]();
    }

    audio_sink.audio_l = new float[VL]();
    audio_sink.audio_r = new float[VL]();
    audio_sink.freq_l = new float[VL]();
    audio_sink.freq_r = new float[VL]();

    const int N = FFTLEN;
    fft_plan = ffts_init_1d_real(N, FFTS_FORWARD);
    fft_in_l = (float*)Aligned_Alloc(32, N * sizeof(float));
    fft_in_r = (float*)Aligned_Alloc(32, N * sizeof(float));
    fft_out_l = (complex<float>*)Aligned_Alloc(32, (N / 2 + 1) * sizeof(complex<float>));
    fft_out_r = (complex<float>*)Aligned_Alloc(32, (N / 2 + 1) * sizeof(complex<float>));
    fft_window = new float[N];
    for (int i = 0; i < N; ++i)
        fft_window[i] = (1.f - cosf(2.f * 3.141592f * i / float(N))) / 2.f; // sin(x)^2

    // Holds a harmonic frequency of the dominant frequency of the audio.
    freq_l = 60.f;
    freq_r = 60.f;

    // The index of the writer in the audio buffers
    writer = 0;
    reader_l = 0;
    reader_r = 0;

    channel_max_l = 1.f;
    channel_max_r = 1.f;
}

template <typename ClockT, typename AudioStreamT>
inline AudioProcess<ClockT, AudioStreamT>::~AudioProcess() {
    delete[] audio_sink.audio_l;
    delete[] audio_sink.audio_r;
    delete[] audio_sink.freq_l;
    delete[] audio_sink.freq_r;
    delete[] audio_buff_l;
    delete[] audio_buff_r;

    Aligned_Free(fft_in_l);
    Aligned_Free(fft_in_r);
    Aligned_Free(fft_out_l);
    Aligned_Free(fft_out_r);
    delete[] fft_window;

    for (int i = 0; i < HISTORY_NUM_FRAMES; ++i) {
        delete[] prev_buff_l[i];
        delete[] prev_buff_r[i];
    }

    ffts_free(fft_plan);
}

template <typename ClockT, typename AudioStreamT>
inline void AudioProcess<ClockT, AudioStreamT>::step() {
    audio_stream.get_next_pcm(audio_buff_l + writer, audio_buff_r + writer, ABL);
    writer = move_index(writer, ABL, TBL);

    now_time = ClockT::now();
    if (now_time - next_time > chrono::milliseconds(60)) {
        next_time = now_time - chrono::milliseconds(1);
    }

    if (now_time > next_time) {
        // Get next read location in audio buffer
        reader_l = advance_index(writer, reader_l, freq_l, TBL);
        reader_r = advance_index(writer, reader_r, freq_r, TBL);
        if (xcorr_sync) {
            reader_l = cross_correlation_sync(writer, reader_l, HISTORY_SEARCH_RANGE, prev_buff_l, frame_id, audio_buff_l, channel_max_l);
            reader_r = cross_correlation_sync(writer, reader_r, HISTORY_SEARCH_RANGE, prev_buff_r, frame_id, audio_buff_r, channel_max_r);
        }

        // Downsample and window the audio
        for (int i = 0; i < FFTLEN; ++i) {
            #define downsmpl(s) s[(i * 2 + writer) % TBL]
            fft_in_l[i] = downsmpl(audio_buff_l) / channel_max_l * fft_window[i];
            fft_in_r[i] = downsmpl(audio_buff_r) / channel_max_r * fft_window[i];
            #undef downsmpl
        }
        // TODO fft magnitudes are different between windows and linux
        ffts_execute(fft_plan, fft_in_l, fft_out_l);
        ffts_execute(fft_plan, fft_in_r, fft_out_r);
        fft_out_l[0] = 0;
        fft_out_r[0] = 0;

        float max_amplitude_l = -std::numeric_limits<float>::infinity();
        float max_amplitude_r = -std::numeric_limits<float>::infinity();

        audio_sink.mtx.lock();
        for (int i = 0; i < VL; ++i) {
            float sample_l = audio_buff_l[(i + reader_l) % TBL];
            float sample_r = audio_buff_r[(i + reader_r) % TBL];

            if (std::abs(sample_l) > max_amplitude_l)
                max_amplitude_l = std::abs(sample_l);
            if (std::abs(sample_r) > max_amplitude_r)
                max_amplitude_r = std::abs(sample_r);

            sample_l = .66f * sample_l / (channel_max_l + 0.0001f);
            sample_r = .66f * sample_r / (channel_max_r + 0.0001f);

            audio_sink.audio_l[i] = mix(audio_sink.audio_l[i], sample_l, wave_smoother);
            audio_sink.audio_r[i] = mix(audio_sink.audio_r[i], sample_r, wave_smoother);

            audio_sink.freq_l[i] = std::abs(fft_out_l[i]) / std::sqrt(float(FFTLEN));
            audio_sink.freq_r[i] = std::abs(fft_out_r[i]) / std::sqrt(float(FFTLEN));
        }
        audio_sink.mtx.unlock();

        if (xcorr_sync) {
            for (int i = 0; i < VL; ++i) {
                prev_buff_l[frame_id % HISTORY_NUM_FRAMES][i] = audio_sink.audio_l[i];
                prev_buff_r[frame_id % HISTORY_NUM_FRAMES][i] = audio_sink.audio_r[i];
            }
        }

        // Rescale with a delay so the rescaling is less obvious and unnatural looking
        channel_max_l = mix(channel_max_l, max_amplitude_l, .5f);
        channel_max_r = mix(channel_max_r, max_amplitude_r, .5f);

        if (fft_sync) {
            freq_l = get_harmonic_less_than(max_frequency(fft_out_l), 80.f);
            freq_r = get_harmonic_less_than(max_frequency(fft_out_r), 80.f);
        }
        else {
            freq_l = 60.f;
            freq_r = 60.f;
        }

        frame_id++;
        next_time += _60fpsDura;
    }
}

template <typename ClockT, typename AudioStreamT>
inline int AudioProcess<ClockT, AudioStreamT>::max_bin(const complex<float>* f) {
    float max = 0.f;
    int max_i = 0;
    // catch frequencies from 5.86 to 586 (that is i * SRF / FFTLEN for i from 1 to 100)
    for (int i = 1; i < 100; ++i) {
        const float mmag = std::abs(f[i]);
        if (mmag > max) {
            max = mmag;
            max_i = i;
        }
    }
    return max_i;
}

template <typename ClockT, typename AudioStreamT>
inline int AudioProcess<ClockT, AudioStreamT>::move_index(int p, int delta, int tbl) {
    p += delta;
    if (p >= tbl) {
        p -= tbl;
    }
    else if (p < 0) {
        p += tbl;
    }
    return p;
}

template <typename ClockT, typename AudioStreamT>
inline int AudioProcess<ClockT, AudioStreamT>::dist_forward(int from, int to, int tbl) {
    int d = to - from;
    if (d < 0)
        d += tbl;
    return d;
}

template <typename ClockT, typename AudioStreamT>
inline int AudioProcess<ClockT, AudioStreamT>::dist_backward(int from, int to, int tbl) {
    return dist_forward(to, from, tbl);
}

template <typename ClockT, typename AudioStreamT>
inline float AudioProcess<ClockT, AudioStreamT>::max_frequency(const complex<float>* f) {
    // more info -> http://dspguru.com/dsp/howtos/how-to-interpolate-fft-peak
    // https://ccrma.stanford.edu/~jos/sasp/Quadratic_Interpolation_Spectral_Peaks.html
    int k = max_bin(f);
    if (k == 0) {
        k = 1;
    }
    const float a = std::abs(f[k - 1]); // std:abs(complex) gives magnitudes
    const float b = std::abs(f[k]);
    const float g = std::abs(f[k + 1]);
    const float d = .5f * (a - g) / (a - 2 * b + g + 0.001f);
    const float kp = k + d;
    // dont let anything negative or close to zero through
    return std::max(kp * float(SRF) / float(FFTLEN), 10.f);
}

template <typename ClockT, typename AudioStreamT>
inline float AudioProcess<ClockT, AudioStreamT>::get_harmonic_less_than(float freq, float thres) {
    // while (freq > 121.f)
    //     freq /= 2.f;
    const float a = std::log2f(freq);
    const float b = std::log2f(thres);
    freq *= std::pow(2.f, std::floor(b - a));
    if (!std::isnormal(freq))
        freq = 60.f;
    return freq;
}

template <typename ClockT, typename AudioStreamT>
inline float AudioProcess<ClockT, AudioStreamT>::mix(float x, float y, float m) {
    return (1.f - m) * x + (m) * y;
}

template <typename ClockT, typename AudioStreamT>
inline int AudioProcess<ClockT, AudioStreamT>::advance_index(int w, int r, float freq, int tbl) {
    int wave_len = int(SR / freq + .5f);

    r = move_index(r, wave_len, tbl);

    //if (int df = dist_forward(r, w, tbl); df < VL)
    //    cout << "reader too close: " << df << "\twave_len: " << wave_len << endl;

    // skip past the discontinuity
    while (dist_forward(r, w, tbl) < VL)
        r = move_index(r, wave_len, tbl);

    return r;
}

template <typename ClockT, typename AudioStreamT>
inline float AudioProcess<ClockT, AudioStreamT>::reverse_dot_prod(
    const float* a_circular, const float* b_buff, int a_offset, int a_sz, int b_sz, float a_scale) {
    float md = 0.f;
    for (int i = 0; i < b_sz; ++i) {
        const float xi_1 = a_circular[(i + a_offset) % a_sz];
        const float xi_2 = b_buff[b_sz - i - 1];
        md += xi_1 / a_scale * xi_2;
    }
    return md;
}

// TODO try fft based cross correlation for perf reasons.
template <typename ClockT, typename AudioStreamT>
inline int AudioProcess<ClockT, AudioStreamT>::cross_correlation_sync(
    int w, int r, int dist, float* prev_buff[HISTORY_NUM_FRAMES], int frame_id, const float* buff, float channel_max) {
    // look through a range of dist samples centered at r
    r = move_index(r, -dist / 2, TBL);
    // Find r that gives best similarity between buff and prev_buff
    int max_r = r;
    float max_md = -std::numeric_limits<float>::infinity();
    for (int i = 0; i < dist / HISTORY_SEARCH_GRANULARITY; ++i) {
        float md = 0.f;
        for (int b = 0; b < HISTORY_NUM_FRAMES; ++b) {
            int cur_buf = (frame_id + b) % HISTORY_NUM_FRAMES;
            md += reverse_dot_prod(buff, prev_buff[cur_buf], r, TBL, HISTORY_BUFF_SZ, channel_max);
        }
        if (md > max_md) {
            max_md = md;
            max_r = r;
        }
        r = move_index(r, HISTORY_SEARCH_GRANULARITY, TBL);
    }
    return max_r;
}

template <typename ClockT, typename AudioStreamT>
inline typename ClockT::duration AudioProcess<ClockT, AudioStreamT>::dura(float x) {
    // dura() converts a second, represented as a double, into the appropriate unit of time for
    // ClockT and with the appropriate arithematic type using dura() avoids errors like this :
    // chrono::seconds(double initializer) dura() : <double,seconds> ->
    // chrono::steady_clock::<typeof count(), time unit>
    return chrono::duration_cast<typename ClockT::duration>(
        chrono::duration<float, std::ratio<1>>(x));
}
