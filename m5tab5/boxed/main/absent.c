/* SPDX-License-Identifier: MIT
 *
 * u-defeatist-0022: what a box does not answer, as the app's own functions.
 *
 * The boxed ELF is the player's sources unchanged, so it still calls the
 * network, NVS, esp_audio_codec, the hardware JPEG decoder, the crypto
 * section, raw I2C and a few others that a feckless box (feckless-libs'
 * umode branch, to u-feckless-0049) has no host call for. Each is defined
 * here to fail the way the player already copes with on a trusted run:
 *
 *   the network      down and never coming up: no Wi-Fi, no Ethernet, no
 *                    sockets, no HTTP client or server. The player is an
 *                    offline player that way already when nothing is
 *                    configured, and its network screens say so.
 *   NVS              nothing stored, nothing written: every get is
 *                    ESP_ERR_NVS_NOT_FOUND, so settings start from their
 *                    defaults and the card's settings file.
 *   esp_audio_codec  only with DEFEATIST_BOXED_CODEC off (u-defeatist-0023):
 *                    absent_codec.c. On, the box answers it
 *                    (u-feckless-0052).
 *   hardware JPEG    no engine: album art decodes in software
 *                    (feckless_image_handler's fallback).
 *   the rest         esp_ota/esp_partition (no "exit to launcher" slot to
 *                    pick: a box ends instead), raw I2C (the power-off
 *                    pulse), HID (a callback a box cannot call yet),
 *                    USB class registration (likewise), deep sleep (the
 *                    app ends rather than power the board down).
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
#include "esp_log.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "driver/i2c_master.h"
#include "driver/jpeg_decode.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "psa/crypto.h"

#include "ethernet.h"
#include "feckless_fetch.h"
#include "feckless_https.h"
#include "feckless_net.h"
#include "hid.h"
#include "nvs_handler.h"
#include "usbhost.h"
#include "wifi.h"
#include "wifistore.h"

static const char *TAG = "boxed";

static bool empty(char *out, size_t n)
{
    if (out && n) out[0] = '\0';
    return false;
}

/* ---- the network: down ---------------------------------------------------- */

void feckless_net_set_hooks(const feckless_net_hooks_t *hooks) { (void)hooks; }
bool net_online(void) { return false; }
bool net_probe(uint32_t timeout_ms, int *gw_ms, int *net_ms, char *what, size_t what_size)
{
    (void)timeout_ms;
    if (gw_ms) *gw_ms = -1;
    if (net_ms) *net_ms = -1;
    if (what && what_size) snprintf(what, what_size, "no network in a box");
    return false;
}
void net_route_describe(char *out, size_t out_size)
{
    if (out && out_size) snprintf(out, out_size, "none (boxed)");
}
bool ethernet_connected(void) { return false; }
bool ethernet_link(void) { return false; }
bool ethernet_ip(char *out, size_t out_size) { return empty(out, out_size); }

