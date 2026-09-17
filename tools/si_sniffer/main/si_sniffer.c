/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A listen-only GameCube SI capture, streamed live over the serial probe.
 *
 * Purpose built for reverse engineering the GameCube to GBA link. It is not
 * BlueRetro with the adapter parts switched off - it is its own firmware, and
 * that is the point:
 *
 *   - Nothing to run out of. BlueRetro's IRAM is 99.75% full and its capture
 *     ends up in a 128kB memory card buffer that has to be downloaded through
 *     the companion app. Here there is no Bluetooth stack, no memory card and
 *     no vendor opcodes, so the capture streams out of the UART for as long as
 *     you care to leave it running.
 *
 *   - It records what was on the wire, not what a decoder made of it. Earlier
 *     captures stored decoded bytes, which threw away the one thing needed to
 *     tell a real frame from a misaligned one. This sends the RMT's own pulse
 *     durations.
 *
 *   - It physically cannot talk back. The RMT output is never routed to the
 *     pin at all, so there is no code path, however wrong, that transmits onto
 *     a bus somebody else is using.
 *
 * The pin also gets its internal pull-up, which holds the line high when
 * nothing is driving it. An undriven input floats and reads as noise at well
 * over a kilohertz, which is what swamped the first attempt at this.
 */

#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_intr_alloc.h>
#include <esp_log.h>
#include <esp_rom_gpio.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <soc/rmt_struct.h>
#include <soc/gpio_sig_map.h>
#include <soc/periph_defs.h>
#include <hal/clk_gate_ll.h>

/* Which console port to listen to, zero based: 0 is the port marked 1. The
 * pins are BlueRetro hw2's, same order. */
#ifndef SI_PORT
#define SI_PORT 0
#endif

static const uint8_t port_pin[4] = {19, 5, 26, 27};

/* Always RMT channel 0, whichever port is chosen - any pin can be routed to
 * any channel's input, and channel 0 is the one that can own all eight memory
 * blocks starting from the bottom. That is 512 items, so a whole transaction
 * fits with room to spare. */
#define SI_CH 0
#define SI_MEM_BLOCKS 8
#define SI_MAX_ITEMS (SI_MEM_BLOCKS * 64)

/* 0.5us per tick, as BlueRetro uses: 80MHz APB / 40. */
#define SI_DIV_CNT 40

/* Idle gap that ends a capture, in ticks. A GameCube bit cell is 4us and the
 * turnaround between a request and its reply is a few us, so 30us keeps the
 * two halves of one transaction in a single capture - which is what makes the
 * result decodable, because the capture then starts on the first bit of the
 * request rather than somewhere inside the reply. Polls are milliseconds
 * apart, so separate transactions still come out separate. */
#define SI_IDLE_THRES 60

#define MEM_OWNER_SW 0
#define MEM_OWNER_HW 1

/* Interrupt status bits are three per channel: tx end, rx end, error. */
#define SI_RX_DONE_BIT BIT(SI_CH * 3 + 1)
#define SI_RX_ERR_BIT  BIT(SI_CH * 3 + 2)

/* Record on the wire, little endian:
 *   u8  magic[4]
 *   u8  port
 *   u8  flags     bit0: records were dropped before this one
 *                 bit1: this capture filled the memory and was cut short
 *   u16 count     number of items that follow
 *   u32 ts        CPU cycles, 240MHz
 *   u8  items[2 * count]
 *
 * One item is one (low, high) pulse pair, two bytes: duration in the low
 * seven bits in 0.5us ticks, clamped at 127, and the level in the top bit. */
#define SI_REC_HDR 12
static const uint8_t si_magic[4] = {0xA5, 0x5A, 0x17, 0x53};

#define RING_SIZE (64 * 1024)
static uint8_t ring[RING_SIZE];
static volatile uint32_t ring_head;
static volatile uint32_t ring_tail;
static volatile uint32_t lost;
static volatile uint32_t errors;

/* The RMT's own memory, flat: eight blocks of 64 items. The symbol is
 * PROVIDEd by esp32.peripherals.ld at 0x3ff56800, so this is a
 * declaration rather than a guess at an address.
 *
 * One item is 32 bits: duration0 in bits 0-14, level0 in bit 15,
 * duration1 in bits 16-30, level1 in bit 31. */
