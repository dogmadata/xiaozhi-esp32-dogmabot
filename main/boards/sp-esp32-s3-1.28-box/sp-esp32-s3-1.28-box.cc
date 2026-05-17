#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "lvgl_theme.h"
#include "application.h"
#include "board.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "audio/audio_codec.h"
#include <cstring>
#include <ctime>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>
#include "system_reset.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <esp_timer.h>
#include "i2c_device.h"
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>
#include "power_save_timer.h"
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_manager.h"

#define TAG "Spotpear_ESP32_S3_1_28_BOX"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);


class Cst816d : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };
    Cst816d(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        last_chip_id_ = chip_id;
        read_buffer_ = new uint8_t[6];
    }

    ~Cst816d() {
        if (read_buffer_) {
            delete[] read_buffer_;
            read_buffer_ = nullptr;
        }
    }

    void UpdateTouchPoint() {
        if (!read_buffer_) return;
        ReadRegs(0x02, read_buffer_, 6);
        if (read_buffer_[0] == 0xFF) {
            read_buffer_[0] = 0x00;
        }
        tp_.num = read_buffer_[0] & 0x01;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    const TouchPoint_t& GetTouchPoint() const {
        return tp_;
    }

    static bool Probe(i2c_master_bus_handle_t i2c_bus, uint8_t addr, uint8_t& chip_id) {
        if (!i2c_bus) return false;
        i2c_master_dev_handle_t dev = nullptr;
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 400 * 1000,
            .scl_wait_us = 0,
            .flags = {
                .disable_ack_check = 0,
            },
        };
        esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &cfg, &dev);
        if (ret != ESP_OK || dev == nullptr) {
            return false;
        }
        uint8_t reg = 0xA3;
        uint8_t id = 0;
        ret = i2c_master_transmit_receive(dev, &reg, 1, &id, 1, 100);
        i2c_master_bus_rm_device(dev);
        if (ret == ESP_OK) {
            chip_id = id;
            return true;
        }
        return false;
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
    uint8_t last_chip_id_ = 0;
};


// Round-display UI for the 240x240 GC9A01. The upstream SpiLcdDisplay layout
// assumes a rectangular screen; on this board the corners get clipped by the
// circular bezel. We build a different widget tree here, reusing the same
// protected member pointers from LvglDisplay/LcdDisplay so the base classes'
// SetStatus/UpdateStatusBar/SetEmotion/ShowNotification all keep working.
//
// Side arc geometry: two arcs of kArcSpanDeg each, leaving a kArcGapDeg gap
// at 12h and another at 6h. With the gap at 20°, each arc covers 160°; their
// midpoints sit on the horizontal axis (3h / 9h). Right arc = battery, left
// arc = volume.
constexpr int kArcGapDeg = 20;
constexpr int kArcSpanDeg = 180 - kArcGapDeg;          // 160
constexpr int kArcHalfSpanDeg = kArcSpanDeg / 2;       // 80
constexpr int kRightArcRotation = 270 + kArcGapDeg / 2; // 280 (10° past top, CW)
constexpr int kLeftArcRotation  =  90 + kArcGapDeg / 2; // 100 (10° past bottom, CW)

