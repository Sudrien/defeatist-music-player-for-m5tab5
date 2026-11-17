/* SPDX-License-Identifier: MIT
 *
 * 8074: the .bin's app_main(): the launcher's side of what app_main() was.
 * defeatist_main() (player.c) is the player; this brings up what a
 * launcher brings up before it runs an app, in Avoidant's and Errant's
 * order: the console; NVS; the bus and expanders, which hold the USB host's
 * power; the USB host's owner before storage, which registers mass storage
 * with it as a class; storage; the network, brought up to be asked for
 * (feckless_net_start(): the saved networks, then the radio's and the USB
 * Ethernet's drivers, nothing powered), before the port so Ethernet's class
 * is installed with the rest; and M5Launcher's saved networks, imported
 * once (8068). The port itself is started by the player, after it has
 * registered its own USB classes, as before: usbhost_start() is idempotent,
 * so the same code under a launcher that has the port up already does
 * nothing.
 *
 * 8087: the player is the ELF that ../elf/ built from the rest of main/
 * (elf_main.c is its app_main()), embedded here and run as a launcher runs a
 * published copy.
 *
 * u-defeatist-0028: in a box, as Avoidant's umode branch runs it -- RISC-V
 * user mode, in one block of PSRAM the PMP grants it and nothing else, every
 * call outside it answered by feckless_box_calls -- and not through
 * feckless_abi_run(), which would refuse it by the name feckless_abi_box_v1.
 * This file is the whole of the launcher, in the shape Errant's, Spurious's
 * and Lothesome's standalone .bins have: what is brought up below, then the
 * panel, gfx and touch on core 0, so the panel's interrupts are that core's;
 * then the box, the player's NVS namespace named for it, and the player run
 * on core 1, where its windows cannot hold the panel's interrupts off. The
 * network is started as before, though a box cannot ask for it yet
 * (absent.c).
 *
 * When the player ends: Exit to Launcher and power off both end it the same
 * way in a box (esp_restart(), which the box answers by ending the app;
 * absent.c). So on a clean end this does what a launcher would be there
 * for: back to M5Launcher if one booted this from a slot, and otherwise the
 * power off the player asked for and could not do from the box -- the
 * PMS150's pulse, then deep sleep. A fault restarts, as a panic did.
 */
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"           /* u-defeatist-0028: the power-off pulse */
#include "driver/usb_serial_jtag.h"      /* 5082: the console TX buffer */
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "feckless_box_calls.h"
#include "feckless_net.h"
#include "feckless_umode_idf.h"
#include "feckless_umode_load.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx.h"
#include "launcher.h"
#include "lcd.h"
#include "nvs_handler.h"
#include "nvsns.h"
#include "shim_import.h"
#include "storage.h"
#include "storage_io.h"
#include "tab5io.h"
#include "touch.h"
#include "usbhost.h"

static const char *TAG = "standalone";

/* The player: elf_loader's embed of build/elf/defeatist_player.app.elf. */
extern const uint8_t elf_start[] asm("_binary_defeatist_player_app_elf_start");
extern const uint8_t elf_end[]   asm("_binary_defeatist_player_app_elf_end");

/* u-defeatist-0028: the box. The largest free PSRAM block less 2 MB for this
 * firmware, measured after the panel's framebuffer is taken, and at most
 * 24 MB -- Avoidant's rule (u-avoidant-0005). The player wants about
 * 12 MB before its covers and library: the ELF and its statics 1.35 MB,
 * its three PCM rings 10.5 MB (BOX.md). */
#define BOX_MARGIN      (2u << 20)
#define BOX_MAX         (24u << 20)
#define BOX_STACK       (64 * 1024)         /* app_main's, in the box */
#define BOX_CORE        (1)                 /* not the panel's */

/* u-defeatist-0028: power off from here, as player.c's power_off_now()
 * does it on a trusted run: the PMS150 cuts the supply when P4 of the
 * expander at 0x44 is toggled -- M5Unified's ten writes, 50 ms apart. On
 * battery the rail goes somewhere in the loop; on external power the
 * controller holds it, and deep sleep takes the SoC dark instead. The
 * player has saved its settings before it ended. */
#define PWROFF_PULSE_BIT    (1u << 4)
#define PWROFF_I2C_MS       (1000)