extern volatile uint32_t RMTMEM[];

static intr_handle_t isr_handle;

static inline uint8_t IRAM_ATTR pack(uint32_t dur, uint32_t lvl) {
    if (dur > 0x7F) {
        dur = 0x7F;
    }
    return (uint8_t)(dur | (lvl << 7));
}

/* How many items the receive actually put down. The RMT marks the end with a
 * zero duration entry, so the run of non-zero ones is the capture. Reading a
 * fixed length instead would pad every short capture with whatever the last
 * one left behind, which is the kind of wrong that looks like data.
 *
 * That marker only means anything if what follows it is already zero, which
 * is why the memory is cleared at init and again after every capture. Left
 * uncleared it reads as one 512 item capture after another, which is exactly
 * what the first run on hardware produced. */
static inline uint32_t IRAM_ATTR item_count(void) {
    uint32_t n = 0;

    while (n < SI_MAX_ITEMS && (RMTMEM[n] & 0x7FFF)) {
        n++;
    }
    return n;
}

static void IRAM_ATTR si_capture(void) {
    uint32_t ts = esp_cpu_get_cycle_count();
    uint32_t head = ring_head;
    uint32_t space = (ring_tail - head - 1) & (RING_SIZE - 1);
    uint32_t n, len;

    RMT.conf_ch[SI_CH].conf1.rx_en = 0;
    RMT.conf_ch[SI_CH].conf1.mem_owner = MEM_OWNER_SW;

    n = item_count();
    len = SI_REC_HDR + n * 2;

    if (n && len <= space) {
        uint8_t hdr[SI_REC_HDR];

        hdr[0] = si_magic[0];
        hdr[1] = si_magic[1];
        hdr[2] = si_magic[2];
        hdr[3] = si_magic[3];
        hdr[4] = SI_PORT;
        hdr[5] = (lost ? 1 : 0) | (n >= SI_MAX_ITEMS ? 2 : 0);
        hdr[6] = n;
        hdr[7] = n >> 8;
        hdr[8] = ts;
        hdr[9] = ts >> 8;
        hdr[10] = ts >> 16;
        hdr[11] = ts >> 24;

        for (uint32_t i = 0; i < SI_REC_HDR; i++) {
            ring[head] = hdr[i];
            head = (head + 1) & (RING_SIZE - 1);
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = RMTMEM[i];

            ring[head] = pack(v & 0x7FFF, (v >> 15) & 1);
            head = (head + 1) & (RING_SIZE - 1);
            ring[head] = pack((v >> 16) & 0x7FFF, (v >> 31) & 1);
            head = (head + 1) & (RING_SIZE - 1);
        }

        /* Published last, so the sender never sees half a record. */
        ring_head = head;
        lost = 0;
    }
    else if (n) {
        lost++;
    }

    /* Wipe what was read, including the slot the end marker sat in, so the
     * next capture ends somewhere unambiguous. */
    for (uint32_t i = 0; i <= n && i < SI_MAX_ITEMS; i++) {
        RMTMEM[i] = 0;
    }

    /* Put the receiver back by hand. Nothing here transmits, and it is only
     * the transmit path that would otherwise re-arm it. */
    RMT.conf_ch[SI_CH].conf1.mem_wr_rst = 1;
    RMT.conf_ch[SI_CH].conf1.mem_wr_rst = 0;
    RMT.conf_ch[SI_CH].conf1.mem_owner = MEM_OWNER_HW;
    RMT.conf_ch[SI_CH].conf1.rx_en = 1;
}

static void IRAM_ATTR si_isr(void *arg) {
    uint32_t st = RMT.int_st.val;

    if (st & SI_RX_DONE_BIT) {
        si_capture();
    }

    if (st & SI_RX_ERR_BIT) {
        /* Recover rather than mask it off: only the transmit path re-arms the
         * receiver, and an error produces nothing to transmit, so a port that
         * is not put back here goes deaf until the next reset. */
        RMT.conf_ch[SI_CH].conf1.rx_en = 0;
        RMT.conf_ch[SI_CH].conf1.mem_owner = MEM_OWNER_SW;
        RMT.conf_ch[SI_CH].conf1.mem_wr_rst = 1;
        RMT.conf_ch[SI_CH].conf1.mem_wr_rst = 0;
        RMT.conf_ch[SI_CH].conf1.mem_owner = MEM_OWNER_HW;
        RMT.conf_ch[SI_CH].conf1.rx_en = 1;
        errors++;
    }

    RMT.int_clr.val = st;
}

