#include "engine/input.h"

#include "board/board.h"
#include "engine/autocal.h"
#include "engine/calib_fit.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace wc {

namespace {

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_pause_req = false;
volatile bool s_paused = false;
TiltCal s_cal;

struct Shared {
    uint32_t held = 0;
    uint32_t pressed_acc = 0;    // edges accumulated since last snapshot
    uint32_t released_acc = 0;
    uint32_t clicked_acc = 0;     // short press completed
    uint32_t long_acc = 0;        // held past kLongPressUs
    uint32_t double_acc = 0;      // second click within kDoubleClickUs
    bool touch_down = false;
    bool touch_pressed_acc = false;
    bool touch_released_acc = false;
    int tx = 0, ty = 0;
    int64_t t_us = 0;
    Tilt raw;    // board frame, uncorrected - the map and the correction run in snapshot
    float rint[3] = {0, 0, 0};   // board-frame gyro integral since the last snapshot, deg
    float rdt = 0;               // seconds rint spans
    uint32_t touch_hz = 0;
    uint32_t imu_ok = 0, imu_err = 0;
} s;

void input_task(void *)
{
    const TickType_t period = pdMS_TO_TICKS(2);
    TickType_t wake = xTaskGetTickCount();
    uint32_t tick = 0;
    bool was_down = false;
    uint32_t touch_samples = 0;
    int64_t hz_t0 = esp_timer_get_time();
    int64_t last_imu_us = 0;   // timestamp of the previous IMU read, for the integral's dt

    // Debounce: a button edge is accepted immediately (no added latency), then
    // that button ignores further changes for a lockout window so contact
    // chatter can't register as extra presses or releases.
    constexpr int64_t kLockoutUs = 60000;
    constexpr uint32_t kButtons[] = {BTN_A, BTN_B};
    uint32_t debounced = 0;
    int64_t last_edge_us[2] = {0, 0};
    // Long press fires once while still held; a release before that is a click.
    constexpr int64_t kLongPressUs = 600000;
    int64_t press_start_us[2] = {0, 0};
    bool long_fired[2] = {false, false};
    uint32_t clicked = 0, longp = 0, dbl = 0;
    constexpr int64_t kDoubleClickUs = 400000;
    int64_t last_click_us[2] = {0, 0};

    for (;;) {
        if (s_pause_req) {
            s_paused = true;
            vTaskDelay(pdMS_TO_TICKS(5));
            wake = xTaskGetTickCount();
            last_imu_us = 0;   // the pause gap is not motion - the next read starts fresh
            continue;
        }
        s_paused = false;

        // --- buttons (every 2 ms)
        uint32_t raw_bits = buttons_read();
        uint32_t raw = 0;
        if (raw_bits & ::BTN_BOOT) raw |= BTN_A;
        if (raw_bits & ::BTN_PWR) raw |= BTN_B;
        const int64_t now_us = esp_timer_get_time();
        for (int i = 0; i < 2; i++) {
            const uint32_t bit = kButtons[i];
            if ((raw & bit) != (debounced & bit) && now_us - last_edge_us[i] >= kLockoutUs) {
                debounced ^= bit;
                last_edge_us[i] = now_us;
                if (debounced & bit) {
                    press_start_us[i] = now_us;
                    long_fired[i] = false;
                } else if (!long_fired[i]) {
                    clicked |= bit;
                    if (last_click_us[i] && now_us - last_click_us[i] < kDoubleClickUs) {
                        dbl |= bit;
                        last_click_us[i] = 0;
                        ESP_LOGI("input", "%s double-click", i ? "PWR" : "BOOT");
                    } else {
                        ESP_LOGD("input", "%s click, held %d ms, %d ms after the last", i ? "PWR" : "BOOT",
                                 int((now_us - press_start_us[i]) / 1000),
                                 last_click_us[i] ? int((now_us - last_click_us[i]) / 1000) : -1);
                        last_click_us[i] = now_us;
                    }
                }
            }
            if ((debounced & bit) && !long_fired[i] && now_us - press_start_us[i] >= kLongPressUs) {
                long_fired[i] = true;
                longp |= bit;
            }
        }
        uint32_t held = debounced;

        // --- touch: on interrupt, or keep polling while held so we see the release
        touch_point_t tp{};
        bool have_touch = false;
        if (touch_irq_pending() || (was_down && tick % 3 == 0)) {
            have_touch = touch_read(&tp) == ESP_OK;
            if (have_touch && tp.down) touch_samples++;
        }

        // --- IMU (every 4 ms)
        imu_sample_t imu{};
        bool have_imu = false;
        float imu_dt = 0;   // seconds since the previous read; 0 on the first one
        if (tick % 2 == 0) {
            have_imu = imu_read(&imu) == ESP_OK;
            have_imu ? s.imu_ok++ : s.imu_err++;
            if (have_imu) {
                if (last_imu_us) imu_dt = float(now_us - last_imu_us) * 1e-6f;
                last_imu_us = now_us;
            }
        }

        portENTER_CRITICAL(&s_mux);
        s.pressed_acc |= held & ~s.held;
        s.released_acc |= s.held & ~held;
        s.clicked_acc |= clicked;
        s.long_acc |= longp;
        s.double_acc |= dbl;
        clicked = longp = dbl = 0;
        s.held = held;
        if (have_touch) {
            if (tp.down && !s.touch_down) s.touch_pressed_acc = true;
            if (!tp.down && s.touch_down) s.touch_released_acc = true;
            s.touch_down = tp.down;
            if (tp.down) {
                s.tx = tp.x;
                s.ty = tp.y;
                s.t_us = tp.t_us;
            }
        }
        if (have_imu) {
            // Stored raw, in board axes: the screen mapping is data now (a measured one
            // once calibration runs, the old compile-time guess until then), and it is
            // applied where the correction is - see snapshot. The integral rides along
            // with the same treatment, so a filter that turns by the frame rotation
            // (out.tilt.rx) gets exactly what a 250 Hz gyro would have integrated.
            s.raw = {imu.ax, imu.ay, imu.az, imu.gx, imu.gy, imu.gz};
            s.rint[0] += imu.gx * imu_dt;
            s.rint[1] += imu.gy * imu_dt;
            s.rint[2] += imu.gz * imu_dt;
            s.rdt += imu_dt;
        }
        portEXIT_CRITICAL(&s_mux);

        // The background refresher eats the same raw sample, outside the critical
        // section: its still windows and rare refits have nothing to do with the buttons.
        if (have_imu) wc::cal::autocal().feed({imu.ax, imu.ay, imu.az, imu.gx, imu.gy, imu.gz});

        if (have_touch) was_down = tp.down;

        const int64_t now = esp_timer_get_time();
        if (now - hz_t0 >= 1000000) {
            s.touch_hz = touch_samples;
            touch_samples = 0;
            hz_t0 = now;
        }

        tick++;
        xTaskDelayUntil(&wake, period);
    }
}

}  // namespace

