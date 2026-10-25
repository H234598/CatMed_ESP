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
int main(void)
{
    BoardOrientationPolicy p={0};
    stable(&p,0,8192,0,0,ORIENTATION_0);
    stable(&p,8192,0,0,1000,ORIENTATION_90);
    stable(&p,0,-8192,0,2000,ORIENTATION_180);
    stable(&p,-8192,0,0,3000,ORIENTATION_270);
    assert(!board_orientation_update(&p,true,6000,5900,0,4000)); // diagonal
    assert(!board_orientation_update(&p,true,0,0,8192,4100)); // flat
    assert(!board_orientation_update(&p,true,INT16_MIN,0,0,4200)); // out of range
    assert(!board_orientation_update(&p,true,0,8192,0,4300));
    assert(!board_orientation_update(&p,false,0,8192,0,4600)); // error breaks dwell
    assert(!board_orientation_update(&p,true,0,8192,0,4800));
    assert(!board_orientation_update(&p,true,0,8192,0,5000));
    assert(p.accepted==ORIENTATION_270);
    assert(board_orientation_update(&p,true,0,8192,0,5200));
    assert(!board_orientation_update(&p,true,8192,0,0,5300));
    assert(!board_orientation_update(&p,true,0,8192,0,5400)); // back to accepted
    assert(p.accepted==ORIENTATION_0);
    assert(!board_orientation_update(&p,true,8192,0,0,5500));
    assert(!board_orientation_update(&p,true,8192,0,0,20)); // clock restart
    assert(!board_orientation_update(&p,true,8192,0,0,419));
    assert(board_orientation_update(&p,true,8192,0,0,420));
    const BoardGeometry c6={172,320,true};
    /* Four asymmetric physical corners, not just the central alarm target.
     * These expected points also match the pinned LVGL clockwise point map
     * applied once after the board's zero-angle mirror. They establish the
     * software frame contract, not the unknown physical IMU axis signs. */
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
