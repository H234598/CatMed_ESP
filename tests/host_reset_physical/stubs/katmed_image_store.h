#ifndef TEST_RESET_IMAGE_STORE_H
#define TEST_RESET_IMAGE_STORE_H
typedef enum { KATMED_IMAGE_OK, KATMED_IMAGE_IO_ERROR } KatmedImageStatus;
KatmedImageStatus katmed_image_store_clear_user_data(void);
#endif
