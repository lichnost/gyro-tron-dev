// The tilt calibration's storage, split out of console.cpp so the emulator can run the
// whole calibration flow without the console's heavier machinery (crash dump, USB,
// brightness). One key in the NVS namespace "console": "tiltcal", the calibration record. See
// docs/reference/CALIBRATION.md.

#include "console/console.h"

#include <cstdio>

#include "engine/autocal.h"
#include "engine/calib_fit.h"
#include "engine/input.h"
#include "esp_log.h"
#include "nvs.h"

namespace console {

namespace {

const char *TAG = "tiltcal";

bool s_tilt_calibrated = false;

}  // namespace

void loadTiltCalibration()
{
    // The background refresher (engine/autocal.h) saves through the same door the manual
    // flow does, so the NVS layout has exactly one writer's set of rules.
    wc::cal::autocal().setSave(saveTiltCalibration);
    nvs_handle_t h;
    if (nvs_open("console", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no record: namespace missing (never calibrated on this build)");
        return;
    }
    wc::TiltCal cal;
    size_t len = sizeof(cal);
    // No version gate: there is no migration path to keep alive, the sizeof match is the
    // whole check (docs/reference/CALIBRATION.md). One exception on top: a map that
    // unpacks to a rotation (det +1) mirrors in-plane motion - gravity walks the wrong
    // way around a rolling watch. Every real mount is a reflection (det -1: right-handed
    // chip, left-handed screen frame), so records written while the engine believed
    // det +1 - the truncated-sign-bit blobs, and every map the old derive blessed - are
    // discarded and the watch falls back to the guess until recalibrated.
    const esp_err_t got = nvs_get_blob(h, "tiltcal", &cal, &len);
    if (got == ESP_OK && len == sizeof(cal) &&
        (!cal.has_map || wc::cal::axMapDet(wc::cal::unpackAxMap(cal.axmap)) < 0)) {
        wc::Input::setCalibration(cal);
        s_tilt_calibrated = true;
        ESP_LOGI(TAG, "loaded: residual %.2f deg, map %s, refits %u",
                 cal.residual_deg, cal.has_map ? "measured" : "guess", cal.auto_refits);
    } else if (got == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no record: key empty - never calibrated, the flow ended before "
                      "SAVE, or PWR at the calibrate intro cleared it");
    } else if (got == ESP_OK) {
        // The blob predates a TiltCal size change (axmap went 8->9 bits, Nov 2026) or
        // carries a det +1 map from the old derive - either way it is unusable, and the
        // watch runs the guess until recalibrated.
        ESP_LOGW(TAG, "record IGNORED: blob %u bytes vs struct %u bytes, map flag %u",
                 unsigned(len), unsigned(sizeof(cal)), cal.has_map);
    } else {
        ESP_LOGW(TAG, "record unreadable: nvs error %d", int(got));
    }
    nvs_close(h);
}

void saveTiltCalibration(const wc::TiltCal &cal)
{
    wc::Input::setCalibration(cal);
    s_tilt_calibrated = true;
    nvs_handle_t h;
    const esp_err_t opened = nvs_open("console", NVS_READWRITE, &h);
    if (opened != ESP_OK) {
        ESP_LOGE(TAG, "SAVE FAILED: nvs_open %d - the record lives in RAM until reboot", int(opened));
        return;
    }
    const esp_err_t set = nvs_set_blob(h, "tiltcal", &cal, sizeof(cal));
    const esp_err_t commit = set == ESP_OK ? nvs_commit(h) : set;
    nvs_close(h);
    if (commit != ESP_OK)
        ESP_LOGE(TAG, "SAVE FAILED: set %d commit %d - nothing persisted", int(set), int(commit));
    else
        ESP_LOGI(TAG, "saved: residual %.2f deg, map %s", cal.residual_deg,
                 cal.has_map ? "measured" : "guess");
}

void clearTiltCalibration()
{
    wc::Input::setCalibration(wc::TiltCal{});
    s_tilt_calibrated = false;
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "tiltcal");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "record CLEARED (PWR at the calibrate intro) - recalibrate");
}

bool tiltCalibrated() { return s_tilt_calibrated; }

const char *tiltCalibrationLabel()
{
    static char buf[12];
    if (!tiltCalibrated()) return "GO";
    const uint16_t n = wc::Input::calibration().auto_refits;
    if (n == 0) return "DONE";
    snprintf(buf, sizeof(buf), "AUTO %u", n);
    return buf;
}

}  // namespace console
