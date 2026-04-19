PROJECT_NAME := retro-fit-device

# Point IDF_PATH at the submodule so the SDK is self-contained
export IDF_PATH := $(shell pwd)/esp8266-rtos-sdk

include $(IDF_PATH)/make/project.mk
