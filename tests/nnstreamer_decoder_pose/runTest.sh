#!/usr/bin/env bash
##
## SPDX-License-Identifier: LGPL-2.1-only
##
## @file runTest.sh
## @author MyungJoo Ham <myungjoo.ham@gmail.com>
## @date Nov 01 2018
## @brief SSAT Test Cases for NNStreamer
##
if [[ "$SSATAPILOADED" != "1" ]]; then
    SILENT=0
    INDEPENDENT=1
    search="ssat-api.sh"
    source $search
    printf "${Blue}Independent Mode${NC}"
fi

# This is compatible with SSAT (https://github.com/myungjoo/SSAT)
testInit $1

# Test constant passthrough decoder (1, 2)
PATH_TO_PLUGIN="../../build"
CASESTART=0
CASEEND=1

# THIS SHOULD EMIT ERROR
gstTest "--gst-plugin-path=${PATH_TO_PLUGIN} videotestsrc ! videoconvert ! videoscale ! video/x-raw,width=640,height=480,format=RGB ! tensor_converter ! tensor_split name=a tensorseg=1:640:480:1,2:640:480:1 a.src_0 ! tensor_transform mode=transpose option=1:2:0:3 ! tensor_decoder mode=pose_estimation option1=320:240 option2=640:480 ! fakesink" 0_n 0 1 $PERFORMANCE

# THIS WON'T FAIL, BUT NOT MUCH MEANINGFUL.
gstTest "--gst-plugin-path=${PATH_TO_PLUGIN} videotestsrc num_buffers=4 ! videoconvert ! videoscale ! video/x-raw,width=14,height=14,format=RGB ! tensor_converter ! tensor_split name=a tensorseg=1:14:14:1,2:14:14:1 a.src_0 ! tensor_transform mode=transpose option=1:2:0:3 ! tensor_decoder mode=pose_estimation option1=320:240 option2=14:14 ! fakesink" 1 0 0 $PERFORMANCE

# TEST WITH MORE BUFFERS
gstTest "--gst-plugin-path=${PATH_TO_PLUGIN} videotestsrc num_buffers=20 ! videoconvert ! videoscale ! video/x-raw,width=14,height=14,format=RGB ! tensor_converter ! tensor_transform mode=arithmetic option=typecast:float32,add:128,div:255 ! tensor_split name=a tensorseg=1:14:14:1,2:14:14:1 a.src_0 ! tensor_transform mode=transpose option=1:2:0:3 ! tensor_decoder mode=pose_estimation option1=320:240 option2=14:14 ! fakesink" 2 0 0 $PERFORMANCE

echo "nose 1 2 3 4
leftEye 0 2 3
rightEye 0 1 4
leftEar 0 1
rightEar 0 2
leftShoulder 6 7 11
rightShoulder 5 8 12
leftElbow 5 9
rightElbow 6 10
leftWrist 7
rightWrist 8
leftHip 5 12 13
rightHip 6 11 14
leftKnee 11 15
rightKnee 12 16
leftAnkle 13
rightAnkle 14" > pose_label.txt

# TEST OPTION3 and OPTION4
gstTest "--gst-plugin-path=${PATH_TO_PLUGIN} videotestsrc num_buffers=20 ! videoconvert ! videoscale ! video/x-raw,width=17,height=17,format=RGB ! tensor_converter ! tensor_transform mode=arithmetic option=typecast:float32,add:128,div:255 ! tensor_split name=a tensorseg=1:17:17:1,2:17:17:1 a.src_0 ! tensor_transform mode=transpose option=1:2:0:3 ! tensor_decoder mode=pose_estimation option1=320:240 option2=17:17 option3=pose_label.txt option4=heatmap-only option5=ignored ! fakesink" 3 0 0 $PERFORMANCE

rm pose_label.txt

report
