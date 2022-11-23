#define _CRT_SECURE_NO_WARNINGS //Shut up Visual Studio
#include <stdio.h>
#include <string>
#include <iostream>
#include <map>
#include <algorithm>
#include <vector>
#include <elfio/elfio.hpp>

//USO structure definitons

typedef struct uso_load_info {
    uint32_t uso_size;
    uint32_t uso_align;
    uint32_t noload_size;
    uint32_t noload_align;
} uso_load_info_t;

typedef struct uso_header {
    uint16_t num_sections;
    uint16_t eh_frame_section;
    uint32_t sections_ofs;
    uint32_t import_sym_table_ofs;
    uint32_t export_sym_table_ofs;
    uint16_t ctors_section;
    uint16_t dtors_section;
} uso_header_t;

typedef struct uso_section_info {
    uint32_t data_ofs;
    uint32_t data_size;
    uint32_t data_align;
    uint32_t internal_relocs_ofs;
    uint32_t external_relocs_ofs;
} uso_section_info_t;

typedef struct uso_symbol {
    uint32_t name_ofs; //Relative to first symbol in symbol table
    uint32_t addr;
    uint16_t section;
    uint16_t name_len; //Top bit used to tell if symbol is weak
} uso_symbol_t;

typedef struct uso_reloc {
    uint32_t offset;
    uint32_t info; //Upper 6 bits are relocation type, lower 26 bits are either symbol or section index
    uint32_t sym_offset; //Section-relative symbol offset, zero for external relocations
} uso_reloc_t;

struct section_info {
    ELFIO::Elf_Half reloc_elf_section;
    std::vector<uso_reloc_t> internal_relocs;
    std::vector<uso_reloc_t> external_relocs;
    const char *data;
    size_t size;
    size_t align;
};

struct symbol_info {
    ELFIO::Elf_Word src_symbol;
    std::string name;
    uint16_t section;
    bool weak;
    ELFIO::Elf64_Addr addr;
};

//Section map info
std::map<ELFIO::Elf_Half, uint16_t> out_section_map;
std::vector<section_info> out_sections;

//Symbol tables
std::vector<symbol_info> import_syms;
std::vector<symbol_info> export_syms;
std::map<ELFIO::Elf_Word, size_t> import_sym_map;

//ELF info
ELFIO::elfio elf_reader;
ELFIO::Elf_Half elf_symbol_sec_index;

//These symbols must not be undefined and used in the ELF
std::vector<std::string> prohibited_import_symbols = {
    //Special c library symbols
    "__dso_handle",
    "_init",
    "_fini",
    //Special functions for shared object start/end
    "_prolog",
    "_epilog"
};

//These symbols ignore visibility attributes
std::vector<std::string> hideable_symbols = {
    "__dso_handle",
    //Special functions for shared object start/end
    "_prolog",
    "_epilog"
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
        std::cerr << "ELF file is missing symbol table." << std::endl;
        return false;
    }
    return elf_reader.get_class() == ELFIO::ELFCLASS32 //Check for 32-bit ELF
        && elf_reader.get_encoding() == ELFIO::ELFDATA2MSB //Check for Big-Endian Platform
        && elf_reader.get_machine() == ELFIO::EM_MIPS //Check for MIPS platform
        && elf_reader.get_type() == ELFIO::ET_REL; //Check for relocatable ELF
}

bool elf_has_global_constructors()
{
	ELFIO::Elf_Half ctor_section = elf_find_section(".ctors");
	//ELF has constructors if .ctors section exists and has non-zero size
	if(ctor_section != ELFIO::SHN_UNDEF && elf_reader.sections[ctor_section]->get_size() > 0) {
		return true;
	}
	return false;
}

bool sym_compare(symbol_info &first, symbol_info &second)
{
    //Compare names as strings
    return first.name < second.name;
}

