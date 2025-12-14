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
    for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, filepath.c_str()) == 0) {
            file_entry = &entries[i];
            break;
        }
    }
    
    if (file_entry == nullptr) {
        delete[] entries;
        return -1;
    }
    
    // Check if it's a directory
    if (file_entry->type == TYPE_DIR) {
        delete[] entries;
        return -1; // Cannot cat a directory
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
    std::cout << "name\t type\t size\n";
    
    // Print each file/directory
    for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0') {
            std::cout << entries[i].file_name << "\t ";
            if (entries[i].type == TYPE_DIR) {
                std::cout << "dir\t -\n";
            } else {
                std::cout << "file\t " << entries[i].size << "\n";
            }
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
    // Check dest filename length
    if (destpath.length() > 55) {
        return -1;
    }
    
    // Read current directory
    dir_entry* entries = read_dir_entries(current_dir_block);
    
    // Find source file
    dir_entry* src_entry = nullptr;
    int src_idx = -1;
    for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, sourcepath.c_str()) == 0) {
            src_entry = &entries[i];
            src_idx = i;
            break;
        }
    }
    
    if (src_entry == nullptr) {
        delete[] entries;
        return -1;
    }
    
    // Check if source is a file (not a directory)
    if (src_entry->type != TYPE_FILE) {
        delete[] entries;
        return -1;
    }
    
    // Check if dest file already exists
    for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, destpath.c_str()) == 0) {
            delete[] entries;
            return -1; // Dest already exists (noclobber)
        }
    }
    
    // Find free directory entry for dest
    int dest_entry_idx = find_free_dir_entry(current_dir_block);
    if (dest_entry_idx == -1) {
        delete[] entries;
        return -1;
    }
    
    // Read FAT
    read_fat();
    
    // Read source file data
    std::string data;
    uint8_t block[BLOCK_SIZE];
    int16_t current_block = src_entry->first_blk;
    uint32_t bytes_remaining = src_entry->size;
    
    while (current_block != FAT_EOF && bytes_remaining > 0) {
        disk.read(current_block, block);
        uint32_t bytes_to_read = std::min((uint32_t)BLOCK_SIZE, bytes_remaining);
        data.append((char*)block, bytes_to_read);
        bytes_remaining -= bytes_to_read;
        current_block = fat[current_block];
    }
    
    uint32_t data_size = data.length();
    
    // Calculate number of blocks needed
    int blocks_needed = (data_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (blocks_needed == 0) blocks_needed = 1;
    
    // Allocate blocks for dest file
    int16_t first_block = -1;
    int16_t prev_block = -1;
    
    for (int i = 0; i < blocks_needed; i++) {
        int16_t free_block = find_free_block();
        if (free_block == -1) {
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
    
    // Write data to new blocks
    current_block = first_block;
    uint32_t offset = 0;
    
    while (current_block != FAT_EOF) {
        std::memset(block, 0, BLOCK_SIZE);
        uint32_t bytes_to_write = std::min((uint32_t)BLOCK_SIZE, data_size - offset);
        if (bytes_to_write > 0) {
            std::memcpy(block, data.c_str() + offset, bytes_to_write);
        }
        disk.write(current_block, block);
        offset += bytes_to_write;
        current_block = fat[current_block];
    }
    
    // Write FAT to disk
    write_fat();
    
    // Re-read entries (may have changed)
    delete[] entries;
    entries = read_dir_entries(current_dir_block);
    
    // Create directory entry for dest
    std::strcpy(entries[dest_entry_idx].file_name, destpath.c_str());
    entries[dest_entry_idx].size = data_size;
    entries[dest_entry_idx].first_blk = first_block;
    entries[dest_entry_idx].type = TYPE_FILE;
    entries[dest_entry_idx].access_rights = READ | WRITE;
    
    // Write directory back to disk
    write_dir_entries(current_dir_block, entries);
    
    delete[] entries;
    return 0;
}

// mv <sourcepath> <destpath> renames the file <sourcepath> to the name <destpath>,
// or moves the file <sourcepath> to the directory <destpath> (if dest is a directory)
int
FS::mv(std::string sourcepath, std::string destpath)
{
    // Check dest filename length
    if (destpath.length() > 55) {
        return -1;
    }
    
    // Read current directory
    dir_entry* entries = read_dir_entries(current_dir_block);
    
    // Find source file
    int src_idx = -1;
    for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, sourcepath.c_str()) == 0) {
            src_idx = i;
            break;
        }
    }
    
    if (src_idx == -1) {
        delete[] entries;
        return -1;
    }
    
    // Check if source is a file (not a directory)
    if (entries[src_idx].type != TYPE_FILE) {
        delete[] entries;
        return -1;
    }
    
    // Check if dest file already exists
    for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, destpath.c_str()) == 0) {
            delete[] entries;
            return -1; // Dest already exists (noclobber)
        }
    }
    
    // Simply rename the file (just change the name in directory entry)
    std::strcpy(entries[src_idx].file_name, destpath.c_str());
    
    // Write directory back to disk
    write_dir_entries(current_dir_block, entries);
    
    delete[] entries;
    return 0;
}

