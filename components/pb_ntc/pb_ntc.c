// SPDX-License-Identifier: MIT
// See docs/NTC_CONVERSION.md for the full reverse-engineering derivation.
#include "pb_ntc.h"
#include "pb_board.h"

#include <math.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "nvs.h"
#ifdef CONFIG_PB_HIL_DEVBOARD
#include "freertos/FreeRTOS.h"
#else
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#endif

static const char *TAG = "pb_ntc";

// --- Per-channel calibration offset (shared by both backends) ----------------
// Stored as centi-°C, hard-clamped to ±(PB_NTC_OFFSET_MAX_C*100). Written by the
// HTTP task and read by the control task on every sample, so it is atomic. The
// offset is added to every OK reading BEFORE it feeds the moving-average filter,
// so pb_ntc_read() (instantaneous, used by the cutoffs) and pb_ntc_smoothed_c()
// (display/control) both return the one calibrated value. Raw-count fault
// thresholds are unaffected — calibration cannot touch the fail-closed logic.
#define PB_NTC_NVS_NS       "app_nvs"
#define PB_NTC_KEY_OFF_CH   "ntc_off_ch"    // i32 centi-°C, chamber
#define PB_NTC_KEY_OFF_PTC  "ntc_off_ptc"   // i32 centi-°C, PTC
#define PB_NTC_OFFSET_CENTI_MAX  ((int)(PB_NTC_OFFSET_MAX_C * 100.0f))

static _Atomic int s_offset_centi[2];       // [PB_NTC_CHAMBER], [PB_NTC_PTC]

static float ntc_offset_c(pb_ntc_channel_t ch)
{
    if (ch < PB_NTC_CHAMBER || ch > PB_NTC_PTC) return 0.0f;
    return (float)atomic_load(&s_offset_centi[ch]) / 100.0f;
}

void pb_ntc_load_calibration(void)
{
    int32_t ch_centi = 0, ptc_centi = 0;
    nvs_handle_t h;
    if (nvs_open(PB_NTC_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, PB_NTC_KEY_OFF_CH,  &v) == ESP_OK) ch_centi  = v;
        if (nvs_get_i32(h, PB_NTC_KEY_OFF_PTC, &v) == ESP_OK) ptc_centi = v;
        nvs_close(h);
    }
    // Clamp on load: a corrupt / out-of-range stored value must clamp, never be
    // applied raw. Bound is identical to the setter's ±5 °C.
    if (ch_centi  < -PB_NTC_OFFSET_CENTI_MAX) ch_centi  = -PB_NTC_OFFSET_CENTI_MAX;
    if (ch_centi  >  PB_NTC_OFFSET_CENTI_MAX) ch_centi  =  PB_NTC_OFFSET_CENTI_MAX;
    if (ptc_centi < -PB_NTC_OFFSET_CENTI_MAX) ptc_centi = -PB_NTC_OFFSET_CENTI_MAX;
    if (ptc_centi >  PB_NTC_OFFSET_CENTI_MAX) ptc_centi =  PB_NTC_OFFSET_CENTI_MAX;
    atomic_store(&s_offset_centi[PB_NTC_CHAMBER], (int)ch_centi);
    atomic_store(&s_offset_centi[PB_NTC_PTC],     (int)ptc_centi);
    ESP_LOGI(TAG, "calibration loaded: chamber=%+.2fC ptc=%+.2fC",
             ch_centi / 100.0, ptc_centi / 100.0);
}

esp_err_t pb_ntc_set_offset_c(pb_ntc_channel_t ch, float offset_c)
{
    if (ch < PB_NTC_CHAMBER || ch > PB_NTC_PTC) return ESP_ERR_INVALID_ARG;
    float clamped = pb_ntc_clamp_offset_c(offset_c);          // ±5 °C hard bound
    int centi = (int)lroundf(clamped * 100.0f);
    if (centi < -PB_NTC_OFFSET_CENTI_MAX) centi = -PB_NTC_OFFSET_CENTI_MAX;
    if (centi >  PB_NTC_OFFSET_CENTI_MAX) centi =  PB_NTC_OFFSET_CENTI_MAX;
    atomic_store(&s_offset_centi[ch], centi);
    nvs_handle_t h;
    if (nvs_open(PB_NTC_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, ch == PB_NTC_CHAMBER ? PB_NTC_KEY_OFF_CH
                                            : PB_NTC_KEY_OFF_PTC, centi);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "calibration offset ch%d set to %+.2f C", (int)ch, (double)clamped);
    return ESP_OK;
}

