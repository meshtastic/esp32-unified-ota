import os
Import("env")

# --- Configuration ---
# Set the maximum size for your target partition (ota_1) in bytes.
# 0x0A0000 hex = 655360 decimal
TARGET_PARTITION_MAX_SIZE = 655360

def check_firmware_size(source, target, env):
    """
    This function is called after the firmware.bin is generated.
    """
    # target[0] is the SCons node for the .bin file
    firmware_path = str(target[0])
    
    # Sanity check if the file exists
    if not os.path.exists(firmware_path):
        print(f"Error: Could not find firmware.bin at {firmware_path}")
        return

    # Get the actual size of the file
    firmware_size = os.path.getsize(firmware_path)
    remaining = TARGET_PARTITION_MAX_SIZE - firmware_size

    print(f"--- OTA_1 Size Check ---")
    print(f"Size : {firmware_size} bytes")
    print(f"Limit: {TARGET_PARTITION_MAX_SIZE} bytes")

    if firmware_size > TARGET_PARTITION_MAX_SIZE:
        print(f"\033[91mError: Firmware size ({firmware_size} bytes) exceeds limit by {abs(remaining)} bytes.\033[0m")
        # Exit with a non-zero code to fail the build
        env.Exit(1)
    else:
        print(f"\033[92mSuccess: {remaining} bytes free.\033[0m")
    print("------------------------")

# Attach the action specifically to the .bin output file
# This ensures it runs whenever the binary is updated/linked
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_firmware_size)
