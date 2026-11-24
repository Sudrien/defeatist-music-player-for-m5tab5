/* SPDX-License-Identifier: MIT
 *
 * u-defeatist-0022: what a box does not answer, as the app's own functions.
 * (In boxed/main/ until u-defeatist-0028 made the player's one build the
 * boxed one.)
 *
 * The boxed ELF is the player's sources unchanged, so it still calls the
 * network, NVS, esp_audio_codec, raw I2C and a few
 * others that a feckless box (feckless-libs' umode branch, to
 * u-feckless-0049) has no host call for. Each is defined
 * here to fail the way the player already copes with on a trusted run:
 *
 *   the network      the routes and sockets answered by the box since
 *                    u-feckless-0071 (u-defeatist-0042): a cable the
 *                    launcher brought up is the player's, and MPD serves on
 *                    it. The HTTP client is the box's since
 *                    u-feckless-0073 (u-defeatist-0044): web radio, the
 *                    benchmark, podcast search, station pictures. The
 *                    HTTP server, feckless_https and esp_netif's
 *                    interfaces since u-feckless-0075..0077
 *                    (u-defeatist-0045): remote control. feckless_fetch
 *                    since u-feckless-0081 (u-defeatist-0046): podcast
 *                    downloads. The radio's control, the saved networks
 *                    and the network hooks since u-feckless-0083
 *                    (u-defeatist-0047): Wi-Fi and NTP. Still here: the
 *                    portal's access point and its DHCP server and DNS;
 *                    they fail as with no radio, and portal_absent() says
 *                    so.
 *   the crypto       SHA-256 answered by the box since u-feckless-0078
 *                    (u-defeatist-0045): approve.c's allowed browsers.
 *   NVS              answered by the box since u-feckless-0053
 *                    (u-defeatist-0024), in the namespace the launcher
 *                    names; with none named it reads as empty, as here
 *                    before.
 *   esp_audio_codec  only with DEFEATIST_BOXED_CODEC off (u-defeatist-0023):
 *                    absent_codec.c. On, the box answers it
 *                    (u-feckless-0052).
 *   hardware JPEG    answered by the box since u-feckless-0065
 *                    (u-defeatist-0029): its stand-ins are gone from here.
 *   the rest         esp_ota/esp_partition (no slot to pick: u-defeatist-0025
 *                    has the table say "hosted", so Exit to Launcher shows
 *                    and leaves by esp_restart(), which a box answers by
 *                    ending the app), raw I2C (the power-off pulse), HID (a
 *                    callback a box cannot call yet), USB class
 *                    registration (likewise), deep sleep (returns, and the
 *                    player's esp_restart() after it ends the app).
 *
 * Each of these comes out of this file when a box answers it -- the box's
 * call then replaces it at the next build, and boxcheck says which are
 * left. Not a stub of the player's own code: every name here is a
 * library's, and the player's source is untouched.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "esp_err.h"
#include "netabsent.h"   /* u-defeatist-0041 */
#include "esp_log.h"

#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "esp_system.h"   /* u-defeatist-0025: esp_restart */
#include "driver/i2c_master.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "ethernet.h"
#include "hid.h"
#include "usbhost.h"
#include "wifi.h"

static const char *TAG = "boxed";

/* ---- the network: down ---------------------------------------------------- */

/*
 * u-defeatist-0042: the routes' state -- net_online, net_probe,
 * net_route_describe, ethernet_*, wifi_up, wifi_connected, wifi_ntp_synced,
 * wifi_sta_ip, wifi_sta_ssid, wifistore_count -- and lwIP's sockets,
 * select() and close() are the box's now (u-feckless-0071), and so are
 * esp_http_client and esp_crt_bundle_attach (u-feckless-0073,
 * u-defeatist-0044), and so are esp_http_server, feckless_https and
 * esp_netif_find_if / get_ip_info / get_all_ip6 (u-feckless-0075..0077,
 * u-defeatist-0045). u-defeatist-0047: and the radio's control --
 * wifi_scan_list, wifi_join, wifi_psk, wifistore_save, esp_wifi_get_mac --
 * and, in feckless_box_libc, feckless_net_set_hooks, wifi_apply_settings,
 * wifi_request_apply, wifi_request_restart and wifi_auth_name
 * (u-feckless-0083). All gone from here. What stays below is the setup
 * portal's access point, which a box cannot run yet.
 */

/* u-defeatist-0041, 0042, 0047: said, not only failed -- see netabsent.h.
 * Goes when a box answers the portal's AP. */
