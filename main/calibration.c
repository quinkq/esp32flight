/*
 * The weather station's calibration (calibration.h; "Settings" in
 * DISPLAY.md, in the planning notes).
 *
 *  - Offsets for the outdoor SHT40 and the indoor sensor; none for the
 *    BME280's humidity, which isn't shown (open decision #6).
 *  - Pressure at sea level with the usual barometric formula, from the
 *    station's altitude and the outdoor temperature:
 *    p0 = p * (1 - 0.0065 h / (T + 0.0065 h + 273.15)) ^ -5.257
 */
#include "calibration.h"

#include <math.h>
#include "altitude.h"
#include "ws_settings.h"

void calibration_get(calibration_t *c)
{
    ws_settings_t cfg;
    ws_settings_get(&cfg);
    *c = (calibration_t){
        .out_t = cfg.cal_out_t,
        .out_rh = cfg.cal_out_rh,
        .in_t = cfg.cal_in_t,
        .in_rh = cfg.cal_in_rh,
        .bat_trim_pm = cfg.bat_trim_pm,
    };
    c->have_alt = altitude_m(&cfg, &c->alt_m);
}

static uint16_t humidity(int32_t rh)
{
    return (uint16_t)(rh < 0 ? 0 : rh > 10000 ? 10000 : rh);
}

void calibration_apply(const calibration_t *c, readings_slot_t *s)
{
    if (s->flags & READINGS_SHT_OK) {
        s->sht_t = (int16_t)(s->sht_t + c->out_t);
        s->sht_rh = humidity((int32_t)s->sht_rh + c->out_rh);
    }
    if (s->flags & READINGS_INDOOR_OK) {
        calibration_indoor(c, &s->in_t, &s->in_rh);
    }
    if (s->bat_mv != WS_LINK_NO_U16) {
        const int32_t mv = ((int32_t)s->bat_mv * (1000 + c->bat_trim_pm) + 500) / 1000;
        s->bat_mv = (uint16_t)(mv < 0 ? 0 : mv >= WS_LINK_NO_U16 ? WS_LINK_NO_U16 - 1 : mv);
    }
    if ((s->flags & READINGS_BME_OK) && c->have_alt) {
        /* the outdoor temperature: the SHT40's (already corrected), else the BME280's */
        const double t = (s->flags & READINGS_SHT_OK) ? s->sht_t / 100.0 : s->bme_t / 100.0;
        const double h = c->alt_m;
        const double p0 = s->bme_p / 10.0 * pow(1.0 - 0.0065 * h / (t + 0.0065 * h + 273.15), -5.257);
        s->bme_p = (uint16_t)lround(p0 * 10.0);
    }
}

void calibration_indoor(const calibration_t *c, int16_t *t, uint16_t *rh)
{
    *t = (int16_t)(*t + c->in_t);
    *rh = humidity((int32_t)*rh + c->in_rh);
}