static void power_off(void)
{
    ESP_LOGW(TAG, "powering off");
    lcd_backlight_set(0);
    i2c_master_dev_handle_t exp2 = tab5io_exp2();
    if (exp2) {
        const uint8_t setup[][2] = {
            { PI4IOE_REG_IO_DIR,     1 },     /* output        */
            { PI4IOE_REG_OUT_HIGH_Z, 0 },     /* out of high-Z */
        };
        for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i++) {
            uint8_t reg = setup[i][0], val = 0;
            if (i2c_master_transmit_receive(exp2, &reg, 1, &val, 1, PWROFF_I2C_MS) != ESP_OK) continue;
            val = setup[i][1] ? (uint8_t)(val | PWROFF_PULSE_BIT) : (uint8_t)(val & (uint8_t)~PWROFF_PULSE_BIT);
            const uint8_t w[2] = { reg, val };
            (void)i2c_master_transmit(exp2, w, sizeof w, PWROFF_I2C_MS);
        }
        uint8_t reg = PI4IOE_REG_OUT_SET, out = 0;
        if (i2c_master_transmit_receive(exp2, &reg, 1, &out, 1, PWROFF_I2C_MS) == ESP_OK) {
            for (int i = 0; i < 10; i++) {
                const uint8_t v = (i & 1) ? (uint8_t)(out | PWROFF_PULSE_BIT) : (uint8_t)(out & (uint8_t)~PWROFF_PULSE_BIT);
                const uint8_t w[2] = { PI4IOE_REG_OUT_SET, v };
                (void)i2c_master_transmit(exp2, w, sizeof w, PWROFF_I2C_MS);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    } else {
        ESP_LOGE(TAG, "power off: expander 0x44 not up");
    }
    esp_deep_sleep_start();
}

void app_main(void)
{
    /*
     * 5082: THE CONSOLE'S 4 KB TX BUFFER, WHICH WAS NEVER THERE.
     *
     * sdkconfig.defaults has asked for it since 3a88a45 with
     * CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_TX_BUF_SIZE=4096, and IDF 5.5
     * has no such symbol (5077 found it). Without the driver the console
     * writes byte by byte into the peripheral's 64-byte FIFO, spinning
     * while it is full, and after 50 ms without progress drops bytes
     * until it drains -- which is how a board log came to read
     * `stalleI (78827) ...`, a line cut off by the next one. The only
     * way to a buffer is to install the driver and point the console at
     * it. Once a write fails to make room in 50 ms, later bytes drop at
     * once until one gets through, so with no host reading the log
     * nothing blocks. Before the banner, so everything from here is
     * buffered; the 4 KB ring comes from internal RAM, as the defaults'
     * note always meant it to.
     */
    {
        usb_serial_jtag_driver_config_t usj = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        usj.tx_buffer_size = 4096;
        const esp_err_t usj_err = usb_serial_jtag_driver_install(&usj);
        if (usj_err == ESP_OK) {
            usb_serial_jtag_vfs_use_driver();
        }
        /* 5087's line, as 5089 had it under the banner: the banner is
         * the ELF's now, and prints after this one. A cut line in a log
         * that says `4096-byte` is the host not reading, not the buffer
         * missing. */
        if (usj_err == ESP_OK) {
            ESP_LOGI(TAG, "console: USB-serial-JTAG driver, 4096-byte TX buffer");
        } else {
            ESP_LOGW(TAG, "console: driver not installed (%s); unbuffered",
                     esp_err_to_name(usj_err));
        }
    }

    /*
     * NVS, and the saved networks in it.
     *
     * The erase-and-retry is IDF's documented idiom, not defensiveness:
     * a partition that is full or was written by a different NVS version
     * cannot be opened, and the alternative to erasing it is a player
     * that has no network list and no way to acquire one. Both failures
     * lose the saved networks; only one of them can be recovered from
     * without a reflash.
     */
    const esp_err_t nvs_err = nvsh_init();      /* 8064: the erase-and-retry is in it */
    if (nvs_err != ESP_OK) {
        /* Not fatal: the saved-network list is left empty, which is
         * settings.c's bargain. A player that will not start because it
         * could not read its network list is a worse program than one
         * that forgets them. */
        ESP_LOGE(TAG, "nvsh_init: %s", esp_err_to_name(nvs_err));
    }
    if (nvs_err == ESP_OK) nvsns_migrate();     /* 6006: before anything reads it */

    ESP_ERROR_CHECK(tab5io_init());             /* 6056: bus, then expanders */
    /* The bus owner before anything that registers a class driver with
     * it: storage_init() registers one. */
    ESP_ERROR_CHECK(usbhost_init(tab5io_exp2()));

    /* Before storage_init(), because settings.c reads through the same
     * arbiter and the first mount happens in there. It degrades to plain
     * stdio if it is ever reached before this, which is the reason
     * nothing is ESP_ERROR_CHECK()ed on the media itself: an empty slot
     * and an empty port are a normal way to boot, and the chooser says
     * so on screen. */
    storage_io_init();

    /* 6053: storage lives in feckless-storage-handler and does not own
     * the bus; this is the bus. */
    static const storage_usb_t storage_usb = {
        .register_class = usbhost_register_class,
        .set_power      = usbhost_set_power,
        .powered        = usbhost_powered,
    };
    ESP_ERROR_CHECK(storage_init(&storage_usb));

    /* The saved networks (in this player's namespace), the USB host's
     * configuration selector, wifi_init() and ethernet_init(). Nothing is
     * powered: the player's hooks say whether the radio is wanted. */
    const feckless_net_start_t net = {
        .exp2                  = tab5io_exp2(),
        .usb_register_class    = usbhost_register_class,
        .usb_set_config_select = usbhost_set_config_select,
        .nvs_ns                = DEFEATIST_NVS_NS,
    };
    feckless_net_start(&net);

    /* 8068: M5Launcher's saved networks, once, before the radio asks for
     * any. Does nothing unless a launcher booted this from a slot and no
     * network is saved yet (shim_import.h); the card is up by now. */
    if (nvs_err == ESP_OK) (void)shim_import_launcher_wifi(DEFEATIST_NVS_NS);

    /* u-defeatist-0028: the panel, gfx and touch, on core 0, before the box
     * is sized. The player's own inits find them up and return ESP_OK. */
    esp_lcd_panel_handle_t panel = NULL;
    if (lcd_init(&panel) != ESP_OK || gfx_init(panel, LCD_H_RES, LCD_V_RES) != ESP_OK) {
        ESP_LOGE(TAG, "the panel did not come up");
        return;
    }
    touch_init(tab5io_bus(), LCD_H_RES, LCD_V_RES);     /* the player copes without */

    /* The box. The stack guard's interrupt out of every core's routing
     * first, which a window needs. NVS in the player's namespace only:
     * the saved networks and everything else stay out of its reach. */
    feckless_umode_guard_unroute();
    const size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t box = big > BOX_MARGIN ? big - BOX_MARGIN : 0;
    if (box > BOX_MAX) box = BOX_MAX;
    box &= ~(size_t)63;
    const char *const ns[] = { DEFEATIST_NVS_NS };
    (void)feckless_box_calls_nvs(ns, 1);
    uint32_t n;
    const feckless_umode_call_t *calls = feckless_box_calls_v1(&n);
    feckless_umode_app_t *app = NULL;
    const esp_err_t err = feckless_umode_load(elf_start, (size_t)(elf_end - elf_start), calls, n,
                                              (uint32_t)box, BOX_STACK, &app);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the player would not load in a box of %u KB: %s",
                 (unsigned)(box / 1024), esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "the player in a box of %u KB", (unsigned)(box / 1024));
    const feckless_umode_end_t end = feckless_umode_app_run_on(app, BOX_CORE);
    feckless_umode_unload(app);
    feckless_box_calls_end();

    if (end.reason == FECKLESS_UMODE_END_EXIT || end.reason == FECKLESS_UMODE_END_KILLED) {
        ESP_LOGI(TAG, "the player ended");
        if (launcher_present()) launcher_exit_to_launcher();   /* back to M5Launcher; no return */
        power_off();
    }
    ESP_LOGE(TAG, "the player ended: reason %d, %s at 0x%08x; restarting", (int)end.reason,
             end.reason == FECKLESS_UMODE_END_FAULT ? feckless_umode_cause_name(end.mcause) : "-",
             (unsigned)end.pc);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}
