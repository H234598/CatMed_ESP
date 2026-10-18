    if(argc==2&&std::strcmp(argv[1],"peer_rekey_context")==0){
        level=ROOT;katmed_mesh_poll(&now);
        KatmedInteractionInput enter{};enter.kind=KATMED_INTERACTION_ENTER;enter.menu_entry=17;
        assert(katmed_mesh_interaction_input(&enter,now.monotonic_ms));
        KatmedInteractionInput alarm{};alarm.kind=KATMED_INTERACTION_ALARM;alarm.active=true;
        assert(katmed_mesh_interaction_input(&alarm,now.monotonic_ms));
        KatmedInteractionStatus before{},after{};katmed_mesh_interaction_status(&before,now.monotonic_ms);
        level=2;advance_now(1100);katmed_mesh_poll(&now);katmed_mesh_interaction_status(&after,now.monotonic_ms);
        std::printf("rekey before_menu=%d before_alarm=%d after_menu=%d after_alarm=%d\n",before.menu_open,before.alarm_active,after.menu_open,after.alarm_active);
        assert(before.menu_open&&before.alarm_active&&after.menu_open&&after.alarm_active&&!after.owns_token&&!after.receive_white);
        katmed_mesh_stop();katmed_mesh_interaction_status(&after,now.monotonic_ms);
        assert(after.menu_open&&after.alarm_active&&!after.owns_token&&!after.receive_white);
        return 0;
    }
    if(argc==2&&(std::strcmp(argv[1],"interaction_root")==0||std::strcmp(argv[1],"interaction_leaf")==0||std::strcmp(argv[1],"interaction_previous")==0)){
        bool leaf=std::strcmp(argv[1],"interaction_leaf")==0;
        level=leaf?2:ROOT;katmed_mesh_poll(&now);
        auto challenge=latest_kind(MESH_HELLO);std::memcpy(current_local_nonce,challenge.session_nonce,16);
        KatmedInteractionStatus status{};katmed_mesh_interaction_status(&status,now.monotonic_ms);
        assert(!status.greeting_visible); // own initialization never greets
        if(std::strcmp(argv[1],"interaction_previous")==0){
            auto known=peer_frame(MESH_HELLO,1);known.sender=88;known.id.device=88;
            assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,known.payload,sizeof(known.payload),&known.payload_length));
            inject(known);katmed_mesh_poll(&now);
        }
        auto hello=peer_frame(MESH_HELLO,1);
        assert(katmed_mesh_encode_hello(current_local_nonce,catalog,"d941fae",0,hello.payload,sizeof(hello.payload),&hello.payload_length));
        inject(hello);katmed_mesh_poll(&now);
        if(leaf){
            Memory remote_memory;MeshHistory remote{};remote.backend={&remote_memory,read_blob,write_blob};
            uint8_t scratch[4096];mesh_history_open(&remote,provision.group,scratch);
            mesh_history_add(&remote,99);mesh_history_add(&remote,11);assert(mesh_history_admit(&remote,11));mesh_history_commit(&remote,scratch);
            auto f=peer_frame(MESH_STATUS,2);uint64_t members[]{11,99};
            f.payload_length=mesh_status_encode(&remote,MS_PREHISTORY|MS_CURRENT,0,99,members,2,f.payload);
            inject(f);katmed_mesh_poll(&now);katmed_mesh_interaction_status(&status,now.monotonic_ms);
            assert(status.greeting_visible&&status.greeting_id==11); // root-confirmed SELF join, not self init
            advance_now(1800);katmed_mesh_poll(&now);
            auto control_frame=latest_kind(MESH_INTERACTION);katmed::InteractionMessage after_admission;
            assert(katmed::interaction_decode(control_frame.payload,control_frame.payload_length,after_admission));
            assert(!(after_admission.flags&8));
            auto commits=history_memory.commits;
            f.session_counter=3;inject(f);katmed_mesh_poll(&now);assert(history_memory.commits==commits);
        }else{
            katmed::InteractionMessage m;m.op=1;m.query=1;m.flags=8;
            auto f=peer_frame(MESH_INTERACTION,2);f.payload_length=katmed::interaction_encode(m,f.payload);
            inject(f);katmed_mesh_poll(&now);
            auto reply=latest_kind(MESH_INTERACTION);katmed::InteractionMessage answer;
            assert(katmed::interaction_decode(reply.payload,reply.payload_length,answer)&&answer.op==2);
            advance_now(16000);hello.session_counter=3;inject(hello);katmed_mesh_poll(&now);
            // Admission waits for a complete, non-warning historical baseline.
            katmed_mesh_interaction_status(&status,now.monotonic_ms);assert(!status.greeting_visible);
            MeshHistory peer_history{};peer_history.generation=1;mesh_history_add(&peer_history,99);
            bool previous=std::strcmp(argv[1],"interaction_previous")==0;
            if(previous){assert(mesh_history_admit(&peer_history,99));peer_history.dirty=false;}
            auto history_frame=peer_frame(MESH_STATUS,4);
            history_frame.payload_length=mesh_status_encode(&peer_history,MS_PREHISTORY,0,11,nullptr,0,history_frame.payload);
            inject(history_frame);katmed_mesh_poll(&now);
            katmed_mesh_interaction_status(&status,now.monotonic_ms);if(previous){assert(!status.greeting_visible);std::puts("prior confirmed peer fact suppresses stale new-join claim");return 0;}
            assert(status.greeting_visible&&status.greeting_id==99);
            KatmedInteractionInput enter{};enter.kind=KATMED_INTERACTION_ENTER;enter.menu_entry=17;
            assert(katmed_mesh_interaction_input(&enter,now.monotonic_ms));enter.kind=KATMED_INTERACTION_DOWN;enter.contact_id=1;
            assert(katmed_mesh_interaction_input(&enter,now.monotonic_ms));katmed_mesh_poll(&now);
            katmed_mesh_interaction_status(&status,now.monotonic_ms);assert(status.owns_token);
            advance_now(16180);enter.kind=KATMED_INTERACTION_UP;assert(katmed_mesh_interaction_input(&enter,now.monotonic_ms));katmed_mesh_poll(&now);
            m.query=2;m.challenge=answer.challenge;f.session_counter=5;f.payload_length=katmed::interaction_encode(m,f.payload);
            // old challenge expired: receive may refresh query, but cannot mutate token or renew it
            inject(f);katmed_mesh_poll(&now);katmed_mesh_interaction_status(&status,now.monotonic_ms);assert(status.owns_token);
        }
        MeshHistory reboot{};uint8_t scratch[4096];reboot.backend={&history_memory,read_blob,write_blob};mesh_history_open(&reboot,provision.group,scratch);
        uint64_t fact=0;assert(!mesh_history_unprocessed(&reboot,&fact));
        KatmedInteractionInput dismiss{};dismiss.kind=KATMED_INTERACTION_DISMISS_GREETING;dismiss.target_id=status.greeting_id;
        assert(katmed_mesh_interaction_input(&dismiss,now.monotonic_ms));katmed_mesh_interaction_status(&status,now.monotonic_ms);assert(!status.greeting_visible);
        std::puts(leaf?"real leaf adapter: authenticated root SELF admission, duplicate and reboot dedupe OK":"real root adapter: confirmed admission, popup persistence, transient wire dispatch, local token and stale challenge OK");return 0;
    }
