#define main established_mesh_repro
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include "../host_mesh/check_engine.cpp"
#pragma GCC diagnostic pop
#undef main
int main(){
    KatmedMeshDiagnostics d{};
    assert(!katmed_mesh_diagnostics(&d));
    uint8_t catalog[32]{};catalog[0]=43;
    StoreRecovery recovery{};
    StoreBackend backend{&memory,read_blob,write_blob};
    assert(store_open(&store,backend,11,catalog,75,&recovery)==STORE_OK);
    for(unsigned i=0;i<16;++i)provision.group[i]=uint8_t(i+1);
    for(unsigned i=0;i<32;++i)provision.secret[i]=uint8_t(i+30);
    std::memcpy(provision.time_source,"offline.invalid",15);provision.time_source_length=15;
    provision.configured=true;store.record.provision=provision;
    std::memcpy(store.record.bound_group,provision.group,16);
    assert(app_init(&state,&recovery.shared,&now,recovery.boot_identity,75,catalog)==APP_OK);
    KatmedMeshServices services{&store,&state,nullptr,shared_result,utc_received,nullptr};
    assert(katmed_mesh_start(&provision,&services)==MESH_ACCEPTED);
    assert(katmed_mesh_diagnostics(&d));
    // Direct STA state must distinguish DHCP from Mesh event/root state.
    assert(d.station_valid==0);
    host_station_present=true;host_station_up=true;
    host_station_dhcp=ESP_NETIF_DHCP_STARTED;
    assert(katmed_mesh_diagnostics(&d));
    assert(d.station_valid==1 && d.station_up==1 &&
           d.station_ipv4_nonzero==0 && d.station_dhcp_status==1);
    host_station_ip=0x01020304;host_station_dhcp=ESP_NETIF_DHCP_STOPPED;
    assert(katmed_mesh_diagnostics(&d));
    assert(d.station_ipv4_nonzero==1 && d.station_dhcp_status==2);
    host_station_up=false; // SDK may still return cached IP while down.
    assert(katmed_mesh_diagnostics(&d));
    assert(d.station_up==0 && d.station_ipv4_nonzero==1);
    host_station_dhcp=ESP_NETIF_DHCP_INIT;
    assert(katmed_mesh_diagnostics(&d) && d.station_dhcp_status==0);
    host_station_ip_result=ESP_FAIL;
    assert(katmed_mesh_diagnostics(&d));
    assert(d.station_valid==0 && d.station_up==0 &&
           d.station_ipv4_nonzero==0 && d.station_dhcp_status==0);
    host_station_ip_result=ESP_OK;host_station_dhcp_result=ESP_FAIL;
    assert(katmed_mesh_diagnostics(&d) && d.station_valid==0);
    host_station_dhcp_result=ESP_OK;host_station_present=false;
    assert(katmed_mesh_diagnostics(&d) && d.station_valid==0);
    std::puts("direct STA getter distinguishes no-netif, DHCP states, cached-down IP and API failures");
    return 0;
}
