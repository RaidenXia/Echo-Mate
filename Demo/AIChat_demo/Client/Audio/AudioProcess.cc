#include "./AudioProcess.h"
#include "../Utils/user_log.h"
#include <iostream>
#include <fstream>

AudioProcess::AudioProcess(int sample_rate, int channels, int frame_duration_ms) 
    : sample_rate(sample_rate), 
      channels(channels), 
      frame_duration_ms(frame_duration_ms),
      encoder(nullptr), 
      decoder(nullptr), 
      capture_handle(nullptr),
      playback_handle(nullptr),
      capture_running(false),
      playback_running(false) {
    if (!initializeOpus()) {
        USER_LOG_ERROR("Failed to initialize Opus encoder/decoder.");
    }
}

AudioProcess::~AudioProcess() {
    stopRecording();
    stopPlaying();

    cleanupOpus();
    clearRecordedAudioQueue();
    clearPlaybackAudioQueue();
}

int AudioProcess::set_pcm_params(snd_pcm_t *pcm, snd_pcm_stream_t stream, unsigned int rate, unsigned int channels, snd_pcm_format_t format) {
    snd_pcm_hw_params_t *params;
    int err;

    snd_pcm_hw_params_alloca(&params);
    err = snd_pcm_hw_params_any(pcm, params);
    if (err < 0) return err;

    err = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) return err;

    err = snd_pcm_hw_params_set_format(pcm, params, format);
    if (err < 0) return err;

    err = snd_pcm_hw_params_set_channels(pcm, params, channels);
    if (err < 0) return err;

    unsigned int exact_rate = rate;
    err = snd_pcm_hw_params_set_rate_near(pcm, params, &exact_rate, 0);
    if (err < 0) return err;
    if (exact_rate != rate) {
        USER_LOG_WARN("Requested rate %u Hz, got %u Hz", rate, exact_rate);
    }

    // 设置 period size（每次传输的帧数）
    snd_pcm_uframes_t frames = sample_rate * frame_duration_ms / 1000;
    err = snd_pcm_hw_params_set_period_size_near(pcm, params, &frames, 0);
    if (err < 0) return err;

    err = snd_pcm_hw_params(pcm, params);
    return err;
}

void AudioProcess::capture_loop() {
    const char *device = "plughw:0,0"; 
    int err;

    err = snd_pcm_open(&capture_handle, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        USER_LOG_ERROR("Capture open failed: %s", snd_strerror(err));
        snd_pcm_close(capture_handle);
        capture_handle = nullptr;
        return;
    }

    err = set_pcm_params(capture_handle, SND_PCM_STREAM_CAPTURE, sample_rate, channels, SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        USER_LOG_ERROR("Capture hw params failed: %s", snd_strerror(err));
        snd_pcm_close(capture_handle);
        capture_handle = nullptr;
        return;
    }

    snd_pcm_uframes_t period_frames = sample_rate * frame_duration_ms / 1000;
    std::vector<int16_t> buffer(period_frames * channels);

    while (capture_running) {
        err = snd_pcm_readi(capture_handle, buffer.data(), period_frames);
        if (err == -EPIPE) {
            // xrun
            snd_pcm_prepare(capture_handle);
            continue;
        } else if (err < 0) {
            USER_LOG_ERROR("Capture read error: %s", snd_strerror(err));
            break;
        } else if (err != static_cast<int>(period_frames)) {
            // 短读，忽略或重试
            continue;
        }

        // 将数据放入队列
        {
            std::lock_guard<std::mutex> lock(recordedAudioMutex);
            if (recordedAudioQueue.size() >= 750) recordedAudioQueue.pop();
            recordedAudioQueue.push(buffer);
        }
        recordedAudioCV.notify_one();
    }

    snd_pcm_close(capture_handle);
    capture_handle = nullptr;
}

