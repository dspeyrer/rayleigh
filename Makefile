EXE = build/rayleigh
IMGUI_DIR = ./imgui
SOURCES = main.cpp
SOURCES += $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp $(IMGUI_DIR)/imgui_widgets.cpp
SOURCES += $(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp $(IMGUI_DIR)/backends/imgui_impl_sdlrenderer3.cpp
OBJS = $(addprefix build/, $(addsuffix .o, $(basename $(notdir $(SOURCES)))))
UNAME_S := $(shell uname -s)
LINUX_GL_LIBS = -lGL

CXXFLAGS = -std=c++17 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends
CXXFLAGS += -O3 -g -Wall -Wformat
LIBS =

##---------------------------------------------------------------------
## BUILD FLAGS PER PLATFORM
##---------------------------------------------------------------------

ifeq ($(UNAME_S), Linux)
	ECHO_MESSAGE = "Linux"
	LIBS += -ldl `pkg-config sdl3 --libs`

	CXXFLAGS += `pkg-config sdl3 --cflags`
endif

ifeq ($(UNAME_S), Darwin)
	ECHO_MESSAGE = "Mac OS X"
	LIBS += -framework Cocoa -framework IOKit -framework CoreVideo
	LIBS += `pkg-config --libs sdl3`
	LIBS += -L/usr/local/lib

	CXXFLAGS += `pkg-config --cflags sdl3`
	CXXFLAGS += -I/usr/local/include -I/opt/local/include
endif

ifeq ($(OS), Windows_NT)
	ECHO_MESSAGE = "MinGW"
	LIBS += -lgdi32 -limm32 `pkg-config --static --libs sdl3`

	CXXFLAGS += `pkg-config --cflags sdl3`
endif

##---------------------------------------------------------------------
## BUILD RULES
##---------------------------------------------------------------------

build/%.o: %.cpp | build
	$(info CXX $<)
	@$(CXX) $(CXXFLAGS) -c -o $@ $<

build/%.o: $(IMGUI_DIR)/%.cpp | build
	$(info CXX $<)
	@$(CXX) $(CXXFLAGS) -c -o $@ $<

build/%.o: $(IMGUI_DIR)/backends/%.cpp | build
	$(info CXX $<)
	@$(CXX) $(CXXFLAGS) -c -o $@ $<

all: $(EXE)

run: $(EXE)
	@$(EXE)

build:
	mkdir build

$(EXE): $(OBJS) | build
	$(info CXX EXE)
	@$(CXX) -o $@ $^ $(CXXFLAGS) $(LIBS)

clean:
	rm -rf build