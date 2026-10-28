LOCAL_TVISION_PATH ?= ../../../tvision

EXECUTABLE := turbotest

DIRECTORIES := \
	../../Ring \
	$(LOCAL_TVISION_PATH)/include

LIBRARIES := \
	pthread

DEPENDENCIES := \
	liburing \
	ncursesw

OBJECTS := \
	../../Ring/FastRing.o \
	../../Ring/TurboRing.o \
	TurboTest.o \
	$(LOCAL_TVISION_PATH)/build/libtvision.a

FLAGS += \
	-Wno-unused-result -Wno-format-truncation -Wno-format-overflow -Wno-stringop-overflow \
	-rdynamic -fno-omit-frame-pointer -O2 -MMD -gdwarf \
	$(foreach directory, $(DIRECTORIES), -I$(directory)) \
	$(shell pkg-config --cflags $(DEPENDENCIES))

CFLAGS   += $(FLAGS)
CXXFLAGS += $(FLAGS) -std=gnu++17

# Internal headers of Turbo Vision include each other from there, only TurboRing needs them
../../Ring/TurboRing.o: CXXFLAGS += -I$(LOCAL_TVISION_PATH)/include/tvision

LIBS := \
	$(foreach library, $(LIBRARIES), -l$(library)) \
	$(shell pkg-config --libs $(DEPENDENCIES))

# Libraries Turbo Vision has found on its own, known once it is configured: libgpm for the mouse of the Linux console
EXTRA = $(if $(shell grep -s '^GPM:FILEPATH=/' $(LOCAL_TVISION_PATH)/build/CMakeCache.txt),-lgpm)

all: build

$(LOCAL_TVISION_PATH)/build/libtvision.a:
	cmake -S $(LOCAL_TVISION_PATH) -B $(LOCAL_TVISION_PATH)/build -D CMAKE_BUILD_TYPE=Release -D CMAKE_POSITION_INDEPENDENT_CODE=ON -D TV_BUILD_EXAMPLES=OFF
	+make -C $(LOCAL_TVISION_PATH)/build tvision

build: $(PREREQUISITES) $(OBJECTS)
	$(CXX) $(OBJECTS) $(FLAGS) $(LIBS) $(EXTRA) -o $(EXECUTABLE)

clean:
	rm -f $(EXECUTABLE) $(filter %.o,$(OBJECTS)) $(wildcard $(filter %.d,$(OBJECTS:.o=.d)))

-include $(wildcard $(filter %.d,$(OBJECTS:.o=.d)))
