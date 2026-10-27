"""Exercise the production HTTPS key selection with the installed IDF MbedTLS.

Run directly with IDF_PATH and KATMED_MBEDTLS_HOST_BUILD set. The host build
contains the unmodified MbedTLS sources from that IDF tree; no device is used.
"""

import os
import subprocess
import tempfile
from pathlib import Path

from test_network_startup import function


ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("KATMED_WEB_SOURCE", ROOT / "firmware/components/katmed_web/katmed_web.c"))

HOST = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psa/crypto.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "esp_key_config.h"

#define ESP_OK 0
#define ESP_FAIL -1
#define WEB_TAG "test"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define HTTP_GET 0
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){.server_port=80,.ctrl_port=32768})
#define HTTPD_SSL_CONFIG_DEFAULT() ((httpd_ssl_config_t){.httpd=HTTPD_DEFAULT_CONFIG()})

typedef struct {
    int max_open_sockets,max_uri_handlers,backlog_conn,recv_wait_timeout;
    int send_wait_timeout,server_port,ctrl_port,stack_size;
    bool lru_purge_enable;
    void *uri_match_fn;
} httpd_config_t;
typedef struct {
    httpd_config_t httpd;
    int tls_handshake_timeout_ms;
    const void *servercert,*prvtkey_pem,*cacert_pem;
    size_t servercert_len,prvtkey_len,cacert_len;
    void *user_cb;
    bool client_cert_authmode_optional;
    const esp_key_config_t *server_key;
} httpd_ssl_config_t;
typedef struct {const char *uri;int method;void *handler,*user_ctx;} httpd_uri_t;
static struct {
    struct { uint8_t key[256],cert[1024];size_t key_len,cert_len; } identity;
    struct { struct {size_t cert_len;const void *client_cert;} record; } auth;
    void *https,*redirect;
    bool started;
    int certificate_fd;
    bool certificate_verified;
    uint8_t certificate_fingerprint[32];
    psa_key_id_t tls_key_id;
    esp_key_config_t tls_key_config;
} web;
static int legacy_parse_result, psa_wrap_result, https_starts, https_stops;
static bool force_https_failure;
static psa_key_id_t last_tls_key_id;
static void *tls_session_callback, *katmed_web_plain_uri_match, *redirect_get;
static uint64_t monotonic_ms(void){return 0;}
static void web_socket_diag_sample(uint64_t now,bool force){(void)now;(void)force;}
static bool resources_allow(bool post,const char *site){(void)post;(void)site;return true;}
static bool register_routes(void *handle){return handle!=NULL;}
static void wipe(void *ptr,size_t len){volatile uint8_t *p=ptr;while(len--)*p++=0;}
static int httpd_ssl_start(void **handle,const httpd_ssl_config_t *cfg){
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    ++https_starts;
    if(cfg->server_key && cfg->server_key->source==ESP_KEY_SOURCE_PSA){
        last_tls_key_id=cfg->server_key->psa.key_id;
        psa_wrap_result=mbedtls_pk_wrap_psa(&pk,cfg->server_key->psa.key_id);
        if(psa_wrap_result==0 && mbedtls_pk_get_bitlen(&pk)!=256)psa_wrap_result=-1;
        if(psa_wrap_result==0){
            mbedtls_x509_crt crt;
            mbedtls_ssl_config conf;
            mbedtls_x509_crt_init(&crt);
            mbedtls_ssl_config_init(&conf);
            psa_wrap_result=mbedtls_x509_crt_parse_der(&crt,cfg->servercert,
                                                       cfg->servercert_len);
            if(psa_wrap_result==0)
                psa_wrap_result=mbedtls_ssl_config_defaults(&conf,MBEDTLS_SSL_IS_SERVER,
                               MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
            if(psa_wrap_result==0)
                psa_wrap_result=mbedtls_ssl_conf_own_cert(&conf,&crt,&pk);
            mbedtls_ssl_config_free(&conf);
            mbedtls_x509_crt_free(&crt);
        }
        mbedtls_pk_free(&pk);
        if(psa_wrap_result!=0)return ESP_FAIL;
    }else{
        legacy_parse_result=mbedtls_pk_parse_key(&pk,cfg->prvtkey_pem,
                                                  cfg->prvtkey_len,NULL,0);
        mbedtls_pk_free(&pk);
        if(legacy_parse_result!=0)return ESP_FAIL;
    }
    if(force_https_failure)return ESP_FAIL;
    *handle=(void*)2;
    return ESP_OK;
}
static int httpd_start(void **handle,const httpd_config_t *cfg){(void)cfg;*handle=(void*)3;return ESP_OK;}
static int httpd_register_uri_handler(void *h,const httpd_uri_t *u){(void)h;(void)u;return ESP_OK;}
static int httpd_ssl_stop(void *h){(void)h;++https_stops;return ESP_OK;}
static int httpd_stop(void *h){(void)h;return ESP_OK;}

/* The two functions below are extracted unchanged from the product source. */
'''

END = r'''
int main(int argc,char **argv){
    if(argc!=2)return 2;
    if(psa_crypto_init()!=PSA_SUCCESS)return 2;
    /* Scalar 1 is synthetic test material, not an actual device key. */
    web.identity.key[31]=1;
    web.identity.key_len=32;
    psa_key_attributes_t attrs=PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t writer_id=PSA_KEY_ID_NULL;
    psa_set_key_type(&attrs,PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attrs,256);
    psa_set_key_usage_flags(&attrs,PSA_KEY_USAGE_SIGN_HASH|PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attrs,PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    if(psa_import_key(&attrs,web.identity.key,32,&writer_id)!=PSA_SUCCESS)return 12;
    psa_reset_key_attributes(&attrs);
    mbedtls_pk_context writer_key;
    mbedtls_x509write_cert writer;
    mbedtls_pk_init(&writer_key);
    mbedtls_x509write_crt_init(&writer);
    if(mbedtls_pk_wrap_psa(&writer_key,writer_id)!=0)return 13;
    mbedtls_x509write_crt_set_version(&writer,MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_subject_key(&writer,&writer_key);
    mbedtls_x509write_crt_set_issuer_key(&writer,&writer_key);
    mbedtls_x509write_crt_set_md_alg(&writer,MBEDTLS_MD_SHA256);
    const uint8_t serial[1]={1};
    if(mbedtls_x509write_crt_set_serial_raw(&writer,serial,sizeof(serial))!=0 ||
       mbedtls_x509write_crt_set_subject_name(&writer,"CN=test.local")!=0 ||
       mbedtls_x509write_crt_set_issuer_name(&writer,"CN=test.local")!=0 ||
       mbedtls_x509write_crt_set_validity(&writer,"20200101000000","20991231235959")!=0)
        return 14;
    uint8_t cert_buf[1024];
    int cert_len=mbedtls_x509write_crt_der(&writer,cert_buf,sizeof(cert_buf));
    if(cert_len<=0 || cert_len>(int)sizeof(web.identity.cert))return 15;
    memcpy(web.identity.cert,cert_buf+sizeof(cert_buf)-cert_len,(size_t)cert_len);
    web.identity.cert_len=(size_t)cert_len;
    mbedtls_x509write_crt_free(&writer);
    mbedtls_pk_free(&writer_key);
    if(psa_destroy_key(writer_id)!=PSA_SUCCESS)return 16;
    if(!start_listeners()){
        fprintf(stderr,"real SDK parser rejected production HTTPS key selection: %d\n",
                legacy_parse_result);
        return 3;
    }
    if(!web.started || https_starts!=1 || psa_wrap_result!=0)return 4;
    stop_listeners();
    if(https_stops!=1 || web.tls_key_id!=PSA_KEY_ID_NULL)return 5;
    psa_key_attributes_t old_key=PSA_KEY_ATTRIBUTES_INIT;
    if(psa_get_key_attributes(last_tls_key_id,&old_key)==PSA_SUCCESS)return 6;
    /* Existing DER identities keep the SDK's original parser path. */
    FILE *legacy=fopen(argv[1],"rb");
    if(!legacy)return 7;
    size_t n=fread(web.identity.key,1,sizeof(web.identity.key),legacy);
    if(fclose(legacy)!=0 || n<=32 || n==sizeof(web.identity.key))return 8;
    web.identity.key_len=n;
    if(!start_listeners() || legacy_parse_result!=0 || web.tls_key_id!=PSA_KEY_ID_NULL)return 9;
    stop_listeners();
    /* Failed listener startup must release an imported key before retry. */
    memset(web.identity.key,0,sizeof(web.identity.key));
    web.identity.key[31]=1;
    web.identity.key_len=32;
    force_https_failure=true;
    if(start_listeners() || web.tls_key_id!=PSA_KEY_ID_NULL)return 10;
    if(psa_get_key_attributes(last_tls_key_id,&old_key)==PSA_SUCCESS)return 11;
    puts("real SDK key parser and PSA TLS handoff PASS");
    return 0;
}
'''


def main():
    idf = Path(os.environ["IDF_PATH"])
    build = Path(os.environ["KATMED_MBEDTLS_HOST_BUILD"])
    mbedtls = idf / "components/mbedtls/mbedtls"
    text = SOURCE.read_text()
    c = HOST + function(text, "static bool stop_listeners(void)") + "\n" + function(
        text, "static bool start_listeners(void)"
    ) + END
    with tempfile.TemporaryDirectory(prefix="katmed-tls-key-") as tmp:
        program = Path(tmp) / "check.c"
        binary = Path(tmp) / "check"
        private_pem = Path(tmp) / "synthetic.pem"
        private_der = Path(tmp) / "synthetic.der"
        subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey",
                        "-noout", "-out", str(private_pem)], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run(["openssl", "pkey", "-in", str(private_pem),
                        "-outform", "DER", "-out", str(private_der)],
                       check=True, stdout=subprocess.DEVNULL)
        program.write_text(c)
        includes = [
            idf / "components/mbedtls/port/include",
            Path(os.environ.get("KATMED_MBEDTLS_HOST_CONFIG", build / "host-include")),
            mbedtls / "include",
            mbedtls / "tf-psa-crypto/include",
            mbedtls / "tf-psa-crypto/drivers/builtin/include",
            idf / "components/esp_security/include",
        ]
        libraries = [
            build / "library/libmbedtls.a",
            build / "library/libmbedx509.a",
            build / "library/libmbedcrypto.a",
            build / "library/libtfpsacrypto.a",
            build / "tf-psa-crypto/core/libtfpsacrypto.a",
            build / "tf-psa-crypto/drivers/builtin/libbuiltin.a",
            build / "tf-psa-crypto/drivers/p256-m/libp256-m.a",
            build / "tf-psa-crypto/drivers/everest/libeverest.a",
            build / "tf-psa-crypto/drivers/pqcp/libpqcp.a",
            build / "tf-psa-crypto/extras/libextras.a",
            build / "tf-psa-crypto/utilities/libutilities.a",
        ]
        cc = ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function"]
        cc += ["-I" + str(path) for path in includes]
        cc += [str(program), "-Wl,--start-group", *map(str, libraries), "-Wl,--end-group", "-pthread", "-o", str(binary)]
        subprocess.run(cc, check=True)
        subprocess.run([str(binary), str(private_der)], check=True)


if __name__ == "__main__":
    main()
