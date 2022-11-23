#define _CRT_SECURE_NO_WARNINGS //Shut up Visual Studio
#include <iostream>
#include <vector>
#include <string>
#include <stdio.h>
#include <elfio/elfio.hpp>

typedef struct uso_symbol {
    uint32_t name_ofs; //Relative to first symbol in symbol table
    uint32_t addr;
    uint16_t section;
    uint16_t name_len; //Top bit used to tell if symbol is weak
} uso_symbol_t;

struct symbol_info {
    ELFIO::Elf_Word src_symbol;
    std::string name;
    uint16_t section;
    bool weak;
    ELFIO::Elf64_Addr addr;
};

std::vector<symbol_info> export_sym_list; //Global symbol table

//ELF info
ELFIO::elfio elf_reader;
ELFIO::Elf_Half elf_symbol_sec_index;

std::vector<std::string> unexported_symbols = {
    //Special c library symbols
    "__dso_handle",
    "_init",
    "_fini",
    //Symbols defined in n64.ld
    "__kseg0_start",
    "__libdragon_text_start",
    "__text_start", 
    "__text_end", 
    "__EH_FRAME_BEGIN__",
    "__CTOR_LIST__", 
    "__CTOR_END__", 
    "__data_start", 
    "_gp", 
    "__data_end", 
    "__bss_start", 
    "__bss_end", 
    "__rom_end", 
    "end",
};

ELFIO::Elf_Half elf_find_section(std::string name)
{
    for (ELFIO::Elf_Half i = 1; i < elf_reader.sections.size(); i++) {
        if (elf_reader.sections[i]->get_name() == name) {
            //Found section name in list of sections
            return i;
        }
    }
    //ELFIO::SHN_UNDEF is never a valid section index
    return ELFIO::SHN_UNDEF;
}

bool elf_valid()
{
    //Check if symbol table can be found
    elf_symbol_sec_index = elf_find_section(".symtab");
    if (elf_symbol_sec_index == ELFIO::SHN_UNDEF) {
        std::cout << "ELF file is missing symbol table." << std::endl;
        return false;
    }
    return elf_reader.get_class() == ELFIO::ELFCLASS32 //Check for 32-bit ELF
        && elf_reader.get_encoding() == ELFIO::ELFDATA2MSB //Check for Big-Endian Platform
        && elf_reader.get_machine() == ELFIO::EM_MIPS //Check for MIPS platform
        && elf_reader.get_type() == ELFIO::ET_EXEC; //Check for non-relocatable ELF
}

bool sym_is_exported(std::string name)
{
    for (size_t i = 0; i < unexported_symbols.size(); i++) {
        if (unexported_symbols[i] == name) {
            return false;
        }
    }
    return true;
}

bool sym_compare(symbol_info &first, symbol_info &second)
{
    //Compare names as strings
    return first.name < second.name;
}

void sym_collect()
{
    ELFIO::symbol_section_accessor sym_accessor(elf_reader, elf_reader.sections[elf_symbol_sec_index]);
    for (ELFIO::Elf_Xword i = 0; i < sym_accessor.get_symbols_num(); i++) {
        //Symbol temporaries
        std::string name;
        ELFIO::Elf64_Addr value;
        ELFIO::Elf_Xword size;
        unsigned char bind;
        unsigned char type;
        ELFIO::Elf_Half section_index;
        unsigned char other;
        sym_accessor.get_symbol(i, name, value, size, bind, type, section_index, other);
        //Skip local symbols
        if (bind == ELFIO::STB_LOCAL) {
            continue;
        }
        //Reject defined NULL symbols
        if (section_index != ELFIO::SHN_UNDEF && value == 0) {
            std::cout << "Symbol " << name << " has NULL address." << std::endl;
            exit(1);
        } else if(value == 0) {
			//Skip undefined NULL symbols
			continue;
		}
        //Reject symbol names longer than 32767 characters
        if (name.length() >= 32767) {
            std::cout << "Symbol ID " << i << " has too long of a name" << std::endl;
        }
        if (sym_is_exported(name)) {
            symbol_info symbol;
            //Populare symbol
            symbol.src_symbol = i;
            symbol.name = name;
            symbol.section = 0; //Global symbols always use section 0
            symbol.weak = false; //Global symbols are never weak
            symbol.addr = value;
            export_sym_list.push_back(symbol);
        }
    }
    //Sort symbols
    std::sort(export_sym_list.begin(), export_sym_list.end(), sym_compare);
}