float pb_ntc_get_offset_c(pb_ntc_channel_t ch) { return ntc_offset_c(ch); }

#ifdef CONFIG_PB_HIL_DEVBOARD

static portMUX_TYPE s_hil_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_hil_temp[2];
static pb_ntc_status_t s_hil_status[2];
static pb_ntc_status_t s_last_status[2];
static bool s_ready;

void pb_ntc_hil_reset(void)
{
    taskENTER_CRITICAL(&s_hil_mux);
    for (int i = 0; i < 2; ++i) {
        s_hil_temp[i] = 25.0f;
        s_hil_status[i] = PB_NTC_OK;
        s_last_status[i] = PB_NTC_OK;
    }
    taskEXIT_CRITICAL(&s_hil_mux);
}

void pb_ntc_hil_set(pb_ntc_channel_t ch, pb_ntc_status_t status, float temp_c)
{
    if (ch < PB_NTC_CHAMBER || ch > PB_NTC_PTC) return;
    taskENTER_CRITICAL(&s_hil_mux);
    s_hil_status[ch] = status;
    s_hil_temp[ch] = temp_c;
    s_last_status[ch] = status;
    taskEXIT_CRITICAL(&s_hil_mux);
}

esp_err_t pb_ntc_init(void)
{
    pb_ntc_hil_reset();
    s_ready = true;
    ESP_LOGW(TAG, "HIL dev-board backend: ADC disabled; temperatures are injected");
    return ESP_OK;
}

pb_ntc_status_t pb_ntc_read(pb_ntc_channel_t ch, float *out_c)
{
    if (!s_ready || ch < PB_NTC_CHAMBER || ch > PB_NTC_PTC) {
        if (out_c) *out_c = NAN;
        return PB_NTC_UNINIT;
    }
    taskENTER_CRITICAL(&s_hil_mux);
    pb_ntc_status_t status = s_hil_status[ch];
    float temp_c = status == PB_NTC_OK ? s_hil_temp[ch] : NAN;
    s_last_status[ch] = status;
    taskEXIT_CRITICAL(&s_hil_mux);
    if (status == PB_NTC_OK) temp_c += ntc_offset_c(ch);   // apply calibration
    if (out_c) *out_c = temp_c;
    return status;
}

pb_ntc_status_t pb_ntc_last_status(pb_ntc_channel_t ch)
{
    if (ch < PB_NTC_CHAMBER || ch > PB_NTC_PTC) return PB_NTC_UNINIT;
    taskENTER_CRITICAL(&s_hil_mux);
    pb_ntc_status_t status = s_last_status[ch];
    taskEXIT_CRITICAL(&s_hil_mux);
    return status;
}

float pb_ntc_smoothed_c(pb_ntc_channel_t ch)
{
    if (!s_ready || ch < PB_NTC_CHAMBER || ch > PB_NTC_PTC) return NAN;
    taskENTER_CRITICAL(&s_hil_mux);
    float temp_c = s_hil_status[ch] == PB_NTC_OK ? s_hil_temp[ch] : NAN;
    taskEXIT_CRITICAL(&s_hil_mux);
    if (isfinite(temp_c)) temp_c += ntc_offset_c(ch);   // apply calibration
    return temp_c;
}

#else

