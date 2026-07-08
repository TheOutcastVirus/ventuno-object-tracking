# Connecting the Ventuno board to the Create 3 base

This documents how the compute board (a Qualcomm / `qcom` board — **not** a Raspberry
Pi) talks to the iRobot Create 3 base, why the stock TurtleBot 4 setup does not apply
as-is, and every fix that was made permanent.

## TL;DR

- The board reaches the Create 3 over a **USB-ethernet gadget** link:
  board `usb0 = 192.168.186.3/24`  ↔  Create 3 `192.168.186.2`.
- Our Create 3 runs **Iron** firmware at the **root namespace**, so it subscribes to
  **`/cmd_vel` directly**. There is **no `create3_republisher`** and no `/_do_not_use`
  bridge — publish `geometry_msgs/Twist` to `/cmd_vel` and it drives.
- Everything is made permanent: gadget via a **systemd service**, DDS env in
  **`~/.bashrc`**, and the launch files no longer start the republisher.

## Topology (this is the part that bit us)

On a TurtleBot 4 the **Create 3 is the USB host** and the **compute board is the USB
device (gadget)** running a USB-ethernet function. The stock Raspberry Pi does this
with `dtoverlay=dwc2,dr_mode=peripheral` + `g_ether` in its boot firmware. That boot
firmware is Pi-specific and does nothing on this Qualcomm board, so we build the
equivalent by hand.

Consequences:

- Plug the cable into the board's **USB-C** port (it is the dual-role/`dwc3` port and
  is in **device** mode), going to the **Create 3's USB-C**.
- The board's **USB-A ports are host-only** and will **not** work for this.
- A **charge-only cable** enumerates *nothing* (total silence in `dmesg`). Use a data
  cable.

## The USB gadget

`scripts/create3_usb_gadget.sh` builds a CDC-ECM gadget with `configfs`/`libcomposite`,
bound to the board's UDC (`a600000.usb`), and assigns the IP:

- interface: `usb0`, MAC `12:34:56:78:9a:bc` (stable name)
- board IP: `192.168.186.3/24`; Create 3: `192.168.186.2`
- ECM host-side MAC (the Create 3's usb iface): `12:34:56:78:9a:bd`

Made permanent by **`scripts/create3-usb-gadget.service`** (installed to
`/etc/systemd/system/`, `systemctl enable`d). It runs the script at boot.

> **Why systemd and not a NetworkManager profile?** NetworkManager marks the USB
> gadget interface *unmanaged*, so a NM/netplan profile is unreliable for it. The
> oneshot service owns both the gadget creation and the `usb0` address.

Manual control:

```bash
sudo scripts/create3_usb_gadget.sh        # bring up (idempotent)
sudo scripts/create3_usb_gadget.sh down   # tear down
sudo systemctl start create3-usb-gadget   # same as the boot path
```

## DDS discovery

The host has several NICs (Wi-Fi `wlp3s0` is the default route). By default Fast DDS
joins discovery multicast on the default NIC, **not** `usb0`, so it never hears the
base — even though `tcpdump` shows the base's SPDP arriving on `usb0`. Fix: whitelist
the link's interface with **`scripts/fastdds_usb0.xml`** (loopback + `192.168.186.3`).

The DDS environment lives in **`~/.bashrc`** (must match the Create 3's Application
Config — simple/multicast discovery, so `ROS_DISCOVERY_SERVER` stays unset):

```bash
export ROS_DOMAIN_ID=0
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export FASTRTPS_DEFAULT_PROFILES_FILE=/home/ubuntu/Documents/ventuno-object-tracking/scripts/fastdds_usb0.xml
```

## Namespace: no republisher needed

A stock Jazzy TB4 hides the Create 3 under `/_do_not_use` and needs
`create3_republisher` to expose clean topics. **This base does not.** It runs Iron at
the root namespace; its `motion_control` node subscribes to `/cmd_vel` directly, and
`geometry_msgs/Twist`'s type hash is identical between Iron and Jazzy
(`RIHS01_9c45bf16…`), so a Jazzy publisher matches it.

`movement_test.launch.py` and `tracker.launch.py` were updated to **drop the
republisher** and publish straight to `/cmd_vel`.

## Bring-up & verification

```bash
# 1. Gadget link (automatic at boot; manual:)
sudo systemctl start create3-usb-gadget
ping -c3 192.168.186.2                     # expect 0% loss

# 2. ROS graph (new shell picks up ~/.bashrc env)
ros2 node list                             # expect /_internal/mobility, motion_control, ...
ros2 topic info -v /cmd_vel                # Subscription count >= 1 (node motion_control)

# 3. Drive (clear floor / on blocks, OFF the dock)
ros2 launch object_tracker movement_test.launch.py
```

## Troubleshooting — the reboot / one-way-link trap

The gadget link is fragile across USB bounces. Two failure modes and how to break the
loop:

1. **After the *Create 3* reboots**, the gadget's TX (IN) endpoint wedges: the link
   goes **one-way** — the base's multicast still arrives, but *our* unicast never
   leaves `usb0` (`ping` = 100% loss, `neigh FAILED`, and `tcpdump` shows no outbound
   frames). **Fix: re-bind the UDC** (unbind then rebind), e.g. rerun
   `sudo scripts/create3_usb_gadget.sh` or:
   ```bash
   G=/sys/kernel/config/usb_gadget/create3
   echo "" | sudo tee $G/UDC ; echo a600000.usb | sudo tee $G/UDC
   ```

2. **Re-binding the gadget bounces the base's USB link**, which makes the **base's ROS
   app go silent** (it's still pingable — its web UI at `http://192.168.186.2` answers,
   but no SPDP). **Fix: restart only the base's application — never the Reboot
   button:**
   ```bash
   curl -X POST http://192.168.186.2/api/restart-app
   ```
   (or the Create 3 web UI → Application → Restart Application). A full **Reboot**
   re-wedges the gadget TX and restarts the whole loop.

Quick checks:

```bash
cat /sys/class/udc/*/state                 # want: configured
cat /sys/class/net/usb0/carrier            # want: 1
sudo tcpdump -ni usb0 'src 192.168.186.2 and udp'   # base announcing DDS?
```