void sym_sort()
{
    //Sort import and export symbol tables
    std::sort(import_syms.begin(), import_syms.end(), sym_compare);
    std::sort(export_syms.begin(), export_syms.end(), sym_compare);
}

bool sym_is_prohibited_import(std::string name)
{
    return std::find(prohibited_import_symbols.begin(), prohibited_import_symbols.end(), name) != prohibited_import_symbols.end();
}

bool sym_is_hideable(std::string name)
{
    return std::find(hideable_symbols.begin(), hideable_symbols.end(), name) != hideable_symbols.end();
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
        //Reject symbol names longer than 32767 characters
        if (name.length() >= 32767) {
            std::cerr << "Symbol ID " << i << " has too long of a name" << std::endl;
            exit(1);
        }
        if (section_index == ELFIO::SHN_UNDEF) {
            if (sym_is_prohibited_import(name)) {
                std::cerr << "Disallowed import symbol " << name << "." << std::endl;
                exit(1);
            }
            symbol_info symbol;
            //Populate import symbol
            symbol.src_symbol = i;
            symbol.name = name;
            symbol.section = 0; //Import symbols have no section
            symbol.weak = type == ELFIO::STB_WEAK; //Set weak flag
            symbol.addr = value;
            import_syms.push_back(symbol); //Add import symbol
        } else {
            //Only add symbols with default visibility for export but also include always exported symbols
            if (sym_is_hideable(name) || other == ELFIO::STV_DEFAULT) {
                symbol_info symbol;
                //Populate export symbol
                symbol.src_symbol = i;
                symbol.name = name;
                if (out_section_map.find(section_index) == out_section_map.end()) {
                    symbol.section = 0; //Fallback to section 0 if section index cannot be found
                    //Check for absolute symbol that will point to NULL
                    if (value == 0) {
                        std::cerr << "NULL absolute symbols disallowed." << std::endl;
                        exit(1);
                    }
                } else {
                    symbol.section = out_section_map[section_index]; //Lookup section index in map
                }

                symbol.weak = false; //Export symbols are never weak
                symbol.addr = value;
                export_syms.push_back(symbol);
            }
        }
    }
    if (export_syms.size() == 0 && !elf_has_global_constructors()) {
        //Warn about no external symbols for ELF
        std::cerr << "No exported symbols or global constructors in input ELF." << std::endl;
        std::cerr << "Exported symbols are defined, are non-local, and have default visibility." << std::endl;
        std::cerr << "Certain symbols can have non-default visibility as well." << std::endl;
        std::cerr << "These symbols are known as hideable symbols." << std::endl;
        std::cerr << "Hideable Symbols: ";
        for (size_t i = 0; i < hideable_symbols.size(); i++) {
            std::cerr << hideable_symbols[i];
            std::cerr << " ";
        }
        std::cerr << std::endl;
    }
    //Sort symbols here for correct import symbol to elf symbol mapping and runtime optimizations
    sym_sort();
    //Generate mapping for import symbols
    for (size_t i = 0; i < import_syms.size(); i++) {
        import_sym_map[import_syms[i].src_symbol] = i;
    }
}

uint32_t sym_get_data_size(std::vector<symbol_info> &syms)
{
    uint32_t size = 4+(sizeof(uso_symbol_t) * syms.size()); //Import symbol table size
    //Add sum of name sizes (length+1) to import symbol table size
    for (size_t i = 0; i < syms.size(); i++) {
        size += syms[i].name.length() + 1;
    }
    return size;
}