// Low-side NTC divider supply K in Rntc = Rref*V/(K-V) = the ~3.3 V rail.
// (The RE report's 0.1 V was wrong; on hardware the pin sits ~1.6 V at ambient.)
// HW check 2026-07-21: reads ~33 C while the room is 25-26 C, which is correct —
// the chamber had been running at 65 C <1 h earlier, so residual heat in the
// enclosure/heater mass keeps the sensor above room air (sensor != room temp).
// So the standard 3.3 V rail is the right constant, no fudge factor. TODO: for a
// rigorous absolute check, compare against a probe co-located with the NTC at a
// steady drying temp.
#define PB_VSUPPLY_V      3.3f
// Raw-count fault thresholds (from stock fcn.4200ca8e).
#define PB_RAW_OPEN_MAX   0xFFD   // raw > this => open / over-range
#define PB_RAW_SHORT_MIN  0x14    // raw <= this => short / under-range
#define PB_AVG_WINDOW     5

// R/T table, reverse-engineered verbatim from DROM @0x3c0e6638.
// Index i corresponds to temperature (PB_RT_TEMP_BASE + i) degrees C.
// Resistance (kOhm) is monotonically decreasing with temperature.
#define PB_RT_TEMP_BASE   12
static const float PB_RT_R_KOHM[] = {
    198.7f,189.4f,180.7f,172.4f,164.5f,157.0f,149.9f,143.2f,136.8f,130.7f, // 12-21
    124.9f,119.4f,114.2f,109.2f,104.5f,100.0f, 95.7f, 91.6f, 87.8f, 84.1f, // 22-31
     80.6f, 77.2f, 74.0f, 70.9f, 68.0f, 65.3f, 62.6f, 60.1f, 57.7f, 55.4f, // 32-41
     53.2f, 51.1f, 49.1f, 47.2f, 45.3f, 43.6f, 41.9f, 40.3f, 38.8f, 37.3f, // 42-51
     35.9f, 34.5f, 33.2f, 32.0f, 30.8f, 29.7f, 28.6f, 27.6f, 26.6f, 25.6f, // 52-61
     24.7f, 23.8f, 23.0f, 22.2f, 21.4f, 20.6f, 19.9f, 19.2f, 18.6f, 17.9f, // 62-71
     17.3f, 16.7f, 16.2f, 15.6f, 15.1f, 14.6f, 14.1f, 13.6f, 13.2f, 12.8f, // 72-81
     12.4f, 12.0f, 11.6f, 11.2f, 10.9f, 10.5f, 10.2f,  9.9f,  9.6f,  9.3f, // 82-91
      9.0f,  8.7f,  8.4f,  8.2f,  7.9f,  7.7f,  7.4f,  7.2f,  7.0f,  6.8f, // 92-101
      6.6f,  6.4f,  6.2f,  6.0f,  5.9f,  5.7f,  5.5f,  5.4f,  5.2f,  5.1f, // 102-111
      4.9f,  4.8f,  4.7f,  4.5f,  4.4f,  4.3f,  4.2f,  4.1f,  3.9f,  3.8f, // 112-121
      3.7f,  3.6f,  3.5f,  3.4f,                                          // 122-125
};
#define PB_RT_N ((int)(sizeof(PB_RT_R_KOHM) / sizeof(PB_RT_R_KOHM[0])))

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali[2];
static int s_rref_kohm;
static bool s_ready;

// moving-average state per channel
static float s_win[2][PB_AVG_WINDOW];
static int   s_win_cnt[2];
static int   s_win_idx[2];
static pb_ntc_status_t s_last_status[2] = { PB_NTC_UNINIT, PB_NTC_UNINIT };

static const adc_channel_t s_chan[2] = { PB_ADC_CH_CHAMBER, PB_ADC_CH_PTC };

// Resistance (kOhm) -> temperature (C), linear interpolation over the R/T table.
// Interpolation is strictly better than the stock nearest-entry and stays <1C of it.
static float rntc_to_temp_c(float r_kohm)
{
    if (r_kohm >= PB_RT_R_KOHM[0])          return (float)PB_RT_TEMP_BASE;           // clamp cold
    if (r_kohm <= PB_RT_R_KOHM[PB_RT_N - 1]) return (float)(PB_RT_TEMP_BASE + PB_RT_N - 1); // clamp hot
    for (int i = 0; i < PB_RT_N - 1; i++) {
        float rhi = PB_RT_R_KOHM[i];        // higher R, lower temp
        float rlo = PB_RT_R_KOHM[i + 1];    // lower R,  higher temp
        if (r_kohm <= rhi && r_kohm >= rlo) {
            float f = (rhi - r_kohm) / (rhi - rlo);   // 0..1
            return (float)(PB_RT_TEMP_BASE + i) + f;  // +1C per index step
        }
    }
    return NAN; // unreachable
}

