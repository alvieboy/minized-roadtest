#PROJECT=papilio_pro
#PART=xc6slx9-2-tqg144

# For bootloader
BOARD=MINIZED
DEFINES="-DBOARD_ID=0xBC010000 -DBOARD_MEMORYSIZE=0x20000 -DZPU20 -DZPUINO_HAS_ICACHE"
ZPUDIR=../../../zpuino/

all: bootloader.vhd

bootloader.vhd:
	$(MAKE) -C $(ZPUDIR)/bootloader -f Makefile BOARD=$(BOARD) SIZE=$(SIZE) DEFINES=$(DEFINES)
	cp $(ZPUDIR)/bootloader/bootloader.vhd .
	clean:
	rm -f bootloader.vhd
	$(MAKE) -C $(ZPUDIR)/bootloader clean
