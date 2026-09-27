/* Official Flipper Wi-Fi Developer Board V1: common-anode RGB LED, active low,
 * R=GPIO6, G=GPIO5, B=GPIO4 (schematic sheet 2). No work on the radio hot path. */
#include "indicator.h"
#include <stdatomic.h>
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static atomic_uint base_state=LED_BOOT, host_state=LED_IDLE, operation=LED_IDLE;
static atomic_uint host_at, error_at, operation_at;
static atomic_bool host_valid, error_valid;
static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time()/1000); }
void indicator_base(LedState state) {
    if (state < LED_STATE_COUNT) { atomic_store(&base_state,state); atomic_store(&host_valid,false); }
}
void indicator_host(LedState state) {
    if (state < LED_STATE_COUNT) {
        atomic_store(&host_state,state); atomic_store(&host_at,millis());
        atomic_store(&host_valid,true);
    }
}
void indicator_operation(LedState state) {
    if (state < LED_STATE_COUNT) { atomic_store(&operation,state); atomic_store(&operation_at,millis()); }
}
void indicator_error(void) { atomic_store(&error_at,millis()); atomic_store(&error_valid,true); }
static void task(void *arg) {
    (void)arg;
    LedState previous=LED_STATE_COUNT;
    uint32_t since=millis();
    for (;;) {
        uint32_t now=millis();
        LedState state=atomic_load(&base_state), op=atomic_load(&operation);
        if (atomic_load(&host_valid) && now-atomic_load(&host_at)<3000) state=atomic_load(&host_state);
        if (op!=LED_IDLE && (op!=LED_BACKUP || now-atomic_load(&operation_at)<3000)) state=op;
        if (atomic_load(&error_valid) && now-atomic_load(&error_at)<4200) state=LED_ERROR;
        if (state!=previous) { previous=state; since=now; }
        LedColor rgb=indicator_pattern(state,now-since);
        const uint8_t levels[]={rgb.red,rgb.green,rgb.blue};
        for (unsigned i=0;i<3;++i) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE,i,levels[i]);
            ledc_update_duty(LEDC_LOW_SPEED_MODE,i);
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
bool indicator_init(void) {
    ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,.duty_resolution=LEDC_TIMER_8_BIT,
        .timer_num=LEDC_TIMER_0,.freq_hz=4000,.clk_cfg=LEDC_AUTO_CLK};
    if (ledc_timer_config(&timer)!=ESP_OK) return false;
    const int pins[]={6,5,4};
    for (unsigned i=0;i<3;++i) {
        ledc_channel_config_t channel={.gpio_num=pins[i],.speed_mode=LEDC_LOW_SPEED_MODE,
            .channel=i,.intr_type=LEDC_INTR_DISABLE,.timer_sel=LEDC_TIMER_0,.duty=0,
            .hpoint=0,.flags.output_invert=1};
        if (ledc_channel_config(&channel)!=ESP_OK) return false;
    }
    return xTaskCreate(task,"indicator",2048,NULL,1,NULL)==pdPASS;
}