bool wifi_up(void) { return false; }
bool wifi_connected(void) { return false; }
bool wifi_ntp_synced(void) { return false; }
bool wifi_sta_ip(char *out, size_t out_size) { return empty(out, out_size); }
bool wifi_sta_ssid(char *out, size_t out_size) { return empty(out, out_size); }
int wifi_scan_list(wifi_seen_t *out, int max) { (void)out; (void)max; return 0; }
esp_err_t wifi_join(const char *ssid, const char *secret, uint32_t timeout_ms)
{
    (void)ssid; (void)secret; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t wifi_apply_settings(void) { return ESP_ERR_NOT_SUPPORTED; }
void wifi_request_apply(void) {}
void wifi_request_restart(const char *why) { (void)why; }
esp_err_t wifi_ap_begin(const char *ssid) { (void)ssid; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t wifi_ap_end(void) { return ESP_OK; }
int wifi_ap_clients(void) { return 0; }
struct esp_netif_obj *wifi_ap_netif(void) { return NULL; }
const char *wifi_auth_name(uint8_t auth) { (void)auth; return "?"; }
esp_err_t wifi_psk(const char *ssid, const char *passphrase, uint8_t key[32])
{
    (void)ssid; (void)passphrase;
    memset(key, 0, 32);
    return ESP_ERR_NOT_SUPPORTED;
}
int wifistore_count(void) { return 0; }
esp_err_t wifistore_save(const char *ssid, const char *secret, bool is_psk)
{
    (void)ssid; (void)secret; (void)is_psk;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_wifi_get_mac(wifi_interface_t ifx, uint8_t mac[6])
{
    (void)ifx;
    memset(mac, 0, 6);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_netif_t *esp_netif_find_if(esp_netif_find_predicate_t fn, void *ctx) { (void)fn; (void)ctx; return NULL; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ip)
{
    (void)n;
    if (ip) memset(ip, 0, sizeof *ip);
    return ESP_ERR_NOT_SUPPORTED;
}
int esp_netif_get_all_ip6(esp_netif_t *n, esp_ip6_addr_t if_ip6[]) { (void)n; (void)if_ip6; return 0; }
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

/* sockets: every call fails, as with no interface up */
int lwip_socket(int domain, int type, int protocol)
{
    (void)domain; (void)type; (void)protocol;
    errno = ENOSYS;
    return -1;
}
int lwip_bind(int s, const struct sockaddr *name, socklen_t namelen) { (void)s; (void)name; (void)namelen; errno = EBADF; return -1; }
int lwip_listen(int s, int backlog) { (void)s; (void)backlog; errno = EBADF; return -1; }
int lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen) { (void)s; (void)addr; (void)addrlen; errno = EBADF; return -1; }
int lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen)
{
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    errno = EBADF;
    return -1;
}
int lwip_getsockname(int s, struct sockaddr *name, socklen_t *namelen) { (void)s; (void)name; (void)namelen; errno = EBADF; return -1; }
int lwip_getpeername(int s, struct sockaddr *name, socklen_t *namelen) { (void)s; (void)name; (void)namelen; errno = EBADF; return -1; }
ssize_t lwip_recv(int s, void *mem, size_t len, int flags) { (void)s; (void)mem; (void)len; (void)flags; errno = EBADF; return -1; }
ssize_t lwip_recvfrom(int s, void *mem, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    (void)s; (void)mem; (void)len; (void)flags; (void)from; (void)fromlen;
    errno = EBADF;
    return -1;
}
ssize_t lwip_send(int s, const void *data, size_t size, int flags) { (void)s; (void)data; (void)size; (void)flags; errno = EBADF; return -1; }
ssize_t lwip_sendto(int s, const void *data, size_t size, int flags, const struct sockaddr *to, socklen_t tolen)
{
    (void)s; (void)data; (void)size; (void)flags; (void)to; (void)tolen;
    errno = EBADF;
    return -1;
}
const char *lwip_inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    (void)af; (void)src;
    if (dst && size) dst[0] = '\0';
    errno = EAFNOSUPPORT;
    return NULL;
}
int lwip_getaddrinfo(const char *nodename, const char *servname, const struct addrinfo *hints, struct addrinfo **res)
{
    (void)nodename; (void)servname; (void)hints;
    if (res) *res = NULL;
    return EAI_FAIL;
}
void lwip_freeaddrinfo(struct addrinfo *ai) { (void)ai; }
u32_t ipaddr_addr(const char *cp) { (void)cp; return IPADDR_NONE; }
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout)
{
    (void)nfds; (void)rd; (void)wr; (void)ex; (void)timeout;
    errno = ENOSYS;
    return -1;
}
/* No socket was ever opened, and a file is fclose()d: nothing to close. */
int close(int fd) { (void)fd; errno = EBADF; return -1; }

