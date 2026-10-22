/* Composite recovery drives the real IDF Mesh adapter and authenticated wire.
 * Sparse state is a declared synthetic codec fixture, not a natural run.
 * Its remote endpoint uses the same production ledger, not an allocator model. */
if(argc==2&&(!strcmp(argv[1],"names_merge_recovery")||!strcmp(argv[1],"names_merge_bank_old")||!strcmp(argv[1],"names_merge_bank_new")||!strcmp(argv[1],"names_bounded_open"))){
    const bool bounded_open=!strcmp(argv[1],"names_bounded_open");
    const bool bank_fault=!bounded_open&&strcmp(argv[1],"names_merge_recovery")!=0;
    const bool bank_new=!strcmp(argv[1],"names_merge_bank_new");
    const uint8_t reg[16]={37};
    auto put=[](uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[i]=uint8_t(v>>(8*i));};
    auto le=[](const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[i])<<(8*i);return v;};
    auto descriptor=[&](const std::vector<uint64_t> &ids,uint64_t writer,uint64_t serial){
        std::vector<uint8_t> file(64+8*ids.size()),d(80);memcpy(file.data(),"KMN2",4);put(file.data()+4,2,2);put(file.data()+6,64,2);
        memcpy(file.data()+8,reg,16);memcpy(file.data()+24,provision.group,16);put(file.data()+40,writer,8);put(file.data()+48,serial,8);put(file.data()+56,8,2);put(file.data()+60,crc32(0,file.data(),60),4);
        unsigned assigned=0;for(size_t i=0;i<ids.size();++i){put(file.data()+64+8*i,ids[i],8);if(ids[i])++assigned;}
        put(d.data(),writer,8);put(d.data()+8,serial,8);put(d.data()+16,writer,8);put(d.data()+24,serial,8);SHA256(file.data(),file.size(),d.data()+32);
        put(d.data()+64,ids.size(),4);put(d.data()+68,ids.size()+1,4);put(d.data()+72,assigned,4);put(d.data()+76,1,2);return d;
    };
    auto seed=[&](NamesStore &s,NamesMemory &bytes,uint64_t self,const std::vector<uint64_t> &ids,uint64_t writer){
        const auto d=descriptor(ids,writer,1);assert(names_open(&s,bytes.backend(),self,provision.group)==NAMES_OK);
        assert(names_receive_begin(&s,reg,d.data(),false,writer,1,nullptr,0,0,0)==NAMES_OK);
        for(size_t at=0;at<ids.size();at+=89){size_t n=ids.size()-at;if(n>89)n=89;assert(names_receive_page(&s,uint32_t(at),ids.data()+at,n)==NAMES_OK);}
        assert(names_publish(&s)==NAMES_OK);assert(names_confirm(&s,names_published(&s))==NAMES_OK);assert(names_finish(&s)==NAMES_OK);
    };
    std::vector<uint64_t> source(200);for(size_t i=0;i<source.size();++i)source[i]=1000+i;
    source[0]=11;source[1]=22;source[2]=33;source[3]=44;source.back()=99;
    NamesMemory root_bytes,remote_bytes;NamesStore local_seed{},remote{};
    if(bounded_open){std::vector<uint64_t> large(5000);for(size_t i=0;i<large.size();++i)large[i]=i+11;seed(local_seed,root_bytes,11,large,77);}
    else seed(local_seed,root_bytes,11,{11,22,0,44},77);
    seed(remote,remote_bytes,99,source,99);
    const std::vector<uint8_t> exact_source(names_published(&remote),names_published(&remote)+80);
    const auto before_open=root_bytes.file_reads;
    assert(katmed_mesh_names_backend(root_bytes.backend(),11,provision.group)==(bounded_open?NAMES_BUSY:NAMES_OK));
    assert(root_bytes.file_reads-before_open<=32);
    host_station_present=true;level=ROOT;
    uint64_t max_reads=0,max_writes=0,max_write_bytes=0;
    auto poll=[&](){const auto reads=root_bytes.file_reads,writes=root_bytes.file_writes,bytes=root_bytes.file_write_bytes;
        katmed_mesh_poll(&now);const auto r=root_bytes.file_reads-reads,w=root_bytes.file_writes-writes,b=root_bytes.file_write_bytes-bytes;
        if(r>max_reads)max_reads=r;
        if(w>max_writes)max_writes=w;
        if(b>max_write_bytes)max_write_bytes=b;
        assert(r<=32);advance_now(now.monotonic_ms+101);
    };
    if(bounded_open){
        unsigned calls=0;char value[32];bool assigned=false,uncertain=true;
        do {poll();katmed_mesh_device_name(value,sizeof(value),&assigned,&uncertain);assert(++calls<20);}while(!assigned||uncertain);
        assert(!strcmp(value,"KatmedESP-001"));
        NamesStore durable{};assert(names_open(&durable,root_bytes.backend(),11,provision.group)==NAMES_OK&&names_cursor(&durable)==5001);
        printf("actual Mesh backend bind and cold-open continuation for seeded 5000 slots: %u polls, max/poll reads=%llu; no fresh start\n",calls,(unsigned long long)max_reads);return 0;
    }
    poll();uint64_t counter=1;uint8_t nonce[16];for(unsigned j=0;j<16;++j)nonce[j]=uint8_t(40+j);
    auto frame=[&](KatmedMeshKind kind){auto f=peer_frame(kind,counter++);memcpy(f.session_nonce,nonce,16);return f;};
    auto send=[&](const NamesMessage &m,const uint64_t *ids=nullptr,size_t count=0){auto f=frame(MESH_NAME_SYNC);size_t n=0;assert(names_wire_encode(&m,ids,count,f.payload,sizeof(f.payload),&n));f.payload_length=uint16_t(n);inject(f);return f;};
    auto handshake=[&](){for(unsigned i=0;i<3;++i){const auto h=latest_kind(MESH_HELLO);memcpy(current_local_nonce,h.session_nonce,16);auto f=frame(MESH_HELLO);assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,f.payload,sizeof(f.payload),&f.payload_length));inject(f);poll();}NamesMessage caps{};caps.opcode=NAMES_CAPS;send(caps);poll();};
    handshake();size_t scan=0;bool offered=false,dropped_page=false,restarted=false,dropped_commit_ack=false,done=false,fault_armed=false,fault_seen=false;
    unsigned source_pages=0,prepared_replies=0;KatmedMeshFrame stale_page{},stale_prepared{};NamesMessage pending_prepared{};bool hold_prepared=false,late_ack_checked=false;
    uint8_t receiver_base[80]{};
    for(unsigned step=0;step<600&&!done;++step){
        const size_t end=sent.size();
        for(;scan<end;++scan){
            KatmedMeshFrame f{};NamesMessage m{};
            if(!katmed_mesh_wire_decode(sent[scan].data(),sent[scan].size(),provision.group,provision.secret,&f)||f.kind!=MESH_NAME_SYNC||f.recipient!=99||!names_wire_decode(f.payload,f.payload_length,&m))continue;
            if(m.opcode==NAMES_CAPS){NamesMessage caps{};caps.opcode=NAMES_CAPS;send(caps);continue;}
            if(m.opcode==NAMES_REQUEST&&!memcmp(m.target,exact_source.data(),80)){
                assert(m.controller==11);uint64_t ids[89];size_t n=89;assert(names_read_page(&remote,false,m.progress,ids,&n)==NAMES_OK&&n);
                m.opcode=NAMES_PAGE;m.start=m.progress;++source_pages;
                if(!dropped_page){dropped_page=true;continue;}
                stale_page=send(m,ids,n);if(source_pages==2)send(m,ids,n); // duplicate logical page, fresh wirecounter
            }else if(m.opcode==NAMES_MANIFEST){
                if(!offered||le(m.target+64,4)<source.size()){auto offer=m;offer.flags=6;offer.reserved_device=0;offer.reserved_index=0;memcpy(offer.target,exact_source.data(),80);send(offer);offered=true;continue;}
                NamesStore inspect{};assert(names_open(&inspect,root_bytes.backend(),11,provision.group)==NAMES_OK);
                if(names_phase(&inspect)==NAMES_LOCAL_PREPARE)continue;
                const uint8_t *p=names_published(&remote);memcpy(receiver_base,p,80);
                assert(names_receive_begin(&remote,m.registry,m.target,false,m.controller,m.epoch,m.barrier,m.barrier_count,m.reserved_device,m.reserved_index)==NAMES_OK);
                m.opcode=names_phase(&remote)==NAMES_PREPARED&&!memcmp(m.base,receiver_base,80)?NAMES_PREPARED_ACK:NAMES_REQUEST;
                memcpy(m.base,receiver_base,80);m.progress=uint32_t(le(remote.ctl+52,4));memcpy(m.progress_hash,remote.ctl+56,32);send(m);
            }else if(m.opcode==NAMES_PAGE){
                uint64_t ids[89];size_t n=(f.payload_length-328)/8;for(size_t i=0;i<n;++i)ids[i]=names_wire_slot(f.payload,i);
                const auto received=names_receive_page(&remote,m.start,ids,n);
                if(received!=NAMES_OK)fprintf(stderr,"remote receive %u start %u phase %u progress %llu targetslots %llu\n",unsigned(received),m.start,unsigned(names_phase(&remote)),(unsigned long long)le(remote.ctl+52,4),(unsigned long long)le(m.target+64,4));
                assert(received==NAMES_OK);m.start=0;memcpy(m.base,receiver_base,80);
                m.progress=uint32_t(le(remote.ctl+52,4));memcpy(m.progress_hash,remote.ctl+56,32);
                m.opcode=names_phase(&remote)==NAMES_PREPARED?NAMES_PREPARED_ACK:NAMES_REQUEST;
                if(m.opcode==NAMES_PREPARED_ACK){++prepared_replies;if(bank_fault&&!fault_seen){pending_prepared=m;hold_prepared=true;continue;}}
                send(m);if(m.opcode==NAMES_PREPARED_ACK)send(m); // duplicate ACK has no extra right
            }else if(m.opcode==NAMES_COMMIT){
                if(memcmp(names_published(&remote),m.target,80))assert(names_publish(&remote)==NAMES_OK);
                assert(names_confirm(&remote,names_published(&remote))==NAMES_OK);assert(names_finish(&remote)==NAMES_OK);
                if(!dropped_commit_ack){dropped_commit_ack=true;continue;}
                m.opcode=NAMES_COMMIT_ACK;m.progress=uint32_t(le(m.target+64,4));memcpy(m.progress_hash,m.target+32,32);send(m);send(m);
            }
        }
        poll();
        NamesStore inspect{};assert(names_open(&inspect,root_bytes.backend(),11,provision.group)==NAMES_OK);
        if(!restarted&&names_phase(&inspect)==NAMES_LOCAL_PREPARE&&inspect.ctl[49]==4&&le(inspect.ctl+52,4)>0){
            assert(!memcmp(inspect.ctl+352,exact_source.data(),80)&&le(inspect.ctl+432,8)==99);
            assert(names_reopen(&remote)==NAMES_OK); // exact source S survives its own restart
            assert(katmed_mesh_names_backend(root_bytes.backend(),11,provision.group)==NAMES_OK);
            level=ROOT+1;poll();level=ROOT;poll();
            const auto stable=root_bytes.keys[0];inject(stale_page);poll();assert(root_bytes.keys[0]==stable);
            for(unsigned j=0;j<16;++j)++nonce[j];
            counter=1;scan=sent.size();handshake();restarted=true;
        }
        if(hold_prepared&&!fault_armed){stale_prepared=send(pending_prepared);poll();root_bytes.applies=bank_new;root_bytes.unknown_on_commit=root_bytes.commits+1;hold_prepared=false;fault_armed=true;}
        if(fault_armed&&!fault_seen&&root_bytes.commits>=root_bytes.unknown_on_commit){
            root_bytes.unknown_on_commit=0;fault_seen=true;assert(names_open(&inspect,root_bytes.backend(),11,provision.group)==NAMES_OK);
            assert(names_cursor(&inspect)==(bank_new?201u:5u));poll();
            // Old outer nonce is rejected before the logical ACK handler.
            inject(stale_prepared);poll();
            assert(names_open(&inspect,root_bytes.backend(),11,provision.group)==NAMES_OK&&names_cursor(&inspect)==(bank_new?201u:5u));
            scan=sent.size();handshake();
            // Even re-authenticated bytes cannot restore the old E-round.
            const auto before=root_bytes.keys[0];send(pending_prepared);poll();
            assert(root_bytes.keys[0]==before);late_ack_checked=true;
        }
        if(dropped_commit_ack&&names_phase(&inspect)==NAMES_IDLE&&names_cursor(&inspect)==201)done=true;
    }
    if(!done)fprintf(stderr,"merge trace incomplete: offered=%u dropped=%u restarted=%u lostack=%u sourcepages=%u prepared=%u ctlphase=%u cursor=%llu\n",offered,dropped_page,restarted,dropped_commit_ack,source_pages,prepared_replies,unsigned(root_bytes.keys[0][48]),(unsigned long long)le(root_bytes.keys[0].data()+180,4));
    assert(done&&offered&&dropped_page&&restarted&&dropped_commit_ack&&source_pages>=6&&prepared_replies);
    assert(!bank_fault||(fault_seen&&late_ack_checked));
    NamesStore final{};assert(names_open(&final,root_bytes.backend(),11,provision.group)==NAMES_OK);
    uint16_t i=0,r=0;assert(names_lookup(&final,33,&i,&r)==NAMES_OK&&i==3&&!r);assert(names_lookup(&final,99,&i,&r)==NAMES_OK&&i==200&&!r);
    char name[32];bool assigned=false,uncertain=true;katmed_mesh_device_name(name,sizeof(name),&assigned,&uncertain);assert(assigned&&!uncertain&&!strcmp(name,"KatmedESP-001"));
    printf("composite real Mesh MERGE: dropped/duplicate pages, exact-S restart, root reopen/rekey, delayed old-nonce page, lost/duplicate/late ACKs%s; max/poll reads=%llu writes=%llu writebytes=%llu\n",bank_fault?(bank_new?", bank CommitUnknown NEW":", bank CommitUnknown OLD"):"",(unsigned long long)max_reads,(unsigned long long)max_writes,(unsigned long long)max_write_bytes);
    return 0;
}
if(argc==2&&(!strcmp(argv[1],"names_retarget_recovery")||!strcmp(argv[1],"names_retarget_old")||!strcmp(argv[1],"names_retarget_new"))){
    const bool faults=strcmp(argv[1],"names_retarget_recovery")!=0;
    const bool applies=!strcmp(argv[1],"names_retarget_new");
    const uint8_t reg[16]={37};
    auto put=[](uint8_t *p,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[i]=uint8_t(v>>(8*i));};
    auto le=[](const uint8_t *p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[i])<<(8*i);return v;};
    auto descriptor=[&](const std::vector<uint64_t> &ids,uint64_t writer,uint64_t serial){
        std::vector<uint8_t> file(64+8*ids.size()),d(80);memcpy(file.data(),"KMN2",4);put(file.data()+4,2,2);put(file.data()+6,64,2);
        memcpy(file.data()+8,reg,16);memcpy(file.data()+24,provision.group,16);put(file.data()+40,writer,8);put(file.data()+48,serial,8);put(file.data()+56,8,2);put(file.data()+60,crc32(0,file.data(),60),4);
        unsigned assigned=0;for(size_t i=0;i<ids.size();++i){put(file.data()+64+8*i,ids[i],8);if(ids[i])++assigned;}
        put(d.data(),writer,8);put(d.data()+8,serial,8);put(d.data()+16,writer,8);put(d.data()+24,serial,8);SHA256(file.data(),file.size(),d.data()+32);
        put(d.data()+64,ids.size(),4);put(d.data()+68,ids.size()+1,4);put(d.data()+72,assigned,4);put(d.data()+76,1,2);return d;
    };
    NamesMemory bytes;NamesStore seed{};
    const std::vector<uint64_t> old_ids=faults?std::vector<uint64_t>{99,22,0,44}:std::vector<uint64_t>{99,11,0,44};
    const auto old_p=descriptor(old_ids,99,1);
    assert(names_open(&seed,bytes.backend(),11,provision.group)==NAMES_OK);
    assert(names_receive_begin(&seed,reg,old_p.data(),false,99,1,nullptr,0,0,0)==NAMES_OK);
    assert(names_receive_page(&seed,0,old_ids.data(),old_ids.size())==NAMES_OK);assert(names_publish(&seed)==NAMES_OK);
    auto confirmed=names_confirm(&seed,names_published(&seed));assert(confirmed==(faults?NAMES_NEED_SYNC:NAMES_OK));assert(names_finish(&seed)==NAMES_OK);
    const auto preserved_view=bytes.keys[1];
    assert(katmed_mesh_names_backend(bytes.backend(),11,provision.group)==NAMES_OK);
    host_station_present=true;level=ROOT+1;
    uint64_t max_reads=0,max_writes=0;
    auto poll=[&](){const auto reads=bytes.file_reads,writes=bytes.file_writes;katmed_mesh_poll(&now);
        const auto r=bytes.file_reads-reads,w=bytes.file_writes-writes;
        if(r>max_reads)max_reads=r;
        if(w>max_writes)max_writes=w;
        assert(r<=32);advance_now(now.monotonic_ms+101);
    };
    poll();uint64_t controller=99,counter=1;uint8_t nonce[16];for(unsigned j=0;j<16;++j)nonce[j]=uint8_t(40+j);
    auto frame=[&](KatmedMeshKind kind){auto f=peer_frame(kind,counter++);f.sender=f.id.device=controller;memcpy(f.session_nonce,nonce,16);return f;};
    auto send=[&](const NamesMessage &m,const uint64_t *ids=nullptr,size_t count=0){auto f=frame(MESH_NAME_SYNC);size_t n=0;assert(names_wire_encode(&m,ids,count,f.payload,sizeof(f.payload),&n));f.payload_length=uint16_t(n);inject(f);poll();return f;};
    auto handshake=[&](){for(unsigned i=0;i<3;++i){auto h=latest_kind(MESH_HELLO);memcpy(current_local_nonce,h.session_nonce,16);auto f=frame(MESH_HELLO);assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,f.payload,sizeof(f.payload),&f.payload_length));inject(f);poll();}NamesMessage caps{};caps.opcode=NAMES_CAPS;send(caps);};
    handshake();
    std::vector<uint64_t> old_target=old_ids;old_target.push_back(55);old_target.push_back(66);
    auto old_t=descriptor(old_target,99,1);put(old_t.data()+8,2,8); // same physical append file, new logical G
    NamesMessage manifest{};manifest.opcode=NAMES_MANIFEST;manifest.flags=1|16;manifest.controller=99;manifest.epoch=2;
    memcpy(manifest.registry,reg,16);memcpy(manifest.target,old_t.data(),80);memcpy(manifest.base,old_p.data(),80);
    manifest.barrier_flags=1;manifest.barrier_count=1;manifest.barrier[0]=11;manifest.reserved_device=66;manifest.reserved_index=6;
    send(manifest);auto page=manifest;page.opcode=NAMES_PAGE;page.start=4;uint64_t held=55;
    const auto delayed_old=send(page,&held,1);
    const auto old_bank=bytes.files[0];const auto old_control=bytes.keys[0];
    assert(le(old_control.data()+52,4)==5&&old_control[48]==NAMES_RX_APPEND);
    // An authenticated coordinator change must discard all former rights and
    // rotate the challenge before the forgotten old coordinator can re-echo.
    uint8_t old_challenge[16];memcpy(old_challenge,current_local_nonce,16);
    controller=77;for(unsigned j=0;j<16;++j)nonce[j]=uint8_t(70+j);counter=1;handshake();
    const auto new_challenge=latest_kind(MESH_HELLO);assert(memcmp(new_challenge.session_nonce,old_challenge,16));
    const auto bound=bytes.keys[0];inject(delayed_old);poll();assert(bytes.keys[0]==bound&&bytes.files[0]==old_bank&&bytes.keys[1]==preserved_view);
    std::vector<uint64_t> newer(200);for(size_t i=0;i<newer.size();++i)newer[i]=1000+i;
    for(size_t i=0;i<old_target.size();++i)newer[i]=old_target[i];
    newer[2]=faults?11:33;newer[6]=77;
    const auto target=descriptor(newer,77,5);
    manifest.flags=2|4|16;manifest.controller=77;manifest.epoch=3;memcpy(manifest.target,target.data(),80);
    bool reopened=false,dropped_verify=false,duplicated_verify=false,dropped_write=false,duplicated_write=false,intent_fault_seen=false,view_fault_seen=false,done=false;
    size_t scan=sent.size();unsigned verify_pages=0,write_pages=0;
    for(unsigned step=0;step<800&&!done;++step){
        if(step%10==0)send(manifest);
        const size_t end=sent.size();
        for(;scan<end;++scan){
            KatmedMeshFrame f{};NamesMessage m{};
            if(!katmed_mesh_wire_decode(sent[scan].data(),sent[scan].size(),provision.group,provision.secret,&f)||f.kind!=MESH_NAME_SYNC||f.recipient!=77||!names_wire_decode(f.payload,f.payload_length,&m))continue;
            if(m.opcode==NAMES_CAPS){NamesMessage caps{};caps.opcode=NAMES_CAPS;send(caps);continue;}
            if(m.opcode==NAMES_REQUEST&&!memcmp(m.target,target.data(),80)&&m.progress<newer.size()){
                size_t n=newer.size()-m.progress;if(n>89)n=89;
                const bool verify=(m.flags&8)!=0;m.opcode=NAMES_PAGE;m.start=m.progress;
                if(verify){++verify_pages;if(!dropped_verify){dropped_verify=true;continue;}}
                else {++write_pages;if(!dropped_write){dropped_write=true;continue;}}
                const bool last=verify&&m.start+n==newer.size();
                if(last&&faults&&!intent_fault_seen){bytes.applies=applies;bytes.unknown_on_commit=bytes.commits+1;}
                const auto before_ctl=bytes.keys[0],before_a=bytes.files[0],before_b=bytes.files[1];
                const auto delivered=send(m,newer.data()+m.start,n);
                if(verify&&last&&faults&&!intent_fault_seen){
                    assert(bytes.commits>=bytes.unknown_on_commit);bytes.unknown_on_commit=0;intent_fault_seen=true;
                    assert(bytes.files[0]==old_bank&&bytes.keys[1]==preserved_view);
                    assert(!memcmp(bytes.keys[0].data()+192,applies?target.data():old_t.data(),80));
                    poll();handshake();scan=sent.size();break;
                }
                if(verify&&!last){assert(bytes.keys[0]==before_ctl&&bytes.files[0]==before_a&&bytes.files[1]==before_b&&bytes.keys[1]==preserved_view);}
                if(verify&&!duplicated_verify){
                    duplicated_verify=true;send(m,newer.data()+m.start,n);
                    const auto last=latest_kind(MESH_NAME_SYNC);NamesMessage response{};
                    assert(names_wire_decode(last.payload,last.payload_length,&response));
                    assert(response.opcode==NAMES_REQUEST&&response.progress==m.start+n&&(response.flags&8));
                }
                if(!verify&&!duplicated_write){duplicated_write=true;inject(delivered);poll();send(m,newer.data()+m.start,n);}
                if(verify&&!last&&!reopened){
                    assert(katmed_mesh_names_backend(bytes.backend(),11,provision.group)==NAMES_OK);
                    for(unsigned j=0;j<16;++j)++nonce[j];
                    counter=1;handshake();
                    assert(bytes.keys[0]==before_ctl&&bytes.files[0]==old_bank&&bytes.keys[1]==preserved_view);
                    handshake();scan=sent.size();reopened=true;break;
                }
            }else if((m.opcode==NAMES_PREPARED_ACK||(m.opcode==NAMES_REQUEST&&m.progress==newer.size()&&!(m.flags&8)))&&!memcmp(m.target,target.data(),80)){
                assert(bytes.keys[1]==preserved_view||view_fault_seen);
                auto commit=m;commit.opcode=NAMES_COMMIT;commit.start=0;
                if(faults&&!view_fault_seen){bytes.applies=applies;bytes.unknown_on_commit=bytes.commits+2;}
                send(commit);
                if(faults&&!view_fault_seen){
                    if(bytes.unknown_on_commit&&bytes.commits>=bytes.unknown_on_commit){
                        bytes.unknown_on_commit=0;view_fault_seen=true;
                        assert(bytes.keys[1].empty()==!applies);
                        poll();handshake();scan=sent.size();break;
                    }
                    bytes.unknown_on_commit=0;
                }
                // Lost final ACK and repeated COMMIT after IDLE are harmless.
                send(commit);
                NamesStore inspect{};assert(names_open(&inspect,bytes.backend(),11,provision.group)==NAMES_OK);
                if(names_phase(&inspect)==NAMES_IDLE){assert(names_cursor(&inspect)==201);done=true;break;}
            }
        }
        poll();
        char name[32];bool a=false,u=false;katmed_mesh_device_name(name,sizeof(name),&a,&u);
        if(!done&&!view_fault_seen){assert(!strcmp(name,faults?"KatmedESP":"KatmedESP-002"));assert(a==!faults);}
    }
    if(!done)fprintf(stderr,"retarget trace incomplete: reopen=%u verify=%u write=%u intent=%u view=%u phase=%u progress=%llu\\n",reopened,verify_pages,write_pages,intent_fault_seen,view_fault_seen,unsigned(bytes.keys[0][48]),(unsigned long long)le(bytes.keys[0].data()+52,4));
    assert(done&&reopened&&dropped_verify&&duplicated_verify&&dropped_write&&duplicated_write&&verify_pages>=4&&write_pages>=3);
    assert(!faults||(intent_fault_seen&&view_fault_seen));
    char value[32];bool assigned=false,uncertain=true;katmed_mesh_device_name(value,sizeof(value),&assigned,&uncertain);
    assert(assigned&&!uncertain&&!strcmp(value,faults?"KatmedESP-003":"KatmedESP-002")&&!strcmp(value,host_hostname));
    NamesStore durable{};assert(names_open(&durable,bytes.backend(),11,provision.group)==NAMES_OK);
    uint16_t i=0,r=0;assert(names_lookup(&durable,55,&i,&r)==NAMES_OK&&i==5&&!r);assert(names_lookup(&durable,66,&i,&r)==NAMES_OK&&i==6&&!r);
    printf("composite real Mesh RETARGET: coordinator/rekey, stale old page, write-free pass restart, duplicate/lost pages, shared-P bank and repeated COMMIT%s; max/poll reads=%llu writes=%llu\n",faults?(applies?", Intent/View CommitUnknown NEW":", Intent/View CommitUnknown OLD"):"",(unsigned long long)max_reads,(unsigned long long)max_writes);
    return 0;
}