// rm <filepath> removes / deletes the file <filepath>
int
FS::rm(std::string filepath)
{
    // Read current directory
    dir_entry* entries = read_dir_entries(current_dir_block);
    
    // Find file/directory
    int file_idx = -1;
    for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, filepath.c_str()) == 0) {
            file_idx = i;
            break;
        }
    }
    
    if (file_idx == -1) {
        delete[] entries;
        return -1;
    }
    
    // Read FAT
    read_fat();
    
    // Handle directory case
    if (entries[file_idx].type == TYPE_DIR) {
        // Check if directory is empty (only contains '..')
        dir_entry* dir_entries = read_dir_entries(entries[file_idx].first_blk);
        bool is_empty = true;
        for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
            if (dir_entries[i].file_name[0] != '\0' && 
                std::strcmp(dir_entries[i].file_name, "..") != 0) {
                is_empty = false;
                break;
            }
        }
        delete[] dir_entries;
        
        if (!is_empty) {
            delete[] entries;
            return -1; // Directory not empty
        }
        
        // Free the directory block
        fat[entries[file_idx].first_blk] = FAT_FREE;
    } else {
        // Free all blocks used by the file
        int16_t current_block = entries[file_idx].first_blk;
        while (current_block != FAT_EOF && current_block != FAT_FREE) {
            int16_t next_block = fat[current_block];
            fat[current_block] = FAT_FREE;
            current_block = next_block;
        }
    }
    
    // Write FAT to disk
    write_fat();
    
    // Clear directory entry
    std::memset(&entries[file_idx], 0, sizeof(dir_entry));
    
    // Write directory back to disk
    write_dir_entries(current_dir_block, entries);
    
    delete[] entries;
    return 0;
}

// append <filepath1> <filepath2> appends the contents of file <filepath1> to
// the end of file <filepath2>. The file <filepath1> is unchanged.
int
FS::append(std::string filepath1, std::string filepath2)
{
    // Read current directory
    dir_entry* entries = read_dir_entries(current_dir_block);
    
    // Find file1 (source)
    int file1_idx = -1;
    for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, filepath1.c_str()) == 0) {
            file1_idx = i;
            break;
        }
    }
    
    if (file1_idx == -1) {
        delete[] entries;
        return -1;
    }
    
    // Find file2 (destination)
    int file2_idx = -1;
    for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, filepath2.c_str()) == 0) {
            file2_idx = i;
            break;
        }
    }
    
    if (file2_idx == -1) {
        delete[] entries;
        return -1;
    }
    
    // Check both are files (not directories)
    if (entries[file1_idx].type != TYPE_FILE || entries[file2_idx].type != TYPE_FILE) {
        delete[] entries;
        return -1;
    }
    
    // Read FAT
    read_fat();
    
    // Read file1 data
    std::string file1_data;
    uint8_t block[BLOCK_SIZE];
    int16_t current_block = entries[file1_idx].first_blk;
    uint32_t bytes_remaining = entries[file1_idx].size;
    
    while (current_block != FAT_EOF && bytes_remaining > 0) {
        disk.read(current_block, block);
        uint32_t bytes_to_read = std::min((uint32_t)BLOCK_SIZE, bytes_remaining);
        file1_data.append((char*)block, bytes_to_read);
        bytes_remaining -= bytes_to_read;
        current_block = fat[current_block];
    }
    
    if (file1_data.empty()) {
        delete[] entries;
        return 0; // Nothing to append
    }
    
    // Find the last block of file2
    int16_t last_block = entries[file2_idx].first_blk;
    while (fat[last_block] != FAT_EOF) {
        last_block = fat[last_block];
    }
    
    // Calculate how many bytes are used in the last block
    uint32_t file2_size = entries[file2_idx].size;
    uint32_t bytes_in_last_block = file2_size % BLOCK_SIZE;
    if (bytes_in_last_block == 0 && file2_size > 0) {
        bytes_in_last_block = BLOCK_SIZE; // Last block is full
    }
    
    // Read the last block of file2
    disk.read(last_block, block);
    
    // Append file1 data
    uint32_t file1_offset = 0;
    uint32_t file1_size = file1_data.length();
    
    while (file1_offset < file1_size) {
        uint32_t space_in_block = BLOCK_SIZE - bytes_in_last_block;
        uint32_t bytes_to_write = std::min(space_in_block, file1_size - file1_offset);
        
        if (bytes_to_write > 0) {
            std::memcpy(block + bytes_in_last_block, file1_data.c_str() + file1_offset, bytes_to_write);
            disk.write(last_block, block);
            file1_offset += bytes_to_write;
            bytes_in_last_block += bytes_to_write;
        }
        
        // If we still have data to write and the block is full, allocate a new block
        if (file1_offset < file1_size && bytes_in_last_block >= BLOCK_SIZE) {
            int16_t new_block = find_free_block();
            if (new_block == -1) {
                delete[] entries;
                return -1;
            }
            fat[last_block] = new_block;
            fat[new_block] = FAT_EOF;
            last_block = new_block;
            bytes_in_last_block = 0;
            std::memset(block, 0, BLOCK_SIZE);
        }
    }
    
    // Write FAT to disk
    write_fat();
    
    // Update file2 size
    entries[file2_idx].size += file1_size;
    
    // Write directory back to disk
    write_dir_entries(current_dir_block, entries);
    
    delete[] entries;
    return 0;
}

