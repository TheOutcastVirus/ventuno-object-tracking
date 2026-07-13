# Create 3 base connection (USB gadget + DDS)

The compute board (Ventuno Q, a Qualcomm/`qcom` board — **not** a Raspberry Pi) talks to
the iRobot Create 3 base over USB. The stock TurtleBot 4 setup does not apply as-is
because the stock Pi-specific boot firmware trick doesn't exist on this board.

## TL;DR

- The board reaches the Create 3 over a **USB-ethernet gadget** link: board
  `usb0 = 192.168.186.3/24` ↔ Create 3 `192.168.186.2`.
- This Create 3 runs **Iron** firmware at the **root namespace**, so it subscribes to
  **`/cmd_vel` directly**. There is **no `create3_republisher`** and no `/_do_not_use`
  bridge — publish `geometry_msgs/Twist` to `/cmd_vel` and it drives.
- Everything is permanent: gadget via **systemd service**
  (`scripts/create3-usb-gadget.service`, installed by `install_ventuno_deps.sh` unless
  `--skip-create3-service`), DDS env in `~/.bashrc`, launch files don't start the republisher.

## Topology — the part that bites people

On a stock TurtleBot 4 the **Create 3 is the USB host** and the **compute board is the USB
device (gadget)**. The stock Pi does this via `dtoverlay=dwc2,dr_mode=peripheral` + `g_ether`
in Pi-specific boot firmware, which does nothing on this Qualcomm board — the gadget is
built by hand instead (`scripts/create3_usb_gadget.sh`).

Consequences:
- Plug the cable into the board's **USB-C** port (dual-role/`dwc3`, in device mode) →
  Create 3's USB-C.
- The board's **USB-A ports are host-only** and will not work for this link.
- A **charge-only cable** enumerates nothing at all (total silence in `dmesg`) — must be a
  data cable.

## The USB gadget

`scripts/create3_usb_gadget.sh` builds a CDC-ECM gadget with `configfs`/`libcomposite`,
bound to the board's UDC (`a600000.usb`):
- interface `usb0`, MAC `12:34:56:78:9a:bc`
- board IP `192.168.186.3/24`, Create 3 `192.168.186.2`
- ECM host-side MAC (Create 3's usb iface) `12:34:56:78:9a:bd`

Installed as `/etc/systemd/system/create3-usb-gadget.service` (enabled at boot). Uses a
oneshot systemd service rather than NetworkManager because NM marks the gadget interface
*unmanaged*, making an NM/netplan profile unreliable for it.

Manual control:
```bash
sudo scripts/create3_usb_gadget.sh        # bring up (idempotent)
sudo scripts/create3_usb_gadget.sh down   # tear down
sudo systemctl start create3-usb-gadget   # same as boot path
```

## DDS discovery over usb0

The board has multiple NICs (e.g. Wi-Fi is the default route). Fast DDS joins discovery
multicast on the default NIC, not `usb0`, by default — so it never hears the base even
though `tcpdump` shows the base's SPDP arriving on `usb0`. Fix: whitelist the link's
interface with `scripts/fastdds_usb0.xml` (loopback + `192.168.186.3`). See
[references/ros-networking.md](ros-networking.md) for the general DDS/env details.

DDS env (lives in `~/.bashrc` / `~/.ventuno_object_tracking_env`; must match the Create 3's
simple/multicast discovery config, so `ROS_DISCOVERY_SERVER` stays unset):
```bash
export ROS_DOMAIN_ID=0
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export FASTRTPS_DEFAULT_PROFILES_FILE=$VENTUNO_OBJECT_TRACKING_ROOT/scripts/fastdds_usb0.xml
```

## Namespace: no republisher needed

A stock Jazzy TB4 hides the Create 3 under `/_do_not_use` and needs `create3_republisher`
to expose clean topics. This base does **not** need that — it runs Iron at the root
namespace, so its `motion_control` node subscribes to `/cmd_vel` directly, and
`geometry_msgs/Twist`'s type hash is identical between Iron and Jazzy
(`RIHS01_9c45bf16…`), so a Jazzy publisher matches it fine.

If a launch file is publishing to a `/_do_not_use`-prefixed topic or starting
`create3_republisher`, that's wrong for this base — publish `Twist` straight to `/cmd_vel`
(see `movement_test.launch.py` and `tracker.launch.py` for the working pattern).

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

The gadget link is fragile across USB bounces. Two failure modes, and how to break the loop:

**1. After the Create 3 reboots**, the gadget's TX (IN) endpoint wedges: the link goes
one-way — the base's multicast still arrives, but *our* unicast never leaves `usb0`
(`ping` = 100% loss, `neigh FAILED`, `tcpdump` shows no outbound frames).

Fix: re-bind the UDC (unbind then rebind):
```bash
sudo scripts/create3_usb_gadget.sh
# or manually:
G=/sys/kernel/config/usb_gadget/create3
echo "" | sudo tee $G/UDC ; echo a600000.usb | sudo tee $G/UDC
```

**2. Re-binding the gadget bounces the base's USB link**, which makes the base's ROS app go
silent (still pingable, its web UI at `http://192.168.186.2` answers, but no SPDP).

Fix: restart only the base's application — **never hit the physical Reboot button**, which
re-wedges the gadget TX and restarts the whole loop:
```bash
curl -X POST http://192.168.186.2/api/restart-app
# or: Create 3 web UI -> Application -> Restart Application
```

Quick checks:
```bash
cat /sys/class/udc/*/state                 # want: configured
cat /sys/class/net/usb0/carrier            # want: 1
sudo tcpdump -ni usb0 'src 192.168.186.2 and udp'   # base announcing DDS?
```