static void si_rmt_init(void) {
    uint8_t pin = port_pin[SI_PORT];
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    periph_ll_enable_clk_clear_rst(PERIPH_RMT_MODULE);

    /* Talk to the channel memory directly rather than through the FIFO. */
    RMT.apb_conf.fifo_mask = 1;

    gpio_config(&cfg);

    /* Input only. The output signal is deliberately never connected, so this
     * firmware cannot drive the bus even if it is wrong. */
    esp_rom_gpio_connect_in_signal(pin, RMT_SIG_IN0_IDX + SI_CH, false);

    RMT.conf_ch[SI_CH].conf0.div_cnt = SI_DIV_CNT;
    RMT.conf_ch[SI_CH].conf0.mem_size = SI_MEM_BLOCKS;
    RMT.conf_ch[SI_CH].conf0.idle_thres = SI_IDLE_THRES;
    RMT.conf_ch[SI_CH].conf0.carrier_en = 0;
    RMT.conf_ch[SI_CH].conf0.carrier_out_lv = 0;
    RMT.conf_ch[SI_CH].conf1.ref_always_on = 1;
    RMT.conf_ch[SI_CH].conf1.rx_filter_en = 0;
    RMT.conf_ch[SI_CH].conf1.rx_filter_thres = 0;
    RMT.conf_ch[SI_CH].conf1.tx_conti_mode = 0;
    RMT.conf_ch[SI_CH].conf1.idle_out_en = 0;

    RMT.conf_ch[SI_CH].conf1.mem_wr_rst = 1;
    RMT.conf_ch[SI_CH].conf1.mem_wr_rst = 0;

    /* Powers up holding rubbish, and the end of a capture is found by
     * looking for a zero, so start from zero. */
    for (uint32_t i = 0; i < SI_MAX_ITEMS; i++) {
        RMTMEM[i] = 0;
    }

    RMT.conf_ch[SI_CH].conf1.mem_owner = MEM_OWNER_HW;

    RMT.int_clr.val = SI_RX_DONE_BIT | SI_RX_ERR_BIT;
    RMT.int_ena.val |= SI_RX_DONE_BIT | SI_RX_ERR_BIT;

    ESP_ERROR_CHECK(esp_intr_alloc(ETS_RMT_INTR_SOURCE, ESP_INTR_FLAG_IRAM,
        si_isr, NULL, &isr_handle));

    RMT.conf_ch[SI_CH].conf1.rx_en = 1;
}

/* The only job this firmware has, so it can have the CPU. */
static void si_tx_task(void *arg) {
    while (1) {
        uint32_t head = ring_head;
        uint32_t tail = ring_tail;

        if (tail == head) {
            vTaskDelay(1);
            continue;
        }

        /* One contiguous run at a time; the wrap is the next pass. */
        uint32_t run = (head > tail) ? (head - tail) : (RING_SIZE - tail);

        uart_write_bytes(UART_NUM_0, (const char *)&ring[tail], run);
        ring_tail = (tail + run) & (RING_SIZE - 1);
    }
}

void app_main(void) {
    /* Said before the UART driver goes in and before any binary follows, so a
     * capture file starts with something a human can read. The host decoder
     * skips whatever precedes the first magic anyway. */
    printf("\n$SI sniffer: port %d, gpio %d, rmt ch %d, idle %d ticks\n",
        SI_PORT, port_pin[SI_PORT], SI_CH, SI_IDLE_THRES);
    printf("$SI record: magic[4] port flags count[2] ts[4] items[2*count]\n");
    printf("$SI item: low 7 bits ticks of 0.5us clamped at 127, top bit level\n");
    fflush(stdout);

    /* Nothing must print into the binary stream from here on. */
    esp_log_level_set("*", ESP_LOG_NONE);

    uart_config_t uart_cfg = {
        .baud_rate = 921600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 32 * 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_cfg));

    si_rmt_init();

    xTaskCreatePinnedToCore(si_tx_task, "si_tx", 3072, NULL, 5, NULL, 0);
}
