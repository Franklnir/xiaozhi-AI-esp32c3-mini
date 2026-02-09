/**
 * ESP32-C3 with INMP441 Microphone, MAX98357A Speaker, and SSD1306 OLED
 * 
 * Wiring:
 * GPIO 5 - BCLK (shared by mic and speaker)
 * GPIO 6 - WS/LRC (shared by mic and speaker)
 * GPIO 4 - INMP441 SD (Mic Data In)
 * GPIO 7 - MAX98357A DIN (Speaker Data Out)
 * GPIO 3 - Push-to-talk + WiFi config button
 * GPIO 2 - Hands-free mode toggle + Reset SSID button (optional)
 * GPIO 8 - OLED SDA
 * GPIO 9 - OLED SCL
 */

#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_ssd1306.h>
#include <driver/i2c_master.h>
#include <nvs.h>
#include <cstdio>

#define TAG "Esp32c3Inmp441Board"

class Esp32c3Inmp441Board : public WifiBoard {
private:
    Button boot_button_;
    Button reset_ssid_button_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_timer_handle_t hands_free_timer_ = nullptr;
    int64_t last_hands_free_trigger_us_ = 0;
    int64_t last_voice_activity_us_ = 0;
    bool hands_free_enabled_ = true;
    bool wait_for_wake_word_ = false;
    bool boot_button_pressed_ = false;

