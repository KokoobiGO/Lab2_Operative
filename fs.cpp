#include <iostream>
#include <cstring>
#include <sstream>
#include "fs.h"

FS::FS()
{
    std::cout << "FS::FS()... Creating file system\n";
    current_dir_block = ROOT_BLOCK;
}

FS::~FS()
{

}

// Helper function: Read FAT from disk into memory
void
FS::read_fat()
{
    uint8_t block[BLOCK_SIZE];
    disk.read(FAT_BLOCK, block);
    std::memcpy(fat, block, BLOCK_SIZE);
}

// Helper function: Write FAT from memory to disk
void
FS::write_fat()
{
    uint8_t block[BLOCK_SIZE];
    std::memcpy(block, fat, BLOCK_SIZE);
    disk.write(FAT_BLOCK, block);
}

// Helper function: Find a free block in the FAT
int16_t
FS::find_free_block()
{
    for (int i = 2; i < BLOCK_SIZE/2; i++) {
        if (fat[i] == FAT_FREE) {
            return i;
        }
    }
    return -1;
}

// Helper function: Find free directory entry index in a directory block
int
FS::find_free_dir_entry(uint16_t dir_block)
{
    dir_entry* entries = read_dir_entries(dir_block);
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] == '\0') {
            delete[] entries;
            return i;
        }
    }
    delete[] entries;
    return -1;
}

// Helper function: Read directory entries from a block
dir_entry*
FS::read_dir_entries(uint16_t dir_block)
{
    uint8_t block[BLOCK_SIZE];
    disk.read(dir_block, block);
    
    dir_entry* entries = new dir_entry[BLOCK_SIZE / sizeof(dir_entry)];
    std::memcpy(entries, block, BLOCK_SIZE);
    return entries;
}

// Helper function: Write directory entries to a block
void
FS::write_dir_entries(uint16_t dir_block, dir_entry* entries)
{
    uint8_t block[BLOCK_SIZE];
    std::memcpy(block, entries, BLOCK_SIZE);
    disk.write(dir_block, block);
}

// formats the disk, i.e., creates an empty file system
int
FS::format()
{
    // Initialize FAT: all entries are free
    for (int i = 0; i < BLOCK_SIZE/2; i++) {
        fat[i] = FAT_FREE;
    }
    
    // Mark block 0 (root directory) as EOF
    fat[ROOT_BLOCK] = FAT_EOF;
    
    // Mark block 1 (FAT block) as EOF
    fat[FAT_BLOCK] = FAT_EOF;
    
    // Write FAT to disk
    write_fat();
    
    // Initialize root directory as empty
    uint8_t root_block[BLOCK_SIZE];
    std::memset(root_block, 0, BLOCK_SIZE);
    disk.write(ROOT_BLOCK, root_block);
    
    // Set current directory to root
    current_dir_block = ROOT_BLOCK;
    
    return 0;
}

// create <filepath> creates a new file on the disk, the data content is
// written on the following rows (ended with an empty row)
int
FS::create(std::string filepath)
{
    // Check filename length (max 55 chars + null terminator)
    if (filepath.length() > 55) {
        std::cout << "Error: Filename too long (max 55 characters)\n";
        return -1;
    }
    
    // Read current directory
    dir_entry* entries = read_dir_entries(current_dir_block);
    
    // Check if file already exists
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, filepath.c_str()) == 0) {
            std::cout << "Error: File already exists\n";
            delete[] entries;
            return -1;
        }
    }
    
    // Find free directory entry
    int free_entry_idx = find_free_dir_entry(current_dir_block);
    if (free_entry_idx == -1) {
        std::cout << "Error: Directory is full\n";
        delete[] entries;
        return -1;
    }
    
    // Read user input until empty line
    std::string line;
    std::string data;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            break;
        }
        data += line + "\n";
    }
    
    uint32_t data_size = data.length();
    
    // Calculate number of blocks needed
    int blocks_needed = (data_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (blocks_needed == 0) blocks_needed = 1; // At least one block even for empty file
    
    // Read FAT
    read_fat();
    
    // Find and allocate blocks
    int16_t first_block = -1;
    int16_t prev_block = -1;
    
    for (int i = 0; i < blocks_needed; i++) {
        int16_t free_block = find_free_block();
        if (free_block == -1) {
            std::cout << "Error: Disk is full\n";
            delete[] entries;
            return -1;
        }
        
        if (first_block == -1) {
            first_block = free_block;
        }
        
        if (prev_block != -1) {
            fat[prev_block] = free_block;
        }
        
        fat[free_block] = FAT_EOF;
        prev_block = free_block;
    }
    
    // Write data to blocks
    uint8_t block[BLOCK_SIZE];
    int16_t current_block = first_block;
    uint32_t offset = 0;
    
    while (current_block != FAT_EOF) {
        std::memset(block, 0, BLOCK_SIZE);
        
        uint32_t bytes_to_write = std::min((uint32_t)BLOCK_SIZE, data_size - offset);
        if (bytes_to_write > 0) {
            std::memcpy(block, data.c_str() + offset, bytes_to_write);
        }
        
        disk.write(current_block, block);
        offset += bytes_to_write;
        
        int16_t next_block = fat[current_block];
        current_block = next_block;
    }
    
    // Write FAT to disk
    write_fat();
    
    // Create directory entry
    std::strcpy(entries[free_entry_idx].file_name, filepath.c_str());
    entries[free_entry_idx].size = data_size;
    entries[free_entry_idx].first_blk = first_block;
    entries[free_entry_idx].type = TYPE_FILE;
    entries[free_entry_idx].access_rights = READ | WRITE;
    
    // Write directory back to disk
    write_dir_entries(current_dir_block, entries);
    
    delete[] entries;
    return 0;
}

