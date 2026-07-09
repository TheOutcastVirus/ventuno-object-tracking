#!/usr/bin/env bash
# Optional remote access setup. Installs Tailscale, generates an SSH key if one
# is missing, and optionally brings Tailscale up.
#
# Usage:
#   bash scripts/install_tailscale_ssh.sh
#   TAILSCALE_AUTH_KEY=tskey-auth-... bash scripts/install_tailscale_ssh.sh
set -euo pipefail

SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519}"
TAILSCALE_AUTH_KEY="${TAILSCALE_AUTH_KEY:-}"

sudo_cmd() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    sudo "$@"
  fi
}

echo "Installing Tailscale..."
curl -fsSL https://tailscale.com/install.sh | sudo_cmd sh

if command -v systemctl >/dev/null 2>&1; then
  sudo_cmd systemctl enable --now tailscaled
fi

echo "Generating SSH key if needed..."
mkdir -p "$HOME/.ssh"
chmod 700 "$HOME/.ssh"
if [ ! -f "$SSH_KEY" ]; then
  ssh-keygen -t ed25519 -a 100 -f "$SSH_KEY" -N "" -C "$(whoami)@$(hostname)"
fi

echo "Running tailscale up..."
if [ -n "$TAILSCALE_AUTH_KEY" ]; then
  sudo_cmd tailscale up --ssh --auth-key "$TAILSCALE_AUTH_KEY"
else
  sudo_cmd tailscale up --ssh
fi

echo
echo "SSH public key:"
cat "$SSH_KEY.pub"