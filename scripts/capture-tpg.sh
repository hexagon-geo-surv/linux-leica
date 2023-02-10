#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2021-2022, Ideas on Board Oy
#
# Author: Laurent Pinchart <laurent.pinchart@ideasonboard.com>
#
# Modified for testing TPG on Debix by:
# Paul Elder <paul.elder@ideasonboard.com>

set -e

#
# Parse command line arguments
#

usage() {
	echo "Usage: $0 [options] <format>"
	echo ""
	echo "Capture frames from the TPG module on the ISP on the i.MX8MP."
	echo ""
	echo "Supported options:"
	echo "-c, --count value         Number of frames to capture (default: 8)"
	echo "-h, --help                Show this help text"
	echo "-t, --test-pattern value  Set test pattern control [0, 4] (default: 0)"
	echo "-s, --size WxH            Set the frame size (default: 1920x1080)"
	echo "-r, --rate fps            Set the fps (default: 30)"
	echo "    --use-csi             Use the csi2 receiver instead of the tpg"
	echo "    --no-save             Do not save captured frames to disk"
}

count=8
save=1
mbus="YUYV8_1_5X8"
pattern=0
use_csi=
fps="30"

size_capture="1920x1080"
size_output="1920x1080"

while [ $# != 0 ] ; do
	option="$1"
	shift

	case "${option}" in
	-c|--count)
		count="$1"
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	-t|--test-pattern)
		pattern="$1"
		shift
		;;
	-s|--size)
		size_capture="$1"
		size_output="$1"
		shift
		;;
	-r|--rate)
		fps="$1"
		shift
		;;
	--use-csi)
		use_csi=1
		;;
	--no-save)
		save=
		;;
	-*)
		echo "Unknown option ${option}"
		exit 1
		;;
	*)
		format="${option}"
		;;
	esac
done

if [ -z $format ]; then
	echo "Error: format not provided"
	exit 1
fi

isp_bypass=1

case "${format}" in
YUYV | UYVY | 422P | NV16 | NV61 | NV16M | NV61M | YVU422M | Y8)
	mbus="YUYV8_2X8"
	isp_bypass=
	;;
NV21 | NV12 | NV21M | NV12M | YU12 | YV12)
	mbus="YUYV8_1_5X8"
	isp_bypass=
	;;
SRGGB8)
	mbus="SRGGB8_1X8"
	;;
SGRBG8)
	mbus="SGRBG8_1X8"
	;;
SGBRG8)
	mbus="SGBRG8_1X8"
	;;
SBGGR8)
	mbus="SBGGR8_1X8"
	;;
SRGGB10)
	mbus="SRGGB10_1X10"
	;;
SGRBG10)
	mbus="SGRBG10_1X10"
	;;
SGBRG10)
	mbus="SGBRG10_1X10"
	;;
SBGGR10)
	mbus="SBGGR10_1X10"
	;;
SRGGB12)
	mbus="SRGGB12_1X12"
	;;
SGRBG12)
	mbus="SGRBG12_1X12"
	;;
SGBRG12)
	mbus="SGBRG12_1X12"
	;;
SBGGR12)
	mbus="SBGGR12_1X12"
	;;
*)
	echo "Unsupported format ${format}"
	exit 1
	;;
esac

media_dev=/dev/media0
mediactl="media-ctl -d ${media_dev}"

isp="rkisp1_isp"
resizer="rkisp1_resizer_mainpath"
tpg="rkisp1_tpg"
capture="rkisp1_mainpath"

csis="csis-32e40000.csi"
#sensor="ov5640 1-003c"
sensor="imx219 1-0010"

#
# Configure the pipeline
#

#dmesg -c > /dev/null

#code="SRGGB10_1X10"
code="SGRBG8_1X8"

${mediactl} -r

if [ ! -z $use_csi ]; then
	${mediactl} -l "'${sensor}':0 -> '${csis}':0 [1]"
	${mediactl} -l "'${csis}':1 -> '${isp}':0 [1]"
else
	${mediactl} -l "'${tpg}':0 -> '${isp}':0 [1]"
fi

${mediactl} -l "'${isp}':2 -> '${resizer}':0 [1]"

if [ ! -z $use_csi ]; then
	${mediactl} -V "'${sensor}':0 [fmt:${code}/${size_capture}]"
	${mediactl} -V "'${csis}':0 [fmt:${code}/${size_capture}]"
	${mediactl} -V "'${csis}':1 [fmt:${code}/${size_capture}]"
else
	${mediactl} -V "'${tpg}':0 [fmt:${code}/${size_capture}]"
fi

${mediactl} -V "'${isp}':0 [fmt:${code}/${size_capture} crop:(0,0)/${size_capture}]"

if [ ! -z $isp_bypass ]; then
	${mediactl} -V "'${isp}':2 [fmt:${mbus}/${size_output} crop:(0,0)/${size_output}]"
	${mediactl} -V "'${resizer}':0 [fmt:${mbus}/${size_output} crop:(0,0)/${size_output}]"
else
	${mediactl} -V "'${isp}':2 [fmt:${mbus}/${size_capture} crop:(0,0)/${size_capture}]"
	${mediactl} -V "'${resizer}':0 [fmt:${mbus}/${size_capture} crop:(0,0)/${size_output}]"
fi

${mediactl} -V "'${resizer}':1 [fmt:${mbus}/${size_output}]"


#
# Capture frames
#

if [ "$count" = 0 ] ; then
	echo "Pipeline configured, use ${vdev} to capture ${format} ${size_output}"
	exit 0
fi

vdev=$(${mediactl} -e ${capture})
tpg_vdev=$(${mediactl} -e ${tpg})

rm -f /tmp/frame-*.bin
rm -f frame-0000*

if [ ! -z $save ] ; then
	save="-F/tmp/frame-#.bin"
fi

if [ ! -z $use_csi ]; then
	echo "Capturing from ${sensor}"
else
	echo "Setting test pattern to ${pattern}"
	v4l2-ctl --device ${tpg_vdev} --set-ctrl test_pattern=${pattern}
	echo "Setting fps to ${fps}"
	v4l2-ctl --device ${tpg_vdev} --set-subdev-fps pad=0,fps=${fps}
fi

echo "Capturing ${count} frames in ${format} at ${size_output} at ${fps} fps from ${vdev}, saving to ${save}"
yavta -f ${format} -s ${size_output} -c${count} ${save} ${vdev}

if [ ! -z $save ] ; then
	mv /tmp/frame-0000*.bin .
fi
