#!/bin/sh
# Build the ME74_2IMUS test dashboard. Run from anywhere:  sh ME74_2IMUS/tools/build.sh
#
# Edit tools/build_dashboard.py (home page, zeroing, test workflow) or the tab
# sources listed below, then run this. Never edit the generated HTML by hand:
# every build overwrites it.
#
# Output: ME74_2IMUS/ME74_test_dashboard.html
# ME74_2IMUS/ME74_dashboard.html is the frozen fallback; this script never touches it.
set -e
cd "$(dirname "$0")/../.."
python3 ME74_2IMUS/tools/build_dashboard.py \
  ME74_2IMUS/ME74_test_dashboard.html "ME74 Dashboard" ME74_2IMUS \
  gps=gps-baseline/dashboard.html \
  imu=gui/imu_dashboard.html \
  imu2=gui/imu_dashboard.html \
  lora=gui/radio_dashboard.html \
  espnow=gui/espnow_dashboard.html
