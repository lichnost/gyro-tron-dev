#include "engine/engine.h"

#include <cmath>

#include "board/board.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace wc {

static const char *TAG = "engine";

int64_t Engine::nowUs() const { return esp_timer_get_time(); }

bool Engine::waitFrameAfter(uint32_t since, int timeout_ms) const
{
    for (int i = 0; i < timeout_ms / 5 && frame_ <= since; i++) vTaskDelay(pdMS_TO_TICKS(5));
    return frame_ > since;
}

bool Engine::init(const EngineConfig &cfg)
{
    if (board_init() != ESP_OK) return false;
    if (display_init(cfg.spi_hz) != ESP_OK) return false;
    if (!gfx_.init()) return false;
    if (!presenter_.init(cfg.present_buffers)) return false;
    presenter_.setVsync(cfg.vsync);

    // Push the black frame before lighting the panel so there's no garbage flash.
    gfx_.markAllDirty();
    presenter_.present(gfx_);
    presenter_.flush();
    display_set_brightness(cfg.brightness);

    if (!input_.init()) return false;
    ESP_LOGI(TAG, "ready");
    return true;
}

// The game task's stack buffer and TCB, kept at file scope: the frame loop checks the
// canary at the buffer's bottom every frame, so it needs the address the heap handed out.
static StackType_t *s_stack = nullptr;
static StaticTask_t s_game_tcb;

