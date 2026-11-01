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
 * Today this is linked into the player's one build. The ELF split puts
 * defeatist_main() in the ELF and keeps this in the .bin that embeds it.
 */
#include "defeatist.h"

#include "driver/usb_serial_jtag.h"      /* 5082: the console TX buffer */
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "feckless_net.h"
#include "nvs_handler.h"
#include "nvsns.h"
#include "shim_import.h"
#include "storage.h"
#include "storage_io.h"
#include "tab5io.h"
#include "usbhost.h"

static const char *TAG = "standalone";

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
        defeatist_note_console(usj_err);        /* 5089: said under the banner */
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

    defeatist_main();
}