void AudioProcess::playback_loop() {
    const char *device = "default";
    int err;

    err = snd_pcm_open(&playback_handle, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        USER_LOG_ERROR("Playback open failed: %s", snd_strerror(err));
        snd_pcm_close(playback_handle);
        playback_handle = nullptr;
        return;
    }

    err = set_pcm_params(playback_handle, SND_PCM_STREAM_PLAYBACK, sample_rate, channels, SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        USER_LOG_ERROR("Playback hw params failed: %s", snd_strerror(err));
        snd_pcm_close(playback_handle);
        playback_handle = nullptr;
        return;
    }

    snd_pcm_uframes_t period_frames = sample_rate * frame_duration_ms / 1000;
    std::vector<int16_t> buffer(period_frames * channels);

    while (playback_running) {
        bool have_data = false;
        {
            std::lock_guard<std::mutex> lock(playbackMutex);
            if (!playbackQueue.empty()) {
                buffer = playbackQueue.front();
                playbackQueue.pop();
                have_data = true;
            }
        }

        if (!have_data) {
            // 队列空，播放静音
            // printf("Playback queue empty, playing silence\n");
            std::fill(buffer.begin(), buffer.end(), 0);
        }

        err = snd_pcm_writei(playback_handle, buffer.data(), period_frames);
        if (err == -EPIPE) {
            // underrun
            snd_pcm_prepare(playback_handle);
            continue;
        } else if (err < 0) {
            USER_LOG_ERROR("Playback write error: %s", snd_strerror(err));
            break;
        }
    }

    snd_pcm_close(playback_handle);
    playback_handle = nullptr;
}

bool AudioProcess::initializeOpus() {
    int error;

    // 初始化 Opus 编码器
    encoder = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK) {
        USER_LOG_ERROR("Opus encoder initialization failed: %s", opus_strerror(error));
        return false;
    }

    // 初始化 Opus 解码器
    decoder = opus_decoder_create(sample_rate, channels, &error);
    if (error != OPUS_OK) {
        USER_LOG_ERROR("Opus decoder initialization failed: %s", opus_strerror(error));
        opus_encoder_destroy(encoder);
        return false;
    }
    return true;
}

void AudioProcess::cleanupOpus() {
    if (encoder) {
        opus_encoder_destroy(encoder);
        encoder = nullptr;
    }
    if (decoder) {
        opus_decoder_destroy(decoder);
        decoder = nullptr;
    }
}

bool AudioProcess::startRecording() {
    if (capture_running) {
        USER_LOG_WARN("Already recording.");
        return false;
    }
    capture_running = true;
    capture_thread = std::thread(&AudioProcess::capture_loop, this);
    USER_LOG_INFO("Recording started.");
    return true;
}

bool AudioProcess::stopRecording() {
    if (!capture_running) return false;
    capture_running = false;
    if (capture_thread.joinable()) capture_thread.join();
    USER_LOG_INFO("Recording stopped.");
    return true;
}

bool AudioProcess::getRecordedAudio(std::vector<int16_t>& recordedData) {
    std::unique_lock<std::mutex> lock(recordedAudioMutex);
    recordedAudioCV.wait(lock, [this] { return !recordedAudioQueue.empty() || !capture_running; });

    if (recordedAudioQueue.empty()) {
        return false; // 队列为空且不再录音
    }

    recordedData.swap(recordedAudioQueue.front());
    recordedAudioQueue.pop();
    return true;
}

void AudioProcess::clearRecordedAudioQueue() {
    std::lock_guard<std::mutex> lock(recordedAudioMutex);
    std::queue<std::vector<int16_t>> empty;
    std::swap(recordedAudioQueue, empty);
}

bool AudioProcess::startPlaying() {
    if (playback_running) {
        USER_LOG_WARN("Already playing.");
        return false;
    }
    playback_running = true;
    playback_thread = std::thread(&AudioProcess::playback_loop, this);
    USER_LOG_INFO("Playback started.");
    return true;
}

