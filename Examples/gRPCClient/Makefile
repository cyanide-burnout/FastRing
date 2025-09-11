EXECUTABLE := grpctest

DIRECTORIES := \
	../../Ring \
	../../Supplimentary

LIBRARIES := \
	pthread

DEPENDENCIES := \
	liburing \
	jemalloc \
	libcurl \
	zlib \
	libprotobuf-c

OBJECTS := \
	../../Ring/FastRing.o \
	../../Ring/Fetch.o \
	../../Supplimentary/gRPCClient.o \
	../../Supplimentary/ProtoBuf.o \
	gRPCTest.pb-c.o \
	gRPCTest.o

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

build: $(PREREQUISITES) $(OBJECTS)
	$(CC) $(OBJECTS) $(FLAGS) $(LIBS) -o $(EXECUTABLE)

clean:
	rm -f $(EXECUTABLE) $(OBJECTS) $(wildcard $(filter %.d,$(OBJECTS:.o=.d)))
	find . -name "*.pb-c.?" | xargs rm -f

-include $(wildcard $(filter %.d,$(OBJECTS:.o=.d)))