void section_collect()
{
    section_info section_data;
    //Push absolute fallback section
    section_data.reloc_elf_section = ELFIO::SHN_UNDEF;
    section_data.data = NULL;
    section_data.size = 0;
    section_data.align = 0;
    out_section_map[ELFIO::SHN_UNDEF] = 0;
    out_sections.push_back(section_data);
    //Iterate through non-NULL sections 
    for (ELFIO::Elf_Half i = 1; i < elf_reader.sections.size(); i++) {
        ELFIO::Elf_Xword flags = elf_reader.sections[i]->get_flags();
        if (flags & ELFIO::SHF_ALLOC) {
            ELFIO::Elf64_Word type = elf_reader.sections[i]->get_type();
            //Add section data info
            section_data.size = elf_reader.sections[i]->get_size();
            section_data.align = elf_reader.sections[i]->get_addr_align();
            if (type == ELFIO::SHT_NOBITS) {
                //SHT_NOBITS sections have no relocation data or data
                section_data.reloc_elf_section = ELFIO::SHN_UNDEF;
                section_data.data = NULL;
            } else {
                //Relocation section name is .rel#name for a section with a name of name if one exists
                std::string reloc_sec_name = ".rel" + elf_reader.sections[i]->get_name();
                section_data.reloc_elf_section = elf_find_section(reloc_sec_name);
                section_data.data = elf_reader.sections[i]->get_data();
            }
            //Add section
            out_section_map[i] = out_sections.size();
            out_sections.push_back(section_data);
        }
        
    }
}

void reloc_build()
{
    //Loop through output sections with attached relocation sections
    for (size_t i = 0; i < out_sections.size(); i++) {
        if (out_sections[i].reloc_elf_section != ELFIO::SHN_UNDEF) {
            ELFIO::relocation_section_accessor reloc_accessor(elf_reader, elf_reader.sections[out_sections[i].reloc_elf_section]);
            for (ELFIO::Elf_Xword j = 0; j < reloc_accessor.get_entries_num(); j++) {
                uso_reloc_t reloc_tmp;
                //Temporaries for relocation
                ELFIO::Elf64_Addr offset;
                ELFIO::Elf_Word symbol;
                unsigned int type;
                ELFIO::Elf_Sxword addend;
                reloc_accessor.get_entry(j, offset, symbol, type, addend);
                //Write known fields
                reloc_tmp.offset = offset;
                reloc_tmp.info = (type << 26);
                {
                    //Read symbol relocation is accessing
                    ELFIO::symbol_section_accessor sym_accessor(elf_reader, elf_reader.sections[elf_symbol_sec_index]);
                    //Temporaries for symbol lookup
                    std::string sym_name;
                    ELFIO::Elf64_Addr sym_value;
                    ELFIO::Elf_Xword sym_size;
                    unsigned char sym_bind;
                    unsigned char sym_type;
                    ELFIO::Elf_Half sym_section;
                    unsigned char sym_other;
                    sym_accessor.get_symbol(symbol, sym_name, sym_value, sym_size, sym_bind, sym_type, sym_section, sym_other);
                    if (sym_section == ELFIO::SHN_UNDEF) {
                        //Relocation references undefined symbol
                        reloc_tmp.info |= import_sym_map[symbol] & 0x3FFFFFF; //Write import symbol ID
                        reloc_tmp.sym_offset = 0; //Assume 0 symbol offset for these symbols
                        out_sections[i].external_relocs.push_back(reloc_tmp); //Write external relocation
                    } else {
                        reloc_tmp.info |= out_section_map[sym_section] & 0x3FFFFFF; //Write section ID
                        reloc_tmp.sym_offset = sym_value; //Use section-relative address as symbol offset
                        out_sections[i].internal_relocs.push_back(reloc_tmp); //Write internal relocation
                    }
                }
            }
        }
    }
}

bool common_is_used()
{
    //Iterate over ELF symbols
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
        if (section_index == ELFIO::SHN_COMMON) {
            //Terminate if any symbol is found using section SHN_COMMON
            return true;
        }
    }
    //Common section is not used
    return false;
}

