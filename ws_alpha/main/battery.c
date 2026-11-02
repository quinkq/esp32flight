/*
 * Cell voltage through the divider on GPIO1 (ADC1 channel 1): 469.5 kΩ to
 * the cell, 320.7 kΩ to GND, 100 nF at the pin. A full cell gives 1.70 V
 * at the pin, an empty one ~1.2 V; the 12 dB range goes to ~2.5 V.
 */
#include "battery.h"

#include <stdbool.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "ws_link.h"

static const char *TAG = "battery";

#define ADC_CHAN      ADC_CHANNEL_1
#define ADC_ATTEN     ADC_ATTEN_DB_12
#define R_TOP_OHM     469500
#define R_BOTTOM_OHM  320700
#define SAMPLES       8

#define MV_MIN        2000     /* the C3 can't run on less: the divider is broken */
#define MV_MAX        4500     /* more than a full cell: the divider is broken */

#define MV_LOW        3400
#define MV_EMPTY      3100
#define MV_RECOVERED  3300     /* hysteresis: leave EMPTY only above this */

/* reloaded after any reset but a deep-sleep wake, which is fine: after a
 * reset the mode is simply judged afresh */
static RTC_DATA_ATTR bool s_empty;

uint16_t battery_read_mv(void)
{
    const adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    const adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1, .chan = ADC_CHAN, .atten = ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_unit_handle_t adc = NULL;
    adc_cali_handle_t cali = NULL;
    int pin_mv = -1;

    if (adc_oneshot_new_unit(&unit_cfg, &adc) == ESP_OK &&
        adc_oneshot_config_channel(adc, ADC_CHAN, &chan_cfg) == ESP_OK &&
        adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali) == ESP_OK) {
        int sum = 0, n = 0;
        for (int i = 0; i < SAMPLES; i++) {
            int raw;
            if (adc_oneshot_read(adc, ADC_CHAN, &raw) == ESP_OK) {
                sum += raw;
                n++;
            }
        }
        if (n == 0 || adc_cali_raw_to_voltage(cali, sum / n, &pin_mv) != ESP_OK) {
            pin_mv = -1;
        }
    }
    if (cali) {
        adc_cali_delete_scheme_curve_fitting(cali);
    }
    if (adc) {
        adc_oneshot_del_unit(adc);
    }

    if (pin_mv < 0) {
        ESP_LOGE(TAG, "ADC reading failed");
        return WS_LINK_NO_U16;
    }
    const int mv = (int)((int64_t)pin_mv * (R_TOP_OHM + R_BOTTOM_OHM) / R_BOTTOM_OHM);
    if (mv < MV_MIN || mv > MV_MAX) {
        ESP_LOGE(TAG, "%d mV (pin %d mV) is impossible: divider fault?", mv, pin_mv);
        return WS_LINK_NO_U16;
    }
    return (uint16_t)mv;
}

battery_mode_t battery_mode(uint16_t mv)
{
    if (mv == WS_LINK_NO_U16) {
        return BATTERY_OK;
    }
    if (s_empty ? mv < MV_RECOVERED : mv < MV_EMPTY) {
        s_empty = true;
        return BATTERY_EMPTY;
    }
    s_empty = false;
    return mv < MV_LOW ? BATTERY_LOW : BATTERY_OK;
}
