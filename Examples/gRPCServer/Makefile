LOCAL_LIBH2O_PATH ?= ../../../h2o

EXECUTABLE := grpctest

DIRECTORIES := \
	../../Ring \
	../../Supplimentary

LIBRARIES := \
	pthread \
	bsd \
	m

DEPENDENCIES := \
	liburing \
	jemalloc \
	libuv \
	zlib \
	openssl \
	libprotobuf-c

OBJECTS := \
	../../Ring/FastRing.o \
	../../Ring/FastUVLoop.o \
	../../Supplimentary/H2OCore.o \
	../../Supplimentary/ProtoBuf.o \
	../../Supplimentary/gRPCServer.o \
	gRPCTest.pb-c.o \
	gRPCTest.o

ifneq ($(LOCAL_LIBH2O_PATH), )
	FLAGS   += -DUSE_LOCAL_H2O -I$(LOCAL_LIBH2O_PATH)/include -I$(LOCAL_LIBH2O_PATH)/deps/picotls/include/ -I$(LOCAL_LIBH2O_PATH)/deps/quicly/include
	OBJECTS += $(LOCAL_LIBH2O_PATH)/build/libh2o.a
else
	DEPENDENCIES += libh2o
endif

FLAGS += \
	-Wno-unused-result -Wno-format-truncation -Wno-format-overflow -Wno-stringop-overflow \
	-rdynamic -fno-omit-frame-pointer -O2 -MMD -gdwarf \
	$(foreach directory, $(DIRECTORIES), -I$(directory)) \
	$(shell pkg-config --cflags $(DEPENDENCIES))

CFLAGS   += $(FLAGS)
CXXFLAGS += $(FLAGS)

LIBS := \
	$(foreach library, $(LIBRARIES), -l$(library)) \
	$(shell pkg-config --libs $(DEPENDENCIES))

all: build

%.pb-c.c: %.proto
	protoc --c_out=. $<

$(LOCAL_LIBH2O_PATH)/build/libh2o.a:
	mkdir -p $(LOCAL_LIBH2O_PATH)/build
	cmake -S $(LOCAL_LIBH2O_PATH) -B $(LOCAL_LIBH2O_PATH)/build -D CMAKE_VERBOSE_MAKEFILE:BOOL=ON -D CMAKE_BUILD_TYPE=Release -D WITH_LIBUV=ON -D WITH_MRUBY=OFF
	+make -C $(LOCAL_LIBH2O_PATH)/build

build: $(PREREQUISITES) $(OBJECTS)
	$(CC) $(OBJECTS) $(FLAGS) $(LIBS) -o $(EXECUTABLE)

clean:
	rm -f $(EXECUTABLE) $(filter %.o,$(OBJECTS)) $(wildcard $(filter %.d,$(OBJECTS:.o=.d)))
	find . -name "*.pb-c.?" | xargs rm -f

-include $(wildcard $(filter %.d,$(OBJECTS:.o=.d)))
