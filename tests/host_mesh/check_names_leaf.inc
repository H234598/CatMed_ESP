if(argc==2&&(!std::strcmp(argv[1],"names_leaf")||!std::strcmp(argv[1],"names_successor")||!std::strcmp(argv[1],"names_mixed")||!std::strcmp(argv[1],"names_mixed_fresh")||!std::strcmp(argv[1],"names_prepare_old")||!std::strcmp(argv[1],"names_prepare_new"))){
    host_station_present=true;
    NamesMemory leaf_bytes,root_bytes;NamesStore remote{};const uint8_t reg[16]={37};
    assert(names_open(&remote,root_bytes.backend(),99,provision.group)==NAMES_OK);
    assert(names_initialize(&remote,reg)==NAMES_OK);assert(names_publish(&remote)==NAMES_OK);assert(names_finish(&remote)==NAMES_OK);
    assert(names_reserve(&remote,99,nullptr,0)==NAMES_OK);assert(names_publish(&remote)==NAMES_OK);assert(names_confirm(&remote,names_published(&remote))==NAMES_OK);assert(names_finish(&remote)==NAMES_OK);
    uint64_t members[]={11};assert(names_reserve(&remote,11,members,1)==NAMES_OK);
    assert(katmed_mesh_names_backend(leaf_bytes.backend(),11,provision.group)==NAMES_OK);
    level=ROOT+1;katmed_mesh_poll(&now);uint64_t counter=1;
    auto handshake=[&](){auto challenge=latest_kind(MESH_HELLO);memcpy(current_local_nonce,challenge.session_nonce,16);auto f=peer_frame(MESH_HELLO,counter++);assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,f.payload,sizeof(f.payload),&f.payload_length));inject(f);katmed_mesh_poll(&now);if(!strcmp(argv[1],"names_mixed_fresh"))return;NamesMessage caps{};caps.opcode=NAMES_CAPS;auto c=peer_frame(MESH_NAME_SYNC,counter++);size_t n=0;assert(names_wire_encode(&caps,nullptr,0,c.payload,sizeof(c.payload),&n));c.payload_length=uint16_t(n);inject(c);katmed_mesh_poll(&now);};
    auto legacy=[&](){auto f=peer_frame(MESH_NAME_MAP,counter++);StoreMeshNameEntry entries[]={{99,1},{11,99}};assert(katmed_mesh_encode_name_map(entries,2,f.payload,sizeof(f.payload),&f.payload_length));inject(f);katmed_mesh_poll(&now);};
    handshake();
    if(!strcmp(argv[1],"names_mixed_fresh")){unsigned before=leaf_bytes.commits;legacy();char name[32];bool assigned=true,uncertain=false;katmed_mesh_device_name(name,sizeof(name),&assigned,&uncertain);assert(!strcmp(name,"KatmedESP")&&!assigned&&uncertain&&leaf_bytes.commits==before);assert(leaf_bytes.keys[0].empty()&&!leaf_bytes.exists[0]&&!leaf_bytes.exists[1]);puts("unsupported old coordinator: fresh node stays KatmedESP, explicit uncertainty and no old-map fallback OK");return 0;}
    NamesMessage m{};m.opcode=NAMES_MANIFEST;m.flags=2|16;m.controller=99;m.epoch=1;memcpy(m.registry,reg,16);memcpy(m.target,names_target(&remote),80);m.reserved_device=11;m.reserved_index=2;m.barrier_count=1;m.barrier[0]=11;m.barrier_flags=1;
    auto deliver=[&](const NamesMessage &message,const uint64_t *ids=nullptr,size_t count=0){auto f=peer_frame(MESH_NAME_SYNC,counter++);size_t n=0;assert(names_wire_encode(&message,ids,count,f.payload,sizeof(f.payload),&n));f.payload_length=uint16_t(n);inject(f);katmed_mesh_poll(&now);advance_now(now.monotonic_ms+1);};
    deliver(m);m.opcode=NAMES_PAGE;m.start=0;uint64_t first=99;deliver(m,&first,1);
    char value[32];bool a=false,u=false;katmed_mesh_device_name(value,sizeof(value),&a,&u);assert(!a&&u&&!strcmp(value,"KatmedESP"));
    // Actual radio-root transition with known partial T cannot allocate or
    // turn a file prefix into complete P. Persistent projection remains old.
    const auto partial=leaf_bytes.keys[0];level=ROOT;advance_now(now.monotonic_ms+1);katmed_mesh_poll(&now);assert(leaf_bytes.keys[1].empty());
    level=ROOT+1;advance_now(now.monotonic_ms+1);katmed_mesh_poll(&now);handshake();
    m.opcode=NAMES_MANIFEST;m.start=0;deliver(m);m.opcode=NAMES_PAGE;m.start=1;uint64_t self=11;
    const bool prepare_unknown=!strcmp(argv[1],"names_prepare_old")||!strcmp(argv[1],"names_prepare_new");
    auto prepared_count=[&](){unsigned count=0;for(auto &bytes:sent){KatmedMeshFrame f{};NamesMessage msg{};if(katmed_mesh_wire_decode(bytes.data(),bytes.size(),provision.group,provision.secret,&f)&&f.kind==MESH_NAME_SYNC&&names_wire_decode(f.payload,f.payload_length,&msg)&&msg.opcode==NAMES_PREPARED_ACK)++count;}return count;};
    unsigned acknowledgements=prepared_count();
    if(prepare_unknown){leaf_bytes.applies=!strcmp(argv[1],"names_prepare_new");leaf_bytes.unknown_on_commit=leaf_bytes.commits+2;}
    deliver(m,&self,1);
    if(prepare_unknown){assert(prepared_count()==acknowledgements);leaf_bytes.unknown_on_commit=0;katmed_mesh_poll(&now);assert(prepared_count()==acknowledgements);handshake();m.opcode=NAMES_MANIFEST;m.start=0;deliver(m);assert(prepared_count()>acknowledgements);}

    katmed_mesh_device_name(value,sizeof(value),&a,&u);assert(!a&&!strcmp(value,"KatmedESP"));
    auto prepared=latest_kind(MESH_NAME_SYNC);NamesMessage ack{};assert(names_wire_decode(prepared.payload,prepared.payload_length,&ack)&&ack.opcode==NAMES_PREPARED_ACK);
    assert(names_publish(&remote)==NAMES_OK);m.opcode=NAMES_COMMIT;m.start=0;
    const unsigned setters_before_commit=host_hostname_set_successes;
    deliver(m);
    assert(host_hostname_set_successes==setters_before_commit+1);
    katmed_mesh_device_name(value,sizeof(value),&a,&u);assert(a&&!u&&!strcmp(value,"KatmedESP-002"));assert(!strcmp(host_hostname,value));
    if(!strcmp(argv[1],"names_mixed")){unsigned before=leaf_bytes.commits;legacy();katmed_mesh_device_name(value,sizeof(value),&a,&u);assert(a&&u&&!strcmp(value,"KatmedESP-002")&&!strcmp(host_hostname,value)&&leaf_bytes.commits==before);puts("mixed version: confirmed new-procedure name survives old map, no replacement or false extended replica OK");return 0;}
    if(!strcmp(argv[1],"names_successor")){
        level=ROOT;katmed_mesh_poll(&now);auto hello=latest_kind(MESH_HELLO);memcpy(current_local_nonce,hello.session_nonce,16);
        NamesMemory join_bytes;NamesStore join{};assert(names_open(&join,join_bytes.backend(),88,provision.group)==NAMES_OK);uint64_t count=1;
        auto make=[&](KatmedMeshKind kind){auto f=peer_frame(kind,count++);f.sender=f.id.device=88;for(unsigned i=0;i<16;++i)f.session_nonce[i]=uint8_t(90+i);return f;};
        auto reply=[&](const NamesMessage &msg){auto f=make(MESH_NAME_SYNC);size_t n=0;assert(names_wire_encode(&msg,nullptr,0,f.payload,sizeof(f.payload),&n));f.payload_length=uint16_t(n);inject(f);};
        auto h=make(MESH_HELLO);assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,h.payload,sizeof(h.payload),&h.payload_length));inject(h);katmed_mesh_poll(&now);NamesMessage caps{};caps.opcode=NAMES_CAPS;reply(caps);size_t scan=sent.size();bool done=false;
        for(unsigned step=0;step<100&&!done;++step){advance_now(now.monotonic_ms+10);katmed_mesh_poll(&now);size_t end=sent.size();for(;scan<end;++scan){KatmedMeshFrame f{};if(!katmed_mesh_wire_decode(sent[scan].data(),sent[scan].size(),provision.group,provision.secret,&f)||f.kind!=MESH_NAME_SYNC||f.recipient!=88)continue;NamesMessage msg{};assert(names_wire_decode(f.payload,f.payload_length,&msg));if(msg.opcode==NAMES_CAPS)continue;
            if(msg.opcode==NAMES_MANIFEST){if(join.fresh&&msg.reserved_device!=88){msg.opcode=NAMES_REQUEST;memset(msg.base,0,80);reply(msg);continue;}assert(names_receive_begin(&join,msg.registry,msg.target,false,msg.controller,msg.epoch,msg.barrier,msg.barrier_count,msg.reserved_device,msg.reserved_index)==NAMES_OK);msg.opcode=NAMES_REQUEST;msg.progress=join.ctl[52];memset(msg.base,0,80);reply(msg);}
            else if(msg.opcode==NAMES_PAGE){uint64_t ids[89];size_t n=(f.payload_length-328)/8;for(size_t i=0;i<n;++i)ids[i]=names_wire_slot(f.payload,i);assert(names_receive_page(&join,msg.start,ids,n)==NAMES_OK);msg.opcode=NAMES_PREPARED_ACK;msg.start=0;msg.progress=join.ctl[52];memcpy(msg.progress_hash,join.ctl+56,32);memset(msg.base,0,80);reply(msg);}
            else if(msg.opcode==NAMES_COMMIT){assert(names_publish(&join)==NAMES_OK);assert(names_confirm(&join,names_published(&join))==NAMES_OK);assert(names_finish(&join)==NAMES_OK);msg.opcode=NAMES_COMMIT_ACK;msg.progress=msg.target[64];memcpy(msg.progress_hash,msg.target+32,32);memset(msg.base,0,80);reply(msg);done=true;}
        }}
        assert(done);katmed_mesh_poll(&now);advance_now(now.monotonic_ms+1);katmed_mesh_poll(&now);NamesStore root_after{};assert(names_open(&root_after,leaf_bytes.backend(),11,provision.group)==NAMES_OK);uint16_t index=0,reserved=0;
        assert(names_lookup(&root_after,99,&index,&reserved)==NAMES_OK&&index==1);assert(names_lookup(&root_after,11,&index,&reserved)==NAMES_OK&&index==2);assert(names_lookup(&root_after,88,&index,&reserved)==NAMES_OK&&index==3);assert(names_cursor(&root_after)==4);
        katmed_mesh_device_name(value,sizeof(value),&a,&u);assert(a&&!u&&!strcmp(value,"KatmedESP-002"));puts("ordinary valid production root succession: old root001/new root002 preserved, next peer003 confirmed OK");return 0;
    }
    const unsigned commits=leaf_bytes.commits,applied=host_hostname_set_calls;
    deliver(m);assert(leaf_bytes.commits==commits&&host_hostname_set_calls==applied); // duplicate COMMIT in IDLE
    // Real reopen and fresh authenticated session, then same published P.
    assert(katmed_mesh_names_backend(leaf_bytes.backend(),11,provision.group)==NAMES_OK);
    level=ROOT;katmed_mesh_poll(&now);level=ROOT+1;katmed_mesh_poll(&now);handshake();deliver(m);
    auto committed=latest_kind(MESH_NAME_SYNC);assert(names_wire_decode(committed.payload,committed.payload_length,&ack)&&ack.opcode==NAMES_COMMIT_ACK);
    katmed_mesh_device_name(value,sizeof(value),&a,&u);assert(a&&!u&&!strcmp(value,"KatmedESP-002")&&host_hostname_set_calls==applied);(void)partial;
    puts(prepare_unknown?"actual leaf PREPARED CommitUnknown: no ACK before reopen/rekey, exact RX-full or PREPARED state resumes and commits OK":"actual leaf: durable partial target, incomplete root, prepared/view barrier and same-P COMMIT after IDLE/reopen/fresh session OK");return 0;
}