bool Input::init()
{
    return xTaskCreatePinnedToCore(input_task, "input", 5120, nullptr, configMAX_PRIORITIES - 3, nullptr, 0) ==
           pdPASS;
}

void Input::pause(bool paused)
{
    s_pause_req = paused;
    if (paused)
        while (!s_paused) vTaskDelay(pdMS_TO_TICKS(2));
    if (paused)
    {   // a rotation accumulated just before the pause must not land in the first frame
        // after it - drop the undelivered integral, the pause gap is not motion
        portENTER_CRITICAL(&s_mux);
        s.rint[0] = s.rint[1] = s.rint[2] = 0;
        s.rdt = 0;
        portEXIT_CRITICAL(&s_mux);
    }
}

void Input::snapshot(InputState &out)
{
    Tilt raw;
    TiltCal cal;
    portENTER_CRITICAL(&s_mux);
    out.held = s.held;
    out.pressed = s.pressed_acc;
    out.released = s.released_acc;
    out.clicked = s.clicked_acc;
    out.long_press = s.long_acc;
    out.double_clicked = s.double_acc;
    out.touch.down = s.touch_down;
    out.touch.pressed = s.touch_pressed_acc;
    out.touch.released = s.touch_released_acc;
    out.touch.x = s.tx;
    out.touch.y = s.ty;
    out.touch.t_us = s.t_us;
    // The map and the correction run here, off the raw sample, so the task stores what
    // the sensor said and nothing else. One call, the same math the emulator feeds its
    // synthetic IMU through. The frame rotation goes through the same pipeline: the map
    // applies to it like to the rate, and the bias comes off over the time the integral
    // really spans - see applyPipeline.
    raw = s.raw;
    raw.rx = s.rint[0];
    raw.ry = s.rint[1];
    raw.rz = s.rint[2];
    raw.rdt = s.rdt;
    s.rint[0] = s.rint[1] = s.rint[2] = 0;
    s.rdt = 0;
    cal = s_cal;
    out.touch_hz = s.touch_hz;
    out.imu_ok = s.imu_ok;
    out.imu_err = s.imu_err;
    s.pressed_acc = s.released_acc = s.clicked_acc = s.long_acc = s.double_acc = 0;
    s.touch_pressed_acc = s.touch_released_acc = false;
    portEXIT_CRITICAL(&s_mux);
    wc::cal::applyPipeline(cal, raw, out.tilt);
}

void Input::setCalibration(const TiltCal &cal)
{
    portENTER_CRITICAL(&s_mux);
    s_cal = cal;
    portEXIT_CRITICAL(&s_mux);
}

TiltCal Input::calibration()
{
    portENTER_CRITICAL(&s_mux);
    const TiltCal cal = s_cal;
    portEXIT_CRITICAL(&s_mux);
    return cal;
}

Tilt Input::rawTilt()
{
    portENTER_CRITICAL(&s_mux);
    const Tilt raw = s.raw;
    portEXIT_CRITICAL(&s_mux);
    return raw;
}

}  // namespace wc