bool AudioProcess::stopPlaying() {
    if (!playback_running) return false;
    playback_running = false;
    if (playback_thread.joinable()) playback_thread.join();
    USER_LOG_INFO("Playback stopped.");
    return true;
}

void AudioProcess::clearPlaybackAudioQueue() {
    std::lock_guard<std::mutex> lock(playbackMutex);
    std::queue<std::vector<int16_t>> empty;
    std::swap(playbackQueue, empty);
}

void AudioProcess::addFrameToPlaybackQueue(const std::vector<int16_t>& pcm_frame) {
    std::lock_guard<std::mutex> lock(playbackMutex);
    
    // 计算每帧的样本数量
    int frame_size = sample_rate / 1000 * frame_duration_ms * channels;

    // 如果当前帧大小小于预期的帧大小，则填充静音
    if (pcm_frame.size() < static_cast<size_t>(frame_size)) {
        auto tempFrame = pcm_frame;
        tempFrame.resize(frame_size, 0); // 使用0填充至目标长度
        playbackQueue.push(tempFrame);
    } else {
        playbackQueue.push(pcm_frame);
    }
}

std::queue<std::vector<int16_t>> AudioProcess::loadAudioFromFile(const std::string& filename, int frame_duration_ms) {
    std::ifstream infile(filename, std::ios::binary);
    if (!infile) {
        USER_LOG_ERROR("Failed to open file: %s", filename.c_str());
        return std::queue<std::vector<int16_t>>();
    }

    // 获取文件大小
    infile.seekg(0, std::ios::end);
    std::streampos fileSize = infile.tellg();
    infile.seekg(0, std::ios::beg);

    // 计算样本数量
    size_t numSamples = static_cast<size_t>(fileSize) / sizeof(int16_t);

    // 读取音频数据
    std::vector<int16_t> audio_data(numSamples);
    infile.read(reinterpret_cast<char*>(audio_data.data()), fileSize);

    if (!infile) {
        USER_LOG_ERROR("Error reading file: %s", filename.c_str());
        return std::queue<std::vector<int16_t>>();
    }

    // 计算每帧的样本数量
    int frame_size = sample_rate / 1000 * frame_duration_ms * channels;

    // 将音频数据切分成帧
    std::queue<std::vector<int16_t>> audio_frames;
    for (size_t i = 0; i < numSamples; i += frame_size) {
        size_t remaining_samples = numSamples - i;
        size_t current_frame_size = (remaining_samples > frame_size) ? frame_size : remaining_samples;

        std::vector<int16_t> frame(current_frame_size);
        std::copy(audio_data.begin() + i, audio_data.begin() + i + current_frame_size, frame.begin());
        audio_frames.push(frame);
    }

    return audio_frames;
}


void AudioProcess::saveToPCMFile(const std::string& filename, const std::queue<std::vector<int16_t>>& audioQueue) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        USER_LOG_ERROR("Failed to open file: %s", filename.c_str());
        return;
    }

    {
        std::queue<std::vector<int16_t>> tempQueue = audioQueue;
        while (!tempQueue.empty()) {
            const std::vector<int16_t>& frame = tempQueue.front();
            file.write(reinterpret_cast<const char*>(frame.data()), frame.size() * sizeof(int16_t));
            tempQueue.pop();
        }
    }

    file.close();
    USER_LOG_INFO("Saved recording to %s", filename.c_str());
}

void AudioProcess::saveToPCMFile(const std::string& filename) {
    std::unique_lock<std::mutex> lock(recordedAudioMutex);
    saveToPCMFile(filename, recordedAudioQueue);
}

