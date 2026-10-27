# ESP-IDF's wrapper uses include_next, so its public port must precede TF-PSA.
include_directories(BEFORE "$ENV{IDF_PATH}/components/mbedtls/port/include")
