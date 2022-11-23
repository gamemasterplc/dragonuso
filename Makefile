BUILD_DIR=build
SOURCE_DIR=src
include $(N64_INST)/include/n64.mk

#Host C++ compiler information
HOST_CXX := g++
HOST_CXXFLAGS := -Itools -O3 -s

#Tool binaries
ELF2USO := tools/elf2uso
MAKE_GLOBAL_SYMS := tools/make_global_syms
MAKE_USO_EXTERNS := tools/make_uso_externs

PROJECT_NAME := dragonuso

#Output files
MAIN_ELF := $(BUILD_DIR)/$(PROJECT_NAME).elf
OUT_DFS := $(BUILD_DIR)/$(PROJECT_NAME).dfs
FINAL_ROM := $(PROJECT_NAME).z64

#USO definitions
USO_EXTERNS := $(BUILD_DIR)/uso_externs.ld
USO_DIR := uso
GLOBAL_SYMS := $(USO_DIR)/global_syms.sym
USO_LIST :=
ALL_OBJECTS := 

all: $(FINAL_ROM)

#USO linking/building rules
$(USO_DIR)/%.uso: $(BUILD_DIR)/%.plf $(ELF2USO)
	@echo "    [USO] $@"
	$(ELF2USO) $< $@
	
#USO binary linking rules
%.plf:
	@echo "    [LD] $@"
	$(N64_LD) -Ur -Tuso.ld -Map=$(basename $@).map -o $@ $^
	$(N64_SIZE) -G $@
	
# USOs can't use GP register and are set up to hide symbols by default
%.uso: CFLAGS += -fvisibility=hidden -mno-gpopt
%.uso: CXXFLAGS += -fvisibility=hidden -mno-gpopt
# Change all the dependency chain of USOs to use the N64 toolchain
%.uso: CC=$(N64_CC)
%.uso: CXX=$(N64_CXX)
%.uso: AS=$(N64_AS)
%.uso: CFLAGS+=$(N64_CFLAGS)
%.uso: CXXFLAGS+=$(N64_CXXFLAGS)
%.uso: ASFLAGS+=$(N64_ASFLAGS)
%.uso: RSPASFLAGS+=$(N64_RSPASFLAGS)

#Module 1 sources and build instructions
SOURCES := module1.cpp counter.cpp
OBJECTS := $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(SOURCES))))
$(BUILD_DIR)/module1.plf: $(OBJECTS)
ALL_OBJECTS += $(OBJECTS)
USO_LIST += module1.uso

#Module 2 sources and build instructions
SOURCES := module2.cpp
OBJECTS := $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(SOURCES))))
$(BUILD_DIR)/module2.plf: $(OBJECTS)
ALL_OBJECTS += $(OBJECTS)
USO_LIST += module2.uso

#Main binary sources must be last
SOURCES := main.cpp uso.c
OBJECTS := $(addprefix $(BUILD_DIR)/, $(addsuffix .o, $(basename $(SOURCES))))
ALL_OBJECTS += $(OBJECTS)

#Create ist of USO files
ALL_USOS := $(addprefix $(USO_DIR)/, $(USO_LIST))

#DFS needs global symbols and USOs to build
$(OUT_DFS): $(ALL_USOS) $(GLOBAL_SYMS)
#Main ELF needs to know about symbols not satisfied by any USO
$(MAIN_ELF): $(OBJECTS) $(USO_EXTERNS)
#Final ROM Information
$(FINAL_ROM): N64_ROM_TITLE="RSPQ Demo"
$(FINAL_ROM): $(OUT_DFS)

#Global symbol rule
$(GLOBAL_SYMS): $(MAIN_ELF) $(MAKE_GLOBAL_SYMS)
	@echo "    [GLOBAL_SYMBOLS] $@"
	$(MAKE_GLOBAL_SYMS) $(MAIN_ELF) $(GLOBAL_SYMS)
	
#Rule for list of symbols not satisfied by any USO
$(USO_EXTERNS): $(ALL_USOS) $(MAKE_USO_EXTERNS)
	@echo "    [EXTERNS] $@"
	$(MAKE_USO_EXTERNS) $(USO_EXTERNS) $(ALL_USOS)
	
clean:
	rm -rf $(BUILD_DIR) $(ALL_USOS) $(GLOBAL_SYMS) $(FINAL_ROM) $(ELF2USO) $(MAKE_GLOBAL_SYMS) $(MAKE_USO_EXTERNS)

#Specify object dependencies
DEP_FILES += $(ALL_OBJECTS:.o=.d)
-include $(DEP_FILES)

#Tool recipes

$(ELF2USO): tools/elf2uso.cpp
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $^
	
$(MAKE_GLOBAL_SYMS): tools/make_global_syms.cpp
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $^
	
$(MAKE_USO_EXTERNS): tools/make_uso_externs.cpp
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $^
	
.PHONY: all clean
