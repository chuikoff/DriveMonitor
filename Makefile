# ============================================================================
#  DriveMonitor - MinGW build. GPL-2.0-or-later. See LICENSE.
# ============================================================================

ifeq ($(OS),Windows_NT)
    CC      = g++
    WINDRES = windres
else
    CC      = x86_64-w64-mingw32-g++
    WINDRES = x86_64-w64-mingw32-windres
endif

SRCDIR  = src
OBJDIR  = obj
OUTDIR  = bin
TARGET  = $(OUTDIR)/DriveMonitor.exe
SRCS    = $(SRCDIR)/main.cpp \
          $(SRCDIR)/mainwnd.cpp \
          $(SRCDIR)/smart.cpp \
          $(SRCDIR)/profile.cpp \
          $(SRCDIR)/drivedb.cpp \
          $(SRCDIR)/ata_smart.cpp \
          $(SRCDIR)/nvme_smart.cpp \
          $(SRCDIR)/usb_bridge.cpp \
          $(SRCDIR)/health_assessment.cpp \
          $(SRCDIR)/lang.cpp

OBJS    = $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(SRCS))
RES_O   = $(OBJDIR)/app_res.o

# Compiler flags:
#   -mwindows          : GUI subsystem (no console window)
#   -O2                : Optimized release build
#   -DWIN32 ...        : Win32 platform defines expected by the source
#   CXXFLAGS           : strict warnings (conversions, pedantic, extra)
#   V=1                : Show full compiler/linker commands (default is quiet).
CFLAGS  = -mwindows -O2 \
          -DWIN32 -D_WIN32 -D_WINDOWS -DNDEBUG \
          -I$(SRCDIR) \
          -finput-charset=UTF-8
CXXFLAGS += -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion

# Linker flags:
#   -static*           : Statically link the C/C++ runtime so the .exe
#                        has no external runtime DLL dependencies.
#   -lcomctl32 ...     : Win32 system libraries used by the UI and by
#                        the S.M.A.R.T. / SetupAPI code paths.
LDFLAGS = -mwindows \
          -static -static-libgcc -static-libstdc++ \
          -lcomctl32 -lcomdlg32 -lshell32 \
          -luser32 -lgdi32 -lkernel32 -ladvapi32 -lole32 -luuid \
          -lgdiplus -lshlwapi -lsetupapi -lcfgmgr32


.PHONY: all clean test

TESTDIR = tests
TEST_TARGET = $(OUTDIR)/profile_test.exe

all: $(OUTDIR) $(OBJDIR) $(TARGET)
	@echo ""
	@echo "  Build complete: $(TARGET)"
	@echo "  100% Free Open Source Software - have fun!"
	@echo ""

# V=1 prints full compiler/linker commands; default is quiet.
ifeq ($(V),1)
Q :=
else
Q := @
endif

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp $(SRCDIR)/resource.h | $(OBJDIR)
	@echo "  CC  $<"
	$(Q)$(CC) $(CFLAGS) $(CXXFLAGS) -c $< -o $@

# Resource compilation: the .rc file references the .manifest and .ico,
# so the .o depends on all three.
$(RES_O): $(SRCDIR)/app.rc $(SRCDIR)/app.manifest $(SRCDIR)/app.ico $(SRCDIR)/resource.h | $(OBJDIR)
	@echo "  RC  $(SRCDIR)/app.rc"
	$(Q)$(WINDRES) --include-dir=$(SRCDIR) $(SRCDIR)/app.rc -o $(RES_O)

$(TARGET): $(OBJS) $(RES_O) | $(OUTDIR)
	@echo "  LD  $@"
	$(Q)$(CC) $(OBJS) $(RES_O) $(LDFLAGS) -o $@

# Console resolver tests. profile.cpp only: drivedb and the GUI stay out.
# DriveDbApply and DriveDbLookup are stubs in profile_test.cpp.
test: $(OUTDIR) $(OBJDIR) $(TEST_TARGET)
	$(TEST_TARGET)

$(OBJDIR)/profile_test.o: $(TESTDIR)/profile_test.cpp $(SRCDIR)/smart.h | $(OBJDIR)
	@echo "  CC  $<"
	$(Q)$(CC) -O2 -DWIN32 -D_WIN32 -D_WINDOWS -DNDEBUG \
	    -I$(SRCDIR) -finput-charset=UTF-8 $(CXXFLAGS) -c $< -o $@

$(TEST_TARGET): $(OBJDIR)/profile_test.o $(OBJDIR)/profile.o
	@echo "  LD  $@"
	$(Q)$(CC) $(OBJDIR)/profile_test.o $(OBJDIR)/profile.o \
	    -static -static-libgcc -static-libstdc++ -lkernel32 -o $@

$(OBJDIR):
ifeq ($(OS),Windows_NT)
	@if not exist $(OBJDIR) mkdir $(OBJDIR)
else
	@mkdir -p $(OBJDIR)
endif

$(OUTDIR):
ifeq ($(OS),Windows_NT)
	@if not exist $(OUTDIR) mkdir $(OUTDIR)
else
	@mkdir -p $(OUTDIR)
endif

clean:
ifeq ($(OS),Windows_NT)
	@if exist $(OBJDIR) rmdir /s /q $(OBJDIR)
	@if exist $(OUTDIR) rmdir /s /q $(OUTDIR)
else
	@rm -rf $(OBJDIR) $(OUTDIR)
endif
	@echo "  Clean."