bool AudioProcess::encode(const std::vector<int16_t>& pcm_frame, uint8_t* opus_data, size_t& opus_data_size) {
    if (!encoder) {
        USER_LOG_ERROR("Encoder not initialized");
        return false;
    }

    int frame_size = pcm_frame.size() / channels; // 每帧的样本数量（单通道）

    if (frame_size <= 0) {
        USER_LOG_ERROR("Invalid PCM frame size: %d", frame_size);
        return false;
    }

    // 对当前帧进行编码
    int encoded_bytes_size = opus_encode(encoder, pcm_frame.data(), frame_size, opus_data, 2048); // max 2048 bytes

    if (encoded_bytes_size < 0) {
        USER_LOG_ERROR("Encoding failed: %s", opus_strerror(encoded_bytes_size));
        return false;
    }

    opus_data_size = static_cast<size_t>(encoded_bytes_size);
    return true;
}

bool AudioProcess::decode(const uint8_t* opus_data, size_t opus_data_size, std::vector<int16_t>& pcm_frame) {
    if (!decoder) {
        USER_LOG_ERROR("Decoder not initialized");
        return false;
    }

    int frame_size = 960;  // 40ms 帧, 16000Hz 采样率, 理论上应该是 640 个样本，但是 Opus 限制为 960
    // int frame_size = sample_rate / 1000 * frame_duration_ms;
    pcm_frame.resize(frame_size * channels);

    // 对当前帧进行解码
    int decoded_samples = opus_decode(decoder, opus_data, static_cast<int>(opus_data_size), pcm_frame.data(), frame_size, 0);

    if (decoded_samples < 0) {
        USER_LOG_ERROR("Decoding failed: %s", opus_strerror(decoded_samples));
        return false;
    }

    pcm_frame.resize(decoded_samples * channels);
    return true;
}

BinProtocol* AudioProcess::PackBinFrame(const uint8_t* payload, size_t payload_size, int ws_protocol_version) {
    // Allocate memory for BinaryProtocol + payload
    auto pack = (BinProtocol*)malloc(sizeof(BinProtocol) + payload_size);
    if (!pack) {
        USER_LOG_ERROR("Memory allocation failed");
        return nullptr;
    }

    pack->version = htons(ws_protocol_version);
    pack->type = htons(0);  // Indicate audio data type
    pack->payload_size = htonl(payload_size);
    assert(sizeof(BinProtocol) == 8);

    // Copy payload data
    memcpy(pack->payload, payload, payload_size);

    return pack;
}

bool AudioProcess::UnpackBinFrame(const uint8_t* packed_data, size_t packed_data_size, BinProtocolInfo& protocol_info, std::vector<uint8_t>& opus_data) {
    // 检查输入数据的有效性
    if (packed_data_size < sizeof(uint16_t) * 2 + sizeof(uint32_t)) { // 至少需要2字节版本+2字节类型+4字节负载大小
        USER_LOG_ERROR("Packed data size is too small");
        return false;
    }

    // 解析头部信息
    const uint16_t* version_ptr = reinterpret_cast<const uint16_t*>(packed_data);
    const uint16_t* type_ptr = reinterpret_cast<const uint16_t*>(packed_data + sizeof(uint16_t));
    const uint32_t* payload_size_ptr = reinterpret_cast<const uint32_t*>(packed_data + sizeof(uint16_t) * 2);

    uint16_t version = ntohs(*version_ptr);
    uint16_t type = ntohs(*type_ptr);
    uint32_t payload_size = ntohl(*payload_size_ptr);

    // 确认总数据大小是否匹配
    if (packed_data_size < sizeof(uint16_t) * 2 + sizeof(uint32_t) + payload_size) {
        USER_LOG_ERROR("Packed data size does not match payload size");
        return false;
    }

    // protocol_info
    protocol_info.version = version;
    protocol_info.type = type;

    // 提取并填充opus_data
    opus_data.clear();
    opus_data.insert(opus_data.end(), packed_data + sizeof(uint16_t) * 2 + sizeof(uint32_t), 
                     packed_data + sizeof(uint16_t) * 2 + sizeof(uint32_t) + payload_size);

    return true;
}