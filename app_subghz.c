/* Copyright (C) 2022-2023 Salvatore Sanfilippo -- All Rights Reserved
 * See the LICENSE file for information about the license. */

#include "app.h"
#include "custom_presets.h"

#include <flipper_format/flipper_format_i.h>

void raw_sampling_worker_start(ProtoViewApp *app);
void raw_sampling_worker_stop(ProtoViewApp *app);

ProtoViewModulation ProtoViewModulations[] = {
    {"OOK 650Khz", FuriHalSubGhzPresetOok650Async, NULL},
    {"OOK 270Khz", FuriHalSubGhzPresetOok270Async, NULL},
    {"2FSK 2.38Khz", FuriHalSubGhzPreset2FSKDev238Async, NULL},
    {"2FSK 47.6Khz", FuriHalSubGhzPreset2FSKDev476Async, NULL},
    {"MSK", FuriHalSubGhzPresetMSK99_97KbAsync, NULL},
    {"GFSK", FuriHalSubGhzPresetGFSK9_99KbAsync, NULL},
    {"TPMS 1 (FSK)", 0, (uint8_t*)protoview_subghz_tpms1_async_regs},
    {"TPMS 2 (FSK)", 0, (uint8_t*)protoview_subghz_tpms2_async_regs},
    {NULL, 0, NULL} /* End of list sentinel. */
};

/* Called after the application initialization in order to setup the
 * subghz system and put it into idle state. If the user wants to start
 * receiving we will call radio_rx() to start a receiving worker and
 * associated thread. */
void radio_begin(ProtoViewApp* app) {
    furi_assert(app);
    furi_hal_subghz_reset();
    furi_hal_subghz_idle();

    /* The CC1101 preset can be either one of the standard presets, if
     * the modulation "custom" field is NULL, or a custom preset we
     * defined in custom_presets.h. */
    if (ProtoViewModulations[app->modulation].custom == NULL)
        furi_hal_subghz_load_preset(ProtoViewModulations[app->modulation].preset);
    else
        furi_hal_subghz_load_custom_preset(ProtoViewModulations[app->modulation].custom);
    furi_hal_gpio_init(&gpio_cc1101_g0, GpioModeInput, GpioPullNo, GpioSpeedLow);
    app->txrx->txrx_state = TxRxStateIDLE;
}

/* Setup subghz to start receiving using a background worker. */
uint32_t radio_rx(ProtoViewApp* app) {
    furi_assert(app);
    if(!furi_hal_subghz_is_frequency_valid(app->frequency)) {
        furi_crash(TAG" Incorrect RX frequency.");
    }

    if (app->txrx->txrx_state == TxRxStateRx) return app->frequency;

    furi_hal_subghz_idle(); /* Put it into idle state in case it is sleeping. */
    uint32_t value = furi_hal_subghz_set_frequency_and_path(app->frequency);
    FURI_LOG_E(TAG, "Switched to frequency: %lu", value);
    furi_hal_gpio_init(&gpio_cc1101_g0, GpioModeInput, GpioPullNo, GpioSpeedLow);
    furi_hal_subghz_flush_rx();
    furi_hal_subghz_rx();
    if (!app->txrx->debug_direct_sampling) {

        furi_hal_subghz_start_async_rx(subghz_worker_rx_callback,
                                       app->txrx->worker);
        subghz_worker_start(app->txrx->worker);
    } else {
        raw_sampling_worker_start(app);
    }
    app->txrx->txrx_state = TxRxStateRx;
    return value;
}

/* Stop subghz worker (if active), put radio on idle state. */
void radio_rx_end(ProtoViewApp* app) {
    furi_assert(app);
    if (app->txrx->txrx_state == TxRxStateRx) {
        if (!app->txrx->debug_direct_sampling) {
            if(subghz_worker_is_running(app->txrx->worker)) {
                subghz_worker_stop(app->txrx->worker);
                furi_hal_subghz_stop_async_rx();
            }
        } else {
            raw_sampling_worker_stop(app);
        }
    }
    furi_hal_subghz_idle();
    app->txrx->txrx_state = TxRxStateIDLE;
}

/* Put radio on sleep. */
void radio_sleep(ProtoViewApp* app) {
    furi_assert(app);
    if (app->txrx->txrx_state == TxRxStateRx) {
        /* We can't go from having an active RX worker to sleeping.
         * Stop the RX subsystems first. */
        radio_rx_end(app);
    }
    furi_hal_subghz_sleep();
    app->txrx->txrx_state = TxRxStateSleep;
}

/* ============================= Raw sampling mode =============================
 * This is useful only for debugging: in this mode instead of using the
 * subghz thread, we read in a busy loop from the GDO0 pin of the CC1101
 * in order to get exactly what the chip is receiving. Then using the
 * CPU ticks counter we fill the buffer of data with the pulses level
 * and duration. */

int32_t direct_sampling_thread(void *ctx) {
    ProtoViewApp *app = ctx;
    bool last_level = false;
    uint32_t last_change_time = DWT->CYCCNT;

    if (0) while(app->txrx->ds_thread_running) furi_delay_ms(1);

    while(app->txrx->ds_thread_running) {
        uint16_t d[50]; uint8_t l[50];
        for (uint32_t j = 0; j < 500; j++) {
            volatile uint32_t maxloops = 50000;
            while(maxloops-- && app->txrx->ds_thread_running) {
                bool l = furi_hal_gpio_read(&gpio_cc1101_g0);
                if (l != last_level) break;
            }
            if (maxloops == 0) {
                FURI_LOG_E(TAG, "Max loops reached in DS");
                furi_delay_tick(1);
            }
            /* g0 no longer equal to last level. */
            uint32_t now = DWT->CYCCNT;
            uint32_t dur = now - last_change_time;
            dur /= furi_hal_cortex_instructions_per_microsecond();

            raw_samples_add(RawSamples, last_level, dur);
            if (j < 50) {
                l[j] = last_level;
                d[j] = dur;
            }

            last_level = !last_level; /* What g0 is now. */
            last_change_time = now;
            if (!app->txrx->ds_thread_running) break;
        }

        for (uint32_t j = 0; j < 50; j++)
            printf("%d=%u ", (unsigned int)l[j], (unsigned int)d[j]);
        printf("\n");
        furi_delay_ms(50);
    }
    FURI_LOG_E(TAG, "Exiting DS thread");
    return 0;
}

void raw_sampling_worker_start(ProtoViewApp *app) {
    if (app->txrx->ds_thread != NULL) return;
    app->txrx->ds_thread_running = true;
    app->txrx->ds_thread = furi_thread_alloc_ex("ProtoView DS", 2048, direct_sampling_thread, app);
    furi_thread_start(app->txrx->ds_thread);
}

void raw_sampling_worker_stop(ProtoViewApp *app) {
    if (app->txrx->ds_thread == NULL) return;
    app->txrx->ds_thread_running = false;
    furi_thread_join(app->txrx->ds_thread);
    furi_thread_free(app->txrx->ds_thread);
    app->txrx->ds_thread = NULL;
}