    void ToggleHandsFreeMode() {
        hands_free_enabled_ = !hands_free_enabled_;
        auto& app = Application::GetInstance();
        if (hands_free_enabled_) {
            ESP_LOGI(TAG, "Hands-free mode enabled");
            GetDisplay()->ShowNotification("Hands-free ON");
            last_hands_free_trigger_us_ = 0;
            last_voice_activity_us_ = esp_timer_get_time();
            wait_for_wake_word_ = false;
            app.GetAudioService().EnableWakeWordDetection(true);
            // Enter listening immediately when enabled, then idle timeout will close it.
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.ToggleChatState();
            }
        } else {
            ESP_LOGI(TAG, "Hands-free mode disabled");
            GetDisplay()->ShowNotification("Hands-free OFF");
            wait_for_wake_word_ = false;
            app.GetAudioService().EnableWakeWordDetection(false);
            // If currently listening, close channel and return to idle.
            // If speaking, it may return to listening briefly, then timer branch will close it.
            if (app.GetDeviceState() == kDeviceStateListening) {
                app.ToggleChatState();
            }
        }
    }

    bool ClearStoredWifiCredentials() {
        nvs_handle_t nvs_handle = 0;
        esp_err_t ret = nvs_open("wifi", NVS_READWRITE, &nvs_handle);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "WiFi namespace not found, nothing to clear");
            return true;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open WiFi namespace: %s", esp_err_to_name(ret));
            return false;
        }

        ret = nvs_erase_all(nvs_handle);
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to clear WiFi credentials: %s", esp_err_to_name(ret));
            return false;
        }

        ESP_LOGI(TAG, "Stored WiFi credentials cleared");
        return true;
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = true,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        ESP_LOGI(TAG, "I2C bus initialized (SDA: %d, SCL: %d)", DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
    }

    void InitializeButtons() {
        // Press down to start listening, release to stop (push-to-talk)
        boot_button_.OnPressDown([this]() {
            boot_button_pressed_ = true;
            auto& app = Application::GetInstance();
            app.StartListening();
        });
        boot_button_.OnPressUp([this]() {
            boot_button_pressed_ = false;
            auto& app = Application::GetInstance();
            app.StopListening();
        });
        
        // Click during startup to enter WiFi config mode
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            }
        });

        // GPIO2 single click toggles hands-free mode.
        reset_ssid_button_.OnClick([this]() {
            ToggleHandsFreeMode();
        });

        // Hold reset button to clear saved SSID/password and reboot.
        reset_ssid_button_.OnLongPress([this]() {
            ESP_LOGW(TAG, "Reset SSID button long pressed");

            if (!ClearStoredWifiCredentials()) {
                GetDisplay()->ShowNotification("Failed to reset SSID");
                return;
            }

            GetDisplay()->ShowNotification("SSID reset, rebooting...");
            esp_restart();
        });
    }

    void TryStartHandsFreeListening() {
#if HANDS_FREE_AUTO_LISTEN
        auto& app = Application::GetInstance();
        if (!hands_free_enabled_) {
            // OFF means truly OFF:
            // - keep wake-word disabled
            // - if channel is still listening, close it
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.GetAudioService().EnableWakeWordDetection(false);
            } else if (app.GetDeviceState() == kDeviceStateListening && !boot_button_pressed_) {
                app.ToggleChatState();
            }
            return;
        }

        auto state = app.GetDeviceState();
        int64_t now_us = esp_timer_get_time();

        // Push-to-talk (GPIO3) must not be interrupted by hands-free timer logic.
        if (boot_button_pressed_) {
            last_voice_activity_us_ = now_us;
            wait_for_wake_word_ = false;
            return;
        }

        // Do not auto-open listening while local audio is still playing.
        if (!app.GetAudioService().IsIdle()) {
            last_voice_activity_us_ = now_us;
            return;
        }

        // When user/device is actively talking, refresh activity timestamp.
        if (state == kDeviceStateSpeaking || app.IsVoiceDetected()) {
            last_voice_activity_us_ = now_us;
            wait_for_wake_word_ = false;
            return;
        }

        // In listening but no voice for timeout => close channel and go LOW_POWER.
        if (state == kDeviceStateListening) {
            if (now_us - last_voice_activity_us_ >= (int64_t)HANDS_FREE_IDLE_TIMEOUT_MS * 1000) {
                ESP_LOGI(TAG, "Hands-free idle timeout %d ms, entering low-power standby", HANDS_FREE_IDLE_TIMEOUT_MS);
                app.ToggleChatState();  // listening -> close channel -> idle -> LOW_POWER
#if CONFIG_USE_ESP_WAKE_WORD
                wait_for_wake_word_ = true;
                char hint[64];
                snprintf(hint, sizeof(hint), "Standby, say: %s", HANDS_FREE_WAKE_WORD_HINT);
                GetDisplay()->ShowNotification(hint);
#else
                // Fallback if wake word feature is disabled.
                wait_for_wake_word_ = false;
#endif
                last_hands_free_trigger_us_ = now_us;
                last_voice_activity_us_ = now_us;
            }
            return;
        }

        // Idle state:
        // - if waiting for wake word, do not auto reopen channel.
        // - if not waiting, keep hands-free active by opening channel.
        if (state == kDeviceStateIdle) {
            if (wait_for_wake_word_) {
                return;
            }

            if (now_us - last_hands_free_trigger_us_ < (int64_t)HANDS_FREE_AUTO_LISTEN_RETRY_MS * 1000) {
                return;
            }

            last_hands_free_trigger_us_ = now_us;
            app.ToggleChatState();  // idle -> listening(auto mode)
            last_voice_activity_us_ = now_us;
            return;
        }

        // Any non-idle/non-listening state resets wake-word wait gate.
        wait_for_wake_word_ = false;
#endif
    }

    void InitializeHandsFreeMode() {
#if HANDS_FREE_AUTO_LISTEN
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* board = static_cast<Esp32c3Inmp441Board*>(arg);
                board->TryStartHandsFreeListening();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "hands_free_listen",
            .skip_unhandled_events = true
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &hands_free_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(
            hands_free_timer_,
            (uint64_t)HANDS_FREE_AUTO_LISTEN_INTERVAL_MS * 1000));
        last_voice_activity_us_ = esp_timer_get_time();
        ESP_LOGI(TAG, "Hands-free enabled (interval=%d ms, retry=%d ms, idle_timeout=%d ms)",
                 HANDS_FREE_AUTO_LISTEN_INTERVAL_MS,
                 HANDS_FREE_AUTO_LISTEN_RETRY_MS,
                 HANDS_FREE_IDLE_TIMEOUT_MS);
#if !CONFIG_USE_ESP_WAKE_WORD
        ESP_LOGW(TAG, "Wake-word feature is disabled; standby wake by voice command is not available");
#endif
#else
        ESP_LOGI(TAG, "Hands-free auto-listen disabled, using push-to-talk on GPIO %d", BOOT_BUTTON_GPIO);
#endif
    }

