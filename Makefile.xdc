ROOTDIR ?= ../dvsdk
TARGET ?= xdc
include $(ROOTDIR)/Rules.make

LIBDIR = xdc_lib
XDC_PATH = $(USER_XDC_PATH);../../packages;$(DMAI_INSTALL_DIR)/packages;$(CE_INSTALL_DIR)/packages;$(FC_INSTALL_DIR)/packages;$(LINK_INSTALL_DIR)/packages;$(XDAIS_INSTALL_DIR)/packages;$(CMEM_INSTALL_DIR)/packages;$(CODEC_INSTALL_DIR)/packages;$(CE_INSTALL_DIR)/examples
XDC_CFG		= $(TARGET)_config
XDC_CFLAGS	= $(XDC_CFG)/compiler.opt
XDC_LFILE	= $(XDC_CFG)/linker.cmd
XDC_CFGFILE	= $(TARGET).cfg
XDC_PLATFORM = ti.platforms.evmDM365
XDC_TARGET = gnu.targets.arm.GCArmv5T
export CSTOOL_DIR
CONFIGURO = $(XDC_INSTALL_DIR)/xs xdc.tools.configuro
CONFIG_BLD = config.bld

xdclib: $(XDC_LFILE)
	@mkdir -p $(LIBDIR)
	@cp -n $(shell cat $(XDC_LFILE) | tail -n +5 | head -n -2) $(LIBDIR)/
	@echo Generating xdclink.cmd
	@sed "s/\/[a-zA-Z].*\//$(LIBDIR)\//" $(XDC_CFG)/linker.cmd > xdclink.cmd
	@sed "s/INPUT/GROUP/" -i xdclink.cmd

$(XDC_LFILE) $(XDC_CFLAGS):	$(XDC_CFGFILE)
	@echo Configuring application using $<
	$(VERBOSE) XDCPATH="$(XDC_PATH)" $(CONFIGURO) -o $(XDC_CFG) -t $(XDC_TARGET) -p $(XDC_PLATFORM) -b $(CONFIG_BLD) $(XDC_CFGFILE)

clean:
	rm -rf $(XDC_CFG)
	rm -rf $(LIBDIR)
	rm xdclink.cmd