bool portal_absent(void) { return true; }

/* u-defeatist-0049: wifi_ap_begin, wifi_ap_begin_secure, wifi_ap_end,
 * wifi_ap_clients and wifi_ap_ip are the box's, and wifi_ap_netif is
 * feckless_box_libc's (u-feckless-0084): the hotspot. What stays is the
 * setup portal's DHCP options and captive DNS, which nothing starts since
 * u-defeatist-0048. */

esp_err_t esp_netif_set_dns_info(esp_netif_t *n, esp_netif_dns_type_t t, esp_netif_dns_info_t *d)
{
    (void)n; (void)t; (void)d;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_netif_dhcps_start(esp_netif_t *n) { (void)n; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_netif_dhcps_stop(esp_netif_t *n) { (void)n; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_netif_dhcps_option(esp_netif_t *n, esp_netif_dhcp_option_mode_t op, esp_netif_dhcp_option_id_t id,
                                 void *val, uint32_t len)
{
    (void)n; (void)op; (void)id; (void)val; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

/* ---- the rest -------------------------------------------------------------- */

/* u-defeatist-0025: the table a boxed app is given, so launcher.c's
 * launcher_kind() says LAUNCHER_HOSTED -- a factory app running, and an
 * "app2" beside it -- which is the truth of a box: a launcher is running
 * this, and leaving is esp_restart(), which the box answers by ending the
 * app (feckless_box_calls' c_esp_restart) and the launcher's menu comes
 * back. Without it every lookup was NULL, launcher_present() false, and
 * Exit to Launcher was hidden: power off was the only way out.
 *
 * launcher.c, the only caller in this ELF, compares the pointers and never
 * reads through them; the entries are filled in all the same, so anything
 * that did would read a plausible, empty, unencrypted app slot. */
static const esp_partition_t s_box_factory = {
    .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_FACTORY, .label = "boxed",
};
static const esp_partition_t s_box_app2 = {
    .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0, .label = "app2",
};
const esp_partition_t *esp_ota_get_running_partition(void) { return &s_box_factory; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { (void)p; return ESP_ERR_NOT_SUPPORTED; }
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char *label)
{
    if (type != ESP_PARTITION_TYPE_APP) return NULL;
    if (subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) return &s_box_factory;
    if (subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 && label && !strcmp(label, "app2")) return &s_box_app2;
    return NULL;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *wr, size_t wr_size, int timeout_ms)
{
    (void)dev; (void)wr; (void)wr_size; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *wr, size_t wr_size, uint8_t *rd,
                                      size_t rd_size, int timeout_ms)
{
    (void)dev; (void)wr; (void)wr_size; (void)rd; (void)rd_size; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

/* u-defeatist-0054: hid_init and hid_present are the box's
 * (u-feckless-0086): a headset's and keyboard's media keys. */
esp_err_t usbhost_register_class(const char *name, usbhost_class_fn fn) { (void)name; (void)fn; return ESP_ERR_NOT_SUPPORTED; }

/* The power-off tail: a box cannot power the board down, so the app ends
 * and the launcher comes back. u-defeatist-0025: by esp_restart(), not
 * abort(). The box answers esp_restart() as an app asking to end (a
 * warning, feckless_box_calls' c_esp_restart); abort() was logged as an
 * error, the app counted as killed. Both are noreturn, as this is: the
 * esp_restart() player.c writes after esp_deep_sleep_start() is dead code
 * to the compiler, so it is called here. */
/* u-defeatist-0055: what the app meant by ending, for the launcher
 * (u-feckless-0087's feckless_box_end_wish, in the box section; its values
 * are feckless_box_calls.h's feckless_box_wish_t, which the ELF build does
 * not include). */
void feckless_box_end_wish(uint32_t wish);
#define BOX_WISH_POWER_OFF 1
#define BOX_WISH_LAUNCHER  2

void esp_deep_sleep_start(void)
{
    /* u-defeatist-0028 made the launcher power off (or go back to
     * M5Launcher) after a clean end; u-defeatist-0055 tells it which. */
    ESP_LOGW(TAG, "power off asked for: the app ends here and the launcher powers off");
    feckless_box_end_wish(BOX_WISH_POWER_OFF);
    esp_restart();
}

/* launcher.c's hook, before its restart: back to the launcher's menu, not
 * a power off (u-defeatist-0055). The trusted build has launcher.c's weak
 * one, which does nothing. */
void launcher_before_restart(void)
{
    feckless_box_end_wish(BOX_WISH_LAUNCHER);
}
