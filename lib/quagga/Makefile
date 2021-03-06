#   BSD LICENSE
#
#   Copyright(c) 2015 Intel Corporation. All rights reserved.
#   All rights reserved.
#
#   Redistribution and use in source and binary forms, with or without
#   modification, are permitted provided that the following conditions
#   are met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in
#       the documentation and/or other materials provided with the
#       distribution.
#     * Neither the name of Intel Corporation nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
#   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

# Default target, can be overwritten by command line or environment
RTE_TARGET ?= x86_64-native-linuxapp-gcc

include $(RTE_SDK)/mk/rte.vars.mk

ifneq ($(CONFIG_RTE_EXEC_ENV),"linuxapp")
$(error This application can only operate in a linuxapp environment, \
please change the definition of the RTE_TARGET environment variable)
endif

# library name
LIB = libquagga.a

LIBABIVER := 1

# all source are stored in SRC-Y
SRCS-y :=\
	str.c event_counter.c linklist.c agentx.c network.c	routemap.c \
	workqueue.c vrf.c memtypes.c sockunion.c nexthop.c buffer.c \
	privs.c table.c smux.c if.c log.c pqueue.c pid_output.c regex.c \
	checksum.c snmp.c md5.c distribute.c vty.c memory.c sigevent.c \
	daemon.c hash.c command.c getopt.c filter.c thread.c stream.c \
	if_rmap.c zclient.c	jhash.c	prefix.c plist.c sockopt.c getopt1.c\
	keychain.c vector.c

#CFLAGS += $(WERROR_FLAGS)
CFLAGS_QUAGGA := -Wstrict-prototypes -Wmissing-prototypes
CFLAGS_QUAGGA += -Wmissing-declarations -Wpointer-arith
CFLAGS_QUAGGA += -Wcast-align
CFLAGS_QUAGGA += -Wformat-security
CFLAGS_QUAGGA += -Wundef -Wwrite-strings
#CFLAGS_QUAGGA += -W -Wformat-nonliteral -Wold-style-definition -Wcast-qual -Wnested-externs

CFLAGS += -g -O3 --std=gnu99 $(CFLAGS_QUAGGA)
CFLAGS += -D_GNU_SOURCE -D__USE_GNU -D__STDC_FORMAT_MACROS


ifeq ($(CONFIG_RTE_BUILD_SHARED_LIB),y)
ifeq ($(CONFIG_RTE_LIBRTE_IXGBE_PMD),y)
LDLIBS += -lrte_pmd_ixgbe
endif
endif

include $(RTE_SDK)/mk/rte.extlib.mk
