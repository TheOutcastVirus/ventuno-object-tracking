# Board setup

## Increase UDP socket buffers (raw image topics)

**Symptom:** `/oak/rgb/image_raw` published well below its configured `rgb_fps`
(e.g. capped around 13-22 Hz, jittery), even after lowering the camera's
capture resolution. Subscribing via a compressed `image_transport` topic
instead got a solid 30 fps from the same captures, which ruled out the
camera/USB link as the bottleneck.

**Cause:** the kernel's default UDP socket buffers
(`net.core.rmem_max`/`wmem_max`) are 208 KB, smaller than a single raw camera
frame (e.g. 640x360 BGR8 = 691 KB). `rmw_fastrtps_cpp` sends over UDP, so
raw image messages were fragmenting and dropping in transit, throttling
effective topic hz far below the actual publish rate.

**Fix:** raise the buffer ceiling so DDS can move full raw frames:

```bash
sudo sysctl -w net.core.rmem_max=16777216 net.core.wmem_max=16777216 \
               net.core.rmem_default=16777216 net.core.wmem_default=16777216
```

Make it permanent (survives reboot):

```bash
sudo tee /etc/sysctl.d/60-ros2-dds.conf <<'EOF'
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.core.rmem_default=16777216
net.core.wmem_default=16777216
EOF
sudo sysctl --system
```

Any ROS 2 process that was already running opened its sockets under the old
limit, so it must be **restarted** (e.g. relaunch `oak_camera`) to pick up
the new ceiling. Verify with:

```bash
ros2 topic hz /oak/rgb/image_raw   # expect a solid ~30 Hz, not climbing/jittery
```