void Engine::run(Game &game)
{
    game_ = &game;
    // The 16 KB stack comes out of internal RAM, which Wi-Fi has been eating into since
    // boot - a quiet exhaustion here shows up as a watch that answers every link and
    // network request but sits on a black screen forever, logging nothing, because the
    // task that draws never existed. So try, and when memory is momentarily too
    // fragmented, let the boot settle (the splash task's stack frees about now) and try
    // again; if it still cannot fit, restart loudly - a counted bad boot that three in a
    // row land the console in safe mode, not a silent brick.
    //
    // The stack is heap-allocated rather than a static .bss array: .bss sits in internal
    // RAM on the ESP32-S3 no less than the heap does, and a stack reserved outright there
    // costs Wi-Fi its headroom (~49 KB free against an init that wants more, even before
    // any game runs). Games' own stack needs fit in 16 KB.
    //
    // The buffer is allocated here rather than handed to xTaskCreatePinnedToCore so the
    // engine knows its address: the frame loop checks the canary at its bottom every
    // frame, because the FreeRTOS canary is only tested at context switch, and a stack
    // overflow may corrupt the stack without ever context-switching before the panic.
    constexpr size_t kStackWords = 16384 / sizeof(StackType_t);
    for (int attempt = 1;; attempt++) {
        s_stack = static_cast<StackType_t *>(
            heap_caps_malloc(kStackWords * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (s_stack) break;
        ESP_LOGE(TAG, "game task: no room, attempt %d (largest internal block %u)", attempt,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (attempt >= 5) {
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    xTaskCreateStaticPinnedToCore(loopEntry, "game", kStackWords, this, configMAX_PRIORITIES - 4, s_stack,
                                  &s_game_tcb, 1);
}

// The two 0xa5 fill words at the very bottom of the stack: the last bytes a downward-
// growing stack consumes. Reading them costs two word loads per frame and catches an
// overflow one frame after it happens, context switch or not. A miss is not survivable
// quietly - the bytes below the buffer are someone else's heap headers - so it aborts,
// and the panic path writes the core dump that names the offender.
static void checkStackCanary()
{
    // StackType_t is a byte-wide type on Xtensa; the fill word is compared as 32 bits.
    const uint32_t *bottom = reinterpret_cast<const uint32_t *>(s_stack);
    if (bottom[0] != 0xa5a5a5a5U || bottom[1] != 0xa5a5a5a5U) {
        ESP_LOGE(TAG, "game task stack canary is gone - aborting for a core dump "
                      "(high water %u bytes left)",
                 unsigned(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
        abort();   // panic path: core dump to the partition, crash banner on the next boot
    }
}

void Engine::loopEntry(void *arg) { static_cast<Engine *>(arg)->loop(); }

// PWR held across a sleep or wake transition would be re-consumed as input (or wake the
// console straight back up), so each transition waits out the press. Bounded, because a
// stuck line must fall through to its caller's fallback rather than wedge the console.
static void waitPwrReleased(int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 10 && (buttons_read() & ::BTN_PWR); i++) vTaskDelay(pdMS_TO_TICKS(10));
}

void Engine::activate(Game &app)
{
    if (game_ && game_ != &app) {
        // How close the outgoing app came to its stack floor: a chronic near-miss shows
        // up here, in the log, instead of waiting for the crash that proves it.
        ESP_LOGI(TAG, "app out: stack high water %u bytes",
                 unsigned(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
        game_->leave(*this);
    }
    game_ = &app;
    gfx_.clear(colors::black);
    if (!app.begun_) {
        app.begun_ = true;
        app.begin(*this);
    }
    app.enter(*this);
}

void Engine::irisOut()
{
    // Black closes in from the rim to a point, like a watch lid shutting.
    constexpr int kSteps = 9;
    int prev_r = Gfx::R + 1;
    for (int k = 1; k <= kSteps; k++) {
        const float t = float(k) / kSteps;
        const int r = int(Gfx::R * (1.0f - t * t));
        for (int y = 0; y < Gfx::H; y++) {
            const int dy = y - Gfx::CY;
            if (dy * dy >= prev_r * prev_r) {
                gfx_.fillRect(0, y, Gfx::W, 1, colors::black);
                continue;
            }
            const int outer = int(std::sqrt(float(prev_r * prev_r - dy * dy)));
            const int inner = dy * dy < r * r ? int(std::sqrt(float(r * r - dy * dy))) : 0;
            gfx_.fillRect(Gfx::CX - outer - 1, y, outer - inner + 1, 1, colors::black);
            gfx_.fillRect(Gfx::CX + inner, y, outer - inner + 1, 1, colors::black);
        }
        prev_r = r;
        presenter_.present(gfx_);
    }
    presenter_.flush();
}

// Everything off except the chip: Wi-Fi paused, input task idle, panel, touch and IMU asleep.
void Engine::quiesce()
{
    if (game_) game_->leave(*this);   // enter() runs again on wake
    input_.pause(true);
    display_sleep(true);
    touch_sleep(true);
    imu_sleep(true);
    // Don't wake instantly on the click that asked for sleep.
    waitPwrReleased(1000);
}

void Engine::powerOff()
{
    // Deep sleep draws next to nothing; PWR cold-boots the console into the launcher.
    // If PWR still reads pressed we'd wake instantly and boot in a loop, so wait for
    // it to settle and fall back to staying asleep (light sleep) if it never does.
    waitPwrReleased(3000);
    if (buttons_read() & ::BTN_PWR) {
        ESP_LOGW(TAG, "PWR line still high; staying in light sleep instead of powering off");
        gpio_wakeup_enable(PIN_KEY_PWR, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
        esp_light_sleep_start();
        gpio_wakeup_disable(PIN_KEY_PWR);
        esp_restart();
    }
    rtc_gpio_pulldown_en(PIN_KEY_PWR);   // don't let a floating pin wake us
    rtc_gpio_pullup_dis(PIN_KEY_PWR);
    esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_KEY_PWR, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_deep_sleep_start();
}

void Engine::doShutDown()
{
    ESP_LOGI(TAG, "shut down");
    quiesce();
    if (before_sleep_) before_sleep_();
    // The power chip lets go of everything, and that is the end of this function. If it does
    // not - it could not be reached, or it declines with a cable in - deep sleep is the next
    // best thing, and looks the same from outside: dark, and PWR starts it from cold.
    pmu_power_off();
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG, "still powered; deep sleep instead");
    powerOff();
}

void Engine::doSleep()
{
    ESP_LOGI(TAG, "sleep");
    irisOut();
    quiesce();

    // On USB power, nap rather than sleep. Light sleep does not hold with a cable in: the
    // chip comes straight back out (measured: "sleep", then "wake" 354 ms later), so the
    // screen of a watch left on its charger overnight never went dark - on an AMOLED, the
    // one place that matters. It would also drop the USB port under the manager app, and
    // there is no battery to save. So: the panel and the motion sensor go off, which is
    // everything the eye and the panel care about, and the loop below waits for PWR.
    // Pull the cable and it carries on into the real thing.
    wake_requested_ = false;
    bool napped = false;
    while (pmu_usb_power() && !wake_requested_ && !(buttons_read() & ::BTN_PWR)) {
        if (!napped) ESP_LOGI(TAG, "on USB power: screen off, everything else left running");
        napped = true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    const bool woken_from_nap = napped && (wake_requested_ || (buttons_read() & ::BTN_PWR));
    wake_requested_ = false;

    bool suspended = false;
    if (!woken_from_nap) {
        if (before_sleep_) before_sleep_();
        suspended = true;
        // Stage 1: light sleep. RAM is kept, so a PWR press resumes instantly.
        gpio_wakeup_enable(PIN_KEY_PWR, GPIO_INTR_HIGH_LEVEL);
        esp_sleep_enable_gpio_wakeup();
        if (auto_off_s_ > 0) esp_sleep_enable_timer_wakeup(uint64_t(auto_off_s_) * 1000000ULL);
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t slept = esp_light_sleep_start();
        const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        gpio_wakeup_disable(PIN_KEY_PWR);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        // Said every time, because "it woke up by itself" cannot be looked into afterwards
        // without knowing whether the chip refused to sleep or something woke it.
        ESP_LOGI(TAG, "light sleep: %s, woken by cause %d after %lld ms", esp_err_to_name(slept), int(cause),
                 (long long)((esp_timer_get_time() - t0) / 1000));

        if (cause == ESP_SLEEP_WAKEUP_TIMER) {
            // Stage 2: nobody came back, so on to deep sleep - after asking the console to
            // remember what was running, so the PWR press that follows can return to it.
            ESP_LOGI(TAG, "auto off after %d s asleep", auto_off_s_);
            if (hibernate_ && !hibernate_()) ESP_LOGW(TAG, "nothing snapshotted for wake");
            powerOff();
        }
    }
    ESP_LOGI(TAG, "wake");

    touch_sleep(false);
    imu_sleep(false);
    display_sleep(false);
    // The press that woke us shouldn't also act in the app.
    waitPwrReleased(2000);
    input_.pause(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    input_.snapshot(input_state_);
    input_state_ = InputState{};
    if (suspended && after_wake_) after_wake_();

    gfx_.clear(colors::black);
    gfx_.markAllDirty();
    // Sent to sleep from the power menu: wake into what it interrupted, not back into it.
    if (pending_) {
        Game *next = pending_;
        pending_ = nullptr;
        activate(*next);
    } else {
        game_->enter(*this);
    }
    dimmed_ = false;
    pwr_held_since_us_ = 0;
    last_frame_us_ = last_activity_us_ = esp_timer_get_time();
}

void Engine::injectTouch(int x0, int y0, int x1, int y1, int ms)
{
    inj_.x0 = x0, inj_.y0 = y0, inj_.x1 = x1, inj_.y1 = y1;
    inj_.touch_t0 = esp_timer_get_time();
    inj_.touch_t1 = inj_.touch_t0 + int64_t(std::max(ms, 40)) * 1000;
}

void Engine::injectButton(uint32_t mask, int ms)
{
    inj_.btn_mask = mask;
    inj_.btn_until = esp_timer_get_time() + int64_t(std::max(ms, 40)) * 1000;
}

// Long enough for a slow step of a test script - a full-screen screenshot over a weak
// link can take the better part of a minute - and short enough that a watch left with a
// frozen gravity heals itself long before anyone picks it up and wonders why tilt died.
static constexpr int64_t kInjectedTiltUs = 60 * 1000000LL;

void Engine::injectTilt(float ax, float ay, float az)
{
    inj_.tilt_until = std::isnan(ax) ? 0 : esp_timer_get_time() + kInjectedTiltUs;
    inj_.ax = ax, inj_.ay = ay, inj_.az = az;
}

void Engine::applyInjected(int64_t now)
{
    InputState &in = input_state_;
    const bool touching = now < inj_.touch_t1;
    if (touching || inj_.touch_was) {
        const float t = std::min(1.0f, float(now - inj_.touch_t0) / float(std::max<int64_t>(1, inj_.touch_t1 - inj_.touch_t0)));
        in.touch.x = int(inj_.x0 + (inj_.x1 - inj_.x0) * t);
        in.touch.y = int(inj_.y0 + (inj_.y1 - inj_.y0) * t);
        in.touch.t_us = now;
        in.touch.pressed = touching && !inj_.touch_was;
        in.touch.released = !touching && inj_.touch_was;
        in.touch.down = touching;
        inj_.touch_was = touching;
    }
    const bool holding = now < inj_.btn_until;
    if (holding || inj_.btn_was) {
        if (holding) in.held |= inj_.btn_mask;
        if (holding && !inj_.btn_was) in.pressed |= inj_.btn_mask;
        if (!holding && inj_.btn_was) in.released |= inj_.btn_mask, in.clicked |= inj_.btn_mask;
        inj_.btn_was = holding;
    }
    if (now < inj_.tilt_until) {
        in.tilt.ax = inj_.ax, in.tilt.ay = inj_.ay, in.tilt.az = inj_.az;
    }
}

bool Engine::sawActivity() const
{
    const InputState &in = input_state_;
    if (in.pressed || in.released || in.held || in.touch.down || in.touch.released) return true;
    // Deliberate movement (turning the watch to steer) counts; resting in a hand doesn't.
    const Tilt &t = in.tilt;
    return t.gx * t.gx + t.gy * t.gy + t.gz * t.gz > 30.0f * 30.0f;
}

void Engine::loop()
{
    Game *first = game_;
    game_ = nullptr;
    activate(*first);
    last_frame_us_ = last_activity_us_ = esp_timer_get_time();
    int64_t fps_t0 = last_frame_us_;
    uint32_t fps_frames0 = presenter_.stats().frames;

    for (;;) {
        checkStackCanary();
        const int64_t t0 = esp_timer_get_time();
        float dt = float(t0 - last_frame_us_) / 1e6f;
        if (dt > 0.1f) dt = 0.1f;   // don't explode physics after a stall
        last_frame_us_ = t0;

        input_.snapshot(input_state_);
        applyInjected(t0);

        // Auto off when nobody's using it: dim for the last 10 s, then power down.
        const bool busy = game_->keepAwake() || (keep_awake_ && keep_awake_());
        if (sawActivity() || busy) last_activity_us_ = t0;
        if (auto_off_s_ > 0) {
            const int64_t idle = t0 - last_activity_us_;
            const bool warn = idle > int64_t(auto_off_s_ - 10) * 1000000;
            if (warn != dimmed_) {
                display_dim(warn);
                dimmed_ = warn;
            }
            if (idle > int64_t(auto_off_s_) * 1000000) {
                ESP_LOGI(TAG, "auto off after %d s idle", auto_off_s_);
                irisOut();
                quiesce();
                if (before_sleep_) before_sleep_();
                if (hibernate_ && !hibernate_()) ESP_LOGW(TAG, "nothing snapshotted for wake");
                powerOff();
            }
        } else if (dimmed_) {
            display_dim(false);
            dimmed_ = false;
        }

        // Console-wide: a long hold of PWR is the power menu.
        if (!(input_state_.held & BTN_B)) {
            pwr_held_since_us_ = 0;
        } else if (!pwr_held_since_us_) {
            pwr_held_since_us_ = t0;
        } else if (power_menu_ && game_ != power_menu_ && !pending_ &&
                   t0 - pwr_held_since_us_ >= int64_t(POWER_PEEK_S * 1e6f)) {
            before_power_ = game_;
            pending_ = power_menu_;
        }
        // Console-wide: BOOT (the small key by the USB port) goes back to the home menu.
        if ((input_state_.pressed & BTN_A) && home_ && game_ != home_) pending_ = home_;
        if (pending_) {
            Game *next = pending_;
            pending_ = nullptr;
            activate(*next);
            input_state_ = InputState{};   // don't leak this frame's taps into the new app
        }

        if (redraw_req_) {
            redraw_req_ = false;
            game_->redraw();
        }
        game_->update(*this, dt);
        if (restart_requested_) {
            ESP_LOGI(TAG, "restart");
            quiesce();
            if (before_sleep_) before_sleep_();
            esp_restart();
        }
        if (shutdown_requested_) doShutDown();
        if (sleep_requested_) {
            sleep_requested_ = false;
            // Never drop the USB connection out from under a file copy.
            if (busy_ && busy_()) ESP_LOGW(TAG, "sleep refused: a transfer is in progress");
            else doSleep();
            continue;
        }
        if (pending_) continue;   // app asked to switch; skip drawing a stale frame
        const int64_t t1 = esp_timer_get_time();
        game_->draw(*this, gfx_);
        const int64_t t2 = esp_timer_get_time();
        presenter_.present(gfx_);
        const int64_t t3 = esp_timer_get_time();

        stats_.update_us = uint32_t(t1 - t0);
        stats_.draw_us = uint32_t(t2 - t1);
        stats_.frame_us = uint32_t(t3 - t0);
        frame_++;
        if (t3 - fps_t0 >= 500000) {
            // Frames that actually reached the panel, not loop iterations.
            const uint32_t shown = presenter_.stats().frames;
            stats_.fps = (shown - fps_frames0) * 1e6f / float(t3 - fps_t0);
            fps_frames0 = shown;
            fps_t0 = t3;
        }

        // Nothing changed on screen: sleep until just after the next vblank so
        // update() keeps a steady ~60 Hz cadence instead of spinning the core.
        if (presenter_.stats().last_bytes == 0) {
            const int64_t next = display_last_vsync_us() + 16667;
            const int64_t ms = (next - esp_timer_get_time()) / 1000;
            vTaskDelay(pdMS_TO_TICKS(ms > 0 && ms < 17 ? ms + 1 : 1));
        }
    }
}

}  // namespace wc
