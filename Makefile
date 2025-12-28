
# Copyright (c) 2012, Mauro Scomparin
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of Mauro Scomparin nor the
#       names of its contributors may be used to endorse or promote products
#       derived from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY Mauro Scomparin ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL Mauro Scomparin BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# File:		Makefile.
# Author:	Mauro Scomparin <http://scompoprojects.worpress.com>.
# Version:	1.0.0.
# Description:	Sample makefile.


#==============================================================================
#           Cross compiling toolchain / tools specifications
#==============================================================================

# Prefix for the arm-eabi-none toolchain.
# I'm using codesourcery g++ lite compilers available here:
# http://www.mentor.com/embedded-software/sourcery-tools/sourcery-codebench/editions/lite-edition/
PREFIX_ARM = arm-none-eabi

# Microcontroller properties.
#PART=TM4C1233H6PM
PART=TM4C1294NCPDT
CPU=-mcpu=cortex-m4
#FPU=-mfpu=fpv4-sp-d16 -mfloat-abi=soft
FPU=-mfpu=fpv4-sp-d16 -mfloat-abi=hard

# Stellarisware path
STELLARISWARE_PATH=/home/mosagepa/decomp/STM32/TI_BOARDS/TIVAWARE/

# Program name definition for ARM GNU C compiler.
CC      = ${PREFIX_ARM}-gcc
# Program name definition for ARM GNU Linker.
LD      = ${PREFIX_ARM}-ld
# Program name definition for ARM GNU Object copy.
CP      = ${PREFIX_ARM}-objcopy
# Program name definition for ARM GNU Object dump.
OD      = ${PREFIX_ARM}-objdump

# Option arguments for C compiler.
CFLAGS=-mthumb ${CPU} ${FPU} -O0 -ffunction-sections -fdata-sections -MD -std=c99 -Wall -Wno-pedantic -c -g

# Library stuff passed as flags!
CFLAGS+= -I ${STELLARISWARE_PATH} -I/usr/local/arm-none-eabi/include -DPART_$(PART) -c -DTARGET_IS_BLIZZARD_RA1

# MOSA DEBUG: fix issue when driverlib is already prebuilt with VFP
LDFLAGS += ${FPU} #syscalls_GHCP_ICDI.o

# Flags for LD
LFLAGS  = --gc-sections --stats --print-memory-usage

# Flags for objcopy
CPFLAGS = -Obinary

# flags for objectdump
ODFLAGS = -S

# Some nice coloring...
PRINT = @echo "\e[1;34mBuilding $<\e[0m"
PRINTFLASH = @echo "\e[1;34mFlashing the firmware... $<\e[0m"

# I want to save the path to libgcc, libc.a and libm.a for linking.
# I can get them from the gcc frontend, using some options.
# See gcc documentation
LIB_GCC_PATH=${shell ${CC} ${CFLAGS} -print-libgcc-file-name}
LIBC_PATH=${shell ${CC} ${CFLAGS} -print-file-name=libc.a}
LIBM_PATH=${shell ${CC} ${CFLAGS} -print-file-name=libm.a}

# Uploader tool path.

# Set a relative or absolute path to the upload tool program.
# I used this project: https://github.com/utzig/lm4tools
FLASHER=lm4flash

# Use `SUDO=` for sudo-less flashing (after udev rules), or keep default `sudo`.
SUDO ?= sudo

# Flags for the uploader program.
FLASHER_FLAGS=


#==============================================================================
#                         Project properties
#==============================================================================

# Project name (W/O .c extension eg. "main")
PROJECT_NAME = integr_V03

# UART device defaults (override: `make capture UART0_DEV=/dev/ttyACM0 UART3_DEV=/dev/ttyUSB1`)
UART0_DEV ?= /dev/ttyACM0
UART0_BAUD ?= 9600
UART3_DEV ?= /dev/ttyUSB1
UART3_BAUD ?= 115200


# Startup file name (W/O .c extension eg. "LM4F_startup")
STARTUP_FILE = TM4C1294XL_startup

# Linker file name
LINKER_FILE = TM4C1294XL.ld

SRC = $(wildcard *.c)
OBJS = $(SRC:.c=.o)


#==============================================================================
#                      Rules to make the target
#==============================================================================

#make all rule
all: $(OBJS) ${PROJECT_NAME}.axf ${PROJECT_NAME}

%.o: %.c
	@echo
	$(PRINT)
	@echo Compiling $<...
	$(CC) -c $(CFLAGS) ${<} -o ${@}

${PROJECT_NAME}.axf: $(OBJS)
	@echo
	@echo Making driverlib...
	$(MAKE) -C ${STELLARISWARE_PATH}driverlib/
	@echo
	$(PRINT)
	@echo Linking...
	$(LD) -T $(LINKER_FILE) $(LFLAGS) -o ${PROJECT_NAME}.axf $(OBJS) ${STELLARISWARE_PATH}driverlib/gcc/libdriver.a $(LIBM_PATH) $(LIBC_PATH) $(LIB_GCC_PATH)

${PROJECT_NAME}: ${PROJECT_NAME}.axf
	@echo
	$(PRINT)
	@echo Copying...
	$(CP) $(CPFLAGS) ${PROJECT_NAME}.axf ${PROJECT_NAME}.bin
	@echo
	$(PRINT)
	@echo Creating list file...
	$(OD) $(ODFLAGS) ${PROJECT_NAME}.axf > ${PROJECT_NAME}.lst

# make clean rule
clean:
	rm -f *.bin *.o *.d *.axf *.lst


# Rule to load the project to the board
# I added a sudo because it's needed without a rule.
flash: all
	@echo
	$(PRINTFLASH)
	${SUDO} ${FLASHER} ${PROJECT_NAME}.bin ${FLASHER_FLAGS}
	@echo

# Capture UART logs (writes to ./logs/)
capture:
	python3 tools/uart_session.py --uart0 "$(UART0_DEV)" --uart0-baud $(UART0_BAUD) --uart3 "$(UART3_DEV)" --uart3-baud $(UART3_BAUD) --duration 0

# Send a command to UART3 (example: `make send-uart3 CMD='PSYN 44\r\n'`)
send-uart3:
	@if [ -z "$(CMD)" ]; then echo "ERROR: provide CMD='...'. Example: make send-uart3 CMD='PSYN 44\\r\\n'"; exit 2; fi
	python3 tools/uart_session.py --no-uart0 --uart3 "$(UART3_DEV)" --uart3-baud $(UART3_BAUD) --send-uart3 "$(CMD)" --duration 1 --quiet

# Autonomous build + flash + capture + optional UART3 command
# Examples:
#   make auto
#   make auto CMD='PSYN 44\r\n' DURATION=8
auto: flash
	python3 tools/uart_session.py --uart0 "$(UART0_DEV)" --uart0-baud $(UART0_BAUD) --uart3 "$(UART3_DEV)" --uart3-baud $(UART3_BAUD) --duration $${DURATION:-10} $(if $(CMD),--send-uart3 "$(CMD)",)
	@echo "AUTO FLOW: SUCCESS (build+flash+uart)"

