#include "no_audio_processor.h"
#include <esp_log.h>
#include <cstdint>

#define TAG "NoAudioProcessor"

void NoAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
}

void NoAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (!is_running_ || !output_callback_) {
        return;
    }

    // Simple level-based VAD for boards that don't use AFE.
    // This keeps hands-free idle timeout behavior functional.
    if (vad_state_change_callback_) {
        int32_t peak = 0;
        size_t step = codec_->input_channels() == 2 ? 2 : 1;
        for (size_t i = 0; i < data.size(); i += step) {
            int32_t v = data[i];
            if (v < 0) {
                v = -v;
            }
            if (v > peak) {
                peak = v;
            }
        }

        constexpr int32_t kSpeechPeakThreshold = 1200;
        constexpr int32_t kSilencePeakThreshold = 700;
        constexpr int kSpeechFramesToTrigger = 2;
        constexpr int kSilenceFramesToTrigger = 10;

        if (peak >= kSpeechPeakThreshold) {
            speech_frames_++;
            silent_frames_ = 0;
        } else if (peak <= kSilencePeakThreshold) {
            silent_frames_++;
            speech_frames_ = 0;
        }

        if (!is_speaking_ && speech_frames_ >= kSpeechFramesToTrigger) {
            is_speaking_ = true;
            vad_state_change_callback_(true);
        } else if (is_speaking_ && silent_frames_ >= kSilenceFramesToTrigger) {
            is_speaking_ = false;
            vad_state_change_callback_(false);
        }
    }

    if (codec_->input_channels() == 2) {
        // If input channels is 2, we need to fetch the left channel data
        auto mono_data = std::vector<int16_t>(data.size() / 2);
        for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
            mono_data[i] = data[j];
        }
        output_callback_(std::move(mono_data));
    } else {
        output_callback_(std::move(data));
    }
}

void NoAudioProcessor::Start() {
    is_running_ = true;
    is_speaking_ = false;
    speech_frames_ = 0;
    silent_frames_ = 0;
}

void NoAudioProcessor::Stop() {
    if (is_speaking_ && vad_state_change_callback_) {
        vad_state_change_callback_(false);
    }
    is_running_ = false;
    is_speaking_ = false;
    speech_frames_ = 0;
    silent_frames_ = 0;
}

bool NoAudioProcessor::IsRunning() {
    return is_running_;
}

void NoAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void NoAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

size_t NoAudioProcessor::GetFeedSize() {
    if (!codec_) {
        return 0;
    }
    return frame_samples_;
}

void NoAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
        ESP_LOGE(TAG, "Device AEC is not supported");
    }
}