bool check_gp_relative_relocations()
{
    //Iterate over all sections
    for (ELFIO::Elf_Half i = 0; i < elf_reader.sections.size(); i++) {
        if (elf_reader.sections[i]->get_type() == ELFIO::SHT_REL) {
            ELFIO::relocation_section_accessor reloc_accessor(elf_reader, elf_reader.sections[i]);
            for (ELFIO::Elf_Xword j = 0; j < reloc_accessor.get_entries_num(); j++) {
                //Temporaries for relocation
                ELFIO::Elf64_Addr offset;
                ELFIO::Elf_Word symbol;
                unsigned int type;
                ELFIO::Elf_Sxword addend;
                reloc_accessor.get_entry(j, offset, symbol, type, addend);
                //Check for relocation types using GP register
                //R_MIPS_GPREL16 (7), R_MIPS_GOT16 (9), R_MIPS_CALL16 (11), R_MIPS_CALL_HI16 (30), or R_MIPS_CALL_LO16 (31)
                if (type == 7 || type == 9 || type == 11 || type == 30 || type == 31) {
                    return true;
                }
            }
        }
    }
    //Did not find any relocations using GP
    return false;
}

uint32_t align_val(uint32_t val, uint32_t to)
{
    //Only supports power of 2 alignment
    return (val + to - 1) & ~(to - 1);
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

uint32_t uso_get_align()
{
    uint32_t align = 4; //4 is minimum alignment of several USO data structures
    //Find maximum alignment of section with loaded data
    for (size_t i = 0; i < out_sections.size(); i++) {
        //Only sections with loaded data are considered for USO minimum alignment calculations
        if (out_sections[i].data && out_sections[i].align > align) {
            align = out_sections[i].align;
        }
    }
    return align;
}

uint32_t uso_get_noload_align()
{
    uint32_t align = 1; //1 byte is global minimum alignment
    //Find maximum alignment of section with no loaded data
    for (size_t i = 0; i < out_sections.size(); i++) {
        //Only consider sections without loaded data
        if (!out_sections[i].data && out_sections[i].align > align) {
            align = out_sections[i].align;
        }
    }
    return align;
}

uint32_t uso_get_noload_size()
{
    uint32_t size = 0;
    //Sum up sizes of sections without loaded data
    for (size_t i = 0; i < out_sections.size(); i++) {
        if (!out_sections[i].data) {
            size += out_sections[i].size;
        }
    }
    return size;
}

uint32_t uso_calc_data_start_alignment()
{
    for (size_t i = 0; i < out_sections.size(); i++) {
        if (out_sections[i].data) {
            //Return alignment of first section with allocated data
            return out_sections[i].align;
        }
    }
    //Assume 1 if no loaded data sections are provided
    return 1;
}

uint32_t uso_get_reloc_ofs(uint32_t data_ofs)
{
    //Find end of last data section
    for (size_t i = 0; i < out_sections.size(); i++) {
        if (out_sections[i].data) {
            data_ofs = align_val(data_ofs, out_sections[i].align); //Align next data section offset
            data_ofs += out_sections[i].size; //Go to next data section offset
        }
    }
    return data_ofs;
}

uint32_t uso_get_reloc_table_size(std::vector<uso_reloc_t> &relocs)
{
    return 4 + (relocs.size() * sizeof(uso_reloc_t));
}

void uso_write_elf_name(FILE *file, uint32_t ofs, std::string name)
{
    fseek(file, ofs, SEEK_SET);
    fwrite(name.c_str(), 1, name.length() + 1, file); //Write with NULL terminator
}

void uso_write_u32(FILE *file, uint32_t ofs, uint32_t value)
{
    swap_u32(&value); //Convert count to big endian
    //Write count to offset
    fseek(file, ofs, SEEK_SET);
    fwrite(&value, 1, 4, file);
}

void uso_write_symbol_table(FILE *file, uint32_t ofs, std::vector<symbol_info> &syms)
{
    uint32_t name_ofs = 4 + (syms.size() * sizeof(uso_symbol_t));
    //Write symbol table count
    uso_write_u32(file, ofs, syms.size());
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

void uso_write_relocations(FILE *file, uint32_t ofs, std::vector<uso_reloc_t> &relocs)
{
    uso_write_u32(file, ofs, relocs.size());
    fseek(file, ofs+4, SEEK_SET);
    for (size_t i = 0; i < relocs.size(); i++) {
        uso_reloc_t temp_reloc;
        temp_reloc = relocs[i]; //Copy relocation
        //Byteswap relocation
        swap_u32(&temp_reloc.offset);
        swap_u32(&temp_reloc.info);
        swap_u32(&temp_reloc.sym_offset);
        //Write relocation
        fwrite(&temp_reloc, sizeof(uso_reloc_t), 1, file);
    }
}

void uso_write_sections(FILE *file, uint32_t sections_ofs)
{
    //Calculate offsets
    uint32_t data_ofs = sections_ofs + (out_sections.size() * sizeof(uso_section_info_t));
    data_ofs = align_val(data_ofs, uso_calc_data_start_alignment());
    uint32_t relocs_ofs = align_val(uso_get_reloc_ofs(data_ofs), 4);
    for (size_t i = 0; i < out_sections.size(); i++) {
        //Setup section info
        uso_section_info_t section;
        //Setup section data
        section.data_size = out_sections[i].size;
        section.data_align = out_sections[i].align;
        if (out_sections[i].data) {
            //Calculate properly aligned section offset
            data_ofs = align_val(data_ofs, section.data_align);
            section.data_ofs = data_ofs-sections_ofs;
            //Write section data
            fseek(file, data_ofs, SEEK_SET);
            fwrite(out_sections[i].data, 1, section.data_size, file);
            data_ofs += section.data_size; //Calculate next section data offset
        } else {
            section.data_ofs = 0; //Will be treated as NULL at runtime
        }
        //Setup internal relocations
        section.internal_relocs_ofs = 0;
        if (out_sections[i].internal_relocs.size() > 0) {
            section.internal_relocs_ofs = relocs_ofs-sections_ofs;
            uso_write_relocations(file, relocs_ofs, out_sections[i].internal_relocs);
            relocs_ofs += uso_get_reloc_table_size(out_sections[i].internal_relocs);
        }
        //Setup external relocations
        section.external_relocs_ofs = 0;
        if (out_sections[i].external_relocs.size() > 0) {
            section.external_relocs_ofs = relocs_ofs-sections_ofs;
            uso_write_relocations(file, relocs_ofs, out_sections[i].external_relocs);
            relocs_ofs += uso_get_reloc_table_size(out_sections[i].external_relocs);
        }
        //Byteswap section info
        swap_u32(&section.data_ofs);
        swap_u32(&section.data_size);
        swap_u32(&section.data_align);
        swap_u32(&section.internal_relocs_ofs);
        swap_u32(&section.external_relocs_ofs);
        //Write section info to file
        fseek(file, sections_ofs + (i * sizeof(uso_section_info_t)), SEEK_SET);
        fwrite(&section, sizeof(uso_section_info_t), 1, file);
    }
}

void uso_write_load_info(FILE *file)
{
    uso_load_info load_info;
    //Write USO load info at end of file
    fseek(file, 0, SEEK_END);
    //Write padding zero to align to 2 bytes
    if (ftell(file) % 2 != 0) {
        uint8_t zero = 0;
        fwrite(&zero, 1, 1, file);
    }
    load_info.uso_size = ftell(file); //File size
    load_info.uso_align = uso_get_align();
    load_info.noload_size = uso_get_noload_size();
    load_info.noload_align = uso_get_noload_align();
    swap_u32(&load_info.uso_size);
    swap_u32(&load_info.uso_align);
    swap_u32(&load_info.noload_size);
    swap_u32(&load_info.noload_align);
    fwrite(&load_info, sizeof(uso_load_info), 1, file);
}

void uso_write_header(FILE *file, uso_header_t header)
{
    //Swap all fields of header
    swap_u16(&header.num_sections);
    swap_u16(&header.eh_frame_section);
    swap_u32(&header.sections_ofs);
    swap_u32(&header.import_sym_table_ofs);
    swap_u32(&header.export_sym_table_ofs);
    swap_u16(&header.ctors_section);
    swap_u16(&header.dtors_section);
    //Write header at start of file
    fseek(file, 0, SEEK_SET);
    fwrite(&header, sizeof(uso_header_t), 1, file);
}

bool uso_write(std::string src_elf_name, char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        std::cerr << "Failed to open " << path << " for writing." << std::endl;
        return false;
    }
    uso_header_t header;
    //Write ELF name
    uint32_t data_ofs = sizeof(uso_header_t);
    uso_write_elf_name(file, data_ofs, src_elf_name);
    //Write import symbols
    data_ofs += src_elf_name.size() + 1;
    header.import_sym_table_ofs = 0;
    if (import_syms.size() != 0) {
        data_ofs = align_val(data_ofs, 4);
        header.import_sym_table_ofs = data_ofs;
        uso_write_symbol_table(file, header.import_sym_table_ofs, import_syms);
        data_ofs += sym_get_data_size(import_syms);
    } 
    //Write export symbols
    header.export_sym_table_ofs = 0;
    if (export_syms.size() != 0) {
        data_ofs = align_val(data_ofs, 4);
        header.export_sym_table_ofs = data_ofs;
        uso_write_symbol_table(file, header.export_sym_table_ofs, export_syms);
        data_ofs += sym_get_data_size(export_syms);
    }
    //Write section info
    data_ofs = align_val(data_ofs, 4);
    header.sections_ofs = data_ofs;
    header.num_sections = out_sections.size();
    uso_write_sections(file, header.sections_ofs);
    //Calculate section IDs of a few critical sections
    header.eh_frame_section = out_section_map[elf_find_section(".eh_frame")];
    header.ctors_section = out_section_map[elf_find_section(".ctors")];
    header.dtors_section = out_section_map[elf_find_section(".dtors")];
    //Rewrite some critical fields
    uso_write_header(file, header);
    uso_write_load_info(file);
    //Finish file rite
    fclose(file);
    return true;
}

int main(int argc, char **argv)
{
    //Show usage if too few arguments are passed
    if (argc != 3) {
        std::cout << "Usage: " << argv[0] << " elf_input uso_output" << std::endl;
        std::cout << "elf_input is a relocatable Nintendo 64 ELF file." << std::endl;
        std::cout << "The ELF converted to a uso will be written to uso_output." << std::endl;
        return 1;
    }
    //Try to load ELF
    if (!elf_reader.load(argv[1])) {
        std::cerr << "Failed to read input ELF file." << std::endl;
        return 1;
    }
    //Do elf sanity checks
    if (!elf_valid()) {
        std::cerr << "Input ELF file is not valid relocatable Nintendo 64 ELF file." << std::endl;
        std::cerr << "Try linking with -r if ELF is a valid Nintendo 64 ELF file." << std::endl;
        return 1;
    }
    //Check for limitations of shared object system
    if (common_is_used()) {
        std::cerr << "Common section symbols should not be in input ELF file." << std::endl;
        std::cerr << "Pass -d to the linker or add FORCE_COMMON_ALLOCATION to the linker script to fix." << std::endl;
        return 1;
    }
    if (check_gp_relative_relocations()) {
        std::cerr << "Relocations using GP register should not be present in input ELF file." << std::endl;
        std::cerr << "Compile with -mno-gpopt (not -G 0) and without -fPIC, -fpic, -mshared, or -mabicalls to fix." << std::endl;
        return 1;
    }
    //Prepare for writing USO
    section_collect();
    sym_collect();
    reloc_build();
    //Write USO and return write status
    if (!uso_write(argv[1], argv[2])) {
        return 1;
    }
    return 0;
}