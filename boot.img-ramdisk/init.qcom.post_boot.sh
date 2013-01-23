#!/system/bin/sh
# Copyright (c) 2009-2011, Code Aurora Forum. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of Code Aurora nor
#       the names of its contributors may be used to endorse or promote
#       products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
# OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

target=`getprop ro.product.device`
case "$target" in
    "GT-S6202"| "GT-S7500" | "GT-S6500" | "GT-S6500D" | "GT-S6500T" | "GT-S7509" | "SGH-I827" | "msm7201a_ffa" | "msm7201a_surf" | "msm7627_ffa" | "msm7627_surf" | "msm7627a" | \
    "qsd8250_surf" | "qsd8250_ffa" | "msm7630_surf" | "msm7630_1x" | "msm7630_fusion" | "qsd8650a_st1x")
        echo "smartassV2" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
#        echo 90 > /sys/devices/system/cpu/cpu0/cpufreq/ondemand/up_threshold
#        chown system /sys/devices/system/cpu/cpu0/cpufreq/ondemand/sampling_rate
        ;;
esac

case "$target" in
    "msm7201a_ffa" | "msm7201a_surf")
        echo 500000 > /sys/devices/system/cpu/cpu0/cpufreq/ondemand/sampling_rate
        ;;
esac

case "$target" in
    "msm7630_surf" | "msm7630_1x" | "msm7630_fusion")
        echo 75000 > /sys/devices/system/cpu/cpu0/cpufreq/ondemand/sampling_rate
        echo 1 > /sys/module/pm2/parameters/idle_sleep_mode
        ;;
esac

case "$target" in
     "GT-S6202" | "GT-S7500" | "GT-S6500" | "GT-S6500D" | "GT-S6500T" | "GT-S7509" | "SGH-I827" | "msm7201a_ffa" | "msm7201a_surf" | "msm7627_ffa" | "msm7627_surf" | "msm7630_surf" | "msm7630_1x" | "msm7630_fusion" | "msm7627a" )
        echo 245760 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
	echo 600000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
        chown system /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
        chown system /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
        ;;
esac

case "$target" in
    "GT-S6202" | "msm7627_ffa" | "msm7627_surf" | "GT-S7500" | "GT-S6500" | "GT-S6500D" | "GT-S6500T" | "GT-S7509" | "SGH-I827" | "msm7627a")
#        echo 25000 > /sys/devices/system/cpu/cpu0/cpufreq/ondemand/sampling_rate
#	echo 75 > /sys/devices/system/cpu/cpufreq/smartass/max_cpu_load
#	echo 40 > /sys/devices/system/cpu/cpufreq/smartass/min_cpu_load
#	echo 480000 > /sys/devices/system/cpu/cpufreq/smartass/sleep_wakeup_freq
#	echo 100000 > /sys/devices/system/cpu/cpufreq/smartass/ramp_up_step
#	echo 200000 > /sys/devices/system/cpu/cpufreq/smartass/ramp_down_step
#	chown system /sys/devices/system/cpu/cpufreq/smartass/max_cpu_load
#	chown system /sys/devices/system/cpu/cpufreq/smartass/min_cpu_load
#	chown system /sys/devices/system/cpu/cpufreq/smartass/sleep_wakeup_freq
#	chown system /sys/devices/system/cpu/cpufreq/smartass/ramp_up_step
#	chown system /sys/devices/system/cpu/cpufreq/smartass/ramp_down_step
        ;;
esac

case "$target" in
    "qsd8250_surf" | "qsd8250_ffa" | "qsd8650a_st1x")
        echo 50000 > /sys/devices/system/cpu/cpu0/cpufreq/ondemand/sampling_rate
        ;;
esac

case "$target" in
    "qsd8650a_st1x")
        mount -t debugfs none /sys/kernel/debug
    ;;
esac

emmc_boot=`getprop ro.emmc`
case "$emmc_boot"
    in "1")
        chown system /sys/devices/platform/rs300000a7.65536/force_sync
        chown system /sys/devices/platform/rs300000a7.65536/sync_sts
        chown system /sys/devices/platform/rs300100a7.65536/force_sync
        chown system /sys/devices/platform/rs300100a7.65536/sync_sts
    ;;
esac