class CustomLcdDisplay : public SpiLcdDisplay {
public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                    esp_lcd_panel_handle_t panel_handle,
                    int width,
                    int height,
                    int offset_x,
                    int offset_y,
                    bool mirror_x,
                    bool mirror_y,
                    bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
    }

    virtual void SetupUI() override {
        Display::SetupUI();  // marks setup_ui_called_; does NOT call SpiLcdDisplay::SetupUI

        DisplayLockGuard lock(this);
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        auto text_font = lvgl_theme->text_font()->font();
        auto icon_font = lvgl_theme->icon_font()->font();
        auto large_icon_font = lvgl_theme->large_icon_font()->font();

        auto screen = lv_screen_active();
        lv_obj_set_style_text_font(screen, text_font, 0);
        lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
        lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

        // Side arcs — symmetric on the left and right of the circle. Each
        // spans kArcSpanDeg centered on the horizontal axis (3h / 9h), with
        // 20° gaps at 12h and 6h. The right arc reports battery level; the
        // left arc reports volume. Both shrink from their midpoint outward
        // (see UpdateStatusBar). The bg track is tinted with the theme's
        // text color so it reads on both light and dark themes.
        battery_arc_right_ = MakeRimArc(screen, kRightArcRotation, kArcSpanDeg);
        volume_arc_left_   = MakeRimArc(screen, kLeftArcRotation,  kArcSpanDeg);
        StyleArcBackground(battery_arc_right_, lvgl_theme);
        StyleArcBackground(volume_arc_left_,   lvgl_theme);

        // Top row — WiFi icon + clock, centered horizontally in the gap above
        // the side arcs. y = 22 keeps the row clear of the arc endpoints that
        // reach down to ~y = 11 at the rim.
        top_bar_ = lv_obj_create(screen);
        lv_obj_set_size(top_bar_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 22);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(top_bar_, 0, 0);
        lv_obj_set_style_pad_all(top_bar_, 0, 0);
        lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

        network_label_ = lv_label_create(top_bar_);
        lv_label_set_text(network_label_, "");
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
        lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

        clock_label_ = lv_label_create(top_bar_);
        lv_label_set_text(clock_label_, "--:--");
        lv_obj_set_style_text_color(clock_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_margin_left(clock_label_, lvgl_theme->spacing(2), 0);

        // Side icons — pushed out toward the arcs so they read as one unit
        // with the matching rim arc. With the icon ~16 px wide and the inner
        // arc edge near x=8 on each side, x=±15 leaves a small gap.
        mute_label_ = lv_label_create(screen);
        lv_label_set_text(mute_label_, FONT_AWESOME_VOLUME_HIGH);
        lv_obj_align(mute_label_, LV_ALIGN_LEFT_MID, 15, 0);
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

        battery_label_ = lv_label_create(screen);
        lv_label_set_text(battery_label_, "");
        lv_obj_align(battery_label_, LV_ALIGN_RIGHT_MID, -15, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);

        // Emoji image + AI-logo fallback, centered.
        emoji_image_ = lv_image_create(screen);
        lv_obj_center(emoji_image_);
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

        emoji_label_ = lv_label_create(screen);
        lv_obj_center(emoji_label_);
        lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
        lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);

        // Device state ("Listening...", "Speaking...", etc.) sits between
        // the top row and the emoji. Notification reuses the same anchor so
        // it covers the state text when it fires.
        status_label_ = lv_label_create(screen);
        lv_obj_set_size(status_label_, 180, 22);
        lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 58);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
        lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);

        notification_label_ = lv_label_create(screen);
        lv_obj_set_size(notification_label_, 180, 28);
        lv_obj_align(notification_label_, LV_ALIGN_TOP_MID, 0, 56);
        lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_bg_color(notification_label_, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(notification_label_, LV_OPA_70, 0);
        lv_obj_set_style_radius(notification_label_, 12, 0);
        lv_obj_set_style_pad_all(notification_label_, lvgl_theme->spacing(2), 0);
        lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_DOT);
        lv_label_set_text(notification_label_, "");
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

        // Chat marquee — small rounded box just below the emoji that scrolls
        // long transcripts horizontally. Uses inverted theme colors so the
        // pill stands out against the screen background.
        chat_box_ = lv_obj_create(screen);
        lv_obj_set_size(chat_box_, 196, 30);
        lv_obj_align(chat_box_, LV_ALIGN_CENTER, 0, 64);
        lv_obj_set_style_radius(chat_box_, 8, 0);
        lv_obj_set_style_bg_color(chat_box_, lvgl_theme->text_color(), 0);
        lv_obj_set_style_bg_opa(chat_box_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chat_box_, 0, 0);
        lv_obj_set_style_pad_hor(chat_box_, lvgl_theme->spacing(3), 0);
        lv_obj_set_style_pad_ver(chat_box_, 0, 0);
        lv_obj_set_scrollbar_mode(chat_box_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(chat_box_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(chat_box_, LV_OBJ_FLAG_HIDDEN);

        chat_message_label_ = lv_label_create(chat_box_);
        lv_obj_set_width(chat_message_label_, lv_pct(100));
        lv_obj_center(chat_message_label_);
        lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->background_color(), 0);
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_label_set_text(chat_message_label_, "");

        // Low-battery popup, same role as the base layout — sits above the
        // bottom band so it stays readable when something else is showing.
        low_battery_popup_ = lv_obj_create(screen);
        lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(low_battery_popup_, 180, 40);
        lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -92);
        lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
        lv_obj_set_style_radius(low_battery_popup_, 12, 0);
        lv_obj_set_style_border_width(low_battery_popup_, 0, 0);
        low_battery_label_ = lv_label_create(low_battery_popup_);
        lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
        lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
        lv_obj_center(low_battery_label_);
        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

        // container_/content_ are intentionally left nullptr — base
        // LcdDisplay::SetChatMessage early-returns on null content_, and we
        // override SetChatMessage below to write to chat_message_label_ directly.
    }

    // Show the last assistant/user message in the chat pill below the emoji.
    // Long content scrolls horizontally (CIRCULAR marquee). While the pill is
    // visible we hide the bottom status_label_ so the clock/state doesn't
    // overlap or compete with the transcript.
    virtual void SetChatMessage(const char* role, const char* content) override {
        if (chat_box_ == nullptr || chat_message_label_ == nullptr) return;
        DisplayLockGuard lock(this);

        if (content == nullptr || strlen(content) == 0) {
            lv_label_set_text(chat_message_label_, "");
            lv_obj_add_flag(chat_box_, LV_OBJ_FLAG_HIDDEN);
            if (status_label_ != nullptr &&
                (notification_label_ == nullptr ||
                 lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN))) {
                lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }

        if (role != nullptr && strcmp(role, "system") == 0) {
            return;  // system messages stay off-screen on the round UI
        }

        if (status_label_ != nullptr) {
            lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(chat_message_label_, content);
        lv_obj_remove_flag(chat_box_, LV_OBJ_FLAG_HIDDEN);
    }

    // The base LcdDisplay::SetTheme assumes container_/top_bar_/bottom_bar_/etc.
    // exist (it dereferences container_ unconditionally). We left container_
    // null on purpose, so reimplement SetTheme against the widget tree we
    // actually created in SetupUI.
    virtual void SetTheme(Theme* theme) override {
        DisplayLockGuard lock(this);
        auto lvgl_theme = static_cast<LvglTheme*>(theme);
        auto screen = lv_screen_active();
        auto text_font = lvgl_theme->text_font()->font();
        auto icon_font = lvgl_theme->icon_font()->font();
        auto large_icon_font = lvgl_theme->large_icon_font()->font();
        auto chosen_icon_font = (text_font->line_height >= 40) ? large_icon_font : icon_font;

        lv_obj_set_style_text_font(screen, text_font, 0);
        lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
        lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

        if (mute_label_)        { lv_obj_set_style_text_font(mute_label_, chosen_icon_font, 0);
                                  lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0); }
        if (battery_label_)     { lv_obj_set_style_text_font(battery_label_, chosen_icon_font, 0);
                                  lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0); }
        if (network_label_)     { lv_obj_set_style_text_font(network_label_, chosen_icon_font, 0);
                                  lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0); }
        if (status_label_)       lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
        if (clock_label_)        lv_obj_set_style_text_color(clock_label_, lvgl_theme->text_color(), 0);
        if (chat_box_)           lv_obj_set_style_bg_color(chat_box_, lvgl_theme->text_color(), 0);
        if (chat_message_label_) lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->background_color(), 0);
        if (notification_label_) {
            lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
            lv_obj_set_style_bg_color(notification_label_, lvgl_theme->user_bubble_color(), 0);
        }
        if (emoji_label_)        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
        if (low_battery_popup_)  lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
        StyleArcBackground(battery_arc_right_, lvgl_theme);
        StyleArcBackground(volume_arc_left_,   lvgl_theme);

        Display::SetTheme(lvgl_theme);  // persists the theme name to NVS
    }

    virtual void ClearChatMessages() override {
        if (chat_box_ == nullptr) return;
        DisplayLockGuard lock(this);
        if (chat_message_label_ != nullptr) lv_label_set_text(chat_message_label_, "");
        lv_obj_add_flag(chat_box_, LV_OBJ_FLAG_HIDDEN);
        if (status_label_ != nullptr &&
            (notification_label_ == nullptr ||
             lv_obj_has_flag(notification_label_, LV_OBJ_FLAG_HIDDEN))) {
            lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Drive the two rim arcs, the clock and the volume icon. The base
    // LvglDisplay::UpdateStatusBar would otherwise overwrite status_label_
    // with the time when the device is idle; here we suppress that and feed
    // the time into a dedicated clock_label_ at the top instead.
    virtual void UpdateStatusBar(bool update_all = false) override {
        SpiLcdDisplay::UpdateStatusBar(update_all);

        DisplayLockGuard lock(this);
        auto& board = Board::GetInstance();
        auto codec = board.GetAudioCodec();

        // Clock at the top of the screen — always shows current time once
        // the system clock is set.
        if (clock_label_ != nullptr) {
            time_t now = time(nullptr);
            struct tm* tmv = localtime(&now);
            if (tmv != nullptr && tmv->tm_year >= 2025 - 1900) {
                char buf[16];
                strftime(buf, sizeof(buf), "%H:%M", tmv);
                lv_label_set_text(clock_label_, buf);
            }
        }

        // If the device is idle, the base just wrote "HH:MM" into
        // status_label_. We have a dedicated clock for that, so clear the
        // status text in idle so the area stays free for the emoji.
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateIdle && status_label_ != nullptr) {
            lv_label_set_text(status_label_, "");
        }

        // Volume icon: speaker-high when audible, speaker-x when muted.
        if (mute_label_ != nullptr && codec != nullptr) {
            const char* icon = (codec->output_volume() == 0)
                ? FONT_AWESOME_VOLUME_XMARK
                : FONT_AWESOME_VOLUME_HIGH;
            lv_label_set_text(mute_label_, icon);
        }

        // Right arc — battery.
        int level;
        bool charging;
        bool discharging;
        if (board.GetBatteryLevel(level, charging, discharging) && battery_arc_right_ != nullptr) {
            lv_color_t color;
            if (charging)              color = lv_color_hex(0x33aaff);
            else if (level <= 20)      color = lv_color_hex(0xcc4444);
            else if (level <= 50)      color = lv_color_hex(0xddaa33);
            else                       color = lv_color_hex(0x33aa55);
            SetArcLevel(battery_arc_right_, level, color);
        }

        // Left arc — volume.
        if (volume_arc_left_ != nullptr && codec != nullptr) {
            int vol = codec->output_volume();
            if (vol < 0)        vol = 0;
            else if (vol > 100) vol = 100;
            lv_color_t color = (vol == 0)
                ? lv_color_hex(0x666666)              // muted = grey
                : lv_color_hex(0x2288dd);             // audible = blue
            SetArcLevel(volume_arc_left_, vol, color);
        }
    }

private:
    // Helper for constructing one of the rim arcs. The indicator is driven
    // explicitly by lv_arc_set_angles — we don't rely on arc value/mode, so
    // the helper just builds an empty arc collapsed at its midpoint.
    static lv_obj_t* MakeRimArc(lv_obj_t* parent, int rotation_deg, int span_deg) {
        lv_obj_t* arc = lv_arc_create(parent);
        lv_obj_set_size(arc, 240, 240);
        lv_obj_center(arc);
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_arc_set_rotation(arc, rotation_deg);
        lv_arc_set_bg_angles(arc, 0, span_deg);
        lv_arc_set_angles(arc, span_deg / 2, span_deg / 2);  // collapsed
        lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
        lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
        return arc;
    }

    // Bg-track color = theme text color at 30% opacity — gives a visible but
    // muted track on both light (dark track on white) and dark (light track
    // on black) themes.
    static void StyleArcBackground(lv_obj_t* arc, LvglTheme* theme) {
        if (arc == nullptr || theme == nullptr) return;
        lv_obj_set_style_arc_color(arc, theme->text_color(), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_30, LV_PART_MAIN);
    }

    // Drive a rim arc from a 0–100 level. The indicator grows symmetrically
    // outward from the arc's midpoint.
    static void SetArcLevel(lv_obj_t* arc, int level, lv_color_t color) {
        int half = level * kArcHalfSpanDeg / 100;
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_angles(arc, kArcHalfSpanDeg - half, kArcHalfSpanDeg + half);
        lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    }

    lv_obj_t* battery_arc_right_ = nullptr;
    lv_obj_t* volume_arc_left_   = nullptr;
    lv_obj_t* clock_label_       = nullptr;
    lv_obj_t* chat_box_          = nullptr;
};