static float push_average(pb_ntc_channel_t ch, float v)
{
    s_win[ch][s_win_idx[ch]] = v;
    s_win_idx[ch] = (s_win_idx[ch] + 1) % PB_AVG_WINDOW;
    if (s_win_cnt[ch] < PB_AVG_WINDOW) s_win_cnt[ch]++;
    float sum = 0.0f;
    for (int i = 0; i < s_win_cnt[ch]; i++) sum += s_win[ch][i];
    return sum / (float)s_win_cnt[ch];
}

esp_err_t pb_ntc_init(void)
{
    s_rref_kohm = pb_board_rref_kohm();

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = PB_ADC_UNIT };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) return err;

    // 12dB atten matches the stock config; curve-fitting is available on the C3.
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    for (int i = 0; i < 2; i++) {
        err = adc_oneshot_config_channel(s_adc, s_chan[i], &chan_cfg);
        if (err != ESP_OK) return err;

        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = PB_ADC_UNIT,
            .chan = s_chan[i],
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cali init failed on ch%d: %s", i, esp_err_to_name(err));
            return err;
        }
        s_win_cnt[i] = 0;
        s_win_idx[i] = 0;
    }
    s_ready = true;
    ESP_LOGI(TAG, "init ok (Rref=%d kOhm, Vsupply=%.2f V)", s_rref_kohm, PB_VSUPPLY_V);
    return ESP_OK;
}

// Returns the INSTANTANEOUS temperature in *out_c (NAN on any fault) so the
// heater over-temp cutoffs act on the freshest sample, not a lagged average.
// A successful read still feeds the moving-average filter that backs
// pb_ntc_smoothed_c() (used for display/telemetry). The per-channel status is
// latched for pb_ntc_last_status().
pb_ntc_status_t pb_ntc_read(pb_ntc_channel_t ch, float *out_c)
{
    pb_ntc_status_t st = PB_NTC_UNINIT;
    float t = NAN;

    do {
        if (!s_ready) { st = PB_NTC_UNINIT; break; }
        int raw = 0;
        if (adc_oneshot_read(s_adc, s_chan[ch], &raw) != ESP_OK) { st = PB_NTC_UNINIT; break; }
        int mv = 0;
        if (adc_cali_raw_to_voltage(s_cali[ch], raw, &mv) != ESP_OK) { st = PB_NTC_UNINIT; break; }
        if (raw > PB_RAW_OPEN_MAX)  { st = PB_NTC_OPEN;  break; }
        if (raw <= PB_RAW_SHORT_MIN) { st = PB_NTC_SHORT; break; }
        float v = (float)mv / 1000.0f;               // volts at the pin
        if (v <= 0.0f || v >= PB_VSUPPLY_V) { st = PB_NTC_OPEN; break; }
        float r_kohm = (float)s_rref_kohm * v / (PB_VSUPPLY_V - v);
        t = rntc_to_temp_c(r_kohm);
        t += ntc_offset_c(ch);                        // apply calibration offset
        push_average(ch, t);                          // feed the display filter
        st = PB_NTC_OK;
    } while (0);

    s_last_status[ch] = st;
    if (out_c) *out_c = t;                            // instantaneous (NAN on fault)
    return st;
}

pb_ntc_status_t pb_ntc_last_status(pb_ntc_channel_t ch) { return s_last_status[ch]; }

float pb_ntc_smoothed_c(pb_ntc_channel_t ch)
{
    if (!s_ready || s_win_cnt[ch] == 0) return NAN;
    float sum = 0.0f;
    for (int i = 0; i < s_win_cnt[ch]; i++) sum += s_win[ch][i];
    return sum / (float)s_win_cnt[ch];
}

#endif
