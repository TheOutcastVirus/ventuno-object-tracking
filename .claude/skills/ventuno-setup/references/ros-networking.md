# ROS 2 / DDS networking tuning

## Raw image topic publishes far below its configured rate

**Symptom:** a raw image topic (e.g. `/oak/rgb/image_raw`) publishes well below its
configured rate (e.g. capped around 13-22 Hz, jittery) even after lowering capture
resolution. Subscribing via a compressed `image_transport` topic instead gets a solid
full rate from the same captures — which rules out the camera/USB link as the bottleneck.

**Cause:** the kernel's default UDP socket buffers (`net.core.rmem_max`/`wmem_max`) are
208 KB, smaller than a single raw camera frame (e.g. 640x360 BGR8 = 691 KB).
`rmw_fastrtps_cpp` sends over UDP, so raw image messages fragment and drop in transit,
throttling effective topic hz far below the actual publish rate.

**Fix:** raise the buffer ceiling so DDS can move full raw frames. `install_ventuno_deps.sh`
installs this permanently unless run with `--skip-dds-tuning`
(`/etc/sysctl.d/60-ros2-dds.conf`). To do it by hand or apply immediately without a reboot:

```bash
sudo sysctl -w net.core.rmem_max=16777216 net.core.wmem_max=16777216 \
               net.core.rmem_default=16777216 net.core.wmem_default=16777216
```

Permanent:
```bash
sudo tee /etc/sysctl.d/60-ros2-dds.conf <<'EOF'
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.core.rmem_default=16777216
net.core.wmem_default=16777216
EOF
sudo sysctl --system
```

Any ROS 2 process already running opened its sockets under the old limit, so it must be
**restarted** (e.g. relaunch `oak_camera`) to pick up the new ceiling. Verify:

```bash
ros2 topic hz /oak/rgb/image_raw   # expect a solid rate, not climbing/jittery
```

## DDS discovery misses a peer reachable on a specific NIC

See [references/create3-connection.md](create3-connection.md) for the concrete case (Create 3
over `usb0`). General shape of the problem: a board with multiple NICs has Fast DDS join
discovery multicast only on the default-route interface. If a peer is reachable and even
visible in `tcpdump` on a non-default interface but never shows up in `ros2 node list` /
`ros2 topic info`, whitelist that interface explicitly via a `FASTRTPS_DEFAULT_PROFILES_FILE`
XML profile (see `scripts/fastdds_usb0.xml` for the working pattern) rather than relying on
Fast DDS's default interface selection.
