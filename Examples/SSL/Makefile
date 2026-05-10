EXECUTABLE := ssltest

DIRECTORIES := \
	../../Ring

LIBRARIES := \
	pthread

DEPENDENCIES := \
	liburing \
	jemalloc \
	openssl \
	libcares

OBJECTS := \
	../../Ring/FastRing.o \
	../../Ring/FastBuffer.o \
	../../Ring/FastBIO.o \
	../../Ring/Resolver.o \
	../../Ring/SSLSocket.o \
	SSLTest.o

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

build: $(PREREQUISITES) $(OBJECTS)
	$(CC) $(OBJECTS) $(FLAGS) $(LIBS) -o $(EXECUTABLE)

clean:
	rm -f $(EXECUTABLE) $(OBJECTS) $(wildcard $(filter %.d,$(OBJECTS:.o=.d)))

-include $(wildcard $(filter %.d,$(OBJECTS:.o=.d)))