/* the HTTP client: no handle, so nothing after init does anything */
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) { (void)config; return NULL; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len) { (void)c; (void)write_len; return ESP_ERR_NOT_SUPPORTED; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { (void)c; return ESP_FAIL; }
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) { (void)c; return -1; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { (void)c; return -1; }
int esp_http_client_read(esp_http_client_handle_t c, char *buffer, int len) { (void)c; (void)buffer; (void)len; return -1; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { (void)c; return false; }
esp_err_t esp_http_client_get_header(esp_http_client_handle_t c, const char *key, char **value)
{
    (void)c; (void)key;
    if (value) *value = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_http_client_get_url(esp_http_client_handle_t c, char *url, const int len)
{
    (void)c;
    if (url && len > 0) url[0] = '\0';
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value)
{
    (void)c; (void)key; (void)value;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c) { (void)c; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int t) { (void)c; (void)t; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url) { (void)c; (void)url; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_ERR_NOT_SUPPORTED; }

/* the HTTP server: never started, so no handle and no request */
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    (void)config;
    if (handle) *handle = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t httpd_stop(httpd_handle_t h) { (void)h; return ESP_OK; }
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u) { (void)h; (void)u; return ESP_ERR_INVALID_ARG; }
esp_err_t httpd_queue_work(httpd_handle_t h, httpd_work_fn_t work, void *arg) { (void)h; (void)work; (void)arg; return ESP_ERR_INVALID_ARG; }
esp_err_t httpd_get_client_list(httpd_handle_t h, size_t *fds, int *client_fds)
{
    (void)h; (void)client_fds;
    if (fds) *fds = 0;
    return ESP_ERR_INVALID_ARG;
}
esp_err_t httpd_sess_trigger_close(httpd_handle_t h, int fd) { (void)h; (void)fd; return ESP_ERR_INVALID_ARG; }
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size)
{
    (void)qry; (void)key;
    empty(val, val_size);
    return ESP_ERR_NOT_FOUND;
}
bool httpd_uri_match_wildcard(const char *t, const char *u, size_t n) { (void)t; (void)u; (void)n; return false; }
esp_err_t httpd_req_get_cookie_val(httpd_req_t *r, const char *name, char *val, size_t *val_size)
{
    (void)r; (void)name; (void)val; (void)val_size;
    return ESP_ERR_NOT_FOUND;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *f, char *val, size_t val_size)
{
    (void)r; (void)f;
    empty(val, val_size);
    return ESP_ERR_NOT_FOUND;
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    empty(buf, buf_len);
    return ESP_ERR_NOT_FOUND;
}
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len) { (void)r; (void)buf; (void)buf_len; return -1; }
int httpd_req_to_sockfd(httpd_req_t *r) { (void)r; return -1; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t len) { (void)r; (void)buf; (void)len; return ESP_ERR_INVALID_ARG; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t len) { (void)r; (void)buf; (void)len; return ESP_ERR_INVALID_ARG; }
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t e, const char *msg) { (void)r; (void)e; (void)msg; return ESP_ERR_INVALID_ARG; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *f, const char *v) { (void)r; (void)f; (void)v; return ESP_ERR_INVALID_ARG; }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_ERR_INVALID_ARG; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { (void)r; (void)t; return ESP_ERR_INVALID_ARG; }
httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t h, int fd) { (void)h; (void)fd; return HTTPD_WS_CLIENT_INVALID; }
esp_err_t httpd_ws_recv_frame(httpd_req_t *r, httpd_ws_frame_t *p, size_t max) { (void)r; (void)p; (void)max; return ESP_ERR_INVALID_ARG; }
esp_err_t httpd_ws_send_frame(httpd_req_t *r, httpd_ws_frame_t *p) { (void)r; (void)p; return ESP_ERR_INVALID_ARG; }
esp_err_t httpd_ws_send_frame_async(httpd_handle_t h, int fd, httpd_ws_frame_t *f) { (void)h; (void)fd; (void)f; return ESP_ERR_INVALID_ARG; }

