#!/usr/bin/env python3
"""
Generate per-motor EEPROM config images for the FIREFLY_G20_F051_CAN target.

The same firmware binary is flashed to all four MCUs; each motor position
then gets a tiny EEPROM image that sets its DroneCAN node ID and ESC index
(bytes 176 and 177 of the 192-byte EEPROM page at 0x08007C00). Everything
else is 0xFF: the firmware hard-loads the Firefly settings at boot and
sanitises out-of-range CAN bytes to defaults.

Usage:
    python3 tools/gen_firefly_can_config.py [--base-node-id 40] [--out obj]

Flash per position (after flashing the firmware itself):
    st-flash write obj/can_config_m1.bin 0x08007C00   # motor 1 connector
    st-flash write obj/can_config_m2.bin 0x08007C00   # motor 2 connector
    ...then power-cycle.
"""
import argparse
import pathlib

EEPROM_SIZE = 192
CAN_NODE_OFFSET = 176
ESC_INDEX_OFFSET = 177
REQUIRE_ARMING_OFFSET = 178


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base-node-id", type=int, default=40,
                    help="node ID for motor 1; motors 2-4 use +1..+3 (default 40; 30 would put motor 3 on ID 32, which am32.ca corrupts)")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("obj"),
                    help="output directory (default obj/)")
    ap.add_argument("--require-arming", action="store_true",
                    help="only spin when DroneCAN ArmingStatus says FULLY_ARMED. "
                         "Requires a flight controller that broadcasts ArmingStatus "
                         "(some PX4 builds do not); default off = obey RawCommand "
                         "directly, like a PWM ESC (PX4 sends zeros when disarmed "
                         "and the firmware zeroes throttle after 250 ms of silence)")
    args = ap.parse_args()

    if not 1 <= args.base_node_id <= 124:
        ap.error("base node ID must be 1..124 (IDs base..base+3 must stay <= 127)")

    # am32.ca round-trips EEPROM bytes 176..183 as text and trims ASCII
    # whitespace, so a node ID of 9..13 or 32 is dropped on save, shifting
    # the ESC index into the node-ID byte and corrupting require_arming.
    bad = [n for n in range(args.base_node_id, args.base_node_id + 4) if n in (9, 10, 11, 12, 13, 32)]
    if bad:
        ap.error(f"node ID(s) {bad} are ASCII whitespace and get corrupted by an "
                 "am32.ca settings save; choose a base that avoids 9..13 and 32 "
                 "(e.g. --base-node-id 40)")

    args.out.mkdir(parents=True, exist_ok=True)
    for esc_index in range(4):
        buf = bytearray(b"\xff" * EEPROM_SIZE)
        # byte 0 is the AM32 bootloader's boot flag: anything other than
        # 0x01 keeps the bootloader resident and the app NEVER runs
        buf[0] = 0x01
        buf[CAN_NODE_OFFSET] = args.base_node_id + esc_index
        buf[ESC_INDEX_OFFSET] = esc_index
        buf[REQUIRE_ARMING_OFFSET] = 1 if args.require_arming else 0
        path = args.out / f"can_config_m{esc_index + 1}.bin"
        path.write_bytes(buf)
        print(f"{path}: node_id={buf[CAN_NODE_OFFSET]} esc_index={esc_index} "
              f"require_arming={buf[REQUIRE_ARMING_OFFSET]}")


if __name__ == "__main__":
    main()
