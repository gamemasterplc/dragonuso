#define _CRT_SECURE_NO_WARNINGS //Shut up Visual Studio
#include <stdio.h>
#include <iostream>
#include <vector>
#include <algorithm>

typedef struct uso_symbol {
    uint32_t name_ofs; //Relative to first symbol in symbol table
    uint32_t addr;
    uint16_t section;
    uint16_t name_len; //Top bit used to tell if symbol is weak
} uso_symbol_t;

typedef struct uso_header {
    uint16_t num_sections;
    uint16_t eh_frame_section;
    uint32_t sections_ofs;
    uint32_t import_sym_table_ofs;
    uint32_t export_sym_table_ofs;
    uint16_t ctors_section;
    uint16_t dtors_section;
} uso_header_t;

struct uso_symbol_info {
    std::string name;
    uint32_t addr;
    uint16_t section;
    bool weak;
};

struct uso_info {
    std::vector<uso_symbol_info> import_syms;
    std::vector<uso_symbol_info> export_syms;
};

std::vector<uso_info> uso_list;
std::vector<std::string> uso_extern_list;

bool file_read(FILE *file, uint32_t ofs, void *dst, uint32_t size)
{
    return fseek(file, ofs, SEEK_SET) == 0 && fread(dst, size, 1, file) != 0;
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

void uso_read_header(FILE *file, uso_header_t &header)
{
    //Try to read USO symbol
    if (!file_read(file, 0, &header, sizeof(uso_header_t))) {
        std::cerr << "Failed to read USO header." << std::endl;
        fclose(file);
        exit(1);
    }
    //Byteswap header
    swap_u16(&header.num_sections);
    swap_u16(&header.eh_frame_section);
    swap_u32(&header.sections_ofs);
    swap_u32(&header.import_sym_table_ofs);
    swap_u32(&header.export_sym_table_ofs);
    swap_u16(&header.ctors_section);
    swap_u16(&header.dtors_section);
}

void uso_read_symbol(FILE *file, uint32_t ofs, uso_symbol_t &symbol)
{
    if (!file_read(file, ofs, &symbol, sizeof(uso_symbol_t))) {
        std::cerr << "Failed to read USO symbol." << std::endl;
        fclose(file);
        exit(1);
    }
    //Byteswap symbol
    swap_u32(&symbol.name_ofs);
    swap_u32(&symbol.addr);
    swap_u16(&symbol.section);
    swap_u16(&symbol.name_len);
}

void uso_read_symbol_name(FILE *file, uint32_t ofs, uint32_t len, std::string &name)
{
    char temp_buf[32768];
    //Read name from file to temporary buffer
    if (!file_read(file, ofs, temp_buf, len)) {
        std::cerr << "Failed to read symbol name." << std::endl;
        fclose(file);
        exit(1);
    }
    temp_buf[len] = 0; //Add terminator to name
    name = temp_buf; //Copy buffer to std::string
}

uint32_t uso_get_sym_table_count(FILE *file, uint32_t sym_table_ofs)
{
    uint32_t size;
    if (!file_read(file, sym_table_ofs, &size, 4)) {
        std::cerr << "Failed to read symbol table size." << std::endl;
        fclose(file);
        exit(1);
    }
    swap_u32(&size);
    return size;
}

void uso_read_symbol_table(FILE *file, uint32_t ofs, std::vector<uso_symbol_info> &list)
{
	//Skip reading 0-offset symbol tables
	if(ofs == 0) {
		return;
	}
    uint32_t num_symbols = uso_get_sym_table_count(file, ofs);
    for (uint32_t i = 0; i < num_symbols; i++) {
        uso_symbol_info sym_info;
        uso_symbol_t symbol;
        uso_read_symbol(file, 4 + ofs + (i * sizeof(uso_symbol_t)), symbol);
        sym_info.addr = symbol.addr;
        sym_info.section = symbol.section;
        //Determine weakness of symbol
        if (symbol.name_len & 0x8000) {
            sym_info.weak = true;
        } else {
            sym_info.weak = false;
        }
        uso_read_symbol_name(file, ofs + symbol.name_ofs, symbol.name_len & 0x7FFF, sym_info.name);
        list.push_back(sym_info);
    }
}

bool uso_read(char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        std::cerr << "Failed to open " << path << " for reading." << std::endl;
        return false;
    }
    uso_header_t header;
    uso_info tmp_uso_info;
    uso_read_header(file, header); //Must be first so offsets can be accurate
    uso_read_symbol_table(file, header.import_sym_table_ofs, tmp_uso_info.import_syms);
    uso_read_symbol_table(file, header.export_sym_table_ofs, tmp_uso_info.export_syms);
    uso_list.push_back(tmp_uso_info);
    fclose(file); //Close file
    return true;
}

bool uso_sym_compare(const uso_symbol_info &first, const uso_symbol_info &second)
{
    //Compare names as strings
    return first.name < second.name;
}

bool uso_sym_is_extern(size_t uso_id, std::string name)
{
    uso_symbol_info symbol = { name, 0, 0, false };
    for (size_t i = 0; i < uso_list.size(); i++) {
        //Skip this USO
        if (i == uso_id) {
            continue;
        }
        //Search for symbol
        std::vector<uso_symbol_info>::iterator begin = uso_list[i].export_syms.begin();
        std::vector<uso_symbol_info>::iterator end = uso_list[i].export_syms.end();
        if (std::binary_search(begin, end, symbol, uso_sym_compare)) {
            //Symbol is not extern if found in any export symbol table
            return false;
        }
    }
    //Symbol is extern if defined in no export symbol table
    return true;
}

void generate_uso_extern_list()
{
    //Check all USOs
    for (size_t i = 0; i < uso_list.size(); i++) {
        //Check import symbols in each USO
        std::vector<uso_symbol_info> &import_sym_ref = uso_list[i].import_syms;
        for (size_t j = 0; j < import_sym_ref.size(); j++) {
            //Add extern symbols to extern list
            if (uso_sym_is_extern(i, import_sym_ref[j].name)) {
                uso_extern_list.push_back(import_sym_ref[j].name);
            }
        }
    }
}

bool write_uso_extern_list(char *path)
{
    //Try to open output file
    FILE *file = fopen(path, "w");
    if (!file) {
        std::cerr << "Failed to open " << path << " for writing." << std::endl;
        return false;
    }
    //Print LD externs for every symbol
    for (size_t i = 0; i < uso_extern_list.size(); i++) {
        fprintf(file, "EXTERN(%s)\n", uso_extern_list[i].c_str());
    }
    fclose(file); //Close file
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " output uso_list" << std::endl;
        std::cout << "output is the destination of the result." << std::endl;
        std::cout << "uso_list is a possibly empty space separated list of files." << std::endl;
        return 1;
    }
    //Read in USOs passed in on command line
    for (int i = 2; i < argc; i++) {
        if (!uso_read(argv[i])) {
            return 1;
        }
    }
    //Generate extern list
    generate_uso_extern_list();
    //Write extern list
    if (!write_uso_extern_list(argv[1])) {
        return 1;
    }
    return 0;
}