if(argc==2&&(!std::strcmp(argv[1],"names_barrier")||!std::strcmp(argv[1],"names_lease_old")||!std::strcmp(argv[1],"names_lease_new"))){
    const bool lease_case=strcmp(argv[1],"names_barrier")!=0;
    NamesMemory root_bytes;assert(katmed_mesh_names_backend(root_bytes.backend(),11,provision.group)==NAMES_OK);
    level=ROOT;for(unsigned i=0;i<4;++i){katmed_mesh_poll(&now);advance_now(now.monotonic_ms+1);}
    auto challenge=latest_kind(MESH_HELLO);memcpy(current_local_nonce,challenge.session_nonce,16);
    struct Node{uint64_t id=0,counter=1;uint8_t nonce[16]{};NamesMemory bytes;NamesStore state{};uint8_t base[80]{};};
    Node nodes[8];size_t scan=sent.size();
    auto frame=[&](Node &n,KatmedMeshKind k){auto f=peer_frame(k,n.counter++);f.sender=f.id.device=n.id;memcpy(f.session_nonce,n.nonce,16);return f;};
    auto response=[&](Node &node,const NamesMessage &m){auto f=frame(node,MESH_NAME_SYNC);size_t n=0;assert(names_wire_encode(&m,nullptr,0,f.payload,sizeof(f.payload),&n));f.payload_length=uint16_t(n);inject(f);katmed_mesh_poll(&now);};
    for(unsigned i=0;i<8;++i){auto &n=nodes[i];n.id=300+i;for(unsigned j=0;j<16;++j)n.nonce[j]=uint8_t(i*17+j+2);assert(names_open(&n.state,n.bytes.backend(),n.id,provision.group)==NAMES_OK);auto h=frame(n,MESH_HELLO);assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,h.payload,sizeof(h.payload),&h.payload_length));inject(h);katmed_mesh_poll(&now);NamesMessage caps{};caps.opcode=NAMES_CAPS;response(n,caps);}
    for(unsigned tick=0;tick<800;++tick){
        advance_now(now.monotonic_ms+10);katmed_mesh_poll(&now);
        const size_t end=sent.size();
        for(;scan<end;++scan){KatmedMeshFrame f{};if(!katmed_mesh_wire_decode(sent[scan].data(),sent[scan].size(),provision.group,provision.secret,&f)||f.kind!=MESH_NAME_SYNC)continue;Node *node=nullptr;for(auto &n:nodes)if(n.id==f.recipient)node=&n;if(!node)continue;NamesMessage m{};assert(names_wire_decode(f.payload,f.payload_length,&m));if(m.opcode==NAMES_CAPS||m.opcode==NAMES_HEARTBEAT)continue;
            const uint8_t *p=names_published(&node->state);if(p)memcpy(node->base,p,80);
            if(m.opcode==NAMES_MANIFEST){
                bool admitted=false;for(unsigned i=0;i<m.barrier_count;++i)if(m.barrier[i]==node->id)admitted=true;
                if(node->state.fresh&&!admitted&&m.reserved_device!=node->id){m.opcode=NAMES_REQUEST;memset(m.base,0,80);response(*node,m);continue;}
                bool same=p&&!memcmp(p,m.target,80);bool append=p&&!memcmp(p+16,m.target+16,16);
                assert(names_receive_begin(&node->state,m.registry,m.target,append,m.controller,m.epoch,m.barrier,m.barrier_count,m.reserved_device,m.reserved_index)==NAMES_OK);
                m.opcode=(same||names_phase(&node->state)==NAMES_PREPARED)?NAMES_PREPARED_ACK:NAMES_REQUEST;
                memcpy(m.base,node->base,80);m.progress=(same||m.opcode==NAMES_PREPARED_ACK)?uint32_t(m.target[64]):node->state.ctl[52];
                memcpy(m.progress_hash,(same||m.opcode==NAMES_PREPARED_ACK)?m.target+32:node->state.ctl+56,32);if(!(lease_case&&node->id==307&&m.opcode==NAMES_PREPARED_ACK&&m.target[64]==9))response(*node,m);
            }else if(m.opcode==NAMES_PAGE){uint64_t ids[89];size_t n=(f.payload_length-328)/8;for(size_t i=0;i<n;++i)ids[i]=names_wire_slot(f.payload,i);assert(names_receive_page(&node->state,m.start,ids,n)==NAMES_OK);m.opcode=names_phase(&node->state)==NAMES_PREPARED?NAMES_PREPARED_ACK:NAMES_REQUEST;m.start=0;m.progress=node->state.ctl[52];memcpy(m.progress_hash,node->state.ctl+56,32);memcpy(m.base,node->base,80);if(!(lease_case&&node->id==307&&m.opcode==NAMES_PREPARED_ACK&&m.target[64]==9))response(*node,m);
            }else if(m.opcode==NAMES_COMMIT){bool same=p&&!memcmp(p,m.target,80);if(!same)assert(names_publish(&node->state)==NAMES_OK);auto r=names_confirm(&node->state,names_published(&node->state));assert(r==NAMES_OK||r==NAMES_NEED_SYNC);assert(names_finish(&node->state)==NAMES_OK);m.opcode=NAMES_COMMIT_ACK;m.progress=m.target[64];memcpy(m.progress_hash,m.target+32,32);memcpy(m.base,node->base,80);response(*node,m);}
        }
        NamesStore snapshot{};assert(names_open(&snapshot,root_bytes.backend(),11,provision.group)==NAMES_OK);
        if(lease_case&&names_phase(&snapshot)==NAMES_PREPARED&&snapshot.ctl[50]==8&&snapshot.ctl[104]==9){
            assert(names_cursor(&snapshot)==9);
            root_bytes.unknown=true;root_bytes.applies=!strcmp(argv[1],"names_lease_new");
            advance_now(now.monotonic_ms+30001);
            auto ninth=peer_frame(MESH_HELLO,1);ninth.sender=ninth.id.device=9999;
            assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,ninth.payload,sizeof(ninth.payload),&ninth.payload_length));
            inject(ninth);katmed_mesh_poll(&now);
            NamesStore durable{};assert(names_open(&durable,root_bytes.backend(),11,provision.group)==NAMES_OK);
            assert(names_cursor(&durable)==9&&names_phase(&durable)==NAMES_PREPARED);
            assert(durable.ctl[50]==(root_bytes.applies?0:8));
            root_bytes.unknown=false;katmed_mesh_poll(&now);
            auto fresh=latest_kind(MESH_HELLO);assert(memcmp(fresh.session_nonce,current_local_nonce,16));
            katmed_mesh_poll(&now);
            assert(names_open(&durable,root_bytes.backend(),11,provision.group)==NAMES_OK);
            if(!root_bytes.applies)assert(names_cursor(&durable)==9&&durable.ctl[50]==8);
            else assert(names_cursor(&durable)==10);
            puts(root_bytes.applies?"lease E CommitUnknown NEW: no premature publish; reopen, fresh challenge and actual smaller E permit completion OK":"lease E CommitUnknown OLD: old E remains blocking after reopen/rekey; no false smaller barrier OK");return 0;
        }
        if(names_cursor(&snapshot)==10&&names_phase(&snapshot)==NAMES_IDLE){for(auto &n:nodes){uint16_t index=0,reserved=0;assert(names_lookup(&snapshot,n.id,&index,&reserved)==NAMES_OK&&index>=2&&!reserved);}auto ninth=peer_frame(MESH_HELLO,1);ninth.sender=ninth.id.device=9999;assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,ninth.payload,sizeof(ninth.payload),&ninth.payload_length));inject(ninth);katmed_mesh_poll(&now);assert(names_open(&snapshot,root_bytes.backend(),11,provision.group)==NAMES_OK&&names_cursor(&snapshot)==10);uint16_t absent=0,reserved=0;assert(names_lookup(&snapshot,9999,&absent,&reserved)==NAMES_OK&&!absent&&!reserved);puts("eight live supported sessions: frozen barriers/JOIN_PENDING, unique names; ninth active peer rejected without consuming an index OK");return 0;}
    }
    assert(!"multi-peer name barrier did not finish");
}
