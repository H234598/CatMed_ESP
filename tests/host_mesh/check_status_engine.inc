    if(argc==2&&(std::strcmp(argv[1],"status_root")==0||
                std::strcmp(argv[1],"status_leaf")==0||std::strcmp(argv[1],"status_fair")==0)){
        bool leaf=std::strcmp(argv[1],"status_leaf")==0;
        bool fair=std::strcmp(argv[1],"status_fair")==0;
        level=leaf?2:ROOT;katmed_mesh_poll(&now);
        auto challenge=latest_kind(MESH_HELLO);
        std::memcpy(current_local_nonce,challenge.session_nonce,16);
        auto hello_at=[&](uint64_t device,uint64_t counter){
            auto f=peer_frame(MESH_HELLO,counter);f.sender=f.id.device=device;
            assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,
                f.payload,sizeof(f.payload),&f.payload_length));
            inject(f);katmed_mesh_poll(&now);
        };
        auto status_at=[&](MeshHistory &remote,uint64_t device,uint64_t counter,uint8_t flags,uint8_t page=0){
            auto f=peer_frame(MESH_STATUS,counter);f.sender=f.id.device=device;
            uint64_t members[2]{11,device};
            f.payload_length=mesh_status_encode(&remote,uint8_t(flags|(leaf?MS_CURRENT:0)),page,
                leaf?device:11,members,leaf?2:0,f.payload);
            inject(f);katmed_mesh_poll(&now);return f;
        };
        KatmedMeshCounts counts{};
        if(fair){
            unsigned masks[8]{};uint64_t counters[8]{};
            for(unsigned i=0;i<8;++i){counters[i]=1;hello_at(1000+i,counters[i]);}
            for(unsigned tick=1;tick<=24;++tick){
                advance_now(1000+1000*tick);
                // Keep HELLO membership fresh independently of status sends.
                for(unsigned i=0;i<8;++i)hello_at(1000+i,++counters[i]);
                // At most the first poll of a tick can emit the extra status.
                katmed_mesh_poll(&now);
                unsigned n=0;
                for(const auto &raw:sent){KatmedMeshFrame f{};
                    if(katmed_mesh_wire_decode(raw.data(),raw.size(),provision.group,provision.secret,&f)&&
                       f.kind==MESH_STATUS){
                        assert(f.recipient>=1000&&f.recipient<1008);
                        masks[f.recipient-1000]|=1u<<f.payload[2];++n;
                    }
                }
                assert(n==tick);
            }
            for(unsigned m:masks)assert(m==7);
            katmed_mesh_counts(&counts);assert(counts.current==9&&(counts.quality&MS_CAPACITY));
            auto before=counts.history;hello_at(2000,1);katmed_mesh_counts(&counts);
            assert(counts.history==before&&(counts.quality&MS_CAPACITY));
            std::puts("global status fair 8-peer 3-page 24-tick bounded sends/capacity OK");return 0;
        }
        hello_at(99,1);katmed_mesh_counts(&counts);
        assert(counts.history==2&&!counts.history_complete);
        MeshHistory remote{};remote.generation=1;mesh_history_add(&remote,11);mesh_history_add(&remote,99);
        advance_now(16000);hello_at(99,2);
        auto f=status_at(remote,99,3,MS_PREHISTORY);
        katmed_mesh_counts(&counts);assert(counts.current==2&&counts.current_valid&&counts.history_complete);
        const unsigned writes=history_memory.commits;
        f.session_counter=4;inject(f);katmed_mesh_poll(&now);assert(history_memory.commits==writes);
        inject(f);katmed_mesh_poll(&now);assert(history_memory.commits==writes); // replay
        auto foreign=f;foreign.group[0]^=1;foreign.session_counter=5;
        inject(foreign);katmed_mesh_poll(&now);assert(history_memory.commits==writes);
        if(leaf){
            advance_now(17000);hello_at(99,5);status_at(remote,99,6,MS_PREHISTORY|MS_STORE_ERROR);
            katmed_mesh_counts(&counts);assert(counts.quality&MS_STORE_ERROR);
            advance_now(18000);hello_at(99,7);
            auto outgoing=latest_kind(MESH_STATUS);
            assert(!(outgoing.payload[1]&(MS_STORE_ERROR|MS_REBUILD))); // no root warning echo
            status_at(remote,99,8,MS_PREHISTORY);katmed_mesh_counts(&counts);
            assert(!(counts.quality&MS_STORE_ERROR)&&counts.history_complete);
        }else{
            history_memory.fail=true;mesh_history_add(&remote,77);
            status_at(remote,99,5,MS_PREHISTORY);katmed_mesh_counts(&counts);
            assert(counts.history==3&&(counts.quality&MS_STORE_ERROR));
            unsigned failed=history_memory.commits;
            status_at(remote,99,6,MS_PREHISTORY);assert(history_memory.commits==failed);
            history_memory.fail=false;mesh_history_add(&remote,88);
            status_at(remote,99,7,MS_PREHISTORY);katmed_mesh_counts(&counts);
            assert(counts.history==4&&!(counts.quality&MS_STORE_ERROR)&&counts.history_complete);
            status_at(remote,99,8,MS_PREHISTORY|MS_CAPACITY);katmed_mesh_counts(&counts);
            assert(counts.quality&MS_CAPACITY);
            status_at(remote,99,9,MS_PREHISTORY);katmed_mesh_counts(&counts);
            assert(!(counts.quality&MS_CAPACITY));
        }
        const uint64_t received=now.monotonic_ms;
        advance_now(received+14999);hello_at(99,20);katmed_mesh_counts(&counts);
        assert(counts.history_complete);
        advance_now(received+15000);hello_at(99,21);katmed_mesh_counts(&counts);
        assert(!counts.history_complete&&(counts.quality&MS_REBUILD));
        status_at(remote,99,22,MS_PREHISTORY);katmed_mesh_counts(&counts);assert(counts.history_complete);
        advance_now(now.monotonic_ms+15000);katmed_mesh_counts(&counts); // poll clock first
        katmed_mesh_poll(&now);katmed_mesh_counts(&counts);
        if(!leaf)assert(counts.current==1);else assert(!counts.current_valid);
        hello_at(99,23);status_at(remote,99,24,MS_PREHISTORY);katmed_mesh_counts(&counts);
        assert(counts.current==2&&counts.history_complete);
        level=leaf?ROOT:2;katmed_mesh_poll(&now);katmed_mesh_counts(&counts);
        assert(!counts.history_complete&&(counts.quality&MS_REBUILD));
        std::puts(leaf?"global leaf freshness/quality direction/root switch OK":
                          "global root freshness/duplicate/auth/replay/persist failure/recovery OK");
        return 0;
    }
