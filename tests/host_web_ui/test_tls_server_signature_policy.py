"""Run actual installed Mbed TLS over memory BIOs with synthetic material only.

Use the same IDF_PATH/KATMED_MBEDTLS_HOST_BUILD as test_tls_server_key_format.
No sockets, serial port, device keys, or firmware build are involved.
"""

import test_tls_server_key_format as fixture


MEMORY_HANDSHAKE = r'''
/* Fixed-size synthetic transport: no operating-system socket is opened. */
typedef struct { unsigned char bytes[65536]; size_t length; } TestQueue;
typedef struct { TestQueue *in, *out; } TestEndpoint;
static int test_send(void *ctx, const unsigned char *data, size_t size) {
    TestEndpoint *ep=ctx;
    if(size>sizeof(ep->out->bytes)-ep->out->length)return MBEDTLS_ERR_SSL_WANT_WRITE;
    memcpy(ep->out->bytes+ep->out->length,data,size);ep->out->length+=size;
    return (int)size;
}
static int test_recv(void *ctx, unsigned char *data, size_t size) {
    TestEndpoint *ep=ctx;
    if(!ep->in->length)return MBEDTLS_ERR_SSL_WANT_READ;
    if(size>ep->in->length)size=ep->in->length;
    memcpy(data,ep->in->bytes,size);ep->in->length-=size;
    memmove(ep->in->bytes,ep->in->bytes+size,ep->in->length);
    return (int)size;
}
static int synthetic_handshake(mbedtls_ssl_config *server_conf,
                               mbedtls_pk_context *server_key) {
    /* Exactly the enabled native PK capability used in ssl_pick_cert. */
    int capable=mbedtls_pk_can_do_psa(server_key,
        MBEDTLS_PK_ALG_ECDSA(PSA_ALG_SHA_256),PSA_KEY_USAGE_SIGN_HASH);
    printf("native TLS ECDSA capability: %d\n",capable);
    static const int suites[]={MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,0};
    mbedtls_ssl_context server,client;
    mbedtls_ssl_config client_conf;
    mbedtls_ssl_init(&server);mbedtls_ssl_init(&client);
    mbedtls_ssl_config_init(&client_conf);
    int ret=mbedtls_ssl_config_defaults(&client_conf,MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&client_conf,MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_min_tls_version(&client_conf,MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&client_conf,MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_min_tls_version(server_conf,MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(server_conf,MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_ciphersuites(&client_conf,suites);
    mbedtls_ssl_conf_ciphersuites(server_conf,suites);
    if(!ret)ret=mbedtls_ssl_setup(&server,server_conf);
    if(!ret)ret=mbedtls_ssl_setup(&client,&client_conf);
    TestQueue c2s={0},s2c={0};
    TestEndpoint server_io={&c2s,&s2c},client_io={&s2c,&c2s};
    mbedtls_ssl_set_bio(&server,&server_io,test_send,test_recv,NULL);
    mbedtls_ssl_set_bio(&client,&client_io,test_send,test_recv,NULL);
    int server_done=0,client_done=0;
    for(unsigned i=0;!ret && i<1000 && (!server_done || !client_done);++i) {
        if(!client_done) {
            int r=mbedtls_ssl_handshake(&client);
            if(!r)client_done=1;
            else if(r!=MBEDTLS_ERR_SSL_WANT_READ && r!=MBEDTLS_ERR_SSL_WANT_WRITE)ret=r;
        }
        if(!ret && !server_done) {
            int r=mbedtls_ssl_handshake(&server);
            if(!r)server_done=1;
            else if(r!=MBEDTLS_ERR_SSL_WANT_READ && r!=MBEDTLS_ERR_SSL_WANT_WRITE) {
                printf("synthetic server handshake returned -0x%04X\n",-r);ret=r;
            }
        }
    }
    if(!ret && (!server_done || !client_done))ret=MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if(!ret && !capable)ret=MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if(!ret)puts("actual installed TLS1.2 ECDHE-ECDSA handshake: PASS");
    mbedtls_ssl_free(&client);mbedtls_ssl_free(&server);
    mbedtls_ssl_config_free(&client_conf);
    return ret;
}
'''


def main():
    marker = "static int httpd_ssl_start(void **handle,const httpd_ssl_config_t *cfg){"
    fixture.HOST = fixture.HOST.replace(marker, MEMORY_HANDSHAKE + marker)
    marker = "            mbedtls_ssl_config_free(&conf);"
    assert fixture.HOST.count(marker) == 1
    fixture.HOST = fixture.HOST.replace(
        marker,
        "            if(psa_wrap_result==0)psa_wrap_result=synthetic_handshake(&conf,&pk);\n"
        + marker,
    )
    fixture.main()


if __name__ == "__main__":
    main()
