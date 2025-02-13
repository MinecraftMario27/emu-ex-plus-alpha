include $(buildSysPath)/imagineCommonTarget.mk
include $(buildSysPath)/evalPkgConfigCFlags.mk

$(OBJ) : $(genConfigH)

targetFile := $(target).a

$(targetDir)/$(targetFile) : $(OBJ)
	@echo "Archiving $@"
	@mkdir -p `dirname $@`
	$(PRINT_CMD)rm -f $@ && $(AR) rcs $@ $^

genPkgConf = $(targetDir)/$(libName).pc

$(genPkgConf) : $(imaginePkgconfigTemplate)
	@echo "Generating pkg-config file $@"
	@mkdir -p $(@D)
	$(PRINT_CMD)sed -e 's/NAME/$(pkgName)/' \
	-e 's:DESCRIPTION:$(pkgDescription):' \
	-e 's/VERSION/$(pkgVersion)/' \
	-e 's/REQUIRES/$(pkgConfigDeps) $(pkgConfigStaticDeps)/' \
	-e 's:CFLAGS:$(pkgCFlags):' \
	-e 's:LIBS:$(LDLIBS):' < $(imaginePkgconfigTemplate) > $@

.PHONY: pkgconfig
pkgconfig : $(genPkgConf)

main: $(targetDir)/$(targetFile) $(genPkgConf)

.PHONY: clean
clean :
	rm -f $(targetDir)/$(targetFile)
	rm -rf $(genPath)
	rm -rf $(objDir)