bool need_swap()
{
    static const uint32_t value = 1;
    //Little endian has first byte equal to lowest 8 bits
    if (*(uint8_t *)(&value) == 1) {
        //Swap on little endian
        return true;
    }
    //Don't swap on big endian
    return false;
}

void swap_u16(uint16_t *ptr)
{
    if (need_swap()) {
        *ptr = (((*ptr >> 8) & 0xFF) << 0) | (((*ptr >> 0) & 0xFF) << 8);
    }
}

void swap_u32(uint32_t *ptr)
{
    if (need_swap()) {
        *ptr = (((*ptr >> 24) & 0xFF) << 0) | (((*ptr >> 16) & 0xFF) << 8)
            | (((*ptr >> 8) & 0xFF) << 16) | (((*ptr >> 0) & 0xFF) << 24);
    }
}

void uso_write_symbol_table_count(FILE *file, uint32_t ofs, uint32_t count)
{
    swap_u32(&count); //Convert count to big endian
    //Write count to offset
    fseek(file, ofs, SEEK_SET);
    fwrite(&count, 1, 4, file);
}

void uso_write_symbol_table(FILE *file, uint32_t ofs, std::vector<symbol_info> &syms)
{
    uint32_t name_ofs = 4 + (syms.size() * sizeof(uso_symbol_t));
    //Write symbol table count
    uso_write_symbol_table_count(file, ofs, syms.size());
    //Iterate over symbols
    for (size_t i = 0; i < syms.size(); i++) {
        //Setup symbol data
        uint16_t name_len = syms[i].name.length();
        uso_symbol_t temp_sym;
        temp_sym.name_ofs = name_ofs;
        temp_sym.addr = syms[i].addr;
        temp_sym.section = syms[i].section;
        //Set name length depending on weak flag
        if (syms[i].weak) {
            temp_sym.name_len = name_len | 0x8000;
        } else {
            temp_sym.name_len = name_len;
        }
        //byteswap symbol data
        swap_u32(&temp_sym.name_ofs);
        swap_u32(&temp_sym.addr);
        swap_u16(&temp_sym.section);
        swap_u16(&temp_sym.name_len);
        //Write symbol data
        fseek(file, 4 + ofs + (i * sizeof(uso_symbol_t)), SEEK_SET);
        fwrite(&temp_sym, sizeof(uso_symbol_t), 1, file);
        //Write symbol name with NULL terminator
        fseek(file, ofs + name_ofs, SEEK_SET);
        fwrite(syms[i].name.c_str(), 1, name_len + 1, file);
        name_ofs += name_len + 1; //Calculate next name offset
    }
}

bool global_sym_write(const char *path)
{
    //Try to open file
    FILE *file = fopen(path, "wb");
    if (!file) {
        std::cout << "Failed to open " << path << " for writing." << std::endl;
        return false;
    }
    //Write symbol table
    uso_write_symbol_table(file, 0, export_sym_list);
    fclose(file);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        //Write usage
        std::cout << "Usage: " << argv[0] << " elf_input syms_output" << std::endl;
        std::cout << "elf_input is a non-relocatable N64 ELF file." << std::endl;
        std::cout << "The global symbols from elf_input will be written to syms_output." << std::endl;
        return 1;
    }
    //Try to load ELF
    if (!elf_reader.load(argv[1])) {
        std::cout << "Failed to read input ELF file." << std::endl;
        return 1;
    }
    //Do elf sanity checks
    if (!elf_valid()) {
        std::cout << "Input ELF file is not valid non-relocatable N64 ELF file." << std::endl;
        return 1;
    }
    sym_collect();
    //Write global symbols and return status
    if (!global_sym_write(argv[2])) {
        return 1;
    }
    return 0;
}