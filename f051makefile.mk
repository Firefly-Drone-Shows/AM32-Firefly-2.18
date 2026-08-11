MCU := F051
PART := STM32F051x8

MCU_LC := $(call lc,$(MCU))

TARGETS_$(MCU) := $(call get_targets,$(MCU))

HAL_FOLDER_$(MCU) := $(HAL_FOLDER)/$(MCU_LC)

MCU_$(MCU) := -mcpu=cortex-m0 -mthumb
LDSCRIPT_$(MCU) := $(wildcard $(HAL_FOLDER_$(MCU))/*.ld)

SRC_BASE_DIR_$(MCU) := \
	$(HAL_FOLDER_$(MCU))/Startup \
	$(HAL_FOLDER_$(MCU))/Drivers/STM32F0xx_HAL_Driver/Src

SRC_DIR_$(MCU) := $(SRC_BASE_DIR_$(MCU)) \
	$(HAL_FOLDER_$(MCU))/Src

CFLAGS_$(MCU) := \
	-I$(HAL_FOLDER_$(MCU))/Inc \
	-I$(HAL_FOLDER_$(MCU))/Drivers/STM32F0xx_HAL_Driver/Inc \
	-I$(HAL_FOLDER_$(MCU))/Drivers/CMSIS/Include \
	-I$(HAL_FOLDER_$(MCU))/Drivers/CMSIS/Device/ST/STM32F0xx/Include

CFLAGS_$(MCU) += \
	-DHSE_VALUE=8000000 \
	-D$(PART) \
	-DHSE_STARTUP_TIMEOUT=100 \
	-DLSE_STARTUP_TIMEOUT=5000 \
	-DLSE_VALUE=32768 \
	-DDATA_CACHE_ENABLE=0 \
	-DINSTRUCTION_CACHE_ENABLE=0 \
	-DVDD_VALUE=3300 \
	-DLSI_VALUE=40000 \
	-DHSI_VALUE=8000000 \
	-DUSE_FULL_LL_DRIVER \
	-DPREFETCH_ENABLE=1

SRC_$(MCU) := $(foreach dir,$(SRC_DIR_$(MCU)),$(wildcard $(dir)/*.[cs]))

# optional CAN support (TCAN4550 over SPI - the F051 has no CAN peripheral).
# -Os overrides the global -O3: the DroneCAN stack does not fit at -O3 in
# the 27K app region. Trailing flags win in gcc, and this is only applied
# to _CAN targets.
CFLAGS_CAN_$(MCU) = \
	-ISrc/DroneCAN \
	-ISrc/DroneCAN/libcanard \
	-ISrc/DroneCAN/dsdl_generated/include \
	-Os \
	-fno-unwind-tables -fno-asynchronous-unwind-tables

SRC_DIR_CAN_$(MCU) = Src/DroneCAN \
		Src/DroneCAN/dsdl_generated/src \
		Src/DroneCAN/libcanard

SRC_CAN_$(MCU) := $(foreach dir,$(SRC_DIR_CAN_$(MCU)),$(wildcard $(dir)/*.[cs]))

# CAN variant lives in ldscripts/ so the LDSCRIPT_F051 wildcard above
# does not pick it up for non-CAN builds
LDSCRIPT_CAN_$(MCU) := $(HAL_FOLDER_$(MCU))/ldscripts/ldscript_CAN.ld
