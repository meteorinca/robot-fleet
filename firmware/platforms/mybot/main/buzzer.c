#include "buzzer.h"
#include "board_config.h"

#ifdef BUZZER_PIN

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/ledc.h"
#include "esp_log.h"

#define BUZZER_TIMER        LEDC_TIMER_2
#define BUZZER_MODE         LEDC_LOW_SPEED_MODE
#define BUZZER_CHANNEL      LEDC_CHANNEL_3
#define BUZZER_DUTY_RES     LEDC_TIMER_13_BIT
#define BUZZER_DUTY         (4095) // 50% of 2^13-1

typedef struct {
    uint32_t freq_hz;
    uint32_t duration_ms;
} buzzer_note_t;

static QueueHandle_t s_buzzer_queue = NULL;

static void buzzer_task(void *arg) {
    buzzer_note_t note;
    while (1) {
        if (xQueueReceive(s_buzzer_queue, &note, portMAX_DELAY)) {
            if (note.freq_hz > 0) {
                ledc_set_freq(BUZZER_MODE, BUZZER_TIMER, note.freq_hz);
                ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, BUZZER_DUTY);
                ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
            } else {
                ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 0);
                ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
            }
            vTaskDelay(pdMS_TO_TICKS(note.duration_ms));
            
            // Turn off buzzer immediately after note finishes (to create gaps if needed or stop entirely)
            ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 0);
            ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
        }
    }
}

void buzzer_init(void) {
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = BUZZER_MODE,
        .timer_num        = BUZZER_TIMER,
        .duty_resolution  = BUZZER_DUTY_RES,
        .freq_hz          = 1000,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = BUZZER_MODE,
        .channel        = BUZZER_CHANNEL,
        .timer_sel      = BUZZER_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = BUZZER_PIN,
        .duty           = 0,
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);

    s_buzzer_queue = xQueueCreate(60, sizeof(buzzer_note_t));
    xTaskCreate(buzzer_task, "buzzer", 2048, NULL, 5, NULL);
    ESP_LOGI("BUZZER", "Buzzer initialized on GPIO %d", BUZZER_PIN);
}

void buzzer_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (!s_buzzer_queue) return;
    buzzer_note_t note = { .freq_hz = freq_hz, .duration_ms = duration_ms };
    xQueueSend(s_buzzer_queue, &note, 0);
}

static void buzzer_clear(void) {
    if (!s_buzzer_queue) return;
    xQueueReset(s_buzzer_queue);
}

void buzzer_demo_coin(void) {
    buzzer_clear();
    buzzer_play_tone(988, 100);
    buzzer_play_tone(1319, 400);
}

void buzzer_demo_gameover(void) {
    buzzer_clear();
    buzzer_play_tone(440, 200);
    buzzer_play_tone(415, 200);
    buzzer_play_tone(392, 200);
    buzzer_play_tone(370, 600);
}

void buzzer_demo_siren(void) {
    buzzer_clear();
    for (int i=0; i<3; i++) {
        buzzer_play_tone(600, 300);
        buzzer_play_tone(800, 300);
    }
}

void buzzer_demo_laser(void) {
    buzzer_clear();
    for (int i = 2000; i > 500; i -= 100) {
        buzzer_play_tone(i, 10);
    }
}

void buzzer_demo_startup(void) {
    buzzer_clear();
    buzzer_play_tone(523, 100);
    buzzer_play_tone(659, 100);
    buzzer_play_tone(784, 100);
    buzzer_play_tone(1046, 200);
}

void buzzer_demo_wifi_connected(void) {
    buzzer_clear();
    buzzer_play_tone(1046, 100);
    buzzer_play_tone(1319, 100);
    buzzer_play_tone(1568, 200);
}

void buzzer_demo_xfiles(void) {
    buzzer_clear();
    // Melody 1
    buzzer_play_tone(880, 300);
    buzzer_play_tone(1319, 300);
    buzzer_play_tone(1175, 300);
    buzzer_play_tone(1319, 300);
    buzzer_play_tone(1568, 300);
    buzzer_play_tone(1319, 800);
    
    // pause
    buzzer_play_tone(0, 200);
    
    // Melody 2
    buzzer_play_tone(880, 300);
    buzzer_play_tone(1319, 300);
    buzzer_play_tone(1175, 300);
    buzzer_play_tone(1319, 300);
    buzzer_play_tone(1760, 300);
    buzzer_play_tone(1319, 800);
}

void buzzer_demo_mario(void) {
    buzzer_clear();
    int notes[] = {
        659, 659, 0, 659, 0, 523, 659, 0, 784, 0, 0, 0, 392, 0, 0, 0,
        523, 0, 0, 392, 0, 0, 330, 0, 0, 440, 0, 494, 0, 466, 440, 0,
        392, 659, 784, 880, 0, 698, 784, 0, 659, 0, 523, 587, 494, 0
    };
    int lengths[] = {
        150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150,
        150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150,
        200, 200, 200, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150, 150
    };
    for(int i = 0; i < sizeof(notes)/sizeof(notes[0]); i++) {
        buzzer_play_tone(notes[i], lengths[i]);
    }
}

void buzzer_demo_1up(void) {
    buzzer_clear();
    buzzer_play_tone(1319, 100);
    buzzer_play_tone(1568, 100);
    buzzer_play_tone(2637, 100);
    buzzer_play_tone(2093, 100);
    buzzer_play_tone(2349, 100);
    buzzer_play_tone(3136, 400);
}

#else
void buzzer_init(void) {}
void buzzer_play_tone(uint32_t freq_hz, uint32_t duration_ms) {}
void buzzer_demo_coin(void) {}
void buzzer_demo_gameover(void) {}
void buzzer_demo_siren(void) {}
void buzzer_demo_laser(void) {}
void buzzer_demo_startup(void) {}
void buzzer_demo_wifi_connected(void) {}
void buzzer_demo_xfiles(void) {}
void buzzer_demo_mario(void) {}
void buzzer_demo_1up(void) {}
#endif
