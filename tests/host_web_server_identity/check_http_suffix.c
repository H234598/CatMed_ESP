static char *json_body(const char *anchor, const char *chain, const unsigned char *key, const char *password)
{
    cJSON *obj=cJSON_CreateObject(); char key_hex[65];
    if (anchor) cJSON_AddStringToObject(obj,"anchor_pem",anchor);
    if (chain) {
        for(size_t i=0;i<32;i++) snprintf(key_hex+2*i,3,"%02x",key[i]);
        cJSON_AddStringToObject(obj,"chain_pem",chain); cJSON_AddStringToObject(obj,"key_hex",key_hex);
    }
    cJSON_AddStringToObject(obj,"password",password);
    char *text=cJSON_PrintUnformatted(obj); cJSON_Delete(obj); return text;
}
static httpd_req_t request(const char *body, const char *cookie, const char *csrf)
{
    return (httpd_req_t){.handle=web.https,.host="device.example",.origin="https://device.example",
        .cookie=cookie,.csrf=csrf,.body=body,.type="application/json",.content_len=strlen(body)};
}
typedef struct { httpd_req_t *request; } ThreadRequest;
static void *identity_post_thread(void *context)
{
    ThreadRequest *call = context;
    server_identity_post(call->request);
    return NULL;
}
static void *anchor_post_thread(void *context)
{
    ThreadRequest *call = context;
    pthread_mutex_lock(&race_mutex);
    while (!anchor_update_go) pthread_cond_wait(&race_condition, &race_mutex);
    pthread_mutex_unlock(&race_mutex);
    server_anchor_post(call->request);
    pthread_mutex_lock(&race_mutex);
    anchor_update_done = true;
    pthread_cond_broadcast(&race_condition);
    pthread_mutex_unlock(&race_mutex);
    return NULL;
}
static void run_anchor_replacement_interleaving(const char *identity_json,
        const char *anchor_json, const char *cookie, const char *csrf)
{
    pthread_mutex_lock(&race_mutex);
    validation_paused = false;
    validation_release = false;
    anchor_update_go = false;
    anchor_update_waiting = false;
    anchor_update_acquired = false;
    anchor_update_done = false;
    anchor_updater_thread_ready = false;
    anchor_race_enabled = true;
    pthread_mutex_unlock(&race_mutex);
    httpd_req_t identity_request = request(identity_json, cookie, csrf);
    httpd_req_t anchor_request = request(anchor_json, cookie, csrf);
    ThreadRequest identity_call = {.request = &identity_request};
    ThreadRequest anchor_call = {.request = &anchor_request};
    pthread_t identity_thread, anchor_thread;
    assert(pthread_create(&identity_thread, NULL, identity_post_thread, &identity_call) == 0);
    pthread_mutex_lock(&race_mutex);
    while (!validation_paused) pthread_cond_wait(&race_condition, &race_mutex);
    pthread_mutex_unlock(&race_mutex);
    assert(pthread_create(&anchor_thread, NULL, anchor_post_thread, &anchor_call) == 0);
    pthread_mutex_lock(&race_mutex);
    anchor_updater_thread = anchor_thread;
    anchor_updater_thread_ready = true;
    anchor_update_go = true;
    pthread_cond_broadcast(&race_condition);
    while (!anchor_update_waiting) pthread_cond_wait(&race_condition, &race_mutex);
    if (anchor_update_acquired)
        while (!anchor_update_done) pthread_cond_wait(&race_condition, &race_mutex);
    validation_release = true;
    pthread_cond_broadcast(&race_condition);
    pthread_mutex_unlock(&race_mutex);
    assert(pthread_join(identity_thread, NULL) == 0);
    assert(pthread_join(anchor_thread, NULL) == 0);
    assert(!strcmp(identity_request.status, "202 Accepted"));
    assert(!strcmp(anchor_request.status, "503 Service Unavailable"));
    pthread_mutex_lock(&race_mutex);
    anchor_race_enabled = false;
    pthread_mutex_unlock(&race_mutex);
}
int main(int argc,char **argv)
{
    assert(argc==8); unsigned char key[33],chain[6144],anchor[2048],other[2048],key2[33],chain2[6144];
    size_t kl=read_file(argv[1],key,sizeof(key));size_t cl=read_file(argv[2],chain,sizeof(chain)-1);
    size_t al=read_file(argv[3],anchor,sizeof(anchor)-1);size_t ol=read_file(argv[4],other,sizeof(other)-1);
    assert(kl==32);chain[cl]=anchor[al]=other[ol]=0;security_now=strtoll(argv[5],NULL,10);
    assert(read_file(argv[6],key2,sizeof(key2))==32);size_t cl2=read_file(argv[7],chain2,sizeof(chain2)-1);chain2[cl2]=0;
    assert(psa_crypto_init()==PSA_SUCCESS);
    web.nvs=1;web.nvs_open=true;web.identifier_ready=true;strcpy(web.acme_identifier,"device.example");
    web.auth_mutex=(void *)1;web.https=(void *)1;web.redirect=(void *)2;web.started=true;
    web.auth.storage_available=true;web.auth.record.magic=0x4b574131u;web.auth.record.version=1;
    web.auth.record.verifier[0]=7;web.auth.derive=synthetic_derive;
    KatmedWebSession *s=&web.auth.sessions[0];memset(s->token,'a',64);s->token[64]=0;
    memset(s->csrf,'b',64);s->csrf[64]=0;s->active=true;s->created_ms=1000;s->last_ms=1000;
    char cookie[90];snprintf(cookie,sizeof(cookie),"katmed_session=%s",s->token);
    char *anchor_body=json_body((char *)anchor,NULL,NULL,"synthetic-password");
    httpd_req_t r=request(anchor_body,NULL,s->csrf);server_anchor_post(&r);assert(!strcmp(r.status,"401 Unauthorized"));
    r=request(anchor_body,cookie,s->csrf);r.host="wrong.example";server_anchor_post(&r);assert(!strcmp(r.response,"host"));
    r=request(anchor_body,cookie,s->csrf);r.origin="https://wrong.example";server_anchor_post(&r);assert(!strcmp(r.response,"origin"));
    r=request(anchor_body,cookie,"wrong");server_anchor_post(&r);assert(!strcmp(r.response,"csrf"));
    r=request(anchor_body,cookie,s->csrf);server_anchor_post(&r);assert(!strcmp(r.status,"200 OK"));free(anchor_body);
    char *body=json_body(NULL,(char *)chain,key,"synthetic-password");
    r=request(body,NULL,s->csrf);server_identity_post(&r);assert(!strcmp(r.status,"401 Unauthorized"));
    r=request(body,cookie,s->csrf);r.host="wrong.example";server_identity_post(&r);assert(!strcmp(r.response,"host"));
    r=request(body,cookie,s->csrf);r.origin="https://wrong.example";server_identity_post(&r);assert(!strcmp(r.response,"origin"));
    r=request(body,cookie,"wrong");server_identity_post(&r);assert(!strcmp(r.response,"csrf"));
    char *bad_password=json_body(NULL,(char *)chain,key,"wrong-password");
    r=request(bad_password,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.status,"401 Unauthorized"));free(bad_password);
    key[31]^=1;char *bad_key=json_body(NULL,(char *)chain,key,"synthetic-password");key[31]^=1;
    r=request(bad_key,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.response,"invalid_identity"));free(bad_key);
    int64_t good_time=security_now;security_now=0;r=request(body,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.response,"time_unavailable"));security_now=good_time;
    security_now=good_time+86400*30;r=request(body,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.response,"invalid_identity"));
    security_now=good_time-86400*30;r=request(body,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.response,"invalid_identity"));security_now=good_time;
    strcpy(web.acme_identifier,"wrong.example");r=request(body,cookie,s->csrf);r.host="192.0.2.42";r.origin="https://192.0.2.42";server_identity_post(&r);assert(!strcmp(r.response,"invalid_identity"));strcpy(web.acme_identifier,"device.example");
    char *other_body=json_body((char *)other,NULL,NULL,"synthetic-password");r=request(other_body,cookie,s->csrf);server_anchor_post(&r);assert(!strcmp(r.status,"200 OK"));free(other_body);
    r=request(body,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.response,"invalid_identity"));
    anchor_body=json_body((char *)anchor,NULL,NULL,"synthetic-password");r=request(anchor_body,cookie,s->csrf);server_anchor_post(&r);assert(!strcmp(r.status,"200 OK"));free(anchor_body);
    mock_set_failure=true;r=request(body,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.response,"stage_failed"));mock_set_failure=false;
    mock_commit_failure=true;r=request(body,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.response,"stage_failed"));mock_commit_failure=false;
    mock_read_index=0;r=request(body,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.response,"stage_failed"));mock_read_index=-1;
    char *race_anchor_body=json_body((char *)other,NULL,NULL,"synthetic-password");
    run_anchor_replacement_interleaving(body,race_anchor_body,cookie,s->csrf);
    free(race_anchor_body);
    assert(web.server_pending);
    KatmedServerRecord old={0};unsigned slot;assert(katmed_server_journal_load(1,&old,&slot)==KATMED_SERVER_LEGACY);
    katmed_web_poll(2000);assert(web.started && !web.server_pending && web.server_identity && !strcmp(web.server_result,"active"));
    assert(katmed_server_journal_load(1,&old,&slot)==KATMED_SERVER_ACTIVE);
    assert(!memcmp(old.key,key,32) && old.chain_len==cl+1 && !memcmp(old.chain,chain,cl+1));
    r=request("",cookie,s->csrf);server_identity_get(&r);assert(!strcmp(r.response,"{\"state\":\"active\"}"));
    r=request("",cookie,s->csrf);r.host="192.0.2.42";server_identity_get(&r);assert(!strcmp(r.status,"200 OK"));
    r=request("",NULL,s->csrf);server_identity_get(&r);assert(!strcmp(r.status,"401 Unauthorized"));
    r=request("",cookie,s->csrf);r.host="wrong.example";server_identity_get(&r);assert(!strcmp(r.response,"host"));
    char *second_body=json_body(NULL,(char *)chain2,key2,"synthetic-password");
    r=request(second_body,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.status,"202 Accepted"));
    assert(memcmp(web.server_pending->key,old.key,32)!=0);
    fail_listener_once=true;katmed_web_poll(2000);assert(web.started && !web.server_pending && !strcmp(web.server_result,"failed"));
    KatmedServerRecord after={0};assert(katmed_server_journal_load(1,&after,&slot)==KATMED_SERVER_ACTIVE && !memcmp(&old,&after,sizeof(old)));
    /* A started candidate remains pending until a successful TLS handshake.
     * Without one, its bounded probe window restores the durable prior slot. */
    synthetic_handshake_enabled=false;
    r=request(second_body,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.status,"202 Accepted"));
    katmed_web_poll(2000);assert(web.started && web.server_pending && web.server_previous);
    assert(katmed_server_journal_load(1,&after,&slot)==KATMED_SERVER_ACTIVE && !memcmp(&old,&after,sizeof(old)));
    katmed_web_poll(12000);assert(web.started && !web.server_pending && !web.server_previous && !strcmp(web.server_result,"failed"));
    synthetic_handshake_enabled=true;
    web.auth.record.cert_len=1;s->certificate=true;
    r=request(second_body,cookie,s->csrf);server_identity_post(&r);assert(!strcmp(r.status,"202 Accepted"));
    mock_commit_failure=true;katmed_web_poll(2000);mock_commit_failure=false;
    assert(web.server_quarantined && !web.started && web.server_pending && web.server_previous);
    assert(katmed_server_journal_load(1,&after,&slot)==KATMED_SERVER_ACTIVE && !memcmp(&old,&after,sizeof(old)));
    free(second_body);free(body);free(web.server_pending);free(web.server_previous);
    puts("real HTTP guards/import, journal and memory TLS activation/rollback: PASS");return 0;
}
