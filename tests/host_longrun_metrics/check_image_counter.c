#define main established_image_metrics_main
#include "../host_image_metrics/check_image_metrics.c"
#undef main

int main(int argc,char **argv)
{
    assert(katmed_images_failed_presentations()==0);
    const int result=established_image_metrics_main(argc,argv);
    assert(result==0);
    /* One invalid logical scene and one real decoder/read failure. The
     * internal file/decoder callbacks must not each count a new operation. */
    assert(katmed_images_failed_presentations()==2);
    puts("image completed-presentation counter passed");
    return 0;
}
