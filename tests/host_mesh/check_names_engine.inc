/* Real root IDF adapter, authenticated outer frames, real new NamesStore on
 * both ends. The remote node drives the same durable API as the leaf caller;
 * this test is not a second allocator or a map of expected production state. */
if(argc==2&&std::strcmp(argv[1],"names_lifetime")==0){
    NamesMemory root_bytes;
    uint64_t lifetime_max_poll_reads=0,max_read_bytes=0,max_write_bytes=0,max_writes=0,max_syncs=0,max_commits=0;
    auto lifetime_poll=[&](){
        const uint64_t reads=root_bytes.file_reads,rb=root_bytes.file_read_bytes,wb=root_bytes.file_write_bytes,writes=root_bytes.file_writes,sy=root_bytes.syncs,cm=root_bytes.commits;
        katmed_mesh_poll(&now);
        const uint64_t r=root_bytes.file_reads-reads,b=root_bytes.file_read_bytes-rb,w=root_bytes.file_write_bytes-wb,ws=root_bytes.file_writes-writes,ys=root_bytes.syncs-sy,cs=root_bytes.commits-cm;
        if(r>lifetime_max_poll_reads)lifetime_max_poll_reads=r;
        if(b>max_read_bytes)max_read_bytes=b;
        if(w>max_write_bytes)max_write_bytes=w;
        if(ws>max_writes)max_writes=ws;
        if(ys>max_syncs)max_syncs=ys;
        if(cs>max_commits)max_commits=cs;
        assert(r<=32&&b<=32*512);
    };
    assert(katmed_mesh_names_backend(root_bytes.backend(),store.record.device,provision.group)==NAMES_OK);
    level=ROOT;host_station_present=true;
    for(unsigned i=0;i<4;++i){lifetime_poll();advance_now(now.monotonic_ms+1);}
    char local[32];bool assigned=false,uncertain=true;
    katmed_mesh_device_name(local,sizeof(local),&assigned,&uncertain);
    assert(!std::strcmp(local,"KatmedESP-001")&&assigned&&!uncertain);
    const unsigned root_boots=boot_random;
    uint64_t first_remote=0;NamesMemory first_bytes;
    for(uint64_t ordinal=2;ordinal<=1002;++ordinal){
        const uint64_t remote=ordinal==1002?first_remote:100000+ordinal;
        if(!first_remote)first_remote=remote;
        NamesMemory remote_bytes;if(ordinal==1002)remote_bytes=first_bytes;NamesStore remote_store{};
        assert(names_open(&remote_store,remote_bytes.backend(),remote,provision.group)==NAMES_OK);
        uint64_t counter=1;uint8_t remote_nonce[16];
        for(unsigned j=0;j<16;++j)remote_nonce[j]=uint8_t(ordinal+j+7);
        auto make=[&](KatmedMeshKind kind){auto f=peer_frame(kind,counter++);f.sender=f.id.device=remote;std::memcpy(f.session_nonce,remote_nonce,16);return f;};
        /* Authenticated old sessions expire through the real lease/rekey
         * caller. The root process and durable registry are not restarted. */
        advance_now(now.monotonic_ms+30001);
        for(unsigned handshake=0;handshake<3;++handshake){
            auto challenge=latest_kind(MESH_HELLO);std::memcpy(current_local_nonce,challenge.session_nonce,16);
            auto hello=make(MESH_HELLO);
            assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,hello.payload,sizeof(hello.payload),&hello.payload_length));
            inject(hello);lifetime_poll();advance_now(now.monotonic_ms+1);
        }
        auto respond=[&](const NamesMessage &message){auto f=make(MESH_NAME_SYNC);size_t n=0;assert(names_wire_encode(&message,nullptr,0,f.payload,sizeof(f.payload),&n));f.payload_length=uint16_t(n);inject(f);};
        NamesMessage caps{};caps.opcode=NAMES_CAPS;respond(caps);
        size_t scan=sent.size();
        bool complete=false;uint8_t receiver_base[80]{};const uint8_t *published=names_published(&remote_store);if(published)memcpy(receiver_base,published,80);
        for(unsigned step=0;step<150&&!complete;++step){
            lifetime_poll();advance_now(now.monotonic_ms+10);
            const size_t end=sent.size();
            for(;scan<end;++scan){
                KatmedMeshFrame f{};if(!katmed_mesh_wire_decode(sent[scan].data(),sent[scan].size(),provision.group,provision.secret,&f)||f.kind!=MESH_NAME_SYNC||f.recipient!=remote)continue;
                NamesMessage m{};assert(names_wire_decode(f.payload,f.payload_length,&m));
                if(m.opcode==NAMES_CAPS)continue;
                if(m.opcode==NAMES_MANIFEST){
                    if(remote_store.fresh&&m.reserved_device!=remote){m.opcode=NAMES_REQUEST;std::memset(m.base,0,80);respond(m);continue;}
                    const uint8_t *old=names_published(&remote_store);const bool append=old&&!memcmp(old+16,m.target+16,16);
                    assert(names_receive_begin(&remote_store,m.registry,m.target,append,m.controller,m.epoch,m.barrier,m.barrier_count,m.reserved_device,m.reserved_index)==NAMES_OK);
                    m.opcode=names_phase(&remote_store)==NAMES_PREPARED?NAMES_PREPARED_ACK:NAMES_REQUEST;
                    if(old)memcpy(m.base,old,80);else std::memset(m.base,0,80);m.progress=uint32_t(remote_store.ctl[52])|(uint32_t(remote_store.ctl[53])<<8);respond(m);
                }else if(m.opcode==NAMES_PAGE){
                    uint64_t ids[89];size_t n=(f.payload_length-328)/8;
                    for(size_t j=0;j<n;++j)ids[j]=names_wire_slot(f.payload,j);
                    assert(names_receive_page(&remote_store,m.start,ids,n)==NAMES_OK);
                    m.opcode=names_phase(&remote_store)==NAMES_PREPARED?NAMES_PREPARED_ACK:NAMES_REQUEST;
                    memcpy(m.base,receiver_base,80);m.start=0;m.progress=uint32_t(remote_store.ctl[52])|(uint32_t(remote_store.ctl[53])<<8)|(uint32_t(remote_store.ctl[54])<<16)|(uint32_t(remote_store.ctl[55])<<24);memcpy(m.progress_hash,remote_store.ctl+56,32);
                    respond(m);
                }else if(m.opcode==NAMES_COMMIT){
                    assert(names_publish(&remote_store)==NAMES_OK);
                    assert(names_confirm(&remote_store,names_published(&remote_store))==NAMES_OK);
                    assert(names_finish(&remote_store)==NAMES_OK);
                    m.opcode=NAMES_COMMIT_ACK;memcpy(m.progress_hash,m.target+32,32);m.progress=uint32_t(m.target[64])|(uint32_t(m.target[65])<<8);memcpy(m.base,receiver_base,80);respond(m);complete=true;
                }
            }
        }
        if(!complete)std::fprintf(stderr,"incomplete natural ordinal %llu\n",(unsigned long long)ordinal);
        assert(complete);
        lifetime_poll();advance_now(now.monotonic_ms+1);
        char visible[32],expected[32];bool a=false,u=true;
        names_visible(&remote_store,visible,&a,&u);std::snprintf(expected,sizeof(expected),"KatmedESP-%03u",unsigned(ordinal==1002?2:ordinal));assert(a&&!u&&!std::strcmp(visible,expected));
        if(ordinal==64||ordinal==65||ordinal==999||ordinal==1000||ordinal==1001)std::printf("natural root/mesh/store ordinal %llu confirmed\n",(unsigned long long)ordinal);
        if(ordinal==2)first_bytes=remote_bytes;
        // Bound harness captures; production retains every identity in bytes.
        if(sent.size()>100)sent.erase(sent.begin(),sent.end()-100);
    }
    NamesStore reopened{};assert(names_open(&reopened,root_bytes.backend(),store.record.device,provision.group)==NAMES_OK);
    assert(names_cursor(&reopened)==1002);uint16_t index=0,pending=0;
    assert(names_lookup(&reopened,first_remote,&index,&pending)==NAMES_OK&&index==2&&!pending);
    assert(boot_random>root_boots); // challenge rotations occurred, no start/reboot.
    std::printf("natural 1001 names (root plus 1000 distinct peers), live root adapter/wire/store/churn, absent peer rejoin without new index OK; max root poll file reads=%llu readbytes=%llu writes=%llu writebytes=%llu syncs=%llu commits=%llu\n",(unsigned long long)lifetime_max_poll_reads,(unsigned long long)max_read_bytes,(unsigned long long)max_writes,(unsigned long long)max_write_bytes,(unsigned long long)max_syncs,(unsigned long long)max_commits);return 0;
}