// cat <filepath> reads the content of a file and prints it on the screen
int
FS::cat(std::string filepath)
{
    // Read current directory
    dir_entry* entries = read_dir_entries(current_dir_block);
    
    // Find file
    dir_entry* file_entry = nullptr;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, filepath.c_str()) == 0) {
            file_entry = &entries[i];
            break;
        }
    }
    
    if (file_entry == nullptr) {
        std::cout << "Error: File not found\n";
        delete[] entries;
        return -1;
    }
    
    // Read FAT
    read_fat();
    
    // Read and print file contents
    uint8_t block[BLOCK_SIZE];
    int16_t current_block = file_entry->first_blk;
    uint32_t bytes_remaining = file_entry->size;
    
    while (current_block != FAT_EOF && bytes_remaining > 0) {
        disk.read(current_block, block);
        
        uint32_t bytes_to_print = std::min((uint32_t)BLOCK_SIZE, bytes_remaining);
        for (uint32_t i = 0; i < bytes_to_print; i++) {
            std::cout << (char)block[i];
        }
        
        bytes_remaining -= bytes_to_print;
        current_block = fat[current_block];
    }
    
    delete[] entries;
    return 0;
}

// ls lists the content in the current directory (files and sub-directories)
int
FS::ls()
{
    // Read current directory
    dir_entry* entries = read_dir_entries(current_dir_block);
    
    // Print header
    std::cout << "name\t size\n";
    
    // Print each file/directory
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0') {
            std::cout << entries[i].file_name << "\t " << entries[i].size << "\n";
        }
    }
    
    delete[] entries;
    return 0;
}

// cp <sourcepath> <destpath> makes an exact copy of the file
// <sourcepath> to a new file <destpath>
int
FS::cp(std::string sourcepath, std::string destpath)
{
    std::cout << "FS::cp(" << sourcepath << "," << destpath << ")\n";
    return 0;
}

// mv <sourcepath> <destpath> renames the file <sourcepath> to the name <destpath>,
// or moves the file <sourcepath> to the directory <destpath> (if dest is a directory)
int
FS::mv(std::string sourcepath, std::string destpath)
{
    std::cout << "FS::mv(" << sourcepath << "," << destpath << ")\n";
    return 0;
}

// rm <filepath> removes / deletes the file <filepath>
int
FS::rm(std::string filepath)
{
    std::cout << "FS::rm(" << filepath << ")\n";
    return 0;
}

// append <filepath1> <filepath2> appends the contents of file <filepath1> to
// the end of file <filepath2>. The file <filepath1> is unchanged.
int
FS::append(std::string filepath1, std::string filepath2)
{
    std::cout << "FS::append(" << filepath1 << "," << filepath2 << ")\n";
    return 0;
}

// mkdir <dirpath> creates a new sub-directory with the name <dirpath>
// in the current directory
int
FS::mkdir(std::string dirpath)
{
    std::cout << "FS::mkdir(" << dirpath << ")\n";
    return 0;
}

// cd <dirpath> changes the current (working) directory to the directory named <dirpath>
int
FS::cd(std::string dirpath)
{
    std::cout << "FS::cd(" << dirpath << ")\n";
    return 0;
}

// pwd prints the full path, i.e., from the root directory, to the current
// directory, including the current directory name
int
FS::pwd()
{
    std::cout << "/\n";
    return 0;
}

// chmod <accessrights> <filepath> changes the access rights for the
// file <filepath> to <accessrights>.
int
FS::chmod(std::string accessrights, std::string filepath)
{
    std::cout << "FS::chmod(" << accessrights << "," << filepath << ")\n";
    return 0;
}