esp_err_t feckless_https_start(httpd_handle_t *out, const httpd_config_t *cfg, uint16_t port)
{
    (void)cfg; (void)port;
    if (out) *out = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t feckless_https_stop(httpd_handle_t h) { (void)h; return ESP_OK; }
esp_err_t feckless_https_redirect(httpd_req_t *req) { (void)req; return ESP_ERR_INVALID_ARG; }
bool feckless_https_fingerprint(char *out, size_t out_size) { return empty(out, out_size); }

/* downloads */
bool feckless_fetch_start(const feckless_fetch_opts_t *opts) { (void)opts; return false; }
void feckless_fetch_cancel(void) {}
void feckless_fetch_pause(void) {}
void feckless_fetch_discard(const char *part) { (void)part; }
feckless_fetch_state_t feckless_fetch_state(int64_t *id, int64_t *got, int64_t *total, char *path, size_t path_size,
                                            uint32_t *gen)
{
    if (id) *id = 0;
    if (got) *got = 0;
    if (total) *total = 0;
    if (gen) *gen = 0;
    empty(path, path_size);
    return FECKLESS_FETCH_IDLE;
}
bool feckless_fetch_live(const char *path, char *part, size_t part_size, int64_t *ready, int64_t *total,
                         feckless_fetch_state_t *st)
{
    (void)path;
    empty(part, part_size);
    if (ready) *ready = 0;
    if (total) *total = 0;
    if (st) *st = FECKLESS_FETCH_IDLE;
    return false;
}
void feckless_fetch_why(char *out, size_t out_size)
{
    if (out && out_size) snprintf(out, out_size, "no network in a box");
}

/* the crypto section: approve.c's code hash */
psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t *input, size_t input_length, uint8_t *hash,
                              size_t hash_size, size_t *hash_length)
{
    (void)alg; (void)input; (void)input_length; (void)hash; (void)hash_size;
    if (hash_length) *hash_length = 0;
    return PSA_ERROR_NOT_SUPPORTED;
}

/* ---- NVS: nothing stored -------------------------------------------------- */

esp_err_t nvsh_get_u8(const char *ns, const char *key, uint8_t *out) { (void)ns; (void)key; (void)out; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvsh_get_str(const char *ns, const char *key, char *buf, size_t *len)
{
    (void)ns; (void)key; (void)buf; (void)len;
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvsh_get_blob(const char *ns, const char *key, void *buf, size_t *len)
{
    (void)ns; (void)key; (void)buf; (void)len;
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvsh_set_u8(const char *ns, const char *key, uint8_t v) { (void)ns; (void)key; (void)v; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t nvsh_set_str(const char *ns, const char *key, const char *s) { (void)ns; (void)key; (void)s; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t nvsh_set_blob(const char *ns, const char *key, const void *buf, size_t len)
{
    (void)ns; (void)key; (void)buf; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t nvsh_erase_key(const char *ns, const char *key) { (void)ns; (void)key; return ESP_ERR_NOT_SUPPORTED; }

/* ---- the hardware JPEG decoder: no engine --------------------------------- */

esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *cfg, jpeg_decoder_handle_t *ret)
{
    (void)cfg;
    if (ret) *ret = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}
void *jpeg_alloc_decoder_mem(size_t size, const jpeg_decode_memory_alloc_cfg_t *cfg, size_t *allocated)
{
    (void)size; (void)cfg;
    if (allocated) *allocated = 0;
    return NULL;
}
esp_err_t jpeg_decoder_get_info(const uint8_t *bs, uint32_t n, jpeg_decode_picture_info_t *info)
{
    (void)bs; (void)n; (void)info;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t eng, const jpeg_decode_cfg_t *cfg, const uint8_t *bs,
                               uint32_t n, uint8_t *out, uint32_t out_size, uint32_t *out_len)
{
    (void)eng; (void)cfg; (void)bs; (void)n; (void)out; (void)out_size;
    if (out_len) *out_len = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

/* ---- the rest -------------------------------------------------------------- */

const esp_partition_t *esp_ota_get_running_partition(void) { return NULL; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { (void)p; return ESP_ERR_NOT_SUPPORTED; }
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char *label)
{
    (void)type; (void)subtype; (void)label;
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

esp_err_t hid_init(hid_button_cb_t cb) { (void)cb; return ESP_ERR_NOT_SUPPORTED; }
bool hid_present(void) { return false; }
esp_err_t usbhost_register_class(const char *name, usbhost_class_fn fn) { (void)name; (void)fn; return ESP_ERR_NOT_SUPPORTED; }

/* The power-off tail: a box cannot power the board down, so the app ends
 * (the box answers abort() by ending it) and the launcher comes back. */
void esp_deep_sleep_start(void)
{
    ESP_LOGW(TAG, "power off asked for: a boxed app ends instead");
    abort();
}