// mkdir <dirpath> creates a new sub-directory with the name <dirpath>
// in the current directory
int
FS::mkdir(std::string dirpath)
{
    // Check dirname length
    if (dirpath.length() > 55) {
        return -1;
    }
    
    // Read current directory
    dir_entry* entries = read_dir_entries(current_dir_block);
    
    // Check if name already exists
    for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, dirpath.c_str()) == 0) {
            delete[] entries;
            return -1; // Already exists
        }
    }
    
    // Find free directory entry in current directory
    int free_entry_idx = find_free_dir_entry(current_dir_block);
    if (free_entry_idx == -1) {
        delete[] entries;
        return -1;
    }
    
    // Read FAT
    read_fat();
    
    // Find a free block for the new directory
    int16_t new_dir_block = find_free_block();
    if (new_dir_block == -1) {
        delete[] entries;
        return -1;
    }
    
    // Mark the new block as EOF in FAT
    fat[new_dir_block] = FAT_EOF;
    write_fat();
    
    // Initialize the new directory block (empty except for '..')
    dir_entry* new_dir_entries = new dir_entry[BLOCK_SIZE / sizeof(dir_entry)];
    std::memset(new_dir_entries, 0, BLOCK_SIZE);
    
    // Create '..' entry pointing to parent directory
    std::strcpy(new_dir_entries[0].file_name, "..");
    new_dir_entries[0].size = 0;
    new_dir_entries[0].first_blk = current_dir_block;
    new_dir_entries[0].type = TYPE_DIR;
    new_dir_entries[0].access_rights = READ | WRITE | EXECUTE;
    
    // Write new directory to disk
    write_dir_entries(new_dir_block, new_dir_entries);
    delete[] new_dir_entries;
    
    // Create directory entry in current directory
    std::strcpy(entries[free_entry_idx].file_name, dirpath.c_str());
    entries[free_entry_idx].size = 0;
    entries[free_entry_idx].first_blk = new_dir_block;
    entries[free_entry_idx].type = TYPE_DIR;
    entries[free_entry_idx].access_rights = READ | WRITE | EXECUTE;
    
    // Write current directory back to disk
    write_dir_entries(current_dir_block, entries);
    
    delete[] entries;
    return 0;
}

// cd <dirpath> changes the current (working) directory to the directory named <dirpath>
int
FS::cd(std::string dirpath)
{
    // Handle special case: cd to root
    if (dirpath == "/") {
        current_dir_block = ROOT_BLOCK;
        return 0;
    }
    
    // Read current directory
    dir_entry* entries = read_dir_entries(current_dir_block);
    
    // Find the directory
    int dir_idx = -1;
    for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] != '\0' && 
            std::strcmp(entries[i].file_name, dirpath.c_str()) == 0) {
            dir_idx = i;
            break;
        }
    }
    
    if (dir_idx == -1) {
        delete[] entries;
        return -1; // Directory not found
    }
    
    // Check if it's a directory
    if (entries[dir_idx].type != TYPE_DIR) {
        delete[] entries;
        return -1; // Not a directory
    }
    
    // Change to the directory
    current_dir_block = entries[dir_idx].first_blk;
    
    delete[] entries;
    return 0;
}

// pwd prints the full path, i.e., from the root directory, to the current
// directory, including the current directory name
int
FS::pwd()
{
    // If we're at root, just print /
    if (current_dir_block == ROOT_BLOCK) {
        std::cout << "/\n";
        return 0;
    }
    
    // Build path by traversing from current to root
    std::string path = "";
    uint16_t block = current_dir_block;
    
    while (block != ROOT_BLOCK) {
        // Read the current directory to find '..' entry
        dir_entry* entries = read_dir_entries(block);
        
        uint16_t parent_block = ROOT_BLOCK;
        for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
            if (entries[i].file_name[0] != '\0' && 
                std::strcmp(entries[i].file_name, "..") == 0) {
                parent_block = entries[i].first_blk;
                break;
            }
        }
        delete[] entries;
        
        // Read parent directory to find current directory's name
        dir_entry* parent_entries = read_dir_entries(parent_block);
        std::string dir_name = "";
        
        for (int i = 0; i < BLOCK_SIZE / (int)sizeof(dir_entry); i++) {
            if (parent_entries[i].file_name[0] != '\0' && 
                parent_entries[i].type == TYPE_DIR &&
                parent_entries[i].first_blk == block) {
                dir_name = parent_entries[i].file_name;
                break;
            }
        }
        delete[] parent_entries;
        
        // Prepend to path
        path = "/" + dir_name + path;
        
        // Move to parent
        block = parent_block;
    }
    
    std::cout << path << "\n";
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
