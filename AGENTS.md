# Agent notes

## Ventuno Q board setup and debugging

If you're bringing up a new/wiped Ventuno Q board, debugging a build or runtime failure on
the board, or figuring out why NPU inference or the Create 3 link isn't working, read
[`.claude/skills/ventuno-setup/SKILL.md`](.claude/skills/ventuno-setup/SKILL.md) first and
follow its references as needed:

- [`references/executorch-qnn.md`](.claude/skills/ventuno-setup/references/executorch-qnn.md) —
  ExecuTorch build, QNN/HTP chipset patch, model export, QNN runtime issues.
- [`references/create3-connection.md`](.claude/skills/ventuno-setup/references/create3-connection.md) —
  USB gadget link and DDS bring-up for the Create 3 base.
- [`references/ros-networking.md`](.claude/skills/ventuno-setup/references/ros-networking.md) —
  DDS/UDP socket tuning and multi-NIC discovery issues.

The automated entry point for a fresh board is `bash scripts/install_ventuno_deps.sh`
(idempotent, safe to re-run after a partial failure — see `--help` for flags to isolate a
failing stage).