class Spotpear_ESP32_S3_1_28_BOX : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    Display* display_ = nullptr;
    esp_timer_handle_t touchpad_timer_ = nullptr;
    Cst816d* cst816d_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    PowerManager* power_manager_ = nullptr;

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_3);
        rtc_gpio_set_direction(GPIO_NUM_3, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_3, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 290);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            // 关闭ES8311音频编解码器
            auto codec = GetAudioCodec();
            if (codec) {
                codec->EnableInput(false);
                codec->EnableOutput(false);
            }
            rtc_gpio_set_level(GPIO_NUM_3, 0);
            // 启用保持功能，确保睡眠期间电平不变
            rtc_gpio_hold_en(GPIO_NUM_3);
            esp_lcd_panel_disp_on_off(panel_, false); //关闭显示
            esp_deep_sleep_start();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(BATTERY_CHARGING_PIN, ADC_CHANNEL_0);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            // .glitch_ignore_cnt = 7,
            // .intr_priority = 0,
            // .trans_queue_depth = 0,
            // .flags = {
            //     .enable_internal_pullup = 1,
            // },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeCodecI2c_Touch() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = TP_PIN_NUM_TP_SDA,
            .scl_io_num = TP_PIN_NUM_TP_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        esp_err_t ret = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
            i2c_bus_ = nullptr;
        }
    }


    static void touchpad_timer_callback(void* arg) {
        auto* board = static_cast<Spotpear_ESP32_S3_1_28_BOX*>(arg);
        if (!board || !board->cst816d_) return;
        static bool was_touched = false;
        static int64_t touch_start_time = 0;
        const int64_t TOUCH_THRESHOLD_MS = 500;  // 触摸时长阈值，超过500ms视为长按

        board->cst816d_->UpdateTouchPoint();
        auto touch_point = board->cst816d_->GetTouchPoint();

        // 检测触摸开始
        if (touch_point.num > 0 && !was_touched) {
            was_touched = true;
            touch_start_time = esp_timer_get_time() / 1000; // 转换为毫秒
        }
        // 检测触摸释放
        else if (touch_point.num == 0 && was_touched) {
            was_touched = false;
            int64_t touch_duration = (esp_timer_get_time() / 1000) - touch_start_time;

            // 只有短触才触发
            if (touch_duration < TOUCH_THRESHOLD_MS) {
                auto& app = Application::GetInstance();
                // During startup (before connected), pressing touch enters Wi-Fi config mode without reboot
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    board->EnterWifiConfigMode();
                    return;
                }
                app.ToggleChatState();
            }
        }
    }

    void InitializeCst816DTouchPad() {
        ESP_LOGI(TAG, "Init Cst816D");

        // RST/INT 管脚初始化
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << TP_PIN_NUM_TP_RST);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);

        gpio_config_t int_conf = {};
        int_conf.intr_type = GPIO_INTR_DISABLE;
        int_conf.mode = GPIO_MODE_INPUT;
        int_conf.pin_bit_mask = (1ULL << TP_PIN_NUM_TP_INT);
        int_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        int_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&int_conf);

        // 触摸芯片复位序列
        gpio_set_level(TP_PIN_NUM_TP_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(TP_PIN_NUM_TP_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));

        // 探测是否存在触摸芯片
        uint8_t chip_id = 0;
        if (!i2c_bus_) {
            ESP_LOGW(TAG, "Touch I2C bus not initialized, skip touch");
            return;
        }
        bool touch_available = Cst816d::Probe(i2c_bus_, 0x15, chip_id);
        if (!touch_available) {
            ESP_LOGW(TAG, "CST816D not found, running in non-touch mode");
            // 释放触摸I2C，避免无设备时反复报错
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
            return;
        }

        cst816d_ = new Cst816d(i2c_bus_, 0x15);

        // 创建定时器，10ms 间隔
        esp_timer_create_args_t timer_args = {
            .callback = touchpad_timer_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touchpad_timer",
            .skip_unhandled_events = true,
        };

        if (esp_timer_create(&timer_args, &touchpad_timer_) == ESP_OK) {
            esp_timer_start_periodic(touchpad_timer_, 10 * 1000); // 10ms = 10000us
        }
    }

    // SPI初始化
    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(DISPLAY_SPI_SCLK_PIN, DISPLAY_SPI_MOSI_PIN,
                                    DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // GC9A01初始化
    void InitializeGc9a01Display() {
        ESP_LOGI(TAG, "Init GC9A01 display");
        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_handle_t io_handle = NULL;
        esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, 0, NULL);
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

        ESP_LOGI(TAG, "Install GC9A01 panel driver");
        esp_lcd_panel_handle_t panel_handle = NULL;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_SPI_RESET_PIN;    // Set to -1 if not use
        panel_config.rgb_endian = LCD_RGB_ENDIAN_BGR;           //LCD_RGB_ENDIAN_RGB;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
        panel_ = panel_handle;
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

        uint8_t data_0x62[] = { 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70 };
        esp_lcd_panel_io_tx_param(io_handle, 0x62, data_0x62, sizeof(data_0x62));

        uint8_t data_0x63[] = { 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70 };
        esp_lcd_panel_io_tx_param(io_handle, 0x63, data_0x63, sizeof(data_0x63));

        uint8_t data_0x36[] = { 0x48};
        esp_lcd_panel_io_tx_param(io_handle, 0x36, data_0x36, sizeof(data_0x36));

        // uint8_t data_0x74[] = { 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00};
        // esp_lcd_panel_io_tx_param(io_handle, 0x74, data_0x74, sizeof(data_0x74));

        uint8_t data_0xC3[] = { 0x1F};
        esp_lcd_panel_io_tx_param(io_handle, 0xC3, data_0xC3, sizeof(data_0xC3));

        uint8_t data_0xC4[] = { 0x1F};
        esp_lcd_panel_io_tx_param(io_handle, 0xC4, data_0xC4, sizeof(data_0xC4));

        display_ = new CustomLcdDisplay(io_handle, panel_handle,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    Spotpear_ESP32_S3_1_28_BOX() : boot_button_(BOOT_BUTTON_GPIO) {
        // 先初始化触摸的I2C并探测/初始化触摸（若无触摸则跳过）
        InitializeCodecI2c_Touch();
        InitializeCst816DTouchPad();

        // 初始化音频I2C
        InitializeCodecI2c();

        // 显示相关先建立起来
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeButtons();
        if (GetBacklight()) {
            GetBacklight()->RestoreBrightness();
        }

        // 显示和背光可用后再初始化省电逻辑，避免空指针
        InitializePowerSaveTimer();
        InitializePowerManager();
    }

    ~Spotpear_ESP32_S3_1_28_BOX() {
        if (touchpad_timer_) {
            esp_timer_stop(touchpad_timer_);
            esp_timer_delete(touchpad_timer_);
            touchpad_timer_ = nullptr;
        }
        if (cst816d_) {
            delete cst816d_;
            cst816d_ = nullptr;
        }
        if (power_save_timer_) {
            delete power_save_timer_;
            power_save_timer_ = nullptr;
        }
        if (power_manager_) {
            delete power_manager_;
            power_manager_ = nullptr;
        }
        if (display_) {
            delete display_;
            display_ = nullptr;
        }
        if (i2c_bus_) {
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
        }
        if (codec_i2c_bus_) {
            i2c_del_master_bus(codec_i2c_bus_);
            codec_i2c_bus_ = nullptr;
        }
    }


    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    Cst816d* GetTouchpad() {
        return cst816d_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!power_manager_) {
            level = 0;
            charging = false;
            discharging = true;
            return false;
        }
        
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(Spotpear_ESP32_S3_1_28_BOX);
