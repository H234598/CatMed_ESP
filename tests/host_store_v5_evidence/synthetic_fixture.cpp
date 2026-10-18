// Deliberately synthetic record, encoded by the unchanged product serializer.
#include "../../firmware/components/katmed_store/katmed_store.cpp"
#include <cstdio>

int main() {
    StoreRecord r{};
    r.device=17; r.epoch=3; r.next_sequence=8;
    r.image_count=75;
    r.shared=app_default_settings();
    r.shared.revision.coordinator=r.device;
    r.local={ORIENTATION_90,ANALOG,r.device};
    r.provision.configured=true;
    for (unsigned i=0;i<16;++i) r.provision.group[i]=r.bound_group[i]=static_cast<uint8_t>(i+1);
    for (unsigned i=0;i<32;++i) r.provision.secret[i]=static_cast<uint8_t>(0xa0+i);
    const char ssid[]="synthetic-net", password[]="synthetic-pass", ntp[]="time.example";
    r.provision.ssid_length=sizeof(ssid)-1;
    r.provision.password_length=sizeof(password)-1;
    r.provision.time_source_length=sizeof(ntp)-1;
    std::memcpy(r.provision.ssid,ssid,sizeof(ssid)-1);
    std::memcpy(r.provision.password,password,sizeof(password)-1);
    std::memcpy(r.provision.time_source,ntp,sizeof(ntp)-1);
    r.last_confirmation.valid=true;
    r.last_confirmation.id={17,2,5};
    r.last_confirmation.round={{17,2,1},1};
    r.last_confirmation.occurred_at={1000,2,1700000000000,true,CLOCK_SYNCHRONIZED};
    r.pending_count=1;
    r.pending[0]={{17,3,6},17,{{17,3,1},2},{1,17},{2000,3,0,false,CLOCK_UNAVAILABLE}};
    uint8_t bytes[KATMED_STORE_BLOB_MAX]{};
    size_t n=0;
    if (encode_record(r,bytes,&n)!=STORE_OK) return 1;
    return std::fwrite(bytes,1,n,stdout)==n ? 0 : 1;
}
