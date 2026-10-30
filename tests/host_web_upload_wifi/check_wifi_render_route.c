/* Reuse the existing HTTPD route stubs, including auth and response capture. */
#define main retained_wifi_reset_routes_main
#include "check_wifi_reset_routes.c"
#undef main

static unsigned snapshot_reads;
#ifdef KATMED_WEB_WIFI_RENDER_SNAPSHOT
static bool (*real_snapshot)(void *,KatmedWebWifiRenderSnapshot *);
static bool counted_snapshot(void *context,KatmedWebWifiRenderSnapshot *out)
{
    ++snapshot_reads;
    return real_snapshot(context,out);
}
#endif
unsigned wifi_render_snapshot_reads(void) { return snapshot_reads; }
void wifi_render_route_register(KatmedWebWifiServices services)
{
    routes[0]=(httpd_uri_t){0};
    route_count=0;
    services.authorize=auth;
#ifdef KATMED_WEB_WIFI_RENDER_SNAPSHOT
    real_snapshot=services.render_snapshot;
    snapshot_reads=0;
    if (real_snapshot) services.render_snapshot=counted_snapshot;
#endif
    assert(katmed_web_wifi_register((httpd_handle_t)1,&services));
}

int wifi_render_route_get(bool authenticated, bool *profile_present)
{
    httpd_req_t req;
    req_init(&req,NULL);
    req.authenticated=authenticated;
    assert(route("/api/wifi",HTTP_GET)(&req)==ESP_OK);
    if (authenticated) assert(req.response_complete);
    if (profile_present)
        *profile_present=strstr(req.response,"synthetic-network")!=NULL;
    if (!strcmp(req.response_status,"200 OK")) {
        assert(strstr(req.response,"password")==NULL);
        return 200;
    }
    if (!strcmp(req.response_status,"401 Unauthorized")) return 401;
    if (!strcmp(req.response_status,"503 Service Unavailable")) return 503;
    return 0;
}
