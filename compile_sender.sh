#!/bin/bash

#Define the source file and output executable
SOURCE_FILE="16_sender_test.cpp"
OUTPUT_FILE="16_sender_test"

# Include paths for the libraries
MAVSDK_INCLUDE="/usr/include/mavsdk"
GLOG_INCLUDE="/usr/include/glog"
RF24_INCLUDE="/usr/include/RF24"
EIGEN_INCLUDE="/usr/include/eigen3"
MQTT_INCLUDE="/usr/include/"


# Library files
MAVSDK_LIB="/usr/lib/libmavsdk.so.2.12.3"
GLOG_LIB="/usr/lib/aarch64-linux-gnu/libglog.so"
RF24_LIB="/usr/lib/librf24.so"
MQTT_LIB="/usr/lib/aarch64-linux-gnu/libmosquitto.so"

# Compiler flags
COMPILER_FLAGS="-pthread -std=gnu++17"
DEFINE_FLAGS="-DRF24_NO_INTERRUPT"

# Compile the command
g++ $DEFINE_FLAGS -isystem $MAVSDK_INCLUDE -isystem $GLOG_INCLUDE \
	   $COMPILER_FLAGS -o $OUTPUT_FILE $SOURCE_FILE $MAVSDK_LIB $GLOG_LIB $MQTT_LIB


# Check if the compilation succeeded
if [ $? -eq 0 ]; then
	    echo "Compilation successful! Output: $OUTPUT_FILE"
    else
	        echo "Compilation failed!"
fi

