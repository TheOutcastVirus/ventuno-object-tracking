#!/usr/bin/env bash
# Bring up a CDC-ECM USB-ethernet gadget so this board (USB *device*) can talk to
# the Create 3 (USB *host*) over the USB-C link, exactly like a stock TurtleBot 4
# Raspberry Pi. Board side = 192.168.186.3/24, Create 3 side = 192.168.186.2.
#
#   sudo scripts/create3_usb_gadget.sh          # set up + bind
#   sudo scripts/create3_usb_gadget.sh down     # tear down
#
# Idempotent: safe to re-run.
set -euo pipefail

G=/sys/kernel/config/usb_gadget/create3
UDC_NAME="$(ls /sys/class/udc | head -n1)"     # a600000.usb on this board
IFACE_IP="192.168.186.3/24"
DEV_MAC="12:34:56:78:9a:bc"                     # board side (stable -> stable ifname)
HOST_MAC="12:34:56:78:9a:bd"                    # Create 3 side

teardown() {
  if [ -d "$G" ]; then
    echo "" > "$G/UDC" 2>/dev/null || true
    rm -f "$G"/configs/c.1/ecm.usb0 2>/dev/null || true
    [ -d "$G/configs/c.1/strings/0x409" ] && rmdir "$G/configs/c.1/strings/0x409" 2>/dev/null || true
    [ -d "$G/configs/c.1" ] && rmdir "$G/configs/c.1" 2>/dev/null || true
    [ -d "$G/functions/ecm.usb0" ] && rmdir "$G/functions/ecm.usb0" 2>/dev/null || true
    [ -d "$G/strings/0x409" ] && rmdir "$G/strings/0x409" 2>/dev/null || true
    rmdir "$G" 2>/dev/null || true
    echo "gadget torn down"
  fi
}

if [ "${1:-}" = "down" ]; then teardown; exit 0; fi

modprobe libcomposite
teardown   # clean slate

mkdir -p "$G"
cd "$G"
echo 0x1d6b > idVendor          # Linux Foundation
echo 0x0104 > idProduct         # Multifunction Composite Gadget
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

mkdir -p strings/0x409
echo "0123456789" > strings/0x409/serialnumber
echo "Ventuno"    > strings/0x409/manufacturer
echo "Create3 Link" > strings/0x409/product

mkdir -p configs/c.1/strings/0x409
echo "CDC ECM" > configs/c.1/strings/0x409/configuration
echo 250 > configs/c.1/MaxPower

mkdir -p functions/ecm.usb0
echo "$DEV_MAC"  > functions/ecm.usb0/dev_addr
echo "$HOST_MAC" > functions/ecm.usb0/host_addr

ln -s functions/ecm.usb0 configs/c.1/

echo "$UDC_NAME" > UDC           # bind -> creates local usb net iface
echo "bound gadget to UDC $UDC_NAME"

# The ecm function records the interface name it created; read it directly.
sleep 1
IFACE="$(cat "$G/functions/ecm.usb0/ifname" 2>/dev/null || true)"
if [ -z "$IFACE" ] || [ ! -e "/sys/class/net/$IFACE" ]; then
  echo "WARN: gadget iface not found yet"; exit 0
fi
ip addr flush dev "$IFACE" 2>/dev/null || true
ip addr add "$IFACE_IP" dev "$IFACE"
ip link set "$IFACE" up
echo "configured $IFACE -> $IFACE_IP"
