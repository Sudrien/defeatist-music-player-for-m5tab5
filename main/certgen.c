/*
 * certgen.c -- see certgen.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "certgen.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

int certgen_make(const char *name, certgen_rng_t rng, void *rng_ctx,
                 char *key_pem, size_t key_cap,
                 char *crt_pem, size_t crt_cap)
{
    if (!name || !name[0] || !rng || !key_pem || !crt_pem) return -1;

    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    int ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret == 0) ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                                            mbedtls_pk_ec(key), rng, rng_ctx);
    if (ret == 0) ret = mbedtls_pk_write_key_pem(&key, (unsigned char *)key_pem, key_cap);
    if (ret != 0) goto out;

    /* Same name as subject and issuer: it signs itself. */
    char dn[96];
    snprintf(dn, sizeof(dn), "CN=%s,O=Defeatist Music Player", name);

    /* name.local, lower case, for the SAN. */
    char host[64];
    size_t n = 0;
    for (const char *p = name; *p && n + 7 < sizeof(host); p++) {
        host[n++] = (char)tolower((unsigned char)*p);
    }
    memcpy(host + n, ".local", 7);

    unsigned char serial[16];
    if ((ret = rng(rng_ctx, serial, sizeof(serial))) != 0) goto out;
    serial[0] &= 0x7F;          /* positive */
    if (!serial[0]) serial[0] = 1;

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if ((ret = mbedtls_x509write_crt_set_subject_name(&crt, dn)) != 0) goto out;
    if ((ret = mbedtls_x509write_crt_set_issuer_name(&crt, dn)) != 0) goto out;
    if ((ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial))) != 0) goto out;
    if ((ret = mbedtls_x509write_crt_set_validity(&crt, "20260101000000",
                                                  "20491231235959")) != 0) goto out;
    if ((ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) != 0) goto out;
    if ((ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE)) != 0) goto out;
    if ((ret = mbedtls_x509write_crt_set_subject_key_identifier(&crt)) != 0) goto out;

    mbedtls_x509_san_list san;
    memset(&san, 0, sizeof(san));
    san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san.node.san.unstructured_name.p = (unsigned char *)host;
    san.node.san.unstructured_name.len = strlen(host);
    if ((ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san)) != 0) goto out;

    ret = mbedtls_x509write_crt_pem(&crt, (unsigned char *)crt_pem, crt_cap, rng, rng_ctx);

out:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    if (ret != 0) {
        if (key_cap) key_pem[0] = '\0';
        if (crt_cap) crt_pem[0] = '\0';
    }
    return ret;
}

int certgen_fingerprint(const char *crt_pem, unsigned char out[32])
{
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int ret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)crt_pem, strlen(crt_pem) + 1);
    if (ret == 0) ret = mbedtls_sha256(crt.raw.p, crt.raw.len, out, 0);
    mbedtls_x509_crt_free(&crt);
    return ret;
}