public:
    Esp32c3Inmp441Board()
        : boot_button_(BOOT_BUTTON_GPIO),
          reset_ssid_button_(RESET_SSID_BUTTON_GPIO, false, RESET_SSID_LONG_PRESS_MS) {
        ESP_LOGI(TAG, "Initializing ESP32-C3 INMP441 Board with OLED");
        ESP_LOGI(TAG, "  BCLK: GPIO %d", AUDIO_I2S_GPIO_BCLK);
        ESP_LOGI(TAG, "  WS:   GPIO %d", AUDIO_I2S_GPIO_WS);
        ESP_LOGI(TAG, "  DIN:  GPIO %d (Mic)", AUDIO_I2S_GPIO_DIN);
        ESP_LOGI(TAG, "  DOUT: GPIO %d (Speaker)", AUDIO_I2S_GPIO_DOUT);
        ESP_LOGI(TAG, "  Button: GPIO %d", BOOT_BUTTON_GPIO);
        ESP_LOGI(TAG, "  GPIO2 Button: GPIO %d hands-free toggle (click), reset SSID (long press: %d ms)",
                 RESET_SSID_BUTTON_GPIO, RESET_SSID_LONG_PRESS_MS);
        ESP_LOGI(TAG, "  OLED: SDA=%d, SCL=%d (%dx%d)", 
                 DISPLAY_SDA_PIN, DISPLAY_SCL_PIN, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        
        InitializeI2c();
        InitializeButtons();
        InitializeHandsFreeMode();
    }

    virtual AudioCodec* GetAudioCodec() override {
        // NoAudioCodecDuplex for INMP441 mic + MAX98357A speaker
        // Uses same I2S bus with shared BCLK/WS
        static NoAudioCodecDuplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN
        );
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        static Display* display = nullptr;
        
        if (display == nullptr) {
            // Try to initialize SSD1306 OLED - first try 0x3C, then 0x3D
            uint8_t i2c_addresses[] = {0x3C, 0x3D};
            esp_lcd_panel_io_handle_t panel_io = nullptr;
            esp_lcd_panel_handle_t panel = nullptr;
            
            for (int i = 0; i < 2; i++) {
                uint8_t addr = i2c_addresses[i];
                ESP_LOGI(TAG, "Trying OLED at I2C address 0x%02X", addr);
                
                esp_lcd_panel_io_i2c_config_t io_config = {
                    .dev_addr = addr,
                    .on_color_trans_done = nullptr,
                    .user_ctx = nullptr,
                    .control_phase_bytes = 1,
                    .dc_bit_offset = 6,
                    .lcd_cmd_bits = 8,
                    .lcd_param_bits = 8,
                    .flags = {
                        .dc_low_on_data = false,
                        .disable_control_phase = false,
                    },
                    .scl_speed_hz = 400000,
                };
                
                if (esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &panel_io) != ESP_OK) {
                    continue;
                }

                esp_lcd_panel_dev_config_t panel_config = {
                    .reset_gpio_num = GPIO_NUM_NC,
                    .bits_per_pixel = 1,
                    .flags = {
                        .reset_active_high = false,
                    },
                    .vendor_config = nullptr,
                };
                
                if (esp_lcd_new_panel_ssd1306(panel_io, &panel_config, &panel) != ESP_OK) {
                    esp_lcd_panel_io_del(panel_io);
                    panel_io = nullptr;
                    continue;
                }
                
                if (esp_lcd_panel_reset(panel) != ESP_OK ||
                    esp_lcd_panel_init(panel) != ESP_OK ||
                    esp_lcd_panel_disp_on_off(panel, true) != ESP_OK) {
                    esp_lcd_panel_del(panel);
                    esp_lcd_panel_io_del(panel_io);
                    panel = nullptr;
                    panel_io = nullptr;
                    continue;
                }
                
                // Success!
                ESP_LOGI(TAG, "OLED initialized at address 0x%02X", addr);
                display = new OledDisplay(panel_io, panel, 
                                          DISPLAY_WIDTH, DISPLAY_HEIGHT, 
                                          DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
                break;
            }
            
            if (display == nullptr) {
                // OLED not found, use no display
                ESP_LOGW(TAG, "OLED not found, continuing without display");
                static NoDisplay no_display;
                display = &no_display;
            }
        }
        return display;
    }
};

DECLARE_BOARD(Esp32c3Inmp441Board);
