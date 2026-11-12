# RD91 RTT 3.1.4 scaffold; based on SDK RT_Thread10 and local UART-GCC.
TARGET = rd91_controller

DEBUG = 1
OPT = -Os

BUILD_DIR = build

######################################
# paths
######################################
SDK_ROOT ?= ../../_sdk/Nsingtech.N32WB43x_Library.2.2.0
PROJECT_ROOT = $(SDK_ROOT)

C_SOURCES =  \
src/main.c \
src/alarm_output.c \
src/charge_led.c \
src/battery_gauge.c \
bsp/n32wb43x_it.c \
bsp/rt_board.c \
bsp/board.c \
bsp/i2c1.c \
drivers/imu_qmi8a01z.c \
drivers/wt588f02.c \
drivers/sgm41513.c \
drivers/cw2217.c \
app/rd91_app.c \
app/impact.c \
app/button.c \
app/motion_stats.c \
app/shock_detect.c \
app/charge_indicator.c \
app/battery_level.c \
$(PROJECT_ROOT)/firmware/CMSIS/device/system_n32wb43x.c \
$(PROJECT_ROOT)/firmware/n32wb43x_std_periph_driver/src/misc.c \
$(PROJECT_ROOT)/firmware/n32wb43x_std_periph_driver/src/n32wb43x_gpio.c \
$(PROJECT_ROOT)/firmware/n32wb43x_std_periph_driver/src/n32wb43x_rcc.c \
$(PROJECT_ROOT)/firmware/n32wb43x_std_periph_driver/src/n32wb43x_usart.c \
$(PROJECT_ROOT)/firmware/n32wb43x_std_periph_driver/src/n32wb43x_i2c.c \
$(PROJECT_ROOT)/firmware/n32wb43x_std_periph_driver/src/n32wb43x_exti.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/clock.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/device.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/idle.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/ipc.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/irq.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/kservice.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/mem.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/memheap.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/mempool.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/object.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/scheduler.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/slab.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/thread.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/timer.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/components.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/cpu.c \
$(PROJECT_ROOT)/middlewares/rt-thread/src/signal.c \
$(PROJECT_ROOT)/middlewares/rt-thread/libcpu/cortex-m4/cpuport.c \
$(PROJECT_ROOT)/projects/n32wb43x_EVAL/examples/RT_Thread/DeviceDrivers/uart/src/drv_usart.c \
$(PROJECT_ROOT)/middlewares/rt-thread/components/drivers/serial/serial.c \
$(PROJECT_ROOT)/middlewares/rt-thread/components/drivers/src/completion.c

# 未编入 SDK 的 RT_Thread GPIO 设备驱动 drv_gpio.c：
# 它定义了 EXTI0..4 / EXTI9_5 的 IRQHandler，与 bsp/n32wb43x_it.c 中
# 我们自己实现的 EXTI0_IRQHandler（IMU INT）重复，链接会报 multiple definition。
# 本工程不使用 rt_pin_* 接口，因此由 bsp 独占 EXTI 所有权。
# 若将来要用 RTT 的 pin 设备，应改为通过 rt_pin_attach_irq 挂中断，而不是两边都定义。

ASM_SOURCES =  \
startup/startup_n32wb43x_gcc.s \
$(PROJECT_ROOT)/middlewares/rt-thread/libcpu/cortex-m4/context_gcc.S

######################################
# binaries
######################################
PREFIX = arm-none-eabi-
ifdef GCC_PATH
CC = "$(GCC_PATH)/$(PREFIX)gcc"
AS = "$(GCC_PATH)/$(PREFIX)gcc" -x assembler-with-cpp
CP = "$(GCC_PATH)/$(PREFIX)objcopy"
SZ = "$(GCC_PATH)/$(PREFIX)size"
else
CC = $(PREFIX)gcc
AS = $(PREFIX)gcc -x assembler-with-cpp
CP = $(PREFIX)objcopy
SZ = $(PREFIX)size
endif
HEX = $(CP) -O ihex
BIN = $(CP) -O binary -S

######################################
# CFLAGS
######################################
CPU = -mcpu=cortex-m4
FPU = -mfpu=fpv4-sp-d16
FLOAT-ABI = -mfloat-abi=soft
MCU = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)

C_DEFS = \
-DUSE_STDPERIPH_DRIVER \
-Dn32wb43x

C_INCLUDES =  \
-I$(PROJECT_ROOT)/firmware/n32wb43x_std_periph_driver/inc \
-Iinc -Ibsp -Iapp -Idrivers \
-I$(PROJECT_ROOT)/middlewares/rt-thread/include \
-I$(PROJECT_ROOT)/projects/n32wb43x_EVAL/examples/RT_Thread/DeviceDrivers/gpio/inc \
-I$(PROJECT_ROOT)/projects/n32wb43x_EVAL/examples/RT_Thread/DeviceDrivers/uart/inc \
-I$(PROJECT_ROOT)/firmware/CMSIS/device \
-I$(PROJECT_ROOT)/firmware/CMSIS/core \
-I$(PROJECT_ROOT)/middlewares/rt-thread/components/drivers/include/drivers \
-I$(PROJECT_ROOT)/middlewares/rt-thread/components/drivers/include \
-I$(PROJECT_ROOT)/middlewares/rt-thread/components/drivers/include/ipc

ASFLAGS = $(MCU) $(AS_DEFS) $(AS_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections
CFLAGS += -std=c99 $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections

ifeq ($(DEBUG), 1)
CFLAGS += -g -gdwarf-2
endif

CFLAGS += -MMD -MP -MF"$(@:%.o=%.d)"

######################################
# LDFLAGS
######################################
LDSCRIPT = linker/n32wb43x_flash.ld

LIBS = -lc -lm -lnosys
LIBDIR =
LDFLAGS = $(MCU) -specs=nano.specs -T$(LDSCRIPT) $(LIBDIR) $(LIBS) \
          -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections \
          -Wl,--print-memory-usage

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
ASM_OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(patsubst %.s,%.o,$(patsubst %.S,%.o,$(ASM_SOURCES)))))
OBJECTS += $(ASM_OBJECTS)
vpath %.s $(sort $(dir $(ASM_SOURCES)))
vpath %.S $(sort $(dir $(ASM_SOURCES)))

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

# GNU Make 4.4 on Windows does not resolve the uppercase .S extension via
# vpath/implicit rules, so link it explicitly (vendor file is context_gcc.S).
$(BUILD_DIR)/context_gcc.o: $(PROJECT_ROOT)/middlewares/rt-thread/libcpu/cortex-m4/context_gcc.S Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile $(LDSCRIPT)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(HEX) $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(BIN) $< $@

$(BUILD_DIR):
	mkdir $@

-include $(wildcard $(BUILD_DIR)/*.d)
