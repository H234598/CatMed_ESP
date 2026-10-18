/* Reuse the actual adapter/SDK harness and its fault injection unchanged. */
#define main established_mesh_repro
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type" /* Renamed C++ main loses implicit return 0. */
#include "../host_mesh/check_engine.cpp"
#pragma GCC diagnostic pop
#undef main

int main(int argc,char **argv){
    assert(argc==2);
    const int result=established_mesh_repro(argc,argv);
    assert(result==0);
    KatmedMeshDiagnostics d{};
    assert(katmed_mesh_diagnostics(&d));
    assert(d.observed_level_changes>0);
    assert(d.authenticated_session_up>0);
    if(argc==2&&std::strcmp(argv[1],"send_failure")==0){
        assert(failed_sends>0&&d.sdk_send_failures==failed_sends);
        assert(d.rx_queue_drops==0);
    }else if(argc==2&&std::strcmp(argv[1],"queue_overflow")==0){
        assert(dropped==1&&d.rx_queue_drops==dropped);
        assert(d.sdk_send_failures==0);
    }
    katmed_mesh_stop();
    KatmedMeshDiagnostics stopped{};
    assert(katmed_mesh_diagnostics(&stopped));
    assert(stopped.authenticated_session_down>d.authenticated_session_down);
    std::puts("mesh adapter-observed counters passed");
    return 0;
}
