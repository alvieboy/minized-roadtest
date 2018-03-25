all: boot.bin
.PHONY: boot.bin
boot.bin:
	bootgen -image bootgen.bif -w -o i boot.bin
	@echo
	@echo Now, do: program_flash -fsbl zynq_fsbl.elf -f boot.bin -flash_type qspi_single
	@echo