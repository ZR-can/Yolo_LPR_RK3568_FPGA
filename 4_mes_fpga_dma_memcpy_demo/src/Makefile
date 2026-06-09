CC ?= aarch64-buildroot-linux-gnu-gcc
INCLUDE +=
CFLAGS  += -g -Wall -O2 $(INCLUDE)
LIBS    +=
LDFLAGS :=
CXXFLAGS:= $(CFLAGS)
TARGETS := dma_memcpy_demo rk_fpga_pcie_comm_demo

.PHONY : everything objs clean distclean rebuild

all : $(TARGETS)

objs : mes_dma_memcpy_demo.o rk_fpga_pcie_comm_demo.o

rebuild: distclean all

clean :
	rm -rf *~
	rm -rf *.o

distclean : clean
	rm -rf $(TARGETS)

dma_memcpy_demo : mes_dma_memcpy_demo.o
	$(CC) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

rk_fpga_pcie_comm_demo : rk_fpga_pcie_comm_demo.o
	$(CC) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

%.o : %.c
	$(CC) $(CFLAGS) -c -o $@ $<
