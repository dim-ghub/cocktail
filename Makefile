# Sober refuses Vulkan layers loaded from writable paths (anything under
# ~/.var/app crashes it), so the layer ships as a Flatpak VulkanLayer
# extension, mounted read-only at /usr/lib/extensions/vulkan/SoberFps.

EXT_ID     := org.freedesktop.Platform.VulkanLayer.SoberFps
BRANCH     := 25.08
SDK        := org.gnome.Sdk//50
APP        := org.vinegarhq.Sober
EXT_PREFIX := /usr/lib/extensions/vulkan/SoberFps
BIN_DIR    := $(HOME)/.local/bin

BUILD  := $(CURDIR)/build
STAGE  := $(BUILD)/ext
REPO   := $(BUILD)/repo
BUNDLE := $(BUILD)/soberfps.flatpak

all: $(BUNDLE)

# Build inside the GNOME 50 SDK so the .so matches the runtime's glibc.
libsober_fpslimit.so: fpslimit_layer.c
	flatpak run --filesystem=$(CURDIR) --command=gcc $(SDK) \
		-O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra \
		-o $(CURDIR)/$@ $(CURDIR)/$< -lpthread

$(BUNDLE): libsober_fpslimit.so sober_fpslimit.json.in
	rm -rf $(STAGE) $(REPO)
	install -Dm755 libsober_fpslimit.so $(STAGE)/files/lib/libsober_fpslimit.so
	mkdir -p $(STAGE)/files/share/vulkan/implicit_layer.d
	sed 's|@LIB@|$(EXT_PREFIX)/lib/libsober_fpslimit.so|' sober_fpslimit.json.in \
		> $(STAGE)/files/share/vulkan/implicit_layer.d/sober_fpslimit.json
	printf '[Runtime]\nname=$(EXT_ID)\n\n[ExtensionOf]\nref=runtime/org.freedesktop.Platform/x86_64/$(BRANCH)\n' \
		> $(STAGE)/metadata
	flatpak build-export --runtime --subject="soberfps" $(REPO) $(STAGE) $(BRANCH)
	flatpak build-bundle --runtime $(REPO) $@ $(EXT_ID) $(BRANCH)

install: $(BUNDLE)
	flatpak install --user -y --noninteractive --reinstall --bundle $(BUNDLE)
	flatpak override --user --env=SOBER_FPSLIMIT=1 $(APP)
	install -Dm755 soberfps $(BIN_DIR)/soberfps

uninstall:
	-flatpak uninstall --user -y --noninteractive $(EXT_ID)//$(BRANCH)
	-flatpak override --user --unset-env=SOBER_FPSLIMIT $(APP)
	rm -f $(BIN_DIR)/soberfps

clean:
	rm -rf $(BUILD) libsober_fpslimit.so

.PHONY: all install uninstall clean
