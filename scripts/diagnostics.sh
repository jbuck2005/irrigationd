#!/bin/sh
# Opens each of the 14 zones for 2 seconds in turn, to check the wiring.
# Needs IRRIGATION_TOKEN, and IRRIGATION_HOST if irrigationd is on another machine.
for z in $(seq 1 14); do irrigationctl "ZONE=$z TIME=2"; sleep 3; done
