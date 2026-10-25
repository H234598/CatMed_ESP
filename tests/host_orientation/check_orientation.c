#include "board_orientation.h"
#include "board_geometry.h"
#include "board_indev_gate.h"
#include <assert.h>
#include <stdio.h>
static void stable(BoardOrientationPolicy *p,int x,int y,int z,uint64_t t,Orientation want)
{
    assert(!board_orientation_update(p,true,x,y,z,t));
    assert(!board_orientation_update(p,true,x,y,z,t+399));
    assert(board_orientation_update(p,true,x,y,z,t+400));
    assert(p->valid && p->accepted==want);
}
static void mounted_frame_stability(void)
{
    const struct { int16_t x,y; Orientation angle; } poses[]={
        {0,8192,ORIENTATION_180},{8192,0,ORIENTATION_270},
        {0,-8192,ORIENTATION_0},{-8192,0,ORIENTATION_90}
    };
    for(unsigned i=0;i<4;++i){
        BoardOrientationPolicy p={0};
        stable(&p,poses[i].x,poses[i].y,0,0,poses[i].angle);
        // A held corrected pose is already accepted, not a fresh candidate.
        for(uint64_t t=500;t<=1500;t+=100){
            assert(!board_orientation_update(&p,true,poses[i].x,poses[i].y,0,t));
            assert(!p.pending && p.accepted==poses[i].angle);
        }
        const unsigned next=(i+1)%4;
        assert(!board_orientation_update(&p,true,poses[next].x,poses[next].y,0,1600));
        assert(p.pending && p.candidate==poses[next].angle);
        // Returning to the accepted frame cancels the interrupted dwell.
        assert(!board_orientation_update(&p,true,poses[i].x,poses[i].y,0,1800));
        assert(!p.pending && p.accepted==poses[i].angle);
        stable(&p,poses[next].x,poses[next].y,0,2000,poses[next].angle);
    }
    BoardOrientationPolicy edge={0};
    assert(!board_orientation_update(&edge,true,4914,0,0,0));
    stable(&edge,4915,0,0,100,ORIENTATION_270);
    assert(!board_orientation_update(&edge,true,12289,0,0,600));
    assert(!board_orientation_update(&edge,true,8192,6145,0,700));
    assert(!board_orientation_update(&edge,true,0,8192,5735,800));
    assert(!board_orientation_update(&edge,true,8192,4097,5734,900));
    assert(edge.valid && edge.accepted==ORIENTATION_270 && !edge.pending);
    stable(&edge,0,8192,5734,1000,ORIENTATION_180);
    stable(&edge,8192,6144,0,2000,ORIENTATION_270);
}
int main(void)
{
    mounted_frame_stability();
    BoardOrientationPolicy p={0};
    /* Four-pose C6 mounting calibration: every former automatic pose was
     * opposite the known manual display frame. Raw values here are synthetic,
     * not claimed sensor readings from the physical acceptance sequence. */
    stable(&p,0,8192,0,0,ORIENTATION_180);
    stable(&p,8192,0,0,1000,ORIENTATION_270);
    stable(&p,0,-8192,0,2000,ORIENTATION_0);
    stable(&p,-8192,0,0,3000,ORIENTATION_90);
    assert(!board_orientation_update(&p,true,6000,5900,0,4000)); // diagonal
    assert(!board_orientation_update(&p,true,0,0,8192,4100)); // flat
    assert(!board_orientation_update(&p,true,INT16_MIN,0,0,4200)); // out of range
    assert(!board_orientation_update(&p,true,0,8192,0,4300));
    assert(!board_orientation_update(&p,false,0,8192,0,4600)); // error breaks dwell
    assert(!board_orientation_update(&p,true,0,8192,0,4800));
    assert(!board_orientation_update(&p,true,0,8192,0,5000));
    assert(p.accepted==ORIENTATION_90);
    assert(board_orientation_update(&p,true,0,8192,0,5200));
    assert(!board_orientation_update(&p,true,8192,0,0,5300));
    assert(!board_orientation_update(&p,true,0,8192,0,5400)); // back to accepted
    assert(p.accepted==ORIENTATION_180);
    assert(!board_orientation_update(&p,true,8192,0,0,5500));
    assert(!board_orientation_update(&p,true,8192,0,0,20)); // clock restart
    assert(!board_orientation_update(&p,true,8192,0,0,419));
    assert(board_orientation_update(&p,true,8192,0,0,420));
    const BoardGeometry c6={172,320,true};
    /* Four asymmetric physical corners, not just the central alarm target.
     * These expected points also match the pinned LVGL clockwise point map
     * applied once after the board's zero-angle mirror. They establish the
     * software touch frame contract, not raw physical IMU measurements. */
    const uint16_t raw[4][2]={{0,0},{171,0},{0,319},{171,319}};
    const int16_t corners[4][4][2]={
        {{171,0},{0,0},{171,319},{0,319}},
        {{319,171},{319,0},{0,171},{0,0}},
        {{0,319},{171,319},{0,0},{171,0}},
        {{0,0},{0,171},{319,0},{319,171}}
    };
    for(unsigned angle=0;angle<4;++angle)for(unsigned corner=0;corner<4;++corner){
        int16_t x,y;bool down;
        assert(board_geometry_sample(c6,(Orientation)angle,true,true,
            raw[corner][0],raw[corner][1],&down,&x,&y) && down);
        assert(x==corners[angle][corner][0] && y==corners[angle][corner][1]);
        assert(!board_geometry_sample(c6,(Orientation)angle,true,true,172,0,&down,&x,&y));
        assert(!down);
    }
    const int16_t expected_x[]={171,319,0,0},expected_y[]={0,171,319,0};
    for(unsigned angle=0;angle<4;++angle){
        int16_t x,y;uint16_t w,h;
        assert(board_geometry_touch(c6,(Orientation)angle,0,0,&x,&y));
        assert(x==expected_x[angle] && y==expected_y[angle]);
        assert(board_geometry_size(c6,(Orientation)angle,&w,&h));
        assert(w==(angle%2 ? 320 : 172) && h==(angle%2 ? 172 : 320));
        BoardIndevGate gate={0};bool reset;
        assert(board_indev_gate_poll(&gate,true,true,false,&reset));
        board_indev_gate_invalidate(&gate); // same rotation invalidation contract
        assert(!board_indev_gate_poll(&gate,true,true,false,&reset) && reset);
        assert(!board_indev_gate_poll(&gate,true,false,false,&reset));
        assert(board_indev_gate_poll(&gate,true,true,false,&reset) && !reset);
    }
    puts("orientation: four poses, dwell, hysteresis, flat, invalid, failure and restart PASS");
}
