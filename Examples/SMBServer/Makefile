EXECUTABLE := smbserver

ARCHITECTURE    ?= $(shell $(CC) -dumpmachine | grep -o -E '^[^-]+')
PKG_CONFIG_PATH  = /usr/lib/$(ARCHITECTURE)-linux-gnu/pkgconfig/:/usr/lib/pkgconfig:/usr/share/pkgconfig

DIRECTORIES := \
	../../Ring \
	../../Supplimentary

LIBRARIES := \
	pthread \
	bsd \
	m \
	md \
	krb5 \
	com_err \
	k5crypto \
	gssapi_krb5 \
	krb5support

DEPENDENCIES := \
	liburing \
	openssl

OBJECTS := \
	../../Ring/FastRing.o \
	../../Ring/SambarAdapter.o \
	Sombrero.o \
	SMBServer.o \
	/usr/lib/$(ARCHITECTURE)-linux-gnu/libsmb2.a

FLAGS += \
	-Wno-unused-result -Wno-format-truncation -Wno-format-overflow -Wno-stringop-overflow \
	-rdynamic -fno-omit-frame-pointer -O2 -MMD -gdwarf \
	$(foreach directory, $(DIRECTORIES), -I$(directory)) \
	$(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(DEPENDENCIES))

CFLAGS   += $(FLAGS)
CXXFLAGS += $(FLAGS)

LIBS := \
	$(foreach library, $(LIBRARIES), -l$(library)) \
	$(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(DEPENDENCIES))

all: build

build: $(OBJECTS)
	$(CC) $(OBJECTS) $(FLAGS) $(LIBS) -o $(EXECUTABLE)

clean:
	rm -f $(EXECUTABLE) $(filter %.o,$(OBJECTS)) $(wildcard $(filter %.d,$(OBJECTS:.o=.d)))

-include $(wildcard $(filter %.d,$(OBJECTS:.o=.d)))
