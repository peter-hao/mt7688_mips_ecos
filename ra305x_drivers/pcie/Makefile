include $(TOPDIR)/rules.mak
include $(TOPDIR)/.config

EXTRA_CFLAGS = -I. -D_KERNEL -mips32r2
ALL_OBJS = ra_pcie.o

MODULE = $(shell pwd | sed "s/.*\///" ).o

all: $(MODULE)

$(MODULE): $(ALL_OBJS:.o=.d)
	$(XLD) -r $(ENDIAN) -o $(MODULE) $(ALL_OBJS)

clean:
	rm -f $(MODULE) $(ALL_OBJS)
	rm -f *.d
